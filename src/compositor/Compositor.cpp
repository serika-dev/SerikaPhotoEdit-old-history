#include "../document/Document.h"
#include "../document/FillRenderer.h"
#include "../document/TextLayout.h"
#include "../io/FormatIO.h"
#include "GpuProcessor.h"
#include <QColorSpace>
#include <QFontMetricsF>
#include <QJsonArray>
#include <QLinearGradient>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

namespace serika {
QStringList blendModeNames() {
    return {"Normal",
            "Dissolve",
            "Darken",
            "Multiply",
            "Color Burn",
            "Linear Burn",
            "Darker Color",
            "Lighten",
            "Screen",
            "Color Dodge",
            "Linear Dodge (Add)",
            "Lighter Color",
            "Overlay",
            "Soft Light",
            "Hard Light",
            "Vivid Light",
            "Linear Light",
            "Pin Light",
            "Hard Mix",
            "Difference",
            "Exclusion",
            "Subtract",
            "Divide",
            "Hue",
            "Saturation",
            "Color",
            "Luminosity"};
}
QStringList adjustmentNames() {
    return {"Brightness/Contrast",
            "Levels",
            "Curves",
            "Exposure",
            "Vibrance",
            "Hue/Saturation",
            "Color Balance",
            "Black & White",
            "Photo Filter",
            "Channel Mixer",
            "Color Lookup",
            "Invert",
            "Posterize",
            "Threshold",
            "Gradient Map",
            "Selective Color",
            "Shadows/Highlights",
            "HDR Toning",
            "Desaturate",
            "Match Color",
            "Replace Color",
            "Equalize",
            "Auto Tone",
            "Auto Contrast",
            "Auto Color"};
}
namespace {
struct Pixel {
    double r = 0, g = 0, b = 0, a = 0;
};
using Triple = std::array<double, 3>;
double clamp(double x) { return std::clamp(x, 0.0, 1.0); }
double luminance(const Triple &c) { return .3 * c[0] + .59 * c[1] + .11 * c[2]; }
Triple clipColor(Triple c) {
    const double l = luminance(c), n = *std::min_element(c.begin(), c.end()),
                 x = *std::max_element(c.begin(), c.end());
    if (n < 0)
        for (double &v : c)
            v = l + (v - l) * l / (l - n);
    if (x > 1)
        for (double &v : c)
            v = l + (v - l) * (1 - l) / (x - l);
    for (double &v : c)
        v = clamp(v);
    return c;
}
Triple setLum(Triple c, double l) {
    const double d = l - luminance(c);
    for (double &v : c)
        v += d;
    return clipColor(c);
}
double saturation(const Triple &c) {
    return *std::max_element(c.begin(), c.end()) - *std::min_element(c.begin(), c.end());
}
Triple setSat(Triple c, double s) {
    std::array<int, 3> order{0, 1, 2};
    std::sort(order.begin(), order.end(), [&](int a, int b) { return c[a] < c[b]; });
    const int lo = order[0], mid = order[1], hi = order[2];
    if (c[hi] > c[lo]) {
        c[mid] = (c[mid] - c[lo]) * s / (c[hi] - c[lo]);
        c[hi] = s;
    } else
        c[mid] = c[hi] = 0;
    c[lo] = 0;
    return c;
}
double burn(double b, double s) { return s <= 0 ? 0 : 1 - std::min(1.0, (1 - b) / s); }
double dodge(double b, double s) { return s >= 1 ? 1 : std::min(1.0, b / (1 - s)); }
double vivid(double b, double s) { return s < .5 ? burn(b, 2 * s) : dodge(b, 2 * s - 1); }
double channelBlend(double b, double s, const QString &mode) {
    // Historical image-editor formulas operate on gamma-encoded RGB. The optional
    // linear mode converts both inputs to linear sRGB before evaluating these formulas.
    if (mode == "Darken")
        return std::min(b, s);
    if (mode == "Multiply")
        return b * s;
    if (mode == "Color Burn")
        return burn(b, s);
    if (mode == "Linear Burn")
        return std::max(0.0, b + s - 1);
    if (mode == "Lighten")
        return std::max(b, s);
    if (mode == "Screen")
        return b + s - b * s;
    if (mode == "Color Dodge")
        return dodge(b, s);
    if (mode == "Linear Dodge (Add)" || mode == "Linear Dodge" || mode == "Add")
        return std::min(1.0, b + s);
    if (mode == "Overlay")
        return b < .5 ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    if (mode == "Hard Light")
        return s < .5 ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    if (mode == "Soft Light") {
        const double d = b <= .25 ? ((16 * b - 12) * b + 4) * b : std::sqrt(b);
        return s <= .5 ? b - (1 - 2 * s) * b * (1 - b) : b + (2 * s - 1) * (d - b);
    }
    if (mode == "Vivid Light")
        return vivid(b, s);
    if (mode == "Linear Light")
        return clamp(b + 2 * s - 1);
    if (mode == "Pin Light")
        return s < .5 ? std::min(b, 2 * s) : std::max(b, 2 * s - 1);
    if (mode == "Hard Mix")
        return vivid(b, s) < .5 ? 0 : 1;
    if (mode == "Difference")
        return std::abs(b - s);
    if (mode == "Exclusion")
        return b + s - 2 * b * s;
    if (mode == "Subtract")
        return std::max(0.0, b - s);
    if (mode == "Divide")
        return s <= 0 ? 1 : std::min(1.0, b / s);
    return s;
}
Triple blended(Triple b, Triple s, const QString &mode) {
    if (mode == "Darker Color")
        return b[0] + b[1] + b[2] <= s[0] + s[1] + s[2] ? b : s;
    if (mode == "Lighter Color")
        return b[0] + b[1] + b[2] >= s[0] + s[1] + s[2] ? b : s;
    if (mode == "Hue")
        return setLum(setSat(s, saturation(b)), luminance(b));
    if (mode == "Saturation")
        return setLum(setSat(b, saturation(s)), luminance(b));
    if (mode == "Color")
        return setLum(s, luminance(b));
    if (mode == "Luminosity")
        return setLum(b, luminance(s));
    for (int i = 0; i < 3; ++i)
        b[i] = channelBlend(b[i], s[i], mode);
    return b;
}
double linearize(double x) { return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4); }
double encode(double x) { return x <= .0031308 ? 12.92 * x : 1.055 * std::pow(x, 1.0 / 2.4) - .055; }
Pixel readPixel(const QImage &image, int x, int y) {
    if (image.format() == QImage::Format_RGBA64) {
        const auto p = reinterpret_cast<const QRgba64 *>(image.constScanLine(y))[x];
        return {p.red() / 65535., p.green() / 65535., p.blue() / 65535., p.alpha() / 65535.};
    }
    if (image.format() == QImage::Format_RGBA32FPx4) {
        const auto *p = reinterpret_cast<const float *>(image.constScanLine(y)) + 4 * x;
        return {p[0], p[1], p[2], p[3]};
    }
    const auto *p = image.constScanLine(y) + 4 * x;
    return {p[0] / 255., p[1] / 255., p[2] / 255., p[3] / 255.};
}
void writePixel(QImage &image, int x, int y, const Pixel &p) {
    if (image.format() == QImage::Format_RGBA64) {
        auto *line = reinterpret_cast<QRgba64 *>(image.scanLine(y));
        line[x] = QRgba64::fromRgba64(qRound(clamp(p.r) * 65535), qRound(clamp(p.g) * 65535),
                                      qRound(clamp(p.b) * 65535), qRound(clamp(p.a) * 65535));
    } else if (image.format() == QImage::Format_RGBA32FPx4) {
        auto *line = reinterpret_cast<float *>(image.scanLine(y)) + 4 * x;
        line[0] = float(p.r);
        line[1] = float(p.g);
        line[2] = float(p.b);
        line[3] = float(clamp(p.a));
    } else {
        auto *line = image.scanLine(y) + 4 * x;
        line[0] = uchar(qRound(clamp(p.r) * 255));
        line[1] = uchar(qRound(clamp(p.g) * 255));
        line[2] = uchar(qRound(clamp(p.b) * 255));
        line[3] = uchar(qRound(clamp(p.a) * 255));
    }
}
QImage::Format nativeFormat(int depth) {
    return depth == 16   ? QImage::Format_RGBA64
           : depth == 32 ? QImage::Format_RGBA32FPx4
                         : QImage::Format_RGBA8888;
}
QImage normalImage(const QImage &image) {
    return image.convertToFormat(image.depth() > 32
                                     ? (image.format() == QImage::Format_RGBA32FPx4 ||
                                                image.format() == QImage::Format_RGBA32FPx4_Premultiplied
                                            ? QImage::Format_RGBA32FPx4
                                            : QImage::Format_RGBA64)
                                     : QImage::Format_RGBA8888);
}
QImage blank(QSize size, QImage::Format format) {
    QImage image(size, format);
    image.fill(Qt::transparent);
    return image;
}
void placeImage(QImage &target, const QImage &source, QPointF offset) {
    if (source.format() == target.format() && offset == QPointF(offset.toPoint())) {
        const QPoint origin = offset.toPoint();
        const QRect overlap = QRect(origin, source.size()).intersected(target.rect());
        const int bytes = target.depth() / 8;
        for (int y = overlap.top(); y <= overlap.bottom(); ++y)
            std::memcpy(target.scanLine(y) + overlap.left() * bytes,
                        source.constScanLine(y - origin.y()) + (overlap.left() - origin.x()) * bytes,
                        overlap.width() * bytes);
    } else {
        QPainter painter(&target);
        painter.drawImage(offset, source);
    }
}
double param(const QJsonObject &p, const QString &key, double fallback) {
    return p.value(key).toDouble(fallback);
}
double percent(const QJsonObject &p, const QString &key, double fallback) {
    double v = param(p, key, fallback);
    return v > 1 ? v / 100 : v;
}
QColor colorParam(const QJsonObject &p, const QString &key, const QColor &fallback) {
    QString value = p.value(key).toString();
    if (value.isEmpty() && (key == "start" || key == "end"))
        value = p.value(key + "Color").toString();
    QColor c(value);
    return c.isValid() ? c : fallback;
}
Triple hsv(Triple c) {
    const double hi = *std::max_element(c.begin(), c.end()), lo = *std::min_element(c.begin(), c.end()),
                 d = hi - lo;
    double h = 0;
    if (d > 0) {
        if (hi == c[0])
            h = (c[1] - c[2]) / d;
        else if (hi == c[1])
            h = 2 + (c[2] - c[0]) / d;
        else
            h = 4 + (c[0] - c[1]) / d;
        h /= 6;
        if (h < 0)
            h += 1;
    }
    return {h, hi <= 0 ? 0 : d / hi, hi};
}
Triple rgbFromHsv(Triple c) {
    const double h = c[0] - std::floor(c[0]), v = c[2], s = clamp(c[1]);
    const double f = h * 6 - std::floor(h * 6), p = v * (1 - s), q = v * (1 - s * f),
                 t = v * (1 - s * (1 - f));
    switch (int(h * 6) % 6) {
    case 0:
        return {v, t, p};
    case 1:
        return {q, v, p};
    case 2:
        return {p, v, t};
    case 3:
        return {p, q, v};
    case 4:
        return {t, p, v};
    default:
        return {v, p, q};
    }
}
std::vector<QPointF> curvePoints(const QJsonValue &value) {
    std::vector<QPointF> points;
    for (const auto &v : value.toArray()) {
        const auto p = v.toArray();
        if (p.size() >= 2)
            points.emplace_back(clamp(p[0].toDouble() / 255), clamp(p[1].toDouble() / 255));
    }
    if (points.size() < 2)
        return {{0, 0}, {1, 1}};
    std::sort(points.begin(), points.end(), [](auto a, auto b) { return a.x() < b.x(); });
    points.erase(std::unique(points.begin(), points.end(), [](auto a, auto b) { return a.x() == b.x(); }),
                 points.end());
    return points;
}
double curve(double x, const std::vector<QPointF> &points) {
    if (x <= points.front().x())
        return points.front().y();
    if (x >= points.back().x())
        return points.back().y();
    int i = 1;
    while (i < int(points.size()) && points[i].x() < x)
        ++i;
    const auto a = points[i - 1], b = points[i];
    const double dx = b.x() - a.x(), t = (x - a.x()) / dx, delta = (b.y() - a.y()) / dx;
    const double prev = i > 1 ? (a.y() - points[i - 2].y()) / (a.x() - points[i - 2].x()) : delta;
    const double next =
        i + 1 < int(points.size()) ? (points[i + 1].y() - b.y()) / (points[i + 1].x() - b.x()) : delta;
    const double m0 = prev * delta <= 0 ? 0 : 2 * prev * delta / (prev + delta),
                 m1 = next * delta <= 0 ? 0 : 2 * next * delta / (next + delta);
    return clamp((2 * t * t * t - 3 * t * t + 1) * a.y() + (t * t * t - 2 * t * t + t) * dx * m0 +
                 (-2 * t * t * t + 3 * t * t) * b.y() + (t * t * t - t * t) * dx * m1);
}
double maskValue(const QImage &mask, int x, int y) { return maskSample(mask, x, y); }
QImage alphaMask(const QImage &image, bool preserveDepth = false) {
    QImage mask = makeMask(image.size(), preserveDepth ? (image.format() == QImage::Format_RGBA32FPx4 ? 32
                                                          : image.depth() > 32                        ? 16
                                                                                                      : 8)
                                                       : 8);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const double a = clamp(readPixel(image, x, y).a);
            setMaskSample(mask, x, y, a);
        }
    return mask;
}
QImage boxBlurMask(const QImage &input, int radius) {
    if (radius <= 0)
        return input;
    radius = std::min(radius, 250);
    QImage horizontal(input.size(), QImage::Format_Grayscale8),
        result(input.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < input.height(); ++y) {
        int sum = 0;
        for (int x = -radius; x <= radius; ++x)
            if (x >= 0 && x < input.width())
                sum += input.constScanLine(y)[x];
        for (int x = 0; x < input.width(); ++x) {
            horizontal.scanLine(y)[x] = uchar(sum / (2 * radius + 1));
            if (x - radius >= 0)
                sum -= input.constScanLine(y)[x - radius];
            if (x + radius + 1 < input.width())
                sum += input.constScanLine(y)[x + radius + 1];
        }
    }
    for (int x = 0; x < input.width(); ++x) {
        int sum = 0;
        for (int y = -radius; y <= radius; ++y)
            if (y >= 0 && y < input.height())
                sum += horizontal.constScanLine(y)[x];
        for (int y = 0; y < input.height(); ++y) {
            result.scanLine(y)[x] = uchar(sum / (2 * radius + 1));
            if (y - radius >= 0)
                sum -= horizontal.constScanLine(y - radius)[x];
            if (y + radius + 1 < input.height())
                sum += horizontal.constScanLine(y + radius + 1)[x];
        }
    }
    return result;
}
QImage gaussianMask(QImage mask, int radius) {
    int r = std::max(1, int(radius * .58));
    for (int i = 0; i < 3 && radius > 0; ++i)
        mask = boxBlurMask(mask, r);
    return mask;
}
std::vector<float> distanceToAlphaClass(const QImage &alpha, bool foreground) {
    const int width = alpha.width(), height = alpha.height(), extent = std::max(width, height);
    std::vector<float> result(size_t(width) * height);
    std::vector<double> f(extent), d(extent), boundaries(extent + 1);
    std::vector<int> sites(extent);
    auto transform = [&](int n) {
        int k = 0;
        sites[0] = 0;
        boundaries[0] = -1e30;
        boundaries[1] = 1e30;
        for (int q = 1; q < n; ++q) {
            double s = ((f[q] + double(q) * q) - (f[sites[k]] + double(sites[k]) * sites[k])) /
                       (2. * (q - sites[k]));
            while (s <= boundaries[k]) {
                --k;
                s = ((f[q] + double(q) * q) - (f[sites[k]] + double(sites[k]) * sites[k])) /
                    (2. * (q - sites[k]));
            }
            ++k;
            sites[k] = q;
            boundaries[k] = s;
            boundaries[k + 1] = 1e30;
        }
        k = 0;
        for (int q = 0; q < n; ++q) {
            while (boundaries[k + 1] < q)
                ++k;
            const double distance = q - sites[k];
            d[q] = distance * distance + f[sites[k]];
        }
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x)
            f[x] = (bool(alpha.constScanLine(y)[x]) == foreground) ? 0 : 1e12;
        transform(width);
        for (int x = 0; x < width; ++x)
            result[size_t(y) * width + x] = float(d[x]);
    }
    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < height; ++y)
            f[y] = result[size_t(y) * width + x];
        transform(height);
        for (int y = 0; y < height; ++y)
            result[size_t(y) * width + x] = float(d[y]);
    }
    return result;
}
void compositeInto(QImage &dest, const QImage &source, const QString &mode, double opacity = 1,
                   bool linear = false, const QImage &clip = {}) {
    if (source.isNull())
        return;
    if ((mode == "Normal" || mode == "Pass Through") && !linear && clip.isNull() &&
        (dest.depth() <= 32 || opacity == 1)) {
        QPainter p(&dest);
        p.setOpacity(clamp(opacity));
        p.drawImage(0, 0, source);
        return;
    }
    for (int y = 0; y < dest.height(); ++y)
        for (int x = 0; x < dest.width(); ++x) {
            Pixel s = readPixel(source, x, y);
            s.a *= clamp(opacity);
            if (!clip.isNull())
                s.a *= maskValue(clip, x, y);
            if (s.a <= 0)
                continue;
            if (mode == "Dissolve") {
                // Stable document-coordinate noise avoids shimmering during pan, redraw and export.
                quint32 hash = quint32(x) * 0x9e3779b9u ^ quint32(y) * 0x85ebca6bu;
                hash ^= hash >> 16;
                hash *= 0x7feb352du;
                hash ^= hash >> 15;
                if (double(hash) / 4294967295.0 > s.a)
                    continue;
                s.a = 1;
            }
            Pixel b = readPixel(dest, x, y);
            Triple bc{b.r, b.g, b.b}, sc{s.r, s.g, s.b};
            if (linear) {
                for (auto &v : bc)
                    v = linearize(v);
                for (auto &v : sc)
                    v = linearize(v);
            }
            Triple mix = blended(bc, sc, mode);
            const double a = s.a + b.a * (1 - s.a);
            Triple out;
            for (int i = 0; i < 3; ++i) {
                out[i] = a > 0 ? ((1 - s.a) * b.a * bc[i] + s.a * ((1 - b.a) * sc[i] + b.a * mix[i])) / a : 0;
                if (linear)
                    out[i] = encode(out[i]);
            }
            writePixel(dest, x, y, {out[0], out[1], out[2], a});
        }
}
void clipCompositeInto(QImage &base, const QImage &source, const QString &mode, double opacity, bool linear) {
    // Clipping layers replace color within the base layer's coverage. They must not
    // increase its alpha; multiplying alpha and doing source-over would do exactly that.
    for (int y = 0; y < base.height(); ++y)
        for (int x = 0; x < base.width(); ++x) {
            const Pixel b = readPixel(base, x, y);
            if (b.a <= 0)
                continue;
            Pixel s = readPixel(source, x, y);
            double weight = clamp(s.a * opacity);
            if (weight <= 0)
                continue;
            if (mode == "Dissolve") {
                quint32 hash = quint32(x) * 0x9e3779b9u ^ quint32(y) * 0x85ebca6bu;
                hash ^= hash >> 16;
                hash *= 0x7feb352du;
                hash ^= hash >> 15;
                if (double(hash) / 4294967295.0 > weight)
                    continue;
                weight = 1;
            }
            Triple bc{b.r, b.g, b.b}, sc{s.r, s.g, s.b};
            if (linear) {
                for (auto &v : bc)
                    v = linearize(v);
                for (auto &v : sc)
                    v = linearize(v);
            }
            const Triple blend = blended(bc, sc, mode);
            Triple c;
            for (int i = 0; i < 3; ++i) {
                c[i] = bc[i] + (blend[i] - bc[i]) * weight;
                if (linear)
                    c[i] = encode(c[i]);
            }
            writePixel(base, x, y, {c[0], c[1], c[2], b.a});
        }
}
QImage effectColor(QSize size, QImage::Format format, const QImage &mask, const QColor &color, double opacity,
                   QPoint offset = {}) {
    QImage image = blank(size, format);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x) {
            const QPoint p = QPoint(x, y) - offset;
            const double a = mask.rect().contains(p) ? mask.constScanLine(p.y())[p.x()] / 255. : 0;
            writePixel(image, x, y,
                       {color.redF(), color.greenF(), color.blueF(), a * opacity * color.alphaF()});
        }
    return image;
}
QImage applyEffects(QImage image, const QJsonObject &effects, double fill = 1) {
    const QImage alpha = effects.isEmpty() ? QImage() : alphaMask(image);
    if (fill != 1)
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                Pixel pixel = readPixel(image, x, y);
                pixel.a *= clamp(fill);
                writePixel(image, x, y, pixel);
            }
    if (effects.isEmpty())
        return image;
    QImage below = blank(image.size(), image.format());
    for (const QString &key : {QString("dropShadow"), QString("outerGlow")})
        if (effects.contains(key)) {
            const auto p = effects.value(key).toObject();
            if (p.value("enabled").toBool(true)) {
                const int size = qRound(param(p, "size", key == "dropShadow" ? 8 : 12));
                QImage mask = gaussianMask(alpha, size);
                const double angle = param(p, "angle", 120) * 3.141592653589793 / 180,
                             dist = key == "dropShadow" ? param(p, "distance", 5) : 0;
                const QColor color =
                    colorParam(p, "color", key == "dropShadow" ? QColor(Qt::black) : QColor(255, 225, 150));
                const QImage effect =
                    effectColor(image.size(), image.format(), mask, color, percent(p, "opacity", .65),
                                QPoint(qRound(-std::cos(angle) * dist), qRound(std::sin(angle) * dist)));
                compositeInto(below, effect,
                              p.value("blendMode").toString(key == "dropShadow" ? "Multiply" : "Screen"));
            }
        }
    for (const QString &key :
         {QString("colorOverlay"), QString("gradientOverlay"), QString("patternOverlay")})
        if (effects.contains(key)) {
            const auto p = effects.value(key).toObject();
            if (!p.value("enabled").toBool(true))
                continue;
            QImage overlay = blank(image.size(), image.format());
            if (key == "colorOverlay")
                overlay =
                    effectColor(image.size(), image.format(), alpha, colorParam(p, "color", Qt::red), 1);
            else {
                QPainter painter(&overlay);
                if (key == "gradientOverlay") {
                    QLinearGradient g(0, 0, image.width(), image.height());
                    g.setColorAt(0, colorParam(p, "start", Qt::black));
                    g.setColorAt(1, colorParam(p, "end", Qt::white));
                    painter.fillRect(overlay.rect(), g);
                } else {
                    const int tile = std::max(2, qRound(param(p, "size", 16)));
                    const QColor a = colorParam(p, "color", Qt::white),
                                 b = colorParam(p, "alternate", Qt::black);
                    for (int y = 0; y < image.height(); y += tile)
                        for (int x = 0; x < image.width(); x += tile)
                            painter.fillRect(QRect(x, y, tile, tile), ((x / tile + y / tile) & 1) ? a : b);
                }
                painter.end();
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x) {
                        Pixel c = readPixel(overlay, x, y);
                        c.a *= alpha.constScanLine(y)[x] / 255.;
                        writePixel(overlay, x, y, c);
                    }
            }
            compositeInto(image, overlay, p.value("blendMode").toString("Normal"), percent(p, "opacity", 1));
        }
    for (const QString &key : {QString("innerShadow"), QString("innerGlow"), QString("bevelEmboss"),
                               QString("satin"), QString("stroke")})
        if (effects.contains(key)) {
            const auto p = effects.value(key).toObject();
            if (!p.value("enabled").toBool(true))
                continue;
            QImage mask = gaussianMask(alpha, qRound(param(p, "size", 5)));
            QImage effect = blank(image.size(), image.format());
            const QColor color =
                colorParam(p, "color", key == "innerGlow" ? QColor(Qt::white) : QColor(Qt::black));
            const int radius = std::clamp(qRound(param(p, "size", 3)), 1, 1000);
            const bool insideStroke = p.value("position").toString("outside") == "inside";
            const std::vector<float> strokeDistances =
                key == "stroke" ? distanceToAlphaClass(alpha, !insideStroke) : std::vector<float>();
            const double angle = param(p, "angle", 120) * 3.141592653589793 / 180;
            const QPoint offset(qRound(-std::cos(angle) * param(p, "distance", 3)),
                                qRound(std::sin(angle) * param(p, "distance", 3)));
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const double a = alpha.constScanLine(y)[x] / 255.;
                    double e = 0;
                    QColor tint = color;
                    if (key == "innerGlow")
                        e = a * (1 - mask.constScanLine(y)[x] / 255.);
                    else if (key == "innerShadow") {
                        const QPoint sample = QPoint(x, y) - offset;
                        e = a * (1 - (mask.rect().contains(sample)
                                          ? mask.constScanLine(sample.y())[sample.x()] / 255.
                                          : 0));
                    } else if (key == "stroke") {
                        double distance = std::sqrt(strokeDistances[size_t(y) * image.width() + x]);
                        if (insideStroke)
                            distance = std::min(distance, double(std::min({x + 1, y + 1, image.width() - x,
                                                                           image.height() - y})));
                        e = clamp(radius + .5 - distance) * (insideStroke ? a : 1 - a);
                    } else if (key == "bevelEmboss") {
                        const int left = mask.constScanLine(y)[std::max(0, x - 1)],
                                  right = mask.constScanLine(y)[std::min(mask.width() - 1, x + 1)];
                        const int up = mask.constScanLine(std::max(0, y - 1))[x],
                                  down = mask.constScanLine(std::min(mask.height() - 1, y + 1))[x];
                        const double gradient =
                            ((left - right) * std::cos(angle) + (up - down) * std::sin(angle)) / 255.;
                        e = std::abs(gradient) * a * param(p, "depth", 3);
                        tint = gradient > 0 ? QColor(Qt::white) : QColor(Qt::black);
                    } else {
                        const QPoint one = QPoint(x, y) - offset, two = QPoint(x, y) + offset;
                        const int v1 = mask.rect().contains(one) ? mask.constScanLine(one.y())[one.x()] : 0,
                                  v2 = mask.rect().contains(two) ? mask.constScanLine(two.y())[two.x()] : 0;
                        e = a * std::abs(v1 - v2) / 255.;
                    }
                    writePixel(
                        effect, x, y,
                        {tint.redF(), tint.greenF(), tint.blueF(), clamp(e) * percent(p, "opacity", .75)});
                }
            compositeInto(image, effect, p.value("blendMode").toString("Normal"));
        }
    compositeInto(below, image, "Normal");
    return below;
}
void maskLayer(QImage &image, const Layer &layer, double amount = 1) {
    const QImage mask = renderedLayerMask(layer, image.size(),
                                          image.format() == QImage::Format_RGBA32FPx4 ? 32
                                          : image.depth() > 32                        ? 16
                                                                                      : 8);
    if (mask.isNull() && amount == 1)
        return;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            Pixel p = readPixel(image, x, y);
            double alpha = amount;
            if (!mask.isNull())
                alpha *= maskValue(mask, x, y);
            p.a *= alpha;
            writePixel(image, x, y, p);
        }
}
} // namespace

