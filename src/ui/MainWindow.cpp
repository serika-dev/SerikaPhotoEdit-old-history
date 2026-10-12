#include "MainWindow.h"
#include "Theme.h"
#include "actions/ActionRunner.h"
#include "compositor/GpuProcessor.h"
#include "document/LayerOperations.h"
#include "io/FormatIO.h"
#include "ui/panels/BrushSettingsWidget.h"
#include "ui/panels/LayerTree.h"
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QColorSpace>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <cmath>

namespace serika {
// Icon-only panel button. The glyph property lets applyTheme repaint it in the new text color.
static void setGlyph(QToolButton *button, const QString &glyph, int iconSize) {
    button->setProperty("glyph", glyph);
    button->setProperty("iconButton", true);
    button->setIcon(toolIcon(glyph));
    button->setIconSize(QSize(iconSize, iconSize));
    button->setAutoRaise(true);
}
// Layer rows draw every icon at the tree's 42x30 thumbnail size, so small glyphs are placed
// centred within their narrow column on a transparent pixmap of that size.
static QIcon layerCellIcon(const QString &glyph, int glyphSize, int columnWidth) {
    const qreal ratio = qApp->devicePixelRatio();
    QPixmap pixmap(QSize(42, 30) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    toolIcon(glyph).paint(&painter,
                          QRect((columnWidth - glyphSize) / 2, (30 - glyphSize) / 2, glyphSize, glyphSize));
    return QIcon(pixmap);
}
static QPushButton *button(const QString &text, QWidget *parent, const std::function<void()> &fn) {
    auto *b = new QPushButton(text, parent);
    QObject::connect(b, &QPushButton::clicked, parent, fn);
    return b;
}
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_settings(QSettings::defaultFormat(), QSettings::UserScope, "Serika", "PhotoEdit") {
    initializeBrandFonts();
    GpuProcessor::instance().setEnabled(m_settings.value("gpuProcessing", false).toBool() ||
                                        QCoreApplication::arguments().contains("--gpu"));
    setObjectName("SerikaPhotoEdit");
    setWindowTitle("Serika PhotoEdit");
    setWindowIcon(QIcon(":/serika/logo.png"));
    resize(1600, 1000);
    setMinimumSize(1100, 700);
    setAcceptDrops(true);
    setDockOptions(AllowNestedDocks | AllowTabbedDocks | AnimatedDocks | GroupedDragging);
    m_recent = m_settings.value("recent").toStringList();
    m_center = new QStackedWidget(this);
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName("documentTabs");
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->tabBar()->installEventFilter(this);
    setCentralWidget(m_center);
    buildHome();
    m_center->addWidget(m_tabs);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] { refresh(); });
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int index) { closeDocument(index); });
    m_tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tabs->tabBar(), &QWidget::customContextMenuRequested, this, [this](QPoint pos) {
        const int index = m_tabs->tabBar()->tabAt(pos);
        if (index < 0)
            return;
        QMenu menu;
        menu.addAction("Close", [this, index] { closeDocument(index); });
        menu.addAction("Move to New Window", [this, index] { detachDocument(index); });
        menu.exec(m_tabs->tabBar()->mapToGlobal(pos));
    });
    buildMenus();
    buildOptions();
    buildTools();
    applyTheme(m_settings.value("theme", "Dark").toString());
    buildDocks();
    m_essentialState = saveState(1);
    setWorkspace(m_settings.value("workspace", "Essentials").toString());
    const auto geometry = m_settings.value("geometry").toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    initializeShortcuts();
    connect(&m_recovery, &QTimer::timeout, this, &MainWindow::saveRecovery);
    m_recovery.start(10 * 60 * 1000);
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(80);
    connect(&m_refreshTimer, &QTimer::timeout, this, &MainWindow::refresh);
    refresh();
    if (!m_settings.value("showHome", true).toBool())
        QTimer::singleShot(0, this, [this] {
            if (!currentDocument())
                addDocument(Document::create(QSize(1920, 1080), Qt::white, 8, this));
        });
}
Document *MainWindow::currentDocument() const {
    auto *canvas = currentCanvas();
    return canvas ? canvas->document() : nullptr;
}
CanvasView *MainWindow::currentCanvas() const {
    return m_tabs->currentWidget() ? m_tabs->currentWidget()->findChild<CanvasView *>() : nullptr;
}
QStringList MainWindow::dockNames() const { return m_docks.keys(); }
QAction *MainWindow::command(QMenu *menu, const QString &name, const QKeySequence &shortcut) {
    if (m_commands.contains(name)) {
        menu->addAction(m_commands[name]);
        return m_commands[name];
    }
    auto *a = menu->addAction(name);
    Q_UNUSED(shortcut);
    m_commands[name] = a;
    connect(a, &QAction::triggered, this, [this, name] { runCommand(name); });
    return a;
}
void MainWindow::buildMenus() {
    auto *file = menuBar()->addMenu("File");
    command(file, "New...", QKeySequence::New);
    command(file, "Open...", QKeySequence::Open);
    command(file, "Browse in Folder...");
    command(file, "Open As...");
    auto *recent = file->addMenu("Open Recent");
    connect(recent, &QMenu::aboutToShow, this, [this, recent] {
        recent->clear();
        for (const auto &path : m_recent)
            recent->addAction(QFileInfo(path).fileName(), [this, path] { openFile(path); });
    });
    file->addSeparator();
    command(file, "Close", QKeySequence::Close);
    command(file, "Close All");
    command(file, "Close Others");
    file->addSeparator();
    command(file, "Save", QKeySequence::Save);
    command(file, "Save As...", QKeySequence::SaveAs);
    command(file, "Save a Copy...");
    auto *exportMenu = file->addMenu("Export");
    command(exportMenu, "Export As...", QKeySequence("Ctrl+Alt+Shift+S"));
    command(exportMenu, "Export CMYK TIFF...");
    command(exportMenu, "Export CMYK Separations...");
    command(exportMenu, "Quick Export PNG");
    command(exportMenu, "Export Layers...");
    auto *generate = file->addMenu("Generate");
    command(generate, "Image Assets");
    file->addSeparator();
    command(file, "Place Embedded...");
    command(file, "Place Linked...");
    command(file, "Package...");
    command(file, "File Info...");
    auto *automate = file->addMenu("Automate");
    command(automate, "Batch...");
    file->addSeparator();
    command(file, "Print...", QKeySequence::Print);
    command(file, "Print One Copy");
    file->addSeparator();
    command(file, "Exit", QKeySequence::Quit);
    auto *edit = menuBar()->addMenu("Edit");
    command(edit, "Undo", QKeySequence::Undo);
    command(edit, "Redo", QKeySequence("Ctrl+Shift+Z"));
    command(edit, "Step Backward", QKeySequence("Ctrl+Alt+Z"));
    command(edit, "Toggle Last State");
    command(edit, "Fade...");
    edit->addSeparator();
    command(edit, "Cut", QKeySequence::Cut);
    command(edit, "Copy", QKeySequence::Copy);
    command(edit, "Copy Merged", QKeySequence("Ctrl+Shift+C"));
    command(edit, "Paste", QKeySequence::Paste);
    command(edit, "Paste in Place", QKeySequence("Ctrl+Shift+V"));
    command(edit, "Paste Into");
    command(edit, "Paste Outside");
    edit->addSeparator();
    command(edit, "Fill...");
    command(edit, "Stroke...");
    command(edit, "Content-Aware Fill");
    command(edit, "Free Transform...", QKeySequence("Ctrl+T"));
    auto *transform = edit->addMenu("Transform");
    for (const auto &s : QStringList{"Scale...", "Rotate...", "Skew...", "Distort...", "Perspective...",
                                     "Warp...", "Flip Horizontal", "Flip Vertical"})
        command(transform, s);
    command(edit, "Puppet Warp...");
    command(edit, "Auto-Align Layers");
    edit->addSeparator();
    command(edit, "Define Brush Preset...");
    command(edit, "Define Pattern...");
    command(edit, "Purge History");
    command(edit, "Color Settings...");
    command(edit, "Keyboard Shortcuts...");
    command(edit, "Menus...");
    command(edit, "Preferences...", QKeySequence("Ctrl+,"));
    auto *image = menuBar()->addMenu("Image");
    auto *mode = image->addMenu("Mode");
    for (const auto &s : QStringList{"RGB", "Grayscale", "Bitmap", "Indexed", "CMYK", "Lab", "Multichannel",
                                     "8 Bits/Channel", "16 Bits/Channel", "32 Bits/Channel"})
        command(mode, s);
    auto *adj = image->addMenu("Adjustments");
    for (const auto &s : adjustmentNames()) {
        QKeySequence key;
        if (s == "Levels")
            key = QKeySequence("Ctrl+L");
        if (s == "Curves")
            key = QKeySequence("Ctrl+M");
        if (s == "Hue/Saturation")
            key = QKeySequence("Ctrl+U");
        if (s == "Color Balance")
            key = QKeySequence("Ctrl+B");
        if (s == "Invert")
            key = QKeySequence("Ctrl+I");
        if (s == "Desaturate")
            key = QKeySequence("Ctrl+Shift+U");
        command(adj, s, key);
    }
    command(image, "Auto Tone", QKeySequence("Ctrl+Shift+L"));
    command(image, "Auto Contrast");
    command(image, "Auto Color", QKeySequence("Ctrl+Shift+B"));
    image->addSeparator();
    command(image, "Image Size...");
    command(image, "Canvas Size...");
    auto *rot = image->addMenu("Image Rotation");
    command(rot, "180°");
    command(rot, "90° Clockwise");
    command(rot, "90° Counterclockwise");
    command(image, "Crop to Selection");
    command(image, "Trim");
    command(image, "Reveal All");
    auto *layer = menuBar()->addMenu("Layer");
    command(layer, "New Layer...", QKeySequence("Ctrl+Shift+N"));
    command(layer, "New Layer", QKeySequence("Ctrl+Alt+Shift+N"));
    command(layer, "Layer via Copy", QKeySequence("Ctrl+J"));
    command(layer, "Duplicate Layer");
    command(layer, "Layer via Cut", QKeySequence("Ctrl+Shift+J"));
    command(layer, "Delete Layer");
    command(layer, "Rename Layer...");
    auto *newAdj = layer->addMenu("New Adjustment Layer");
    for (const auto &s : adjustmentNames())
        command(newAdj, "Adjustment: " + s);
    auto *fill = layer->addMenu("New Fill Layer");
    command(fill, "Solid Color...");
    command(fill, "Gradient Fill...");
    command(fill, "Pattern Fill...");
    command(fill, "Edit Fill...");
    layer->addSeparator();
    command(layer, "Layer Style...");
    command(layer, "Add Layer Mask");
    command(layer, "Delete Layer Mask");
    command(layer, "Apply Layer Mask");
    command(layer, "Create Clipping Mask", QKeySequence("Ctrl+Alt+G"));
    command(layer, "Group Layers", QKeySequence("Ctrl+G"));
    command(layer, "Ungroup Layers", QKeySequence("Ctrl+Shift+G"));
    command(layer, "Rasterize Layer");
    auto *smartObjects = layer->addMenu("Smart Objects");
    for (const auto &name : QStringList{"Convert to Smart Object", "Edit Contents...", "Replace Contents...",
                                        "Relink to File...", "Update Linked Content", "Embed Linked",
                                        "Export Contents...", "Edit Smart Filters..."})
        command(smartObjects, name);
    auto *arrange = layer->addMenu("Arrange");
    command(arrange, "Bring Forward", QKeySequence("Ctrl+]"));
    command(arrange, "Send Backward", QKeySequence("Ctrl+["));
    command(arrange, "Bring to Front", QKeySequence("Ctrl+Shift+]"));
    command(arrange, "Send to Back", QKeySequence("Ctrl+Shift+["));
    layer->addSeparator();
    command(layer, "Merge Down", QKeySequence("Ctrl+E"));
    command(layer, "Merge Visible", QKeySequence("Ctrl+Shift+E"));
    command(layer, "Stamp Visible", QKeySequence("Ctrl+Alt+Shift+E"));
    command(layer, "Flatten Image", QKeySequence("Ctrl+Shift+F"));
    auto *type = menuBar()->addMenu("Type");
    for (const auto &s : QStringList{"Character", "Paragraph", "Glyphs"})
        command(type, s);
    command(type, "Edit Text...");
    command(type, "Edit Typography...");
    command(type, "Convert to Paragraph Text");
    command(type, "Type on Path...");
    command(type, "Clear Type Path");
    command(type, "Convert to Shape");
    command(type, "Convert to Point Text");
    command(type, "Warp Text...");
    command(type, "Horizontal Orientation");
    command(type, "Vertical Orientation");
    auto *select = menuBar()->addMenu("Select");
    command(select, "All", QKeySequence::SelectAll);
    command(select, "Deselect", QKeySequence("Ctrl+D"));
    command(select, "Reselect", QKeySequence("Ctrl+Shift+D"));
    command(select, "Inverse", QKeySequence("Ctrl+Shift+I"));
    command(select, "All Layers", QKeySequence("Ctrl+Alt+A"));
    command(select, "Subject");
    command(select, "Color Range...");
    command(select, "Focus Area...");
    command(select, "Select and Mask...");
    auto *modify = select->addMenu("Modify");
    for (const auto &s : QStringList{"Feather...", "Expand...", "Contract...", "Smooth...", "Border..."})
        command(modify, s);
    command(select, "Grow");
    command(select, "Similar");
    command(select, "Transform Selection...");
    command(select, "Load Selection...");
    command(select, "Save Selection...");
    command(select, "New Selection from Layer");
    auto *filters = menuBar()->addMenu("Filter");
    command(filters, "Last Filter", QKeySequence("Ctrl+F"));
    command(filters, "Convert for Smart Filters");
    command(filters, "Camera Raw Filter...");
    command(filters, "Liquify...");
    command(filters, "Neural Filters...");
    filters->addSeparator();
    const QList<QPair<QString, QStringList>> groups = {
        {"Blur",
         {"Gaussian Blur", "Box Blur", "Motion Blur", "Radial Blur", "Surface Blur", "Lens Blur",
          "Smart Blur"}},
        {"Blur Gallery", {"Field Blur", "Iris Blur", "Tilt-Shift"}},
        {"Distort", {"Spherize", "Pinch", "Twirl", "Ripple", "Wave", "Polar Coordinates"}},
        {"Noise", {"Add Noise", "Despeckle", "Dust & Scratches", "Median", "Reduce Noise"}},
        {"Pixelate", {"Mosaic", "Color Halftone", "Pointillize", "Crystallize", "Fragment"}},
        {"Render", {"Clouds", "Difference Clouds", "Lens Flare", "Fibers"}},
        {"Sharpen", {"Sharpen", "Sharpen More", "Sharpen Edges", "Unsharp Mask", "Smart Sharpen"}},
        {"Stylize", {"Emboss", "Find Edges", "Oil Paint", "Wind", "Diffuse"}},
        {"Other", {"High Pass", "Minimum", "Maximum", "Offset", "Custom"}}};
    for (const auto &g : groups) {
        auto *menu = filters->addMenu(g.first);
        for (const auto &s : g.second)
            command(menu, "Filter: " + s);
    }
    auto *threeD = menuBar()->addMenu("3D");
    command(threeD, "3D Workspace Information");
    auto *view = menuBar()->addMenu("View");
    command(view, "Proof Setup...");
    command(view, "Proof Colors")->setCheckable(true);
    command(view, "Gamut Warning")->setCheckable(true);
    command(view, "Zoom In", QKeySequence("Ctrl++"));
    command(view, "Zoom Out", QKeySequence("Ctrl+-"));
    command(view, "Fit on Screen", QKeySequence("Ctrl+0"));
    command(view, "100%", QKeySequence("Ctrl+1"));
    command(view, "200%");
    command(view, "Flip Horizontal View");
    view->addSeparator();
    command(view, "Rulers", QKeySequence("Ctrl+R"))->setCheckable(true);
    m_commands["Rulers"]->setChecked(true);
    command(view, "Guides", QKeySequence("Ctrl+;"))->setCheckable(true);
    m_commands["Guides"]->setChecked(true);
    command(view, "Grid", QKeySequence("Ctrl+'"))->setCheckable(true);
    command(view, "New Guide...");
    command(view, "Clear Guides");
    command(view, "Lock Guides")->setCheckable(true);
    command(view, "Snap")->setCheckable(true);
    command(view, "Extras")->setCheckable(true);
    command(view, "Screen Mode");
    auto *window = menuBar()->addMenu("Window");
    window->setObjectName("windowMenu");
    auto *workspace = window->addMenu("Workspace");
    for (const auto &s : QStringList{"Essentials", "Photography", "Painting", "Graphic and Web", "Motion"})
        command(workspace, "Workspace: " + s);
    command(workspace, "Reset Workspace");
    command(workspace, "Save Workspace...");
    command(window, "Collapse Panels to Icons");
    auto *help = menuBar()->addMenu("Help");
    command(help, "Welcome Project");
    command(help, "Keyboard Reference");
    command(help, "Format Support");
    command(help, "GPU Diagnostics");
    command(help, "About Serika PhotoEdit");
    command(help, "Command Palette...", QKeySequence("Ctrl+Shift+P"));
    initializeLayerOperations();
}
void MainWindow::buildHome() {
    m_home = new QWidget(this);
    m_home->setObjectName("home");
    auto *outer = new QHBoxLayout(m_home);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *rail = new QWidget;
    rail->setObjectName("homeRail");
    rail->setFixedWidth(200);
    auto *rl = new QVBoxLayout(rail);
    rl->setContentsMargins(24, 36, 24, 28);
    rl->setSpacing(8);
    auto *brand = new QLabel("serika");
    brand->setObjectName("homeBrand");
    rl->addWidget(brand);
    auto *product = new QLabel("PhotoEdit");
    product->setObjectName("homeProduct");
    rl->addWidget(product);
    rl->addSpacing(36);
    auto *home = button("Home", rail, [this] { m_center->setCurrentWidget(m_home); });
    home->setObjectName("homeNav");
    home->setCheckable(true);
    home->setAutoExclusive(true);
    home->setChecked(true);
    rl->addWidget(home);
    auto *files = button("Open a file", rail, [this] { runCommand("Open..."); });
    files->setObjectName("homeNav");
    rl->addWidget(files);
    auto *learn = button("Keyboard shortcuts", rail, [this] { runCommand("Keyboard Reference"); });
    learn->setObjectName("homeNav");
    rl->addWidget(learn);
    rl->addStretch();
    auto *local = new QLabel("Your files. Your device.");
    local->setObjectName("muted");
    local->setWordWrap(true);
    rl->addWidget(local);
    auto *version = new QLabel("PhotoEdit 0.0.1");
    version->setObjectName("muted");
    rl->addWidget(version);
    outer->addWidget(rail);

    auto *scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    auto *body = new QWidget;
    body->setObjectName("homeBody");
    auto *bl = new QVBoxLayout(body);
    bl->setContentsMargins(40, 36, 40, 28);
    bl->setSpacing(16);
    auto *eyebrow = new QLabel("SERIKA PHOTOEDIT");
    eyebrow->setObjectName("eyebrow");
    bl->addWidget(eyebrow);
    auto *title = new QLabel("Your creative space.");
    title->setObjectName("homeTitle");
    bl->addWidget(title);
    auto *sub = new QLabel("From the first brushstroke to the final layer.");
    sub->setObjectName("homeLead");
    sub->setWordWrap(true);
    bl->addWidget(sub);
    bl->addSpacing(8);

    auto *hero = new QFrame;
    hero->setObjectName("homeHero");
    auto *hl = new QHBoxLayout(hero);
    hl->setContentsMargins(30, 24, 24, 24);
    hl->setSpacing(24);
    auto *left = new QVBoxLayout;
    left->setSpacing(14);
    left->addStretch();
    auto *h = new QLabel("Start with a blank canvas");
    h->setObjectName("heroTitle");
    h->setWordWrap(true);
    left->addWidget(h);
    auto *copy = new QLabel("Paint, crop, and bring it all together with layers and masks.");
    copy->setObjectName("heroCopy");
    copy->setWordWrap(true);
    left->addWidget(copy);
    left->addSpacing(10);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(10);
    auto *create = button("New canvas", hero, [this] { newDocument(); });
    create->setObjectName("primary");
    create->setToolTip("Create a new document (" +
                       QKeySequence(QKeySequence::New).toString(QKeySequence::NativeText) + ")");
    actions->addWidget(create);
    auto *open = button("Open image", hero, [this] { runCommand("Open..."); });
    open->setObjectName("secondary");
    open->setToolTip("Open a document (" +
                     QKeySequence(QKeySequence::Open).toString(QKeySequence::NativeText) + ")");
    actions->addWidget(open);
    actions->addStretch();
    left->addLayout(actions);
    left->addStretch();
    hl->addLayout(left, 1);
    auto *preview = new QLabel;
    preview->setObjectName("homeMascot");
    preview->setAccessibleName("Serika PhotoEdit mascot drawing on a pen tablet");
    QPixmap mascot(":/serika/logo.png");
    mascot = mascot.scaled(560, 560, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    mascot.setDevicePixelRatio(2);
    preview->setPixmap(mascot);
    preview->setFixedSize(280, 280);
    preview->setAlignment(Qt::AlignCenter);
    hl->addWidget(preview);
    bl->addWidget(hero);
    bl->addSpacing(8);

    auto *row = new QHBoxLayout;
    auto *recent = new QLabel("Recent documents");
    recent->setObjectName("sectionTitle");
    row->addWidget(recent);
    row->addStretch();
    row->addWidget(button("Browse files", body, [this] { runCommand("Open..."); }));
    bl->addLayout(row);
    auto *recents = new QWidget;
    recents->setObjectName("recentContainer");
    auto *grid = new QGridLayout(recents);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(12);
    int index = 0;
    for (const auto &path : m_recent) {
        if (!QFileInfo::exists(path) || index >= 8)
            continue;
        auto *card = new QFrame;
        card->setObjectName("card");
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(12, 12, 12, 12);
        auto *thumb = new QLabel;
        QImage image;
        if (!FormatIO::isRaw(path) && !path.endsWith("spe") && !path.endsWith("psd"))
            image.load(path);
        if (!image.isNull())
            thumb->setPixmap(
                QPixmap::fromImage(image.scaled(160, 110, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        else
            thumb->setPixmap(QIcon(":/serika/logo.png").pixmap(84, 84));
        thumb->setMinimumSize(100, 110);
        thumb->setAlignment(Qt::AlignCenter);
        cl->addWidget(thumb);
        auto *file = button(QFileInfo(path).fileName(), card, [this, path] { openFile(path); });
        file->setToolTip(path);
        file->setMinimumWidth(0);
        file->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        cl->addWidget(file);
        auto *date = new QLabel(QFileInfo(path).lastModified().toString("dd MMM yyyy"));
        date->setObjectName("muted");
        cl->addWidget(date);
        grid->addWidget(card, index / 4, index % 4);
        index++;
    }
    if (!index) {
        auto *empty = new QFrame;
        empty->setObjectName("card");
        auto *el = new QHBoxLayout(empty);
        el->setContentsMargins(22, 20, 22, 20);
        auto *hint =
            new QLabel("Your recent files will appear here.\nTry the welcome project to explore the tools.");
        hint->setObjectName("muted");
        hint->setWordWrap(true);
        el->addWidget(hint, 1);
        el->addWidget(button("Explore welcome project", empty, [this] { openDemo(); }));
        grid->addWidget(empty, 0, 0);
    }
    bl->addWidget(recents);
    bl->addStretch();
    auto *footer = new QLabel("Made by Serika  \u00b7  Open source  \u00b7  Made for your desktop");
    footer->setObjectName("muted");
    bl->addWidget(footer);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);
    m_center->addWidget(m_home);
}
void MainWindow::buildOptions() {
    m_options = addToolBar("Options");
    m_options->setObjectName("options");
    m_options->setMovable(false);
    m_options->setFixedHeight(33);
    auto *toolLabel = new QLabel("Move");
    toolLabel->setObjectName("toolLabel");
    toolLabel->setFixedWidth(85);
    m_options->addWidget(toolLabel);
    m_options->addSeparator();
    auto *brushModeLabel = new QLabel("Mode:");
    auto *brushModeLabelAction = m_options->addWidget(brushModeLabel);
    brushModeLabelAction->setProperty("brushOption", true);
    auto *brushMode = new QComboBox;
    brushMode->setObjectName("brushMode");
    brushMode->addItems(blendModeNames());
    brushMode->addItems({"Behind", "Clear"});
    brushMode->setMaximumWidth(125);
    auto *brushModeAction = m_options->addWidget(brushMode);
    brushModeAction->setProperty("brushOption", true);
    connect(brushMode, &QComboBox::currentTextChanged, this, [this](const QString &mode) {
        if (auto *canvas = currentCanvas())
            canvas->setProperty("brushBlendMode", mode);
    });
    m_options->addWidget(new QLabel("Size:"));
    auto *size = new QSpinBox;
    size->setObjectName("brushSize");
    size->setRange(1, 5000);
    size->setValue(40);
    size->setFixedWidth(65);
    size->setSuffix(" px");
    m_options->addWidget(size);
    connect(size, &QSpinBox::valueChanged, this, [this](int v) {
        if (auto *c = currentCanvas())
            c->setBrushSize(v);
    });
    m_options->addWidget(new QLabel("Hardness:"));
    auto *hard = new QSpinBox;
    hard->setRange(0, 100);
    hard->setValue(80);
    hard->setSuffix("%");
    hard->setFixedWidth(60);
    hard->setObjectName("hardness");
    m_options->addWidget(hard);
    connect(hard, &QSpinBox::valueChanged, this, [this](int v) {
        if (auto *c = currentCanvas())
            c->setBrushHardness(v / 100.0);
    });
    m_options->addWidget(new QLabel("Opacity:"));
    auto *opacity = new QSpinBox;
    opacity->setRange(0, 100);
    opacity->setValue(100);
    opacity->setSuffix("%");
    opacity->setFixedWidth(60);
    opacity->setObjectName("brushOpacity");
    m_options->addWidget(opacity);
    connect(opacity, &QSpinBox::valueChanged, this, [this](int v) {
        if (auto *c = currentCanvas())
            c->setBrushOpacity(v / 100.0);
    });
    m_options->addWidget(new QLabel("Flow:"));
    auto *flow = new QSpinBox;
    flow->setRange(0, 100);
    flow->setValue(100);
    flow->setSuffix("%");
    flow->setFixedWidth(60);
    flow->setObjectName("flow");
    m_options->addWidget(flow);
    connect(flow, &QSpinBox::valueChanged, this, [this](int v) {
        if (auto *c = currentCanvas())
            c->setBrushFlow(v / 100.0);
    });
    m_options->addSeparator();
    auto *select = new QCheckBox("Auto-select");
    select->setObjectName("autoSelect");
    select->setChecked(true);
    m_options->addWidget(select);
    connect(select, &QCheckBox::toggled, this, [this](bool on) {
        if (auto *canvas = currentCanvas())
            canvas->setProperty("autoSelect", on);
    });
    auto *showControls = new QCheckBox("Transform controls");
    showControls->setObjectName("showTransformControls");
    m_options->addWidget(showControls);
    connect(showControls, &QCheckBox::toggled, this, [this](bool on) {
        if (auto *canvas = currentCanvas()) {
            canvas->setProperty("showTransformControls", on);
            canvas->update();
        }
    });
    auto *typeFont = new QFontComboBox;
    typeFont->setObjectName("typeFont");
    typeFont->setMaximumWidth(170);
    m_options->addWidget(typeFont);
    auto *typeSize = new QSpinBox;
    typeSize->setObjectName("typeSize");
    typeSize->setRange(1, 2000);
    typeSize->setValue(48);
    typeSize->setSuffix(" px");
    typeSize->setFixedWidth(70);
    m_options->addWidget(typeSize);
    connect(typeFont, &QFontComboBox::currentFontChanged, this, [this](const QFont &font) {
        if (auto *canvas = currentCanvas())
            canvas->setProperty("typeFont", font);
        auto *doc = currentDocument();
        if (doc && doc->activeLayer() && doc->activeLayer()->kind == LayerKind::Text)
            doc->mutate("Type font", [doc, font] {
                auto size = doc->activeLayer()->font.pixelSize();
                doc->activeLayer()->font = font;
                doc->activeLayer()->font.setPixelSize(size > 0 ? size : 48);
            });
    });
    connect(typeSize, &QSpinBox::valueChanged, this, [this](int size) {
        if (auto *canvas = currentCanvas())
            canvas->setProperty("typeSize", size);
        auto *doc = currentDocument();
        if (doc && doc->activeLayer() && doc->activeLayer()->kind == LayerKind::Text)
            doc->mutate("Type size", [doc, size] { doc->activeLayer()->font.setPixelSize(size); });
    });
    buildCropOptions();
    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_options->addWidget(spacer);
    auto *search = new QToolButton;
    search->setText("⌕");
    search->setToolTip("Command Palette (Ctrl+Shift+P)");
    connect(search, &QToolButton::clicked, this, &MainWindow::palette);
    m_options->addWidget(search);
    auto *workspaces = new QComboBox;
    workspaces->addItems({"Essentials", "Photography", "Painting", "Graphic and Web", "Motion"});
    workspaces->setObjectName("workspacePicker");
    workspaces->setFixedWidth(128);
    connect(workspaces, &QComboBox::currentTextChanged, this, [this](const QString &s) { setWorkspace(s); });
    m_options->addWidget(workspaces);
    for (auto *action : m_options->actions())
        if (auto *widget = m_options->widgetForAction(action)) {
            if (QStringList{"brushSize", "hardness", "brushOpacity", "flow"}.contains(widget->objectName()))
                action->setProperty("brushOption", true);
            if (auto *label = qobject_cast<QLabel *>(widget))
                if (QStringList{"Size:", "Hardness:", "Opacity:", "Flow:"}.contains(label->text()))
                    action->setProperty("brushOption", true);
            if (QStringList{"autoSelect", "showTransformControls"}.contains(widget->objectName()))
                action->setProperty("moveOption", true);
            if (QStringList{"typeFont", "typeSize"}.contains(widget->objectName()))
                action->setProperty("typeOption", true);
        }
}
void MainWindow::buildTools() {
    if (!m_toolDock) {
        m_toolDock = new QDockWidget("Tools", this);
        m_toolDock->setObjectName("tools");
        m_toolDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        m_toolDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
        addDockWidget(Qt::LeftDockWidgetArea, m_toolDock);
    }
    m_tools.clear();
    auto *old = m_toolDock->widget();
    m_toolWidget = new QWidget;
    m_toolWidget->setObjectName("toolbar");
    m_toolDock->setWidget(m_toolWidget);
    if (old)
        old->deleteLater();
    m_toolDock->setMinimumWidth(m_twoColumns ? 76 : 38);
    m_toolDock->setMaximumWidth(m_twoColumns ? 76 : 38);
    auto *v = new QVBoxLayout(m_toolWidget);
    v->setContentsMargins(3, 0, 3, 4);
    v->setSpacing(1);
    auto *chevron = new QToolButton;
    chevron->setText(m_twoColumns ? "‹" : "»");
    chevron->setFixedHeight(16);
    chevron->setToolTip("Toggle toolbar columns");
    v->addWidget(chevron);
    connect(chevron, &QToolButton::clicked, this, [this] {
        m_twoColumns = !m_twoColumns;
        buildTools();
        selectTool(m_tool);
    });
    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(1);
    const auto groups = ShortcutRegistry::toolGroups();
    int row = 0;
    for (const auto &g : groups) {
        auto *b = new QToolButton;
        b->setObjectName("tool_" + g.second.first());
        b->setCheckable(true);
        b->setProperty("iconTool", g.second.first());
        b->setIcon(toolIcon(g.second.first()));
        b->setIconSize(QSize(18, 18));
        b->setFixedSize(30, 27);
        b->setStyleSheet("QToolButton {padding:2px;border-radius:2px;}");
        b->setToolTip(g.second.join(" / ") + (g.first.isEmpty() ? QString() : " (" + g.first + ")"));
        auto *menu = new QMenu(b);
        for (const auto &name : g.second) {
            auto *a = menu->addAction(toolIcon(name), name);
            connect(a, &QAction::triggered, this, [this, b, name] {
                b->setProperty("iconTool", name);
                b->setIcon(toolIcon(name));
                selectTool(name);
            });
            m_tools[name] = b;
        }
        if (g.second.size() > 1) {
            b->setMenu(menu);
            b->setPopupMode(QToolButton::DelayedPopup);
        }
        connect(b, &QToolButton::clicked, this, [this, b, g] {
            QString chosen = g.second.first();
            if (g.second.contains(m_tool) && m_tools[m_tool] == b)
                chosen = m_tool;
            selectTool(chosen);
        });
        grid->addWidget(b, m_twoColumns ? row / 2 : row, m_twoColumns ? row % 2 : 0);
        row++;
    }
    v->addLayout(grid);
    v->addStretch();
    auto *colors = new QToolButton;
    colors->setFixedSize(m_twoColumns ? 62 : 30, 36);
    colors->setToolTip("Foreground / background colors (X swaps, D resets)");
    colors->setStyleSheet("background:" + m_foreground.name() + ";border:3px solid " + m_background.name() +
                          ";");
    v->addWidget(colors);
    connect(colors, &QToolButton::clicked, this, [this, colors] {
        auto c =
            QColorDialog::getColor(m_foreground, this, "Foreground color", QColorDialog::ShowAlphaChannel);
        if (c.isValid()) {
            m_foreground = c;
            colors->setStyleSheet("background:" + c.name() + ";border:3px solid " + m_background.name() +
                                  ";");
            selectTool(m_tool);
        }
    });
    auto *quick = new QToolButton;
    quick->setText("◉");
    quick->setToolTip("Quick Mask (Q)");
    v->addWidget(quick);
    connect(quick, &QToolButton::clicked, this, [this] {
        m_quickMask = !m_quickMask;
        if (auto *c = currentCanvas())
            c->setQuickMask(m_quickMask);
    });
    auto *screen = new QToolButton;
    screen->setText("▣");
    screen->setToolTip("Screen Mode (F)");
    v->addWidget(screen);
    connect(screen, &QToolButton::clicked, this, [this] { runCommand("Screen Mode"); });
    updateShortcutLabels();
}
QDockWidget *MainWindow::dock(const QString &name, QWidget *widget, bool visible) {
    auto *d = new QDockWidget(name, this);
    d->setObjectName("panel_" + name);
    d->setWidget(widget);
    d->setMinimumWidth(240);
    d->setAllowedAreas(Qt::AllDockWidgetAreas);
    addDockWidget(Qt::RightDockWidgetArea, d);
    d->setVisible(visible);
    m_docks[name] = d;
    for (auto *a : menuBar()->actions())
        if (a->menu() && a->menu()->objectName() == "windowMenu") {
            auto *toggle = d->toggleViewAction();
            a->menu()->addAction(toggle);
            break;
        }
    d->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(d, &QWidget::customContextMenuRequested, this, [d](QPoint pos) {
        QMenu menu;
        menu.addAction("Float panel", [d] { d->setFloating(true); });
        menu.addAction("Close panel", [d] { d->hide(); });
        menu.exec(d->mapToGlobal(pos));
    });
    return d;
}
void MainWindow::buildLayers(QWidget *parent) {
    auto *v = new QVBoxLayout(parent);
    v->setContentsMargins(7, 6, 7, 4);
    v->setSpacing(5);
    auto *search = new QLineEdit;
    search->setPlaceholderText("Filter layers by name");
    v->addWidget(search);
    connect(search, &QLineEdit::textChanged, this, [this](const QString &s) {
        for (int i = 0; i < m_layers->topLevelItemCount(); ++i) {
            auto *item = m_layers->topLevelItem(i);
            item->setHidden(!item->text(1).contains(s, Qt::CaseInsensitive));
        }
    });
    auto *h = new QHBoxLayout;
    m_blend = new QComboBox;
    m_blend->addItems(blendModeNames());
    m_blend->setMinimumWidth(136);
    h->addWidget(m_blend, 1);
    h->addWidget(new QLabel("Opacity"));
    m_layerOpacity = new QSpinBox;
    m_layerOpacity->setRange(0, 100);
    m_layerOpacity->setSuffix("%");
    m_layerOpacity->setFixedWidth(62);
    h->addWidget(m_layerOpacity);
    v->addLayout(h);
    auto *locks = new QHBoxLayout;
    locks->addWidget(new QLabel("Lock:"));
    const QHash<QString, QString> lockGlyphs{
        {"Alpha", "layer:transparency"}, {"Pixels", "Brush"}, {"Position", "Move"}};
    const QHash<QString, QString> lockTips{{"Alpha", "Lock transparent pixels"},
                                           {"Pixels", "Lock image pixels"},
                                           {"Position", "Lock position"}};
    for (const auto &s : QStringList{"Alpha", "Pixels", "Position"}) {
        auto *b = new QToolButton;
        setGlyph(b, lockGlyphs[s], 16);
        b->setFixedSize(24, 22);
        b->setToolTip(lockTips[s]);
        b->setCheckable(true);
        b->setObjectName("lock" + s);
        locks->addWidget(b);
        connect(b, &QToolButton::toggled, this, [this, s](bool on) {
            if (m_refreshing)
                return;
            auto *d = currentDocument();
            if (!d || !d->activeLayer())
                return;
            d->mutate("Lock " + s, [d, s, on] {
                if (s == "Alpha")
                    d->activeLayer()->lockAlpha = on;
                else if (s == "Position")
                    d->activeLayer()->lockPosition = on;
                else
                    d->activeLayer()->locked = on;
            });
        });
    }
    locks->addStretch();
    locks->addWidget(new QLabel("Fill"));
    m_layerFill = new QSpinBox;
    m_layerFill->setRange(0, 100);
    m_layerFill->setSuffix("%");
    m_layerFill->setFixedWidth(60);
    locks->addWidget(m_layerFill);
    v->addLayout(locks);
    auto *layerTree = new LayerTree;
    m_layers = layerTree;
    m_layers->setObjectName("layersTree");
    m_layers->setColumnCount(4);
    m_layers->setHeaderHidden(true);
    m_layers->header()->setStretchLastSection(false);
    m_layers->header()->setMinimumSectionSize(8);
    m_layers->header()->setSectionResizeMode(LayerTree::EyeColumn, QHeaderView::Fixed);
    m_layers->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_layers->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_layers->header()->setSectionResizeMode(3, QHeaderView::Fixed);
    m_layers->setColumnWidth(LayerTree::EyeColumn, 30);
    m_layers->setColumnWidth(2, 44);
    m_layers->setColumnWidth(3, 40);
    m_layers->setIconSize(QSize(42, 30));
    m_layers->setIndentation(14);
    m_layers->setRootIsDecorated(true);
    m_layers->setTreePosition(1);
    m_layers->setUniformRowHeights(true);
    m_layers->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_layers->setDragDropMode(QAbstractItemView::InternalMove);
    m_layers->setDefaultDropAction(Qt::MoveAction);
    m_layers->setDropIndicatorShown(true);
    m_layers->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layerTree->eyeIcon = layerCellIcon("layer:eye", 18, 30);
    v->addWidget(m_layers, 1);
    connect(m_layers, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *i) {
        if (m_refreshing || !i)
            return;
        if (auto *d = currentDocument())
            d->setActiveIndex(d->indexForId(LayerTree::idOf(i)));
    });
    // A press-and-drag down the eye column is one history step, as in Photoshop.
    layerTree->visibilityStrokeStarted = [this] {
        if (auto *d = currentDocument())
            d->beginTransaction("Layer visibility");
    };
    layerTree->visibilityStrokeFinished = [this] {
        if (auto *d = currentDocument())
            d->endTransaction();
    };
    layerTree->visibilityChanged = [this](quint64 id, bool on) {
        auto *d = currentDocument();
        const int index = d ? d->indexForId(id) : -1;
        if (index >= 0 && d->state.layers[index].visible != on)
            d->mutate("Layer visibility", [d, index, on] { d->state.layers[index].visible = on; });
    };
    layerTree->soloRequested = [this](quint64 id) {
        auto *d = currentDocument();
        if (!d || d->indexForId(id) < 0)
            return;
        // The layer stays visible together with the groups that contain it.
        QSet<quint64> keep{id};
        quint64 parent = d->state.layers[d->indexForId(id)].parentId;
        while (parent && !keep.contains(parent)) {
            keep.insert(parent);
            const int index = d->indexForId(parent);
            parent = index >= 0 ? d->state.layers[index].parentId : 0;
        }
        bool alone = true;
        for (const auto &layer : d->state.layers)
            alone = alone && layer.visible == keep.contains(layer.id);
        const QString key = QString::number(quintptr(d)) + ":" + QString::number(id);
        d->mutate("Layer visibility", [this, d, keep, alone, key] {
            if (alone) {
                const auto saved = m_soloVisibility.take(key);
                for (auto &layer : d->state.layers)
                    layer.visible = saved.value(layer.id, true);
            } else {
                QHash<quint64, bool> saved;
                for (auto &layer : d->state.layers) {
                    saved[layer.id] = layer.visible;
                    layer.visible = keep.contains(layer.id);
                }
                m_soloVisibility[key] = saved;
            }
        });
    };
    layerTree->dragFinished = [this] {
        QTimer::singleShot(0, this, [this] {
            m_refreshing = true;
            refreshLayers();
            m_refreshing = false;
        });
    };
    layerTree->layersDropped = [this](const QVector<quint64> &ids, quint64 targetId,
                                      LayerTree::DropPlacement placement) {
        // Applied after the drag loop returns so the tree is not rebuilt underneath QDrag.
        QTimer::singleShot(0, this, [this, ids, targetId, placement] {
            auto *d = currentDocument();
            if (!d || ids.contains(targetId))
                return;
            const QSet<quint64> moving(ids.cbegin(), ids.cend());
            auto siblings = [d, &moving](quint64 parent) {
                QVector<quint64> result; // bottom-to-top, excluding the layers being moved
                for (const auto &layer : d->state.layers)
                    if (layer.parentId == parent && !moving.contains(layer.id))
                        result.append(layer.id);
                return result;
            };
            const int targetIndex = d->indexForId(targetId);
            const Layer *target = targetIndex >= 0 ? &d->state.layers[targetIndex] : nullptr;
            const bool container =
                target && (target->kind == LayerKind::Group || target->kind == LayerKind::Artboard);
            quint64 parent = 0, before = 0;
            if (!target) {
                const auto bottom = siblings(0);
                before = bottom.isEmpty() ? 0 : bottom.first();
            } else if (placement == LayerTree::DropPlacement::Inside && container) {
                parent = targetId;
            } else if (placement == LayerTree::DropPlacement::Below) {
                parent = target->parentId;
                before = targetId;
            } else {
                parent = target->parentId;
                const auto row = siblings(parent);
                const int at = row.indexOf(targetId);
                before = at >= 0 && at + 1 < row.size() ? row[at + 1] : 0;
            }
            const auto result = reparentLayers(d, ids, parent, before);
            if (!result.error.isEmpty())
                showMessage(result.error);
            else if (result.changed)
                selectLayerIds(result.selection);
        });
    };
    connect(m_layers, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *, int) { renameLayer(); });
    connect(m_layers, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int column) {
        auto *doc = currentDocument();
        if (!doc)
            return;
        auto *layer = doc->activeLayer();
        if (layer) {
            layer->maskTarget = column == 2 && !layer->mask.isNull();
            if (layer->maskTarget) {
                if (QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier))
                    runCommand("Toggle Layer Mask");
                else if (QApplication::keyboardModifiers().testFlag(Qt::AltModifier)) {
                    if (auto *canvas = currentCanvas())
                        canvas->setMaskPreview(canvas->maskPreview() == CanvasView::MaskPreview::Grayscale
                                                   ? CanvasView::MaskPreview::None
                                                   : CanvasView::MaskPreview::Grayscale);
                }
            } else if (auto *canvas = currentCanvas())
                canvas->setMaskPreview(CanvasView::MaskPreview::None);
            refreshProperties();
        }
        Q_UNUSED(item);
    });
    m_layers->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_layers, &QWidget::customContextMenuRequested, this, [this](QPoint p) {
        if (auto *item = m_layers->itemAt(p))
            if (auto *document = currentDocument())
                document->setActiveIndex(document->indexForId(item->data(1, Qt::UserRole).toULongLong()));
        QMenu menu;
        for (const auto &s :
             QStringList{"Duplicate Layer", "Delete Layer", "Rename Layer...", "Convert to Smart Object",
                         "Rasterize Layer", "Merge Down", "Merge Visible", "Flatten Image",
                         "Create Clipping Mask", "New Selection from Layer", "Layer Style..."})
            menu.addAction(s, [this, s] { runCommand(s); });
        auto *masks = menu.addMenu("Layer Mask");
        for (const QString &name :
             QStringList{"Add Layer Mask", "Toggle Layer Mask", "Invert Layer Mask",
                         "Load Selection from Layer Mask", "Apply Layer Mask", "Delete Layer Mask"})
            masks->addAction(name, [this, name] { runCommand(name); });
        masks->addSeparator();
        masks->addAction("Show mask grayscale", [this] {
            if (auto *canvas = currentCanvas())
                canvas->setMaskPreview(CanvasView::MaskPreview::Grayscale);
        });
        masks->addAction("Show mask overlay", [this] {
            if (auto *canvas = currentCanvas())
                canvas->setMaskPreview(CanvasView::MaskPreview::Overlay);
        });
        masks->addAction("Hide mask preview", [this] {
            if (auto *canvas = currentCanvas())
                canvas->setMaskPreview(CanvasView::MaskPreview::None);
        });
        auto *vectors = menu.addMenu("Vector Mask");
        for (const QString &name :
             QStringList{"Add Vector Mask", "Toggle Vector Mask", "Load Selection from Vector Mask",
                         "Apply Vector Mask", "Delete Vector Mask"})
            vectors->addAction(name, [this, name] { runCommand(name); });
        menu.addAction("Edit Smart Filters...", [this] { runCommand("Edit Smart Filters..."); });
        menu.exec(m_layers->mapToGlobal(p));
    });
    connect(m_blend, &QComboBox::currentTextChanged, this, [this](const QString &s) {
        if (m_refreshing)
            return;
        auto *d = currentDocument();
        if (d && d->activeLayer())
            d->mutate("Blend mode", [d, s] { d->activeLayer()->blendMode = s; });
    });
    connect(m_layerOpacity, &QSpinBox::valueChanged, this, [this](int value) {
        if (m_refreshing)
            return;
        auto *d = currentDocument();
        if (d && d->activeLayer())
            d->mutate("Layer opacity", [d, value] { d->activeLayer()->opacity = value / 100.0; });
    });
    connect(m_layerFill, &QSpinBox::valueChanged, this, [this](int value) {
        if (m_refreshing)
            return;
        auto *d = currentDocument();
        if (d && d->activeLayer())
            d->mutate("Layer fill", [d, value] { d->activeLayer()->fill = value / 100.0; });
    });
    // Footer in Photoshop's order, right-aligned: styles, mask, adjustment, group, new, delete.
    auto *footer = new QHBoxLayout;
    footer->setSpacing(2);
    footer->addStretch();
    const QList<QPair<QString, QString>> buttons = {{"layer:fx", "Layer Style..."},
                                                    {"layer:mask", "Add Layer Mask"},
                                                    {"layer:adjustment", ""},
                                                    {"layer:folder", "Group Layers"},
                                                    {"layer:new", "New Layer"},
                                                    {"layer:trash", "Delete Layer"}};
    const QHash<QString, QString> footerTips{{"Layer Style...", "Add a layer style"},
                                             {"Add Layer Mask", "Add a mask"},
                                             {"Group Layers", "Group layers (Ctrl+G)"},
                                             {"New Layer", "Create a new layer"},
                                             {"Delete Layer", "Delete layer"}};
    for (const auto &b : buttons) {
        auto *tb = new QToolButton;
        tb->setObjectName("layerFooter_" + b.first.mid(6));
        setGlyph(tb, b.first, 18);
        tb->setFixedSize(28, 26);
        footer->addWidget(tb);
        if (b.second.isEmpty()) {
            tb->setToolTip("Create a new adjustment layer");
            tb->setPopupMode(QToolButton::InstantPopup);
            auto *menu = new QMenu(tb);
            for (const auto &s : adjustmentNames()) {
                const QString glyph = adjustmentNames().indexOf(s) < 16 ? "adj:" + s : "layer:adjustment";
                auto *action =
                    menu->addAction(toolIcon(glyph), s, this, [this, s] { runCommand("Adjustment: " + s); });
                action->setData(glyph);
            }
            tb->setMenu(menu);
            tb->setStyleSheet("QToolButton::menu-indicator { image: none; width: 0; }");
        } else {
            tb->setToolTip(footerTips.value(b.second, b.second));
            connect(tb, &QToolButton::clicked, this, [this, b] { runCommand(b.second); });
        }
    }
    v->addLayout(footer);
}
void MainWindow::buildDocks() {
    auto *color = new QWidget;
    auto *colorLayout = new QVBoxLayout(color);
    colorLayout->setContentsMargins(10, 9, 10, 9);
    auto *spectrum = new QLabel;
    QImage img(250, 116, QImage::Format_RGB32);
    for (int y = 0; y < img.height(); y++)
        for (int x = 0; x < img.width(); x++)
            img.setPixelColor(x, y,
                              QColor::fromHsvF(x / double(img.width()), 1.0 - y / double(img.height()), 1));
    spectrum->setPixmap(QPixmap::fromImage(img));
    spectrum->setScaledContents(true);
    spectrum->setMinimumHeight(85);
    colorLayout->addWidget(spectrum);
    m_fgSwatch = new QLabel(m_foreground.name().toUpper());
    m_fgSwatch->setFixedHeight(20);
    colorLayout->addWidget(m_fgSwatch);
    colorLayout->addWidget(button("Choose foreground...", color, [this] {
        auto c = QColorDialog::getColor(m_foreground, this, "Foreground color");
        if (c.isValid()) {
            m_foreground = c;
            selectTool(m_tool);
        }
    }));
    dock("Color", color, true);
    for (const auto &name : QStringList{"Swatches", "Gradients", "Patterns", "Styles", "Brushes"}) {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        auto *grid = new QGridLayout;
        for (int i = 0; i < 24; i++) {
            auto *b = new QToolButton;
            b->setFixedSize(30, 26);
            QColor c = QColor::fromHsv((i * 29) % 360, 140 + (i % 3) * 40, 150 + (i % 4) * 25);
            if (name == "Swatches")
                b->setStyleSheet("background:" + c.name() + ";border:1px solid #222;");
            else if (name == "Gradients")
                b->setStyleSheet("background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 " + c.name() +
                                 ",stop:1 #eee);");
            else
                b->setIcon(toolIcon(name == "Brushes" ? "Brush" : "Rectangle", c));
            b->setToolTip(name + " preset " + QString::number(i + 1));
            grid->addWidget(b, i / 6, i % 6);
            connect(b, &QToolButton::clicked, this, [this, name, c, i] {
                if (name == "Swatches") {
                    m_foreground = c;
                    selectTool(m_tool);
                } else if (name == "Brushes") {
                    selectTool("Brush");
                    m_options->findChild<QSpinBox *>("brushSize")->setValue(5 + i * 8);
                } else if (name == "Gradients") {
                    m_foreground = c;
                    selectTool("Gradient");
                } else if (name == "Styles") {
                    runCommand("Layer Style...");
                } else {
                    m_foreground = c;
                    selectTool("Pattern Stamp");
                }
            });
        }
        v->addLayout(grid);
        v->addStretch();
        dock(name, w);
    }
    m_docks["Color"]->raise();
    auto *adjustments = new QWidget;
    auto *av = new QGridLayout(adjustments);
    av->setContentsMargins(8, 8, 8, 8);
    av->setSpacing(4);
    // The sixteen panel adjustments match Photoshop's Adjustments panel; the rest live under "More".
    const QStringList panelAdjustments = adjustmentNames().mid(0, 16);
    int ai = 0;
    for (const auto &s : panelAdjustments) {
        auto *b = new QToolButton;
        b->setObjectName("adjustment_" + s);
        setGlyph(b, "adj:" + s, 20);
        b->setToolTip("Add a " + s + " adjustment");
        b->setFixedSize(30, 28);
        av->addWidget(b, ai / 8, ai % 8);
        connect(b, &QToolButton::clicked, this, [this, s] { adjust(s); });
        ai++;
    }
    auto *more = new QToolButton;
    more->setText("More adjustments");
    more->setPopupMode(QToolButton::InstantPopup);
    auto *moreMenu = new QMenu(more);
    for (const auto &s : adjustmentNames().mid(16))
        moreMenu->addAction(s, this, [this, s] { adjust(s); });
    more->setMenu(moreMenu);
    av->addWidget(more, ai / 8 + 1, 0, 1, 8, Qt::AlignLeft);
    av->setRowStretch(ai / 8 + 2, 1);
    av->setColumnStretch(8, 1);
    dock("Adjustments", adjustments, true);
    m_docks["Adjustments"]->raise();
    auto *layers = new QWidget;
    buildLayers(layers);
    dock("Layers", layers, true);
    for (const auto &name : QStringList{"Channels", "Paths"}) {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        auto *list = new QListWidget;
        list->setObjectName(name.toLower() + "List");
        v->addWidget(list);
        if (name == "Channels") {
            list->addItems({"RGB composite", "Red", "Green", "Blue", "Selection"});
            connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *i) {
                if (i->text() == "Selection")
                    runCommand("Load Selection...");
                else {
                    auto *d = currentDocument();
                    if (d)
                        showMessage("Channel values are available in the Info panel. Use Image > Mode for "
                                    "conversion.");
                }
            });
            v->addWidget(button("Save selection as channel", w, [this] { runCommand("Save Selection..."); }));
        } else {
            v->addWidget(button("Create pen path", w, [this] { selectTool("Pen"); }));
            v->addWidget(
                button("Selection from path", w, [this] { runCommand("New Selection from Layer"); }));
        }
        dock(name, w);
    }
    m_docks["Layers"]->raise();
    m_properties = new QWidget;
    new QVBoxLayout(m_properties);
    auto *propertiesScroll = new QScrollArea;
    propertiesScroll->setWidgetResizable(true);
    propertiesScroll->setFrameShape(QFrame::NoFrame);
    propertiesScroll->setWidget(m_properties);
    dock("Properties", propertiesScroll, true);
    m_history = new QListWidget;
    m_history->setContextMenuPolicy(Qt::CustomContextMenu);
    m_history->setToolTip("Click to restore a state. Right-click to choose the History Brush source.");
    connect(m_history, &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        auto *item = m_history->itemAt(position);
        auto *canvas = currentCanvas();
        if (!item || !canvas)
            return;
        const int sourceIndex = m_history->row(item) - 1;
        QMenu menu(this);
        menu.addAction("Use as History Brush source", this, [this, canvas, sourceIndex] {
            canvas->setProperty("historySourceIndex", sourceIndex);
            showMessage("History Brush source selected.");
        });
        menu.exec(m_history->viewport()->mapToGlobal(position));
    });
    dock("History", m_history);
    connect(m_history, &QListWidget::itemClicked, this, [this](QListWidgetItem *i) {
        auto *d = currentDocument();
        if (!d)
            return;
        int target = m_history->row(i);
        int active = d->historyNames().size();
        while (active > target && d->canUndo()) {
            d->undo();
            active--;
        }
        while (active < target && d->canRedo()) {
            d->redo();
            active++;
        }
    });
    m_info = new QLabel("Open a document to inspect colors.");
    m_info->setMargin(10);
    m_info->setMinimumHeight(90);
    dock("Info", m_info);
    m_histogram = new QLabel;
    m_histogram->setMinimumSize(240, 100);
    m_histogram->setAlignment(Qt::AlignCenter);
    dock("Histogram", m_histogram);
    m_navigator = new QLabel;
    m_navigator->setMinimumSize(240, 140);
    m_navigator->setAlignment(Qt::AlignCenter);
    auto *nav = new QWidget;
    auto *nv = new QVBoxLayout(nav);
    nv->addWidget(m_navigator);
    auto *slider = new QSlider(Qt::Horizontal);
    slider->setRange(5, 400);
    slider->setValue(100);
    nv->addWidget(slider);
    connect(slider, &QSlider::valueChanged, this, [this](int v) {
        if (auto *c = currentCanvas())
            c->setZoom(v / 100.0);
    });
    dock("Navigator", nav);
    buildBrushSettings();
    for (const auto &name : QStringList{"Character", "Paragraph", "Glyphs"}) {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        if (name == "Glyphs") {
            auto *edit = new QTextEdit;
            edit->setPlainText("Aa Bb Cc 0123456789\nα β γ δ λ π Ω\n© ® ™ € £ ¥ → ← ↑ ↓");
            v->addWidget(edit);
            v->addWidget(button("Insert selected glyphs", w, [this, edit] {
                if (auto *d = currentDocument())
                    if (d->activeLayer() && d->activeLayer()->kind == LayerKind::Text)
                        d->mutate("Insert glyphs",
                                  [d, edit] { d->activeLayer()->text += edit->textCursor().selectedText(); });
            }));
        } else {
            auto *font = new QFontComboBox;
            v->addWidget(font);
            auto *size = new QSpinBox;
            size->setRange(1, 2000);
            size->setValue(48);
            v->addWidget(size);
            connect(font, &QFontComboBox::currentFontChanged, this, [this](const QFont &f) {
                auto *d = currentDocument();
                if (d && d->activeLayer() && d->activeLayer()->kind == LayerKind::Text)
                    d->mutate("Font", [d, f] {
                        int size = d->activeLayer()->font.pixelSize();
                        d->activeLayer()->font = f;
                        d->activeLayer()->font.setPixelSize(size > 0 ? size : 48);
                    });
            });
            connect(size, &QSpinBox::valueChanged, this, [this](int n) {
                auto *d = currentDocument();
                if (d && d->activeLayer() && d->activeLayer()->kind == LayerKind::Text)
                    d->mutate("Type size", [d, n] { d->activeLayer()->font.setPixelSize(n); });
            });
            v->addWidget(button("Edit text...", w, [this] { runCommand("Edit Text..."); }));
            v->addStretch();
        }
        dock(name, w);
    }
    auto *actions = new QWidget;
    auto *actv = new QVBoxLayout(actions);
    m_actionsList = new QListWidget;
    actv->addWidget(m_actionsList);
    auto *acth = new QHBoxLayout;
    acth->addWidget(button("● Record", actions, [this] {
        m_recording = !m_recording;
        if (m_recording) {
            m_recorded.clear();
            m_actionSteps = {};
            m_actionsList->clear();
        }
        showMessage(m_recording ? "Recording editing commands." : "Recording stopped.");
    }));
    acth->addWidget(button("▶ Play", actions, [this] {
        bool previous = m_recording;
        m_recording = false;
        QString error;
        if (currentDocument() && !ActionRunner::run(currentDocument(), m_actionSteps, &error))
            QMessageBox::warning(this, "Action stopped", error);
        m_recording = previous;
    }));
    acth->addWidget(button("Save", actions, [this] {
        auto path = QFileDialog::getSaveFileName(this, "Save action set", {}, "Serika actions (*.speaction)");
        if (path.isEmpty())
            return;
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument(QJsonObject{{"version", 1}, {"steps", m_actionSteps}}).toJson());
        }
    }));
    actv->addLayout(acth);
    actv->addWidget(button("Load action set...", actions, [this] {
        auto path = QFileDialog::getOpenFileName(this, "Load action set", {}, "Actions (*.speaction *.json)");
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            m_recorded.clear();
            m_actionsList->clear();
            auto obj = QJsonDocument::fromJson(file.readAll()).object();
            m_actionSteps = obj["steps"].toArray();
            if (m_actionSteps.isEmpty())
                for (const auto &v : obj["commands"].toArray())
                    m_actionSteps.append(QJsonObject{{"command", v.toString()}});
            for (const auto &v : m_actionSteps) {
                auto step = v.toObject();
                auto label = step["command"].toString() + ": " + step["name"].toString();
                m_recorded.append(label);
                m_actionsList->addItem(label);
            }
        }
    }));
    dock("Actions", actions);
    auto *notes = new QTextEdit;
    notes->setPlaceholderText("Document notes");
    dock("Notes", notes);
    connect(notes, &QTextEdit::textChanged, this, [this, notes] {
        if (m_refreshing)
            return;
        auto *d = currentDocument();
        if (d)
            d->mutate("Notes", [d, notes] { d->state.metadata["notes"] = notes->toPlainText(); });
    });
    auto *comps = new QWidget;
    auto *cv = new QVBoxLayout(comps);
    auto *list = new QListWidget;
    list->setObjectName("layerCompsList");
    cv->addWidget(list);
    cv->addWidget(button("Capture layer comp", comps, [this, list] {
        auto *d = currentDocument();
        if (!d)
            return;
        d->captureLayerComp("Comp " + QString::number(list->count() + 1));
        refresh();
    }));
    cv->addWidget(button("Delete selected comp", comps, [this, list] {
        if (auto *d = currentDocument())
            if (list->currentRow() >= 0)
                d->deleteLayerComp(list->currentRow());
    }));
    connect(list, &QListWidget::itemClicked, this, [this, list](QListWidgetItem *i) {
        if (auto *d = currentDocument())
            d->applyLayerComp(list->row(i));
    });
    dock("Layer Comps", comps);
    auto *timeline = new QWidget;
    auto *tv = new QHBoxLayout(timeline);
    auto *fps = new QSpinBox;
    fps->setRange(1, 60);
    fps->setValue(12);
    auto *play = button("▶ Play", timeline, [] {});
    tv->addWidget(play);
    tv->addWidget(new QLabel("FPS"));
    tv->addWidget(fps);
    tv->addStretch();
    auto *timer = new QTimer(timeline);
    connect(play, &QPushButton::clicked, this, [timer, fps] {
        if (timer->isActive())
            timer->stop();
        else
            timer->start(1000 / fps->value());
    });
    connect(timer, &QTimer::timeout, this, [this] {
        auto *d = currentDocument();
        if (!d || d->state.layers.isEmpty())
            return;
        int next = (d->state.activeIndex + 1) % d->state.layers.size();
        for (int i = 0; i < d->state.layers.size(); i++)
            d->state.layers[i].visible = i == next;
        d->setActiveIndex(next);
        d->touch();
    });
    dock("Timeline", timeline);
    m_docks["Timeline"]->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, m_docks["Timeline"]);
    m_docks["Timeline"]->hide();
    auto *libraries = new QTreeView;
    auto *model = new QFileSystemModel(libraries);
    QString presetDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/presets";
    QDir().mkpath(presetDir);
    model->setRootPath(presetDir);
    libraries->setModel(model);
    libraries->setRootIndex(model->index(presetDir));
    for (int i = 1; i < 4; i++)
        libraries->hideColumn(i);
    dock("Libraries", libraries);
    // Split the primary docks before tabifying companions: Qt otherwise inserts
    // splitDockWidget's second dock into the first dock's existing tab group.
    for (auto *panel : m_docks) {
        removeDockWidget(panel);
        panel->hide();
    }
    const QStringList primary = {"Color", "Adjustments", "Layers", "Properties"};
    QDockWidget *previous = nullptr;
    for (const auto &name : primary) {
        auto *panel = m_docks[name];
        addDockWidget(Qt::RightDockWidgetArea, panel);
        if (previous)
            splitDockWidget(previous, panel, Qt::Vertical);
        panel->show();
        previous = panel;
    }
    const QHash<QString, QString> companions = {{"Swatches", "Color"},     {"Gradients", "Color"},
                                                {"Styles", "Adjustments"}, {"Brush Settings", "Adjustments"},
                                                {"Channels", "Layers"},    {"Paths", "Layers"}};
    for (auto it = m_docks.begin(); it != m_docks.end(); ++it) {
        if (primary.contains(it.key()))
            continue;
        addDockWidget(it.key() == "Timeline" ? Qt::BottomDockWidgetArea : Qt::RightDockWidgetArea,
                      it.value());
        if (companions.contains(it.key()))
            tabifyDockWidget(m_docks[companions[it.key()]], it.value());
        it.value()->hide();
    }
    for (const auto &name : primary)
        m_docks[name]->raise();
    resizeDocks({m_docks["Color"], m_docks["Adjustments"], m_docks["Layers"], m_docks["Properties"]},
                {190, 150, 365, 190}, Qt::Vertical);
    resizeDocks({m_docks["Layers"]}, {278}, Qt::Horizontal);
}
void MainWindow::selectTool(const QString &tool) {
    m_tool = tool;
    for (const auto &group : ShortcutRegistry::toolGroups())
        if (group.second.contains(tool))
            m_lastToolInGroup[group.first] = tool;
    for (auto *b : m_tools)
        b->setChecked(false);
    if (m_tools.contains(tool)) {
        m_tools[tool]->setChecked(true);
        m_tools[tool]->setProperty("iconTool", tool);
        m_tools[tool]->setIcon(toolIcon(tool));
    }
    m_options->findChild<QLabel *>("toolLabel")->setText(tool);
    bool paint = QStringList{"Brush",         "Pencil",
                             "Mixer Brush",   "Color Replacement",
                             "Clone Stamp",   "Pattern Stamp",
                             "History Brush", "Art History Brush",
                             "Eraser",        "Background Eraser",
                             "Magic Eraser",  "Spot Healing",
                             "Healing Brush", "Blur",
                             "Sharpen",       "Smudge",
                             "Dodge",         "Burn",
                             "Sponge",        "Quick Selection"}
                     .contains(tool);
    bool type = tool.contains("Type");
    for (auto *action : m_options->actions()) {
        if (action->property("brushOption").toBool())
            action->setVisible(paint);
        if (action->property("moveOption").toBool())
            action->setVisible(tool == "Move" || tool == "Artboard");
        if (action->property("typeOption").toBool())
            action->setVisible(type);
        if (action->property("cropOption").toBool())
            action->setVisible(tool == "Crop" || tool == "Perspective Crop");
    }
    if (auto *c = currentCanvas()) {
        c->setTool(tool);
        c->setProperty("brushBlendMode", m_options->findChild<QComboBox *>("brushMode")->currentText());
        c->setForeground(m_foreground);
        c->setBackground(m_background);
        if (m_brushSettings)
            c->setBrushPreset(m_brushSettings->preset());
        c->setProperty("autoSelect", m_options->findChild<QCheckBox *>("autoSelect")->isChecked());
        c->setProperty("showTransformControls",
                       m_options->findChild<QCheckBox *>("showTransformControls")->isChecked());
        c->setProperty("typeFont", m_options->findChild<QFontComboBox *>("typeFont")->currentFont());
        c->setProperty("typeSize", m_options->findChild<QSpinBox *>("typeSize")->value());
        applyCropOptions();
    }
    if (m_fgSwatch) {
        m_fgSwatch->setText(m_foreground.name().toUpper());
        m_fgSwatch->setStyleSheet("border-left:20px solid " + m_foreground.name() + ";padding-left:8px;");
    }
}
void MainWindow::addDocument(Document *document) {
    document->setParent(this);
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto *canvas = new CanvasView(document, page);
    canvas->setObjectName("canvas");
    canvas->setProperty("externalShortcuts", true);
    connect(canvas, &CanvasView::interactionError, this, &MainWindow::showMessage);
    connect(canvas, &CanvasView::cropSettingsChanged, this, [this, canvas] {
        if (canvas != currentCanvas())
            return;
        if (auto *overlay = m_options->findChild<QComboBox *>("cropOverlay")) {
            QSignalBlocker block(overlay);
            overlay->setCurrentText(canvas->cropSettings().overlay);
        }
    });
    connect(canvas, &CanvasView::brushSettingsChanged, this, [this, canvas] {
        if (canvas != currentCanvas())
            return;
        syncBrushSettings(canvas->brushPreset());
    });
    v->addWidget(canvas, 1);
    auto *status = new QWidget;
    status->setFixedHeight(24);
    auto *h = new QHBoxLayout(status);
    h->setContentsMargins(6, 0, 6, 0);
    h->setSpacing(12);
    auto *zoom = button("100%", status, [this, canvas] {
        bool ok = false;
        double value =
            QInputDialog::getDouble(this, "Zoom", "Percentage", canvas->zoom() * 100, 1, 6400, 1, &ok);
        if (ok)
            canvas->setZoom(value / 100);
    });
    zoom->setFixedWidth(64);
    zoom->setFlat(true);
    h->addWidget(zoom);
    auto *label = new QLabel;
    label->setObjectName("documentStatus");
    h->addWidget(label);
    h->addStretch();
    auto *hint = new QLabel;
    hint->setObjectName("muted");
    hint->setText("Space: pan   ·   Alt: sample   ·   [ ]: brush size");
    h->addWidget(hint);
    v->addWidget(status);
    auto *context = new QFrame(canvas);
    context->setObjectName("contextBar");
    auto *ch = new QHBoxLayout(context);
    ch->setContentsMargins(7, 5, 7, 5);
    ch->setSpacing(4);
    for (const auto &pair : QList<QPair<QString, QString>>{{"Select subject", "Subject"},
                                                           {"Remove background", "Remove Background"},
                                                           {"Add mask", "Add Layer Mask"},
                                                           {"Invert", "Inverse"},
                                                           {"Deselect", "Deselect"}}) {
        auto *b = button(pair.first, context, [this, pair] { runCommand(pair.second); });
        b->setObjectName("context_" + pair.second);
        ch->addWidget(b);
    }
    context->adjustSize();
    context->move(36, 30);
    auto update = [this, document, page, canvas, label, zoom, context] {
        int index = m_tabs->indexOf(page);
        if (index >= 0)
            m_tabs->setTabText(index, document->title + (document->isModified() ? " *" : ""));
        label->setText(QString("%1 × %2 px  |  %3 / %4  |  %5-bit  |  %6 ppi")
                           .arg(document->state.size.width())
                           .arg(document->state.size.height())
                           .arg(document->state.colorMode,
                                document->state.iccProfile.isEmpty() ? "sRGB" : "Embedded ICC")
                           .arg(document->state.bitDepth)
                           .arg(document->state.resolution));
        zoom->setText(QString::number(canvas->zoom() * 100, 'f', 1) + "%");
        bool selected = document->hasSelection();
        context->findChild<QPushButton *>("context_Inverse")->setVisible(selected);
        context->findChild<QPushButton *>("context_Deselect")->setVisible(selected);
        context->findChild<QPushButton *>("context_Subject")->setVisible(!selected);
        context->findChild<QPushButton *>("context_Remove Background")->setVisible(!selected);
        context->adjustSize();
        context->move(std::max(24, (canvas->width() - context->width()) / 2), 30);
        if (currentDocument() == document) {
            m_commands["Undo"]->setEnabled(document->canUndo());
            m_commands["Redo"]->setEnabled(document->canRedo());
            if (!m_refreshTimer.isActive())
                m_refreshTimer.start();
        }
    };
    connect(document, &Document::changed, this, update);
    connect(document, &Document::activeLayerChanged, this, update);
    connect(canvas, &CanvasView::zoomChanged, this, [update](qreal) { update(); });
    connect(canvas, &CanvasView::colorPicked, this, [this](QColor c) {
        m_foreground = c;
        selectTool(m_tool);
    });
    connect(canvas, &CanvasView::cursorInfo, this, [this](QPointF p, QColor c) {
        m_info->setText(QString("Actual   R %1   G %2   B %3\nX %4 px    Y %5 px\n%6")
                            .arg(c.red())
                            .arg(c.green())
                            .arg(c.blue())
                            .arg(p.x(), 0, 'f', 1)
                            .arg(p.y(), 0, 'f', 1)
                            .arg(m_tool));
    });
    int index = m_tabs->addTab(page, document->title);
    m_tabs->setCurrentIndex(index);
    m_center->setCurrentWidget(m_tabs);
    canvas->setSurround(themeCanvas(m_theme));
    canvas->setShowRulers(m_rulers);
    canvas->setShowGrid(m_grid);
    canvas->setShowGuides(m_guides);
    canvas->setQuickMask(m_quickMask);
    selectTool(m_tool);
    canvas->setFocus();
    QTimer::singleShot(0, canvas, [canvas] { canvas->fitToView(); });
    update();
}
void MainWindow::refreshLayers() {
    QHash<quint64, bool> expanded;
    QSet<quint64> selected;
    std::function<void(QTreeWidgetItem *)> remember = [&](QTreeWidgetItem *item) {
        quint64 id = item->data(1, Qt::UserRole).toULongLong();
        expanded[id] = item->isExpanded();
        if (item->isSelected())
            selected.insert(id);
        for (int child = 0; child < item->childCount(); ++child)
            remember(item->child(child));
    };
    for (int index = 0; index < m_layers->topLevelItemCount(); ++index)
        remember(m_layers->topLevelItem(index));
    m_layers->clear();
    auto *d = currentDocument();
    if (!d)
        return;
    QHash<quint64, QTreeWidgetItem *> items;
    QTreeWidgetItem *active = nullptr;
    auto *tree = static_cast<LayerTree *>(m_layers);
    const QIcon lockIcon = layerCellIcon("layer:lock", 14, 26);
    const QIcon folderIcon = toolIcon("layer:folder");
    const QIcon textIcon = toolIcon("Horizontal Type");
    bool anyMask = false, anyBadge = false;
    // Photoshop-style thumbnail: content centred on a checkerboard inside a thin frame.
    const auto layerThumbnail = [this](const QImage &image) {
        const QSize box(42, 30);
        QPixmap pixmap(box * devicePixelRatioF());
        pixmap.setDevicePixelRatio(devicePixelRatioF());
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        const QSize fitted = image.size().scaled(box - QSize(2, 2), Qt::KeepAspectRatio);
        const QRect frame(QPoint((box.width() - fitted.width()) / 2, (box.height() - fitted.height()) / 2),
                          fitted);
        for (int y = 0; y < frame.height(); y += 4)
            for (int x = 0; x < frame.width(); x += 4)
                painter.fillRect(QRect(frame.x() + x, frame.y() + y, 4, 4).intersected(frame),
                                 (x / 4 + y / 4) % 2 ? QColor("#CCCCCC") : QColor("#FFFFFF"));
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(frame, image);
        painter.setPen(m_layers->palette().color(QPalette::Mid));
        painter.drawRect(frame.adjusted(0, 0, -1, -1));
        return QIcon(pixmap);
    };
    for (int i = d->state.layers.size() - 1; i >= 0; i--) {
        const auto &l = d->state.layers[i];
        auto *item = new QTreeWidgetItem;
        const bool container = l.kind == LayerKind::Group || l.kind == LayerKind::Artboard;
        // Only groups and artboards accept drops onto the row; other rows insert above or below.
        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
        if (container)
            flags |= Qt::ItemIsDropEnabled;
        item->setFlags(flags);
        tree->setEyeVisible(item, l.visible);
        item->setText(1, l.clipped ? "↳ " + l.name : l.name);
        item->setToolTip(1, l.name);
        item->setData(1, Qt::UserRole, QVariant::fromValue(l.id));
        item->setData(1, LayerTree::ContainerRole, container);
        if (l.locked || l.lockAlpha || l.lockPosition) {
            item->setIcon(3, lockIcon);
            item->setToolTip(3, l.locked      ? "Pixels locked"
                                : l.lockAlpha ? "Transparency locked"
                                              : "Position locked");
        }
        if (!l.effects.isEmpty()) {
            item->setText(3, "fx");
            item->setToolTip(3, "Layer effects");
            QFont font = item->font(3);
            font.setItalic(true);
            item->setFont(3, font);
        }
        if (!l.visible)
            item->setForeground(1, m_layers->palette().color(QPalette::Disabled, QPalette::Text));
        if (container) {
            item->setIcon(1, folderIcon);
        } else if (l.kind == LayerKind::Text) {
            item->setIcon(1, textIcon);
        } else {
            QImage image;
            if (l.kind == LayerKind::Pixel || l.kind == LayerKind::SmartObject) {
                QSize extent = l.pixels.size;
                QSize thumb = extent.scaled(QSize(42, 30), Qt::KeepAspectRatio);
                image = QImage(thumb, QImage::Format_RGBA8888);
                image.fill(Qt::transparent);
                if (!extent.isEmpty()) {
                    QPainter painter(&image);
                    painter.setRenderHint(QPainter::SmoothPixmapTransform);
                    painter.scale(thumb.width() / double(extent.width()),
                                  thumb.height() / double(extent.height()));
                    for (auto it = l.pixels.tiles.begin(); it != l.pixels.tiles.end(); ++it)
                        painter.drawImage(QPoint(int(it.key() >> 32) * 256, int(quint32(it.key())) * 256),
                                          it.value());
                }
            } else
                image = d->layerImage(l);
            if (!image.isNull())
                item->setIcon(1, layerThumbnail(image));
        }
        items[l.id] = item;
        if (!l.mask.isNull()) {
            item->setIcon(2, layerThumbnail(l.mask));
            anyMask = true;
        }
        anyBadge = anyBadge || l.locked || l.lockAlpha || l.lockPosition || !l.effects.isEmpty();
        item->setToolTip(2, l.maskTarget ? "Layer mask targeted" : "Click to paint on the layer mask");
        item->setExpanded(expanded.value(l.id, true));
        item->setSelected(selected.contains(l.id));
        if (i == d->state.activeIndex)
            active = item;
    }
    for (int i = d->state.layers.size() - 1; i >= 0; i--) {
        const auto &layer = d->state.layers[i];
        auto *item = items[layer.id];
        if (layer.parentId && items.contains(layer.parentId) && layer.parentId != layer.id)
            items[layer.parentId]->addChild(item);
        else
            m_layers->addTopLevelItem(item);
        item->setExpanded(expanded.value(layer.id, true));
    }
    // Mask and badge columns only take room from the name when some layer needs them.
    m_layers->setColumnWidth(2, anyMask ? 46 : 0);
    m_layers->setColumnWidth(3, anyBadge ? 26 : 0);
    if (active) {
        m_layers->setCurrentItem(active, 1, QItemSelectionModel::NoUpdate);
        active->setSelected(true);
    }
    if (auto *l = d->activeLayer()) {
        if (l->kind == LayerKind::Group || l->kind == LayerKind::Artboard) {
            if (m_blend->findText("Pass Through") < 0)
                m_blend->addItem("Pass Through");
        } else {
            int pass = m_blend->findText("Pass Through");
            if (pass >= 0)
                m_blend->removeItem(pass);
        }
        m_blend->setCurrentText(l->blendMode);
        m_layerOpacity->setValue(qRound(l->opacity * 100));
        m_layerFill->setValue(qRound(l->fill * 100));
        auto *w = m_docks["Layers"]->widget();
        w->findChild<QToolButton *>("lockAlpha")->setChecked(l->lockAlpha);
        w->findChild<QToolButton *>("lockPixels")->setChecked(l->locked);
        w->findChild<QToolButton *>("lockPosition")->setChecked(l->lockPosition);
    }
    updateLayerOperationUi();
}
void MainWindow::refreshProperties() {
    auto *layout = m_properties->layout();
    while (auto *item = layout->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    auto *d = currentDocument();
    if (!d) {
        layout->addWidget(new QLabel("No document open"));
        return;
    }
    auto *l = d->activeLayer();
    if (!l)
        return;
    auto *title = new QLabel(l->name);
    title->setStyleSheet("font-weight:600;");
    layout->addWidget(title);
    auto *info = new QLabel(QString("%1 × %2 px\nX %3   Y %4")
                                .arg(d->state.size.width())
                                .arg(d->state.size.height())
                                .arg(l->offset.x())
                                .arg(l->offset.y()));
    layout->addWidget(info);
    if (l->kind == LayerKind::Adjustment) {
        layout->addWidget(new QLabel(l->adjustment));
        layout->addWidget(button("Edit adjustment...", m_properties, [this] {
            auto *doc = currentDocument();
            if (doc && doc->activeLayer())
                adjust(doc->activeLayer()->adjustment);
        }));
    }
    if (l->kind == LayerKind::Text) {
        layout->addWidget(new QLabel(QString("%1 · %2 px").arg(l->font.family()).arg(l->font.pixelSize())));
        layout->addWidget(button("Edit text...", m_properties, [this] { runCommand("Edit Text..."); }));
    }
    if (l->kind == LayerKind::GradientFill || l->kind == LayerKind::PatternFill ||
        l->kind == LayerKind::SolidFill)
        layout->addWidget(button("Edit fill...", m_properties, [this] { runCommand("Edit Fill..."); }));
    if (!l->mask.isNull())
        addMaskProperties(d, l->id, false);
    if (!l->vectorMask.isEmpty())
        addMaskProperties(d, l->id, true);
    if (!l->smartFilters.isEmpty())
        addSmartFilterProperties(d, l->id);
    layout->addWidget(button("Transform...", m_properties, [this] { transformDialog(); }));
    qobject_cast<QVBoxLayout *>(layout)->addStretch();
}
void MainWindow::refresh() {
    if (m_refreshing)
        return;
    if (static_cast<LayerTree *>(m_layers)->dragging) {
        m_refreshTimer.start();
        return;
    }
    m_refreshing = true;
    auto *d = currentDocument();
    if (bool(d) != m_editing) {
        m_editing = bool(d);
        m_toolDock->setVisible(m_editing);
        m_options->setVisible(m_editing);
        if (m_editing)
            setWorkspace(m_workspace);
        else
            for (auto *panel : m_docks)
                panel->hide();
    }
    setWindowTitle(d ? d->title + " — Serika PhotoEdit" : "Serika PhotoEdit");
    m_center->setCurrentWidget(m_tabs->count() ? static_cast<QWidget *>(m_tabs) : m_home);
    if (m_commands.contains("Undo"))
        m_commands["Undo"]->setEnabled(d && d->canUndo());
    if (m_commands.contains("Redo"))
        m_commands["Redo"]->setEnabled(d && d->canRedo());
    const auto proof = d ? d->state.metadata.value("proofing").toObject() : QJsonObject{};
    m_commands["Proof Colors"]->setChecked(proof.value("enabled").toBool());
    m_commands["Gamut Warning"]->setChecked(proof.value("gamutWarning").toBool());
    for (const auto &name : QStringList{"Proof Setup...", "Proof Colors", "Gamut Warning",
                                        "Export CMYK TIFF...", "Export CMYK Separations..."})
        m_commands[name]->setEnabled(d != nullptr);
    refreshLayers();
    refreshProperties();
    if (auto *list = m_docks["Layer Comps"]->widget()->findChild<QListWidget *>("layerCompsList")) {
        int selected = list->currentRow();
        list->clear();
        if (d)
            for (const auto &value : d->layerComps())
                list->addItem(value.toObject().value("name").toString());
        list->setCurrentRow(selected);
    }
    m_history->clear();
    if (d) {
        auto names = d->historyNames();
        m_history->addItem("Initial state");
        for (const auto &s : names) {
            auto *item = new QListWidgetItem(s, m_history);
            item->setData(Qt::UserRole, false);
        }
        if (m_history->count())
            m_history->setCurrentRow(m_history->count() - 1);
        auto image = d->composite();
        m_navigator->setPixmap(
            QPixmap::fromImage(image.scaled(244, 138, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        QImage small = image.scaled(256, 256, Qt::KeepAspectRatio, Qt::FastTransformation);
        int bins[256] = {};
        for (int y = 0; y < small.height(); y++)
            for (int x = 0; x < small.width(); x++)
                bins[qGray(small.pixel(x, y))]++;
        int maximum = *std::max_element(std::begin(bins), std::end(bins));
        QImage graph(256, 100, QImage::Format_RGB32);
        graph.fill(qApp->palette().color(QPalette::Base));
        QPainter painter(&graph);
        painter.setPen(qApp->palette().color(QPalette::Text));
        for (int i = 0; i < 256; i++)
            painter.drawLine(i, 99, i, 99 - (maximum ? bins[i] * 90 / maximum : 0));
        m_histogram->setPixmap(QPixmap::fromImage(graph));
        if (auto *notes = qobject_cast<QTextEdit *>(m_docks["Notes"]->widget()))
            if (notes->toPlainText() != d->state.metadata["notes"].toString())
                notes->setPlainText(d->state.metadata["notes"].toString());
        if (auto *paths = m_docks["Paths"]->widget()->findChild<QListWidget *>()) {
            paths->clear();
            for (const auto &l : d->state.layers)
                if (!l.shape.isEmpty())
                    paths->addItem(l.name);
        }
    } else {
        m_navigator->clear();
        m_histogram->clear();
    }
    m_refreshing = false;
}
void MainWindow::applyTheme(const QString &name) {
    qApp->setPalette(themePalette(name));
    qApp->setStyleSheet(themeStyle(name));
    m_settings.setValue("theme", name);
    for (auto *widget : QApplication::topLevelWidgets()) {
        auto *window = qobject_cast<MainWindow *>(widget);
        if (!window)
            continue;
        window->m_theme = name;
        for (auto *tool : window->findChildren<QToolButton *>()) {
            const auto glyph = tool->property("glyph").toString();
            if (!glyph.isEmpty()) {
                tool->setIcon(toolIcon(glyph));
                if (tool->menu())
                    for (auto *action : tool->menu()->actions())
                        if (!action->data().toString().isEmpty())
                            action->setIcon(toolIcon(action->data().toString()));
                continue;
            }
            const auto iconName = tool->property("iconTool").toString();
            if (iconName.isEmpty())
                continue;
            tool->setIcon(toolIcon(iconName));
            if (tool->menu())
                for (auto *action : tool->menu()->actions())
                    action->setIcon(toolIcon(action->text()));
        }
        if (window->m_tabs)
            for (int i = 0; i < window->m_tabs->count(); i++)
                if (auto *canvas = window->m_tabs->widget(i)->findChild<CanvasView *>())
                    canvas->setSurround(themeCanvas(name));
        if (window->m_layers)
            static_cast<LayerTree *>(window->m_layers)->eyeIcon = layerCellIcon("layer:eye", 18, 30);
        if (window->m_navigator && window->m_histogram)
            window->refresh();
    }
}
void MainWindow::setWorkspace(const QString &name, bool reset) {
    m_workspace = name;
    m_settings.setValue("workspace", name);
    QString file =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/workspaces/" + name + ".json";
    if (!reset) {
        QFile f(file);
        if (f.open(QIODevice::ReadOnly)) {
            auto obj = QJsonDocument::fromJson(f.readAll()).object();
            if (obj["schema"].toInt() == 3 &&
                restoreState(QByteArray::fromBase64(obj["layout"].toString().toLatin1()), 1)) {
                m_toolDock->setVisible(bool(currentDocument()));
                m_options->setVisible(bool(currentDocument()));
                if (!currentDocument())
                    for (auto *panel : m_docks)
                        panel->hide();
                return;
            }
        }
    }
    if (!m_essentialState.isEmpty())
        restoreState(m_essentialState, 1);
    for (auto *d : m_docks)
        d->hide();
    for (const auto &s : QStringList{"Color", "Swatches", "Gradients", "Adjustments", "Styles", "Layers",
                                     "Channels", "Paths", "Properties"})
        m_docks[s]->show();
    m_docks["Layers"]->raise();
    m_docks["Color"]->raise();
    m_docks["Adjustments"]->raise();
    if (name == "Photography") {
        m_docks["Histogram"]->show();
        tabifyDockWidget(m_docks["Color"], m_docks["Histogram"]);
        m_docks["Histogram"]->raise();
    }
    if (name == "Painting") {
        m_docks["Brushes"]->show();
        tabifyDockWidget(m_docks["Adjustments"], m_docks["Brushes"]);
        m_docks["Brush Settings"]->show();
        m_docks["Brush Settings"]->raise();
    }
    if (name == "Graphic and Web") {
        m_docks["Character"]->show();
        m_docks["Paragraph"]->show();
        tabifyDockWidget(m_docks["Adjustments"], m_docks["Character"]);
        tabifyDockWidget(m_docks["Character"], m_docks["Paragraph"]);
        m_docks["Character"]->raise();
    }
    if (name == "Motion")
        m_docks["Timeline"]->show();
    if (auto *picker = m_options->findChild<QComboBox *>("workspacePicker")) {
        QSignalBlocker block(picker);
        picker->setCurrentText(name);
    }
    if (currentDocument())
        QTimer::singleShot(0, this, [this] {
            resizeDocks({m_docks["Color"], m_docks["Adjustments"], m_docks["Layers"], m_docks["Properties"]},
                        m_workspace == "Painting" ? QList<int>{170, 400, 300, 140}
                                                  : QList<int>{200, 110, 500, 150},
                        Qt::Vertical);
        });
    m_toolDock->setVisible(bool(currentDocument()));
    m_options->setVisible(bool(currentDocument()));
    if (!currentDocument())
        for (auto *panel : m_docks)
            panel->hide();
}
void MainWindow::saveWorkspace() {
    if (!currentDocument())
        return;
    QString folder = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/workspaces";
    QDir().mkpath(folder);
    QFile f(folder + "/" + m_workspace + ".json");
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(QJsonObject{{"schema", 3},
                                          {"name", m_workspace},
                                          {"layout", QString::fromLatin1(saveState(1).toBase64())}})
                    .toJson());
}
void MainWindow::setPanelsVisible(bool toolbarAlso) {
    if (!currentDocument())
        return;
    bool &hidden = toolbarAlso ? m_panelsHidden : m_docksHidden;
    hidden = !hidden;
    if (hidden) {
        for (auto it = m_docks.begin(); it != m_docks.end(); ++it) {
            m_visibilityBeforeHide[it.key()] = it.value()->isVisible();
            it.value()->hide();
        }
    } else
        for (auto it = m_docks.begin(); it != m_docks.end(); ++it)
            it.value()->setVisible(m_visibilityBeforeHide.value(it.key()));
    if (toolbarAlso) {
        m_toolDock->setVisible(!hidden);
        m_options->setVisible(!hidden);
    }
}
void MainWindow::showMessage(const QString &text) {
    auto *d = currentDocument();
    if (d) {
        auto *label = m_tabs->currentWidget()->findChild<QLabel *>("documentStatus");
        if (label) {
            label->setText(text);
            QTimer::singleShot(6500, this, [this] {
                if (auto *doc = currentDocument())
                    if (auto *label = m_tabs->currentWidget()->findChild<QLabel *>("documentStatus"))
                        label->setText(QString("%1 × %2 px  |  %3  |  %4-bit")
                                           .arg(doc->state.size.width())
                                           .arg(doc->state.size.height())
                                           .arg(doc->state.colorMode)
                                           .arg(doc->state.bitDepth));
            });
        }
    } else
        QMessageBox::information(this, "Serika PhotoEdit", text);
}
void MainWindow::showImportReport(Document *d) {
    if (d->importReport.isEmpty())
        return;
    auto *report = new QTextEdit;
    report->setReadOnly(true);
    report->setPlainText(d->importReport.join("\n"));
    auto *panel = dock("Import Report", report, true);
    panel->setFloating(true);
    panel->resize(480, 320);
}
void MainWindow::addRecent(const QString &path) {
    m_recent.removeAll(path);
    m_recent.prepend(path);
    while (m_recent.size() > 12)
        m_recent.removeLast();
    m_settings.setValue("recent", m_recent);
}
void MainWindow::openFile(const QString &path) {
    qreal pdfDpi = 150;
    if (QFileInfo(path).suffix().compare("pdf", Qt::CaseInsensitive) == 0) {
        bool ok;
        pdfDpi = QInputDialog::getDouble(this, "Import PDF", "Resolution (dpi)", 150, 36, 1200, 0, &ok);
        if (!ok)
            return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    auto *d = QFileInfo(path).suffix().compare("pdf", Qt::CaseInsensitive) == 0
                  ? FormatIO::openPdf(path, pdfDpi, &error, this)
                  : FormatIO::open(path, &error, this);
    QApplication::restoreOverrideCursor();
    if (!d) {
        QMessageBox::warning(this, "Open failed", error);
        return;
    }
    if (FormatIO::isRaw(path)) {
        develop(d);
        if (d->state.metadata["developCancelled"].toBool()) {
            delete d;
            return;
        }
    }
    addDocument(d);
    addRecent(path);
    showImportReport(d);
}
bool MainWindow::saveDocument(bool saveAs, bool copy) {
    auto *d = currentDocument();
    if (!d)
        return false;
    if (isSmartObjectContents(d))
        return saveSmartObjectContents(d, saveAs || copy);
    QString path = d->filePath;
    const QString extension = QFileInfo(path).suffix().toLower();
    if (!copy && !path.isEmpty() && !QStringList{"spe", "speb", "psd", "psb"}.contains(extension)) {
        saveAs = true;
        path = QFileInfo(path).absolutePath() + "/" + QFileInfo(path).completeBaseName() + ".spe";
    }
    if (saveAs || path.isEmpty()) {
        path =
            QFileDialog::getSaveFileName(this, copy ? "Save a copy" : "Save document",
                                         path.isEmpty() ? d->title + ".spe" : path, FormatIO::saveFilter());
        if (path.isEmpty())
            return false;
        if (QFileInfo(path).suffix().isEmpty())
            path += ".spe";
    }
    QString error;
    QString oldPath = d->filePath, oldTitle = d->title;
    bool ok = FormatIO::save(d, path, &error);
    if (!ok) {
        QMessageBox::warning(this, "Save failed", error);
        return false;
    }
    if (copy) {
        d->filePath = oldPath;
        d->title = oldTitle;
    } else {
        d->filePath = path;
        d->title = QFileInfo(path).fileName();
        d->markSaved();
        addRecent(path);
    }
    refresh();
    return true;
}
bool MainWindow::closeDocument(int index) {
    if (index < 0 || index >= m_tabs->count())
        return true;
    auto *page = m_tabs->widget(index);
    auto *c = page->findChild<CanvasView *>();
    auto *d = c ? c->document() : nullptr;
    if (d && d->isModified()) {
        m_tabs->setCurrentIndex(index);
        auto result = QMessageBox::question(this, "Save changes?", "Save changes to " + d->title + "?",
                                            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (result == QMessageBox::Cancel)
            return false;
        if (result == QMessageBox::Save && !saveDocument())
            return false;
    }
    m_tabs->removeTab(index);
    page->deleteLater();
    if (d)
        d->deleteLater();
    refresh();
    return true;
}
void MainWindow::closeEvent(QCloseEvent *event) {
    saveWorkspace();
    while (m_tabs->count())
        if (!closeDocument(m_tabs->count() - 1)) {
            event->ignore();
            return;
        }
    m_settings.setValue("geometry", saveGeometry());
    event->accept();
}
void MainWindow::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}
void MainWindow::dropEvent(QDropEvent *e) {
    for (const auto &url : e->mimeData()->urls())
        if (url.isLocalFile())
            openFile(url.toLocalFile());
}
void MainWindow::detachDocument(int index) {
    if (index < 0 || index >= m_tabs->count())
        return;
    auto *page = m_tabs->widget(index);
    auto *canvas = page->findChild<CanvasView *>();
    if (!canvas)
        return;
    auto *document = canvas->document();
    disconnect(document, nullptr, this, nullptr);
    auto *window = new MainWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    m_tabs->removeTab(index);
    window->addDocument(document);
    page->deleteLater();
    window->show();
    refresh();
}
bool MainWindow::eventFilter(QObject *object, QEvent *event) {
    if (object == m_tabs->tabBar()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            int index = m_tabs->tabBar()->tabAt(mouse->position().toPoint());
            if (mouse->button() == Qt::MiddleButton) {
                closeDocument(index);
                return true;
            }
            if (mouse->button() == Qt::LeftButton && index >= 0)
                m_draggedPage = m_tabs->widget(index);
        } else if (event->type() == QEvent::MouseButtonRelease) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton && m_draggedPage) {
                int index = m_tabs->indexOf(m_draggedPage);
                m_draggedPage = nullptr;
                if (!rect().contains(mapFromGlobal(mouse->globalPosition().toPoint()))) {
                    detachDocument(index);
                    return true;
                }
            }
        }
    }
    return QMainWindow::eventFilter(object, event);
}
void MainWindow::saveRecovery() {
    if (!m_settings.value("recovery", true).toBool())
        return;
    QString folder = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/recovery";
    QDir().mkpath(folder);
    for (int i = 0; i < m_tabs->count(); i++) {
        auto *c = m_tabs->widget(i)->findChild<CanvasView *>();
        if (c && c->document()->isModified())
            FormatIO::saveNative(c->document(), folder + "/" + QString::number(i) + "-" +
                                                    QFileInfo(c->document()->title).completeBaseName() +
                                                    ".spe");
    }
}
void MainWindow::openDemo() {
    auto *d = Document::create(QSize(1600, 1000), Qt::transparent, 8, this);
    d->title = "Amber valley.spe";
    d->state.layers.clear();
    auto layer = [&](QString name, const std::function<void(QPainter &)> &draw) {
        d->addLayer(name);
        d->activeLayer()->pixels.paint(QRect(QPoint(), d->state.size), draw);
    };
    layer("Evening sky", [](QPainter &p) {
        QLinearGradient g(0, 0, 0, 1000);
        g.setColorAt(0, QColor("#453b59"));
        g.setColorAt(.55, QColor("#d48376"));
        g.setColorAt(1, QColor("#efb685"));
        p.fillRect(QRect(0, 0, 1600, 1000), g);
    });
    layer("Amber sun", [](QPainter &p) {
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#f5c897"));
        p.drawEllipse(QPointF(1100, 330), 116, 116);
    });
    layer("Far ridge", [](QPainter &p) {
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#896779"));
        p.drawPolygon(QPolygonF{QPointF(0, 800), QPointF(0, 610), QPointF(270, 375), QPointF(570, 620),
                                QPointF(900, 310), QPointF(1280, 690), QPointF(1600, 450),
                                QPointF(1600, 1000), QPointF(0, 1000)});
    });
    layer("Valley", [](QPainter &p) {
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#504a67"));
        QPainterPath path;
        path.moveTo(0, 680);
        path.cubicTo(500, 710, 520, 990, 950, 680);
        path.cubicTo(1270, 470, 1400, 670, 1600, 690);
        path.lineTo(1600, 1000);
        path.lineTo(0, 1000);
        p.drawPath(path);
    });
    layer("Foreground", [](QPainter &p) {
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#292e43"));
        QPainterPath path;
        path.moveTo(0, 825);
        path.cubicTo(380, 675, 1050, 1035, 1600, 775);
        path.lineTo(1600, 1000);
        path.lineTo(0, 1000);
        p.drawPath(path);
        for (int i = 0; i < 12; i++) {
            int x = 30 + i * 32;
            int y = 770 + i * 3;
            p.drawPolygon(
                QPolygonF{QPointF(x, y - 80 - i * 2), QPointF(x - 25, y + 25), QPointF(x + 25, y + 25)});
            p.fillRect(x - 3, y, 6, 60, QColor("#292e43"));
        }
    });
    d->addLayer("AMBER VALLEY", LayerKind::Text);
    auto *text = d->activeLayer();
    text->text = "AMBER VALLEY";
    text->font = QFont("Segoe UI");
    text->font.setPixelSize(43);
    text->font.setLetterSpacing(QFont::AbsoluteSpacing, 9);
    text->color = QColor("#ffe1bd");
    text->offset = QPointF(98, 125);
    d->touch();
    d->markSaved();
    addDocument(d);
}
} // namespace serika
