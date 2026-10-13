#include "OnnxRuntime.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>
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

namespace serika {
namespace {
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

OnnxRuntime &OnnxRuntime::instance() {
    static OnnxRuntime runtime;
    return runtime;
}
QString OnnxRuntime::libraryFileName() {
#ifdef Q_OS_WIN
    return "onnxruntime.dll";
#elif defined(Q_OS_MACOS)
    return "libonnxruntime.dylib";
#else
    return "libonnxruntime.so";
#endif
}
QString OnnxRuntime::path() const {
    if (!m_override.isEmpty())
        return m_override;
    if (const auto env = qEnvironmentVariable("SERIKA_ONNXRUNTIME"); !env.isEmpty())
        return env;
    if (const auto saved = QSettings().value("ai/runtimePath").toString(); !saved.isEmpty())
        return saved;
    // An explicit path avoids picking up an unrelated, older onnxruntime from the system search path.
    const QStringList candidates{QCoreApplication::applicationDirPath() + "/" + libraryFileName(),
                                 QCoreApplication::applicationDirPath() + "/ai/" + libraryFileName(),
                                 QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                     "/runtime/" + libraryFileName()};
    for (const auto &candidate : candidates)
        if (QFileInfo(candidate).isFile())
            return candidate;
    return candidates.first();
}
void OnnxRuntime::setPath(const QString &path) {
    QMutexLocker lock(&m_mutex);
    m_override = path;
}
bool OnnxRuntime::load(QString *error) {
    QMutexLocker lock(&m_mutex);
    if (m_api)
        return true;
    const QString file = path();
    if (!QFileInfo(file).isFile()) {
        if (error)
            *error = "ONNX Runtime was not found at " + QDir::toNativeSeparators(file) + ".";
        return false;
    }
    m_library.setFileName(file);
    if (!m_library.load()) {
        if (error)
            *error = "Could not load ONNX Runtime: " + m_library.errorString();
        return false;
    }
    using GetApiBase = const OrtApiBase *(ORT_API_CALL *)();
    auto getApiBase = reinterpret_cast<GetApiBase>(m_library.resolve("OrtGetApiBase"));
    if (!getApiBase) {
        if (error)
            *error = "The ONNX Runtime library does not export OrtGetApiBase.";
        return false;
    }
    const OrtApiBase *base = getApiBase();
    const OrtApi *api = base->GetApi(ORT_API_VERSION);
    const QString version = QString::fromUtf8(base->GetVersionString());
    if (!api) {
        if (error)
            *error = "ONNX Runtime " + version + " is older than the required 1.20.";
        return false;
    }
    OrtEnv *env = nullptr;
    if (!check(api, api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "serika", &env), error,
               "ONNX Runtime environment"))
        return false;
    m_api = api;
    m_env = env;
    m_version = version;
    return true;
}
bool OnnxRuntime::directMlAvailable() {
    return m_api && m_library.resolve("OrtSessionOptionsAppendExecutionProvider_DML");
}

OnnxModel::~OnnxModel() { release(); }
void OnnxModel::release() {
    const OrtApi *api = OnnxRuntime::instance().api();
    if (api && m_session)
        api->ReleaseSession(m_session);
    if (api && m_options)
        api->ReleaseSessionOptions(m_options);
    m_session = nullptr;
    m_options = nullptr;
    m_path.clear();
    m_inputSize = {};
}
bool OnnxModel::load(const QString &modelPath, QString *error) {
    if (m_session && m_path == modelPath)
        return true;
    release();
    auto &runtime = OnnxRuntime::instance();
    if (!runtime.load(error))
        return false;
    if (!QFileInfo(modelPath).isFile()) {
        if (error)
            *error = "The model file " + QDir::toNativeSeparators(modelPath) + " is not installed.";
        return false;
    }
    const OrtApi *api = runtime.api();
    if (!check(api, api->CreateSessionOptions(&m_options), error, "Session options") ||
        // 0 lets ONNX Runtime use one thread per physical core, which is fastest for these models.
        !check(api, api->SetIntraOpNumThreads(m_options, qEnvironmentVariableIntValue("SERIKA_AI_THREADS")),
               error, "Session threads") ||
        // Subnormal floats are many times slower on x86 CPUs.
        !check(api, api->AddSessionConfigEntry(m_options, "session.set_denormal_as_zero", "1"), error,
               "Session options") ||
        !check(api, api->SetSessionGraphOptimizationLevel(m_options, ORT_ENABLE_ALL), error,
               "Graph optimization"))
        return false;
    m_device = "CPU";
#ifdef _WIN32
    // The DirectML build of ONNX Runtime runs on any DirectX 12 GPU. Use it when that build is the
    // one installed, unless SERIKA_AI_DEVICE=cpu; any failure simply keeps the CPU provider.
    using AppendDml = OrtStatus *(ORT_API_CALL *)(OrtSessionOptions *, int);
    if (runtime.directMlAvailable() &&
        qEnvironmentVariable("SERIKA_AI_DEVICE").compare("cpu", Qt::CaseInsensitive) != 0) {
        QLibrary library(runtime.loadedPath());
        auto appendDml =
            reinterpret_cast<AppendDml>(library.resolve("OrtSessionOptionsAppendExecutionProvider_DML"));
        if (appendDml && check(api, api->DisableMemPattern(m_options), nullptr, {}) &&
            check(api, api->SetSessionExecutionMode(m_options, ORT_SEQUENTIAL), nullptr, {}) &&
            check(api, appendDml(m_options, 0), nullptr, {}))
            m_device = "DirectML GPU";
    }
    const std::wstring modelFile = QDir::toNativeSeparators(modelPath).toStdWString();
#else
    const std::string modelFile = modelPath.toStdString();
#endif
    if (!check(api, api->CreateSession(runtime.env(), modelFile.c_str(), m_options, &m_session), error,
               "Loading the AI model"))
        return false;
    OrtAllocator *allocator = nullptr;
    char *name = nullptr;
    if (!check(api, api->GetAllocatorWithDefaultOptions(&allocator), error, "Allocator") ||
        !check(api, api->SessionGetInputName(m_session, 0, allocator, &name), error, "Model input"))
        return false;
    m_input = name;
    check(api, api->AllocatorFree(allocator, name), nullptr, {});
    if (!check(api, api->SessionGetOutputName(m_session, 0, allocator, &name), error, "Model output"))
        return false;
    m_output = name;
    check(api, api->AllocatorFree(allocator, name), nullptr, {});
    OrtTypeInfo *typeInfo = nullptr;
    if (check(api, api->SessionGetInputTypeInfo(m_session, 0, &typeInfo), nullptr, {})) {
        const OrtTensorTypeAndShapeInfo *tensor = nullptr;
        size_t count = 0;
        if (check(api, api->CastTypeInfoToTensorInfo(typeInfo, &tensor), nullptr, {}) && tensor &&
            check(api, api->GetDimensionsCount(tensor, &count), nullptr, {}) && count == 4) {
            int64_t dims[4]{};
            if (check(api, api->GetDimensions(tensor, dims, 4), nullptr, {}) && dims[2] > 0 && dims[3] > 0)
                m_inputSize = QSize(int(dims[3]), int(dims[2]));
        }
        api->ReleaseTypeInfo(typeInfo);
    }
    m_path = modelPath;
    return true;
}
bool OnnxModel::run(std::vector<float> &input, const std::vector<int64_t> &shape, std::vector<float> *output,
                    std::vector<int64_t> *outputShape, QString *error) {
    const OrtApi *api = OnnxRuntime::instance().api();
    if (!api || !m_session) {
        if (error)
            *error = "The AI model is not loaded.";
        return false;
    }
    OrtMemoryInfo *memory = nullptr;
    if (!check(api, api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory), error, "Memory"))
        return false;
    OrtValue *inputTensor = nullptr, *outputTensor = nullptr;
    const bool created = check(
        api,
        api->CreateTensorWithDataAsOrtValue(memory, input.data(), input.size() * sizeof(float), shape.data(),
                                            shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputTensor),
        error, "Input tensor");
    api->ReleaseMemoryInfo(memory);
    if (!created)
        return false;
    const char *inputs[]{m_input.c_str()};
    const char *outputs[]{m_output.c_str()};
    const bool ran =
        check(api, api->Run(m_session, nullptr, inputs, &inputTensor, 1, outputs, 1, &outputTensor), error,
              "Running the AI model");
    api->ReleaseValue(inputTensor);
    if (!ran)
        return false;
    float *data = nullptr;
    OrtTensorTypeAndShapeInfo *info = nullptr;
    size_t count = 0;
    bool ok = check(api, api->GetTensorMutableData(outputTensor, reinterpret_cast<void **>(&data)), error,
                    "Model output") &&
              check(api, api->GetTensorTypeAndShape(outputTensor, &info), error, "Output shape") &&
              check(api, api->GetDimensionsCount(info, &count), error, "Output shape") && count >= 1 &&
              count <= 8;
    if (ok) {
        outputShape->assign(count, 0);
        ok = check(api, api->GetDimensions(info, outputShape->data(), count), error, "Output shape");
        size_t elements = 1;
        for (const auto dimension : *outputShape)
            elements *= size_t(std::max<int64_t>(dimension, 0));
        if (ok)
            output->assign(data, data + elements);
    }
    if (info)
        api->ReleaseTensorTypeAndShapeInfo(info);
    api->ReleaseValue(outputTensor);
    return ok;
}
} // namespace serika