QColor blendColor(QColor backdrop, QColor source, const QString &mode) {
    const Triple color = blended({backdrop.redF(), backdrop.greenF(), backdrop.blueF()},
                                 {source.redF(), source.greenF(), source.blueF()}, mode);
    return QColor::fromRgbF(clamp(color[0]), clamp(color[1]), clamp(color[2]), source.alphaF());
}
QImage renderLayerImage(const Layer &layer, const QSize &canvas, int bitDepth) {
    const auto transformedContent = [&](QImage image) {
        const QJsonArray matrix = layer.parameters.value("contentTransform").toArray();
        if (matrix.size() == 9) {
            const QTransform transform(matrix[0].toDouble(), matrix[1].toDouble(), matrix[2].toDouble(),
                                       matrix[3].toDouble(), matrix[4].toDouble(), matrix[5].toDouble(),
                                       matrix[6].toDouble(), matrix[7].toDouble(), matrix[8].toDouble());
            if (transform.isInvertible())
                image = image.transformed(transform, Qt::SmoothTransformation);
        }
        return image;
    };
    if (layer.kind == LayerKind::Pixel || layer.kind == LayerKind::SmartObject) {
        QImage image = layer.pixels.image();
        if (layer.kind == LayerKind::SmartObject) {
            for (const auto &value : layer.smartFilters) {
                const QJsonObject entry = value.toObject();
                if (!entry.value("enabled").toBool(true))
                    continue;
                const QString name = entry.value("name").toString();
                const QJsonObject parameters = entry.value("parameters").toObject();
                QImage filtered =
                    (entry.value("kind").toString() == "adjustment" || adjustmentNames().contains(name))
                        ? applyAdjustment(image, name, parameters)
                        : applyFilter(image, name, parameters);
                if (filtered.isNull() || filtered.size() != image.size())
                    continue;
                filtered = normalImage(filtered).convertToFormat(image.format());
                const double opacity = clamp(entry.value("opacity").toDouble(1));
                const QString mode = entry.value("blendMode").toString("Normal");
                if (opacity == 1 && mode == "Normal")
                    image = filtered;
                else
                    for (int y = 0; y < image.height(); ++y)
                        for (int x = 0; x < image.width(); ++x) {
                            const Pixel a = readPixel(filtered, x, y), b = readPixel(image, x, y);
                            const Triple blend = blended({b.r, b.g, b.b}, {a.r, a.g, a.b}, mode);
                            writePixel(image, x, y,
                                       {b.r + (blend[0] - b.r) * opacity, b.g + (blend[1] - b.g) * opacity,
                                        b.b + (blend[2] - b.b) * opacity, b.a + (a.a - b.a) * opacity});
                        }
            }
        }
        return transformedContent(image);
    }
    QImage image = blank(canvas, nativeFormat(bitDepth));
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    if (layer.kind == LayerKind::Text) {
        paintTextLayer(p, layer, canvas);
    } else if (layer.kind == LayerKind::Shape) {
        p.setBrush(layer.color);
        p.setPen(layer.stroke.alpha() > 0 ? QPen(layer.stroke, layer.strokeWidth) : QPen(Qt::NoPen));
        p.drawPath(layer.shape);
    } else if (layer.kind == LayerKind::SolidFill)
        p.fillRect(image.rect(), layer.color);
    else if (layer.kind == LayerKind::GradientFill) {
        p.drawImage(0, 0, renderGradientFill(canvas, bitDepth, layer.parameters, layer.color));
    } else if (layer.kind == LayerKind::PatternFill)
        p.drawImage(0, 0, renderPatternFill(canvas, bitDepth, layer.parameters));
    p.end();
    return transformedContent(image);
}

