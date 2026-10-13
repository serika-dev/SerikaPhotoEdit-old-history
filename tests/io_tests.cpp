#include "io/FormatIO.h"
#include "io/LayerExtras.h"
#include <QColorSpace>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

using namespace serika;
namespace {
QImage pattern(QSize size, int depth = 8) {
    QImage image(size, depth == 32   ? QImage::Format_RGBA32FPx4
                       : depth == 16 ? QImage::Format_RGBA64
                                     : QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x) {
            if (depth == 16)
                reinterpret_cast<QRgba64 *>(image.scanLine(y))[x] =
                    QRgba64::fromRgba64((x * 257 + y * 31) % 65536, (y * 997 + x * 77) % 65536,
                                        (x * 129 + y * 541) % 65536, 65535);
            else if (depth == 32) {
                auto p = reinterpret_cast<float *>(image.scanLine(y)) + x * 4;
                p[0] = float(x) / size.width() * 3 - .25;
                p[1] = float(y) / size.height();
                p[2] = .125;
                p[3] = 1;
            } else {
                auto p = image.scanLine(y) + x * 4;
                p[0] = (x * 17 + y * 7) % 256;
                p[1] = (y * 13 + x * 3) % 256;
                p[2] = (x * 31 + y * 19) % 256;
                p[3] = 255;
            }
        }
    return image;
}
bool identical(const QImage &a, const QImage &b) {
    if (a.size() != b.size() || a.format() != b.format())
        return false;
    const int n = (a.width() * a.depth() + 7) / 8;
    for (int y = 0; y < a.height(); ++y)
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y), n))
            return false;
    return true;
}
void be16(QByteArray &b, quint16 v) {
    char p[2];
    qToBigEndian(v, p);
    b.append(p, 2);
}
void be32(QByteArray &b, quint32 v) {
    char p[4];
    qToBigEndian(v, p);
    b.append(p, 4);
}
void le16(QByteArray &b, quint16 v) {
    char p[2];
    qToLittleEndian(v, p);
    b.append(p, 2);
}
void le32(QByteArray &b, quint32 v) {
    char p[4];
    qToLittleEndian(v, p);
    b.append(p, 4);
}
QByteArray externalCompositePsd(bool rle) {
    QByteArray b = "8BPS";
    be16(b, 1);
    b += QByteArray(6, 0);
    be16(b, 3);
    be32(b, 2);
    be32(b, 3);
    be16(b, 8);
    be16(b, 3);
    be32(b, 0);
    be32(b, 0);
    be32(b, 0);
    be16(b, rle ? 1 : 0);
    if (rle)
        for (int i = 0; i < 6; ++i)
            be16(b, 4);
    for (int c = 0; c < 3; ++c)
        for (int y = 0; y < 2; ++y) {
            if (rle)
                b += char(2);
            for (int x = 0; x < 3; ++x)
                b += char(c * 60 + y * 7 + x);
        }
    return b;
}
QByteArray externalMaskedPsd(int depth) {
    const auto scalar = [=](QByteArray &bytes, double value) {
        if (depth == 16)
            be16(bytes, quint16(qRound(value * 65535)));
        else {
            const float sample = float(value);
            quint32 bits;
            std::memcpy(&bits, &sample, sizeof(bits));
            be32(bytes, bits);
        }
    };
    QVector<QByteArray> channels;
    for (int channel = 0; channel < 5; ++channel) {
        QByteArray plane;
        be16(plane, 0);
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 4; ++x) {
                const double coverage = depth == 16 ? (12345 + x * 5000 + y * 1000) / 65535.
                                                    : (x == 0 && y == 0 ? 1e-8 : .05 + x * .2 + y * .01);
                scalar(plane, channel == 4 ? coverage : (channel == 0 || channel == 3 ? 1 : 0));
            }
        channels.append(plane);
    }
    QByteArray mask;
    for (quint32 value : {1u, 1u, 3u, 5u})
        be32(mask, value);
    mask += char(0);  // Black outside the independent mask rectangle.
    mask += char(16); // Has native mask parameters.
    mask += char(1);  // Density parameter is present.
    mask += char(128);
    mask += char(0); // Real mask flags.
    mask += char(0);
    for (quint32 value : {1u, 1u, 3u, 5u})
        be32(mask, value);
    QByteArray extra;
    be32(extra, mask.size());
    extra += mask;
    be32(extra, 0); // Blending ranges.
    const QByteArray name = "External Mask";
    extra += char(name.size());
    extra += name;
    extra += QByteArray((4 - (name.size() + 1) % 4) % 4, 0);
    QByteArray info;
    be16(info, 0xffff); // One layer, with merged alpha.
    for (quint32 value : {1u, 2u, 3u, 6u})
        be32(info, value);
    be16(info, 5);
    for (int channel = 0; channel < 5; ++channel) {
        be16(info, quint16(channel < 3 ? channel : channel == 3 ? -1 : -2));
        be32(info, channels[channel].size());
    }
    info += "8BIMnorm";
    info += char(255);
    info += QByteArray(3, 0);
    be32(info, extra.size());
    info += extra;
    for (const QByteArray &channel : channels)
        info += channel;
    if (info.size() % 2)
        info += char(0);
    QByteArray layerMask;
    be32(layerMask, info.size());
    layerMask += info;
    be32(layerMask, 0);
    QByteArray bytes = "8BPS";
    be16(bytes, 1);
    bytes += QByteArray(6, 0);
    be16(bytes, 4);
    be32(bytes, 5);
    be32(bytes, 8);
    be16(bytes, depth);
    be16(bytes, 3);
    be32(bytes, 0);
    be32(bytes, 0);
    be32(bytes, layerMask.size());
    bytes += layerMask;
    be16(bytes, 0);
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 40; ++i)
            scalar(bytes, c == 0 || c == 3 ? 1 : 0);
    return bytes;
}
QByteArray syntheticDng() {
    struct Tag {
        quint16 id, type;
        quint32 count;
        QByteArray data;
    };
    QVector<Tag> tags;
    auto shorts = [](std::initializer_list<quint16> values) {
        QByteArray b;
        for (auto v : values)
            le16(b, v);
        return b;
    };
    auto longs = [](std::initializer_list<quint32> values) {
        QByteArray b;
        for (auto v : values)
            le32(b, v);
        return b;
    };
    const int w = 96, h = 80;
    tags = {{256, 4, 1, longs({w})},
            {257, 4, 1, longs({h})},
            {258, 3, 1, shorts({16})},
            {259, 3, 1, shorts({1})},
            {262, 3, 1, shorts({32803})},
            {271, 2, 7, QByteArray("Serika\0", 7)},
            {272, 2, 15, QByteArray("Synthetic DNG\0", 14) + char(0)},
            {273, 4, 1, longs({0})},
            {277, 3, 1, shorts({1})},
            {278, 4, 1, longs({h})},
            {279, 4, 1, longs({w * h * 2})},
            {284, 3, 1, shorts({1})},
            {33421, 3, 2, shorts({2, 2})},
            {33422, 1, 4, QByteArray::fromHex("00010102")},
            {50706, 1, 4, QByteArray::fromHex("01040000")},
            {50707, 1, 4, QByteArray::fromHex("01010000")},
            {50708, 2, 15, QByteArray("Serika Test RAW\0", 16).left(15)},
            {50714, 3, 1, shorts({0})},
            {50717, 4, 1, longs({65535})},
            {50778, 3, 1, shorts({21})}};
    QByteArray matrix;
    for (int v : {1, 0, 0, 0, 1, 0, 0, 0, 1}) {
        le32(matrix, v);
        le32(matrix, 1);
    }
    tags.append({50721, 10, 9, matrix});
    std::sort(tags.begin(), tags.end(), [](const Tag &a, const Tag &b) { return a.id < b.id; });
    QByteArray extra;
    quint32 base = 8 + 2 + tags.size() * 12 + 4;
    QByteArray ifd;
    le16(ifd, tags.size());
    int stripEntry = -1;
    for (const auto &tag : tags) {
        le16(ifd, tag.id);
        le16(ifd, tag.type);
        le32(ifd, tag.count);
        if (tag.id == 273)
            stripEntry = ifd.size();
        if (tag.data.size() <= 4) {
            ifd += tag.data;
            ifd += QByteArray(4 - tag.data.size(), 0);
        } else {
            le32(ifd, base + extra.size());
            extra += tag.data;
            if (extra.size() % 2)
                extra += char(0);
        }
    }
    le32(ifd, 0);
    quint32 strip = base + extra.size();
    qToLittleEndian(strip, ifd.data() + stripEntry);
    QByteArray b = "II";
    le16(b, 42);
    le32(b, 8);
    b += ifd;
    b += extra;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            le16(b, quint16(6000 + x * 350 + y * 130));
    return b;
}
QByteArray twoFrameGif() {
    QByteArray b = "GIF89a";
    le16(b, 2);
    le16(b, 2);
    b += QByteArray::fromHex("800000ff00000000ff");
    for (int colour : {0, 1}) {
        b += QByteArray::fromHex("21f90404050000002c00000000020002000002");
        QByteArray codes;
        quint32 bits = 0;
        int shift = 0;
        for (int code : {4, colour, 4, colour, 4, colour, 4, colour, 5}) {
            bits |= quint32(code) << shift;
            shift += 3;
            while (shift >= 8) {
                codes += char(bits & 255);
                bits >>= 8;
                shift -= 8;
            }
        }
        if (shift)
            codes += char(bits & 255);
        b += char(codes.size());
        b += codes;
        b += char(0);
    }
    b += char(0x3b);
    return b;
}
QByteArray minimalPdf() {
    QByteArray pdf = "%PDF-1.4\n";
    QVector<qsizetype> offsets{0};
    auto object = [&](int id, const QByteArray &body) {
        offsets.append(pdf.size());
        pdf += QByteArray::number(id) + " 0 obj\n" + body + "\nendobj\n";
    };
    object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    object(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 144 72] /Resources << >> /Contents 4 0 R >>");
    const QByteArray paint = "1 0 0 rg 0 0 144 72 re f\n";
    object(4, "<< /Length " + QByteArray::number(paint.size()) + " >>\nstream\n" + paint + "endstream");
    const auto xref = pdf.size();
    pdf += "xref\n0 5\n0000000000 65535 f \n";
    for (int i = 1; i < offsets.size(); ++i)
        pdf += QByteArray::number(offsets[i]).rightJustified(10, '0') + " 00000 n \n";
    pdf += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return pdf;
}
} // namespace
class IoTests : public QObject {
    Q_OBJECT
  private slots:
    void nativeFullState() {
        QTemporaryDir dir;
        std::unique_ptr<Document> doc(Document::create({513, 300}, Qt::transparent, 16));
        doc->title = "Fidelity Ω";
        doc->state.layers[0].pixels = TileImage::fromImage(pattern(doc->state.size, 16));
        doc->state.iccProfile = QColorSpace(QColorSpace::DisplayP3).iccProfile();
        doc->state.resolution = 300;
        doc->state.guides = {{true, 37.5}, {false, 129.25}};
        doc->state.selection = QImage(doc->state.size, QImage::Format_Grayscale8);
        doc->state.selection.fill(87);
        doc->state.metadata = {{"XMP", "<packet>metadata</packet>"},
                               {"develop", QJsonObject{{"exposure", 1.25}}}};
        doc->blendLinear = true;
        doc->historyLimit = 123;
        Layer group;
        group.id = 73;
        group.kind = LayerKind::Group;
        group.name = "Group";
        doc->state.layers.append(group);
        Layer text;
        text.id = 91;
        text.parentId = 73;
        text.name = "Editable type";
        text.kind = LayerKind::Text;
        text.text = "Serika α";
        text.font = QFont("Arial", 17, QFont::Bold);
        text.offset = {13.25, -9.5};
        text.opacity = .47;
        text.fill = .81;
        text.blendMode = "Overlay";
        text.color = QColor::fromRgbF(.125, .25, .375, .625);
        text.stroke = Qt::yellow;
        text.strokeWidth = 2.5;
        text.locked = true;
        text.lockAlpha = true;
        text.lockPosition = true;
        text.clipped = true;
        text.maskTarget = true;
        text.maskEnabled = false;
        text.visible = false;
        text.effects = {{"shadow", QJsonObject{{"radius", 4}}}};
        text.parameters = {{"size", 17}};
        text.linkedPath = "test.png";
        text.shape.moveTo(3, 5);
        text.shape.cubicTo(6, 7, 9, 10, 11, 13);
        text.mask = QImage(doc->state.size, QImage::Format_Grayscale8);
        text.mask.fill(122);
        doc->state.layers.append(text);
        doc->state.activeIndex = 2;
        QString error, path = dir.filePath("test.spe");
        QVERIFY2(FormatIO::saveNative(doc.get(), path, &error), qPrintable(error));
        std::unique_ptr<Document> loaded(FormatIO::openNative(path, &error));
        QVERIFY2(loaded, qPrintable(error));
        QCOMPARE(loaded->state.size, doc->state.size);
        QCOMPARE(loaded->state.layers.size(), 3);
        QCOMPARE(loaded->title, doc->title);
        QCOMPARE(loaded->state.activeIndex, 2);
        QCOMPARE(loaded->state.iccProfile, doc->state.iccProfile);
        QCOMPARE(loaded->state.metadata, doc->state.metadata);
        QCOMPARE(loaded->state.resolution, 300.0);
        QCOMPARE(loaded->historyLimit, 123);
        QVERIFY(loaded->blendLinear);
        QVERIFY(identical(loaded->state.selection, doc->state.selection));
        QCOMPARE(loaded->state.guides.size(), 2);
        QCOMPARE(loaded->state.guides[0].position, 37.5);
        const auto &l = loaded->state.layers[2];
        QCOMPARE(l.id, text.id);
        QCOMPARE(l.parentId, text.parentId);
        QCOMPARE(l.kind, text.kind);
        QCOMPARE(l.text, text.text);
        QCOMPARE(l.font, text.font);
        QCOMPARE(l.offset, text.offset);
        QCOMPARE(l.opacity, text.opacity);
        QCOMPARE(l.fill, text.fill);
        QCOMPARE(l.blendMode, text.blendMode);
        QCOMPARE(l.shape, text.shape);
        QCOMPARE(l.effects, text.effects);
        QCOMPARE(l.parameters, text.parameters);
        QCOMPARE(l.color, text.color);
        QCOMPARE(l.stroke, text.stroke);
        QCOMPARE(l.strokeWidth, text.strokeWidth);
        QCOMPARE(l.linkedPath, text.linkedPath);
        QVERIFY(l.locked && l.lockAlpha && l.lockPosition && l.clipped && l.maskTarget && !l.maskEnabled &&
                !l.visible);
        QVERIFY(identical(l.mask, text.mask));
        auto &original = doc->state.layers[0].pixels;
        auto &copy = loaded->state.layers[0].pixels;
        QCOMPARE(copy.tiles.size(), original.tiles.size());
        for (auto it = original.tiles.cbegin(); it != original.tiles.cend(); ++it)
            QVERIFY(identical(it.value(), copy.tiles.value(it.key())));
        QVERIFY(!loaded->isModified());
    }
    void nativeFloat() {
        QTemporaryDir dir;
        std::unique_ptr<Document> d(Document::create({17, 13}, Qt::transparent, 32));
        d->state.layers[0].pixels = TileImage::fromImage(pattern(d->state.size, 32));
        QString e, p = dir.filePath("float.speb");
        QVERIFY2(FormatIO::saveNative(d.get(), p, &e), qPrintable(e));
        std::unique_ptr<Document> r(FormatIO::openNative(p, &e));
        QVERIFY2(r, qPrintable(e));
        QVERIFY(identical(d->state.layers[0].pixels.image(), r->state.layers[0].pixels.image()));
    }
    void nativeCorruption() {
        QTemporaryDir dir;
        std::unique_ptr<Document> d(Document::create({7, 5}, Qt::red));
        QString p = dir.filePath("bad.spe"), error;
        QVERIFY(FormatIO::saveNative(d.get(), p, &error));
        QFile f(p);
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto bytes = f.readAll();
        f.close();
        auto corrupted = bytes;
        corrupted[corrupted.size() / 2] = char(corrupted[corrupted.size() / 2] ^ 64);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(corrupted);
        f.close();
        std::unique_ptr<Document> r(FormatIO::openNative(p, &error));
        QVERIFY(!r);
        QVERIFY(error.contains("checksum", Qt::CaseInsensitive));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes.left(bytes.size() - 6));
        f.close();
        r.reset(FormatIO::openNative(p, &error));
        QVERIFY(!r);
        QVERIFY(!error.isEmpty());
    }
    void psdStack_data() {
        QTest::addColumn<QString>("extension");
        QTest::addColumn<int>("depth");
        QTest::newRow("psd8") << QString("psd") << 8;
        QTest::newRow("psb8") << QString("psb") << 8;
        QTest::newRow("psd16") << QString("psd") << 16;
        QTest::newRow("psb32") << QString("psb") << 32;
    }
    void psdStack() {
        QFETCH(QString, extension);
        QFETCH(int, depth);
        QTemporaryDir dir;
        std::unique_ptr<Document> d(Document::create({31, 29}, Qt::transparent, depth));
        d->state.layers[0].pixels = TileImage::fromImage(pattern(d->state.size, depth));
        d->state.layers[0].name = "Background Ω";
        Layer group;
        group.id = 7;
        group.kind = LayerKind::Group;
        group.name = "Nested group";
        group.blendMode = "Pass Through";
        d->state.layers.append(group);
        Layer layer;
        layer.id = 8;
        layer.parentId = 7;
        layer.name = "Masked overlay";
        layer.pixels = TileImage::fromImage(pattern({13, 11}, depth));
        layer.offset = {4, 5};
        layer.opacity = 128 / 255.0;
        layer.fill = 192 / 255.0;
        layer.blendMode = "Screen";
        layer.mask = QImage(d->state.size, QImage::Format_Grayscale8);
        layer.mask.fill(184);
        layer.maskEnabled = false;
        layer.lockPosition = true;
        d->state.layers.append(layer);
        d->state.iccProfile = QColorSpace(QColorSpace::SRgb).iccProfile();
        QString error, path = dir.filePath("stack." + extension);
        QVERIFY2(FormatIO::savePsd(d.get(), path, &error), qPrintable(error));
        std::unique_ptr<Document> r(FormatIO::openPsd(path, &error));
        QVERIFY2(r, qPrintable(error));
        QCOMPARE(r->state.layers.size(), 3);
        QCOMPARE(r->state.bitDepth, depth);
        QCOMPARE(r->state.iccProfile, d->state.iccProfile);
        const auto &l = r->state.layers[2];
        QCOMPARE(l.name, layer.name);
        QCOMPARE(l.parentId, group.id);
        QCOMPARE(l.blendMode, layer.blendMode);
        QCOMPARE(l.opacity, layer.opacity);
        QCOMPARE(l.fill, layer.fill);
        QCOMPARE(l.offset, layer.offset);
        QVERIFY(l.lockPosition);
        QVERIFY(!l.maskEnabled);
        QVERIFY(identical(l.mask, normalizeMask(layer.mask, depth)));
        QVERIFY(identical(l.pixels.image(), layer.pixels.image()));
        QVERIFY(identical(r->state.layers[0].pixels.image(), d->state.layers[0].pixels.image()));
    }
    void independentPsdComposite_data() {
        QTest::addColumn<bool>("rle");
        QTest::newRow("raw") << false;
        QTest::newRow("packbits") << true;
    }
    void independentPsdComposite() {
        QFETCH(bool, rle);
        QTemporaryDir dir;
        QString path = dir.filePath("external.psd");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(externalCompositePsd(rle));
        f.close();
        QString error;
        std::unique_ptr<Document> d(FormatIO::openPsd(path, &error));
        QVERIFY2(d, qPrintable(error));
        auto image = d->state.layers[0].pixels.image();
        QCOMPARE(image.size(), QSize(3, 2));
        QCOMPARE(image.pixelColor(2, 1), QColor(9, 69, 129));
    }
    void malformedPsd() {
        QTemporaryDir dir;
        QString path = dir.filePath("bad.psd");
        QFile f(path);
        auto bytes = externalCompositePsd(true);
        bytes[40] = char(255);
        bytes[41] = char(255);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes);
        f.close();
        QString error;
        std::unique_ptr<Document> d(FormatIO::openPsd(path, &error));
        QVERIFY(!d);
        QVERIFY(!error.isEmpty());
    }
    void rasterRoundTrip_data() {
        QTest::addColumn<QString>("extension");
        QTest::addColumn<int>("depth");
        QTest::newRow("png") << QString("png") << 8;
        QTest::newRow("png16") << QString("png") << 16;
        QTest::newRow("tga") << QString("tga") << 8;
        QTest::newRow("tiff") << QString("tiff") << 8;
        QTest::newRow("tiff16") << QString("tiff") << 16;
    }
    void rasterRoundTrip() {
        QFETCH(QString, extension);
        QFETCH(int, depth);
        if (!FormatIO::writableFormats().contains(extension))
            QSKIP("Image codec plugin is not installed.");
        QTemporaryDir dir;
        std::unique_ptr<Document> d(Document::create({23, 19}, Qt::transparent, depth));
        d->state.layers[0].pixels = TileImage::fromImage(pattern(d->state.size, depth));
        QString e, p = dir.filePath("image." + extension);
        QVERIFY2(FormatIO::save(d.get(), p, &e), qPrintable(e));
        std::unique_ptr<Document> r(FormatIO::open(p, &e));
        QVERIFY2(r, qPrintable(e));
        QVERIFY(identical(d->state.layers[0].pixels.image(), r->state.layers[0].pixels.image()));
    }
    void lossyRaster_data() {
        QTest::addColumn<QString>("extension");
        QTest::newRow("jpeg") << QString("jpeg");
        QTest::newRow("webp") << QString("webp");
    }
    void gifFramesAsLayers() {
        QTemporaryDir dir;
        QString path = dir.filePath("frames.gif");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(twoFrameGif());
        f.close();
        QString error;
        std::unique_ptr<Document> d(FormatIO::open(path, &error));
        QVERIFY2(d, qPrintable(error));
        QCOMPARE(d->state.layers.size(), 2);
        QCOMPARE(d->state.layers[0].pixels.image().pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(d->state.layers[1].pixels.image().pixelColor(0, 0), QColor(Qt::blue));
    }
    void pdfRasterImport() {
        if (!FormatIO::readableFormats().contains("pdf"))
            QSKIP("Qt PDF module is not enabled in this build.");
        QTemporaryDir dir;
        QString path = dir.filePath("page.pdf");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(minimalPdf());
        f.close();
        QString error;
        std::unique_ptr<Document> d(FormatIO::openPdf(path, 150, &error));
        QVERIFY2(d, qPrintable(error));
        QCOMPARE(d->state.size, QSize(300, 150));
        QCOMPARE(d->state.resolution, 150.0);
        QCOMPARE(d->state.layers.size(), 1);
        QCOMPARE(d->composite().pixelColor(150, 75), QColor(Qt::red));
    }
    void lossyRaster() {
        QFETCH(QString, extension);
        if (!FormatIO::writableFormats().contains(extension))
            QSKIP("Image codec plugin is not installed.");
        QTemporaryDir dir;
        std::unique_ptr<Document> d(Document::create({64, 64}, Qt::transparent));
        QImage image(64, 64, QImage::Format_RGBA8888);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                image.setPixelColor(x, y, QColor(x * 4, y * 4, 120));
        d->state.layers[0].pixels.setImage(image);
        d->state.iccProfile = QColorSpace(QColorSpace::SRgb).iccProfile();
        QString error, path = dir.filePath("gradient." + extension);
        QVERIFY2(FormatIO::save(d.get(), path, &error, 96), qPrintable(error));
        std::unique_ptr<Document> r(FormatIO::open(path, &error));
        QVERIFY2(r, qPrintable(error));
        const auto result = r->composite();
        double sum = 0;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                auto a = image.pixelColor(x, y), b = result.pixelColor(x, y);
                sum += std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) +
                       std::abs(a.blue() - b.blue());
            }
        QVERIFY(sum / (64 * 64 * 3) < 5.0);
        QVERIFY(!r->state.iccProfile.isEmpty());
    }
    void psdRecordsAreBottomToTopLikePhotoshop() {
        // Photoshop writes layer records bottom-to-top with a group's end marker before its children.
        // Without Serika's private ordering tags, the stack must come from that layout alone.
        QTemporaryDir dir;
        std::unique_ptr<Document> d(Document::create({6, 4}, Qt::white));
        d->state.layers[0].name = "Paper";
        Layer group;
        group.id = 20;
        group.kind = LayerKind::Group;
        group.name = "Details";
        d->state.layers.append(group);
        Layer inside;
        inside.id = 21;
        inside.parentId = 20;
        inside.name = "Inside";
        QImage blue({6, 4}, QImage::Format_RGBA8888);
        blue.fill(Qt::blue);
        inside.pixels = TileImage::fromImage(blue);
        d->state.layers.append(inside);
        Layer top;
        top.id = 22;
        top.name = "Top";
        QImage red({2, 2}, QImage::Format_RGBA8888);
        red.fill(Qt::red);
        top.pixels = TileImage::fromImage(red);
        d->state.layers.append(top);
        QString error, path = dir.filePath("photoshop-order.psd");
        QVERIFY2(FormatIO::savePsd(d.get(), path, &error), qPrintable(error));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray bytes = f.readAll();
        f.close();
        // The first record is the bottom layer and the end marker precedes the group's children.
        QVERIFY(bytes.indexOf("Paper") < bytes.indexOf("</Layer group>"));
        QVERIFY(bytes.indexOf("</Layer group>") < bytes.indexOf("Inside"));
        QVERIFY(bytes.indexOf("Inside") < bytes.indexOf("Details"));
        QVERIFY(bytes.indexOf("Details") < bytes.indexOf("Top"));
        for (const QByteArray key : {"8BIMsPEi", "8BIMsPEd"})
            for (qsizetype at = bytes.indexOf(key); at >= 0; at = bytes.indexOf(key, at + 1))
                bytes.replace(at + 4, 4, "xSkp");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes);
        f.close();
        std::unique_ptr<Document> r(FormatIO::openPsd(path, &error));
        QVERIFY2(r, qPrintable(error));
        QCOMPARE(r->state.layers.size(), 4);
        QCOMPARE(r->state.layers[0].name, QString("Paper"));
        QCOMPARE(r->state.layers.last().name, QString("Top"));
        const int insideIndex = [&] {
            for (int i = 0; i < r->state.layers.size(); ++i)
                if (r->state.layers[i].name == "Inside")
                    return i;
            return -1;
        }();
        QVERIFY(insideIndex > 0);
        const int groupIndex = r->indexForId(r->state.layers[insideIndex].parentId);
        QVERIFY(groupIndex > insideIndex);
        QCOMPARE(r->state.layers[groupIndex].name, QString("Details"));
        QVERIFY(!r->importReport.join('\n').contains("Unmatched"));
        // The white paper stays at the bottom: blue shows through, the red top layer is on top.
        const QImage composite = r->composite();
        QCOMPARE(composite.pixelColor(5, 3), QColor(Qt::blue));
        QCOMPARE(composite.pixelColor(0, 0), QColor(Qt::red));
    }
    void psdUnknownTaggedBlock() {
        QTemporaryDir dir;
        std::unique_ptr<Document> d(Document::create({8, 7}, Qt::blue));
        QString error, path = dir.filePath("unknown.psd");
        QVERIFY(FormatIO::savePsd(d.get(), path, &error));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto bytes = f.readAll();
        f.close();
        auto at = bytes.indexOf("8BIMsPEi");
        QVERIFY(at >= 0);
        bytes.replace(at + 4, 4, "zZzz");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes);
        f.close();
        std::unique_ptr<Document> r(FormatIO::openPsd(path, &error));
        QVERIFY2(r, qPrintable(error));
        QVERIFY(r->importReport.join('\n').contains("zZzz"));
        QVERIFY(!r->state.layers[0].parameters["_psdTaggedBlocks"].toArray().isEmpty());
    }
    void psdEnabledMaskWithGroupOffset() {
        QTemporaryDir dir;
        std::unique_ptr<Document> d(Document::create({23, 17}, QColor(40, 60, 80)));
        Layer group;
        group.id = 7;
        group.kind = LayerKind::Group;
        group.name = "Moved group";
        group.offset = {3, 2};
        group.blendMode = "Pass Through";
        d->state.layers.append(group);
        Layer child;
        child.id = 8;
        child.parentId = 7;
        child.name = "Masked child";
        child.offset = {2, 1};
        QImage pixels(9, 7, QImage::Format_RGBA8888);
        pixels.fill(Qt::red);
        child.pixels.setImage(pixels);
        child.mask = QImage(pixels.size(), QImage::Format_Grayscale8);
        child.mask.fill(255);
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 4; ++x)
                child.mask.scanLine(y)[x] = 0;
        d->state.layers.append(child);
        const auto before = d->composite();
        QString error, path = dir.filePath("masked.psd");
        QVERIFY(FormatIO::savePsd(d.get(), path, &error));
        std::unique_ptr<Document> r(FormatIO::openPsd(path, &error));
        QVERIFY2(r, qPrintable(error));
        QCOMPARE(r->composite(), before);
    }
    void retainedMaskAndSmartGraph_data() {
        QTest::addColumn<QString>("extension");
        QTest::addColumn<int>("depth");
        QTest::newRow("native16") << QString("spe") << 16;
        QTest::newRow("native32") << QString("speb") << 32;
        QTest::newRow("psd16") << QString("psd") << 16;
        QTest::newRow("psb32") << QString("psb") << 32;
    }
    void retainedMaskAndSmartGraph() {
        QFETCH(QString, extension);
        QFETCH(int, depth);
        QTemporaryDir dir;
        std::unique_ptr<Document> doc(Document::create({24, 18}, Qt::transparent, depth));
        Layer group;
        group.id = 41;
        group.kind = LayerKind::Group;
        group.blendMode = "Pass Through";
        group.offset = {2, 3};
        doc->state.layers.append(group);
        Layer child;
        child.id = 42;
        child.parentId = group.id;
        child.offset = {1, 2};
        child.pixels.setImage(pattern({9, 7}, depth));
        child.mask = makeMask({18, 14}, depth, .73123);
        setMaskSample(child.mask, 0, 0, depth == 32 ? 1e-8 : 1 / 65535.);
        setMaskSample(child.mask, 1, 0, 12345 / 65535.);
        child.maskDensity = .63;
        child.maskFeather = 1.5;
        child.maskLinked = false;
        child.maskOffset = {-1.25, 2.5};
        child.vectorMask.moveTo(0, 0);
        child.vectorMask.cubicTo(3, 2, 9, 0, 18, 4);
        child.vectorMask.lineTo(18, 14);
        child.vectorMask.lineTo(0, 14);
        child.vectorMask.closeSubpath();
        child.vectorMaskDensity = .85;
        child.vectorMaskFeather = .75;
        child.vectorMaskLinked = false;
        child.vectorMaskOffset = {.5, -.25};
        child.opacity = .731234;
        child.fill = .82345;
        doc->state.layers.append(child);
        doc->state.activeIndex = 2;
        doc->addSmartFilter("Exposure", {{"exposure", 1}});
        doc->activeLayer()->parameters["contentTransform"] = QJsonArray{2, 0, 0, 0, 2, 0, 0, 0, 1};
        const Layer original = *doc->activeLayer();
        const QImage originalComposite = doc->composite();
        const QImage originalSource = original.pixels.image();
        QString error;
        const QString path = dir.filePath("retained." + extension);
        const bool native = extension == "spe" || extension == "speb";
        QVERIFY2(native ? FormatIO::saveNative(doc.get(), path, &error)
                        : FormatIO::savePsd(doc.get(), path, &error),
                 qPrintable(error));
        std::unique_ptr<Document> loaded(native ? FormatIO::openNative(path, &error)
                                                : FormatIO::openPsd(path, &error));
        QVERIFY2(loaded, qPrintable(error));
        const Layer &restored = loaded->state.layers[loaded->indexForId(original.id)];
        QCOMPARE(restored.kind, LayerKind::SmartObject);
        QCOMPARE(restored.pixels.size, QSize(9, 7));
        QCOMPARE(loaded->layerImage(restored).size(), QSize(18, 14));
        QVERIFY(identical(restored.pixels.image(), originalSource));
        QVERIFY(identical(restored.mask, original.mask));
        QCOMPARE(restored.maskDensity, original.maskDensity);
        QCOMPARE(restored.maskFeather, original.maskFeather);
        QCOMPARE(restored.maskLinked, original.maskLinked);
        QCOMPARE(restored.maskOffset, original.maskOffset);
        QCOMPARE(restored.vectorMask, original.vectorMask);
        QCOMPARE(restored.vectorMaskOffset, original.vectorMaskOffset);
        QCOMPARE(restored.vectorMaskFeather, original.vectorMaskFeather);
        QCOMPARE(restored.smartFilters, original.smartFilters);
        QCOMPARE(restored.parameters, original.parameters);
        QCOMPARE(restored.opacity, original.opacity);
        QCOMPARE(restored.fill, original.fill);
        QVERIFY(identical(loaded->composite(), originalComposite));
        loaded->setActiveIndex(loaded->indexForId(original.id));
        auto filter = loaded->activeLayer()->smartFilters[0].toObject();
        filter["enabled"] = false;
        QVERIFY(loaded->updateSmartFilter(0, filter));
        QVERIFY(!identical(loaded->composite(), originalComposite));
        QVERIFY(identical(loaded->activeLayer()->pixels.image(), originalSource));
    }
    void maskSampleRoundtrip_data() {
        QTest::addColumn<int>("depth");
        QTest::newRow("16-bit") << 16;
        QTest::newRow("float") << 32;
    }
    void maskSampleRoundtrip() {
        QFETCH(int, depth);
        QTemporaryDir dir;
        std::unique_ptr<Document> doc(Document::create({8, 3}, Qt::red, depth));
        auto *layer = doc->activeLayer();
        layer->mask = makeMask(doc->state.size, depth, 1);
        const qreal low = depth == 32 ? 1e-8 : 1 / 65535.;
        setMaskSample(layer->mask, 1, 0, low);
        setMaskSample(layer->mask, 2, 0, 12345 / 65535.);
        QString error;
        const QString path = dir.filePath("coverage.psd");
        QVERIFY2(FormatIO::savePsd(doc.get(), path, &error), qPrintable(error));
        std::unique_ptr<Document> loaded(FormatIO::openPsd(path, &error));
        QVERIFY2(loaded, qPrintable(error));
        const QImage &mask = loaded->activeLayer()->mask;
        QCOMPARE(mask.format(), depth == 32 ? QImage::Format_RGBA32FPx4 : QImage::Format_Grayscale16);
        QVERIFY(identical(mask, layer->mask));
        QVERIFY(std::abs(maskSample(mask, 1, 0) - low) < (depth == 32 ? 1e-14 : 1e-12));
        QVERIFY(identical(loaded->composite(), doc->composite()));
    }
    void vectorExtrasRejectInvalidCurveControls() {
        Layer original;
        original.vectorMask.addRect(0, 0, 5, 5);
        auto extras = io::layerExtras(original);
        for (const QJsonValue &bad :
             {QJsonValue(std::numeric_limits<double>::infinity()), QJsonValue("bad"), QJsonValue(1e300)}) {
            extras["vectorMask"] =
                QJsonObject{{"elements", QJsonArray{QJsonArray{0, 0, 0}, QJsonArray{2, 1, 1},
                                                    QJsonArray{3, bad, 2}, QJsonArray{3, 3, 3}}}};
            Layer restored = original;
            QVERIFY(!io::restoreLayerExtras(restored, extras));
            QCOMPARE(restored.vectorMask, original.vectorMask);
        }
    }
    void independentPsdMaskDepthAndNegativeExtent_data() {
        QTest::addColumn<int>("depth");
        QTest::newRow("16-bit") << 16;
        QTest::newRow("float") << 32;
    }
    void independentPsdMaskDepthAndNegativeExtent() {
        QFETCH(int, depth);
        QTemporaryDir dir;
        const QString path = dir.filePath("external-mask.psd");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(externalMaskedPsd(depth));
        file.close();
        QString error;
        std::unique_ptr<Document> loaded(FormatIO::openPsd(path, &error));
        QVERIFY2(loaded, qPrintable(error));
        const Layer &layer = loaded->state.layers[0];
        QCOMPARE(layer.mask.format(), depth == 16 ? QImage::Format_Grayscale16 : QImage::Format_RGBA32FPx4);
        QCOMPARE(layer.maskOffset, QPointF(-1, 0));
        QVERIFY(!layer.maskLinked);
        QCOMPARE(layer.maskDensity, 128 / 255.);
        const qreal first = depth == 16 ? 12345 / 65535. : 1e-8;
        QVERIFY(std::abs(maskSample(layer.mask, 0, 0) - first) < (depth == 16 ? 1e-12 : 1e-14));
        const qreal second = depth == 16 ? 17345 / 65535. : .25;
        const qreal alpha = 1 - (128 / 255.) * (1 - second);
        const QImage composite = loaded->composite();
        if (depth == 16)
            QVERIFY(std::abs(composite.pixelColor(2, 1).alphaF() - alpha) < 2 / 65535.);
        else
            QVERIFY(std::abs(reinterpret_cast<const float *>(composite.constScanLine(1))[2 * 4 + 3] - alpha) <
                    1e-7);
    }
    void independentZipComposite() {
        QTemporaryDir dir;
        auto raw = externalCompositePsd(false);
        auto compressed = qCompress(raw.mid(40), 6).mid(4);
        auto bytes = raw.left(38);
        be16(bytes, 2);
        bytes += compressed;
        QString error, path = dir.filePath("zip.psd");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes);
        f.close();
        std::unique_ptr<Document> r(FormatIO::openPsd(path, &error));
        if (!r && error.contains("unsupported"))
            QSKIP("PSD ZIP support is disabled in this build.");
        QVERIFY2(r, qPrintable(error));
        QCOMPARE(r->composite().pixelColor(2, 1), QColor(9, 69, 129));
    }
    void developControls() {
        auto image = pattern({35, 27}, 16);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        auto gamma = developImage(image, {{"gamma", 2}}, 16);
        QVERIFY(gamma.pixelColor(5, 5).redF() > image.pixelColor(5, 5).redF());
        auto hue = developImage(image, {{"hue", 120}}, 16);
        QVERIFY(!identical(hue, image));
        auto rotated = developImage(image, {{"angle", 20}}, 16);
        QCOMPARE(rotated.size(), image.size());
        QVERIFY(rotated.pixelColor(0, 0).alpha() == 0);
        auto wide = developImage(image, {{"profile", "ProPhoto RGB"}}, 16);
        QCOMPARE(wide.colorSpace(), QColorSpace(QColorSpace::ProPhotoRgb));
    }
    void hdrPreservesRange() {
        QTemporaryDir dir;
        std::unique_ptr<Document> d(Document::create({16, 12}, Qt::transparent, 32));
        auto pixels = pattern(d->state.size, 32);
        pixels.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
        d->state.iccProfile = pixels.colorSpace().iccProfile();
        for (int y = 0; y < pixels.height(); ++y) {
            auto p = reinterpret_cast<float *>(pixels.scanLine(y));
            for (int x = 0; x < pixels.width(); ++x)
                p[x * 4] = qMax(0.f, p[x * 4]);
        }
        d->state.layers[0].pixels = TileImage::fromImage(pixels);
        QString e, path = dir.filePath("linear.hdr");
        QVERIFY2(FormatIO::save(d.get(), path, &e), qPrintable(e));
        std::unique_ptr<Document> r(FormatIO::open(path, &e));
        QVERIFY2(r, qPrintable(e));
        QCOMPARE(r->state.bitDepth, 32);
        auto result = r->state.layers[0].pixels.image();
        auto a = reinterpret_cast<const float *>(pixels.constScanLine(5)),
             b = reinterpret_cast<const float *>(result.constScanLine(5));
        for (int x = 0; x < pixels.width(); ++x)
            QVERIFY(std::abs(a[x * 4] - b[x * 4]) < .015f);
        QVERIFY(b[15 * 4] > 2);
    }
    void raw16Develop() {
        if (!FormatIO::rawAvailable())
            QSKIP("LibRaw is not enabled in this build.");
        QTemporaryDir dir;
        QString path = dir.filePath("synthetic.dng");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(syntheticDng());
        f.close();
        QString e;
        std::unique_ptr<Document> d(FormatIO::open(path, &e));
        QVERIFY2(d, qPrintable(e));
        QCOMPARE(d->state.bitDepth, 16);
        QVERIFY(d->state.metadata["rawPendingDevelop"].toBool());
        QCOMPARE(d->state.layers[0].pixels.format, QImage::Format_RGBA64);
        auto source = d->state.layers[0].pixels.image();
        auto developed = developImage(source, {{"exposure", 1.0}}, 16);
        QCOMPARE(developed.format(), QImage::Format_RGBA64);
        QVERIFY(developed.pixelColor(20, 20).redF() > source.pixelColor(20, 20).redF());
    }
    void filterProperties() {
        auto image = pattern({35, 27}, 16);
        auto blur = applyFilter(image, "Gaussian Blur", {{"radius", 2}});
        QCOMPARE(blur.size(), image.size());
        QCOMPARE(blur.format(), QImage::Format_RGBA64);
        QVERIFY(!identical(image, blur));
        QImage flat(21, 19, QImage::Format_RGBA64);
        flat.fill(QColor(90, 120, 150, 255));
        auto blurred = applyFilter(flat, "Gaussian Blur", {{"radius", 4}});
        QVERIFY(std::abs(blurred.pixelColor(10, 9).red() - 90) <= 1);
        auto high = applyFilter(flat, "High Pass", {{"radius", 3}});
        QVERIFY(std::abs(high.pixelColor(8, 7).red() - 128) <= 1);
        auto first = applyFilter(image, "Add Noise", {{"amount", 20}, {"seed", 123}}),
             second = applyFilter(image, "Add Noise", {{"amount", 20}, {"seed", 123}});
        QVERIFY(identical(first, second));
        QVERIFY(!identical(first, image));
    }
};
QTEST_MAIN(IoTests)
#include "io_tests.moc"
