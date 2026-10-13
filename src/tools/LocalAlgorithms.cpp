#include "LocalAlgorithms.h"
#include "AiSegmenter.h"
#include "document/Document.h"
#include <QColor>
#include <QMutex>
#include <QMutexLocker>
#include <QPoint>
#include <QSettings>
#include <QVector>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>

namespace serika {
namespace {
QMutex selectorMutex;
std::shared_ptr<ISubjectSelector> customSelector;
int coverageDepth(const QImage &image) {
    return image.format() == QImage::Format_RGBA32FPx4 ? 32 : image.depth() > 32 ? 16 : 8;
}
int colorDistance(QRgb a, QRgb b) {
    const int r = qRed(a) - qRed(b), g = qGreen(a) - qGreen(b), bl = qBlue(a) - qBlue(b);
    return r * r + g * g + bl * bl;
}
qreal coverageAt(const QImage &mask, int x, int y) { return maskSample(mask, x, y); }
} // namespace
void registerSubjectSelector(std::shared_ptr<ISubjectSelector> selector) {
    QMutexLocker locker(&selectorMutex);
    customSelector = std::move(selector);
}
QImage selectSubjectLocally(const QImage &image) {
    std::shared_ptr<ISubjectSelector> selector;
    {
        QMutexLocker locker(&selectorMutex);
        selector = customSelector;
    }
    LocalSubjectSelector fallback;
    QImage mask = selector ? selector->select(image) : fallback.select(image);
    if (mask.isNull())
        mask = fallback.select(image);
    mask = mask.size() == image.size()
               ? mask
               : mask.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return normalizeMask(mask, coverageDepth(image));
}
QString subjectEngineId(SubjectEngine engine) { return engine == SubjectEngine::Ai ? "ai" : "fast"; }
bool parseSubjectEngine(const QString &id, SubjectEngine *engine) {
    const auto key = id.trimmed().toLower();
    if (key != "ai" && key != "fast")
        return false;
    *engine = key == "ai" ? SubjectEngine::Ai : SubjectEngine::Fast;
    return true;
}
SubjectEngine preferredSubjectEngine() {
    SubjectEngine engine = SubjectEngine::Fast;
    parseSubjectEngine(QSettings().value("ai/subjectEngine", "fast").toString(), &engine);
    return engine;
}
void setPreferredSubjectEngine(SubjectEngine engine) {
    QSettings().setValue("ai/subjectEngine", subjectEngineId(engine));
}
QImage selectSubject(const QImage &image, SubjectEngine engine, QString *error) {
    if (engine == SubjectEngine::Fast)
        return selectSubjectLocally(image);
    const QImage mask = AiSegmenter::instance().segment(image, error);
    return mask.isNull() ? QImage() : normalizeMask(mask, coverageDepth(image));
}
QImage layerSubjectSource(const Document *document, int layerIndex) {
    if (!document || layerIndex < 0 || layerIndex >= document->state.layers.size())
        return {};
    return document->layerImage(document->state.layers[layerIndex]);
}
bool applyBackgroundMask(Document *document, int layerIndex, const QImage &subject, QString *error) {
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!document || layerIndex < 0 || layerIndex >= document->state.layers.size())
        return fail("Select a layer first.");
    if (document->state.layers[layerIndex].locked)
        return fail("The layer is locked.");
    const QSize size = layerSubjectSource(document, layerIndex).size();
    if (subject.isNull() || size.isEmpty())
        return fail("The layer has no pixels to separate.");
    const QImage scaled = subject.size() == size
                              ? subject
                              : subject.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    document->mutate("Remove background", [&] {
        Layer &layer = document->state.layers[layerIndex];
        QImage mask = makeMask(size, document->state.bitDepth, 0);
        const bool keep = !layer.mask.isNull() && layer.mask.size() == size && layer.maskLinked &&
                          layer.maskOffset.isNull();
        for (int y = 0; y < size.height(); ++y)
            for (int x = 0; x < size.width(); ++x)
                setMaskSample(mask, x, y,
                              maskSample(scaled, x, y) * (keep ? maskSample(layer.mask, x, y) : 1.0));
        layer.mask = mask;
        layer.maskEnabled = true;
        layer.maskTarget = false;
        layer.maskLinked = true;
        layer.maskOffset = {};
        layer.maskDensity = 1;
        layer.maskFeather = 0;
    });
    return true;
}