QImage applyAdjustment(const QImage &source, const QString &name, const QJsonObject &parameters) {
    if (const auto gpu = GpuProcessor::instance().processAdjustment(source, name, parameters); gpu.usedGpu)
        return gpu.image;
    if (source.isNull())
        return {};
    const QImage input = normalImage(source);
    QImage result(input.size(), input.format());
    result.setColorSpace(source.colorSpace());
    std::array<std::array<quint64, 256>, 3> hist{};
    std::array<std::array<double, 256>, 3> lookup{};
    Triple minimum{1, 1, 1}, maximum{0, 0, 0}, mean{0, 0, 0};
    quint64 count = 0;
    const bool statistics = name == "Equalize" || name.startsWith("Auto ") || name == "Match Color";
    if (statistics) {
        for (int y = 0; y < input.height(); ++y)
            for (int x = 0; x < input.width(); ++x) {
                const Pixel p = readPixel(input, x, y);
                if (p.a <= 0)
                    continue;
                const Triple c{p.r, p.g, p.b};
                ++count;
                for (int i = 0; i < 3; ++i) {
                    minimum[i] = std::min(minimum[i], c[i]);
                    maximum[i] = std::max(maximum[i], c[i]);
                    mean[i] += c[i];
                    ++hist[i][std::clamp(qRound(c[i] * 255), 0, 255)];
                }
            }
        for (int i = 0; i < 3; ++i) {
            mean[i] /= std::max<quint64>(1, count);
            quint64 sum = 0, first = 0;
            for (int j = 0; j < 256; ++j) {
                sum += hist[i][j];
                if (!first && hist[i][j])
                    first = sum;
                lookup[i][j] = count > first ? double(sum - first) / double(count - first) : j / 255.;
            }
        }
        if (name == "Auto Contrast") {
            const double lo = *std::min_element(minimum.begin(), minimum.end()),
                         hi = *std::max_element(maximum.begin(), maximum.end());
            minimum = {lo, lo, lo};
            maximum = {hi, hi, hi};
        }
    }
    const auto master = curvePoints(parameters.value("points")),
               red = curvePoints(parameters.value("redPoints")),
               green = curvePoints(parameters.value("greenPoints")),
               blue = curvePoints(parameters.value("bluePoints"));
    const std::array<std::vector<QPointF>, 3> channels{red, green, blue};
    const QColor filter = colorParam(parameters, "color", QColor(236, 150, 60));
    const auto mapStops = name == "Gradient Map" ? gradientStops(parameters) : QGradientStops{};
    const QColor replace = colorParam(parameters, "source", Qt::red),
                 target = colorParam(parameters, "target", Qt::blue);
    const int cubeSize = parameters.value("cubeSize").toInt();
    const QJsonArray cube = parameters.value("cube").toArray();
    for (int y = 0; y < input.height(); ++y)
        for (int x = 0; x < input.width(); ++x) {
            Pixel p = readPixel(input, x, y);
            Triple c{p.r, p.g, p.b};
            const double lum = luminance(c);
            if (name == "Invert")
                for (double &v : c)
                    v = 1 - v;
            else if (name == "Brightness/Contrast") {
                const double brightness =
                                 param(parameters, "brightness", param(parameters, "amount", 0)) / 100.,
                             contrast = std::clamp(param(parameters, "contrast", 0) / 100., -.99, .99),
                             factor = contrast >= 0 ? 1 / (1 - contrast) : 1 + contrast;
                for (double &v : c)
                    v = (v - .5) * factor + .5 + brightness;
            } else if (name == "Levels") {
                const double lo = param(parameters, "inputBlack", 0) / 255.,
                             hi = param(parameters, "inputWhite", 255) / 255.,
                             outLo = param(parameters, "outputBlack", 0) / 255.,
                             outHi = param(parameters, "outputWhite", 255) / 255.,
                             gamma = std::max(.01, param(parameters, "gamma", 1));
                for (double &v : c)
                    v = outLo +
                        std::pow(clamp((v - lo) / std::max(.00001, hi - lo)), 1 / gamma) * (outHi - outLo);
            } else if (name == "Curves")
                for (int i = 0; i < 3; ++i)
                    c[i] = curve(curve(c[i], channels[i]), master);
            else if (name == "Exposure") {
                const double gain = std::pow(
                                 2, param(parameters, "exposure", param(parameters, "amount", 0) / 50.)),
                             offset = param(parameters, "offset", 0),
                             gamma = std::max(.01, param(parameters, "gamma", 1));
                for (double &v : c)
                    v = encode(std::pow(std::max(0.0, linearize(v) * gain + offset), 1 / gamma));
            } else if (name == "Hue/Saturation" || name == "Vibrance") {
                Triple h = hsv(c);
                h[0] += param(parameters, "hue", 0) / 360.;
                const double sat =
                    param(parameters, "saturation", name == "Vibrance" ? 0 : param(parameters, "amount", 0)) /
                    100.;
                const double vibrance =
                    param(parameters, "vibrance", name == "Vibrance" ? param(parameters, "amount", 0) : 0) /
                    100.;
                h[1] = clamp(h[1] * (1 + sat) + vibrance * (1 - h[1]) * h[1]);
                if (parameters.value("colorize").toBool()) {
                    h[0] = param(parameters, "hue", 0) / 360.;
                    h[1] = clamp(param(parameters, "saturation", 25) / 100.);
                }
                c = rgbFromHsv(h);
                const double light = param(parameters, "lightness", 0) / 100.;
                for (double &v : c)
                    v = light > 0 ? v + (1 - v) * light : v * (1 + light);
            } else if (name == "Desaturate" || name == "Black & White") {
                double value = (std::max({c[0], c[1], c[2]}) + std::min({c[0], c[1], c[2]})) * .5;
                if (name == "Black & White") {
                    const QStringList keys{"reds", "yellows", "greens", "cyans", "blues", "magentas"};
                    const std::array<double, 6> defaults{40, 60, 40, 60, 20, 80};
                    const Triple h = hsv(c);
                    const double position = h[0] * 6;
                    const int lower = int(position) % 6, upper = (lower + 1) % 6;
                    const double fraction = position - std::floor(position);
                    const double mix = (param(parameters, keys[lower], defaults[lower]) * (1 - fraction) +
                                        param(parameters, keys[upper], defaults[upper]) * fraction) /
                                       100.;
                    value = h[2] * (1 - h[1]) + h[2] * h[1] * mix;
                    if (parameters.contains("red") || parameters.contains("green") ||
                        parameters.contains("blue"))
                        value = c[0] * param(parameters, "red", 30) / 100. +
                                c[1] * param(parameters, "green", 59) / 100. +
                                c[2] * param(parameters, "blue", 11) / 100.;
                }
                c = {value, value, value};
            } else if (name == "Color Balance") {
                const double r = param(parameters, "cyanRed", param(parameters, "red", 0)) / 100.,
                             g = param(parameters, "magentaGreen", param(parameters, "green", 0)) / 100.,
                             b = param(parameters, "yellowBlue", param(parameters, "blue", 0)) / 100.;
                const QString tone = parameters.value("tone").toString("midtones");
                const double weight = tone == "shadows"      ? 1 - lum
                                      : tone == "highlights" ? lum
                                                             : 4 * lum * (1 - lum);
                c[0] += r * weight;
                c[1] += g * weight;
                c[2] += b * weight;
                if (parameters.value("preserveLuminosity").toBool(true))
                    c = setLum(c, lum);
            } else if (name == "Photo Filter") {
                const double density = percent(parameters, "density", .25);
                const Triple f{filter.redF(), filter.greenF(), filter.blueF()};
                for (int i = 0; i < 3; ++i)
                    c[i] = c[i] * (1 - density) + f[i] * density;
                if (parameters.value("preserveLuminosity").toBool(true))
                    c = setLum(c, lum);
            } else if (name == "Channel Mixer") {
                const Triple original = c;
                const QStringList keys{"red", "green", "blue"};
                for (int i = 0; i < 3; ++i) {
                    const auto weights = parameters.value(keys[i]).toArray();
                    if (weights.size() >= 3)
                        c[i] = original[0] * weights[0].toDouble() / 100 +
                               original[1] * weights[1].toDouble() / 100 +
                               original[2] * weights[2].toDouble() / 100 +
                               (weights.size() > 3 ? weights[3].toDouble() / 100 : 0);
                }
                if (parameters.value("monochrome").toBool())
                    c = {c[0], c[0], c[0]};
            } else if (name == "Posterize") {
                const int levels =
                    std::clamp(int(param(parameters, "levels", param(parameters, "amount", 4))), 2, 256);
                for (double &v : c)
                    v = std::round(v * (levels - 1)) / (levels - 1);
            } else if (name == "Threshold") {
                const double value =
                    lum >= param(parameters, "threshold", param(parameters, "amount", 128)) / 255. ? 1 : 0;
                c = {value, value, value};
            } else if (name == "Gradient Map") {
                const auto color = sampleGradient(mapStops, lum);
                c = {color.redF(), color.greenF(), color.blueF()};
                p.a *= color.alphaF();
            } else if (name == "Shadows/Highlights" || name == "HDR Toning") {
                const double shadows = param(parameters, "shadows", name == "HDR Toning" ? 25 : 0) / 100.,
                             highlights =
                                 param(parameters, "highlights", name == "HDR Toning" ? 25 : 0) / 100.;
                const double value =
                    clamp(lum + shadows * std::pow(1 - lum, 3) - highlights * std::pow(lum, 3));
                c = setLum(c, value);
                if (name == "HDR Toning") {
                    const double exposure = std::pow(2, param(parameters, "exposure", 0)),
                                 gamma = std::max(.01, param(parameters, "gamma", 1));
                    for (double &v : c)
                        v = std::pow(clamp(v * exposure), 1 / gamma);
                }
            } else if (name == "Selective Color") {
                Triple delta{0, 0, 0};
                const QStringList ranges{"reds",     "yellows", "greens",   "cyans", "blues",
                                         "magentas", "whites",  "neutrals", "blacks"};
                const Triple h = hsv(c);
                for (int j = 0; j < ranges.size(); ++j) {
                    const auto weights = parameters.value(ranges[j]).toObject();
                    if (weights.isEmpty())
                        continue;
                    double weight = 0;
                    if (j < 6) {
                        const double distance = std::abs(h[0] - j / 6.);
                        weight = std::max(0.0, 1 - std::min(distance, 1 - distance) * 6) * h[1];
                    } else if (j == 6)
                        weight = std::max(0.0, (lum - .5) * 2);
                    else if (j == 7)
                        weight = 1 - std::abs(lum - .5) * 2;
                    else
                        weight = std::max(0.0, (.5 - lum) * 2);
                    const double black = param(weights, "black", 0) / 100.;
                    delta[0] -= weight * (param(weights, "cyan", 0) / 100. + black);
                    delta[1] -= weight * (param(weights, "magenta", 0) / 100. + black);
                    delta[2] -= weight * (param(weights, "yellow", 0) / 100. + black);
                }
                for (int i = 0; i < 3; ++i)
                    c[i] += delta[i] * (parameters.value("absolute").toBool() ? 1 : c[i]);
            } else if (name == "Equalize")
                for (int i = 0; i < 3; ++i)
                    c[i] = lookup[i][std::clamp(qRound(c[i] * 255), 0, 255)];
            else if (name.startsWith("Auto ")) {
                for (int i = 0; i < 3; ++i)
                    if (maximum[i] > minimum[i])
                        c[i] = (c[i] - minimum[i]) / (maximum[i] - minimum[i]);
                if (name == "Auto Color") {
                    const double meanLum = luminance(mean);
                    for (int i = 0; i < 3; ++i)
                        c[i] += meanLum - mean[i];
                }
            } else if (name == "Match Color") {
                const auto targetMean = parameters.value("targetMean").toArray();
                const double saturation = param(parameters, "saturation", 100) / 100.,
                             gain = param(parameters, "luminance", 100) / 100.;
                for (int i = 0; i < 3; ++i)
                    c[i] = ((c[i] - lum) * saturation + lum) * gain +
                           (targetMean.size() == 3 ? targetMean[i].toDouble() - mean[i] : 0);
            } else if (name == "Replace Color") {
                const double distance =
                    std::sqrt(std::pow(c[0] - replace.redF(), 2) + std::pow(c[1] - replace.greenF(), 2) +
                              std::pow(c[2] - replace.blueF(), 2));
                const double tolerance = std::max(.0001, param(parameters, "tolerance", .25));
                const double weight = clamp((tolerance - distance) / (tolerance * .25));
                const Triple targetHsv = hsv({target.redF(), target.greenF(), target.blueF()});
                Triple h = hsv(c);
                h[0] = h[0] * (1 - weight) + targetHsv[0] * weight;
                h[1] = h[1] * (1 - weight) + targetHsv[1] * weight;
                c = rgbFromHsv(h);
            } else if (name == "Color Lookup" && cubeSize >= 2 &&
                       cube.size() == cubeSize * cubeSize * cubeSize) {
                std::array<int, 3> lo, hi;
                Triple t;
                for (int i = 0; i < 3; ++i) {
                    const double pos = clamp(c[i]) * (cubeSize - 1);
                    lo[i] = int(pos);
                    hi[i] = std::min(lo[i] + 1, cubeSize - 1);
                    t[i] = pos - lo[i];
                }
                Triple mixed{0, 0, 0};
                for (int corner = 0; corner < 8; ++corner) {
                    const int rr = corner & 1 ? hi[0] : lo[0], gg = corner & 2 ? hi[1] : lo[1],
                              bb = corner & 4 ? hi[2] : lo[2];
                    const double weight = (corner & 1 ? t[0] : 1 - t[0]) * (corner & 2 ? t[1] : 1 - t[1]) *
                                          (corner & 4 ? t[2] : 1 - t[2]);
                    const auto entry = cube[rr + cubeSize * (gg + cubeSize * bb)].toArray();
                    for (int i = 0; i < 3 && i < entry.size(); ++i)
                        mixed[i] += entry[i].toDouble() * weight;
                }
                c = mixed;
            }
            if (input.format() == QImage::Format_RGBA32FPx4 && name == "Exposure")
                writePixel(result, x, y, {c[0], c[1], c[2], p.a});
            else
                writePixel(result, x, y, {clamp(c[0]), clamp(c[1]), clamp(c[2]), p.a});
        }
    return result;
}

