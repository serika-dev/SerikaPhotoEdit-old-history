// Clean-room PSD/PSB implementation based on Adobe's public file format
// specification: https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/
#include "io/EmbeddedDocument.h"
#include "io/FormatInternal.h"
#include "io/LayerExtras.h"
#include <QColorSpace>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#ifdef SERIKA_HAVE_ZLIB
#include <zlib.h>
#endif

namespace serika::io {
namespace {
struct Reader {
    QByteArray b;
    qsizetype p = 0;
    explicit Reader(QByteArray bytes) : b(std::move(bytes)) {}
    qsizetype left() const { return b.size() - p; }
    void need(quint64 n) const {
        if (n > quint64(left()))
            throw std::runtime_error("Truncated or invalid PSD section.");
    }
    QByteArray take(quint64 n) {
        need(n);
        auto v = b.mid(p, n);
        p += n;
        return v;
    }
    void skip(quint64 n) {
        need(n);
        p += n;
    }
    quint8 u8() {
        need(1);
        return quint8(b[p++]);
    }
    quint16 u16() {
        need(2);
        auto v = qFromBigEndian<quint16>(b.constData() + p);
        p += 2;
        return v;
    }
    qint16 i16() { return qint16(u16()); }
    quint32 u32() {
        need(4);
        auto v = qFromBigEndian<quint32>(b.constData() + p);
        p += 4;
        return v;
    }
    qint32 i32() { return qint32(u32()); }
    quint64 u64() {
        need(8);
        auto v = qFromBigEndian<quint64>(b.constData() + p);
        p += 8;
        return v;
    }
    double real() {
        const quint64 bits = u64();
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value))
            throw std::runtime_error("Invalid PSD mask parameter.");
        return value;
    }
    quint64 length(bool psb) { return psb ? u64() : u32(); }
    QString unicode() {
        quint32 n = u32();
        if (n > 1000000)
            throw std::runtime_error("Excessive PSD Unicode string.");
        need(quint64(n) * 2);
        QString s;
        s.reserve(n);
        for (quint32 i = 0; i < n; ++i)
            s += QChar(u16());
        if (s.endsWith(QChar(0)))
            s.chop(1);
        return s;
    }
    QRect rect() {
        qint32 t = i32(), l = i32(), bot = i32(), r = i32();
        if (qint64(r) - l < 0 || qint64(bot) - t < 0 || qint64(r) - l > 300000 || qint64(bot) - t > 300000)
            throw std::runtime_error("Invalid PSD layer bounds.");
        return QRect(l, t, r - l, bot - t);
    }
};
struct Writer {
    QByteArray b;
    void u8(quint8 v) { b += char(v); }
    void u16(quint16 v) {
        char p[2];
        qToBigEndian(v, p);
        b.append(p, 2);
    }
    void i16(qint16 v) { u16(quint16(v)); }
    void u32(quint32 v) {
        char p[4];
        qToBigEndian(v, p);
        b.append(p, 4);
    }
    void i32(qint32 v) { u32(quint32(v)); }
    void u64(quint64 v) {
        char p[8];
        qToBigEndian(v, p);
        b.append(p, 8);
    }
    void real(double value) {
        quint64 bits;
        std::memcpy(&bits, &value, sizeof(bits));
        u64(bits);
    }
    void length(quint64 n, bool psb) {
        if (psb)
            u64(n);
        else {
            if (n > 0xffffffffu)
                throw std::runtime_error("PSD exceeds 4 GB; use PSB.");
            u32(n);
        }
    }
    void raw(const QByteArray &v) { b += v; }
    void rect(QRect r) {
        i32(r.y());
        i32(r.x());
        i32(r.y() + r.height());
        i32(r.x() + r.width());
    }
    void unicode(const QString &s) {
        u32(s.size());
        for (QChar c : s)
            u16(c.unicode());
    }
    void pascal(const QString &s, int alignment) {
        auto a = s.toLatin1().left(255);
        u8(a.size());
        raw(a);
        while ((a.size() + 1) % alignment) {
            u8(0);
            a += char(0);
        }
    }
    void tag(const char *key, const QByteArray &data) {
        raw("8BIM");
        raw(QByteArray(key, 4));
        u32(data.size());
        raw(data);
        if (data.size() % 2)
            u8(0);
    }
};
const QVector<QPair<QString, QByteArray>> modes = {
    {"Pass Through", "pass"},  {"Normal", "norm"},       {"Dissolve", "diss"},
    {"Darken", "dark"},        {"Multiply", "mul "},     {"Color Burn", "idiv"},
    {"Linear Burn", "lbrn"},   {"Darker Color", "dkCl"}, {"Lighten", "lite"},
    {"Screen", "scrn"},        {"Color Dodge", "div "},  {"Linear Dodge (Add)", "lddg"},
    {"Lighter Color", "lgCl"}, {"Overlay", "over"},      {"Soft Light", "sLit"},
    {"Hard Light", "hLit"},    {"Vivid Light", "vLit"},  {"Linear Light", "lLit"},
    {"Pin Light", "pLit"},     {"Hard Mix", "hMix"},     {"Difference", "diff"},
    {"Exclusion", "smud"},     {"Subtract", "fsub"},     {"Divide", "fdiv"},
    {"Hue", "hue "},           {"Saturation", "sat "},   {"Color", "colr"},
    {"Luminosity", "lum "}};
