#include "AiSegmenter.h"
#include "OnnxRuntime.h"
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <vector>

namespace serika {
AiSegmenter &AiSegmenter::instance() {
    static AiSegmenter segmenter;
    return segmenter;
}
AiSegmenter::AiSegmenter() : m_model(std::make_unique<OnnxModel>()) {}
AiSegmenter::~AiSegmenter() = default;
AiModelInfo AiSegmenter::defaultModel() {
    return {"birefnet-lite",
            "BiRefNet lite",
            "MIT",
            "https://huggingface.co/onnx-community/BiRefNet_lite-ONNX/resolve/main/onnx/model.onnx",
            "5600024376f572a557870a5eb0afb1e5961636bef4e1e22132025467d0f03333",
            224005088};
}
QString AiSegmenter::defaultModelPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/models/" +
           defaultModel().id + "/model.onnx";
}
void AiSegmenter::setRuntimePath(const QString &path) { OnnxRuntime::instance().setPath(path); }
void AiSegmenter::setModelPath(const QString &path) {
    QMutexLocker lock(&m_mutex);
    m_modelOverride = path;
    m_model->release();
}
QString AiSegmenter::runtimePath() const { return OnnxRuntime::instance().path(); }
QString AiSegmenter::modelPath() const {
    if (!m_modelOverride.isEmpty())
        return m_modelOverride;
    if (const auto env = qEnvironmentVariable("SERIKA_AI_MODEL"); !env.isEmpty())
        return env;
    if (const auto saved = QSettings().value("ai/modelPath").toString(); !saved.isEmpty())
        return saved;
    return defaultModelPath();
}
AiSegmenter::Status AiSegmenter::status() {
    QMutexLocker lock(&m_mutex);
    auto &runtime = OnnxRuntime::instance();
    Status status;
    status.runtimePath = runtime.loaded() ? runtime.loadedPath() : runtime.path();
    status.modelPath = modelPath();
    QString error;
    status.runtimeAvailable = runtime.load(&error);
    if (status.runtimeAvailable)
        status.runtimeVersion = runtime.version();
    status.modelAvailable = QFileInfo(status.modelPath).isFile();
    if (!status.runtimeAvailable)
        status.message = error;
    else if (!status.modelAvailable)
        status.message =
            "ONNX Runtime " + status.runtimeVersion + " is ready; the AI model is not installed.";
    else
        status.message = "Ready: ONNX Runtime " + status.runtimeVersion + " (" +
                         (runtime.directMlAvailable() ? "DirectML GPU" : "CPU, roughly 5–30 s per image") +
                         ") with " + QFileInfo(status.modelPath).dir().dirName() + ".";
    return status;
}
QImage AiSegmenter::segment(const QImage &image, QString *error) {
    QMutexLocker lock(&m_mutex);
    if (image.isNull()) {
        if (error)
            *error = "There is no image to segment.";
        return {};
    }
    if (!m_model->load(modelPath(), error))
        return {};
    // BiRefNet declares a fixed 1024x1024 input; fall back to that for dynamic exports.
    const QSize size = m_model->inputSize().isValid() ? m_model->inputSize() : QSize(1024, 1024);
    const int w = size.width(), h = size.height();
    // ImageNet normalisation on a square resize, as BiRefNet was trained.
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888)
                           .scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    std::vector<float> input(size_t(3) * w * h), output;
    constexpr float mean[3]{0.485f, 0.456f, 0.406f}, deviation[3]{0.229f, 0.224f, 0.225f};
    for (int y = 0; y < h; ++y) {
        const uchar *row = rgb.constScanLine(y);
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                input[size_t(c) * w * h + size_t(y) * w + x] =
                    (row[x * 3 + c] / 255.0f - mean[c]) / deviation[c];
    }
    std::vector<int64_t> shape;
    if (!m_model->run(input, {1, 3, h, w}, &output, &shape, error))
        return {};
    const int oh = shape.size() >= 2 ? int(shape[shape.size() - 2]) : 0,
              ow = shape.empty() ? 0 : int(shape.back());
    const size_t pixels = size_t(std::max(0, ow)) * size_t(std::max(0, oh));
    if (pixels == 0 || output.size() < pixels) {
        if (error)
            *error = "The AI model returned an unexpected output shape.";
        return {};
    }
    // BiRefNet emits logits; apply a sigmoid unless the model already produced probabilities.
    const auto [low, high] = std::minmax_element(output.begin(), output.begin() + pixels);
    const bool logits = *low < 0.0f || *high > 1.0f;
    QImage small(ow, oh, QImage::Format_Grayscale16);
    for (int y = 0; y < oh; ++y) {
        auto *row = reinterpret_cast<quint16 *>(small.scanLine(y));
        for (int x = 0; x < ow; ++x) {
            float v = output[size_t(y) * ow + x];
            if (logits)
                v = 1.0f / (1.0f + std::exp(-v));
            row[x] = quint16(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f));
        }
    }
    return small.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}
} // namespace serika
