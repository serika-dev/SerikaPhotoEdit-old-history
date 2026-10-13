#pragma once
#include <QLibrary>
#include <QMutex>
#include <QSize>
#include <QString>
#include <cstdint>
#include <string>
#include <vector>

struct OrtApi;
struct OrtEnv;
struct OrtSession;
struct OrtSessionOptions;

namespace serika {
// ONNX Runtime, loaded once per process with QLibrary when an AI feature first needs it. Serika never
// links it; without the library every AI feature reports what is missing and the rest of the editor
// is unaffected. ONNX Runtime wants a single environment per process, which this owns.
class OnnxRuntime {
  public:
    static OnnxRuntime &instance();
    static QString libraryFileName();
    // Explicit override, SERIKA_ONNXRUNTIME, the ai/runtimePath setting, then next to Serika.
    QString path() const;
    // Takes effect before the runtime is first loaded; a loaded runtime stays for the process.
    void setPath(const QString &path);
    bool load(QString *error);
    bool loaded() const { return m_api; }
    const OrtApi *api() const { return m_api; }
    OrtEnv *env() const { return m_env; }
    QString version() const { return m_version; }
    QString loadedPath() const { return m_library.fileName(); }
    bool directMlAvailable();

  private:
    OnnxRuntime() = default;
    QLibrary m_library;
    const OrtApi *m_api = nullptr;
    OrtEnv *m_env = nullptr;
    QString m_override, m_version;
    QMutex m_mutex;
};

// One model loaded with Serika's standard session options: all physical cores, subnormals flushed
// to zero, full graph optimisation, and DirectML when that build of the runtime is installed.
class OnnxModel {
  public:
    OnnxModel() = default;
    OnnxModel(const OnnxModel &) = delete;
    OnnxModel &operator=(const OnnxModel &) = delete;
    ~OnnxModel();
    bool load(const QString &modelPath, QString *error);
    void release();
    bool loaded() const { return m_session; }
    QString path() const { return m_path; }
    QString device() const { return m_device; }
    // The fixed spatial input size the model declares, or an invalid size when it is dynamic.
    QSize inputSize() const { return m_inputSize; }
    // Runs one float tensor through the model's first input and output.
    bool run(std::vector<float> &input, const std::vector<int64_t> &shape, std::vector<float> *output,
             std::vector<int64_t> *outputShape, QString *error);

  private:
    OrtSession *m_session = nullptr;
    OrtSessionOptions *m_options = nullptr;
    std::string m_input, m_output;
    QString m_path, m_device = "CPU";
    QSize m_inputSize;
};
} // namespace serika