QString modeName(const QByteArray &key) {
    for (const auto &m : modes)
        if (m.second == key)
            return m.first;
    return "Normal";
}
QByteArray modeKey(const QString &name) {
    for (const auto &m : modes)
        if (m.first == name || (name == "Linear Dodge" && m.first.startsWith("Linear Dodge")))
            return m.second;
    return "norm";
}
qsizetype planeSize(int w, int h, int depth) {
    const quint64 n = quint64(w) * h * (depth / 8);
    if (n > 512ull * 1024 * 1024)
        throw std::runtime_error("PSD plane exceeds the safe in-memory allocation limit.");
    return n;
}
QByteArray unpackRow(const QByteArray &packed, qsizetype expected) {
    QByteArray out;
    out.reserve(expected);
    qsizetype i = 0;
    while (i < packed.size() && out.size() < expected) {
        qint8 n = qint8(packed[i++]);
        if (n >= 0) {
            int count = int(n) + 1;
            if (count > packed.size() - i || out.size() + count > expected)
                throw std::runtime_error("Invalid PSD PackBits literal.");
            out += packed.mid(i, count);
            i += count;
        } else if (n != -128) {
            int count = 1 - int(n);
            if (i >= packed.size() || out.size() + count > expected)
                throw std::runtime_error("Invalid PSD PackBits run.");
            out += QByteArray(count, packed[i++]);
        }
    }
    if (out.size() != expected)
        throw std::runtime_error("Incomplete PSD PackBits row.");
    return out;
}
QByteArray decodePlane(const QByteArray &data, int w, int h, int depth, bool psb) {
    Reader r(data);
    const auto compression = r.u16();
    const auto expected = planeSize(w, h, depth);
    const qsizetype row = qsizetype(w) * (depth / 8);
    if (!expected)
        return {};
    if (compression == 0)
        return r.take(expected);
    if (compression == 1) {
        QVector<quint32> sizes;
        sizes.reserve(h);
        for (int y = 0; y < h; ++y)
            sizes.append(psb ? r.u32() : r.u16());
        QByteArray out;
        out.reserve(expected);
        for (auto n : sizes)
            out += unpackRow(r.take(n), row);
        return out;
    }
#ifdef SERIKA_HAVE_ZLIB
    if (compression == 2 || compression == 3) {
        QByteArray out(expected, Qt::Uninitialized);
        uLongf n = expected;
        auto compressed = r.take(r.left());
        if (uncompress(reinterpret_cast<Bytef *>(out.data()), &n,
                       reinterpret_cast<const Bytef *>(compressed.constData()), compressed.size()) != Z_OK ||
            n != quint64(expected))
            throw std::runtime_error("Invalid PSD ZIP pixels.");
        if (compression == 3) {
            if (depth == 8) {
                for (int y = 0; y < h; ++y)
                    for (int x = 1; x < w; ++x) {
                        int i = y * row + x;
                        out[i] = char(quint8(out[i]) + quint8(out[i - 1]));
                    }
            } else if (depth == 16) {
                for (int y = 0; y < h; ++y)
                    for (int x = 1; x < w; ++x) {
                        int i = y * row + x * 2;
                        auto a = qFromBigEndian<quint16>(out.constData() + i),
                             prev = qFromBigEndian<quint16>(out.constData() + i - 2);
                        qToBigEndian(quint16(a + prev), out.data() + i);
                    }
            } else {
                for (int y = 0; y < h; ++y) {
                    const qsizetype start = y * row;
                    for (qsizetype x = 1; x < row; ++x)
                        out[start + x] = char(quint8(out[start + x]) + quint8(out[start + x - 1]));
                    QByteArray shuffled = out.mid(start, row);
                    for (int x = 0; x < w; ++x)
                        for (int c = 0; c < 4; ++c)
                            out[start + x * 4 + c] = shuffled[c * w + x];
                }
            }
        }
        return out;
    }
#endif
    throw std::runtime_error("PSD pixel compression is unsupported by this build.");
}
double sample(const QByteArray &p, qsizetype i, int depth) {
    if (p.isEmpty())
        return 0;
    if (depth == 8)
        return quint8(p[i]) / 255.0;
    if (depth == 16)
        return qFromBigEndian<quint16>(p.constData() + i * 2) / 65535.0;
    auto bits = qFromBigEndian<quint32>(p.constData() + i * 4);
    float f;
    std::memcpy(&f, &bits, 4);
    return std::isfinite(f) ? f : 0;
}
double gamma(double n) {
    return n <= 0.0031308 ? 12.92 * n : 1.055 * std::pow(qMax(0.0, n), 1.0 / 2.4) - 0.055;
}
QImage planesToImage(const QHash<int, QByteArray> &planes, int w, int h, int depth, int colorMode) {
    if (w <= 0 || h <= 0)
        return {};
    planeSize(w, h, depth);
    if (quint64(w) * h * 16 > 1024ull * 1024 * 1024)
        throw std::runtime_error("PSD layer exceeds the safe in-memory allocation limit.");
    const auto format = depth == 32   ? QImage::Format_RGBA32FPx4
                        : depth == 16 ? QImage::Format_RGBA64
                                      : QImage::Format_RGBA8888;
    QImage out(w, h, format);
    if (out.isNull())
        throw std::runtime_error("Cannot allocate PSD layer.");
    const QByteArray red = planes.value(0), green = planes.value(1), blue = planes.value(2),
                     black = planes.value(3), alpha = planes.value(-1);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            qsizetype i = qsizetype(y) * w + x;
            double r = sample(red, i, depth), g = sample(green, i, depth), b = sample(blue, i, depth),
                   a = alpha.isEmpty() ? 1 : sample(alpha, i, depth);
            if (colorMode == 1 || colorMode == 8)
                g = b = r;
            else if (colorMode == 4) {
                const double k = sample(black, i, depth);
                r *= k;
                g *= k;
                b *= k;
            } else if (colorMode == 9) {
                double L = r * 100, A = g * 255 - 128, B = b * 255 - 128;
                double fy = (L + 16) / 116, fx = fy + A / 500, fz = fy - B / 200;
                auto inv = [](double t) { return t > 6.0 / 29 ? t * t * t : (108.0 / 841) * (t - 4.0 / 29); };
                double X = 0.96422 * inv(fx), Y = inv(fy), Z = 0.82521 * inv(fz);
                double xx = 0.9555766 * X - 0.0230393 * Y + 0.0631636 * Z,
                       yy = -0.0282895 * X + 1.0099416 * Y + 0.0210077 * Z,
                       zz = 0.0122982 * X - 0.0204830 * Y + 1.3299098 * Z;
                r = gamma(3.2404542 * xx - 1.5371385 * yy - 0.4985314 * zz);
                g = gamma(-0.969266 * xx + 1.8760108 * yy + 0.041556 * zz);
                b = gamma(0.0556434 * xx - 0.2040259 * yy + 1.0572252 * zz);
            }
            if (depth == 32) {
                auto p = reinterpret_cast<float *>(out.scanLine(y)) + 4 * x;
                p[0] = r;
                p[1] = g;
                p[2] = b;
                p[3] = qBound(0.0, a, 1.0);
            } else if (depth == 16) {
                reinterpret_cast<QRgba64 *>(out.scanLine(y))[x] = QRgba64::fromRgba64(
                    qRound(qBound(0.0, r, 1.0) * 65535), qRound(qBound(0.0, g, 1.0) * 65535),
                    qRound(qBound(0.0, b, 1.0) * 65535), qRound(qBound(0.0, a, 1.0) * 65535));
            } else {
                auto p = out.scanLine(y) + 4 * x;
                p[0] = qRound(qBound(0.0, r, 1.0) * 255);
                p[1] = qRound(qBound(0.0, g, 1.0) * 255);
                p[2] = qRound(qBound(0.0, b, 1.0) * 255);
                p[3] = qRound(qBound(0.0, a, 1.0) * 255);
            }
        }
    return out;
}
QByteArray encodePlane(const QImage &image, int channel, int depth) {
    if (image.isNull())
        return QByteArray(2, 0);
    QByteArray encoded(planeSize(image.width(), image.height(), depth) + 2, Qt::Uninitialized);
    encoded[0] = encoded[1] = 0;
    char *dest = encoded.data() + 2;
    QImage im = image.convertToFormat(depth == 32   ? QImage::Format_RGBA32FPx4
                                      : depth == 16 ? QImage::Format_RGBA64
                                                    : QImage::Format_RGBA8888);
    int c = channel == -1 ? 3 : channel;
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x) {
            if (depth == 32) {
                float f = reinterpret_cast<const float *>(im.constScanLine(y))[4 * x + c];
                quint32 bits;
                std::memcpy(&bits, &f, 4);
                qToBigEndian(bits, dest);
                dest += 4;
            } else if (depth == 16) {
                const auto p = reinterpret_cast<const QRgba64 *>(im.constScanLine(y))[x];
                qToBigEndian(quint16(c == 0   ? p.red()
                                     : c == 1 ? p.green()
                                     : c == 2 ? p.blue()
                                              : p.alpha()),
                             dest);
                dest += 2;
            } else
                *dest++ = char(im.constScanLine(y)[4 * x + c]);
        }
    return encoded;
}
QByteArray encodeMask(const QImage &mask, int depth) {
    QByteArray encoded(planeSize(mask.width(), mask.height(), depth) + 2, Qt::Uninitialized);
    encoded[0] = encoded[1] = 0;
    char *dest = encoded.data() + 2;
    for (int y = 0; y < mask.height(); ++y)
        for (int x = 0; x < mask.width(); ++x) {
            const qreal coverage = maskSample(mask, x, y);
            if (depth == 8)
                *dest++ = char(qRound(coverage * 255));
            else if (depth == 16) {
                qToBigEndian(quint16(qRound(coverage * 65535)), dest);
                dest += 2;
            } else {
                float f = float(coverage);
                quint32 bits;
                std::memcpy(&bits, &f, 4);
                qToBigEndian(bits, dest);
                dest += 4;
            }
        }
    return encoded;
}
struct Channel {
    int id;
    quint64 length;
};
struct Record {
    Layer layer;
    QRect bounds, maskBounds;
    QRect realMaskBounds;
    int section = 0, sourceIndex = -1;
    quint8 maskDefault = 255, maskFlags = 0;
    quint8 realMaskDefault = 255, realMaskFlags = 0;
    bool retainedSmartSource = false;
    bool privateMaskLayout = false;
    bool hasRealMask = false;
    QVector<Channel> channels;
};
bool tag64(const QByteArray &key, bool psb) {
    return psb && (key == "LMsk" || key == "Lr16" || key == "Lr32" || key == "Layr" || key == "Mt16" ||
                   key == "Mt32" || key == "Mtrn" || key == "Alph" || key == "FMsk" || key == "lnk2" ||
                   key == "FEid" || key == "FXid" || key == "PxSD");
}
void parseTags(Reader &extra, Record &rec, bool psb, QStringList &report) {
    QJsonArray preserved;
    QStringList unhandled;
    while (extra.left() >= 12) {
        auto signature = extra.take(4);
        if (signature != "8BIM" && signature != "8B64") {
            report.append("Unrecognized layer extra data for " + rec.layer.name);
            break;
        }
        auto key = extra.take(4);
        const auto n = extra.length(signature == "8B64" || tag64(key, psb));
        if (key == "sPEb" && (n == 0 || n > quint64(MaxEmbeddedDocumentBytes)))
            throw std::runtime_error("Invalid Serika PSD embedded contents length.");
        auto data = extra.take(n);
        if (n % 2 && extra.left())
            extra.skip(1);
        Reader r(data);
        if (key == "luni")
            rec.layer.name = r.unicode();
        else if (key == "lyid")
            rec.layer.id = r.u32();
        else if (key == "sPEi")
            rec.sourceIndex = r.i32();
        else if (key == "lsct" || key == "lsdk") {
            rec.section = r.u32();
            if (rec.section == 1 || rec.section == 2)
                rec.layer.kind = LayerKind::Group;
            if (r.left() >= 8) {
                r.skip(4);
                rec.layer.blendMode = modeName(r.take(4));
            }
        } else if (key == "iOpa" && r.left())
            rec.layer.fill = r.u8() / 255.0;
        else if (key == "lspf" && r.left() >= 4) {
            auto flags = r.u32();
            rec.layer.lockAlpha = flags & 1;
            rec.layer.lockPosition = flags & 4;
            rec.layer.locked = flags & 0x80000000u;
        } else if (key == "nvrt") {
            rec.layer.kind = LayerKind::Adjustment;
            rec.layer.adjustment = "Invert";
        } else if (key == "brit" && r.left() >= 4) {
            rec.layer.kind = LayerKind::Adjustment;
            rec.layer.adjustment = "Brightness/Contrast";
            rec.layer.parameters = {{"brightness", r.i16()}, {"contrast", r.i16()}};
        } else if ((key == "post" || key == "thrs") && r.left() >= 2) {
            rec.layer.kind = LayerKind::Adjustment;
            rec.layer.adjustment = key == "post" ? "Posterize" : "Threshold";
            rec.layer.parameters = {{key == "post" ? "levels" : "threshold", r.u16()}};
        } else if (key == "sPEd") {
            auto o = QJsonDocument::fromJson(data).object();
            int kind = o["kind"].toInt(int(rec.layer.kind));
            if (kind < 0 || kind > int(LayerKind::PatternFill))
                throw std::runtime_error("Invalid Serika PSD layer kind.");
            rec.layer.kind = LayerKind(kind);
            auto offset = o["offset"].toArray();
            if (offset.size() == 2)
                rec.layer.offset = QPointF(offset.at(0).toDouble(), offset.at(1).toDouble());
            rec.layer.text = o["text"].toString();
            rec.layer.font.fromString(o["font"].toString());
            rec.layer.color = QColor(o["color"].toString("#ff000000"));
            rec.layer.stroke = QColor(o["stroke"].toString("#00000000"));
            rec.layer.strokeWidth = o["strokeWidth"].toDouble(1);
            rec.layer.effects = o["effects"].toObject();
            rec.layer.parameters = o["parameters"].toObject();
            rec.layer.adjustment = o["adjustment"].toString();
            rec.layer.linkedPath = o["linkedPath"].toString();
            rec.layer.opacity = o["opacity"].toDouble(rec.layer.opacity);
            rec.layer.fill = o["fill"].toDouble(rec.layer.fill);
            QByteArray shape = QByteArray::fromBase64(o["shape"].toString().toLatin1());
            QDataStream stream(shape);
            stream.setVersion(QDataStream::Qt_6_0);
            stream >> rec.layer.shape;
            if (!restoreLayerExtras(rec.layer, o["extras"].toObject()))
                throw std::runtime_error("Invalid Serika mask or smart-object metadata.");
            const QJsonObject extras = o["extras"].toObject();
            rec.retainedSmartSource =
                rec.layer.kind == LayerKind::SmartObject && extras.contains("smartSource");
            rec.privateMaskLayout = extras.contains("maskOffset");
        } else if (key == "sPEb") {
            if (!rec.layer.embeddedDocument.isEmpty() || !validEmbeddedDocument(data))
                throw std::runtime_error("Invalid or duplicate Serika PSD embedded contents.");
            rec.layer.embeddedDocument = data;
        } else {
            preserved.append(QJsonObject{{"key", QString::fromLatin1(key)},
                                         {"data", QString::fromLatin1(data.toBase64())}});
            // Blending options, colour labels, layer metadata and the like keep their default look,
            // so they are carried silently; only blocks that change what the user sees are reported.
            static const QSet<QByteArray> appearanceNeutral{"clbl", "infx", "knko", "lclr", "lnsr", "shmd",
                                                            "fxrp", "lyvr", "cust", "CgEd", "lnkE", "anFX"};
            if (!appearanceNeutral.contains(key))
                unhandled.append(QString::fromLatin1(key));
        }
    }
    if (!preserved.isEmpty())
        rec.layer.parameters["_psdTaggedBlocks"] = preserved;
    if (!unhandled.isEmpty()) {
        static const QList<QPair<QStringList, QString>> features{
            {{"PlLd", "SoLd", "SoLE", "plLd"}, "smart object"},
            {{"lfx2", "lrFX", "lmfx", "lfxs"}, "layer effects"},
            {{"TySh", "tySh"}, "live text"},
            {{"vmsk", "vsms", "vscg", "vogk", "vstk"}, "vector shape"},
            {{"SoCo", "GdFl", "PtFl"}, "fill layer"},
            {{"FXid", "FEid", "smrt"}, "smart filters"}};
        QStringList what;
        for (const auto &feature : features)
            for (const auto &key : feature.first)
                if (unhandled.removeAll(key) && !what.contains(feature.second))
                    what.append(feature.second);
        if (!unhandled.isEmpty())
            what.append("Photoshop data (" + unhandled.join(", ") + ")");
        report.append("“" + rec.layer.name + "”: " + what.join(", ") +
                      " imported as rendered pixels; the original data is kept for saving back to PSD.");
    }
}
QVector<Record> readLayerInfo(const QByteArray &data, bool psb, int depth, int mode, QSize canvas,
                              QStringList &report) {
    Reader r(data);
    if (r.left() < 2)
        return {};
    int count = std::abs(int(r.i16()));
    QVector<Record> records;
    records.reserve(count);
    for (int i = 0; i < count; ++i) {
        Record rec;
        rec.bounds = r.rect();
        rec.layer.offset = rec.bounds.topLeft();
        const auto channels = r.u16();
        if (channels > 56)
            throw std::runtime_error("Invalid PSD layer channel count.");
        for (int c = 0; c < channels; ++c) {
            int id = r.i16();
            rec.channels.append({id, r.length(psb)});
        }
        if (r.take(4) != "8BIM")
            throw std::runtime_error("Invalid PSD layer signature.");
        auto key = r.take(4);
        rec.layer.blendMode = modeName(key);
        if (!std::any_of(modes.cbegin(), modes.cend(), [&](const auto &m) { return m.second == key; }))
            report.append("Unsupported blend mode " + QString::fromLatin1(key) + "; Normal used.");
        rec.layer.opacity = r.u8() / 255.0;
        rec.layer.clipped = r.u8() != 0;
        auto flags = r.u8();
        rec.layer.lockAlpha = flags & 1;
        rec.layer.visible = !(flags & 2);
        r.skip(1);
        Reader ex(r.take(r.u32()));
        Reader mask(ex.take(ex.u32()));
        if (mask.left() >= 18) {
            rec.maskBounds = mask.rect();
            rec.maskDefault = mask.u8();
            rec.maskFlags = mask.u8();
            rec.layer.maskEnabled = !(rec.maskFlags & 2);
            if ((rec.maskFlags & 16) && mask.left()) {
                const quint8 parameters = mask.u8();
                if (parameters & 1)
                    rec.layer.maskDensity = mask.u8() / 255.;
                if (parameters & 2)
                    rec.layer.maskFeather = qBound(0., mask.real(), 1000.);
                if (parameters & 4)
                    rec.layer.vectorMaskDensity = mask.u8() / 255.;
                if (parameters & 8)
                    rec.layer.vectorMaskFeather = qBound(0., mask.real(), 1000.);
            }
            if (mask.left() >= 18) {
                rec.realMaskFlags = mask.u8();
                rec.realMaskDefault = mask.u8();
                rec.realMaskBounds = mask.rect();
                rec.hasRealMask = true;
            }
        }
        ex.skip(ex.u32());
        auto n = ex.u8();
        rec.layer.name = QString::fromLatin1(ex.take(n));
        const int padding = (4 - (n + 1) % 4) % 4;
        ex.skip(padding);
        parseTags(ex, rec, psb, report);
        records.append(rec);
    }
    for (auto &rec : records) {
        QHash<int, QByteArray> planes;
        QByteArray mask;
        QRect decodedMaskBounds;
        quint8 decodedMaskFlags = rec.maskFlags, decodedMaskDefault = rec.maskDefault;
        bool usable = true;
        for (const auto &ch : rec.channels) {
            auto bytes = r.take(ch.length);
            try {
                if (ch.id == -2 || ch.id == -3) {
                    const QRect bounds = ch.id == -3 && rec.hasRealMask ? rec.realMaskBounds : rec.maskBounds;
                    auto v = decodePlane(bytes, bounds.width(), bounds.height(), depth, psb);
                    if (ch.id == -2 || mask.isEmpty()) {
                        mask = v;
                        decodedMaskBounds = bounds;
                        if (ch.id == -3 && rec.hasRealMask) {
                            decodedMaskFlags = rec.realMaskFlags;
                            decodedMaskDefault = rec.realMaskDefault;
                        } else {
                            decodedMaskFlags = rec.maskFlags;
                            decodedMaskDefault = rec.maskDefault;
                        }
                    }
                } else if (ch.id >= -1 && ch.id < 4)
                    planes[ch.id] = decodePlane(bytes, rec.bounds.width(), rec.bounds.height(), depth, psb);
                else
                    report.append("Skipped additional channel " + QString::number(ch.id) + " on " +
                                  rec.layer.name);
            } catch (const std::exception &e) {
                report.append(rec.layer.name + ": " + QString::fromUtf8(e.what()));
                usable = false;
            }
        }
        if (usable && rec.section == 0 && !rec.bounds.isEmpty() && !rec.retainedSmartSource)
            rec.layer.pixels = TileImage::fromImage(
                planesToImage(planes, rec.bounds.width(), rec.bounds.height(), depth, mode));
        if (!mask.isEmpty()) {
            QPoint position;
            QRect localBounds(QPoint(), decodedMaskBounds.size());
            if (!rec.privateMaskLayout) {
                position = decodedMaskBounds.topLeft();
                if (!(decodedMaskFlags & 1))
                    position -= rec.bounds.topLeft();
                localBounds = QRect(position, decodedMaskBounds.size())
                                  .united(QRect(QPoint(), rec.bounds.isEmpty() ? canvas : rec.bounds.size()));
                position -= localBounds.topLeft();
                if (!localBounds.topLeft().isNull()) {
                    // Independent mask extents may start before the layer origin. Preserve the
                    // entire mask, including negative coordinates, rather than discarding samples.
                    rec.layer.maskLinked = false;
                    rec.layer.maskOffset = localBounds.topLeft();
                }
            }
            const QSize maskSize = localBounds.size();
            if (quint64(maskSize.width()) * maskSize.height() * (depth == 32 ? 16 : depth / 8) >
                1024ull * 1024 * 1024)
                throw std::runtime_error("PSD mask exceeds the safe allocation limit.");
            const qreal defaultCoverage =
                (decodedMaskFlags & 4) ? 1 - decodedMaskDefault / 255. : decodedMaskDefault / 255.;
            rec.layer.mask = makeMask(maskSize, depth, defaultCoverage);
            for (int y = 0; y < decodedMaskBounds.height(); ++y) {
                int dy = position.y() + y;
                if (dy < 0 || dy >= maskSize.height())
                    continue;
                for (int x = 0; x < decodedMaskBounds.width(); ++x) {
                    int dx = position.x() + x;
                    if (dx >= 0 && dx < maskSize.width())
                        setMaskSample(
                            rec.layer.mask, dx, dy,
                            (decodedMaskFlags & 4)
                                ? 1 - sample(mask, qsizetype(y) * decodedMaskBounds.width() + x, depth)
                                : sample(mask, qsizetype(y) * decodedMaskBounds.width() + x, depth));
                }
            }
        }
    }
    return records;
}
QByteArray privateLayer(const Layer &l) {
    QByteArray shape;
    QDataStream s(&shape, QIODevice::WriteOnly);
    s.setVersion(QDataStream::Qt_6_0);
    s << l.shape;
    QJsonObject o{{"kind", int(l.kind)},
                  {"offset", QJsonArray{l.offset.x(), l.offset.y()}},
                  {"text", l.text},
                  {"font", l.font.toString()},
                  {"color", l.color.name(QColor::HexArgb)},
                  {"stroke", l.stroke.name(QColor::HexArgb)},
                  {"strokeWidth", l.strokeWidth},
                  {"shape", QString::fromLatin1(shape.toBase64())},
                  {"adjustment", l.adjustment},
                  {"parameters", l.parameters},
                  {"effects", l.effects},
                  {"linkedPath", l.linkedPath},
                  {"opacity", l.opacity},
                  {"fill", l.fill},
                  {"extras", layerExtras(l, true)}};
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
struct ExportRecord {
    Layer layer;
    QImage pixels;
    int section = 0;
};
} // namespace

