#include "AiSegmenter.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QMutexLocker>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
// MinGW ships only part of MSVC's SAL annotations; they are documentation-only, so blank the rest.
#if defined(__MINGW32__)
#ifndef _Check_return_
#define _Check_return_
#endif
#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#ifndef _In_
#define _In_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _In_opt_z_
#define _In_opt_z_
#endif
#ifndef _In_reads_
#define _In_reads_(x)
#endif
#ifndef _In_z_
#define _In_z_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _Inout_opt_
#define _Inout_opt_
#endif
#ifndef _Inout_updates_
#define _Inout_updates_(x)
#endif
#ifndef _Inout_updates_all_
#define _Inout_updates_all_(x)
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#ifndef _Out_writes_
#define _Out_writes_(x)
#endif
#ifndef _Out_writes_all_
#define _Out_writes_all_(x)
#endif
#ifndef _Out_writes_bytes_all_
#define _Out_writes_bytes_all_(x)
#endif
#ifndef _Outptr_
#define _Outptr_
#endif
#ifndef _Outptr_result_buffer_maybenull_
#define _Outptr_result_buffer_maybenull_(x)
#endif
#ifndef _Outptr_result_maybenull_
#define _Outptr_result_maybenull_
#endif
#ifndef _Ret_maybenull_
#define _Ret_maybenull_
#endif
#ifndef _Ret_notnull_
#define _Ret_notnull_
#endif
#ifndef _Return_type_success_
#define _Return_type_success_(x)
#endif
#ifndef _Success_
#define _Success_(x)
#endif
#endif
#include <onnxruntime_c_api.h>
#include <vector>