QImage compositeDocument(const DocumentState &state, bool linear) {
    const auto format = nativeFormat(state.bitDepth);
    QImage canvas = blank(state.size, format);
    if (canvas.isNull())
        return canvas;
    const QColorSpace colorSpace = state.iccProfile.isEmpty() ? QColorSpace(QColorSpace::SRgb)
                                                              : QColorSpace::fromIccProfile(state.iccProfile);
    if (colorSpace.isValid())
        canvas.setColorSpace(colorSpace);
    QHash<quint64, QVector<int>> children;
    QSet<quint64> groupIds;
    QHash<quint64, const Layer *> byId;
    const bool hasClipping = std::any_of(state.layers.cbegin(), state.layers.cend(),
                                         [](const Layer &layer) { return layer.clipped; });
    for (const auto &l : state.layers)
        if (l.kind == LayerKind::Group || l.kind == LayerKind::Artboard)
            groupIds.insert(l.id);
    for (const auto &l : state.layers)
        byId.insert(l.id, &l);
    for (int i = 0; i < state.layers.size(); ++i) {
        const auto &l = state.layers[i];
        children[groupIds.contains(l.parentId) && l.parentId != l.id ? l.parentId : 0].append(i);
    }
    QSet<quint64> visiting;
    auto withGroupOffset = [&](Layer layer) {
        QSet<quint64> ancestors{layer.id};
        quint64 ancestor = layer.parentId;
        while (ancestor && byId.contains(ancestor) && !ancestors.contains(ancestor)) {
            ancestors.insert(ancestor);
            const Layer *groupLayer = byId.value(ancestor);
            layer.offset += groupLayer->offset;
            ancestor = groupLayer->parentId;
        }
        return layer;
    };
    std::function<void(quint64, QImage &)> render;
    render = [&](quint64 parent, QImage &backdrop) {
        QImage clip;
        const QVector<int> siblings = children.value(parent);
        QSet<int> consumed;
        for (int position = 0; position < siblings.size(); ++position) {
            const int index = siblings[position];
            if (consumed.contains(index))
                continue;
            Layer l = withGroupOffset(state.layers[index]);
            if (!l.visible) {
                if (!l.clipped && hasClipping) {
                    clip = QImage(state.size, QImage::Format_Grayscale8);
                    clip.fill(0);
                }
                continue;
            }
            if (l.clipped && clip.isNull())
                continue;
            if (l.kind == LayerKind::Adjustment) {
                QImage adjusted = applyAdjustment(backdrop, l.adjustment, l.parameters);
                const QImage mask = renderedLayerMask(l, state.size, state.bitDepth);
                const double opacity = clamp(l.opacity * l.fill);
                for (int y = 0; y < backdrop.height(); ++y)
                    for (int x = 0; x < backdrop.width(); ++x) {
                        Pixel a = readPixel(adjusted, x, y), b = readPixel(backdrop, x, y);
                        double weight = opacity;
                        if (!mask.isNull())
                            weight *= maskValue(mask, x, y);
                        if (l.clipped)
                            weight *= maskValue(clip, x, y);
                        const Triple blend = blended({b.r, b.g, b.b}, {a.r, a.g, a.b}, l.blendMode);
                        writePixel(backdrop, x, y,
                                   {b.r + (blend[0] - b.r) * weight, b.g + (blend[1] - b.g) * weight,
                                    b.b + (blend[2] - b.b) * weight, b.a});
                    }
                continue;
            }
            // Plain layers are painted straight onto the backdrop. The general path below would
            // expand them to a full-canvas image, copy that, and finish with the same QPainter
            // draw, so a small rectangle cost as much as a full-page photo. Tiles hold the same
            // straight-alpha pixels, so the result is identical.
            const bool rasterMask = l.maskEnabled && !l.mask.isNull();
            const bool plain =
                l.kind == LayerKind::Pixel && l.blendMode == "Normal" && !linear && !hasClipping &&
                l.effects.isEmpty() && l.fill == 1 && (l.vectorMask.isEmpty() || !l.vectorMaskEnabled) &&
                !l.parameters.contains("contentTransform") && l.offset == QPointF(l.offset.toPoint()) &&
                (backdrop.depth() <= 32 || l.opacity == 1);
            const QPoint origin = l.offset.toPoint();
            // Unrotated text renders into its own bounds (grown by a margin for antialiasing and
            // strokes) rather than a full-canvas image. The shift is whole pixels, so glyphs
            // rasterise exactly as before.
            const QJsonObject typography = l.parameters.value("typography").toObject();
            if (l.kind == LayerKind::Text && l.blendMode == "Normal" && !linear && !hasClipping &&
                l.effects.isEmpty() && l.fill == 1 && !rasterMask &&
                (l.vectorMask.isEmpty() || !l.vectorMaskEnabled) &&
                !l.parameters.contains("contentTransform") &&
                l.parameters.value("rotation").toDouble() == 0 && !l.parameters.value("vertical").toBool() &&
                typography.value("path").toArray().isEmpty() && l.offset == QPointF(origin) &&
                (backdrop.depth() <= 32 || l.opacity == 1)) {
                // Text is laid out in layer-local coordinates on a canvas-sized surface and then
                // shifted by the layer offset, so crop and place exactly like that.
                const int margin = 8 + int(std::ceil(l.strokeWidth));
                const QRect local = textLayerBounds(l, state.size)
                                        .toAlignedRect()
                                        .adjusted(-margin, -margin, margin, margin)
                                        .intersected(QRect(QPoint(), state.size));
                if (local.isEmpty())
                    continue;
                QImage text = blank(local.size(), format);
                {
                    QPainter p(&text);
                    p.setRenderHint(QPainter::Antialiasing);
                    p.setRenderHint(QPainter::TextAntialiasing);
                    p.translate(-local.topLeft());
                    paintTextLayer(p, l, state.size);
                }
                QPainter painter(&backdrop);
                painter.setOpacity(clamp(l.opacity));
                painter.drawImage(origin + local.topLeft(), text);
                continue;
            }
            if (plain && !rasterMask) {
                QPainter painter(&backdrop);
                painter.setOpacity(clamp(l.opacity));
                painter.setClipRect(QRect(origin, l.pixels.size));
                for (auto it = l.pixels.tiles.cbegin(); it != l.pixels.tiles.cend(); ++it)
                    painter.drawImage(origin + QPoint(int(it.key() >> 32), int(quint32(it.key()))) *
                                                   TileImage::TileSize,
                                      it.value());
                continue;
            }
            // A linked raster mask at an integer offset samples exactly one mask pixel per layer pixel,
            // so it can be applied in the layer's own (usually small) rectangle instead of across
            // the whole canvas. 8-bit only, matching maskLayer()'s rounding.
            if (plain && rasterMask && l.maskLinked && l.pixels.format == QImage::Format_RGBA8888) {
                QImage image = l.pixels.image();
                const QImage coverage = refineMask(normalizeMask(l.mask, 8), {{"feather", l.maskFeather}});
                const double density = std::clamp(l.maskDensity, 0., 1.);
                for (int y = 0; y < image.height(); ++y) {
                    uchar *line = image.scanLine(y);
                    for (int x = 0; x < image.width(); ++x) {
                        uchar &alpha = line[x * 4 + 3];
                        if (alpha)
                            alpha = uchar(qRound(
                                clamp(alpha / 255. * (1 - density * (1 - maskSample(coverage, x, y)))) *
                                255));
                    }
                }
                QPainter painter(&backdrop);
                painter.setOpacity(clamp(l.opacity));
                painter.drawImage(origin, image);
                continue;
            }
            QImage placed = blank(state.size, format);
            const bool group = l.kind == LayerKind::Group || l.kind == LayerKind::Artboard;
            if (group) {
                if (visiting.contains(l.id))
                    continue;
                visiting.insert(l.id);
                if (l.kind == LayerKind::Group && l.blendMode == "Pass Through" && l.effects.isEmpty() &&
                    !(position + 1 < siblings.size() && state.layers[siblings[position + 1]].clipped)) {
                    QImage before = backdrop, after = backdrop;
                    render(l.id, after);
                    // Interpolate premultiplied results so pass-through adjustments and child
                    // blends see the actual backdrop, including at fractional group opacity.
                    const QImage mask = renderedLayerMask(l, state.size, state.bitDepth);
                    // At full strength the interpolation below returns `after` itself (apart from
                    // the unused colour of fully transparent pixels), so skip the per-pixel pass.
                    if (clamp(l.opacity * l.fill) == 1 && mask.isNull() && !l.clipped)
                        backdrop = after;
                    else
                        for (int y = 0; y < backdrop.height(); ++y)
                            for (int x = 0; x < backdrop.width(); ++x) {
                                Pixel b = readPixel(before, x, y), a = readPixel(after, x, y);
                                double weight = clamp(l.opacity * l.fill);
                                if (!mask.isNull())
                                    weight *= maskValue(mask, x, y);
                                if (l.clipped)
                                    weight *= maskValue(clip, x, y);
                                const double alpha = b.a + (a.a - b.a) * weight;
                                writePixel(
                                    backdrop, x, y,
                                    {alpha ? ((1 - weight) * b.r * b.a + weight * a.r * a.a) / alpha : 0,
                                     alpha ? ((1 - weight) * b.g * b.a + weight * a.g * a.a) / alpha : 0,
                                     alpha ? ((1 - weight) * b.b * b.a + weight * a.b * a.a) / alpha : 0,
                                     alpha});
                            }
                    if (!l.clipped && hasClipping) {
                        render(l.id, placed);
                        maskLayer(placed, l, l.fill);
                        clip = alphaMask(placed, true);
                    }
                    visiting.remove(l.id);
                    continue;
                }
                const auto artboard = l.parameters.value("rect").toArray();
                QRectF artboardRect = artboard.size() == 4
                                          ? QRectF(artboard[0].toDouble(), artboard[1].toDouble(),
                                                   artboard[2].toDouble(), artboard[3].toDouble())
                                      : l.shape.isEmpty() ? QRectF(QPointF(), state.size)
                                                          : l.shape.boundingRect();
                artboardRect.translate(l.offset);
                if (l.kind == LayerKind::Artboard && l.color.alpha() > 0) {
                    QPainter painter(&placed);
                    painter.fillRect(artboardRect, l.color);
                }
                render(l.id, placed);
                if (l.kind == LayerKind::Artboard)
                    for (int y = 0; y < placed.height(); ++y)
                        for (int x = 0; x < placed.width(); ++x)
                            if (!artboardRect.contains(QPointF(x + .5, y + .5)))
                                writePixel(placed, x, y, {});
                visiting.remove(l.id);
            } else {
                const QImage image = renderLayerImage(l, state.size, state.bitDepth);
                if (image.isNull())
                    continue;
                if (l.offset.isNull() && image.size() == state.size && image.format() == format)
                    placed = image;
                else
                    placeImage(placed, image, l.offset);
            }
            maskLayer(placed, l);
            if (!l.clipped && hasClipping)
                clip = alphaMask(placed, true);
            placed = applyEffects(placed, l.effects, l.fill);
            if (!l.clipped) {
                // Evaluate each contiguous clipping stack against the base's own color,
                // then composite the entire stack using the base opacity and blend mode.
                for (int next = position + 1; next < siblings.size(); ++next) {
                    Layer upper = withGroupOffset(state.layers[siblings[next]]);
                    if (!upper.clipped)
                        break;
                    consumed.insert(siblings[next]);
                    if (!upper.visible)
                        continue;
                    if (upper.kind == LayerKind::Adjustment) {
                        const QImage adjusted = applyAdjustment(placed, upper.adjustment, upper.parameters);
                        const QImage mask = renderedLayerMask(upper, state.size, state.bitDepth);
                        for (int y = 0; y < placed.height(); ++y)
                            for (int x = 0; x < placed.width(); ++x) {
                                const Pixel b = readPixel(placed, x, y), a = readPixel(adjusted, x, y);
                                double weight = clamp(upper.opacity * upper.fill);
                                if (!mask.isNull())
                                    weight *= maskValue(mask, x, y);
                                const Triple blend =
                                    blended({b.r, b.g, b.b}, {a.r, a.g, a.b}, upper.blendMode);
                                writePixel(placed, x, y,
                                           {b.r + (blend[0] - b.r) * weight, b.g + (blend[1] - b.g) * weight,
                                            b.b + (blend[2] - b.b) * weight, b.a});
                            }
                        continue;
                    }
                    QImage upperImage = blank(state.size, format);
                    if (upper.kind == LayerKind::Group || upper.kind == LayerKind::Artboard) {
                        if (visiting.contains(upper.id))
                            continue;
                        visiting.insert(upper.id);
                        render(upper.id, upperImage);
                        visiting.remove(upper.id);
                    } else {
                        const QImage local = renderLayerImage(upper, state.size, state.bitDepth);
                        if (local.isNull())
                            continue;
                        if (upper.offset.isNull() && local.size() == state.size && local.format() == format)
                            upperImage = local;
                        else
                            placeImage(upperImage, local, upper.offset);
                    }
                    maskLayer(upperImage, upper);
                    upperImage = applyEffects(upperImage, upper.effects, upper.fill);
                    clipCompositeInto(placed, upperImage, upper.blendMode, upper.opacity, linear);
                }
            }
            compositeInto(backdrop, placed, l.blendMode, l.opacity, linear, l.clipped ? clip : QImage());
        }
    };
    render(0, canvas);
    return canvas;
}
} // namespace serika