Document *readPsd(const QString &path, QString *error, QObject *parent) {
    try {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            throw std::runtime_error(f.errorString().toStdString());
        if (f.size() > 2ll * 1024 * 1024 * 1024)
            throw std::runtime_error("This PSD exceeds the 2 GB reader working-memory limit.");
        Reader r(f.readAll());
        if (r.take(4) != "8BPS")
            throw std::runtime_error("Not a PSD/PSB file.");
        auto version = r.u16();
        if (version != 1 && version != 2)
            throw std::runtime_error("Unsupported PSD version.");
        bool psb = version == 2;
        if (r.take(6) != QByteArray(6, 0))
            throw std::runtime_error("Invalid PSD reserved header bytes.");
        int channels = r.u16();
        int h = r.u32(), w = r.u32(), depth = r.u16(), mode = r.u16();
        if (channels < 1 || channels > 56 || w < 1 || h < 1 || w > (psb ? 300000 : 30000) ||
            h > (psb ? 300000 : 30000) || !(depth == 8 || depth == 16 || depth == 32))
            throw std::runtime_error("Unsupported PSD header dimensions or depth.");
        if (mode != 3 && mode != 1 && mode != 4 && mode != 9 && mode != 8)
            throw std::runtime_error("Supported PSD colour modes are RGB, grayscale, CMYK, Lab and duotone.");
        planeSize(w, h, depth);
        r.skip(r.u32());
        auto doc = std::make_unique<Document>(parent);
        doc->state.size = QSize(w, h);
        doc->state.bitDepth = depth;
        doc->state.colorMode = "RGB";
        doc->title = QFileInfo(path).completeBaseName();
        doc->filePath = path;
        if (mode != 3 && mode != 1)
            doc->importReport.append("PSD colour mode converted to RGB for editing; retain the source for "
                                     "original colour-space data.");
        Reader resources(r.take(r.u32()));
        QJsonArray unknownResources;
        while (resources.left() >= 12) {
            auto sig = resources.take(4);
            if (sig != "8BIM")
                throw std::runtime_error("Invalid PSD resource signature.");
            int id = resources.u16();
            auto n = resources.u8();
            resources.skip(n);
            if ((n + 1) % 2)
                resources.skip(1);
            auto length = resources.u32();
            auto data = resources.take(length);
            if (length % 2)
                resources.skip(1);
            Reader rr(data);
            if (id == 1039)
                doc->state.iccProfile = data;
            else if (id == 1005 && data.size() >= 16)
                doc->state.resolution = rr.u32() / 65536.0;
            else if (id == 1060)
                doc->state.metadata["XMP"] = QString::fromUtf8(data);
            else if (id == 1032 && data.size() >= 16) {
                rr.skip(12);
                auto count = rr.u32();
                if (count > 100000)
                    throw std::runtime_error("Too many PSD guides.");
                for (quint32 i = 0; i < count; ++i) {
                    auto pos = rr.u32();
                    auto dir = rr.u8();
                    doc->state.guides.append({dir == 0, pos / 32.0});
                }
            } else {
                // Print settings, EXIF, IPTC, slices and similar resources are kept verbatim; none
                // of them changes the image, so they are not worth an import warning.
                unknownResources.append(
                    QJsonObject{{"id", id}, {"data", QString::fromLatin1(data.toBase64())}});
            }
        }
        doc->state.metadata["_psdResources"] = unknownResources;
        Reader lm(r.take(r.length(psb)));
        QVector<Record> records;
        if (lm.left() >= (psb ? 8 : 4)) {
            auto info = lm.take(lm.length(psb));
            if (!info.isEmpty())
                records = readLayerInfo(info, psb, depth, mode, doc->state.size, doc->importReport);
            if (lm.left() >= 4)
                lm.skip(lm.u32());
            while (lm.left() >= 12) {
                auto sig = lm.take(4);
                if (sig != "8BIM" && sig != "8B64")
                    break;
                auto key = lm.take(4);
                auto n = lm.length(sig == "8B64" || tag64(key, psb));
                auto data = lm.take(n);
                if (n % 4 && quint64(lm.left()) >= (4 - n % 4))
                    lm.skip(4 - n % 4);
                // Pattern presets, smart-object source files, filter-mask settings and text engine
                // data back features that are imported as pixels, so they need no separate warning.
                static const QSet<QByteArray> documentNeutral{"Patt", "Pat2", "Pat3", "lnk2", "lnk3", "lnkD",
                                                              "FMsk", "Txt2", "FEid", "FXid", "Anno", "pths"};
                if ((key == "Lr16" && depth == 16) || (key == "Lr32" && depth == 32) || key == "Layr")
                    records = readLayerInfo(data, psb, depth, mode, doc->state.size, doc->importReport);
                else if (!documentNeutral.contains(key))
                    doc->importReport.append("Photoshop document data " + QString::fromLatin1(key) +
                                             " is not used by Serika and was skipped.");
            }
        }
        // Photoshop stores layer records bottom-to-top: a group's end marker comes first, then its
        // children, then the group folder. Serika builds before 2026-10 wrote them top-to-bottom
        // (folder first); the first group record tells the two layouts apart.
        bool bottomUp = true;
        for (const auto &rec : records)
            if (rec.section >= 1 && rec.section <= 3) {
                bottomUp = rec.section == 3;
                break;
            }
        if (bottomUp)
            std::reverse(records.begin(), records.end());
        quint64 nextId = 1;
        QSet<quint64> ids;
        for (auto &rec : records) {
            if (rec.section == 3)
                continue;
            if (!rec.layer.id || ids.contains(rec.layer.id)) {
                while (ids.contains(nextId))
                    ++nextId;
                rec.layer.id = nextId++;
            }
            ids.insert(rec.layer.id);
        }
        QVector<quint64> stack;
        QVector<Layer> topDown;
        for (auto &rec : records) {
            if (rec.section == 3) {
                if (!stack.isEmpty())
                    stack.removeLast();
                else
                    doc->importReport.append("Unmatched PSD group end marker.");
                continue;
            }
            rec.layer.parentId = stack.isEmpty() ? 0 : stack.last();
            topDown.append(rec.layer);
            if (rec.section == 1 || rec.section == 2)
                stack.append(rec.layer.id);
        }
        for (auto it = topDown.crbegin(); it != topDown.crend(); ++it)
            doc->state.layers.append(*it);
        QHash<quint64, int> sourceOrder;
        bool completeOrder = true;
        for (const auto &rec : records)
            if (rec.section != 3) {
                if (rec.sourceIndex < 0)
                    completeOrder = false;
                sourceOrder[rec.layer.id] = rec.sourceIndex;
            }
        if (completeOrder)
            std::stable_sort(doc->state.layers.begin(), doc->state.layers.end(),
                             [&](const auto &a, const auto &b) {
                                 return sourceOrder.value(a.id) < sourceOrder.value(b.id);
                             });
        bool pixelLayers = false;
        for (const auto &l : doc->state.layers)
            if (!l.pixels.empty() || l.kind == LayerKind::Adjustment)
                pixelLayers = true;
        // Composite fallback remains useful when layers use unsupported compression.
        if (!pixelLayers) {
            QHash<int, QByteArray> planes;
            const int compression = r.u16();
            if (compression == 0) {
                for (int c = 0; c < channels; ++c) {
                    auto p = r.take(planeSize(w, h, depth));
                    if (c < 4)
                        planes[c == (mode == 3 ? 3 : mode == 1 ? 1 : 4) ? -1 : c] = p;
                }
            } else if (compression == 1) {
                QVector<quint32> rows;
                rows.reserve(h * channels);
                for (int i = 0; i < h * channels; ++i)
                    rows.append(psb ? r.u32() : r.u16());
                for (int c = 0; c < channels; ++c) {
                    QByteArray p;
                    p.reserve(planeSize(w, h, depth));
                    for (int y = 0; y < h; ++y)
                        p += unpackRow(r.take(rows[c * h + y]), qsizetype(w) * (depth / 8));
                    if (c < 4)
                        planes[c == (mode == 3 ? 3 : mode == 1 ? 1 : 4) ? -1 : c] = p;
                }
            } else if (compression == 2 || compression == 3) {
                Writer packed;
                packed.u16(compression);
                packed.raw(r.take(r.left()));
                const auto pixels = decodePlane(packed.b, w, h * channels, depth, psb);
                const auto channelSize = planeSize(w, h, depth);
                for (int c = 0; c < channels; ++c)
                    if (c < 4)
                        planes[c == (mode == 3   ? 3
                                     : mode == 1 ? 1
                                                 : 4)
                                   ? -1
                                   : c] = pixels.mid(c * channelSize, channelSize);
            } else
                throw std::runtime_error(
                    "PSD has no usable layers and its merged compression is unsupported.");
            Layer l;
            l.id = nextId;
            l.name = "Merged image";
            l.pixels = TileImage::fromImage(planesToImage(planes, w, h, depth, mode));
            doc->state.layers = {l};
            if (!records.isEmpty())
                doc->importReport.append(
                    "The merged image was imported because layer pixels could not be decoded.");
        }
        if (mode != 3 && !doc->state.iccProfile.isEmpty()) {
            doc->state.metadata["originalPsdIccProfile"] =
                QString::fromLatin1(doc->state.iccProfile.toBase64());
            doc->state.iccProfile = QColorSpace(QColorSpace::SRgb).iccProfile();
        }
        doc->state.activeIndex = qMax(0, doc->state.layers.size() - 1);
        doc->touch();
        doc->markSaved();
        return doc.release();
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return nullptr;
    }
}