QImage ColorModelSubjectSelector::select(const QImage &input) const {
    if (input.isNull())
        return {};
    const QSize originalSize = input.size();
    const QImage image = (input.width() > 800 || input.height() > 800
                              ? input.scaled(800, 800, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                              : input)
                             .convertToFormat(QImage::Format_ARGB32);
    const int w = image.width(), h = image.height();
    // Border samples train a small RGB codebook. Flooding only border-connected candidates
    // avoids discarding a similarly colored isolated object in the middle of the frame.
    std::array<QRgb, 12> centers{};
    QVector<QRgb> samples;
    const int edge = std::max(1, std::min(w, h) / 40);
    for (int y = 0; y < h; y += 2)
        for (int x = 0; x < w; x += 2)
            if (x < edge || y < edge || x >= w - edge || y >= h - edge)
                samples.append(image.pixel(x, y));
    if (samples.isEmpty())
        return {};
    for (size_t i = 0; i < centers.size(); ++i)
        centers[i] = samples[int(i * samples.size() / centers.size())];
    for (int iteration = 0; iteration < 6; ++iteration) {
        std::array<std::array<qint64, 4>, 12> sums{};
        for (const QRgb sample : samples) {
            int best = 0, distance = std::numeric_limits<int>::max();
            for (int c = 0; c < 12; ++c) {
                const int d = colorDistance(sample, centers[size_t(c)]);
                if (d < distance) {
                    distance = d;
                    best = c;
                }
            }
            auto &s = sums[size_t(best)];
            s[0] += qRed(sample);
            s[1] += qGreen(sample);
            s[2] += qBlue(sample);
            ++s[3];
        }
        for (int c = 0; c < 12; ++c) {
            const auto &s = sums[size_t(c)];
            if (s[3])
                centers[size_t(c)] = qRgb(int(s[0] / s[3]), int(s[1] / s[3]), int(s[2] / s[3]));
        }
    }
    QVector<quint8> background(w * h, 0);
    QVector<QPoint> queue;
    auto candidate = [&](int x, int y) {
        const QRgb pixel = image.pixel(x, y);
        if (qAlpha(pixel) < 20)
            return true;
        int d = std::numeric_limits<int>::max();
        for (const QRgb center : centers)
            d = std::min(d, colorDistance(pixel, center));
        return d < 35 * 35 * 3;
    };
    auto enqueue = [&](int x, int y) {
        const int i = y * w + x;
        if (!background[i] && candidate(x, y)) {
            background[i] = 1;
            queue.append(QPoint(x, y));
        }
    };
    for (int x = 0; x < w; ++x) {
        enqueue(x, 0);
        enqueue(x, h - 1);
    }
    for (int y = 0; y < h; ++y) {
        enqueue(0, y);
        enqueue(w - 1, y);
    }
    for (qsizetype i = 0; i < queue.size(); ++i) {
        const QPoint p = queue[i];
        if (p.x() > 0)
            enqueue(p.x() - 1, p.y());
        if (p.x() + 1 < w)
            enqueue(p.x() + 1, p.y());
        if (p.y() > 0)
            enqueue(p.x(), p.y() - 1);
        if (p.y() + 1 < h)
            enqueue(p.x(), p.y() + 1);
    }
    QImage mask = makeMask({w, h}, coverageDepth(input));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            qreal coverage = background[y * w + x] ? 0 : 1;
            if (coverage > 0 && x > 0 && y > 0 && x + 1 < w && y + 1 < h) {
                // Recover fractional boundary coverage from a local foreground/background color line.
                // Unlike blanket blur, this keeps enclosed same-colored regions selected and leaves
                // thin contrastful strands intact.
                double foreground[3]{}, backdrop[3]{};
                int fgCount = 0, bgCount = 0;
                for (int dy = -3; dy <= 3; ++dy)
                    for (int dx = -3; dx <= 3; ++dx) {
                        const int sx = std::clamp(x + dx, 0, w - 1), sy = std::clamp(y + dy, 0, h - 1);
                        const QRgb sample = image.pixel(sx, sy);
                        double *sum = background[sy * w + sx] ? backdrop : foreground;
                        sum[0] += qRed(sample);
                        sum[1] += qGreen(sample);
                        sum[2] += qBlue(sample);
                        if (background[sy * w + sx])
                            ++bgCount;
                        else
                            ++fgCount;
                    }
                if (fgCount && bgCount) {
                    const QRgb pixel = image.pixel(x, y);
                    const double channels[3]{double(qRed(pixel)), double(qGreen(pixel)),
                                             double(qBlue(pixel))};
                    double numerator = 0, denominator = 0;
                    for (int c = 0; c < 3; ++c) {
                        const double bg = backdrop[c] / bgCount, delta = foreground[c] / fgCount - bg;
                        numerator += (channels[c] - bg) * delta;
                        denominator += delta * delta;
                    }
                    if (denominator > 100)
                        coverage = std::clamp(numerator / denominator, 0., 1.);
                }
            }
            setMaskSample(mask, x, y, coverage);
        }
    }
    mask = mask.scaled(originalSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (originalSize != image.size())
        mask = refineMask(mask, {{"radius", 2}, {"smartRadius", true}}, input);
    for (int y = 0; y < input.height(); ++y)
        for (int x = 0; x < input.width(); ++x) {
            qreal alpha;
            if (input.format() == QImage::Format_RGBA64)
                alpha = reinterpret_cast<const QRgba64 *>(input.constScanLine(y))[x].alpha() / 65535.;
            else if (input.format() == QImage::Format_RGBA32FPx4)
                alpha = reinterpret_cast<const float *>(input.constScanLine(y))[x * 4 + 3];
            else
                alpha = input.pixelColor(x, y).alphaF();
            setMaskSample(mask, x, y, maskSample(mask, x, y) * alpha);
        }
    return mask;
}

