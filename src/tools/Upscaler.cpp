#include "Upscaler.h"
#include "OnnxRuntime.h"
#include "document/Document.h"
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QtCore/private/qzipreader_p.h>
#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace serika {
namespace {
// Interleaved float pixels; RGBA buffers hold premultiplied values in 0..1.
struct Buffer {
    int width = 0, height = 0, channels = 4;
    std::vector<float> data;
    Buffer() = default;
    Buffer(int w, int h, int c) : width(w), height(h), channels(c), data(size_t(w) * h * c) {}
    float *row(int y) { return data.data() + size_t(y) * width * channels; }
    const float *row(int y) const { return data.data() + size_t(y) * width * channels; }
};

void parallelRows(int rows, const std::function<void(int, int)> &work) {
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    if (rows < 64 || threads == 1) {
        work(0, rows);
        return;
    }
    std::vector<std::thread> pool;
    const int chunk = (rows + threads - 1) / threads;
    for (int start = 0; start < rows; start += chunk)
        pool.emplace_back(work, start, std::min(rows, start + chunk));
    for (auto &thread : pool)
        thread.join();
}

Buffer toBuffer(const QImage &image) {
    const QImage source = image.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    Buffer buffer(source.width(), source.height(), 4);
    for (int y = 0; y < source.height(); ++y) {
        const auto *in = reinterpret_cast<const quint16 *>(source.constScanLine(y));
        float *out = buffer.row(y);
        for (int i = 0; i < source.width() * 4; ++i)
            out[i] = in[i] / 65535.0f;
    }
    return buffer;
}
QImage toImage(const Buffer &buffer, bool sixteenBit) {
    QImage image(buffer.width, buffer.height, QImage::Format_RGBA64_Premultiplied);
    for (int y = 0; y < buffer.height; ++y) {
        const float *in = buffer.row(y);
        auto *out = reinterpret_cast<quint16 *>(image.scanLine(y));
        for (int x = 0; x < buffer.width; ++x) {
            const float alpha = std::clamp(in[x * 4 + 3], 0.0f, 1.0f);
            for (int c = 0; c < 3; ++c)
                out[x * 4 + c] = quint16(std::lround(std::clamp(in[x * 4 + c], 0.0f, alpha) * 65535.0f));
            out[x * 4 + 3] = quint16(std::lround(alpha * 65535.0f));
        }
    }
    return sixteenBit ? image : image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

double lanczos3(double x) {
    x = std::abs(x);
    if (x < 1e-8)
        return 1.0;
    if (x >= 3.0)
        return 0.0;
    const double px = M_PI * x;
    return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
}
struct Taps {
    int first = 0;
    std::vector<float> weights;
};
// Lanczos-3 taps for one axis; when shrinking the kernel widens so it also low-pass filters.
std::vector<Taps> lanczosTaps(int source, int target) {
    const double scale = double(target) / source;
    const double stretch = std::max(1.0, 1.0 / scale);
    const double support = 3.0 * stretch;
    std::vector<Taps> taps(target);
    for (int i = 0; i < target; ++i) {
        const double centre = (i + 0.5) / scale;
        const int lo = int(std::floor(centre - support)), hi = int(std::ceil(centre + support));
        Taps &t = taps[i];
        t.first = std::clamp(lo, 0, source - 1);
        t.weights.assign(size_t(std::clamp(hi, 0, source - 1) - t.first + 1), 0.0f);
        double total = 0;
        for (int j = lo; j <= hi; ++j) {
            const double w = lanczos3((j + 0.5 - centre) / stretch);
            t.weights[size_t(std::clamp(j, 0, source - 1) - t.first)] += float(w);
            total += w;
        }
        for (auto &w : t.weights)
            w = float(w / (total == 0 ? 1 : total));
    }
    return taps;
}
Buffer resample(const Buffer &source, int width, int height) {
    if (source.width == width && source.height == height)
        return source;
    const int c = source.channels;
    const auto horizontal = lanczosTaps(source.width, width);
    Buffer wide(width, source.height, c);
    parallelRows(source.height, [&](int from, int to) {
        for (int y = from; y < to; ++y) {
            const float *in = source.row(y);
            float *out = wide.row(y);
            for (int x = 0; x < width; ++x) {
                const Taps &t = horizontal[x];
                for (int k = 0; k < c; ++k) {
                    float sum = 0;
                    for (size_t j = 0; j < t.weights.size(); ++j)
                        sum += in[(t.first + int(j)) * c + k] * t.weights[j];
                    out[x * c + k] = sum;
                }
            }
        }
    });
    const auto vertical = lanczosTaps(source.height, height);
    Buffer result(width, height, c);
    parallelRows(height, [&](int from, int to) {
        for (int y = from; y < to; ++y) {
            const Taps &t = vertical[y];
            float *out = result.row(y);
            std::fill(out, out + size_t(width) * c, 0.0f);
            for (size_t j = 0; j < t.weights.size(); ++j) {
                const float *in = wide.row(t.first + int(j));
                const float w = t.weights[j];
                for (int i = 0; i < width * c; ++i)
                    out[i] += in[i] * w;
            }
        }
    });
    return result;
}
Buffer gaussianBlur(const Buffer &source, double sigma) {
    const int radius = std::max(1, int(std::ceil(sigma * 3)));
    std::vector<float> kernel(size_t(radius) * 2 + 1);
    float total = 0;
    for (int i = -radius; i <= radius; ++i)
        total += kernel[size_t(i + radius)] = float(std::exp(-i * i / (2 * sigma * sigma)));
    for (auto &k : kernel)
        k /= total;
    const int w = source.width, h = source.height, c = source.channels;
    Buffer wide(w, h, c), result(w, h, c);
    parallelRows(h, [&](int from, int to) {
        for (int y = from; y < to; ++y) {
            const float *in = source.row(y);
            float *out = wide.row(y);
            for (int x = 0; x < w; ++x)
                for (int k = 0; k < c; ++k) {
                    float sum = 0;
                    for (int i = -radius; i <= radius; ++i)
                        sum += in[std::clamp(x + i, 0, w - 1) * c + k] * kernel[size_t(i + radius)];
                    out[x * c + k] = sum;
                }
        }
    });
    parallelRows(h, [&](int from, int to) {
        for (int y = from; y < to; ++y) {
            float *out = result.row(y);
            std::fill(out, out + size_t(w) * c, 0.0f);
            for (int i = -radius; i <= radius; ++i) {
                const float *in = wide.row(std::clamp(y + i, 0, h - 1));
                const float k = kernel[size_t(i + radius)];
                for (int j = 0; j < w * c; ++j)
                    out[j] += in[j] * k;
            }
        }
    });
    return result;
}
// Unsharp mask that restores the edge contrast interpolation loses, with a small threshold so
// flat areas and noise are left alone.
void sharpen(Buffer &image, double sigma, double amount) {
    if (amount <= 0)
        return;
    const Buffer blurred = gaussianBlur(image, sigma);
    constexpr float threshold = 0.004f;
    for (size_t i = 0; i < image.data.size(); ++i) {
        const float difference = image.data[i] - blurred.data[i];
        if (std::abs(difference) > threshold)
            image.data[i] += float(amount) * difference;
    }
}

// Straight-alpha RGB for the model. Colours under fully transparent pixels are undefined, so the
// nearest visible colours are bled outward to keep dark halos out of the enlarged edges.
Buffer straightRgb(const Buffer &premultiplied, bool *hasAlpha) {
    const int w = premultiplied.width, h = premultiplied.height;
    Buffer rgb(w, h, 3);
    std::vector<uint8_t> known(size_t(w) * h, 1);
    *hasAlpha = false;
    for (int y = 0; y < h; ++y) {
        const float *in = premultiplied.row(y);
        float *out = rgb.row(y);
        for (int x = 0; x < w; ++x) {
            const float a = in[x * 4 + 3];
            if (a < 0.999f)
                *hasAlpha = true;
            if (a < 1.0f / 255) {
                known[size_t(y) * w + x] = 0;
                continue;
            }
            for (int c = 0; c < 3; ++c)
                out[x * 3 + c] = std::clamp(in[x * 4 + c] / a, 0.0f, 1.0f);
        }
    }
    if (!*hasAlpha)
        return rgb;
    for (int pass = 0; pass < 24; ++pass) {
        std::vector<uint8_t> next = known;
        bool changed = false;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (known[size_t(y) * w + x])
                    continue;
                float sum[3]{};
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h || !known[size_t(ny) * w + nx])
                            continue;
                        for (int c = 0; c < 3; ++c)
                            sum[c] += rgb.row(ny)[nx * 3 + c];
                        ++count;
                    }
                if (count) {
                    for (int c = 0; c < 3; ++c)
                        rgb.row(y)[x * 3 + c] = sum[c] / count;
                    next[size_t(y) * w + x] = 1;
                    changed = true;
                }
            }
        known.swap(next);
        if (!changed)
            break;
    }
    return rgb;
}

