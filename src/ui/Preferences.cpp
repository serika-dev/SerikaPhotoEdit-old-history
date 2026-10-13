#include "MainWindow.h"
#include "compositor/GpuProcessor.h"
#include "tools/AiSegmenter.h"
#include "tools/LocalAlgorithms.h"
#include "tools/OnnxRuntime.h"
#include "tools/Upscaler.h"
#include "ui/SettingsWidgets.h"
#include "ui/Theme.h"
#include "ui/canvas/CanvasView.h"
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace serika {
namespace {
QPixmap themeSwatch(const QString &theme, qreal ratio) {
    const QPalette colors = themePalette(theme);
    QPixmap pixmap(QSize(120, 64) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0.5, 0.5, 119, 63), 6, 6);
    p.setClipPath(clip);
    p.fillRect(QRectF(0, 0, 120, 64), colors.color(QPalette::Window));
    p.fillRect(QRectF(0, 0, 120, 10), colors.color(QPalette::Button));
    p.fillRect(QRectF(0, 10, 14, 54), colors.color(QPalette::Button));
    p.fillRect(QRectF(88, 10, 32, 54), colors.color(QPalette::Button));
    p.fillRect(QRectF(22, 18, 58, 38), themeCanvas(theme));
    p.fillRect(QRectF(30, 25, 42, 24), QColor("#FFFFFF"));
    p.fillRect(QRectF(92, 16, 24, 5), colors.color(QPalette::Highlight));
    for (int i = 0; i < 4; ++i)
        p.fillRect(QRectF(92, 26 + i * 8, i == 1 ? 16 : 24, 3), colors.color(QPalette::Dark));
    p.setClipping(false);
    p.setPen(QPen(colors.color(QPalette::Dark), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(0.5, 0.5, 119, 63), 6, 6);
    return pixmap;
}

void setBadge(QLabel *badge, const QString &state, const QString &text) {
    badge->setText(text);
    badge->setProperty("state", state);
    badge->style()->unpolish(badge);
    badge->style()->polish(badge);
}

// The page/card/row scaffolding of the dialog, and the search index over its rows.
class SettingsBuilder {
  public:
    struct Row {
        QFrame *frame;
        QString keywords;
    };
    struct Card {
        QWidget *container;
        QVBoxLayout *layout;
        QString keywords;
        std::vector<Row> rows;
    };
    struct Page {
        QString name;
        QWidget *widget;
        QVBoxLayout *body;
        QLabel *empty;
        std::vector<std::unique_ptr<Card>> cards;
    };
    std::vector<std::unique_ptr<Page>> pages;
    QStackedWidget *stack = nullptr;
    QListWidget *nav = nullptr;

    Page &page(const QString &name, const QString &subtitle, const QString &glyph) {
        auto page = std::make_unique<Page>();
        page->name = name;
        auto *scroll = new QScrollArea;
        scroll->setObjectName("prefsScroll");
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *content = new QWidget;
        content->setObjectName("prefsPage");
        page->body = new QVBoxLayout(content);
        page->body->setContentsMargins(32, 26, 32, 24);
        page->body->setSpacing(6);
        auto *title = new QLabel(name);
        title->setObjectName("prefsTitle");
        auto *sub = new QLabel(subtitle);
        sub->setObjectName("prefsSubtitle");
        sub->setWordWrap(true);
        page->body->addWidget(title);
        page->body->addWidget(sub);
        page->body->addSpacing(12);
        page->empty = new QLabel("No settings on this page match your search.");
        page->empty->setObjectName("prefsEmpty");
        page->empty->hide();
        page->body->addWidget(page->empty);
        page->body->addStretch(1);
        scroll->setWidget(content);
        page->widget = scroll;
        stack->addWidget(scroll);
        auto *item = new QListWidgetItem(toolIcon(glyph), name, nav);
        item->setSizeHint(QSize(0, 34));
        pages.push_back(std::move(page));
        return *pages.back();
    }
    Card &card(Page &page, const QString &title, const QString &keywords = {}) {
        auto card = std::make_unique<Card>();
        card->container = new QWidget;
        card->container->setObjectName("prefsBare");
        auto *outer = new QVBoxLayout(card->container);
        outer->setContentsMargins(0, 8, 0, 8);
        outer->setSpacing(6);
        auto *heading = new QLabel(title.toUpper());
        heading->setObjectName("prefsCardTitle");
        outer->addWidget(heading);
        auto *frame = new QFrame;
        frame->setObjectName("prefsCard");
        card->layout = new QVBoxLayout(frame);
        card->layout->setContentsMargins(16, 4, 16, 4);
        card->layout->setSpacing(0);
        outer->addWidget(frame);
        card->keywords = (title + " " + keywords).toLower();
        // Cards go above the empty-state label and the closing stretch.
        page.body->insertWidget(page.body->count() - 2, card->container);
        page.cards.push_back(std::move(card));
        return *page.cards.back();
    }
    // A labelled row; `stacked` puts a wide control under the text instead of beside it.
    QFrame *row(Card &card, const QString &label, const QString &hint, QWidget *control,
                bool stacked = false) {
        auto *frame = new QFrame;
        frame->setObjectName("prefsRow");
        frame->setProperty("first", card.rows.empty());
        auto *text = new QWidget;
        text->setObjectName("prefsBare");
        auto *textLayout = new QVBoxLayout(text);
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(2);
        auto *title = new QLabel(label);
        title->setObjectName("prefsLabel");
        textLayout->addWidget(title);
        QLabel *hintLabel = nullptr;
        if (!hint.isEmpty()) {
            hintLabel = new QLabel(hint);
            hintLabel->setObjectName("prefsHint");
            hintLabel->setWordWrap(true);
            hintLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            textLayout->addWidget(hintLabel);
        }
        if (stacked) {
            auto *layout = new QVBoxLayout(frame);
            layout->setContentsMargins(0, 12, 0, 12);
            layout->setSpacing(8);
            layout->addWidget(text);
            if (control)
                layout->addWidget(control);
        } else {
            auto *layout = new QHBoxLayout(frame);
            layout->setContentsMargins(0, 12, 0, 12);
            layout->setSpacing(18);
            layout->addWidget(text, 1);
            if (control)
                layout->addWidget(control, 0, Qt::AlignVCenter | Qt::AlignRight);
        }
        frame->setProperty("hintLabel", QVariant::fromValue<QObject *>(hintLabel));
        card.layout->addWidget(frame);
        card.rows.push_back({frame, (label + " " + hint).toLower()});
        return frame;
    }
    // Shows only rows matching every search word and returns the first page with a match.
    int filter(const QString &query) {
        const QStringList words = query.toLower().split(' ', Qt::SkipEmptyParts);
        int first = -1;
        for (size_t p = 0; p < pages.size(); ++p) {
            auto &page = *pages[p];
            bool pageMatches = false;
            for (auto &card : page.cards) {
                bool cardMatches = false;
                const bool cardTitleMatches =
                    !words.isEmpty() && std::all_of(words.begin(), words.end(), [&](const QString &w) {
                        return card->keywords.contains(w);
                    });
                for (auto &row : card->rows) {
                    const bool match =
                        cardTitleMatches || std::all_of(words.begin(), words.end(), [&](const QString &w) {
                            return row.keywords.contains(w) || page.name.toLower().contains(w);
                        });
                    row.frame->setVisible(match);
                    cardMatches |= match;
                }
                card->container->setVisible(cardMatches);
                pageMatches |= cardMatches;
            }
            page.empty->setVisible(!pageMatches);
            nav->item(int(p))->setHidden(!pageMatches);
            if (pageMatches && first < 0)
                first = int(p);
        }
        return first;
    }
};
} // namespace

void MainWindow::preferences(const QString &initialPage) {
    QDialog dialog(this);
    dialog.setWindowTitle("Preferences");
    dialog.setObjectName("preferences");
    dialog.resize(940, 660);
    dialog.setMinimumSize(760, 520);
    auto *outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *main = new QHBoxLayout;
    main->setSpacing(0);
    outer->addLayout(main, 1);

    auto *sidebar = new QWidget;
    sidebar->setObjectName("prefsSidebar");
    sidebar->setAttribute(Qt::WA_StyledBackground);
    sidebar->setFixedWidth(220);
    auto *side = new QVBoxLayout(sidebar);
    side->setContentsMargins(14, 18, 14, 14);
    side->setSpacing(10);
    auto *brand = new QLabel("PREFERENCES");
    brand->setObjectName("prefsBrand");
    side->addWidget(brand);
    auto *search = new QLineEdit;
    search->setObjectName("prefsSearch");
    search->setPlaceholderText("Search settings");
    search->setClearButtonEnabled(true);
    side->addWidget(search);
    SettingsBuilder ui;
    ui.nav = new QListWidget;
    ui.nav->setObjectName("prefsNav");
    ui.nav->setIconSize(QSize(18, 18));
    ui.nav->setFocusPolicy(Qt::StrongFocus);
    ui.nav->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    side->addWidget(ui.nav, 1);
    auto *version = new QLabel("Serika PhotoEdit " + QCoreApplication::applicationVersion());
    version->setObjectName("prefsBrand");
    side->addWidget(version);
    main->addWidget(sidebar);
    ui.stack = new QStackedWidget;
    main->addWidget(ui.stack, 1);

    // Each control registers how to return to its default for "Restore Defaults".
    std::vector<std::function<void()>> defaults;
    const auto toggle = [&](bool checked, bool fallback) {
        auto *s = new ToggleSwitch;
        s->setChecked(checked);
        defaults.push_back([s, fallback] { s->setChecked(fallback); });
        return s;
    };
    const auto spin = [&](int min, int max, int value, int fallback, const QString &suffix = {}) {
        auto *s = new QSpinBox;
        s->setRange(min, max);
        s->setValue(value);
        s->setSuffix(suffix);
        s->setMinimumWidth(110);
        defaults.push_back([s, fallback] { s->setValue(fallback); });
        return s;
    };
    const auto bare = [](QLayout *layout) {
        auto *w = new QWidget;
        w->setObjectName("prefsBare");
        w->setLayout(layout);
        layout->setContentsMargins(0, 0, 0, 0);
        return w;
    };

    // General
    auto &general = ui.page("General", "Startup, recovery and undo.", "pref:general");
    auto &startup = ui.card(general, "Startup");
    auto *home = toggle(m_settings.value("showHome", true).toBool(), true);
    ui.row(startup, "Show Home at launch", "Recent files and quick-start templates when no document is open.",
           home);
    auto *recovery = toggle(m_settings.value("recovery", true).toBool(), true);
    ui.row(startup, "Automatic recovery",
           "Saves a recovery copy of modified documents every 10 minutes, offered again after a crash.",
           recovery);
    auto &historyCard = ui.card(general, "History", "undo");
    auto *states = spin(1, 1000, m_settings.value("history", 50).toInt(), 50, " steps");
    states->setObjectName("historyStates");
    ui.row(historyCard, "History states",
           "Undo steps kept per document. Applies to open and newly opened documents; more steps use more "
           "memory.",
           states);

    // Interface
    auto &interface = ui.page("Interface", "Theme, panels and on-canvas helpers.", "pref:interface");
    auto &appearance = ui.card(interface, "Appearance", "theme color colour dark light");
    const QString originalTheme = m_theme;
    QString chosenTheme = m_theme;
    auto *themeGrid = new QHBoxLayout;
    themeGrid->setSpacing(10);
    std::vector<std::pair<QString, ChoiceTile *>> themeTiles;
    const auto selectTheme = [&](const QString &name, bool apply) {
        chosenTheme = name;
        for (auto &[theme, tile] : themeTiles)
            tile->setChecked(theme == name);
        if (apply && name != m_theme)
            applyTheme(name); // live preview; Cancel restores the original
    };
    for (const QString theme : {"Darkest", "Dark", "Light", "Lightest"}) {
        auto *tile = new ChoiceTile;
        auto *v = new QVBoxLayout(tile);
        v->setContentsMargins(8, 8, 8, 8);
        v->setSpacing(6);
        auto *swatch = new QLabel;
        swatch->setPixmap(themeSwatch(theme, devicePixelRatioF()));
        swatch->setAlignment(Qt::AlignCenter);
        auto *name = new QLabel(theme);
        name->setAlignment(Qt::AlignCenter);
        v->addWidget(swatch);
        v->addWidget(name);
        tile->setToolTip(theme + " theme");
        tile->clicked = [&selectTheme, theme] { selectTheme(theme, true); };
        themeTiles.emplace_back(theme, tile);
        themeGrid->addWidget(tile);
    }
    themeGrid->addStretch(1);
    selectTheme(m_theme, false);
    defaults.push_back([&selectTheme] { selectTheme("Dark", true); });
    ui.row(appearance, "Color theme", "Previewed as you choose; Cancel restores the previous theme.",
           bare(themeGrid), true);
    auto &panels = ui.card(interface, "Workspace", "panels layout");
    auto *taskBar = toggle(m_settings.value("contextTaskBar", false).toBool(), false);
    ui.row(panels, "Contextual task bar",
           "A floating bar under the canvas with Select Subject, Remove Background and mask actions.",
           taskBar);
    auto *reset = new QPushButton("Reset Layout");
    connect(reset, &QPushButton::clicked, this, [this] { setWorkspace(m_workspace, true); });
    ui.row(panels, "Panel layout",
           "Puts the panels of the " + m_workspace + " workspace back where they started.", reset);
    auto &display = ui.card(interface, "Display", "dpi scaling zoom monitor");
    auto *displaySettings = new QPushButton("Display Settings…");
    connect(displaySettings, &QPushButton::clicked, this,
            [] { QDesktopServices::openUrl(QUrl("ms-settings:display")); });
#ifndef Q_OS_WIN
    displaySettings->hide();
#endif
    ui.row(
        display, "Interface scaling",
        "Serika follows the system display scale of each monitor and stays sharp at 125 %, 150 % and beyond.",
        displaySettings);

    // Performance
    auto &performance = ui.page("Performance", "Graphics processor and blending.", "pref:performance");
    auto &gpuCard = ui.card(performance, "Graphics processor", "gpu opengl");
    const auto gpuInfo = GpuProcessor::instance().diagnostics();
    auto *gpu = toggle(GpuProcessor::instance().enabled(), false);
    gpu->setObjectName("gpuProcessing");
    gpu->setEnabled(gpuInfo.available || gpu->isChecked());
    ui.row(gpuCard, "Use the GPU",
           "Runs supported adjustments and blurs on the graphics card; anything else stays on the CPU.", gpu);
    auto *gpuBadge = new QLabel;
    gpuBadge->setObjectName("prefsBadge");
    setBadge(gpuBadge, gpuInfo.available ? "ok" : "off", gpuInfo.available ? "Available" : "Unavailable");
    ui.row(gpuCard, "Status", gpuInfo.summary(), gpuBadge);
    auto &blending = ui.card(performance, "Rendering", "colour color gamma compositor");
    auto *linear = toggle(m_settings.value("blendLinear", false).toBool(), false);
    ui.row(
        blending, "Blend in linear light",
        "Like Photoshop's \"Blend RGB colors using gamma 1.0\": smoother transparent edges, different look "
        "for blend modes. Applies to open documents.",
        linear);
    ui.row(blending, "Compositor",
           "CPU raster compositor with sparse 256 × 256 copy-on-write tiles and fast paths "
           "for plain, masked and text layers.",
           nullptr);

    // Tools
    auto &toolsPage = ui.page("Tools", "Defaults for painting tools.", "Brush");
    auto &painting = ui.card(toolsPage, "Painting", "brush");
    auto *brush = spin(1, 5000, m_options->findChild<QSpinBox *>("brushSize")->value(), 40, " px");
    ui.row(painting, "Brush size", "Diameter of the Brush, Eraser and other painting tools.", brush);

    // Export
    auto &exportPage = ui.page("Export", "Defaults for Export As.", "pref:export");
    auto &qualityCard = ui.card(exportPage, "Compression", "jpeg webp quality");
    auto *quality = spin(1, 100, m_settings.value("jpegQuality", 92).toInt(), 92);
    auto *qualitySlider = new QSlider(Qt::Horizontal);
    qualitySlider->setRange(1, 100);
    qualitySlider->setValue(quality->value());
    qualitySlider->setMinimumWidth(180);
    connect(qualitySlider, &QSlider::valueChanged, quality, &QSpinBox::setValue);
    connect(quality, &QSpinBox::valueChanged, qualitySlider, &QSlider::setValue);
    auto *qualityRow = new QHBoxLayout;
    qualityRow->setSpacing(10);
    qualityRow->addWidget(qualitySlider);
    qualityRow->addWidget(quality);
    ui.row(qualityCard, "JPEG and WebP quality",
           "Higher keeps more detail and makes larger files. 85–95 suits most work.", bare(qualityRow));

    // AI
    auto &ai = ui.page("AI",
                       "Local models for subject selection and super resolution. Images never leave this "
                       "computer.",
                       "pref:ai");
    auto &subjectCard = ui.card(ai, "Select Subject and Remove Background", "segmentation birefnet engine");
    QString chosenEngine = preferredSubjectEngine() == SubjectEngine::Ai ? "ai" : "fast";
    std::vector<std::pair<QString, ChoiceTile *>> engineTiles;
    const auto selectEngine = [&](const QString &id) {
        chosenEngine = id;
        for (auto &[engine, tile] : engineTiles)
            tile->setChecked(engine == id);
    };
    auto *engineGrid = new QHBoxLayout;
    engineGrid->setSpacing(10);
    for (const auto &[id, title, text] :
         {std::tuple{QString("fast"), QString("Fast"),
                     QString("Colour model. Instant, very low resource use; best "
                             "on plain backgrounds.")},
          std::tuple{
              QString("ai"), QString("AI · BiRefNet"),
              QString("Neural network on this computer. Handles hair and busy backgrounds; 5–30 s on a "
                      "CPU.")}}) {
        auto *tile = new ChoiceTile;
        auto *v = new QVBoxLayout(tile);
        v->setContentsMargins(12, 10, 12, 10);
        v->setSpacing(3);
        auto *name = new QLabel(title);
        name->setObjectName("prefsLabel");
        auto *description = new QLabel(text);
        description->setObjectName("prefsHint");
        description->setWordWrap(true);
        v->addWidget(name);
        v->addWidget(description);
        tile->clicked = [&selectEngine, id] { selectEngine(id); };
        engineTiles.emplace_back(id, tile);
        engineGrid->addWidget(tile, 1);
    }
    selectEngine(chosenEngine);
    defaults.push_back([&selectEngine] { selectEngine("fast"); });
    ui.row(subjectCard, "Engine", "Used by Select > Subject, Layer > Remove Background and AI agents.",
           bare(engineGrid), true);
    auto *subjectBadge = new QLabel;
    subjectBadge->setObjectName("prefsBadge");
    auto *subjectDownload = new QPushButton;
    auto *subjectControls = new QHBoxLayout;
    subjectControls->setSpacing(8);
    subjectControls->addWidget(subjectBadge);
    subjectControls->addWidget(subjectDownload);
    const auto segmentModel = AiSegmenter::defaultModel();
    ui.row(subjectCard, segmentModel.name + " model",
           segmentModel.license + " licence · " + QLocale().formattedDataSize(segmentModel.bytes) +
               " · from "
               "Hugging Face",
           bare(subjectControls));

    auto &upscaleCard = ui.card(ai, "Super Resolution", "upscale enlarge esrgan resolution pixelated");
    QString chosenUpscaler = Upscaler::preferredModelId();
    struct UpscaleWidgets {
        QString id;
        ChoiceTile *tile;
        QLabel *badge;
        QPushButton *download;
    };
    std::vector<UpscaleWidgets> upscaleTiles;
    const auto selectUpscaler = [&](const QString &id) {
        chosenUpscaler = id;
        for (auto &w : upscaleTiles)
            w.tile->setChecked(w.id == id);
    };
    auto *upscaleGrid = new QHBoxLayout;
    upscaleGrid->setSpacing(10);
    for (const auto &model : Upscaler::models()) {
        auto *tile = new ChoiceTile;
        auto *v = new QVBoxLayout(tile);
        v->setContentsMargins(12, 10, 12, 10);
        v->setSpacing(4);
        auto *head = new QHBoxLayout;
        auto *name = new QLabel(model.name);
        name->setObjectName("prefsLabel");
        auto *badge = new QLabel;
        badge->setObjectName("prefsBadge");
        head->addWidget(name, 1);
        head->addWidget(badge);
        auto *description = new QLabel(model.summary);
        description->setObjectName("prefsHint");
        description->setWordWrap(true);
        auto *meta = new QLabel(model.license + " · " + QLocale().formattedDataSize(model.bytes));
        meta->setObjectName("prefsHint");
        auto *download = new QPushButton("Download");
        auto *foot = new QHBoxLayout;
        foot->addWidget(meta, 1);
        foot->addWidget(download);
        v->addLayout(head);
        v->addWidget(description, 1);
        v->addLayout(foot);
        const QString id = model.id;
        tile->clicked = [&selectUpscaler, id] { selectUpscaler(id); };
        upscaleTiles.push_back({id, tile, badge, download});
        upscaleGrid->addWidget(tile, 1);
    }
    selectUpscaler(chosenUpscaler);
    defaults.push_back([&selectUpscaler] { selectUpscaler(Upscaler::defaultModelId()); });
    ui.row(
        upscaleCard, "AI model",
        "Used by Image > Super Resolution, Layer > Upscale Layer and AI agents. Real-ESRGAN enlarges 4× and "
        "invents plausible detail; Preserve Details needs no model.",
        bare(upscaleGrid), true);

    auto &runtimeCard = ui.card(ai, "ONNX Runtime", "runtime library path model file directml gpu");
    auto *runtimeBadge = new QLabel;
    runtimeBadge->setObjectName("prefsBadge");
    auto *runtimeRow = ui.row(runtimeCard, "Runtime", "Checking…", runtimeBadge);
    auto *runtimeHint = qobject_cast<QLabel *>(runtimeRow->property("hintLabel").value<QObject *>());
    const auto pathEditor = [&](const QString &value, const QString &placeholder, const QString &filter,
                                QLineEdit **edit) {
        auto *line = new QLineEdit(value);
        line->setPlaceholderText(placeholder);
        line->setClearButtonEnabled(true);
        auto *browse = new QPushButton("Browse…");
        connect(browse, &QPushButton::clicked, &dialog, [line, filter, &dialog] {
            const QString start = line->text().isEmpty() ? line->placeholderText() : line->text();
            const QString file =
                QFileDialog::getOpenFileName(&dialog, "Choose file", QFileInfo(start).absolutePath(), filter);
            if (!file.isEmpty()) {
                line->setText(QDir::toNativeSeparators(file));
                emit line->editingFinished();
            }
        });
        defaults.push_back([line] { line->clear(); });
        auto *h = new QHBoxLayout;
        h->setSpacing(8);
        h->addWidget(line, 1);
        h->addWidget(browse);
        *edit = line;
        return bare(h);
    };
    QLineEdit *modelPath = nullptr, *runtimePath = nullptr;
    ui.row(runtimeCard, "Subject model file", "Leave empty for the downloaded BiRefNet model.",
           pathEditor(m_settings.value("ai/modelPath").toString(),
                      QDir::toNativeSeparators(AiSegmenter::defaultModelPath()), "ONNX models (*.onnx)",
                      &modelPath),
           true);
    ui.row(runtimeCard, "Runtime library",
           "Leave empty to use the " + OnnxRuntime::libraryFileName() +
               " shipped with Serika. Install the DirectML build to run models on a DirectX 12 GPU.",
           pathEditor(m_settings.value("ai/runtimePath").toString(), "Automatic",
                      "ONNX Runtime (" + OnnxRuntime::libraryFileName() + ")", &runtimePath),
           true);

    auto &agentsCard = ui.card(ai, "AI agents", "mcp claude attach");
    auto *allowAgents = toggle(m_settings.value("mcp/allowLive", true).toBool(), true);
    ui.row(agentsCard, "Allow AI agents to edit open documents",
           "Agents started with --mcp --attach can work on the tabs in this window. Every change is one undo "
           "step.",
           allowAgents);

    const auto refreshAi = [&] {
        AiSegmenter::instance().setModelPath(modelPath->text().trimmed());
        OnnxRuntime::instance().setPath(runtimePath->text().trimmed());
        const auto status = AiSegmenter::instance().status();
        const bool modelReady = status.modelAvailable;
        setBadge(subjectBadge, modelReady ? "ok" : "warn", modelReady ? "Installed" : "Not installed");
        subjectDownload->setVisible(!modelReady);
        subjectDownload->setText("Download " + QLocale().formattedDataSize(segmentModel.bytes));
        for (auto &w : upscaleTiles) {
            const bool installed = Upscaler::installed(w.id);
            setBadge(w.badge, installed ? "ok" : "off", installed ? "Installed" : "Not installed");
            w.download->setVisible(!installed);
        }
        auto &runtime = OnnxRuntime::instance();
        setBadge(runtimeBadge, status.runtimeAvailable ? "ok" : "warn",
                 status.runtimeAvailable ? (runtime.directMlAvailable() ? "GPU · DirectML" : "CPU")
                                         : "Missing");
        runtimeHint->setText(status.runtimeAvailable ? "ONNX Runtime " + status.runtimeVersion + " · " +
                                                           QDir::toNativeSeparators(status.runtimePath)
                                                     : status.message);
    };
    connect(subjectDownload, &QPushButton::clicked, &dialog, [this, &refreshAi] {
        downloadAiModel();
        refreshAi();
    });
    for (auto &w : upscaleTiles)
        connect(w.download, &QPushButton::clicked, &dialog, [this, &refreshAi, &selectUpscaler, id = w.id] {
            if (downloadUpscaleModel(id))
                selectUpscaler(id);
            refreshAi();
        });
    connect(modelPath, &QLineEdit::editingFinished, &dialog, refreshAi);
    connect(runtimePath, &QLineEdit::editingFinished, &dialog, refreshAi);
    refreshAi();

    // Navigation, search and footer.
    connect(ui.nav, &QListWidget::currentRowChanged, ui.stack, &QStackedWidget::setCurrentIndex);
    connect(search, &QLineEdit::textChanged, &dialog, [&](const QString &text) {
        const int first = ui.filter(text);
        auto *current = ui.nav->currentItem();
        if (first >= 0 && (!current || current->isHidden()))
            ui.nav->setCurrentRow(first);
    });
    const QString startPage =
        initialPage.isEmpty() ? m_settings.value("preferences/page", "General").toString() : initialPage;
    ui.nav->setCurrentRow(0);
    for (int i = 0; i < ui.nav->count(); ++i)
        if (ui.nav->item(i)->text() == startPage)
            ui.nav->setCurrentRow(i);

    auto *footer = new QFrame;
    footer->setObjectName("prefsFooter");
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(16, 10, 16, 10);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::RestoreDefaults | QDialogButtonBox::Cancel |
                                         QDialogButtonBox::Ok);
    buttons->button(QDialogButtonBox::Ok)->setObjectName("primary");
    buttons->button(QDialogButtonBox::Ok)->setDefault(true);
    buttons->button(QDialogButtonBox::RestoreDefaults)->setToolTip("Resets every setting in this dialog");
    footerLayout->addWidget(buttons);
    outer->addWidget(footer);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog, [&] {
        for (const auto &reset : defaults)
            reset();
        refreshAi();
    });
    search->setFocus();

    const int result = dialog.exec();
    m_settings.setValue("preferences/page",
                        ui.nav->currentItem() ? ui.nav->currentItem()->text() : "General");
    const auto restoreAiPaths = [this] {
        AiSegmenter::instance().setModelPath(m_settings.value("ai/modelPath").toString());
        OnnxRuntime::instance().setPath(m_settings.value("ai/runtimePath").toString());
    };
    if (result != QDialog::Accepted) {
        if (m_theme != originalTheme)
            applyTheme(originalTheme);
        restoreAiPaths();
        return;
    }
    if (chosenTheme != m_theme)
        applyTheme(chosenTheme);
    SubjectEngine engine = SubjectEngine::Fast;
    parseSubjectEngine(chosenEngine, &engine);
    setPreferredSubjectEngine(engine);
    Upscaler::setPreferredModelId(chosenUpscaler);
    m_settings.setValue("ai/modelPath", modelPath->text().trimmed());
    m_settings.setValue("ai/runtimePath", runtimePath->text().trimmed());
    m_settings.setValue("mcp/allowLive", allowAgents->isChecked());
    restoreAiPaths();
    if (taskBar->isChecked() != m_settings.value("contextTaskBar", false).toBool())
        runCommand("Contextual Task Bar");
    m_settings.setValue("showHome", home->isChecked());
    m_settings.setValue("recovery", recovery->isChecked());
    m_settings.setValue("history", states->value());
    m_settings.setValue("jpegQuality", quality->value());
    m_settings.setValue("blendLinear", linear->isChecked());
    m_settings.setValue("gpuProcessing", gpu->isChecked());
    GpuProcessor::instance().setEnabled(gpu->isChecked());
    m_options->findChild<QSpinBox *>("brushSize")->setValue(brush->value());
    for (int i = 0; i < m_tabs->count(); i++) {
        auto *d = m_tabs->widget(i)->findChild<CanvasView *>()->document();
        d->historyLimit = states->value();
        d->blendLinear = linear->isChecked();
        d->touch();
    }
}
} // namespace serika