QImage selectFocusAreaLocally(const QImage &input, const QJsonObject &parameters) {
    if (input.isNull())
        return {};
    const QImage image = (input.width() > 1600 || input.height() > 1600
                              ? input.scaled(1600, 1600, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                              : input)
                             .convertToFormat(QImage::Format_RGBA8888);
    const int width = image.width(), height = image.height();
    std::vector<double> intensity(size_t(width) * height), energy(intensity.size());
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const uchar *pixel = image.constScanLine(y) + x * 4;
            intensity[size_t(y) * width + x] =
                (.2126 * pixel[0] + .7152 * pixel[1] + .0722 * pixel[2]) / 255.;
        }
    std::vector<double> integral(size_t(width + 1) * (height + 1), 0);
    for (int y = 0; y < height; ++y) {
        double row = 0;
        for (int x = 0; x < width; ++x) {
            const auto sample = [&](int sx, int sy) {
                return intensity[size_t(std::clamp(sy, 0, height - 1)) * width +
                                 std::clamp(sx, 0, width - 1)];
            };
            const double laplacian =
                4 * sample(x, y) - sample(x - 1, y) - sample(x + 1, y) - sample(x, y - 1) - sample(x, y + 1);
            row += laplacian * laplacian;
            integral[size_t(y + 1) * (width + 1) + x + 1] = row + integral[size_t(y) * (width + 1) + x + 1];
        }
    }
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const int left = std::max(0, x - 7), top = std::max(0, y - 7), right = std::min(width, x + 8),
                      bottom = std::min(height, y + 8);
            const double sum =
                integral[size_t(bottom) * (width + 1) + right] - integral[size_t(top) * (width + 1) + right] -
                integral[size_t(bottom) * (width + 1) + left] + integral[size_t(top) * (width + 1) + left];
            energy[size_t(y) * width + x] = sum / ((right - left) * (bottom - top));
        }
    std::vector<double> samples = energy;
    const size_t percentile = samples.size() * 9 / 10;
    std::nth_element(samples.begin(), samples.begin() + qsizetype(percentile), samples.end());
    const double high = samples[percentile];
    const double range = parameters.value("inFocusRange").toDouble(-1);
    const double threshold = std::max(1e-6, high * (range < 0 ? .25 : std::clamp(range / 10., .02, 1.)));
    QImage result = makeMask(image.size(), coverageDepth(input));
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const double ratio = energy[size_t(y) * width + x] / threshold;
            const double t = std::clamp((ratio - .3) / .7, 0., 1.);
            setMaskSample(result, x, y, t * t * (3 - 2 * t) * image.constScanLine(y)[x * 4 + 3] / 255.);
        }
    result = result.scaled(input.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return refineMask(result, {{"feather", parameters.value("feather").toDouble(1)},
                               {"shiftEdge", parameters.value("grow").toDouble()}});
}

