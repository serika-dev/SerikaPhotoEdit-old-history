#include "MainWindow.h"
#include "actions/ActionRunner.h"
#include "compositor/GpuProcessor.h"
#include "io/FormatIO.h"
#include "tools/AiSegmenter.h"
#include "tools/LocalAlgorithms.h"
#include "ui/dialogs/CurvesEditor.h"
#include "ui/dialogs/FillEditorDialog.h"
#include "ui/dialogs/MaskRefineDialog.h"
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QColorDialog>
#include <QColorSpace>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSet>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QToolBar>
#include <QTreeWidget>
#include <algorithm>
namespace serika {
static QDialogButtonBox *dialogButtons(QDialog &d, QVBoxLayout *v) {
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    v->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    return buttons;
}
void MainWindow::newDocument() {
    QDialog dialog(this);
    dialog.setWindowTitle("New document");
    dialog.resize(650, 430);
    auto *outer = new QVBoxLayout(&dialog);
    auto *title = new QLabel("Create your canvas");
    title->setObjectName("sectionTitle");
    outer->addWidget(title);
    auto *h = new QHBoxLayout;
    auto *presets = new QListWidget;
    presets->addItems({"Web · 1920 × 1080", "Photo · 3000 × 2000", "Print · A4 at 300 ppi",
                       "Social · 1080 × 1080", "Portrait · 1080 × 1920", "Custom"});
    h->addWidget(presets, 1);
    auto *form = new QFormLayout;
    auto *name = new QLineEdit("Untitled-1");
    auto *width = new QSpinBox;
    width->setRange(1, 300000);
    width->setValue(1920);
    auto *height = new QSpinBox;
    height->setRange(1, 300000);
    height->setValue(1080);
    auto *ppi = new QDoubleSpinBox;
    ppi->setRange(1, 2400);
    ppi->setValue(72);
    auto *depth = new QComboBox;
    depth->addItems({"8 bit", "16 bit", "32 bit float"});
    auto *background = new QComboBox;
    background->addItems({"White", "Transparent", "Black", "Foreground color"});
    form->addRow("Name", name);
    form->addRow("Width (px)", width);
    form->addRow("Height (px)", height);
    form->addRow("Resolution (ppi)", ppi);
    form->addRow("RGB depth", depth);
    form->addRow("Background", background);
    h->addLayout(form, 1);
    outer->addLayout(h);
    auto *summary = new QLabel("sRGB IEC61966-2.1  ·  Square pixels");
    summary->setObjectName("muted");
    outer->addWidget(summary);
    dialogButtons(dialog, outer);
    connect(presets, &QListWidget::currentRowChanged, &dialog, [width, height, ppi](int row) {
        const QSize sizes[] = {QSize(1920, 1080), QSize(3000, 2000), QSize(2480, 3508), QSize(1080, 1080),
                               QSize(1080, 1920)};
        if (row < 5 && row >= 0) {
            width->setValue(sizes[row].width());
            height->setValue(sizes[row].height());
            ppi->setValue(row == 2 ? 300 : 72);
        }
    });
    presets->setCurrentRow(0);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (qint64(width->value()) * height->value() > 80000000) {
        QMessageBox::warning(this, "Canvas size",
                             "This CPU compositor currently supports canvases up to 80 million pixels. "
                             "Choose smaller dimensions.");
        return;
    }
    QColor bg = background->currentIndex() == 1   ? QColor(Qt::transparent)
                : background->currentIndex() == 2 ? QColor(Qt::black)
                : background->currentIndex() == 3 ? m_foreground
                                                  : QColor(Qt::white);
    int bits = depth->currentIndex() == 0 ? 8 : depth->currentIndex() == 1 ? 16 : 32;
    auto *d = Document::create(QSize(width->value(), height->value()), bg, bits, this);
    d->title = name->text().isEmpty() ? "Untitled-1" : name->text();
    d->state.resolution = ppi->value();
    d->state.iccProfile = QColorSpace(QColorSpace::SRgb).iccProfile();
    d->markSaved();
    addDocument(d);
}
void MainWindow::adjust(const QString &name, bool destructive) {
    auto *d = currentDocument();
    if (!d || !d->activeLayer())
        return;
    const bool editing = !destructive && d->activeLayer()->kind == LayerKind::Adjustment &&
                         d->activeLayer()->adjustment == name;
    if (editing) {
        QSet<quint64> visited;
        const Layer *layer = d->activeLayer();
        while (layer && !visited.contains(layer->id)) {
            visited.insert(layer->id);
            if (layer->locked) {
                showMessage("Unlock the adjustment layer and its parent groups before editing it.");
                return;
            }
            const int parent = d->indexForId(layer->parentId);
            layer = parent < 0 ? nullptr : &d->state.layers[parent];
        }
    }
    QJsonObject params = editing ? d->activeLayer()->parameters : QJsonObject();
    if (name == "Invert" || name == "Desaturate" || name == "Auto Tone" || name == "Auto Contrast" ||
        name == "Auto Color" || name == "Equalize") {
        QString error;
        if (!ActionRunner::execute(d,
                                   QJsonObject{{"command", "adjustment"},
                                               {"name", name},
                                               {"parameters", QJsonObject{{"destructive", destructive}}}},
                                   &error)) {
            QMessageBox::warning(this, name, error);
            return;
        }
        recordStep("adjustment", name, QJsonObject{{"destructive", destructive}});
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(name);
    dialog.resize(500, 360);
    auto *v = new QVBoxLayout(&dialog);
    auto *preview = new QLabel;
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumHeight(145);
    v->addWidget(preview);
    auto *form = new QFormLayout;
    v->addLayout(form);
    QHash<QString, QDoubleSpinBox *> values;
    CurvesEditor *curves = nullptr;
    QPushButton *gradientEditor = nullptr;
    auto field = [&](QString label, QString key, double min, double max, double def, double step = 1) {
        auto *spin = new QDoubleSpinBox;
        spin->setRange(min, max);
        spin->setSingleStep(step);
        spin->setDecimals(step < 1 ? 2 : 0);
        spin->setValue(params.value(key).toDouble(def));
        form->addRow(label, spin);
        values[key] = spin;
    };
    if (name == "Levels") {
        field("Input black", "inputBlack", 0, 254, 0);
        field("Gamma", "gamma", .1, 9.99, 1, .05);
        field("Input white", "inputWhite", 1, 255, 255);
        field("Output black", "outputBlack", 0, 255, 0);
        field("Output white", "outputWhite", 0, 255, 255);
    } else if (name == "Curves") {
        curves = new CurvesEditor;
        curves->setParameters(params);
        curves->setSource(d->composite());
        v->insertWidget(1, curves, 1);
        dialog.resize(560, 680);
    } else if (name == "Hue/Saturation") {
        field("Hue", "hue", -180, 180, 0);
        field("Saturation", "saturation", -100, 100, 0);
        field("Lightness", "lightness", -100, 100, 0);
    } else if (name == "Brightness/Contrast") {
        field("Brightness", "brightness", -100, 100, 0);
        field("Contrast", "contrast", -100, 100, 0);
    } else if (name == "Exposure") {
        field("Exposure (EV)", "exposure", -10, 10, 0, .1);
        field("Offset", "offset", -.5, .5, 0, .01);
        field("Gamma", "gamma", .1, 5, 1, .05);
    } else if (name == "Vibrance") {
        field("Vibrance", "vibrance", -100, 100, 0);
        field("Saturation", "saturation", -100, 100, 0);
    } else if (name == "Color Balance") {
        field("Cyan — Red", "red", -100, 100, 0);
        field("Magenta — Green", "green", -100, 100, 0);
        field("Yellow — Blue", "blue", -100, 100, 0);
    } else if (name == "Posterize")
        field("Levels", "levels", 2, 256, 4);
    else if (name == "Threshold")
        field("Threshold", "threshold", 0, 255, 128);
    else if (name == "Shadows/Highlights") {
        field("Shadows", "shadows", -100, 100, 25);
        field("Highlights", "highlights", -100, 100, 0);
    } else if (name == "Photo Filter") {
        field("Density (%)", "density", 0, 100, 25);
        params["color"] = m_foreground.name();
    } else if (name == "Gradient Map") {
        if (!editing) {
            params["start"] = m_foreground.name(QColor::HexArgb);
            params["end"] = m_background.name(QColor::HexArgb);
        }
        gradientEditor = new QPushButton("Edit gradient stops...");
        form->addRow(gradientEditor);
        form->addRow(new QLabel("Maps luminance through the gradient, including stop opacity."));
    } else if (name == "Black & White") {
        for (const auto &key : QStringList{"reds", "yellows", "greens", "cyans", "blues", "magentas"})
            field(key, key, -200, 300, 100);
    } else if (name == "Channel Mixer") {
        for (const auto &out : QStringList{"red", "green", "blue"})
            for (const auto &in : QStringList{"red", "green", "blue"})
                field(out + " from " + in, out + "_" + in, -200, 200, out == in ? 100 : 0);
    } else if (name == "Selective Color") {
        for (const auto &key : QStringList{"cyan", "magenta", "yellow", "black"})
            field("Neutrals: " + key, key, -100, 100, 0);
    } else if (name == "HDR Toning") {
        field("Exposure", "exposure", -5, 5, 0, .1);
        field("Gamma", "gamma", .1, 5, 1, .05);
        field("Shadows", "shadows", -100, 100, 25);
        field("Highlights", "highlights", -100, 100, 25);
    } else if (name == "Match Color") {
        field("Luminance (%)", "luminance", 0, 200, 100);
        field("Saturation (%)", "saturation", 0, 200, 100);
        params["targetMean"] = QJsonArray{m_foreground.redF(), m_foreground.greenF(), m_foreground.blueF()};
        form->addRow(new QLabel("Match average color to the foreground swatch."));
    } else if (name == "Replace Color") {
        params["source"] = m_foreground.name();
        params["target"] = m_background.name();
        field("Tolerance", "tolerance", .01, 1, .25, .01);
        form->addRow(new QLabel("Replace foreground hue with background hue."));
    } else if (name == "Color Lookup") {
        auto path = QFileDialog::getOpenFileName(this, "Load color lookup", {}, "3D LUT (*.cube)");
        if (path.isEmpty())
            return;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return;
        int cubeSize = 0;
        QJsonArray cube;
        for (auto line : f.readAll().split('\n')) {
            auto s = QString::fromUtf8(line).trimmed();
            if (s.startsWith("LUT_3D_SIZE"))
                cubeSize = s.section(' ', 1).toInt();
            if (s.isEmpty() || s.startsWith('#') || s[0].isLetter())
                continue;
            auto numbers = s.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (numbers.size() == 3)
                cube.append(QJsonArray{numbers[0].toDouble(), numbers[1].toDouble(), numbers[2].toDouble()});
        }
        if (cubeSize < 2 || cube.size() != cubeSize * cubeSize * cubeSize) {
            QMessageBox::warning(this, "Invalid LUT", "The .cube file must contain a complete 3D LUT.");
            return;
        }
        params["cubeSize"] = cubeSize;
        params["cube"] = cube;
    } else {
        field("Amount", "amount", -100, 100, 20);
        field("Saturation", "saturation", -100, 100, 0);
    }
    auto get = [&] {
        QJsonObject result = params;
        for (auto it = values.begin(); it != values.end(); ++it)
            result[it.key()] = it.value()->value();
        if (name == "Channel Mixer")
            for (const auto &out : QStringList{"red", "green", "blue"})
                result[out] = QJsonArray{values[out + "_red"]->value(), values[out + "_green"]->value(),
                                         values[out + "_blue"]->value()};
        if (name == "Selective Color")
            result["neutrals"] = QJsonObject{{"cyan", values["cyan"]->value()},
                                             {"magenta", values["magenta"]->value()},
                                             {"yellow", values["yellow"]->value()},
                                             {"black", values["black"]->value()}};
        if (curves)
            result = curves->parameters();
        return result;
    };
    const auto source = (destructive ? d->layerImage(*d->activeLayer()) : d->composite())
                            .scaled(430, 150, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    auto update = [&] { preview->setPixmap(QPixmap::fromImage(applyAdjustment(source, name, get()))); };
    if (curves)
        connect(curves, &CurvesEditor::parametersChanged, &dialog, update);
    if (gradientEditor)
        connect(gradientEditor, &QPushButton::clicked, &dialog, [&] {
            FillEditorDialog editor(LayerKind::GradientFill, params,
                                    editing ? QColor(Qt::black) : m_foreground,
                                    editing ? QColor(Qt::white) : m_background, &dialog, true);
            editor.setWindowTitle("Gradient Map stops");
            if (editor.exec() == QDialog::Accepted) {
                params = editor.parameters();
                update();
            }
        });
    for (auto *spin : values)
        connect(spin, &QDoubleSpinBox::valueChanged, &dialog, [&](double) { update(); });
    update();
    dialogButtons(dialog, v);
    if (dialog.exec() != QDialog::Accepted)
        return;
    params = get();
    if (destructive) {
        auto actionParams = params;
        actionParams["destructive"] = true;
        QString error;
        if (!ActionRunner::execute(
                d, QJsonObject{{"command", "adjustment"}, {"name", name}, {"parameters", actionParams}},
                &error)) {
            QMessageBox::warning(this, name, error);
            return;
        }
    } else if (editing)
        d->mutate("Edit " + name, [d, params] { d->activeLayer()->parameters = params; });
    else
        d->addAdjustment(name, params);
    auto actionParams = params;
    actionParams["destructive"] = destructive;
    recordStep("adjustment", name, actionParams);
}
void MainWindow::filter(const QString &name, bool repeat) {
    auto *d = currentDocument();
    if (!d || !d->activeLayer())
        return;
    QJsonObject params = m_lastFilterParameters;
    if (!repeat) {
        QDialog dialog(this);
        dialog.setWindowTitle(name);
        auto *v = new QVBoxLayout(&dialog);
        auto *form = new QFormLayout;
        auto *radius = new QDoubleSpinBox;
        radius->setRange(.1, 100);
        radius->setValue(4);
        auto *amount = new QDoubleSpinBox;
        amount->setRange(0, 300);
        amount->setValue(50);
        auto *angle = new QDoubleSpinBox;
        angle->setRange(-360, 360);
        angle->setValue(0);
        auto *size = new QSpinBox;
        size->setRange(2, 200);
        size->setValue(12);
        form->addRow("Radius (px)", radius);
        form->addRow("Amount (%)", amount);
        form->addRow("Angle (°)", angle);
        form->addRow("Cell / kernel size", size);
        v->addLayout(form);
        auto *preview = new QLabel;
        preview->setMinimumSize(350, 150);
        preview->setAlignment(Qt::AlignCenter);
        v->addWidget(preview);
        auto source =
            d->layerImage(*d->activeLayer()).scaled(350, 150, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        auto collect = [&] {
            return QJsonObject{{"radius", radius->value()},
                               {"amount", amount->value()},
                               {"angle", angle->value()},
                               {"size", size->value()}};
        };
        auto update = [&] { preview->setPixmap(QPixmap::fromImage(applyFilter(source, name, collect()))); };
        connect(radius, &QDoubleSpinBox::valueChanged, &dialog, [&](double) { update(); });
        connect(amount, &QDoubleSpinBox::valueChanged, &dialog, [&](double) { update(); });
        connect(angle, &QDoubleSpinBox::valueChanged, &dialog, [&](double) { update(); });
        connect(size, &QSpinBox::valueChanged, &dialog, [&](int) { update(); });
        update();
        dialogButtons(dialog, v);
        if (dialog.exec() != QDialog::Accepted)
            return;
        params = collect();
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    if (d->activeLayer()->kind == LayerKind::SmartObject)
        d->addSmartFilter(name, params);
    else {
        QString error;
        if (!ActionRunner::execute(
                d, QJsonObject{{"command", "filter"}, {"name", name}, {"parameters", params}}, &error))
            showMessage(error);
    }
    QApplication::restoreOverrideCursor();
    m_lastFilter = name;
    m_lastFilterParameters = params;
    recordStep("filter", name, params);
}
void MainWindow::develop(Document *d) {
    if (!d || !d->activeLayer())
        return;
    QImage source = d->layerImage(*d->activeLayer());
    QImage small = source.scaled(760, 560, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QDialog dialog(this);
    dialog.setWindowTitle("Develop — " + d->title);
    dialog.resize(1160, 750);
    auto *outer = new QVBoxLayout(&dialog);
    auto *body = new QHBoxLayout;
    auto *preview = new QLabel;
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumSize(650, 500);
    body->addWidget(preview, 1);
    auto *tabs = new QTabWidget;
    tabs->setFixedWidth(300);
    body->addWidget(tabs);
    outer->addLayout(body, 1);
    QHash<QString, QDoubleSpinBox *> values;
    QJsonObject settings = d->state.metadata["develop"].toObject();
    auto page = [&](QString name) {
        auto *w = new QWidget;
        auto *f = new QFormLayout(w);
        f->setContentsMargins(10, 14, 10, 10);
        tabs->addTab(w, name);
        return f;
    };
    auto field = [&](QFormLayout *form, QString label, QString key, double lo, double hi, double def,
                     double step = 1) {
        auto *s = new QDoubleSpinBox;
        s->setRange(lo, hi);
        s->setValue(settings[key].toDouble(def));
        s->setSingleStep(step);
        s->setDecimals(step < 1 ? 2 : 0);
        form->addRow(label, s);
        values[key] = s;
    };
    auto *basic = page("Basic");
    field(basic, "Temperature", "temperature", 2000, 12000, 6500, 100);
    field(basic, "Tint", "tint", -100, 100, 0);
    field(basic, "Exposure", "exposure", -5, 5, 0, .1);
    field(basic, "Contrast", "contrast", -100, 100, 0);
    field(basic, "Highlights", "highlights", -100, 100, 0);
    field(basic, "Shadows", "shadows", -100, 100, 0);
    field(basic, "Whites", "whites", -100, 100, 0);
    field(basic, "Blacks", "blacks", -100, 100, 0);
    field(basic, "Texture", "texture", -100, 100, 0);
    field(basic, "Clarity", "clarity", -100, 100, 0);
    field(basic, "Dehaze", "dehaze", -100, 100, 0);
    field(basic, "Vibrance", "vibrance", -100, 100, 0);
    field(basic, "Saturation", "saturation", -100, 100, 0);
    auto *tone = page("Curve");
    field(tone, "Gamma", "gamma", .1, 3, 1, .05);
    auto *detail = page("Detail");
    field(detail, "Luminance noise", "luminanceNR", 0, 100, 0);
    field(detail, "Color noise", "colorNR", 0, 100, 0);
    field(detail, "Sharpness", "sharpness", 0, 150, 0);
    field(detail, "Radius", "sharpnessRadius", .1, 5, 1, .1);
    auto *mixer = page("Color");
    field(mixer, "Hue", "hue", -180, 180, 0);
    auto *optics = page("Optics");
    field(optics, "Vignette", "vignette", -100, 100, 0);
    field(optics, "Distortion", "distortion", -100, 100, 0);
    auto *geometry = page("Geometry");
    field(geometry, "Rotate", "angle", -45, 45, 0, .1);
    auto collect = [&] {
        QJsonObject p;
        for (auto it = values.begin(); it != values.end(); ++it)
            p[it.key()] = it.value()->value();
        return p;
    };
    QTimer previewTimer;
    previewTimer.setSingleShot(true);
    previewTimer.setInterval(80);
    auto update = [&] { preview->setPixmap(QPixmap::fromImage(developImage(small, collect(), 8))); };
    connect(&previewTimer, &QTimer::timeout, &dialog, update);
    for (auto *s : values)
        connect(s, &QDoubleSpinBox::valueChanged, &dialog, [&](double) { previewTimer.start(); });
    update();
    auto *output = new QHBoxLayout;
    output->addWidget(new QLabel("Output:"));
    auto *depth = new QComboBox;
    depth->addItems({"16-bit RGB", "8-bit RGB"});
    output->addWidget(depth);
    auto *profile = new QComboBox;
    profile->addItems({"sRGB", "Display P3", "Adobe RGB compatible", "ProPhoto RGB"});
    output->addWidget(profile);
    auto *smart = new QCheckBox("Open as smart object");
    output->addWidget(smart);
    auto *sidecar = new QCheckBox("Write XMP sidecar");
    sidecar->setVisible(d->state.metadata["rawPendingDevelop"].toBool());
    output->addWidget(sidecar);
    output->addStretch();
    outer->addLayout(output);
    auto *buttons = dialogButtons(dialog, outer);
    buttons->button(QDialogButtonBox::Ok)->setText("Open image");
    if (dialog.exec() != QDialog::Accepted) {
        d->state.metadata["developCancelled"] = true;
        return;
    }
    settings = collect();
    QImage result = developImage(source, settings, depth->currentIndex() == 0 ? 16 : 8);
    if (profile->currentIndex() > 0) {
        result.setColorSpace(QColorSpace(QColorSpace::SRgb));
        const auto space = profile->currentIndex() == 1   ? QColorSpace::DisplayP3
                           : profile->currentIndex() == 2 ? QColorSpace::AdobeRgb
                                                          : QColorSpace::ProPhotoRgb;
        result.convertToColorSpace(QColorSpace(space));
    }
    d->mutate("Develop", [&] {
        d->activeLayer()->pixels.setImage(result);
        d->activeLayer()->kind = smart->isChecked() ? LayerKind::SmartObject : LayerKind::Pixel;
        d->state.bitDepth = depth->currentIndex() == 0 ? 16 : 8;
        d->state.iccProfile = result.colorSpace().isValid() ? result.colorSpace().iccProfile()
                                                            : QColorSpace(QColorSpace::SRgb).iccProfile();
        d->state.metadata["develop"] = settings;
        d->state.metadata["rawPendingDevelop"] = false;
        d->state.metadata.remove("developCancelled");
    });
    if (sidecar->isChecked()) {
        QString sourcePath = d->state.metadata["rawSource"].toString();
        QSaveFile f(sourcePath + ".xmp");
        if (f.open(QIODevice::WriteOnly)) {
            QString xml = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
                          "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description "
                          "xmlns:serika=\"https://serika.app/develop/1.0/\"";
            for (auto it = settings.begin(); it != settings.end(); ++it)
                xml += " serika:" + it.key() + "=\"" + QString::number(it.value().toDouble()) + "\"";
            xml += "/></rdf:RDF></x:xmpmeta>";
            f.write(xml.toUtf8());
            if (!f.commit())
                showMessage("Could not write the XMP sidecar.");
        }
    }
}
void MainWindow::transformDialog() {
    auto *c = currentCanvas();
    if (!c)
        return;
    c->beginTransform();
}
void MainWindow::layerStyles() {
    auto *d = currentDocument();
    if (!d || !d->activeLayer())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle("Layer styles");
    dialog.resize(520, 500);
    auto *v = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget;
    v->addWidget(tabs);
    QHash<QString, QCheckBox *> enabled;
    QHash<QString, QSpinBox *> sizes, opacities;
    QHash<QString, QColor> colors;
    auto old = d->activeLayer()->effects;
    const QList<QPair<QString, QString>> effects = {
        {"Drop Shadow", "dropShadow"},         {"Inner Shadow", "innerShadow"},
        {"Outer Glow", "outerGlow"},           {"Inner Glow", "innerGlow"},
        {"Bevel and Emboss", "bevelEmboss"},   {"Satin", "satin"},
        {"Color Overlay", "colorOverlay"},     {"Gradient Overlay", "gradientOverlay"},
        {"Pattern Overlay", "patternOverlay"}, {"Stroke", "stroke"}};
    for (const auto &e : effects) {
        auto *w = new QWidget;
        auto *f = new QFormLayout(w);
        auto p = old[e.second].toObject();
        auto *on = new QCheckBox("Enabled");
        on->setChecked(p["enabled"].toBool(old.contains(e.second)));
        enabled[e.second] = on;
        f->addRow(on);
        auto *size = new QSpinBox;
        size->setRange(1, 200);
        size->setValue(p["size"].toInt(6));
        sizes[e.second] = size;
        f->addRow("Size", size);
        auto *opacity = new QSpinBox;
        opacity->setRange(0, 100);
        opacity->setValue(qRound(p["opacity"].toDouble(.65) * 100));
        opacities[e.second] = opacity;
        f->addRow("Opacity", opacity);
        colors[e.second] = QColor(p["color"].toString(e.second.contains("Glow") ? "#E8893A" : "#000000"));
        auto *color = new QPushButton("Color...");
        f->addRow(color);
        connect(color, &QPushButton::clicked, &dialog, [&, key = e.second] {
            auto c = QColorDialog::getColor(colors[key], &dialog);
            if (c.isValid())
                colors[key] = c;
        });
        tabs->addTab(w, e.first);
    }
    tabs->setTabPosition(QTabWidget::West);
    dialogButtons(dialog, v);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QJsonObject result;
    for (const auto &e : effects)
        if (enabled[e.second]->isChecked())
            result[e.second] = QJsonObject{{"enabled", true},
                                           {"size", sizes[e.second]->value()},
                                           {"opacity", opacities[e.second]->value() / 100.},
                                           {"color", colors[e.second].name()},
                                           {"distance", 8},
                                           {"angle", 120}};
    d->mutate("Layer styles", [d, result] { d->activeLayer()->effects = result; });
}
void MainWindow::preferences(const QString &initialPage) {
    QDialog dialog(this);
    dialog.setWindowTitle("Preferences");
    dialog.resize(740, 550);
    auto *outer = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget;
    tabs->setTabPosition(QTabWidget::West);
    outer->addWidget(tabs, 1);
    auto page = [&](QString name) {
        auto *w = new QWidget;
        auto *f = new QFormLayout(w);
        f->setContentsMargins(20, 20, 20, 15);
        tabs->addTab(w, name);
        return f;
    };
    auto *general = page("General");
    auto *home = new QCheckBox("Show Home at launch");
    home->setChecked(m_settings.value("showHome", true).toBool());
    general->addRow(home);
    auto *recovery = new QCheckBox("Save recovery every 10 minutes");
    recovery->setChecked(m_settings.value("recovery", true).toBool());
    general->addRow(recovery);
    auto *interface = page("Interface");
    auto *theme = new QComboBox;
    theme->addItems({"Darkest", "Dark", "Light", "Lightest"});
    theme->setCurrentText(m_theme);
    interface->addRow("Color theme", theme);
    auto *scale = new QComboBox;
    scale->addItems({"Automatic (per-monitor DPI)", "100%", "150%", "200%"});
    interface->addRow("UI scaling", scale);
    interface->addRow(new QLabel("Qt follows the display's DPI. Restart after changing system scale."));
    auto *taskBar = new QCheckBox("Show the contextual task bar under the canvas");
    taskBar->setChecked(m_settings.value("contextTaskBar", false).toBool());
    interface->addRow(taskBar);
    auto *workspace = page("Workspace");
    auto *reset = new QPushButton("Reset current workspace");
    workspace->addRow(reset);
    connect(reset, &QPushButton::clicked, this, [this] { setWorkspace(m_workspace, true); });
    auto *tools = page("Tools");
    auto *brush = new QSpinBox;
    brush->setRange(1, 5000);
    brush->setValue(m_options->findChild<QSpinBox *>("brushSize")->value());
    tools->addRow("Default brush size", brush);
    auto *history = page("History");
    auto *states = new QSpinBox;
    states->setRange(1, 1000);
    states->setValue(m_settings.value("history", 50).toInt());
    history->addRow("History states", states);
    auto *handling = page("File Handling");
    handling->addRow(new QLabel("Native saves are atomic. PSD import reports skipped blocks."));
    auto *exportPage = page("Export");
    auto *quality = new QSpinBox;
    quality->setRange(1, 100);
    quality->setValue(m_settings.value("jpegQuality", 92).toInt());
    exportPage->addRow("JPEG / WebP quality", quality);
    auto *perf = page("Performance");
    const auto gpuInfo = GpuProcessor::instance().diagnostics();
    auto *gpu = new QCheckBox("Use GPU for supported adjustments and blur");
    gpu->setObjectName("gpuProcessing");
    gpu->setChecked(GpuProcessor::instance().enabled());
    gpu->setEnabled(gpuInfo.available || gpu->isChecked());
    perf->addRow(gpu);
    auto *gpuStatus = new QLabel(gpuInfo.summary());
    gpuStatus->setWordWrap(true);
    gpuStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    perf->addRow(gpuStatus);
    auto *linear = new QCheckBox("Blend in linear light");
    linear->setChecked(m_settings.value("blendLinear", false).toBool());
    perf->addRow(linear);
    auto *memory = new QSpinBox;
    memory->setRange(10, 90);
    memory->setValue(70);
    perf->addRow("Memory preference (%)", memory);
    perf->addRow(new QLabel("CPU raster compositor · sparse 256 × 256 copy-on-write tiles"));
    auto *scratch = new QLineEdit(
        m_settings.value("scratch", QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
            .toString());
    perf->addRow("Scratch folder", scratch);
    auto *cursors = page("Cursors");
    auto *cursor = new QComboBox;
    cursor->addItems({"Brush tip with crosshair", "Precise crosshair"});
    cursors->addRow("Painting cursor", cursor);
    auto *transparency = page("Transparency & Gamut");
    transparency->addRow(new QLabel("8 px checkerboard · gray / light gray"));
    auto *units = page("Units & Rulers");
    auto *unit = new QComboBox;
    unit->addItems({"Pixels", "Inches", "Centimeters"});
    unit->setCurrentText(m_settings.value("units", "Pixels").toString());
    units->addRow("Ruler units", unit);
    auto *guides = page("Guides Grid Slices");
    guides->addRow(new QLabel("Cyan guides · 64 px grid · drag from ruler to add guide"));
    auto *ai = page("AI");
    auto *engine = new QComboBox;
    engine->addItem("Fast: colour model, very low resource use", "fast");
    engine->addItem("AI: BiRefNet on this computer (ONNX Runtime)", "ai");
    engine->setCurrentIndex(preferredSubjectEngine() == SubjectEngine::Ai ? 1 : 0);
    ai->addRow("Select Subject / Remove Background", engine);
    auto *aiStatus = new QLabel;
    aiStatus->setWordWrap(true);
    aiStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ai->addRow("Status", aiStatus);
    auto *modelPath = new QLineEdit(m_settings.value("ai/modelPath").toString());
    modelPath->setPlaceholderText(QDir::toNativeSeparators(AiSegmenter::defaultModelPath()));
    ai->addRow("Model file (.onnx)", modelPath);
    auto *runtimePath = new QLineEdit(m_settings.value("ai/runtimePath").toString());
    runtimePath->setPlaceholderText("Automatic: next to Serika, or set the full library path");
    ai->addRow("ONNX Runtime library", runtimePath);
    const auto refreshAi = [aiStatus, modelPath, runtimePath] {
        AiSegmenter::instance().setModelPath(modelPath->text().trimmed());
        AiSegmenter::instance().setRuntimePath(runtimePath->text().trimmed());
        const auto status = AiSegmenter::instance().status();
        aiStatus->setText((status.ready() ? "✓ " : "") + status.message);
    };
    auto *download = new QPushButton("Download BiRefNet model (" +
                                     QLocale().formattedDataSize(AiSegmenter::defaultModel().bytes) + ")…");
    ai->addRow(download);
    connect(download, &QPushButton::clicked, this, [this, refreshAi] {
        downloadAiModel();
        refreshAi();
    });
    connect(modelPath, &QLineEdit::editingFinished, this, refreshAi);
    connect(runtimePath, &QLineEdit::editingFinished, this, refreshAi);
    auto *allowAgents =
        new QCheckBox("Allow AI agents (MCP --attach) to edit the documents open in this window");
    allowAgents->setChecked(m_settings.value("mcp/allowLive", true).toBool());
    ai->addRow(allowAgents);
    auto *aiNote = new QLabel("Images never leave this computer. Fast works everywhere; AI needs the ONNX "
                              "Runtime library (shipped with Windows builds) and the model file.");
    aiNote->setWordWrap(true);
    ai->addRow(aiNote);
    refreshAi();
    auto *technology = page("Technology Previews");
    auto *technologyInfo =
        new QLabel("GPU image processing is opt-in in Performance. Unsupported operations use the CPU. ICC "
                   "CMYK proofing and export use a printer profile selected in View > Proof Setup.");
    technologyInfo->setWordWrap(true);
    technology->addRow(technologyInfo);
    for (int index = 0; index < tabs->count(); ++index)
        if (tabs->tabText(index) == initialPage)
            tabs->setCurrentIndex(index);
    dialogButtons(dialog, outer);
    const auto restoreAiPaths = [this] {
        AiSegmenter::instance().setModelPath(m_settings.value("ai/modelPath").toString());
        AiSegmenter::instance().setRuntimePath(m_settings.value("ai/runtimePath").toString());
    };
    if (dialog.exec() != QDialog::Accepted) {
        restoreAiPaths();
        return;
    }
    applyTheme(theme->currentText());
    SubjectEngine chosenEngine = SubjectEngine::Fast;
    parseSubjectEngine(engine->currentData().toString(), &chosenEngine);
    setPreferredSubjectEngine(chosenEngine);
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
    m_settings.setValue("scratch", scratch->text());
    m_settings.setValue("units", unit->currentText());
    m_options->findChild<QSpinBox *>("brushSize")->setValue(brush->value());
    for (int i = 0; i < m_tabs->count(); i++) {
        auto *d = m_tabs->widget(i)->findChild<CanvasView *>()->document();
        d->historyLimit = states->value();
        d->blendLinear = linear->isChecked();
        d->touch();
    }
}
void MainWindow::exportAs() {
    auto *d = currentDocument();
    if (!d)
        return;
    QDialog dialog(this);
    dialog.setWindowTitle("Export As");
    dialog.resize(490, 330);
    auto *v = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *format = new QComboBox;
    format->addItems(FormatIO::writableFormats());
    auto *scale = new QDoubleSpinBox;
    scale->setRange(1, 1000);
    scale->setValue(100);
    scale->setSuffix("%");
    auto *quality = new QSpinBox;
    quality->setRange(1, 100);
    quality->setValue(m_settings.value("jpegQuality", 92).toInt());
    auto *strip = new QCheckBox("Strip metadata");
    auto *srgb = new QCheckBox("Convert to sRGB");
    srgb->setChecked(true);
    form->addRow("Format", format);
    form->addRow("Scale", scale);
    form->addRow("Quality", quality);
    form->addRow(strip);
    form->addRow(srgb);
    v->addLayout(form);
    dialogButtons(dialog, v);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString suffix = format->currentText().toLower();
    auto path =
        QFileDialog::getSaveFileName(this, "Export", QFileInfo(d->title).completeBaseName() + "." + suffix,
                                     suffix.toUpper() + " (*." + suffix + ")");
    if (path.isEmpty())
        return;
    auto image = d->composite();
    if (scale->value() != 100)
        image = image.scaled(image.size() * scale->value() / 100., Qt::IgnoreAspectRatio,
                             Qt::SmoothTransformation);
    if (srgb->isChecked()) {
        if (!image.colorSpace().isValid())
            image.setColorSpace(QColorSpace::fromIccProfile(d->state.iccProfile));
        image.convertToColorSpace(QColorSpace(QColorSpace::SRgb));
    }
    if (strip->isChecked())
        for (const auto &key : image.textKeys())
            image.setText(key, QString());
    Document exported;
    exported.state.size = image.size();
    Layer l;
    l.id = 1;
    l.pixels = TileImage::fromImage(image);
    exported.state.layers.append(l);
    exported.state.iccProfile = image.colorSpace().iccProfile();
    QString error;
    if (!FormatIO::save(&exported, path, &error, quality->value()))
        QMessageBox::warning(this, "Export failed", error);
    else
        showMessage("Exported " + QFileInfo(path).fileName());
}
void MainWindow::palette() {
    QDialog dialog(this);
    dialog.setWindowTitle("Command palette");
    dialog.resize(550, 520);
    auto *v = new QVBoxLayout(&dialog);
    auto *search = new QLineEdit;
    search->setPlaceholderText("Find a command or tool...");
    v->addWidget(search);
    auto *list = new QListWidget;
    QStringList names = m_commands.keys();
    names.removeAll("Tool V");
    names.sort();
    for (const auto &name : names)
        if (!name.startsWith("Tool "))
            list->addItem(name);
    for (const auto &tool : toolNames())
        list->addItem("Tool: " + tool);
    v->addWidget(list);
    connect(search, &QLineEdit::textChanged, &dialog, [list](const QString &s) {
        for (int i = 0; i < list->count(); i++)
            list->item(i)->setHidden(!list->item(i)->text().contains(s, Qt::CaseInsensitive));
    });
    connect(list, &QListWidget::itemActivated, &dialog, [&](QListWidgetItem *i) {
        auto name = i->text();
        dialog.accept();
        if (name.startsWith("Tool: "))
            selectTool(name.mid(6));
        else
            runCommand(name);
    });
    search->setFocus();
    dialog.exec();
}
void MainWindow::batchDialog() {
    auto input = QFileDialog::getExistingDirectory(this, "Batch input folder");
    if (input.isEmpty())
        return;
    auto output = QFileDialog::getExistingDirectory(this, "Batch output folder");
    if (output.isEmpty())
        return;
    if (m_recorded.isEmpty()) {
        QMessageBox::information(this, "Batch", "Record or load an action in the Actions panel first.");
        return;
    }
    auto files = QDir(input).entryInfoList(QDir::Files);
    QProgressDialog progress("Processing files...", "Cancel", 0, files.size(), this);
    progress.setWindowModality(Qt::WindowModal);
    int count = 0;
    QStringList errors;
    bool previous = m_recording;
    m_recording = false;
    for (const auto &file : files) {
        progress.setValue(count++);
        qApp->processEvents();
        if (progress.wasCanceled())
            break;
        QString error;
        auto *d = FormatIO::open(file.absoluteFilePath(), &error, this);
        if (!d) {
            errors << file.fileName() + ": " + error;
            continue;
        }
        addDocument(d);
        if (!ActionRunner::run(d, m_actionSteps, &error)) {
            errors << file.fileName() + ": " + error;
            d->markSaved();
            closeDocument(m_tabs->currentIndex());
            continue;
        }
        QString target = output + "/" + file.completeBaseName() + ".spe";
        if (!FormatIO::saveNative(d, target, &error))
            errors << file.fileName() + ": " + error;
        d->markSaved();
        closeDocument(m_tabs->currentIndex());
    }
    m_recording = previous;
    progress.setValue(files.size());
    QMessageBox::information(this, "Batch complete",
                             QString("Processed %1 files.\n%2").arg(count).arg(errors.join("\n")));
}
void MainWindow::renameLayer() {
    auto *d = currentDocument();
    if (!d || !d->activeLayer())
        return;
    bool ok;
    auto name =
        QInputDialog::getText(this, "Rename layer", "Name", QLineEdit::Normal, d->activeLayer()->name, &ok);
    if (ok && !name.isEmpty())
        d->mutate("Rename layer", [d, name] { d->activeLayer()->name = name; });
}
void MainWindow::selectAndMask() {
    auto *d = currentDocument();
    if (!d || !d->activeLayer())
        return;
    const auto source = d->composite();
    const auto initial = d->hasSelection() ? d->state.selection : selectSubjectLocally(source);
    MaskRefineDialog dialog(source, initial, d->state.bitDepth, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto mask = dialog.refinedMask();
    const auto mode = dialog.outputMode();
    d->mutate("Select and Mask", [&] {
        if (mode == MaskRefineDialog::Output::Selection)
            d->setSelection(mask);
        else if (mode == MaskRefineDialog::Output::LayerMask) {
            auto *l = d->activeLayer();
            const auto origin = d->effectiveLayerOffset(*l).toPoint();
            const auto extent = d->layerImage(*l).size();
            l->mask = makeMask(extent, d->state.bitDepth);
            for (int y = 0; y < extent.height(); ++y)
                for (int x = 0; x < extent.width(); ++x)
                    setMaskSample(l->mask, x, y, maskSample(mask, x + origin.x(), y + origin.y()));
            l->maskEnabled = true;
            l->maskTarget = true;
            l->maskDensity = 1;
            l->maskFeather = 0;
            l->maskOffset = {};
            l->maskLinked = true;
        } else {
            d->addLayer("Refined selection");
            auto *l = d->activeLayer();
            l->parentId = 0;
            l->pixels.setImage(dialog.outputImage());
            if (mode == MaskRefineDialog::Output::NewLayerWithMask) {
                l->mask = mask;
                l->maskTarget = true;
            }
        }
    });
}

} // namespace serika