QMutex &modelMutex() {
    static QMutex mutex;
    return mutex;
}
OnnxModel &sharedModel() {
    static OnnxModel model;
    return model;
}
constexpr int tileCore = 96, tilePad = 16;
} // namespace

QVector<UpscaleModelInfo> Upscaler::models() {
    const QString base = "https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/";
    return {{"realesrgan-general-x4v3", "Real-ESRGAN General x4v3",
             "Fast and clean; good for art, screenshots and photos (about 0.4 s per 128 px tile on a CPU).",
             "BSD-3-Clause",
             base + "real_esrgan_general_x4v3/releases/v0.64.0/real_esrgan_general_x4v3-onnx-float.zip",
             "fc094329b24a00c936ebc1cc4c51bc59633e7dc36f4a553344342171cbd3c7cf",
             "real_esrgan_general_x4v3.onnx", 4540895, 4, 0.4},
            {"realesrgan-x4plus", "Real-ESRGAN x4plus",
             "Best quality on photos and detailed art; about 15x slower than General.", "BSD-3-Clause",
             base + "real_esrgan_x4plus/releases/v0.64.0/real_esrgan_x4plus-onnx-float.zip",
             "05aa31323c6a7e831493171cac970e398697709cae7e7ae8e0760e6da4d64ca4", "real_esrgan_x4plus.onnx",
             62174027, 4, 6.0}};
}
UpscaleModelInfo Upscaler::model(const QString &id) {
    for (const auto &info : models())
        if (info.id == id)
            return info;
    return {};
}
QString Upscaler::defaultModelId() { return "realesrgan-general-x4v3"; }
QString Upscaler::preferredModelId() {
    const QString saved = QSettings().value("ai/upscaleModel").toString();
    return model(saved).id.isEmpty() ? defaultModelId() : saved;
}
void Upscaler::setPreferredModelId(const QString &id) { QSettings().setValue("ai/upscaleModel", id); }
QString Upscaler::modelPath(const QString &id) {
    if (const auto env = qEnvironmentVariable("SERIKA_UPSCALE_MODEL"); !env.isEmpty())
        return env;
    const auto info = model(id);
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/models/" + info.id + "/" +
           info.onnxFile;
}
bool Upscaler::installed(const QString &id) {
    return !model(id).id.isEmpty() && QFileInfo(modelPath(id)).isFile();
}
bool Upscaler::installArchive(const QString &zipPath, const QString &id, QString *error) {
    const auto info = model(id);
    const QString folder =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/models/" + info.id;
    QZipReader zip(zipPath);
    if (info.id.isEmpty() || !zip.isReadable() || zip.status() != QZipReader::NoError) {
        if (error)
            *error = "The downloaded archive could not be read.";
        return false;
    }
    if (!QDir().mkpath(folder)) {
        if (error)
            *error = "Could not create " + QDir::toNativeSeparators(folder) + ".";
        return false;
    }
    bool foundModel = false;
    for (const auto &entry : zip.fileInfoList()) {
        // Only the model files, flattened by name, so archive paths can never escape the folder.
        const QString name = QFileInfo(entry.filePath).fileName();
        const QString suffix = QFileInfo(name).suffix().toLower();
        if (!entry.isFile || !(suffix == "onnx" || suffix == "data" || suffix == "json"))
            continue;
        QSaveFile file(folder + "/" + name);
        if (!file.open(QIODevice::WriteOnly) || file.write(zip.fileData(entry.filePath)) != entry.size ||
            !file.commit()) {
            if (error)
                *error = "Could not write " + name + ".";
            return false;
        }
        foundModel |= name == info.onnxFile;
    }
    if (!foundModel && error)
        *error = "The archive does not contain " + info.onnxFile + ".";
    return foundModel;
}
QString Upscaler::methodId(Method method) { return method == Method::Ai ? "ai" : "preserve-details"; }
bool Upscaler::parseMethod(const QString &id, Method *method) {
    const QString key = id.trimmed().toLower();
    if (key == "ai" || key == "super-resolution")
        *method = Method::Ai;
    else if (key == "preserve-details" || key == "preserve" || key == "lanczos")
        *method = Method::PreserveDetails;
    else
        return false;
    return true;
}
int Upscaler::tileCount(QSize size) {
    return ((size.width() + tileCore - 1) / tileCore) * ((size.height() + tileCore - 1) / tileCore);
}