QImage synthesizePatches(const QImage &input, const QImage &inputMask) {
    if (input.isNull() || inputMask.isNull())
        return input;
    const QImage source = input.convertToFormat(QImage::Format_ARGB32);
    QImage result = source;
    const QImage mask = normalizeMask(inputMask);
    const int w = source.width(), h = source.height();
    QVector<quint8> unknown(w * h, 0);
    QVector<QPoint> pixels, donors;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (coverageAt(mask, x, y) > .5) {
                unknown[y * w + x] = 1;
                pixels.append(QPoint(x, y));
            } else if ((x % 3) == 0 && (y % 3) == 0)
                donors.append(QPoint(x, y));
        }
    if (pixels.isEmpty() || donors.isEmpty())
        return input;
    const QVector<quint8> originalUnknown = unknown;
    std::mt19937 generator(0x53455249);
    std::uniform_int_distribution<int> pick(0, int(donors.size()) - 1);
    // Exemplar fill advances from the boundary toward the hole. Candidate patches are
    // initialized randomly, then improved by neighbor propagation and shrinking searches.
    QVector<QPoint> nearest(w * h, QPoint(-1, -1));
    int remaining = int(pixels.size());
    constexpr int patchRadius = 2;
    auto cost = [&](QPoint target, QPoint donor) {
        if (donor.x() < patchRadius || donor.y() < patchRadius || donor.x() >= w - patchRadius ||
            donor.y() >= h - patchRadius)
            return std::numeric_limits<double>::infinity();
        double sum = 0;
        int samples = 0;
        for (int dy = -patchRadius; dy <= patchRadius; ++dy)
            for (int dx = -patchRadius; dx <= patchRadius; ++dx) {
                const int tx = target.x() + dx, ty = target.y() + dy, sx = donor.x() + dx,
                          sy = donor.y() + dy;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h)
                    continue;
                if (originalUnknown[sy * w + sx])
                    return std::numeric_limits<double>::infinity();
                if (!unknown[ty * w + tx]) {
                    sum += colorDistance(result.pixel(tx, ty), source.pixel(sx, sy));
                    ++samples;
                }
            }
        return samples ? sum / samples : std::numeric_limits<double>::infinity();
    };
    QVector<QPoint> frontier;
    QVector<quint8> queued(w * h, 0);
    auto enqueue = [&](QPoint p) {
        if (p.x() < 0 || p.y() < 0 || p.x() >= w || p.y() >= h)
            return;
        const int index = p.y() * w + p.x();
        if (unknown[index] && !queued[index]) {
            frontier.append(p);
            queued[index] = 1;
        }
    };
    for (const QPoint p : pixels) {
        const int x = p.x(), y = p.y();
        if ((x && !unknown[y * w + x - 1]) || (x + 1 < w && !unknown[y * w + x + 1]) ||
            (y && !unknown[(y - 1) * w + x]) || (y + 1 < h && !unknown[(y + 1) * w + x]))
            enqueue(p);
    }
    for (qsizetype cursor = 0; cursor < frontier.size() && remaining > 0; ++cursor) {
        const QPoint target = frontier[cursor];
        QPoint best = donors[pick(generator)];
        double bestCost = cost(target, best);
        auto improve = [&](QPoint candidate) {
            const double d = cost(target, candidate);
            if (d < bestCost) {
                bestCost = d;
                best = candidate;
            }
        };
        for (int attempt = 0; attempt < 32; ++attempt)
            improve(donors[pick(generator)]);
        const std::array<QPoint, 4> directions{QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1)};
        for (const QPoint direction : directions) {
            const QPoint neighbor = target + direction;
            if (neighbor.x() >= 0 && neighbor.y() >= 0 && neighbor.x() < w && neighbor.y() < h) {
                const QPoint n = nearest[neighbor.y() * w + neighbor.x()];
                if (n.x() >= 0)
                    improve(n - direction);
            }
        }
        for (int radius = std::max(w, h); radius >= 1; radius /= 2) {
            std::uniform_int_distribution<int> delta(-radius, radius);
            for (int i = 0; i < 4; ++i)
                improve(best + QPoint(delta(generator), delta(generator)));
        }
        result.setPixel(target, source.pixel(best));
        nearest[target.y() * w + target.x()] = best;
        unknown[target.y() * w + target.x()] = 0;
        --remaining;
        for (const QPoint direction : directions)
            enqueue(target + direction);
    }
    // Blend the soft mask only once, retaining fractional selection boundaries.
    QImage highDepth = input;
    for (const QPoint p : pixels) {
        const QPoint donor = nearest[p.y() * w + p.x()];
        if (donor.x() < 0)
            continue;
        const qreal a = coverageAt(mask, p.x(), p.y());
        if (highDepth.format() == QImage::Format_RGBA32FPx4) {
            float *pixel = reinterpret_cast<float *>(highDepth.scanLine(p.y())) + p.x() * 4;
            const float *oldPixel = reinterpret_cast<const float *>(input.constScanLine(p.y())) + p.x() * 4;
            const float *donorPixel =
                reinterpret_cast<const float *>(input.constScanLine(donor.y())) + donor.x() * 4;
            for (int c = 0; c < 4; ++c)
                pixel[c] = float(oldPixel[c] * (1 - a) + donorPixel[c] * a);
            continue;
        }
        const QColor old = input.pixelColor(p), filled = input.pixelColor(donor);
        highDepth.setPixelColor(p, QColor::fromRgbF(old.redF() * (1 - a) + filled.redF() * a,
                                                    old.greenF() * (1 - a) + filled.greenF() * a,
                                                    old.blueF() * (1 - a) + filled.blueF() * a,
                                                    old.alphaF() * (1 - a) + filled.alphaF() * a));
    }
    return highDepth;
}
} // namespace serika
