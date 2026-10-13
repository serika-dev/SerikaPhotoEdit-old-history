#include "MainWindow.h"
#include "tools/Upscaler.h"
#include "ui/SettingsWidgets.h"
#include "ui/canvas/CanvasView.h"
#include <QBoxLayout>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSlider>
#include <QThread>
#include <QTimer>
#include <atomic>

namespace serika {
namespace {
QString sizeText(QSize size) { return QString("%1 × %2 px").arg(size.width()).arg(size.height()); }
QString durationText(double seconds) {
    if (seconds < 1.5)
        return "about a second";
    if (seconds < 90)
        return QString("about %1 s").arg(qRound(seconds));
    return QString("about %1 min").arg(qRound(seconds / 60));
}
} // namespace

void MainWindow::superResolution(bool layerOnly) {
    auto *document = currentDocument();
    if (!document)
        return;
    const Layer *layer = document->activeLayer();
    if (layerOnly && (!layer || layer->kind != LayerKind::Pixel || layer->pixels.empty() || layer->locked)) {
        showMessage(
            "Select an unlocked pixel layer to upscale. Rasterize text, shapes and smart objects first.");
        return;
    }
    const quint64 layerId = layerOnly ? layer->id : 0;
    const QSize sourceSize = layerOnly ? layer->pixels.size : document->state.size;
    const QString title = layerOnly ? "Upscale Layer" : "Super Resolution";

    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.setObjectName("superResolution");
    dialog.resize(620, 0);
    auto *v = new QVBoxLayout(&dialog);
    v->setContentsMargins(22, 20, 22, 16);
    v->setSpacing(12);
    auto *heading = new QLabel(title);
    heading->setObjectName("prefsTitle");
    auto *intro =
        new QLabel(layerOnly ? "Enlarges “" + layer->name +
                                   "” about its centre with more detail than "
                                   "Free Transform."
                             : "Enlarges the whole document. Pixel layers are resampled; text, shapes and "
                               "smart objects re-render sharply at the new size.");
    intro->setObjectName("prefsSubtitle");
    intro->setWordWrap(true);
    v->addWidget(heading);
    v->addWidget(intro);

    // Scale
    auto *scaleRow = new QHBoxLayout;
    scaleRow->setSpacing(6);
    auto *scaleLabel = new QLabel("Scale");
    scaleLabel->setObjectName("prefsLabel");
    scaleLabel->setMinimumWidth(70);
    scaleRow->addWidget(scaleLabel);
    auto *factor = new QDoubleSpinBox;
    factor->setRange(1.0, 8.0);
    factor->setSingleStep(0.5);
    factor->setDecimals(2);
    factor->setSuffix(" ×");
    factor->setValue(2.0);
    for (const double preset : {2.0, 3.0, 4.0}) {
        auto *button = new QPushButton(QString::number(preset) + "×");
        button->setCheckable(true);
        button->setMinimumWidth(48);
        connect(button, &QPushButton::clicked, factor, [factor, preset] { factor->setValue(preset); });
        connect(factor, &QDoubleSpinBox::valueChanged, button,
                [button, preset](double value) { button->setChecked(qFuzzyCompare(value, preset)); });
        button->setChecked(qFuzzyCompare(factor->value(), preset));
        scaleRow->addWidget(button);
    }
    scaleRow->addWidget(factor);
    scaleRow->addStretch(1);
    v->addLayout(scaleRow);
    auto *dimensions = new QLabel;
    dimensions->setObjectName("prefsHint");
    v->addWidget(dimensions);

    // Method
    auto *methodLabel = new QLabel("Method");
    methodLabel->setObjectName("prefsLabel");
    v->addWidget(methodLabel);
    const bool aiPreferred = m_settings.value("upscale/method", "ai").toString() == "ai";
    Upscaler::Method method = aiPreferred ? Upscaler::Method::Ai : Upscaler::Method::PreserveDetails;
    auto *methods = new QHBoxLayout;
    methods->setSpacing(10);
    std::vector<std::pair<Upscaler::Method, ChoiceTile *>> tiles;
    std::function<void()> refresh;
    const auto addTile = [&](Upscaler::Method id, const QString &name, const QString &text) {
        auto *tile = new ChoiceTile;
        tile->setMinimumHeight(92);
        auto *t = new QVBoxLayout(tile);
        t->setContentsMargins(12, 10, 12, 10);
        t->setSpacing(3);
        auto *label = new QLabel(name);
        label->setObjectName("prefsLabel");
        auto *hint = new QLabel(text);
        hint->setObjectName("prefsHint");
        hint->setWordWrap(true);
        t->addWidget(label);
        t->addWidget(hint, 1);
        tile->clicked = [&, id] {
            method = id;
            refresh();
        };
        tiles.emplace_back(id, tile);
        methods->addWidget(tile, 1);
    };
    addTile(
        Upscaler::Method::Ai, "AI Super Resolution",
        "Real-ESRGAN on this computer. Rebuilds edges and texture; best for low-resolution art and photos.");
    addTile(Upscaler::Method::PreserveDetails, "Preserve Details",
            "Lanczos resampling with edge sharpening. Instant, no model needed.");
    v->addLayout(methods);

    // AI model / detail
    auto *options = new QHBoxLayout;
    options->setSpacing(10);
    auto *modelLabel = new QLabel("Model");
    modelLabel->setMinimumWidth(70);
    auto *model = new QComboBox;
    for (const auto &info : Upscaler::models())
        model->addItem(info.name, info.id);
    model->setCurrentIndex(std::max(0, model->findData(Upscaler::preferredModelId())));
    auto *download = new QPushButton("Download…");
    auto *detailLabel = new QLabel("Detail");
    detailLabel->setMinimumWidth(70);
    auto *detail = new QSlider(Qt::Horizontal);
    detail->setRange(0, 100);
    detail->setValue(m_settings.value("upscale/detail", 50).toInt());
    auto *detailValue = new QLabel;
    detailValue->setMinimumWidth(36);
    options->addWidget(modelLabel);
    options->addWidget(model, 1);
    options->addWidget(download);
    options->addWidget(detailLabel);
    options->addWidget(detail, 1);
    options->addWidget(detailValue);
    v->addLayout(options);
    auto *estimate = new QLabel;
    estimate->setObjectName("prefsHint");
    estimate->setWordWrap(true);
    v->addWidget(estimate);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto *ok = buttons->button(QDialogButtonBox::Ok);
    ok->setObjectName("primary");
    ok->setText(layerOnly ? "Upscale" : "Enlarge");
    v->addSpacing(4);
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    const auto targetFor = [&](QSize size) {
        return QSize(std::max(1, qRound(size.width() * factor->value())),
                     std::max(1, qRound(size.height() * factor->value())));
    };
    const auto jobs = [&] {
        QHash<quint64, Upscaler::Job> work;
        if (layerOnly)
            work.insert(layerId, {layer->pixels.image(), targetFor(sourceSize)});
        else
            work = Upscaler::documentJobs(document, targetFor(sourceSize));
        return work;
    };
    QVector<QSize> layerSizes; // what the estimate is based on
    if (layerOnly)
        layerSizes.append(sourceSize);
    else
        for (const auto &l : document->state.layers)
            if (l.kind == LayerKind::Pixel && !l.pixels.empty())
                layerSizes.append(l.pixels.size);
    refresh = [&] {
        for (auto &[id, tile] : tiles)
            tile->setChecked(id == method);
        const bool ai = method == Upscaler::Method::Ai;
        const QString modelId = model->currentData().toString();
        const bool installed = Upscaler::installed(modelId);
        for (QWidget *w : std::initializer_list<QWidget *>{modelLabel, model})
            w->setVisible(ai);
        download->setVisible(ai && !installed);
        for (QWidget *w : std::initializer_list<QWidget *>{detailLabel, detail, detailValue})
            w->setVisible(!ai);
        detailValue->setText(QString::number(detail->value()) + " %");
        const QSize target = targetFor(sourceSize);
        dimensions->setText(sizeText(sourceSize) + "  →  " + sizeText(target));
        double seconds = 0;
        const double perTile = Upscaler::model(modelId).secondsPerTile;
        for (const QSize pixels : layerSizes)
            seconds +=
                ai ? Upscaler::tileCount(pixels) * perTile
                   : double(pixels.width()) * pixels.height() * factor->value() * factor->value() / 8e6;
        const bool tooLarge = qint64(target.width()) * target.height() > 80000000;
        estimate->setText(tooLarge ? "The result would exceed 80 million pixels; choose a smaller scale."
                          : ai && !installed
                              ? "Download the model first (" +
                                    QLocale().formattedDataSize(Upscaler::model(modelId).bytes) +
                                    "), or choose Preserve Details."
                              : "Takes " + durationText(seconds) + (ai ? " on this computer's CPU." : "."));
        ok->setEnabled(!tooLarge && (!ai || installed));
    };
    connect(factor, &QDoubleSpinBox::valueChanged, &dialog, [&] { refresh(); });
    connect(detail, &QSlider::valueChanged, &dialog, [&] { refresh(); });
    connect(model, &QComboBox::currentIndexChanged, &dialog, [&] { refresh(); });
    connect(download, &QPushButton::clicked, &dialog, [&] {
        downloadUpscaleModel(model->currentData().toString());
        refresh();
    });
    refresh();
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_settings.setValue("upscale/method", Upscaler::methodId(method));
    m_settings.setValue("upscale/detail", detail->value());
    const QString modelId = model->currentData().toString();
    const double detailAmount = detail->value() / 100.0;
    const QHash<quint64, Upscaler::Job> work = jobs();
    const QSize canvasTarget = targetFor(sourceSize);

    // The model can take minutes; it runs on a worker while a cancellable progress dialog shows.
    QProgressDialog progress(method == Upscaler::Method::Ai ? "Enhancing detail with the local AI model…"
                                                            : "Resampling…",
                             "Cancel", 0, 1000, this);
    progress.setWindowTitle(title);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    std::atomic<int> permille{0};
    std::atomic<bool> cancelled{false};
    QHash<quint64, QImage> results;
    QString error;
    QEventLoop loop;
    QThread *worker = QThread::create([&] {
        results = Upscaler::upscaleBatch(
            work, method, modelId, detailAmount,
            [&](double fraction) {
                permille = int(fraction * 1000);
                return !cancelled.load();
            },
            &error);
    });
    connect(worker, &QThread::finished, &loop, &QEventLoop::quit);
    QTimer ticker;
    connect(&ticker, &QTimer::timeout, &progress, [&] { progress.setValue(permille.load()); });
    connect(&progress, &QProgressDialog::canceled, &dialog, [&] { cancelled = true; });
    ticker.start(100);
    worker->start();
    loop.exec();
    worker->wait();
    delete worker;
    ticker.stop();
    progress.close();
    if (results.isEmpty()) {
        if (!cancelled)
            QMessageBox::warning(this, title, error.isEmpty() ? "Nothing could be upscaled." : error);
        return;
    }
    if (layerOnly) {
        if (!Upscaler::applyLayer(document, layerId, results.value(layerId), &error)) {
            showMessage(error);
            return;
        }
    } else {
        document->resizeImage(canvasTarget, Qt::SmoothTransformation, results, title);
    }
    showMessage(QString("%1 · %2").arg(title, method == Upscaler::Method::Ai ? Upscaler::model(modelId).name
                                                                             : "Preserve Details"));
}
} // namespace serika