QImage Upscaler::preserveDetails(const QImage &image, QSize target, double detail) {
    if (image.isNull() || target.isEmpty())
        return {};
    const bool sixteenBit = image.depth() > 32;
    Buffer result = resample(toBuffer(image), target.width(), target.height());
    const double scale =
        std::sqrt(double(target.width()) * target.height() / (double(image.width()) * image.height()));
    // Enlargement softens edges in proportion to the scale; give back about that much contrast.
    if (detail > 0)
        sharpen(result, std::clamp(0.45 * scale, 0.5, 3.0), detail * (scale > 1 ? 1.1 : 0.4));
    return toImage(result, sixteenBit);
}

QImage Upscaler::aiUpscale(const QImage &image, QSize target, const QString &modelId,
                           const Progress &progress, QString *error) {
    if (image.isNull() || target.isEmpty()) {
        if (error)
            *error = "There is no image to upscale.";
        return {};
    }
    const auto info = model(modelId.isEmpty() ? preferredModelId() : modelId);
    if (info.id.isEmpty()) {
        if (error)
            *error = "Unknown upscale model \"" + modelId + "\".";
        return {};
    }
    QMutexLocker lock(&modelMutex());
    OnnxModel &onnx = sharedModel();
    if (!onnx.load(modelPath(info.id), error))
        return {};
    const QSize tile = onnx.inputSize().isValid() ? onnx.inputSize() : QSize(128, 128);
    const int pad = std::min(tilePad, tile.width() / 4);
    const int coreW = tile.width() - 2 * pad, coreH = tile.height() - 2 * pad;
    const Buffer source = toBuffer(image);
    bool hasAlpha = false;
    const Buffer rgb = straightRgb(source, &hasAlpha);
    const int w = source.width, h = source.height, s = info.scale;
    Buffer enlarged(w * s, h * s, 4);
    const int columns = (w + coreW - 1) / coreW, rows = (h + coreH - 1) / coreH;
    std::vector<float> input(size_t(3) * tile.width() * tile.height()), output;
    std::vector<int64_t> shape;
    for (int row = 0; row < rows; ++row)
        for (int column = 0; column < columns; ++column) {
            if (progress && !progress(double(row * columns + column) / (rows * columns))) {
                if (error)
                    *error = "Cancelled.";
                return {};
            }
            const int x0 = column * coreW, y0 = row * coreH;
            const size_t plane = size_t(tile.width()) * tile.height();
            // Edge pixels repeat past the image border so every tile is full-size.
            for (int ty = 0; ty < tile.height(); ++ty) {
                const float *line = rgb.row(std::clamp(y0 - pad + ty, 0, h - 1));
                for (int tx = 0; tx < tile.width(); ++tx) {
                    const float *pixel = line + std::clamp(x0 - pad + tx, 0, w - 1) * 3;
                    for (int c = 0; c < 3; ++c)
                        input[c * plane + size_t(ty) * tile.width() + tx] = pixel[c];
                }
            }
            if (!onnx.run(input, {1, 3, tile.height(), tile.width()}, &output, &shape, error))
                return {};
            if (shape.size() != 4 || shape[1] != 3 || shape[2] != tile.height() * s ||
                shape[3] != tile.width() * s) {
                if (error)
                    *error = "The upscale model returned an unexpected output shape.";
                return {};
            }
            const int outW = int(shape[3]), outPlane = int(shape[2] * shape[3]);
            const int copyW = std::min(coreW, w - x0) * s, copyH = std::min(coreH, h - y0) * s;
            for (int y = 0; y < copyH; ++y) {
                float *out = enlarged.row(y0 * s + y) + size_t(x0) * s * 4;
                const size_t from = size_t(pad * s + y) * outW + pad * s;
                for (int x = 0; x < copyW; ++x)
                    for (int c = 0; c < 3; ++c)
                        out[x * 4 + c] = std::clamp(output[c * size_t(outPlane) + from + x], 0.0f, 1.0f);
            }
        }
    lock.unlock();
    // Alpha is resampled conventionally and the model's colours are premultiplied by it.
    Buffer alpha;
    if (hasAlpha) {
        Buffer sourceAlpha(w, h, 1);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                sourceAlpha.row(y)[x] = source.row(y)[x * 4 + 3];
        alpha = resample(sourceAlpha, w * s, h * s);
    }
    for (int y = 0; y < h * s; ++y) {
        float *line = enlarged.row(y);
        for (int x = 0; x < w * s; ++x) {
            const float a = hasAlpha ? std::clamp(alpha.row(y)[x], 0.0f, 1.0f) : 1.0f;
            for (int c = 0; c < 3; ++c)
                line[x * 4 + c] *= a;
            line[x * 4 + 3] = a;
        }
    }
    if (progress)
        progress(1.0);
    return toImage(resample(enlarged, target.width(), target.height()), image.depth() > 32);
}

