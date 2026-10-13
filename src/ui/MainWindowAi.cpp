#include "MainWindow.h"
#include "tools/AiSegmenter.h"
#include "tools/LocalAlgorithms.h"
#include "tools/Upscaler.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProgressDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QThread>

namespace serika {
QImage MainWindow::computeSubject(const QImage &composite, SubjectEngine engine, QString *error) {
    if (engine == SubjectEngine::Fast)
        return selectSubject(composite, engine, error);
    // The AI model takes a few seconds on the CPU; keep the window responsive meanwhile.
    QProgressDialog progress("Finding the subject with the local AI model…", {}, 0, 0, this);
    progress.setWindowTitle("Serika AI");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(250);
    progress.setCancelButton(nullptr);
    QImage result;
    QString failure;
    QEventLoop loop;
    QThread *worker = QThread::create([&] { result = selectSubject(composite, engine, &failure); });
    connect(worker, &QThread::finished, &loop, &QEventLoop::quit);
    worker->start();
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    worker->wait();
    delete worker;
    if (error)
        *error = failure;
    return result;
}
bool MainWindow::runSubjectCommand(const QString &name) {
    static const QStringList commands{
        "Subject",           "Subject (Fast)",           "Subject (AI)",
        "Remove Background", "Remove Background (Fast)", "Remove Background (AI)"};
    if (!commands.contains(name))
        return false;
    auto *document = currentDocument();
    auto *canvas = currentCanvas();
    if (!document || !canvas)
        return true;
    const bool remove = name.startsWith("Remove Background");
    SubjectEngine engine = name.endsWith("(AI)")     ? SubjectEngine::Ai
                           : name.endsWith("(Fast)") ? SubjectEngine::Fast
                                                     : preferredSubjectEngine();
    if (remove && (!document->activeLayer() || document->activeLayer()->locked)) {
        showMessage("Select an unlocked layer to remove its background.");
        return true;
    }
    QString error;
    const int layerIndex = document->state.activeIndex;
    const QImage source = remove ? layerSubjectSource(document, layerIndex) : document->composite();
    QImage mask = computeSubject(source, engine, &error);
    if (mask.isNull() && engine == SubjectEngine::Ai) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Information);
        box.setWindowTitle("AI background removal");
        box.setText("The local AI engine is not ready.");
        box.setInformativeText(error);
        auto *fast = box.addButton("Use Fast engine", QMessageBox::AcceptRole);
        auto *settings = box.addButton("AI settings…", QMessageBox::ActionRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == settings) {
            preferences("AI");
            return true;
        }
        if (box.clickedButton() != fast)
            return true;
        engine = SubjectEngine::Fast;
        mask = computeSubject(source, engine, &error);
    }
    if (mask.isNull()) {
        showMessage(error.isEmpty() ? "No subject was found." : error);
        return true;
    }
    if (remove && !applyBackgroundMask(document, layerIndex, mask, &error)) {
        showMessage(error);
        return true;
    }
    if (!remove)
        canvas->setSubjectSelection(mask);
    showMessage(QString("%1 · %2 engine")
                    .arg(remove ? "Background removed" : "Subject selected",
                         engine == SubjectEngine::Ai ? "AI" : "Fast"));
    return true;
}
bool MainWindow::downloadVerified(const QString &title, const QString &label, const QString &url,
                                  const QString &sha256, qint64 bytes, const QString &target) {
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        QMessageBox::warning(this, title, "Could not create the model folder.");
        return false;
    }
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, title, "Could not write " + QDir::toNativeSeparators(target) + ".");
        return false;
    }
    QNetworkAccessManager network;
    QNetworkRequest request{QUrl(url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      "SerikaPhotoEdit/" + QCoreApplication::applicationVersion());
    QNetworkReply *reply = network.get(request);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QProgressDialog progress("Downloading " + label + "…", "Cancel", 0, 1000, this);
    progress.setWindowTitle(title);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    bool writeFailed = false;
    connect(reply, &QNetworkReply::readyRead, this, [&] {
        const QByteArray chunk = reply->readAll();
        hash.addData(chunk);
        if (file.write(chunk) != chunk.size())
            writeFailed = true;
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [&](qint64 received, qint64 total) {
        progress.setValue(int(1000 * received / std::max<qint64>(1, total > 0 ? total : bytes)));
    });
    connect(&progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QByteArray tail = reply->readAll();
    hash.addData(tail);
    file.write(tail);
    const bool cancelled = reply->error() == QNetworkReply::OperationCanceledError;
    const QString networkError = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
    reply->deleteLater();
    progress.close();
    if (cancelled) {
        file.cancelWriting();
        return false;
    }
    // The published SHA-256 guards against truncated or substituted downloads.
    const QString digest = QString::fromLatin1(hash.result().toHex());
    if (!networkError.isEmpty() || writeFailed || digest != sha256) {
        file.cancelWriting();
        QMessageBox::warning(this, title,
                             !networkError.isEmpty() ? "The download failed: " + networkError
                             : writeFailed           ? "Writing the downloaded file failed."
                                                     : "The downloaded file did not match the published "
                                                       "checksum and was discarded.");
        return false;
    }
    if (!file.commit()) {
        QMessageBox::warning(this, title, "Could not save the downloaded file.");
        return false;
    }
    return true;
}
bool MainWindow::downloadAiModel() {
    const auto model = AiSegmenter::defaultModel();
    const QString target = AiSegmenter::defaultModelPath();
    const auto answer = QMessageBox::question(
        this, "Download AI model",
        QString("Download %1 (%2 licence, %3) from Hugging Face?\n\n%4\n\nIt is saved to %5 and only used "
                "on this computer.")
            .arg(model.name, model.license, QLocale().formattedDataSize(model.bytes), model.url,
                 QDir::toNativeSeparators(target)));
    if (answer != QMessageBox::Yes ||
        !downloadVerified("Download AI model", model.name, model.url, model.sha256, model.bytes, target))
        return false;
    AiSegmenter::instance().setModelPath({});
    showMessage(model.name + " installed");
    return true;
}
bool MainWindow::downloadUpscaleModel(const QString &id) {
    const auto model = Upscaler::model(id);
    if (model.id.isEmpty())
        return false;
    const QString folder = QFileInfo(Upscaler::modelPath(id)).absolutePath();
    const auto answer = QMessageBox::question(
        this, "Download upscale model",
        QString(
            "Download %1 (%2 licence, %3) from Qualcomm AI Hub?\n\n%4\n\nIt is saved to %5 and only used on "
            "this computer.")
            .arg(model.name, model.license, QLocale().formattedDataSize(model.bytes), model.url,
                 QDir::toNativeSeparators(folder)));
    const QString archive = folder + ".zip";
    if (answer != QMessageBox::Yes || !downloadVerified("Download upscale model", model.name, model.url,
                                                        model.sha256, model.bytes, archive))
        return false;
    QString error;
    const bool installed = Upscaler::installArchive(archive, id, &error);
    QFile::remove(archive);
    if (!installed) {
        QMessageBox::warning(this, "Download upscale model", error);
        return false;
    }
    showMessage(model.name + " installed");
    return true;
}
} // namespace serika
