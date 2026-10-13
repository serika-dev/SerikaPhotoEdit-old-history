#include "document/Document.h"
#include <QJsonArray>
#include <QtTest>
#include <cmath>
#include <memory>

using namespace serika;
namespace {
Layer raster(quint64 id, QSize size, QColor color, int bitDepth = 8) {
    Layer layer;
    layer.id = id;
    QImage image(size, bitDepth == 16 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    image.fill(color);
    layer.pixels = TileImage::fromImage(image);
    return layer;
}
QImage rectangleMask(QSize size, QRect rect, int value = 255) {
    QImage mask(size, QImage::Format_Grayscale8);
    mask.fill(0);
    for (int y = rect.top(); y <= rect.bottom(); ++y)
        for (int x = rect.left(); x <= rect.right(); ++x)
            if (mask.rect().contains(x, y))
                mask.scanLine(y)[x] = uchar(value);
    return mask;
}
} // namespace
class CoreTests : public QObject {
    Q_OBJECT
  private slots:
    void sparseTilesCopyOnWrite() {
        TileImage image;
        image.size = {600, 600};
        image.paint({250, 250, 20, 20}, [](QPainter &p) { p.fillRect(QRect(250, 250, 20, 20), Qt::red); });
        QCOMPARE(image.tiles.size(), 4);
        TileImage snapshot = image;
        const auto oldTile = image.tiles.value(TileImage::key(0, 0)).constBits();
        image.paint({251, 251, 2, 2}, [](QPainter &p) { p.fillRect(QRect(251, 251, 2, 2), Qt::blue); });
        QVERIFY(image.tiles.value(TileImage::key(0, 0)).constBits() != oldTile);
        QCOMPARE(image.tiles.value(TileImage::key(1, 1)).constBits(),
                 snapshot.tiles.value(TileImage::key(1, 1)).constBits());
        QCOMPARE(snapshot.region({251, 251, 1, 1}).pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(image.region({251, 251, 1, 1}).pixelColor(0, 0), QColor(Qt::blue));
        image.paint({0, 0, 600, 600}, [](QPainter &p) {
            p.setCompositionMode(QPainter::CompositionMode_Clear);
            p.fillRect(QRect(0, 0, 600, 600), Qt::transparent);
        });
        QVERIFY(image.empty());
    }
    void sixteenBitTilesPreservePrecision() {
        QImage source(300, 2, QImage::Format_RGBA64);
        source.fill(Qt::transparent);
        reinterpret_cast<QRgba64 *>(source.scanLine(1))[299] =
            QRgba64::fromRgba64(12345, 23456, 34567, 45678);
        const TileImage tiles = TileImage::fromImage(source);
        QCOMPARE(tiles.tiles.size(), 1);
        QCOMPARE(tiles.format, QImage::Format_RGBA64);
        const QImage roundtrip = tiles.image();
        const auto pixel = reinterpret_cast<const QRgba64 *>(roundtrip.constScanLine(1))[299];
        // Source composition in the tile read path retains all four 16-bit channels.
        QCOMPARE(pixel.red(), quint16(12345));
        QCOMPARE(pixel.green(), quint16(23456));
        QCOMPARE(pixel.blue(), quint16(34567));
        QCOMPARE(pixel.alpha(), quint16(45678));
    }
    void growingEdgeTilePreservesPixels() {
        QImage original(40, 40, QImage::Format_RGBA64);
        original.fill(Qt::transparent);
        reinterpret_cast<QRgba64 *>(original.scanLine(1))[1] =
            QRgba64::fromRgba64(12345, 23456, 34567, 65535);
        TileImage tiles = TileImage::fromImage(original);
        tiles.size = {100, 100};
        tiles.paint({80, 80, 1, 1}, [](QPainter &p) { p.fillRect(QRect(80, 80, 1, 1), Qt::red); });
        QCOMPARE(tiles.tiles.value(TileImage::key(0, 0)).size(), QSize(100, 100));
        const QImage output = tiles.image();
        const QRgba64 pixel = reinterpret_cast<const QRgba64 *>(output.constScanLine(1))[1];
        QCOMPARE(pixel.red(), quint16(12345));
        QCOMPARE(output.pixelColor(80, 80), QColor(Qt::red));
    }
    void historyCoalescesAndTracksSavedRevision() {
        std::unique_ptr<Document> d(Document::create({32, 32}, Qt::transparent));
        QVERIFY(!d->isModified());
        d->beginTransaction("Brush stroke");
        for (int i = 0; i < 10; ++i) {
            d->activeLayer()->pixels.paint({i, 0, 1, 1},
                                           [i](QPainter &p) { p.fillRect(QRect(i, 0, 1, 1), Qt::red); });
            d->touch();
        }
        d->endTransaction();
        QCOMPARE(d->historyNames(), QStringList{"Brush stroke"});
        QVERIFY(d->isModified());
        d->markSaved();
        QVERIFY(!d->isModified());
        d->undo();
        QVERIFY(d->isModified());
        QVERIFY(d->activeLayer()->pixels.empty());
        d->redo();
        QVERIFY(!d->isModified());
        QCOMPARE(d->composite().pixelColor(0, 0), QColor(Qt::red));
        d->undo();
        d->addLayer("Alternative");
        QVERIFY(!d->canRedo());
        QVERIFY(d->isModified());
        d->undo();
        QVERIFY(d->isModified());
    }
    void cancelRestoresTilesAndRevision() {
        std::unique_ptr<Document> d(Document::create({8, 8}, Qt::white));
        d->markSaved();
        d->beginTransaction("Canceled gesture");
        d->activeLayer()->pixels.paint({0, 0, 8, 8},
                                       [](QPainter &p) { p.fillRect(QRect(0, 0, 8, 8), Qt::black); });
        d->touch();
        QVERIFY(d->isModified());
        d->cancelTransaction();
        QVERIFY(!d->isModified());
        QVERIFY(!d->canUndo());
        QCOMPARE(d->composite().pixelColor(0, 0), QColor(Qt::white));
        d->beginTransaction("No operation");
        d->endTransaction();
        QVERIFY(!d->canUndo());
    }
    void clearingHistoryPreservesStateAndSavedRevision() {
        std::unique_ptr<Document> document(Document::create({8, 8}, Qt::white));
        document->addLayer("Editable");
        document->markSaved();
        document->addLayer("Temporary");
        document->undo();
        QVERIFY(document->canRedo());
        QVERIFY(!document->isModified());
        document->clearHistory();
        QVERIFY(!document->canUndo());
        QVERIFY(!document->canRedo());
        QVERIFY(document->historyNames().isEmpty());
        QVERIFY(!document->isModified());
        QCOMPARE(document->state.layers.size(), 2);
        document->beginTransaction("Current gesture");
        document->activeLayer()->pixels.paint({0, 0, 1, 1},
                                              [](QPainter &p) { p.fillRect(QRect(0, 0, 1, 1), Qt::red); });
        document->touch();
        document->clearHistory();
        QVERIFY(document->isModified());
        QVERIFY(!document->canUndo());
        QCOMPARE(document->composite().pixelColor(0, 0), QColor(Qt::red));
        document->addLayer("Next edit");
        QVERIFY(document->canUndo());
        document->undo();
        QCOMPARE(document->state.layers.size(), 2);
        QVERIFY(document->isModified());
    }
    void selectionBooleanOperations() {
        std::unique_ptr<Document> d(Document::create({8, 8}, Qt::transparent));
        const QImage left = rectangleMask({8, 8}, {0, 0, 4, 8}), middle = rectangleMask({8, 8}, {2, 0, 4, 8});
        d->setSelection(left);
        QCOMPARE(d->selectionBounds(), QRect(0, 0, 4, 8));
        d->setSelection(middle, "add");
        QCOMPARE(d->selectionBounds(), QRect(0, 0, 6, 8));
        d->setSelection(middle, "subtract");
        QCOMPARE(d->selectionBounds(), QRect(0, 0, 2, 8));
        d->setSelection(left);
        d->setSelection(middle, "intersect");
        QCOMPARE(d->selectionBounds(), QRect(2, 0, 2, 8));
        d->invertSelection();
        QCOMPARE(d->state.selection.constScanLine(0)[2], uchar(0));
        QCOMPARE(d->state.selection.constScanLine(0)[7], uchar(255));
        d->deselect();
        QVERIFY(!d->hasSelection());
        d->undo();
        QVERIFY(d->hasSelection());
        d->setSelection(rectangleMask({8, 8}, {1, 1, 1, 1}, 128));
        d->setSelection(rectangleMask({8, 8}, {1, 1, 1, 1}, 64), "subtract");
        QCOMPARE(d->state.selection.constScanLine(1)[1], uchar(64));
    }
    void blendOracle_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<double>("expected");
        QTest::addColumn<int>("depth");
        const QVector<double> expected{.75, .75, .25,  .1875, 0,      0,    .25, .75, .8125,
                                       1,   1,   .75,  .375,  .375,   .625, .5,  .75, .5,
                                       1,   .5,  .625, 0,     1. / 3, .25,  .25, .25, .75};
        const auto names = blendModeNames();
        QCOMPARE(names.size(), 27);
        QCOMPARE(expected.size(), names.size());
        for (int depth : {8, 16})
            for (int i = 0; i < names.size(); ++i)
                QTest::newRow(qPrintable(QString("%1-%2").arg(names[i]).arg(depth)))
                    << names[i] << expected[i] << depth;
    }
    void blendOracle() {
        QFETCH(QString, mode);
        QFETCH(double, expected);
        QFETCH(int, depth);
        const QColor b = QColor::fromRgbF(.25, .25, .25), s = QColor::fromRgbF(.75, .75, .75);
        const QColor blend = blendColor(b, s, mode);
        QVERIFY2(std::abs(blend.redF() - expected) < .00005,
                 qPrintable(QString("%1: %2 != %3").arg(mode).arg(blend.redF()).arg(expected)));
        DocumentState state;
        state.size = {1, 1};
        state.bitDepth = depth;
        state.layers = {raster(1, {1, 1}, b, depth), raster(2, {1, 1}, s, depth)};
        state.layers[1].blendMode = mode;
        const QImage result = compositeDocument(state);
        QCOMPARE(result.format(), depth == 16 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
        const double tolerance = depth == 16 ? .00006 : .006;
        QVERIFY2(std::abs(result.pixelColor(0, 0).redF() - expected) < tolerance,
                 qPrintable(QString("%1 %2-bit: %3 != %4")
                                .arg(mode)
                                .arg(depth)
                                .arg(result.pixelColor(0, 0).redF())
                                .arg(expected)));
    }
    void alphaCompositingAndMasks() {
        DocumentState state;
        state.size = {2, 1};
        state.layers = {raster(1, {2, 1}, Qt::blue), raster(2, {2, 1}, Qt::red)};
        state.layers[1].opacity = .5;
        state.layers[1].mask = rectangleMask({2, 1}, {0, 0, 1, 1});
        QImage result = compositeDocument(state);
        QVERIFY(std::abs(result.pixelColor(0, 0).redF() - .5) < .006);
        QCOMPARE(result.pixelColor(1, 0), QColor(Qt::blue));
        state.layers[1].maskEnabled = false;
        result = compositeDocument(state);
        QCOMPARE(result.pixelColor(0, 0), result.pixelColor(1, 0));
        state.layers[0].visible = false;
        result = compositeDocument(state);
        QVERIFY(std::abs(result.pixelColor(0, 0).alphaF() - .5) < .006);
        QCOMPARE(result.pixelColor(0, 0).red(), 255);
    }
    void clippingAndGroups() {
        DocumentState state;
        state.size = {4, 1};
        Layer base = raster(1, {4, 1}, Qt::transparent);
        base.pixels.paint({0, 0, 2, 1}, [](QPainter &p) { p.fillRect(QRect(0, 0, 2, 1), Qt::white); });
        Layer clipped = raster(2, {4, 1}, Qt::red);
        clipped.clipped = true;
        state.layers = {base, clipped};
        QImage result = compositeDocument(state);
        QCOMPARE(result.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(result.pixelColor(3, 0).alpha(), 0);
        Layer group;
        group.id = 3;
        group.kind = LayerKind::Group;
        group.blendMode = "Pass Through";
        group.opacity = .5;
        state.layers = {raster(4, {4, 1}, Qt::blue), group, raster(5, {4, 1}, Qt::red)};
        state.layers[2].parentId = 3;
        result = compositeDocument(state);
        QVERIFY(std::abs(result.pixelColor(0, 0).redF() - .5) < .006);
        QVERIFY(std::abs(result.pixelColor(0, 0).blueF() - .5) < .006);
        state.layers[1].visible = false;
        QCOMPARE(compositeDocument(state).pixelColor(0, 0), QColor(Qt::blue));
    }
    void passThroughAdjustmentSeesBackdrop() {
        DocumentState state;
        state.size = {1, 1};
        Layer group;
        group.id = 2;
        group.kind = LayerKind::Group;
        group.blendMode = "Pass Through";
        Layer adjustment;
        adjustment.id = 3;
        adjustment.kind = LayerKind::Adjustment;
        adjustment.adjustment = "Invert";
        adjustment.parentId = 2;
        state.layers = {raster(1, {1, 1}, Qt::red), group, adjustment};
        QCOMPARE(compositeDocument(state).pixelColor(0, 0), QColor(Qt::cyan));
        state.layers[1].blendMode = "Normal";
        QCOMPARE(compositeDocument(state).pixelColor(0, 0), QColor(Qt::red));
    }
    void clippingPreservesSemitransparentBaseCoverage() {
        DocumentState state;
        state.size = {1, 1};
        state.bitDepth = 16;
        Layer base = raster(1, {1, 1}, QColor::fromRgbF(1, 0, 0, .5), 16),
              upper = raster(2, {1, 1}, Qt::blue, 16);
        upper.clipped = true;
        state.layers = {base, upper};
        QColor output = compositeDocument(state).pixelColor(0, 0);
        QVERIFY(std::abs(output.alphaF() - .5) < .0001);
        QVERIFY(output.blueF() > .9999);
        QVERIFY(output.redF() < .0001);
        state.layers.prepend(raster(3, {1, 1}, Qt::green, 16));
        output = compositeDocument(state).pixelColor(0, 0);
        QVERIFY(std::abs(output.greenF() - .5) < .0001);
        QVERIFY(std::abs(output.blueF() - .5) < .0001);
        QVERIFY(output.redF() < .0001);
        state.layers[1].opacity = .5;
        output = compositeDocument(state).pixelColor(0, 0);
        QVERIFY(std::abs(output.greenF() - .75) < .0001);
        QVERIFY(std::abs(output.blueF() - .25) < .0001);
    }
    void sixteenBitLayerMaskPreservesPrecision() {
        DocumentState state;
        state.size = {1, 1};
        state.bitDepth = 16;
        state.layers = {raster(1, {1, 1}, Qt::red, 16)};
        state.layers[0].mask = QImage(1, 1, QImage::Format_Grayscale16);
        reinterpret_cast<quint16 *>(state.layers[0].mask.scanLine(0))[0] = 12345;
        const QImage image = compositeDocument(state);
        const QRgba64 pixel = reinterpret_cast<const QRgba64 *>(image.constScanLine(0))[0];
        QCOMPARE(pixel.alpha(), quint16(12345));
    }
    void layerStyleStrokeAndFillOpacity() {
        DocumentState state;
        state.size = {32, 32};
        Layer layer = raster(1, {32, 32}, Qt::transparent);
        layer.pixels.paint({10, 10, 10, 10}, [](QPainter &p) { p.fillRect(QRect(10, 10, 10, 10), Qt::red); });
        layer.fill = 0;
        layer.effects = {{"stroke", QJsonObject{{"size", 3}, {"color", "#00ff00"}, {"opacity", 1}}}};
        state.layers = {layer};
        const QImage output = compositeDocument(state);
        QVERIFY(output.pixelColor(9, 15).green() > 200);
        QCOMPARE(output.pixelColor(15, 15).alpha(), 0);
        QCOMPARE(output.pixelColor(1, 1).alpha(), 0);
    }
    void groupOffsetMovesDescendants() {
        DocumentState state;
        state.size = {4, 1};
        Layer group;
        group.id = 1;
        group.kind = LayerKind::Group;
        group.blendMode = "Pass Through";
        group.offset = {2, 0};
        Layer child = raster(2, {1, 1}, Qt::red);
        child.parentId = 1;
        state.layers = {group, child};
        const QImage output = compositeDocument(state);
        QCOMPARE(output.pixelColor(2, 0), QColor(Qt::red));
        QCOMPARE(output.pixelColor(0, 0).alpha(), 0);
    }
    void artboardClipsChildrenToMatteRect() {
        DocumentState state;
        state.size = {4, 4};
        Layer artboard;
        artboard.id = 1;
        artboard.kind = LayerKind::Artboard;
        artboard.blendMode = "Pass Through";
        artboard.parameters = {{"rect", QJsonArray{1, 1, 2, 2}}};
        artboard.color = Qt::white;
        Layer child = raster(2, {4, 4}, Qt::red);
        child.parentId = 1;
        state.layers = {artboard, child};
        const QImage output = compositeDocument(state);
        QCOMPARE(output.pixelColor(1, 1), QColor(Qt::red));
        QCOMPARE(output.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(output.pixelColor(3, 3).alpha(), 0);
    }
    void importedIdsDoNotCollide() {
        Document document;
        document.state.size = {1, 1};
        document.state.layers = {raster(91, {1, 1}, Qt::red)};
        QCOMPARE(document.addLayer("New"), quint64(92));
        document.duplicateActiveLayer();
        QCOMPARE(document.activeLayer()->id, quint64(93));
        document.flatten();
        QCOMPARE(document.activeLayer()->id, quint64(94));
    }
    void regionalGestureCompositeMatchesFullOracle() {
        std::unique_ptr<Document> document(Document::create({700, 520}, QColor(20, 60, 120), 16));
        document->addLayer("Paint");
        document->activeLayer()->offset = {-10, 12};
        document->activeLayer()->blendMode = "Screen";
        document->activeLayer()->opacity = .7;
        document->activeLayer()->mask = rectangleMask({700, 520}, {0, 0, 400, 500}, 130);
        document->touch();
        document->composite();
        document->beginTransaction("Regional gesture");
        for (int dab = 0; dab < 3; ++dab) {
            const QRect rect(250 + dab * 5, 250 + dab * 8, 25, 25);
            document->activeLayer()->pixels.paint(
                rect, [rect](QPainter &p) { p.fillRect(rect, QColor(240, 50, 15, 190)); });
            document->touch();
            const QImage cached = document->composite(),
                         oracle = compositeDocument(document->state, document->blendLinear);
            QCOMPARE(cached, oracle);
        }
        document->endTransaction();
        QCOMPARE(document->composite(), compositeDocument(document->state));
        document->undo();
        QCOMPARE(document->composite(), compositeDocument(document->state));
        document->beginTransaction("Global change");
        document->activeLayer()->offset += {1, 0};
        document->touch();
        QCOMPARE(document->composite(), compositeDocument(document->state));
        document->cancelTransaction();
        document->blendLinear = true;
        QCOMPARE(document->composite(), compositeDocument(document->state, true));
    }
    void adjustmentsKnownVectors() {
        QImage image(1, 1, QImage::Format_RGBA64);
        image.fill(QColor::fromRgbF(.25, .5, .75, .5));
        QColor out = applyAdjustment(image, "Invert", {}).pixelColor(0, 0);
        QVERIFY(std::abs(out.redF() - .75) < .0001);
        QVERIFY(std::abs(out.alphaF() - .5) < .0001);
        out = applyAdjustment(image, "Levels", {{"inputBlack", 0}, {"inputWhite", 255}, {"gamma", 2}})
                  .pixelColor(0, 0);
        QVERIFY(std::abs(out.redF() - .5) < .0001);
        out =
            applyAdjustment(image, "Curves", {{"points", QJsonArray{QJsonArray{0, 255}, QJsonArray{255, 0}}}})
                .pixelColor(0, 0);
        QVERIFY(std::abs(out.redF() - .75) < .0001);
        image.fill(Qt::red);
        out = applyAdjustment(image, "Hue/Saturation", {{"hue", 120}}).pixelColor(0, 0);
        QVERIFY(out.greenF() > .999);
        QVERIFY(out.redF() < .001);
        out = applyAdjustment(image, "Hue/Saturation", {{"saturation", -100}}).pixelColor(0, 0);
        QCOMPARE(out.red(), out.green());
        QCOMPARE(out.green(), out.blue());
        image.fill(QColor::fromRgbF(.4, .5, .6));
        out = applyAdjustment(image, "Posterize", {{"levels", 2}}).pixelColor(0, 0);
        QCOMPARE(out.red(), 0);
        QCOMPARE(out.blue(), 255);
    }
    void adjustmentLayerMaskAndUndo() {
        std::unique_ptr<Document> d(Document::create({2, 1}, Qt::red, 16));
        d->addAdjustment("Invert");
        d->activeLayer()->mask = rectangleMask({2, 1}, {0, 0, 1, 1});
        d->touch();
        QImage result = d->composite();
        QCOMPARE(result.pixelColor(0, 0), QColor(Qt::cyan));
        QCOMPARE(result.pixelColor(1, 0), QColor(Qt::red));
        d->undo();
        QCOMPARE(d->state.layers.size(), 1);
        QCOMPARE(d->composite().pixelColor(0, 0), QColor(Qt::red));
    }
    void compositorFastPathsMatchTheGeneralPath() {
        // Plain pixel, masked pixel and text layers and full-strength pass-through groups take
        // direct paths. A disabled effect forces the general path without changing the look, so
        // the two documents must render the same (within one 8-bit step of rounding).
        std::unique_ptr<Document> fast(Document::create({300, 200}, QColor("#204060")));
        Layer group;
        group.id = 50;
        group.kind = LayerKind::Group;
        group.blendMode = "Pass Through";
        fast->state.layers.append(group);
        Layer plain;
        plain.id = 51;
        plain.parentId = 50;
        QImage red({90, 60}, QImage::Format_RGBA8888);
        red.fill(QColor(230, 40, 60, 200));
        plain.pixels = TileImage::fromImage(red);
        plain.offset = {20, 30};
        plain.opacity = 0.7;
        fast->state.layers.append(plain);
        Layer masked;
        masked.id = 52;
        QImage green({280, 150}, QImage::Format_RGBA8888);
        green.fill(QColor(40, 200, 90, 255));
        masked.pixels = TileImage::fromImage(green);
        masked.offset = {10, 25};
        masked.mask = QImage({280, 150}, QImage::Format_Grayscale8);
        for (int y = 0; y < 150; ++y)
            for (int x = 0; x < 280; ++x)
                masked.mask.scanLine(y)[x] = uchar((x * 255) / 279);
        masked.maskDensity = 0.8;
        fast->state.layers.append(masked);
        Layer text;
        text.id = 53;
        text.kind = LayerKind::Text;
        text.text = "Literature Club";
        text.font = QFont("Arial");
        text.font.setPixelSize(30);
        text.color = QColor("#fff0f8");
        text.offset = {40, 120};
        fast->state.layers.append(text);
        std::unique_ptr<Document> general(Document::create({300, 200}, QColor("#204060")));
        general->state.layers = fast->state.layers;
        for (auto &layer : general->state.layers)
            if (layer.kind != LayerKind::Group)
                layer.effects = QJsonObject{{"dropShadow", QJsonObject{{"enabled", false}}}};
        const QImage a = fast->composite().convertToFormat(QImage::Format_RGBA8888);
        const QImage b = general->composite().convertToFormat(QImage::Format_RGBA8888);
        QCOMPARE(a.size(), b.size());
        int worst = 0;
        for (int y = 0; y < a.height(); ++y)
            for (int x = 0; x < a.width() * 4; ++x)
                worst = std::max(worst, std::abs(int(a.constScanLine(y)[x]) - int(b.constScanLine(y)[x])));
        QVERIFY2(worst <= 1, qPrintable(QString("largest channel difference %1").arg(worst)));
    }
    void resizeCanvasCropAndImage() {
        std::unique_ptr<Document> d(Document::create({4, 4}, Qt::transparent));
        d->activeLayer()->pixels.paint({1, 1, 1, 1},
                                       [](QPainter &p) { p.fillRect(QRect(1, 1, 1, 1), Qt::red); });
        d->touch();
        d->resizeCanvas({6, 6}, {1, 1});
        QCOMPARE(d->composite().pixelColor(2, 2), QColor(Qt::red));
        d->crop({1, 1, 4, 4});
        QCOMPARE(d->composite().pixelColor(1, 1), QColor(Qt::red));
        d->resizeImage({8, 8}, Qt::FastTransformation);
        QCOMPARE(d->composite().pixelColor(2, 2), QColor(Qt::red));
        QCOMPARE(d->state.size, QSize(8, 8));
        d->undo();
        QCOMPARE(d->state.size, QSize(4, 4));
        d->undo();
        QCOMPARE(d->state.size, QSize(6, 6));
    }
    void vectorAndTypeRasterization() {
        std::unique_ptr<Document> d(Document::create({128, 64}, Qt::transparent));
        d->addLayer("Vector", LayerKind::Shape);
        d->activeLayer()->color = Qt::red;
        d->activeLayer()->shape.addRect(4, 4, 20, 20);
        d->touch();
        QCOMPARE(d->composite().pixelColor(10, 10), QColor(Qt::red));
        d->addLayer("Type", LayerKind::Text);
        d->activeLayer()->text = "Serika";
        d->activeLayer()->font.setPixelSize(24);
        d->activeLayer()->color = Qt::white;
        d->activeLayer()->offset = {30, 10};
        d->touch();
        QImage image = d->composite();
        bool found = false;
        for (int y = 10; y < 50; ++y)
            for (int x = 30; x < 128; ++x)
                if (image.pixelColor(x, y).red() > 200 && image.pixelColor(x, y).green() > 200)
                    found = true;
        QVERIFY(found);
    }
    void linearBlendOption() {
        DocumentState state;
        state.size = {1, 1};
        state.layers = {raster(1, {1, 1}, Qt::black), raster(2, {1, 1}, Qt::white)};
        state.layers[1].opacity = .5;
        const double gamma = compositeDocument(state, false).pixelColor(0, 0).redF(),
                     linear = compositeDocument(state, true).pixelColor(0, 0).redF();
        QVERIFY(std::abs(gamma - .5) < .006);
        QVERIFY(std::abs(linear - .735) < .006);
    }
    void allAdjustmentsKeepAlphaAndDepth() {
        QImage input(8, 4, QImage::Format_RGBA64);
        input.fill(QColor(50, 100, 180, 130));
        for (const auto &name : adjustmentNames()) {
            const QImage result = applyAdjustment(input, name, {});
            QCOMPARE(result.size(), input.size());
            QCOMPARE(result.format(), QImage::Format_RGBA64);
            QCOMPARE(result.pixelColor(0, 0).alpha(), 130);
        }
    }
};
QTEST_MAIN(CoreTests)
#include "core_tests.moc"
