#pragma once
#include <QHash>
#include <QImage>
#include <QString>
#include <QVector>
#include <functional>

namespace serika {
class Document;
// A downloadable super-resolution model: an ONNX graph with external weights, shipped as a zip.
struct UpscaleModelInfo {
    QString id, name, summary, license, url, sha256, onnxFile;
    qint64 bytes = 0;
    int scale = 4;
    // Rough CPU cost of one 128 x 128 tile, for time estimates.
    double secondsPerTile = 0.2;
};

// Image enlargement in two flavours, like Photoshop's Image Size resampling and Super Resolution:
//  - Preserve Details: Lanczos-3 resampling in premultiplied light with an adaptive unsharp pass.
//    Instant, no downloads.
//  - AI: Real-ESRGAN through ONNX Runtime, run on overlapping 128 px tiles so any size works.
//    Invents plausible detail; best for low-resolution art and photos.
class Upscaler {
  public:
    enum class Method { PreserveDetails, Ai };
    // Reports progress 0..1; returning false cancels.
    using Progress = std::function<bool(double)>;

    static QVector<UpscaleModelInfo> models();
    static UpscaleModelInfo model(const QString &id);
    static QString defaultModelId();
    // The ai/upscaleModel setting when it names a known model, else the default.
    static QString preferredModelId();
    static void setPreferredModelId(const QString &id);
    // AppData/models/<id>/<onnxFile>; SERIKA_UPSCALE_MODEL overrides the path for every model.
    static QString modelPath(const QString &id);
    static bool installed(const QString &id);
    // Extracts a downloaded release zip (its .onnx and .data files) into the model folder.
    static bool installArchive(const QString &zipPath, const QString &id, QString *error);
    static QString methodId(Method method);
    static bool parseMethod(const QString &id, Method *method);
    // Number of model runs an AI upscale of `size` needs.
    static int tileCount(QSize size);

    // Lanczos-3 resize to `target`. `detail` 0..1 sharpens what enlargement softens.
    static QImage preserveDetails(const QImage &image, QSize target, double detail = 0.5);
    // AI enlargement to `target` (the model works at 4x; other sizes are resampled from that).
    static QImage aiUpscale(const QImage &image, QSize target, const QString &modelId,
                            const Progress &progress, QString *error);
    static QImage upscale(const QImage &image, QSize target, Method method, const QString &modelId,
                          double detail, const Progress &progress, QString *error);

    // Work for several layers at once: layer id -> (pixels, target size). Thread-safe; progress
    // covers the whole batch. Returns an empty hash on failure or cancellation.
    struct Job {
        QImage image;
        QSize target;
    };
    static QHash<quint64, QImage> upscaleBatch(const QHash<quint64, Job> &jobs, Method method,
                                               const QString &modelId, double detail,
                                               const Progress &progress, QString *error);
    // The pixel layers Image > Super Resolution resamples for a new canvas size (GUI thread).
    static QHash<quint64, Job> documentJobs(const Document *document, QSize canvas);
    // Replaces a pixel layer with `pixels`, scaled about its centre, as one undo step.
    static bool applyLayer(Document *document, quint64 layerId, const QImage &pixels, QString *error);
    // A rough duration for the status line, in seconds.
    static double estimateSeconds(const QHash<quint64, Job> &jobs, Method method, const QString &modelId);
};
} // namespace serika