bool writePsd(const Document *doc, const QString &path, QString *error) {
    try {
        if (!doc || doc->state.size.isEmpty())
            throw std::runtime_error("Invalid document.");
        for (const auto &layer : doc->state.layers)
            if (!layer.embeddedDocument.isEmpty() && !validEmbeddedDocument(layer.embeddedDocument))
                throw std::runtime_error("Invalid embedded native contents on a PSD layer.");
        bool psb = QFileInfo(path).suffix().compare("psb", Qt::CaseInsensitive) == 0;
        const int depth = doc->state.bitDepth == 32 ? 32 : doc->state.bitDepth == 16 ? 16 : 8;
        if (doc->state.size.width() > (psb ? 300000 : 30000) ||
            doc->state.size.height() > (psb ? 300000 : 30000))
            throw std::runtime_error("Document dimensions require PSB.");
        QVector<ExportRecord> records;
        QSet<quint64> visited;
        std::function<void(quint64)> emitChildren = [&](quint64 parent) {
            for (auto it = doc->state.layers.crbegin(); it != doc->state.layers.crend(); ++it)
                if (it->parentId == parent && !visited.contains(it->id)) {
                    visited.insert(it->id);
                    if (it->kind == LayerKind::Group || it->kind == LayerKind::Artboard) {
                        records.append({*it, {}, 1});
                        emitChildren(it->id);
                        Layer end;
                        end.name = "</Layer group>";
                        end.id = 0;
                        records.append({end, {}, 3});
                    } else {
                        QImage image;
                        if (it->kind == LayerKind::Pixel)
                            image = it->pixels.image();
                        else
                            image = doc->layerImage(*it);
                        records.append({*it, image, 0});
                    }
                }
        };
        emitChildren(0);
        for (const auto &l : doc->state.layers)
            if (!visited.contains(l.id))
                throw std::runtime_error("A layer has a missing or cyclic group parent.");
        if (records.size() > 32767)
            throw std::runtime_error("PSD supports at most 32767 layer records.");
        // Records were gathered top-down; Photoshop expects them bottom-to-top, group end marker first.
        std::reverse(records.begin(), records.end());
        auto absoluteOffset = [&](const Layer &l) {
            QPointF offset = l.offset;
            quint64 id = l.parentId;
            QSet<quint64> seen;
            while (id && !seen.contains(id)) {
                seen.insert(id);
                int index = doc->indexForId(id);
                if (index < 0)
                    break;
                const auto &p = doc->state.layers[index];
                offset += p.offset;
                id = p.parentId;
            }
            return offset.toPoint();
        };
        Writer info, pixels;
        info.i16(-qint16(records.size()));
        for (const auto &rec : records) {
            const auto &l = rec.layer;
            QRect bounds(rec.pixels.isNull() ? QPoint() : absoluteOffset(l), rec.pixels.size());
            info.rect(bounds);
            QVector<QPair<int, QByteArray>> channels;
            for (int c : {0, 1, 2, -1})
                channels.append({c, encodePlane(rec.pixels, c, depth)});
            if (!l.mask.isNull())
                channels.append({-2, encodeMask(l.mask, depth)});
            info.u16(channels.size());
            for (const auto &c : channels) {
                info.i16(c.first);
                info.length(c.second.size(), psb);
                pixels.raw(c.second);
            }
            info.raw("8BIM");
            info.raw(modeKey(l.blendMode));
            info.u8(qRound(qBound(0.0, double(l.opacity), 1.0) * 255));
            info.u8(l.clipped);
            info.u8((l.lockAlpha ? 1 : 0) | (l.visible ? 0 : 2));
            info.u8(0);
            Writer extra;
            if (l.mask.isNull())
                extra.u32(0);
            else {
                const QRect maskBounds(absoluteOffset(l) + (l.maskLinked ? QPoint() : l.maskOffset.toPoint()),
                                       l.mask.size());
                const quint8 parameters = (l.maskDensity != 1 ? 1 : 0) | (l.maskFeather > 0 ? 2 : 0) |
                                          (l.vectorMaskDensity != 1 ? 4 : 0) |
                                          (l.vectorMaskFeather > 0 ? 8 : 0);
                const quint8 flags = (l.maskEnabled ? 0 : 2) | (parameters ? 16 : 0);
                Writer mask;
                mask.rect(maskBounds);
                mask.u8(0);
                mask.u8(flags);
                if (parameters) {
                    mask.u8(parameters);
                    if (parameters & 1)
                        mask.u8(qRound(qBound(0., l.maskDensity, 1.) * 255));
                    if (parameters & 2)
                        mask.real(l.maskFeather);
                    if (parameters & 4)
                        mask.u8(qRound(qBound(0., l.vectorMaskDensity, 1.) * 255));
                    if (parameters & 8)
                        mask.real(l.vectorMaskFeather);
                    mask.u8(flags);
                    mask.u8(0);
                    mask.rect(maskBounds);
                } else
                    mask.u16(0);
                extra.u32(mask.b.size());
                extra.raw(mask.b);
            }
            extra.u32(0);
            extra.pascal(l.name, 4);
            Writer name;
            name.unicode(l.name);
            extra.tag("luni", name.b);
            Writer id;
            id.u32(l.id);
            extra.tag("lyid", id.b);
            Writer fill;
            fill.u8(qRound(qBound(0.0, double(l.fill), 1.0) * 255));
            extra.tag("iOpa", fill.b);
            if (rec.section) {
                Writer group;
                group.u32(rec.section);
                group.raw("8BIM");
                group.raw(modeKey(l.blendMode));
                extra.tag("lsct", group.b);
            } else if (l.kind == LayerKind::Adjustment && l.adjustment == "Invert")
                extra.tag("nvrt", {});
            if (rec.section != 3) {
                extra.tag("sPEd", privateLayer(l));
                if (!l.embeddedDocument.isEmpty())
                    extra.tag("sPEb", l.embeddedDocument);
                Writer order;
                order.i32(doc->indexForId(l.id));
                extra.tag("sPEi", order.b);
            }
            Writer lock;
            lock.u32((l.lockAlpha ? 1 : 0) | (l.lockPosition ? 4 : 0) | (l.locked ? 0x80000000u : 0));
            extra.tag("lspf", lock.b);
            info.u32(extra.b.size());
            info.raw(extra.b);
        }
        info.raw(pixels.b);
        if (info.b.size() % 2)
            info.u8(0);
        Writer layerMask;
        layerMask.length(info.b.size(), psb);
        layerMask.raw(info.b);
        layerMask.u32(0);
        Writer resources;
        auto addResource = [&](int id, const QByteArray &data) {
            resources.raw("8BIM");
            resources.u16(id);
            resources.u16(0);
            resources.u32(data.size());
            resources.raw(data);
            if (data.size() % 2)
                resources.u8(0);
        };
        if (!doc->state.iccProfile.isEmpty())
            addResource(1039, doc->state.iccProfile);
        Writer resolution;
        resolution.u32(qRound(doc->state.resolution * 65536));
        resolution.u16(1);
        resolution.u16(1);
        resolution.u32(qRound(doc->state.resolution * 65536));
        resolution.u16(1);
        resolution.u16(1);
        addResource(1005, resolution.b);
        if (!doc->state.guides.isEmpty()) {
            Writer guides;
            guides.u32(1);
            guides.u32(576);
            guides.u32(576);
            guides.u32(doc->state.guides.size());
            for (const auto &g : doc->state.guides) {
                guides.u32(qRound(g.position * 32));
                guides.u8(g.vertical ? 0 : 1);
            }
            addResource(1032, guides.b);
        }
        if (doc->state.metadata.contains("XMP"))
            addResource(1060, doc->state.metadata["XMP"].toString().toUtf8());
        QSaveFile f(path);
        f.setDirectWriteFallback(false);
        if (!f.open(QIODevice::WriteOnly))
            throw std::runtime_error(f.errorString().toStdString());
        Writer header;
        header.raw("8BPS");
        header.u16(psb ? 2 : 1);
        header.raw(QByteArray(6, 0));
        header.u16(4);
        header.u32(doc->state.size.height());
        header.u32(doc->state.size.width());
        header.u16(depth);
        header.u16(3);
        header.u32(0);
        header.u32(resources.b.size());
        header.raw(resources.b);
        header.length(layerMask.b.size(), psb);
        header.raw(layerMask.b);
        header.u16(0);
        if (f.write(header.b) != header.b.size())
            throw std::runtime_error(f.errorString().toStdString());
        const QImage merged = doc->composite();
        for (int c : {0, 1, 2, -1}) {
            auto plane = encodePlane(merged, c, depth);
            if (f.write(plane.constData() + 2, plane.size() - 2) != plane.size() - 2)
                throw std::runtime_error(f.errorString().toStdString());
        }
        if (!f.commit())
            throw std::runtime_error(f.errorString().toStdString());
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return false;
    }
}
} // namespace serika::io