namespace serika {
struct AiSegmenter::Runtime {
    QLibrary library;
    const OrtApi *api = nullptr;
    OrtEnv *env = nullptr;
    OrtSessionOptions *options = nullptr;
    OrtSession *session = nullptr;
    std::string inputName, outputName;
    int inputWidth = 1024, inputHeight = 1024;
    QString version, path, device = "CPU";
    void releaseSession() {
        if (session)
            api->ReleaseSession(session);
        if (options)
            api->ReleaseSessionOptions(options);
        session = nullptr;
        options = nullptr;
    }
    ~Runtime() {
        if (!api)
            return;
        releaseSession();
        if (env)
            api->ReleaseEnv(env);
    }
};
namespace {
#ifdef Q_OS_WIN
const QString RuntimeFile = "onnxruntime.dll";
#elif defined(Q_OS_MACOS)
const QString RuntimeFile = "libonnxruntime.dylib";
#else
const QString RuntimeFile = "libonnxruntime.so";
#endif
// Converts an OrtStatus into an error message and releases it. Returns true on success.
bool check(const OrtApi *api, OrtStatus *status, QString *error, const QString &what) {
    if (!status)
        return true;
    if (error)
        *error = what + ": " + QString::fromUtf8(api->GetErrorMessage(status));
    api->ReleaseStatus(status);
    return false;
}
} // namespace

AiSegmenter &AiSegmenter::instance() {
    static AiSegmenter segmenter;
    return segmenter;
}
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
void AiSegmenter::setRuntimePath(const QString &path) {
    QMutexLocker lock(&m_mutex);
    m_runtimeOverride = path;
    m_runtime.reset();
}
void AiSegmenter::setModelPath(const QString &path) {
    QMutexLocker lock(&m_mutex);
    m_modelOverride = path;
    if (m_runtime)
        m_runtime->releaseSession();
    m_loadedModel.clear();
}
QString AiSegmenter::runtimePath() const {
    if (!m_runtimeOverride.isEmpty())
        return m_runtimeOverride;
    if (const auto env = qEnvironmentVariable("SERIKA_ONNXRUNTIME"); !env.isEmpty())
        return env;
    if (const auto saved = QSettings().value("ai/runtimePath").toString(); !saved.isEmpty())
        return saved;
    // An explicit path avoids picking up an unrelated, older onnxruntime from the system search path.
    const QStringList candidates{QCoreApplication::applicationDirPath() + "/" + RuntimeFile,
                                 QCoreApplication::applicationDirPath() + "/ai/" + RuntimeFile,
                                 QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                     "/runtime/" + RuntimeFile};
    for (const auto &candidate : candidates)
        if (QFileInfo(candidate).isFile())
            return candidate;
    return candidates.first();
}
QString AiSegmenter::modelPath() const {
    if (!m_modelOverride.isEmpty())
        return m_modelOverride;
    if (const auto env = qEnvironmentVariable("SERIKA_AI_MODEL"); !env.isEmpty())
        return env;
    if (const auto saved = QSettings().value("ai/modelPath").toString(); !saved.isEmpty())
        return saved;
    return defaultModelPath();
}
bool AiSegmenter::loadRuntime(QString *error) {
    if (m_runtime && m_runtime->api)
        return true;
    auto runtime = std::make_unique<Runtime>();
    runtime->path = runtimePath();
    if (!QFileInfo(runtime->path).isFile()) {
        if (error)
            *error = "ONNX Runtime was not found at " + QDir::toNativeSeparators(runtime->path) + ".";
        return false;
    }
    runtime->library.setFileName(runtime->path);
    if (!runtime->library.load()) {
        if (error)
            *error = "Could not load ONNX Runtime: " + runtime->library.errorString();
        return false;
    }
    using GetApiBase = const OrtApiBase *(ORT_API_CALL *)();
    auto getApiBase = reinterpret_cast<GetApiBase>(runtime->library.resolve("OrtGetApiBase"));
    if (!getApiBase) {
        if (error)
            *error = "The ONNX Runtime library does not export OrtGetApiBase.";
        return false;
    }
    const OrtApiBase *base = getApiBase();
    runtime->version = QString::fromUtf8(base->GetVersionString());
    runtime->api = base->GetApi(ORT_API_VERSION);
    if (!runtime->api) {
        if (error)
            *error = "ONNX Runtime " + runtime->version + " is older than the required 1.20.";
        return false;
    }
    if (!check(runtime->api, runtime->api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "serika", &runtime->env), error,
               "ONNX Runtime environment"))
        return false;
    m_runtime = std::move(runtime);
    return true;
}
bool AiSegmenter::loadSession(QString *error) {
    const QString model = modelPath();
    if (m_runtime->session && m_loadedModel == model)
        return true;
    m_runtime->releaseSession();
    m_loadedModel.clear();
    if (!QFileInfo(model).isFile()) {
        if (error)
            *error = "The AI model is not installed. Download it in Preferences → AI, or set its path.";
        return false;
    }
    const OrtApi *api = m_runtime->api;
    if (!check(api, api->CreateSessionOptions(&m_runtime->options), error, "Session options") ||
        // 0 lets ONNX Runtime use one thread per physical core, which is fastest for this model.
        !check(
            api,
            api->SetIntraOpNumThreads(m_runtime->options, qEnvironmentVariableIntValue("SERIKA_AI_THREADS")),
            error, "Session threads") ||
        // Subnormal floats in the transformer stages are many times slower on x86 CPUs.
        !check(api, api->AddSessionConfigEntry(m_runtime->options, "session.set_denormal_as_zero", "1"),
               error, "Session options") ||
        !check(api, api->SetSessionGraphOptimizationLevel(m_runtime->options, ORT_ENABLE_ALL), error,
               "Graph optimization"))
        return false;
    m_runtime->device = "CPU";
#ifdef _WIN32
    // The DirectML build of ONNX Runtime runs on any DirectX 12 GPU. Use it when that build is the
    // one installed, unless SERIKA_AI_DEVICE=cpu; any failure simply keeps the CPU provider.
    using AppendDml = OrtStatus *(ORT_API_CALL *)(OrtSessionOptions *, int);
    if (auto appendDml = reinterpret_cast<AppendDml>(
            m_runtime->library.resolve("OrtSessionOptionsAppendExecutionProvider_DML"));
        appendDml && qEnvironmentVariable("SERIKA_AI_DEVICE").compare("cpu", Qt::CaseInsensitive) != 0) {
        if (check(api, api->DisableMemPattern(m_runtime->options), nullptr, {}) &&
            check(api, api->SetSessionExecutionMode(m_runtime->options, ORT_SEQUENTIAL), nullptr, {}) &&
            check(api, appendDml(m_runtime->options, 0), nullptr, {}))
            m_runtime->device = "DirectML GPU";
    }
#endif
#ifdef _WIN32
    const std::wstring modelFile = QDir::toNativeSeparators(model).toStdWString();
#else
    const std::string modelFile = model.toStdString();
#endif
    if (!check(api,
               api->CreateSession(m_runtime->env, modelFile.c_str(), m_runtime->options, &m_runtime->session),
               error, "Loading the AI model"))
        return false;
    OrtAllocator *allocator = nullptr;
    if (!check(api, api->GetAllocatorWithDefaultOptions(&allocator), error, "Allocator"))
        return false;
    char *name = nullptr;
    if (!check(api, api->SessionGetInputName(m_runtime->session, 0, allocator, &name), error, "Model input"))
        return false;
    m_runtime->inputName = name;
    check(api, api->AllocatorFree(allocator, name), nullptr, {});
    if (!check(api, api->SessionGetOutputName(m_runtime->session, 0, allocator, &name), error,
               "Model output"))
        return false;
    m_runtime->outputName = name;
    check(api, api->AllocatorFree(allocator, name), nullptr, {});
    // Use the model's fixed spatial size when it declares one; BiRefNet expects 1024x1024.
    OrtTypeInfo *typeInfo = nullptr;
    if (check(api, api->SessionGetInputTypeInfo(m_runtime->session, 0, &typeInfo), nullptr, {})) {
        const OrtTensorTypeAndShapeInfo *tensor = nullptr;
        size_t count = 0;
        if (check(api, api->CastTypeInfoToTensorInfo(typeInfo, &tensor), nullptr, {}) && tensor &&
            check(api, api->GetDimensionsCount(tensor, &count), nullptr, {}) && count == 4) {
            int64_t dims[4]{};
            if (check(api, api->GetDimensions(tensor, dims, 4), nullptr, {})) {
                m_runtime->inputHeight = dims[2] > 0 ? int(dims[2]) : 1024;
                m_runtime->inputWidth = dims[3] > 0 ? int(dims[3]) : 1024;
            }
        }
        api->ReleaseTypeInfo(typeInfo);
    }
    m_loadedModel = model;
    return true;
}
AiSegmenter::Status AiSegmenter::status() {
    QMutexLocker lock(&m_mutex);
    Status status;
    status.runtimePath = runtimePath();
    status.modelPath = modelPath();
    QString error;
    status.runtimeAvailable = loadRuntime(&error);
    if (status.runtimeAvailable)
        status.runtimeVersion = m_runtime->version;
    status.modelAvailable = QFileInfo(status.modelPath).isFile();
    if (!status.runtimeAvailable)
        status.message = error;
    else if (!status.modelAvailable)
        status.message =
            "ONNX Runtime " + status.runtimeVersion + " is ready; the AI model is not installed.";
    else {
        const bool gpu = m_runtime->library.resolve("OrtSessionOptionsAppendExecutionProvider_DML");
        status.message = "Ready: ONNX Runtime " + status.runtimeVersion + " (" +
                         (gpu ? "DirectML GPU" : "CPU, roughly 5–30 s per image") + ") with " +
                         QFileInfo(status.modelPath).dir().dirName() + ".";
    }
    return status;
}
QImage AiSegmenter::segment(const QImage &image, QString *error) {
    QMutexLocker lock(&m_mutex);
    if (image.isNull()) {
        if (error)
            *error = "There is no image to segment.";
        return {};
    }
    if (!loadRuntime(error) || !loadSession(error))
        return {};
    const OrtApi *api = m_runtime->api;
    const int w = m_runtime->inputWidth, h = m_runtime->inputHeight;
    // ImageNet normalisation on a square resize, as BiRefNet was trained.
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888)
                           .scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    std::vector<float> input(size_t(3) * w * h);
    constexpr float mean[3]{0.485f, 0.456f, 0.406f}, deviation[3]{0.229f, 0.224f, 0.225f};
    for (int y = 0; y < h; ++y) {
        const uchar *row = rgb.constScanLine(y);
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                input[size_t(c) * w * h + size_t(y) * w + x] =
                    (row[x * 3 + c] / 255.0f - mean[c]) / deviation[c];
    }
    OrtMemoryInfo *memory = nullptr;
    if (!check(api, api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory), error, "Memory"))
        return {};
    const int64_t shape[4]{1, 3, h, w};
    OrtValue *inputTensor = nullptr, *outputTensor = nullptr;
    const bool created =
        check(api,
              api->CreateTensorWithDataAsOrtValue(memory, input.data(), input.size() * sizeof(float), shape,
                                                  4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputTensor),
              error, "Input tensor");
    api->ReleaseMemoryInfo(memory);
    if (!created)
        return {};
    const char *inputNames[]{m_runtime->inputName.c_str()};
    const char *outputNames[]{m_runtime->outputName.c_str()};
    const bool ran = check(
        api,
        api->Run(m_runtime->session, nullptr, inputNames, &inputTensor, 1, outputNames, 1, &outputTensor),
        error, "Running the AI model");
    api->ReleaseValue(inputTensor);
    if (!ran)
        return {};
    QImage mask;
    float *output = nullptr;
    OrtTensorTypeAndShapeInfo *info = nullptr;
    size_t count = 0;
    int64_t dims[8]{};
    if (check(api, api->GetTensorMutableData(outputTensor, reinterpret_cast<void **>(&output)), error,
              "Model output") &&
        check(api, api->GetTensorTypeAndShape(outputTensor, &info), error, "Output shape") &&
        check(api, api->GetDimensionsCount(info, &count), error, "Output shape") && count >= 2 &&
        count <= 8 && check(api, api->GetDimensions(info, dims, count), error, "Output shape")) {
        const int oh = int(dims[count - 2]), ow = int(dims[count - 1]);
        const size_t pixels = size_t(ow) * oh;
        // BiRefNet emits logits; apply a sigmoid unless the model already produced probabilities.
        const auto [low, high] = std::minmax_element(output, output + pixels);
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
        mask = small.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    } else if (error && error->isEmpty()) {
        *error = "The AI model returned an unexpected output shape.";
    }
    if (info)
        api->ReleaseTensorTypeAndShapeInfo(info);
    api->ReleaseValue(outputTensor);
    return mask;
}
} // namespace serika
