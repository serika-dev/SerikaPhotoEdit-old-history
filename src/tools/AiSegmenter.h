#pragma once
#include <QImage>
#include <QMutex>
#include <QString>
#include <memory>

namespace serika {
class OnnxModel;
// A downloadable, open-licensed segmentation model. The default is BiRefNet_lite (MIT).
struct AiModelInfo {
    QString id, name, license, url, sha256;
    qint64 bytes = 0;
};

// Optional local AI subject segmentation through ONNX Runtime, loaded at runtime with QLibrary.
// Serika neither links nor requires ONNX Runtime: without the runtime library or the model file,
// status() explains what is missing and segment() fails cleanly so callers can use the fast
// colour-model engine instead. Nothing here touches the network.
class AiSegmenter {
  public:
    struct Status {
        bool runtimeAvailable = false;
        bool modelAvailable = false;
        QString runtimePath, modelPath, runtimeVersion, message;
        bool ready() const { return runtimeAvailable && modelAvailable; }
    };
    static AiSegmenter &instance();
    static AiModelInfo defaultModel();
    // Where an in-app download stores the default model.
    static QString defaultModelPath();
    // Overrides used by tests, settings and the SERIKA_ONNXRUNTIME / SERIKA_AI_MODEL variables.
    void setRuntimePath(const QString &path);
    void setModelPath(const QString &path);
    QString runtimePath() const;
    QString modelPath() const;
    Status status();
    // Returns a Grayscale16 coverage mask the size of `image` (1 = subject), or a null image.
    QImage segment(const QImage &image, QString *error = nullptr);
    ~AiSegmenter();

  private:
    AiSegmenter();
    std::unique_ptr<OnnxModel> m_model;
    QString m_modelOverride;
    mutable QMutex m_mutex;
};
} // namespace serika