QImage Upscaler::upscale(const QImage &image, QSize target, Method method, const QString &modelId,
                         double detail, const Progress &progress, QString *error) {
    // Shrinking or barely enlarging gains nothing from the model.
    const bool enlarging = target.width() > image.width() * 1.05 || target.height() > image.height() * 1.05;
    if (method == Method::Ai && enlarging)
        return aiUpscale(image, target, modelId, progress, error);
    QImage result = preserveDetails(image, target, detail);
    if (result.isNull() && error)
        *error = "There is no image to resize.";
    return result;
}

QHash<quint64, QImage> Upscaler::upscaleBatch(const QHash<quint64, Job> &jobs, Method method,
                                              const QString &modelId, double detail, const Progress &progress,
                                              QString *error) {
    // Weight each layer by its share of the work so the bar moves evenly.
    const auto weight = [method](const Job &job) {
        return method == Method::Ai ? double(tileCount(job.image.size()))
                                    : double(job.target.width()) * job.target.height() / 1e6 + 0.1;
    };
    double total = 0, done = 0;
    for (const auto &job : jobs)
        total += weight(job);
    QHash<quint64, QImage> results;
    for (auto it = jobs.cbegin(); it != jobs.cend(); ++it) {
        const double share = weight(*it);
        const Progress step = [&](double fraction) {
            return !progress || progress((done + fraction * share) / std::max(total, 1e-9));
        };
        if (!step(0)) {
            if (error)
                *error = "Cancelled.";
            return {};
        }
        QImage image = upscale(it->image, it->target, method, modelId, detail, step, error);
        if (image.isNull())
            return {};
        results.insert(it.key(), image);
        done += share;
    }
    if (progress)
        progress(1.0);
    return results;
}
QHash<quint64, Upscaler::Job> Upscaler::documentJobs(const Document *document, QSize canvas) {
    QHash<quint64, Job> jobs;
    for (const auto &layer : document->state.layers)
        if (layer.kind == LayerKind::Pixel && !layer.pixels.empty() && !layer.pixels.size.isEmpty())
            jobs.insert(layer.id,
                        {layer.pixels.image(),
                         Document::resizedLayerSize(layer.pixels.size, document->state.size, canvas)});
    return jobs;
}
bool Upscaler::applyLayer(Document *document, quint64 layerId, const QImage &pixels, QString *error) {
    const int index = document->indexForId(layerId);
    if (index < 0 || pixels.isNull()) {
        if (error)
            *error = "The layer no longer exists.";
        return false;
    }
    const Layer &layer = document->state.layers[index];
    if (layer.kind != LayerKind::Pixel || layer.locked) {
        if (error)
            *error = "Only unlocked pixel layers can be upscaled.";
        return false;
    }
    document->mutate("Upscale Layer", [document, index, pixels] {
        Layer &l = document->state.layers[index];
        const QSizeF old = l.pixels.size;
        const qreal sx = pixels.width() / old.width(), sy = pixels.height() / old.height();
        const QPointF centre = l.offset + QPointF(old.width() / 2, old.height() / 2);
        if (!l.mask.isNull()) {
            // Masks live in layer coordinates, so they scale with the pixels.
            l.mask = l.mask.scaled(
                QSize(std::max(1, qRound(l.mask.width() * sx)), std::max(1, qRound(l.mask.height() * sy))),
                Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            l.maskOffset = QPointF(l.maskOffset.x() * sx, l.maskOffset.y() * sy);
        }
        l.pixels.setImage(pixels);
        l.offset = QPointF(std::round(centre.x() - pixels.width() / 2.0),
                           std::round(centre.y() - pixels.height() / 2.0));
    });
    return true;
}
double Upscaler::estimateSeconds(const QHash<quint64, Job> &jobs, Method method, const QString &modelId) {
    double seconds = 0;
    const double perTile = model(modelId.isEmpty() ? preferredModelId() : modelId).secondsPerTile;
    for (const auto &job : jobs)
        seconds += method == Method::Ai ? tileCount(job.image.size()) * perTile
                                        : double(job.target.width()) * job.target.height() / 8e6;
    return seconds;
}
} // namespace serika
