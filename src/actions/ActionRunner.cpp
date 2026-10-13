#include "ActionRunner.h"
#include "document/SmartObjectOperations.h"
#include "document/TransformOperations.h"
#include "io/FormatIO.h"
#include <QColorSpace>
#include <QFile>
#include <QJsonDocument>
#include <QSet>
#include <QTransform>
#include <cmath>
#include <memory>

namespace serika {
namespace {
const QSet<QString> filters = {"gaussian blur",
                               "box blur",
                               "motion blur",
                               "radial blur",
                               "surface blur",
                               "smart blur",
                               "lens blur",
                               "field blur",
                               "iris blur",
                               "tilt-shift",
                               "spherize",
                               "pinch",
                               "twirl",
                               "ripple",
                               "wave",
                               "polar coordinates",
                               "add noise",
                               "despeckle",
                               "dust & scratches",
                               "median",
                               "reduce noise",
                               "mosaic",
                               "color halftone",
                               "pointillize",
                               "crystallize",
                               "fragment",
                               "clouds",
                               "difference clouds",
                               "lens flare",
                               "fibers",
                               "sharpen",
                               "sharpen more",
                               "sharpen edges",
                               "unsharp mask",
                               "smart sharpen",
                               "emboss",
                               "find edges",
                               "oil paint",
                               "wind",
                               "diffuse",
                               "high pass",
                               "minimum",
                               "maximum",
                               "offset",
                               "custom",
                               "camera raw filter"};
QImage::Format storage(int depth) {
    return depth == 32   ? QImage::Format_RGBA32FPx4
           : depth == 16 ? QImage::Format_RGBA64
                         : QImage::Format_RGBA8888;
}
double number(const QJsonObject &o, const QString &name, double fallback) {
    return o.value(name).toDouble(fallback);
}
bool targetsMask(const Layer &layer) { return layer.maskTarget && !layer.mask.isNull(); }
QPointF selectionOrigin(const Document *document, const Layer &layer) {
    QPointF origin = document->effectiveLayerOffset(layer);
    if (targetsMask(layer) && !layer.maskLinked)
        origin += layer.maskOffset;
    return origin;
}
qreal selectedAmount(const QImage &selection, QPointF point) {
    const int x = int(std::floor(point.x())), y = int(std::floor(point.y()));
    const qreal dx = point.x() - x, dy = point.y() - y;
    return maskSample(selection, x, y) * (1 - dx) * (1 - dy) +
           maskSample(selection, x + 1, y) * dx * (1 - dy) + maskSample(selection, x, y + 1) * (1 - dx) * dy +
           maskSample(selection, x + 1, y + 1) * dx * dy;
}
QImage maskPixels(const QImage &image, int bitDepth) {
    QImage mask = makeMask(image.size(), bitDepth);
    const QImage linear = image.convertToFormat(QImage::Format_RGBA32FPx4);
    for (int y = 0; y < mask.height(); ++y) {
        const float *row = reinterpret_cast<const float *>(linear.constScanLine(y));
        for (int x = 0; x < mask.width(); ++x) {
            const float *pixel = row + x * 4;
            setMaskSample(mask, x, y, .299 * pixel[0] + .587 * pixel[1] + .114 * pixel[2]);
        }
    }
    return mask;
}
void writeRasterPixels(Layer &layer, const QImage &image) {
    layer.pixels.setImage(image);
    layer.kind = LayerKind::Pixel;
    layer.smartFilters = {};
    layer.parameters.remove("contentTransform");
}
QImage withSelection(const QImage &oldImage, const QImage &changedImage, const Document *doc,
                     const Layer &layer) {
    if (!doc->hasSelection())
        return changedImage;
    QImage old = oldImage.convertToFormat(QImage::Format_RGBA32FPx4),
           changed = changedImage.convertToFormat(QImage::Format_RGBA32FPx4);
    const QImage selection = doc->state.selection;
    const QPointF origin = selectionOrigin(doc, layer);
    for (int y = 0; y < changed.height(); ++y) {
        auto target = reinterpret_cast<float *>(changed.scanLine(y));
        const auto original = reinterpret_cast<const float *>(old.constScanLine(y));
        for (int x = 0; x < changed.width(); ++x) {
            float amount = float(selectedAmount(selection, QPointF(x, y) + origin));
            const float oldAlpha = original[x * 4 + 3], newAlpha = target[x * 4 + 3];
            const float alpha = oldAlpha * (1 - amount) + newAlpha * amount;
            for (int c = 0; c < 3; ++c)
                target[x * 4 + c] = alpha > 1e-7f ? (original[x * 4 + c] * oldAlpha * (1 - amount) +
                                                     target[x * 4 + c] * newAlpha * amount) /
                                                        alpha
                                                  : 0;
            target[x * 4 + 3] = alpha;
        }
    }
    changed.setColorSpace(changedImage.colorSpace());
    return changed.convertToFormat(changedImage.format());
}
} // namespace

QStringList ActionRunner::filterNames() {
    QStringList names;
    for (const auto &filter : filters) {
        QString title = filter;
        for (qsizetype i = 0; i < title.size(); ++i)
            if (i == 0 || title[i - 1] == ' ')
                title[i] = title[i].toUpper();
        names.append(title);
    }
    names.sort();
    return names;
}
bool ActionRunner::execute(Document *doc, const QJsonObject &step, QString *error) {
    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (error)
        error->clear();
    if (!doc)
        return fail("No document for action.");
    QString command = step.value("command").toString(step.value("type").toString());
    QString name = step.value("name").toString();
    QJsonObject params = step.value("parameters").toObject();
    if (params.isEmpty())
        params = step.value("params").toObject();
    QString key = command.trimmed().toLower();
    if (key.startsWith("filter: ")) {
        name = command.mid(8);
        key = "filter";
    } else if (key.startsWith("adjustment: ")) {
        name = command.mid(12);
        key = "adjustment";
    }
    if (filters.contains(key)) {
        name = command;
        key = "filter";
    }
    if (key == "convert to smart object" || key == "convert for smart filters") {
        const auto *layer = doc->activeLayer();
        if (!layer)
            return fail("Select a layer to convert.");
        if (layer->kind == LayerKind::SmartObject)
            return true;
        return convertLayerToEmbeddedSmartObject(doc, layer->id, error);
    }
    if (key == "adjustment" && params.contains("destructive") && !params["destructive"].toBool()) {
        if (!adjustmentNames().contains(name))
            return fail("Unknown adjustment: " + name);
        params.remove("destructive");
        doc->addAdjustment(name, params);
        return true;
    }
    if (key == "filter" || key == "adjustment" || key == "develop" || key == "camera raw filter") {
        Layer *layer = doc->activeLayer();
        if (!layer)
            return fail("Action needs an active layer.");
        if (layer->locked)
            return fail("Active layer is locked.");
        if (key == "filter" && !filters.contains(name.toLower()))
            return fail("Unknown filter: " + name);
        if (key == "adjustment" && !adjustmentNames().contains(name))
            return fail("Unknown adjustment: " + name);
        const bool maskTarget = targetsMask(*layer);
        if (maskTarget && (key == "develop" || key == "camera raw filter"))
            return fail("Develop needs layer pixels rather than a layer mask.");
        if (key == "filter" && layer->kind == LayerKind::SmartObject && !maskTarget &&
            !params.value("destructive").toBool(false)) {
            doc->addSmartFilter(name, params);
            return true;
        }
        QImage source =
            maskTarget ? layer->mask.convertToFormat(storage(doc->state.bitDepth)) : doc->layerImage(*layer);
        if (source.isNull())
            return fail("Active layer has no pixels to process.");
        QImage result = key == "adjustment" ? applyAdjustment(source, name, params)
                        : key == "filter" ? applyFilter(source, name, params)
                                          : developImage(source, params, int(number(params, "bitDepth", 16)));
        if (result.isNull())
            return fail("Image processing failed.");
        result = withSelection(source, result, doc, *layer);
        doc->mutate(key == "filter" || key == "adjustment" ? name : "Develop", [&] {
            auto *l = doc->activeLayer();
            if (maskTarget)
                l->mask = maskPixels(result, doc->state.bitDepth);
            else
                writeRasterPixels(*l, result);
            if (key == "develop" || key == "camera raw filter") {
                doc->state.bitDepth = int(number(params, "bitDepth", 16));
                doc->state.iccProfile = result.colorSpace().iccProfile();
                doc->state.metadata["develop"] = params;
                doc->state.metadata["rawPendingDevelop"] = false;
            }
        });
        return true;
    }
    if (key == "fill" || key == "clear" || key == "delete selection") {
        auto *l = doc->activeLayer();
        if (!l)
            return fail("Fill needs an active layer.");
        if (l->locked)
            return fail("Active layer is locked.");
        if (key != "fill" && l->lockAlpha && !l->maskTarget)
            return fail("Clearing pixels is blocked by the layer's transparency lock.");
        QColor color(params.value("color").toString("#000000"));
        if (!color.isValid())
            return fail("Invalid fill colour.");
        double opacity = qBound(0.0, number(params, "opacity", 1), 1.0);
        QImage source = l->maskTarget && !l->mask.isNull() ? l->mask : doc->layerImage(*l);
        if (source.isNull()) {
            source = QImage(doc->state.size, storage(doc->state.bitDepth));
            source.fill(Qt::transparent);
        }
        if (source.isNull())
            return fail("Cannot allocate fill pixels.");
        QImage result = source.convertToFormat(QImage::Format_RGBA32FPx4);
        for (int y = 0; y < result.height(); ++y) {
            auto row = reinterpret_cast<float *>(result.scanLine(y));
            for (int x = 0; x < result.width(); ++x) {
                if (key == "fill") {
                    float a = color.alphaF() * opacity;
                    const float oldAlpha = row[x * 4 + 3];
                    const bool preserveAlpha = l->lockAlpha && !l->maskTarget;
                    const float alpha = preserveAlpha ? oldAlpha : a + oldAlpha * (1 - a);
                    for (int c = 0; c < 3; ++c)
                        row[x * 4 + c] = alpha > 1e-7f ? (row[x * 4 + c] * oldAlpha * (1 - a) +
                                                          (c == 0   ? color.redF()
                                                           : c == 1 ? color.greenF()
                                                                    : color.blueF()) *
                                                              a * (preserveAlpha ? oldAlpha : 1)) /
                                                             alpha
                                                       : 0;
                    row[x * 4 + 3] = alpha;
                } else if (l->maskTarget && !l->mask.isNull()) {
                    row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 0;
                    row[x * 4 + 3] = 1;
                } else
                    row[x * 4 + 3] = 0;
            }
        }
        result = result.convertToFormat(storage(doc->state.bitDepth));
        result = withSelection(source, result, doc, *l);
        doc->mutate(command, [&] {
            auto *layer = doc->activeLayer();
            if (layer->maskTarget && !layer->mask.isNull())
                layer->mask = maskPixels(result, doc->state.bitDepth);
            else
                writeRasterPixels(*layer, result);
        });
        return true;
    }
    if (key == "resize" || key == "image size" || key == "image size..." || key == "resize canvas" ||
        key == "canvas size...") {
        QSize size(int(number(params, "width", doc->state.size.width())),
                   int(number(params, "height", doc->state.size.height())));
        if (size.width() < 1 || size.height() < 1 || size.width() > 300000 || size.height() > 300000 ||
            quint64(size.width()) * size.height() * 16 > 1024ull * 1024 * 1024)
            return fail("Action resize dimensions exceed safe limits.");
        if (key == "resize canvas" || key == "canvas size...")
            doc->resizeCanvas(size, QPoint(int(number(params, "x", 0)), int(number(params, "y", 0))));
        else
            doc->resizeImage(size, params.value("resample").toString() == "nearest"
                                       ? Qt::FastTransformation
                                       : Qt::SmoothTransformation);
        return true;
    }
    if (key == "crop" || key == "crop to selection") {
        QRect rect(int(number(params, "x", 0)), int(number(params, "y", 0)),
                   int(number(params, "width", doc->state.size.width())),
                   int(number(params, "height", doc->state.size.height())));
        if (key == "crop to selection")
            rect = doc->selectionBounds();
        rect = rect.intersected(QRect(QPoint(), doc->state.size));
        if (rect.isEmpty())
            return fail("Action crop has empty bounds.");
        doc->crop(rect);
        return true;
    }
    if (key == "transform" || key == "flip horizontal" || key == "flip vertical" ||
        key == "rotate 90° clockwise" || key == "rotate 90° counterclockwise" || key == "rotate 180°") {
        auto *l = doc->activeLayer();
        if (!l || l->locked)
            return fail("Transform needs an unlocked active layer.");
        if (l->lockPosition)
            return fail("Transform is blocked by the layer's position lock.");
        double rotation = number(params, "rotation", number(params, "angle", 0)),
               sx = number(params, "scaleX", number(params, "scale", 100) / 100.0),
               sy = number(params, "scaleY", number(params, "scale", 100) / 100.0);
        if (key == "flip horizontal")
            sx = -1;
        if (key == "flip vertical")
            sy = -1;
        if (key == "rotate 90° clockwise")
            rotation = 90;
        if (key == "rotate 90° counterclockwise")
            rotation = -90;
        if (key == "rotate 180°")
            rotation = 180;
        if (!std::isfinite(rotation) || !std::isfinite(sx) || !std::isfinite(sy) || std::abs(sx) < .001 ||
            std::abs(sy) < .001 || std::abs(sx) > 100 || std::abs(sy) > 100)
            return fail("Invalid transform scale or angle.");
        if (params.contains("matrix"))
            return applyLayerTransform(doc, transformFromJson(params["matrix"].toArray()), error);
        QImage source = doc->layerImage(*l);
        if (source.isNull())
            return fail("Transform needs pixel content.");
        QTransform transform;
        transform.rotate(rotation);
        transform.scale(sx, sy);
        transform.shear(number(params, "shearX", 0), number(params, "shearY", 0));
        auto mapped = transform.mapRect(QRectF(source.rect()));
        if (mapped.width() > 300000 || mapped.height() > 300000 ||
            mapped.width() * mapped.height() * 16 > 1024.0 * 1024 * 1024)
            return fail("Transform exceeds safe image dimensions.");
        transform *= QTransform::fromTranslate(number(params, "x", 0), number(params, "y", 0));
        return applyLayerTransform(doc, transform, error);
    }
    if (key == "selection")
        key = name.toLower();
    if (key == "select all" || key == "all") {
        doc->selectAll();
        return true;
    }
    if (key == "deselect" || key == "none") {
        doc->deselect();
        return true;
    }
    if (key == "inverse" || key == "invert selection") {
        doc->invertSelection();
        return true;
    }
    if (key == "rectangle" || key == "ellipse") {
        QImage mask(doc->state.size, QImage::Format_Grayscale8);
        if (mask.isNull())
            return fail("Cannot allocate selection.");
        mask.fill(0);
        QPainter painter(&mask);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        QRectF rect(number(params, "x", 0), number(params, "y", 0),
                    number(params, "width", doc->state.size.width()),
                    number(params, "height", doc->state.size.height()));
        if (key == "ellipse")
            painter.drawEllipse(rect);
        else
            painter.drawRect(rect);
        painter.end();
        doc->setSelection(mask, params.value("operation").toString("replace"));
        return true;
    }
    if (key == "layer")
        key = name.toLower();
    if (key == "duplicate layer" || key == "duplicate") {
        if (!doc->activeLayer())
            return fail("No layer to duplicate.");
        doc->duplicateActiveLayer();
        return true;
    }
    if (key == "new layer" || key == "new layer..." || key == "add") {
        doc->addLayer(params.value("name").toString("Layer"));
        return true;
    }
    if (key == "new group" || key == "group") {
        doc->addLayer(params.value("name").toString("Group"), LayerKind::Group);
        return true;
    }
    if (key == "delete layer" || key == "remove") {
        if (!doc->activeLayer() || doc->activeLayer()->locked)
            return fail("No unlocked layer to remove.");
        doc->removeActiveLayer();
        return true;
    }
    if (key == "add layer mask" || key == "add mask") {
        if (!doc->activeLayer() || doc->activeLayer()->locked)
            return fail("No unlocked layer for mask.");
        doc->addMask();
        return true;
    }
    if (key == "merge down") {
        if (doc->state.activeIndex < 1 || !doc->activeLayer() || doc->activeLayer()->locked)
            return fail("No layer below to merge.");
        const auto &upper = *doc->activeLayer();
        const auto &lower = doc->state.layers[doc->state.activeIndex - 1];
        if (lower.locked || upper.parentId != lower.parentId || upper.kind == LayerKind::Group ||
            lower.kind == LayerKind::Group || upper.kind == LayerKind::Artboard ||
            lower.kind == LayerKind::Artboard)
            return fail("Merge Down requires unlocked pixel layers in the same group.");
        doc->mergeDown();
        return true;
    }
    if (key == "flatten image" || key == "flatten") {
        doc->flatten();
        return true;
    }
    if (key == "set properties" || key == "properties") {
        auto *l = doc->activeLayer();
        if (!l)
            return fail("No active layer.");
        if (params.contains("blendMode") && !blendModeNames().contains(params["blendMode"].toString()))
            return fail("Unknown blend mode.");
        doc->mutate("Layer properties", [&] {
            if (params.contains("name"))
                l->name = params["name"].toString();
            if (params.contains("opacity"))
                l->opacity = qBound(0.0, number(params, "opacity", 1), 1.0);
            if (params.contains("fill"))
                l->fill = qBound(0.0, number(params, "fill", 1), 1.0);
            if (params.contains("blendMode"))
                l->blendMode = params["blendMode"].toString();
            if (params.contains("visible"))
                l->visible = params["visible"].toBool();
            if (params.contains("locked"))
                l->locked = params["locked"].toBool();
        });
        return true;
    }
    if (key == "select layer") {
        int index = doc->state.activeIndex;
        if (params.contains("id"))
            index = doc->indexForId(params["id"].toString().toULongLong());
        else if (params.contains("index"))
            index = params["index"].toInt();
        if (index < 0 || index >= doc->state.layers.size())
            return fail("Action references a missing layer.");
        doc->setActiveIndex(index);
        return true;
    }
    if (key == "stop")
        return fail(step.value("message").toString("Action stopped by an explicit stop step."));
    return fail("Unsupported action command: " + command);
}

bool ActionRunner::run(Document *doc, const QJsonArray &steps, QString *error) {
    if (!doc) {
        if (error)
            *error = "No action document.";
        return false;
    }
    doc->beginTransaction("Run action");
    for (qsizetype i = 0; i < steps.size(); ++i) {
        QJsonObject step;
        if (steps[i].isString())
            step["command"] = steps[i].toString();
        else
            step = steps[i].toObject();
        QString failure;
        if (step.isEmpty() || !execute(doc, step, &failure)) {
            doc->cancelTransaction();
            if (error)
                *error = "Step " + QString::number(i + 1) + ": " +
                         (failure.isEmpty() ? "Invalid action step." : failure);
            return false;
        }
    }
    doc->endTransaction();
    return true;
}
bool ActionRunner::load(const QString &path, QJsonArray *steps, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (file.size() > 16 * 1024 * 1024) {
        if (error)
            *error = "Action file is too large.";
        return false;
    }
    QJsonParseError parse;
    auto json = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError) {
        if (error)
            *error = parse.errorString();
        return false;
    }
    QJsonArray array = json.isArray() ? json.array() : json.object().value("steps").toArray();
    if (array.isEmpty())
        array = json.object().value("commands").toArray();
    if (array.isEmpty()) {
        if (error)
            *error = "Action file contains no steps.";
        return false;
    }
    if (json.isObject() && json.object().value("version").toInt(1) > 1) {
        if (error)
            *error = "Unsupported action version.";
        return false;
    }
    if (steps)
        *steps = array;
    return true;
}
bool ActionRunner::runFile(const QString &actionPath, const QString &inputPath, const QString &outputPath,
                           QString *error) {
    QJsonArray steps;
    if (!load(actionPath, &steps, error))
        return false;
    std::unique_ptr<Document> doc(FormatIO::open(inputPath, error));
    if (!doc)
        return false;
    if (!run(doc.get(), steps, error))
        return false;
    return FormatIO::save(doc.get(), outputPath, error);
}
} // namespace serika
