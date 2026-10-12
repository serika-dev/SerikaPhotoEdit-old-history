#include "CanvasView.h"
#include "CropAlgorithms.h"
#include "document/ColorManagement.h"
#include "document/LayerOperations.h"
#include "document/TransformOperations.h"
#include "tools/LocalAlgorithms.h"
#include <QApplication>
#include <QDateTime>
#include <QIconEngine>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPathStroker>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QSet>
#include <QTabletEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace serika {
namespace {
bool named(const QString &tool, std::initializer_list<const char *> names) {
    for (const char *name : names)
        if (tool == QLatin1String(name))
            return true;
    return false;
}
bool selectedAncestor(const Document *document, quint64 id, const QVector<quint64> &roots) {
    QSet<quint64> visited;
    while (id && !visited.contains(id)) {
        if (roots.contains(id))
            return true;
        visited.insert(id);
        const int index = document->indexForId(id);
        if (index < 0)
            break;
        id = document->state.layers[index].parentId;
    }
    return false;
}
QString maskMoveLock(const Document *document, quint64 id) {
    QSet<quint64> visited;
    while (id && !visited.contains(id)) {
        visited.insert(id);
        const int index = document->indexForId(id);
        if (index < 0)
            return QStringLiteral("The targeted mask layer no longer exists.");
        const Layer &layer = document->state.layers[index];
        if (layer.locked || layer.lockPosition)
            return QStringLiteral("Layer '%1' is locked against movement.").arg(layer.name);
        id = layer.parentId;
    }
    return {};
}
bool paintTool(const QString &tool) {
    return named(tool, {"Brush",
                        "Pencil",
                        "Color Replacement",
                        "Mixer Brush",
                        "Clone Stamp",
                        "Pattern Stamp",
                        "History Brush",
                        "Art History Brush",
                        "Eraser",
                        "Background Eraser",
                        "Blur",
                        "Sharpen",
                        "Smudge",
                        "Dodge",
                        "Burn",
                        "Sponge",
                        "Healing Brush",
                        "Spot Healing",
                        "Quick Selection",
                        "Red Eye"});
}
bool shapeTool(const QString &tool) {
    return named(
        tool, {"Rectangle", "Ellipse", "Triangle", "Polygon", "Line", "Custom Shape", "Frame", "Artboard"});
}
bool selectionTool(const QString &tool) {
    return named(tool, {"Rectangular Marquee", "Elliptical Marquee", "Single Row", "Single Column", "Lasso",
                        "Polygonal Lasso", "Magnetic Lasso", "Object Selection"});
}
bool transportTool(const QString &tool) { return named(tool, {"Patch", "Content-Aware Move"}); }
int selectionAt(const Document *document, int x, int y) {
    const QImage &mask = document->state.selection;
    if (mask.isNull())
        return 255;
    if (x < 0 || y < 0 || x >= mask.width() || y >= mask.height())
        return 0;
    return qRound(maskSample(mask, x, y) * 255);
}
QRect documentRect(const Document *document) { return QRect(QPoint(), document->state.size); }
QImage::Format paintingFormat(const Document *document) {
    return document->state.bitDepth == 16   ? QImage::Format_RGBA64
           : document->state.bitDepth == 32 ? QImage::Format_RGBA32FPx4
                                            : QImage::Format_ARGB32;
}
QImage paintCoverage(QSize size, int depth, const std::function<void(QPainter &)> &draw) {
    if (depth > 32)
        depth = 32;
    QImage coverage(size, depth == 32 ? QImage::Format_RGBX32FPx4
                          : depth > 8 ? QImage::Format_RGBX64
                                      : QImage::Format_RGB32);
    coverage.fill(Qt::black);
    QPainter painter(&coverage);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    draw(painter);
    painter.end();
    return normalizeMask(coverage, depth);
}
QPointF effectiveOffset(const Document *document, const Layer &layer) {
    QPointF offset = layer.offset;
    quint64 parent = layer.parentId;
    QSet<quint64> visited{layer.id};
    while (parent && !visited.contains(parent)) {
        visited.insert(parent);
        const int index = document->indexForId(parent);
        if (index < 0)
            break;
        const Layer &group = document->state.layers[index];
        offset += group.offset;
        parent = group.parentId;
    }
    return offset;
}
void shiftTiles(TileImage &image, QPoint shift) {
    if (shift.isNull())
        return;
    QHash<quint64, QImage> shifted;
    for (auto it = image.tiles.cbegin(); it != image.tiles.cend(); ++it) {
        const int x = int(quint32(it.key() >> 32)), y = int(quint32(it.key()));
        shifted.insert(TileImage::key(x + shift.x() / 256, y + shift.y() / 256), it.value());
    }
    image.tiles = std::move(shifted);
    image.size += QSize(shift.x(), shift.y());
}
QRect alphaBounds(const QImage &image) {
    if (image.isNull())
        return {};
    int top = 0, bottom = image.height() - 1, left = 0, right = image.width() - 1;
    auto rowHasAlpha = [&](int y) {
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(image.pixel(x, y)))
                return true;
        return false;
    };
    while (top <= bottom && !rowHasAlpha(top))
        ++top;
    if (top > bottom)
        return {};
    while (bottom > top && !rowHasAlpha(bottom))
        --bottom;
    auto columnHasAlpha = [&](int x) {
        for (int y = top; y <= bottom; ++y)
            if (qAlpha(image.pixel(x, y)))
                return true;
        return false;
    };
    while (left <= right && !columnHasAlpha(left))
        ++left;
    while (right > left && !columnHasAlpha(right))
        --right;
    return QRect(QPoint(left, top), QPoint(right, bottom));
}
QRectF layerBounds(const Document *document, const Layer &layer) {
    QRectF bounds;
    if (layer.parameters.contains("contentTransform"))
        bounds = alphaBounds(document->layerImage(layer));
    else if (layer.kind == LayerKind::Shape || layer.kind == LayerKind::Artboard)
        bounds = layer.shape.boundingRect().adjusted(-layer.strokeWidth / 2, -layer.strokeWidth / 2,
                                                     layer.strokeWidth / 2, layer.strokeWidth / 2);
    else if (layer.kind == LayerKind::Pixel || layer.kind == LayerKind::SmartObject) {
        for (auto it = layer.pixels.tiles.cbegin(); it != layer.pixels.tiles.cend(); ++it) {
            const QPoint origin(int(quint32(it.key() >> 32)) * 256, int(quint32(it.key())) * 256);
            const QRect tile = alphaBounds(it.value());
            if (!tile.isEmpty())
                bounds = bounds.united(QRectF(tile.translated(origin)));
        }
    } else if (layer.kind != LayerKind::Group && layer.kind != LayerKind::Adjustment)
        bounds = alphaBounds(document->layerImage(layer));
    return bounds.isEmpty() ? QRectF() : bounds.translated(effectiveOffset(document, layer));
}
QColor weightedColor(QColor a, QColor b, qreal amount) {
    return QColor::fromRgbF(
        a.redF() * (1 - amount) + b.redF() * amount, a.greenF() * (1 - amount) + b.greenF() * amount,
        a.blueF() * (1 - amount) + b.blueF() * amount, a.alphaF() * (1 - amount) + b.alphaF() * amount);
}
QPainterPath polygonShape(const QRectF &rect, int sides) {
    QPainterPath path;
    const QPointF center = rect.center();
    for (int i = 0; i < sides; ++i) {
        const qreal angle = -std::numbers::pi / 2 + 2 * std::numbers::pi * i / sides;
        const QPointF p =
            center + QPointF(std::cos(angle) * rect.width() / 2, std::sin(angle) * rect.height() / 2);
        if (!i)
            path.moveTo(p);
        else
            path.lineTo(p);
    }
    path.closeSubpath();
    return path;
}
} // namespace

CanvasView::CanvasView(Document *document, QWidget *parent) : QWidget(parent), m_document(document) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(160, 120);
    connect(document, &Document::changed, this, [this] {
        if (m_transformActive && !m_committingTransform)
            cancelTransform();
        updateSelectionOutline();
        update();
    });
    connect(&m_ants, &QTimer::timeout, this, [this] {
        m_antOffset = (m_antOffset + 1) % 8;
        if (m_selectionActive)
            update();
    });
    m_ants.start(150);
    updateSelectionOutline();
}
void CanvasView::setTool(const QString &tool) {
    cancelTransform();
    if (m_dragging) {
        m_document->cancelTransaction();
        m_dragging = false;
    }
    m_brushDynamics.cancel();
    m_tabletTilt = {};
    m_moving = false;
    m_moveLayerIds.clear();
    m_tool = tool;
    cancelCrop();
    m_transporting = false;
    m_transportSelecting = false;
    m_transportPreview = {};
    m_lasso.clear();
    m_penLayerId = 0;
    m_penPath = {};
    m_perspectivePoints.clear();
    m_pathElement = -1;
    if (tool == "Hand")
        setCursor(Qt::OpenHandCursor);
    else if (tool == "Type" || tool == "Horizontal Type" || tool == "Vertical Type")
        setCursor(Qt::IBeamCursor);
    else if (tool == "Move" || tool == "Path Selection" || tool == "Direct Selection")
        setCursor(Qt::ArrowCursor);
    else
        setCursor(Qt::CrossCursor);
    emit toolChanged(tool);
    update();
}
void CanvasView::setForeground(QColor color) {
    m_foreground = color;
    update();
}
QColor CanvasView::foreground() const { return m_foreground; }
void CanvasView::setBackground(QColor color) { m_background = color; }
void CanvasView::setBrushSize(int size) {
    m_brushSize = std::clamp(size, 1, 5000);
    update();
    emit brushSettingsChanged();
}
void CanvasView::setBrushHardness(qreal hardness) {
    m_hardness = std::clamp(hardness, qreal(0), qreal(1));
    emit brushSettingsChanged();
}
void CanvasView::setBrushOpacity(qreal opacity) {
    m_opacity = std::clamp(opacity, qreal(0), qreal(1));
    emit brushSettingsChanged();
}
void CanvasView::setBrushFlow(qreal flow) {
    m_flow = std::clamp(flow, qreal(0), qreal(1));
    emit brushSettingsChanged();
}
void CanvasView::setBrushSpacing(qreal spacing) {
    m_spacing = std::clamp(spacing, qreal(0.01), qreal(4));
    emit brushSettingsChanged();
}
void CanvasView::setBrushAngle(qreal degrees) {
    m_brushAngle = degrees;
    update();
    emit brushSettingsChanged();
}
void CanvasView::setBrushRoundness(qreal roundness) {
    m_roundness = std::clamp(roundness, qreal(0.05), qreal(1));
    update();
    emit brushSettingsChanged();
}
void CanvasView::setBrushSmoothing(qreal smoothing) {
    m_smoothing = std::clamp(smoothing, qreal(0), qreal(1));
    emit brushSettingsChanged();
}
BrushPreset CanvasView::brushPreset() const {
    BrushPreset preset = m_brushPreset;
    preset.size = m_brushSize;
    preset.hardness = m_hardness;
    preset.opacity = m_opacity;
    preset.flow = m_flow;
    preset.spacing = m_spacing;
    preset.angle = m_brushAngle;
    preset.roundness = m_roundness;
    preset.smoothing = m_smoothing;
    return preset.normalized();
}
void CanvasView::setBrushPreset(const BrushPreset &preset) {
    m_brushPreset = preset.normalized();
    m_brushSize = m_brushPreset.size;
    m_hardness = m_brushPreset.hardness;
    m_opacity = m_brushPreset.opacity;
    m_flow = m_brushPreset.flow;
    m_spacing = m_brushPreset.spacing;
    m_brushAngle = m_brushPreset.angle;
    m_roundness = m_brushPreset.roundness;
    m_smoothing = m_brushPreset.smoothing;
    update();
    emit brushSettingsChanged();
}
QVector<quint64> CanvasView::selectedMoveRoots() const {
    QVector<quint64> ids;
    for (const QVariant &value : property("selectedLayerIds").toList()) {
        const quint64 id = value.toULongLong();
        if (id && m_document->indexForId(id) >= 0)
            ids.append(id);
    }
    if (ids.isEmpty() && m_document->activeLayer())
        ids.append(m_document->activeLayer()->id);
    return selectedLayerRoots(m_document, ids);
}
void CanvasView::nudgeSelectedLayers(QPointF delta) {
    if (!std::isfinite(delta.x()) || !std::isfinite(delta.y()) || delta.isNull())
        return;
    if (m_dragging)
        cancelInteraction();
    const auto roots = selectedMoveRoots();
    Layer *layer = m_document->activeLayer();
    if (roots.size() == 1 && layer && roots.first() == layer->id && layer->maskTarget &&
        !layer->mask.isNull() && !layer->maskLinked) {
        const QString lock = maskMoveLock(m_document, layer->id);
        if (!lock.isEmpty()) {
            emit interactionError(lock);
            return;
        }
        const int index = m_document->indexForId(layer->id);
        // Resolve mutable storage after the transaction snapshot, so Qt detaches its shared vector.
        m_document->mutate("Nudge layer mask",
                           [this, index, delta] { m_document->state.layers[index].maskOffset += delta; });
        return;
    }
    const auto result = translateLayers(m_document, roots, delta);
    if (!result.error.isEmpty())
        emit interactionError(result.error);
}
bool CanvasView::updateMove(QPointF point, Qt::KeyboardModifiers modifiers) {
    QPointF delta = point - m_start;
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        if (std::abs(delta.x()) >= std::abs(delta.y()))
            delta.setY(0);
        else
            delta.setX(0);
    }
    const QPointF incremental = delta - m_moveAppliedDelta;
    QString error;
    if (m_moveMaskOnly && !m_moveLayerIds.isEmpty()) {
        const int index = m_document->indexForId(m_moveLayerIds.first());
        error = maskMoveLock(m_document, m_moveLayerIds.first());
        if (index >= 0 && error.isEmpty()) {
            Layer &layer = m_document->state.layers[index];
            if (layer.mask.isNull() || layer.maskLinked)
                error = QStringLiteral("The targeted mask changed during the gesture.");
            else if (!incremental.isNull())
                m_document->mutate("Move layer mask", [this, index, incremental] {
                    m_document->state.layers[index].maskOffset += incremental;
                });
        }
    } else
        error = translateLayers(m_document, m_moveLayerIds, incremental).error;
    if (!error.isEmpty()) {
        m_document->cancelTransaction();
        m_dragging = false;
        m_moving = false;
        m_moveLayerIds.clear();
        emit interactionError(error);
        return false;
    }
    m_moveAppliedDelta = delta;
    return true;
}
void CanvasView::setShowRulers(bool enabled) {
    m_rulers = enabled;
    update();
}
void CanvasView::setShowGrid(bool enabled) {
    m_grid = enabled;
    update();
}
void CanvasView::setShowGuides(bool enabled) {
    m_guides = enabled;
    update();
}
void CanvasView::setQuickMask(bool enabled) {
    m_quickMask = enabled;
    update();
}
void CanvasView::setSurround(QColor color) {
    m_surround = color;
    update();
}
void CanvasView::setCropSettings(const CropSettings &settings) {
    m_cropSettings = settings;
    m_cropSettings.angle = std::clamp(settings.angle, qreal(-180), qreal(180));
    if (m_cropSettings.ratio.width() <= 0 || m_cropSettings.ratio.height() <= 0)
        m_cropSettings.ratio = {};
    if (m_cropActive && !m_cropSettings.ratio.isEmpty()) {
        const qreal ratio = m_cropSettings.ratio.width() / m_cropSettings.ratio.height();
        const QPointF center = m_cropRect.center();
        qreal width = m_cropRect.width(), height = width / ratio;
        if (height > m_document->state.size.height()) {
            height = m_document->state.size.height();
            width = height * ratio;
        }
        m_cropRect = QRectF(center - QPointF(width / 2, height / 2), QSizeF(width, height));
    }
    emit cropSettingsChanged();
    update();
}
void CanvasView::setCropPreviewRect(QRectF rectangle) {
    m_cropRect = rectangle.normalized();
    m_cropActive = m_cropRect.width() >= 1 && m_cropRect.height() >= 1;
    emit cropPreviewChanged(hasCropPreview());
    update();
}
void CanvasView::resetCrop() {
    m_cropSettings.angle = 0;
    setCropPreviewRect(QRectF(QPointF(), m_document->state.size));
    setCropSettings(m_cropSettings);
}
void CanvasView::swapCropRatio() {
    m_cropSettings.ratio.transpose();
    m_cropSettings.outputSize.transpose();
    setCropSettings(m_cropSettings);
}
void CanvasView::cycleCropOverlay() {
    const QStringList overlays{"Thirds", "Grid", "Diagonal", "Triangle", "Golden Ratio", "None"};
    m_cropSettings.overlay = overlays[(overlays.indexOf(m_cropSettings.overlay) + 1) % overlays.size()];
    emit cropSettingsChanged();
    update();
}
void CanvasView::beginStraighten() {
    if (m_tool != "Crop")
        setTool("Crop");
    if (!m_cropActive)
        resetCrop();
    m_straightenArmed = true;
    setCursor(Qt::CrossCursor);
    update();
}
void CanvasView::cancelCrop() {
    if (m_cropHandle >= 0 || m_perspectiveHandle >= 0 || m_straightening || m_cropRotating)
        m_dragging = false;
    m_cropActive = false;
    m_cropRect = {};
    m_cropHandle = -1;
    m_perspectiveHandle = -1;
    m_perspectivePoints.clear();
    m_straightenArmed = false;
    m_straightening = false;
    m_cropRotating = false;
    m_cropSettings.angle = 0;
    emit cropPreviewChanged(false);
    emit cropSettingsChanged();
    update();
}
bool CanvasView::autoCropTransparent(bool preview) {
    const QRect bounds = transparentContentBounds(m_document->composite());
    if (bounds.isEmpty())
        return false;
    setTool("Crop");
    setCropPreviewRect(bounds);
    return preview || commitCrop();
}
bool CanvasView::autoCropContent(bool preview) {
    const QRect bounds = uniformBorderContentBounds(m_document->composite());
    if (bounds.isEmpty())
        return false;
    setTool("Crop");
    setCropPreviewRect(bounds);
    return preview || commitCrop();
}
bool CanvasView::autoStraighten(bool preview) {
    const qreal angle = estimateStraightenAngle(m_document->composite());
    if (std::abs(angle) < .05)
        return false;
    setTool("Crop");
    m_cropSettings.angle = angle;
    setCropPreviewRect(largestInscribedRotatedRectangle(m_document->state.size, angle));
    emit cropSettingsChanged();
    return preview || commitCrop();
}
void CanvasView::setMaskPreview(MaskPreview preview) {
    m_layerMaskPreview = preview;
    update();
}
bool CanvasView::hasPendingInteraction() const {
    return m_dragging || hasCropPreview() || !m_perspectivePoints.isEmpty() || m_transformActive;
}
void CanvasView::cancelInteraction() {
    m_brushDynamics.cancel();
    m_tabletTilt = {};
    cancelTransform();
    if (m_dragging && !m_transporting && !m_transportSelecting && m_cropHandle == -1 && !m_straightening &&
        !m_cropRotating)
        m_document->cancelTransaction();
    m_dragging = false;
    m_moving = false;
    m_moveLayerIds.clear();
    m_transporting = false;
    m_transportSelecting = false;
    m_transportPreview = {};
    m_transportMask = {};
    m_lasso.clear();
    m_penLayerId = 0;
    m_penPath = {};
    m_dragRect = {};
    m_pathElement = -1;
    cancelCrop();
}
void CanvasView::setBrushOpacityByNumber(int number, bool flow) {
    if (number < 0 || number > 9)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool secondDigit =
        m_numericOpacity >= 0 && now - m_numericOpacityTime < 650 && flow == m_numericOpacityFlow;
    const int value = secondDigit ? m_numericOpacity * 10 + number : (number ? number * 10 : 100);
    m_numericOpacity = secondDigit ? -1 : number;
    m_numericOpacityTime = now;
    m_numericOpacityFlow = flow;
    if (flow)
        setBrushFlow(value / 100.0);
    else
        setBrushOpacity(value / 100.0);
}
QImage CanvasView::activeMaskPreview() const {
    const Layer *layer = m_document->activeLayer();
    if (!layer) {
        m_layerMaskPreviewCache = {};
        m_layerMaskOverlayCache = {};
        return {};
    }
    Layer positioned = *layer;
    positioned.offset = effectiveOffset(m_document, *layer);
    positioned.maskEnabled = true;
    positioned.vectorMaskEnabled = true;
    const Layer &cached = m_cachedMaskLayer;
    if (!m_layerMaskPreviewCache.isNull() && m_cachedMaskCanvas == m_document->state.size &&
        cached.id == positioned.id && cached.offset == positioned.offset &&
        cached.mask.cacheKey() == positioned.mask.cacheKey() &&
        cached.maskDensity == positioned.maskDensity && cached.maskFeather == positioned.maskFeather &&
        cached.maskLinked == positioned.maskLinked && cached.maskOffset == positioned.maskOffset &&
        cached.vectorMask == positioned.vectorMask &&
        cached.vectorMaskDensity == positioned.vectorMaskDensity &&
        cached.vectorMaskFeather == positioned.vectorMaskFeather &&
        cached.vectorMaskLinked == positioned.vectorMaskLinked &&
        cached.vectorMaskOffset == positioned.vectorMaskOffset)
        return m_layerMaskPreviewCache;
    const QImage mask = renderedLayerMask(positioned, m_document->state.size, 8);
    m_cachedMaskLayer = positioned;
    m_cachedMaskLayer.pixels = {};
    m_cachedMaskLayer.parameters = {};
    m_cachedMaskLayer.effects = {};
    m_cachedMaskLayer.smartFilters = {};
    m_cachedMaskCanvas = m_document->state.size;
    m_layerMaskOverlayCache = {};
    m_layerMaskPreviewCache = mask.isNull() ? makeMask(m_document->state.size, 8, 1) : mask;
    return m_layerMaskPreviewCache;
}
int CanvasView::cropHandleAt(QPointF point) const {
    if (!m_cropActive)
        return -1;
    const QList<QPointF> handles{m_cropRect.topLeft(),
                                 m_cropRect.topRight(),
                                 m_cropRect.bottomRight(),
                                 m_cropRect.bottomLeft(),
                                 {m_cropRect.center().x(), m_cropRect.top()},
                                 {m_cropRect.right(), m_cropRect.center().y()},
                                 {m_cropRect.center().x(), m_cropRect.bottom()},
                                 {m_cropRect.left(), m_cropRect.center().y()}};
    for (int i = 0; i < handles.size(); ++i)
        if (QLineF(point, handles[i]).length() < 9 / m_zoom)
            return i;
    return m_cropRect.contains(point) ? 8 : 9;
}
void CanvasView::updateCropDrag(QPointF point, Qt::KeyboardModifiers modifiers) {
    QRectF rectangle = m_initialCropRect;
    const QPointF delta = point - m_start;
    if (m_cropHandle == 8) {
        rectangle.translate(delta);
    } else if (m_cropHandle == 10) {
        rectangle = QRectF(m_start, point).normalized();
    } else {
        if (m_cropHandle == 0 || m_cropHandle == 3 || m_cropHandle == 7)
            rectangle.setLeft(point.x());
        if (m_cropHandle == 1 || m_cropHandle == 2 || m_cropHandle == 5)
            rectangle.setRight(point.x());
        if (m_cropHandle == 0 || m_cropHandle == 1 || m_cropHandle == 4)
            rectangle.setTop(point.y());
        if (m_cropHandle == 2 || m_cropHandle == 3 || m_cropHandle == 6)
            rectangle.setBottom(point.y());
        if (modifiers.testFlag(Qt::AltModifier)) {
            const QPointF center = m_initialCropRect.center();
            rectangle =
                QRectF(center - QPointF(std::abs(point.x() - center.x()), std::abs(point.y() - center.y())),
                       center + QPointF(std::abs(point.x() - center.x()), std::abs(point.y() - center.y())));
        }
        rectangle = rectangle.normalized();
    }
    const QSizeF ratio = m_cropSettings.ratio;
    if (m_cropHandle != 8 && (!ratio.isEmpty() || modifiers.testFlag(Qt::ShiftModifier))) {
        const qreal aspect = ratio.isEmpty() ? 1 : ratio.width() / ratio.height();
        qreal width = rectangle.width(), height = rectangle.height();
        if (m_cropHandle == 4 || m_cropHandle == 6)
            width = height * aspect;
        else
            height = width / aspect;
        const bool fromRight = m_cropHandle == 0 || m_cropHandle == 3 || m_cropHandle == 7;
        const bool fromBottom = m_cropHandle == 0 || m_cropHandle == 1 || m_cropHandle == 4;
        QPointF anchor(fromRight ? rectangle.right() - width : rectangle.left(),
                       fromBottom ? rectangle.bottom() - height : rectangle.top());
        if (m_cropHandle == 10)
            anchor = {point.x() < m_start.x() ? m_start.x() - width : m_start.x(),
                      point.y() < m_start.y() ? m_start.y() - height : m_start.y()};
        rectangle = QRectF(anchor, QSizeF(width, height));
    }
    if (rectangle.width() > .5 && rectangle.height() > .5)
        m_cropRect = rectangle;
    m_cropActive = true;
    emit cropPreviewChanged(true);
    update();
}
void CanvasView::drawCropPreview(QPainter &painter) {
    if (!m_cropActive && m_perspectivePoints.size() != 4)
        return;
    QPainterPath inside;
    if (m_tool == "Perspective Crop" && m_perspectivePoints.size() == 4)
        inside.addPolygon(m_perspectivePoints);
    else
        inside.addRect(m_cropRect);
    QPainterPath outside;
    outside.addRect(QRectF(documentRect(m_document)).adjusted(-5000, -5000, 5000, 5000));
    outside = outside.subtracted(inside);
    painter.fillPath(outside, QColor(0, 0, 0, 150));
    QPen outline(Qt::white, 1);
    outline.setCosmetic(true);
    painter.setPen(outline);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(inside);
    if (m_tool == "Perspective Crop") {
        const QPolygonF &p = m_perspectivePoints;
        for (int i = 1; i < 3; ++i) {
            const qreal t = i / 3.0;
            painter.drawLine(p[0] + (p[1] - p[0]) * t, p[3] + (p[2] - p[3]) * t);
            painter.drawLine(p[0] + (p[3] - p[0]) * t, p[1] + (p[2] - p[1]) * t);
        }
        painter.setBrush(Qt::white);
        for (const QPointF point : p)
            painter.drawRect(QRectF(point - QPointF(3 / m_zoom, 3 / m_zoom), QSizeF(6 / m_zoom, 6 / m_zoom)));
        return;
    }
    painter.save();
    painter.setClipRect(m_cropRect, Qt::IntersectClip);
    QPen guide(QColor(255, 255, 255, 130), 1);
    guide.setCosmetic(true);
    painter.setPen(guide);
    const QString overlay = m_cropSettings.overlay;
    if (overlay == "Thirds" || overlay == "Golden Ratio" || overlay == "Grid") {
        QList<qreal> positions =
            overlay == "Golden Ratio" ? QList<qreal>{.381966, .618034} : QList<qreal>{1.0 / 3, 2.0 / 3};
        if (overlay == "Grid") {
            positions.clear();
            for (int i = 1; i < 10; ++i)
                positions.append(i / 10.0);
        }
        for (const qreal t : positions) {
            const qreal x = m_cropRect.left() + m_cropRect.width() * t;
            const qreal y = m_cropRect.top() + m_cropRect.height() * t;
            painter.drawLine(QPointF(x, m_cropRect.top()), QPointF(x, m_cropRect.bottom()));
            painter.drawLine(QPointF(m_cropRect.left(), y), QPointF(m_cropRect.right(), y));
        }
    } else if (overlay == "Diagonal") {
        painter.drawLine(m_cropRect.topLeft(), m_cropRect.bottomRight());
        painter.drawLine(m_cropRect.topRight(), m_cropRect.bottomLeft());
    } else if (overlay == "Triangle") {
        painter.drawLine(m_cropRect.topLeft(), m_cropRect.bottomRight());
        const QPointF axis = m_cropRect.bottomRight() - m_cropRect.topLeft();
        const qreal denominator = QPointF::dotProduct(axis, axis);
        if (denominator > 0)
            for (const QPointF point : {m_cropRect.topRight(), m_cropRect.bottomLeft()}) {
                const qreal t = QPointF::dotProduct(point - m_cropRect.topLeft(), axis) / denominator;
                painter.drawLine(point, m_cropRect.topLeft() + axis * t);
            }
    }
    painter.restore();
    painter.setPen(outline);
    painter.setBrush(Qt::white);
    const QList<QPointF> handles{m_cropRect.topLeft(),
                                 m_cropRect.topRight(),
                                 m_cropRect.bottomRight(),
                                 m_cropRect.bottomLeft(),
                                 {m_cropRect.center().x(), m_cropRect.top()},
                                 {m_cropRect.right(), m_cropRect.center().y()},
                                 {m_cropRect.center().x(), m_cropRect.bottom()},
                                 {m_cropRect.left(), m_cropRect.center().y()}};
    for (const QPointF handle : handles)
        painter.drawRect(QRectF(handle - QPointF(3 / m_zoom, 3 / m_zoom), QSizeF(6 / m_zoom, 6 / m_zoom)));
}
void CanvasView::rotateDocumentContent(qreal angle) {
    if (std::abs(angle) < .001)
        return;
    const QPointF center(m_document->state.size.width() / 2.0, m_document->state.size.height() / 2.0);
    QTransform transform;
    transform.translate(center.x(), center.y());
    transform.rotate(angle);
    transform.translate(-center.x(), -center.y());
    QVector<QPointF> offsets;
    for (const Layer &layer : m_document->state.layers)
        offsets.append(effectiveOffset(m_document, layer));
    for (int i = 0; i < m_document->state.layers.size(); ++i) {
        Layer &layer = m_document->state.layers[i];
        const QPointF originalOffset = offsets[i];
        QPointF newOffset;
        if (layer.kind == LayerKind::Pixel) {
            const QImage source = m_document->layerImage(layer);
            const QRectF bounds = transform.mapRect(QRectF(originalOffset, source.size()));
            const QRect destination = bounds.toAlignedRect();
            QImage rotated(destination.size(), layer.pixels.format);
            rotated.fill(Qt::transparent);
            QPainter painter(&rotated);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.translate(-destination.topLeft());
            painter.setTransform(transform, true);
            painter.drawImage(originalOffset, source);
            painter.end();
            layer.pixels.setImage(rotated);
            layer.parameters.remove("contentTransform");
            newOffset = destination.topLeft();
        } else if (layer.kind == LayerKind::SmartObject || layer.kind == LayerKind::Text ||
                   layer.kind == LayerKind::Shape) {
            const QImage source = m_document->layerImage(layer);
            const QTransform local = QTransform::fromTranslate(originalOffset.x(), originalOffset.y()) *
                                     transform *
                                     QTransform::fromTranslate(-originalOffset.x(), -originalOffset.y());
            const QPointF displacement = local.mapRect(QRectF(source.rect())).toAlignedRect().topLeft();
            Layer original = layer;
            original.parameters.remove("contentTransform");
            const QImage raw = m_document->layerImage(original);
            const QTransform old =
                QImage::trueMatrix(transformFromJson(layer.parameters.value("contentTransform").toArray()),
                                   raw.width(), raw.height());
            layer.parameters["contentTransform"] = transformJson(old * local);
            newOffset = originalOffset + displacement;
        } else if (layer.kind == LayerKind::Artboard) {
            QTransform local;
            local.translate(originalOffset.x(), originalOffset.y());
            layer.shape = transform.map(local.map(layer.shape));
        } else if (layer.kind == LayerKind::GradientFill) {
            layer.parameters["angle"] = layer.parameters.value("angle").toDouble() + angle;
        }
        if (!layer.mask.isNull()) {
            const QPointF maskOffset = originalOffset + (layer.maskLinked ? QPointF() : layer.maskOffset);
            const QRect bounds = transform.mapRect(QRectF(maskOffset, layer.mask.size())).toAlignedRect();
            const QRect output = bounds.united(QRect(newOffset.toPoint(), layer.pixels.size));
            const QImage mask = paintCoverage(output.size(), layer.mask.depth(), [&](QPainter &painter) {
                painter.translate(-output.topLeft());
                painter.setTransform(transform, true);
                painter.drawImage(maskOffset, layer.mask);
            });
            layer.mask = mask;
            layer.maskOffset = QPointF(output.topLeft()) - newOffset;
            layer.maskLinked = false;
        }
        if (!layer.vectorMask.isEmpty()) {
            QTransform local;
            const QPointF vectorOffset =
                originalOffset + (layer.vectorMaskLinked ? QPointF() : layer.vectorMaskOffset);
            local.translate(vectorOffset.x(), vectorOffset.y());
            QTransform destination;
            destination.translate(-newOffset.x(), -newOffset.y());
            layer.vectorMask = destination.map(transform.map(local.map(layer.vectorMask)));
            layer.vectorMaskOffset = {};
        }
        layer.offset = newOffset;
    }
    if (!m_document->state.selection.isNull()) {
        const QImage selection =
            paintCoverage(m_document->state.size, m_document->state.bitDepth, [&](QPainter &painter) {
                painter.setTransform(transform);
                painter.drawImage(QPoint(), m_document->state.selection);
            });
        m_document->state.selection = selection;
    }
    m_document->state.guides.clear();
    m_document->touch();
}
bool CanvasView::commitCrop() {
    if (m_tool == "Perspective Crop" && m_perspectivePoints.size() == 4) {
        perspectiveCrop();
        cancelCrop();
        return true;
    }
    if (!m_cropActive || m_cropRect.width() < 1 || m_cropRect.height() < 1)
        return false;
    const QRect crop(qRound(m_cropRect.left()), qRound(m_cropRect.top()),
                     std::max(1, qRound(m_cropRect.width())), std::max(1, qRound(m_cropRect.height())));
    const CropSettings settings = m_cropSettings;
    m_document->mutate("Crop", [&] {
        rotateDocumentContent(settings.angle);
        m_document->crop(crop, settings.deleteCroppedPixels);
        if (!settings.outputSize.isEmpty() && settings.outputSize != m_document->state.size)
            m_document->resizeImage(settings.outputSize);
        if (settings.resolution > 0)
            m_document->state.resolution = settings.resolution;
    });
    cancelCrop();
    fitToView();
    return true;
}
QTransform CanvasView::viewTransform() const {
    QTransform transform;
    const qreal ruler = m_rulers ? 20 : 0;
    transform.translate((width() + ruler) / 2.0 + m_pan.x(), (height() + ruler) / 2.0 + m_pan.y());
    transform.rotate(m_rotation);
    transform.scale(m_flip ? -m_zoom : m_zoom, m_zoom);
    transform.translate(-m_document->state.size.width() / 2.0, -m_document->state.size.height() / 2.0);
    return transform;
}
QPointF CanvasView::toDocument(QPointF point) const { return viewTransform().inverted().map(point); }
QPointF CanvasView::fromDocument(QPointF point) const { return viewTransform().map(point); }
void CanvasView::setZoom(qreal zoom) {
    m_zoom = std::clamp(zoom, qreal(0.005), qreal(64));
    m_fitPending = false;
    emit zoomChanged(m_zoom);
    update();
}
qreal CanvasView::zoom() const { return m_zoom; }
void CanvasView::fitToView() {
    const qreal ruler = m_rulers ? 20 : 0;
    const QSize size = m_document->state.size;
    if (size.isEmpty())
        return;
    m_pan = {};
    m_rotation = 0;
    m_zoom =
        std::clamp(std::min((width() - ruler - 64) / size.width(), (height() - ruler - 64) / size.height()),
                   qreal(0.005), qreal(64));
    m_fitPending = false;
    emit zoomChanged(m_zoom);
    update();
}
void CanvasView::fitSelection() {
    const QRect bounds = m_document->selectionBounds();
    if (bounds.isEmpty()) {
        fitToView();
        return;
    }
    m_pan = {};
    m_rotation = 0;
    const qreal ruler = m_rulers ? 20 : 0;
    setZoom(std::min((width() - ruler - 64) / bounds.width(), (height() - ruler - 64) / bounds.height()));
    m_pan =
        QPointF((width() + ruler) / 2.0, (height() + ruler) / 2.0) - fromDocument(QRectF(bounds).center());
    update();
}
void CanvasView::resetView() {
    m_pan = {};
    m_rotation = 0;
    m_flip = false;
    setZoom(1);
}
void CanvasView::rotateView(qreal degrees) {
    m_rotation = std::fmod(degrees, 360.0);
    update();
}
void CanvasView::flipView() {
    m_flip = !m_flip;
    update();
}
void CanvasView::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (m_fitPending)
        fitToView();
}

void CanvasView::updateSelectionOutline() {
    const QImage &mask = m_document->state.selection;
    const qint64 key = mask.isNull() ? 0 : mask.cacheKey();
    if (key == m_selectionKey)
        return;
    m_selectionKey = key;
    m_selectionOutline = {};
    m_maskOverlay = {};
    m_selectionActive = false;
    if (mask.isNull())
        return;
    const QRect bounds = m_document->selectionBounds();
    if (bounds.isEmpty())
        return;
    m_selectionActive = true;
    auto selected = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < mask.width() && y < mask.height() &&
               selectionAt(m_document, x, y) > 127;
    };
    // Coalesce adjacent edge pixels into line segments, keeping large rectangular masks small.
    for (int y = bounds.top(); y <= bounds.bottom() + 1; ++y) {
        int run = -1;
        for (int x = bounds.left(); x <= bounds.right() + 1; ++x) {
            const bool edge = x <= bounds.right() && (selected(x, y) != selected(x, y - 1));
            if (edge && run < 0)
                run = x;
            if (!edge && run >= 0) {
                m_selectionOutline.moveTo(run, y);
                m_selectionOutline.lineTo(x, y);
                run = -1;
            }
        }
    }
    for (int x = bounds.left(); x <= bounds.right() + 1; ++x) {
        int run = -1;
        for (int y = bounds.top(); y <= bounds.bottom() + 1; ++y) {
            const bool edge = y <= bounds.bottom() && (selected(x, y) != selected(x - 1, y));
            if (edge && run < 0)
                run = y;
            if (!edge && run >= 0) {
                m_selectionOutline.moveTo(x, run);
                m_selectionOutline.lineTo(x, y);
                run = -1;
            }
        }
    }
    m_maskOverlay = QImage(mask.size(), QImage::Format_ARGB32);
    for (int y = 0; y < mask.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(m_maskOverlay.scanLine(y));
        for (int x = 0; x < mask.width(); ++x)
            row[x] = qRgba(220, 44, 55, (255 - selectionAt(m_document, x, y)) / 2);
    }
}

QImage CanvasView::proofPreview(const QImage &source) {
    const auto settings = m_document->state.metadata.value("proofing").toObject();
    if (!settings.value("enabled").toBool() && !settings.value("gamutWarning").toBool()) {
        m_proofPreview = {};
        setProperty("proofError", QString());
        return source;
    }
    if (m_proofPreview.isNull() || source.cacheKey() != m_proofSourceKey || settings != m_proofSettings ||
        m_proofSourceIcc != m_document->state.iccProfile) {
        QString error;
        m_proofPreview = displayProofImage(m_document, source, &error);
        m_proofSourceKey = source.cacheKey();
        m_proofSettings = settings;
        m_proofSourceIcc = m_document->state.iccProfile;
        setProperty("proofError", error);
    }
    return m_proofPreview;
}

void CanvasView::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), m_surround);
    const QRect viewport = m_rulers ? rect().adjusted(20, 20, 0, 0) : rect();
    painter.setClipRect(viewport);
    painter.setTransform(viewTransform());
    const QRect imageRect = documentRect(m_document);
    const bool extras = !property("showExtras").isValid() || property("showExtras").toBool();
    for (int i = 8; i > 0; --i) {
        QPen shadow(QColor(0, 0, 0, 8), i * 2 / m_zoom);
        painter.setPen(shadow);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(imageRect).translated(2 / m_zoom, 3 / m_zoom));
    }
    QPixmap checker(16, 16);
    checker.fill(QColor("#9a9a9a"));
    {
        QPainter tiles(&checker);
        tiles.fillRect(0, 0, 8, 8, QColor("#c8c8c8"));
        tiles.fillRect(8, 8, 8, 8, QColor("#c8c8c8"));
    }
    painter.fillRect(imageRect, QBrush(checker));
    painter.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 1);
    painter.save();
    if (m_cropActive && std::abs(m_cropSettings.angle) > .001) {
        const QPointF center = QRectF(imageRect).center();
        painter.translate(center);
        painter.rotate(m_cropSettings.angle);
        painter.translate(-center);
    }
    if (m_layerMaskPreview == MaskPreview::Grayscale)
        painter.drawImage(QPoint(), activeMaskPreview());
    else
        painter.drawImage(QPoint(0, 0),
                          proofPreview(m_transformActive ? m_transformPreview : m_document->composite()));
    if (m_layerMaskPreview == MaskPreview::Overlay) {
        const QImage mask = activeMaskPreview();
        if (m_layerMaskOverlayCache.isNull()) {
            m_layerMaskOverlayCache = QImage(mask.size(), QImage::Format_ARGB32);
            for (int y = 0; y < mask.height(); ++y) {
                auto *row = reinterpret_cast<QRgb *>(m_layerMaskOverlayCache.scanLine(y));
                for (int x = 0; x < mask.width(); ++x)
                    row[x] = qRgba(230, 45, 55, qRound((1 - maskSample(mask, x, y)) * 128));
            }
        }
        painter.drawImage(QPoint(), m_layerMaskOverlayCache);
    }
    painter.restore();
    if (m_transporting && !m_transportPreview.isNull()) {
        painter.save();
        painter.setOpacity(.85);
        painter.drawImage(m_transportDelta, m_transportPreview);
        painter.restore();
    }
    if (m_quickMask && !m_maskOverlay.isNull())
        painter.drawImage(QPoint(), m_maskOverlay);
    painter.save();
    painter.setClipRect(imageRect);
    if (m_grid && extras && m_zoom >= 0.1) {
        const int spacing = std::max(1, m_document->state.metadata.value("gridSpacing").toInt(64));
        QPen grid(QColor(150, 150, 150, 95), 1 / m_zoom, Qt::DotLine);
        painter.setPen(grid);
        for (int x = 0; x < imageRect.width(); x += spacing)
            painter.drawLine(x, 0, x, imageRect.height());
        for (int y = 0; y < imageRect.height(); y += spacing)
            painter.drawLine(0, y, imageRect.width(), y);
    }
    if (m_guides && extras) {
        painter.setPen(QPen(QColor("#58d1d8"), 1 / m_zoom));
        for (const Guide &guide : m_document->state.guides) {
            if (guide.vertical)
                painter.drawLine(QPointF(guide.position, 0), QPointF(guide.position, imageRect.height()));
            else
                painter.drawLine(QPointF(0, guide.position), QPointF(imageRect.width(), guide.position));
        }
    }
    if (m_guideDrag) {
        painter.setPen(QPen(QColor("#58d1d8"), 1 / m_zoom));
        if (m_guideDrag == 1)
            painter.drawLine(QPointF(m_guidePosition, 0), QPointF(m_guidePosition, imageRect.height()));
        else
            painter.drawLine(QPointF(0, m_guidePosition), QPointF(imageRect.width(), m_guidePosition));
    }
    painter.restore();
    if (!m_quickMask && extras && !m_selectionOutline.isEmpty()) {
        painter.setRenderHint(QPainter::Antialiasing, false);
        QPen white(Qt::white, 1);
        white.setCosmetic(true);
        painter.setPen(white);
        painter.drawPath(m_selectionOutline);
        QPen ants(Qt::black, 1);
        ants.setCosmetic(true);
        ants.setDashPattern({4, 4});
        ants.setDashOffset(m_antOffset);
        painter.setPen(ants);
        painter.drawPath(m_selectionOutline);
    }
    if (m_dragging && (selectionTool(m_tool) || shapeTool(m_tool) || m_transportSelecting ||
                       named(m_tool, {"Slice", "Slice Select", "Gradient"}))) {
        QPen preview(Qt::white, 1);
        preview.setCosmetic(true);
        preview.setStyle(Qt::DashLine);
        painter.setPen(preview);
        painter.setBrush(Qt::NoBrush);
        if (named(m_tool, {"Lasso", "Polygonal Lasso", "Magnetic Lasso"}) || m_transportSelecting) {
            painter.drawPolyline(m_lasso);
            if (!m_lasso.isEmpty())
                painter.drawLine(m_lasso.last(), toDocument(m_cursor));
        } else if (m_tool == "Elliptical Marquee" || m_tool == "Ellipse")
            painter.drawEllipse(m_dragRect);
        else if (m_tool == "Gradient" || m_tool == "Line")
            painter.drawLine(m_start, toDocument(m_cursor));
        else if (m_tool == "Triangle" || m_tool == "Polygon")
            painter.drawPath(polygonShape(m_dragRect, m_tool == "Triangle" ? 3 : 6));
        else
            painter.drawRect(m_dragRect);
    }
    if (!m_penPath.isEmpty() && extras) {
        QPen pen(palette().color(QPalette::Highlight), 1);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(m_penPath);
        for (int i = 0; i < m_penPath.elementCount(); ++i) {
            auto element = m_penPath.elementAt(i);
            painter.fillRect(QRectF(element.x - 2 / m_zoom, element.y - 2 / m_zoom, 4 / m_zoom, 4 / m_zoom),
                             palette().color(QPalette::Highlight));
        }
    }
    if (m_tool == "Direct Selection" && extras) {
        const Layer *layer = m_document->activeLayer();
        if (layer && layer->kind == LayerKind::Shape) {
            painter.save();
            painter.translate(effectiveOffset(m_document, *layer));
            QPen outline(palette().color(QPalette::Highlight), 1);
            outline.setCosmetic(true);
            painter.setPen(outline);
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(layer->shape);
            for (int i = 0; i < layer->shape.elementCount(); ++i) {
                const auto element = layer->shape.elementAt(i);
                painter.fillRect(
                    QRectF(element.x - 3 / m_zoom, element.y - 3 / m_zoom, 6 / m_zoom, 6 / m_zoom),
                    palette().color(QPalette::Highlight));
            }
            painter.restore();
        }
    }
    if (property("showTransformControls").toBool() && !m_transformActive) {
        const QRectF bounds = activeLayerBounds();
        if (!bounds.isEmpty()) {
            QPen outline(palette().color(QPalette::Highlight), 1);
            outline.setCosmetic(true);
            painter.setPen(outline);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(bounds);
            const QList<QPointF> handles{bounds.topLeft(),
                                         bounds.topRight(),
                                         bounds.bottomRight(),
                                         bounds.bottomLeft(),
                                         {bounds.center().x(), bounds.top()},
                                         {bounds.center().x(), bounds.bottom()},
                                         {bounds.left(), bounds.center().y()},
                                         {bounds.right(), bounds.center().y()}};
            painter.setBrush(palette().color(QPalette::Base));
            for (const QPointF handle : handles)
                painter.drawRect(
                    QRectF(handle - QPointF(3 / m_zoom, 3 / m_zoom), QSizeF(6 / m_zoom, 6 / m_zoom)));
        }
    }
    if (!m_perspectivePoints.isEmpty() && m_perspectivePoints.size() < 4) {
        QPen outline(palette().color(QPalette::Highlight), 1);
        outline.setCosmetic(true);
        painter.setPen(outline);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(m_perspectivePoints);
        painter.drawLine(m_perspectivePoints.last(), toDocument(m_cursor));
        for (const QPointF point : m_perspectivePoints)
            painter.fillRect(QRectF(point - QPointF(3 / m_zoom, 3 / m_zoom), QSizeF(6 / m_zoom, 6 / m_zoom)),
                             palette().color(QPalette::Highlight));
    }
    drawCropPreview(painter);
    drawTransformPreview(painter);
    if (m_straightening) {
        QPen straighten(QColor("#58d1d8"), 1);
        straighten.setCosmetic(true);
        painter.setPen(straighten);
        painter.drawLine(m_start, toDocument(m_cursor));
    }
    if (paintTool(m_tool) && underMouse() && !m_panning) {
        const QPointF cursor = toDocument(m_cursor);
        QPen pen(Qt::white, 1);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const qreal radius = m_brushSize / 2.0;
        painter.drawEllipse(cursor, radius, radius);
        pen.setColor(Qt::black);
        painter.setPen(pen);
        painter.drawEllipse(cursor, radius + 1 / m_zoom, radius + 1 / m_zoom);
        painter.drawLine(cursor + QPointF(-3 / m_zoom, 0), cursor + QPointF(3 / m_zoom, 0));
        painter.drawLine(cursor + QPointF(0, -3 / m_zoom), cursor + QPointF(0, 3 / m_zoom));
    }
    painter.resetTransform();
    painter.setClipping(false);
    if (m_tool == "Perspective Crop" || m_cropActive || m_transformActive) {
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(
            QPointF(32, height() - 16),
            m_transformActive ? tr("Free Transform: drag handles; outside rotates; Alt scales from center; "
                                   "Shift toggles aspect. Enter applies; Esc cancels.")
            : m_tool == "Perspective Crop" && m_perspectivePoints.size() < 4
                ? tr("Click four corners clockwise. Enter applies; Esc cancels.")
            : m_straightenArmed ? tr("Drag along a horizon or vertical line. Enter applies; Esc cancels.")
                                : tr("Drag handles to crop. Enter applies; Esc cancels. O cycles overlays."));
    }
    if (m_rulers) {
        painter.fillRect(0, 0, width(), 20, palette().color(QPalette::Base));
        painter.fillRect(0, 0, 20, height(), palette().color(QPalette::Base));
        painter.setPen(palette().color(QPalette::Text));
        QFont font = painter.font();
        font.setPixelSize(9);
        painter.setFont(font);
        const qreal ideal = 70 / m_zoom;
        const qreal power = std::pow(10, std::floor(std::log10(std::max(ideal, qreal(1)))));
        const qreal major = (ideal / power < 2 ? 2 : ideal / power < 5 ? 5 : 10) * power;
        const QPointF origin = fromDocument({0, 0});
        if (std::abs(std::remainder(m_rotation, 180.0)) < 0.1) {
            const qreal sign = m_flip ? -1 : 1;
            for (qreal value = std::floor(toDocument({20, 20}).x() / major) * major - major;
                 value <= std::max(toDocument({qreal(width()), 20}).x(), toDocument({20, 20}).x()) + major;
                 value += major) {
                const qreal x = origin.x() + value * m_zoom * sign;
                if (x < 20 || x > width())
                    continue;
                painter.drawLine(QPointF(x, 12), QPointF(x, 20));
                painter.drawText(QPointF(x + 3, 10), QString::number(value, 'f', 0));
                for (int i = 1; i < 5; ++i) {
                    const qreal minor = x + major * m_zoom * sign * i / 5;
                    painter.drawLine(QPointF(minor, 16), QPointF(minor, 20));
                }
            }
            for (qreal value = std::floor(toDocument({20, 20}).y() / major) * major;
                 value < toDocument({20, qreal(height())}).y() + major; value += major) {
                const qreal y = origin.y() + value * m_zoom;
                if (y < 20 || y > height())
                    continue;
                painter.drawLine(QPointF(12, y), QPointF(20, y));
                painter.save();
                painter.translate(10, y + 3);
                painter.rotate(-90);
                painter.drawText(QPointF(0, 0), QString::number(value, 'f', 0));
                painter.restore();
                for (int i = 1; i < 5; ++i) {
                    const qreal minor = y + major * m_zoom * i / 5;
                    painter.drawLine(QPointF(16, minor), QPointF(20, minor));
                }
            }
        }
        painter.fillRect(0, 0, 20, 20, palette().color(QPalette::Button));
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawLine(20, 0, 20, height());
        painter.drawLine(0, 20, width(), 20);
    }
}

void CanvasView::chooseSelectionOperation(Qt::KeyboardModifiers modifiers) {
    m_selectionOperation = modifiers.testFlag(Qt::ShiftModifier)
                               ? (modifiers.testFlag(Qt::AltModifier) ? "intersect" : "add")
                               : (modifiers.testFlag(Qt::AltModifier) ? "subtract" : "replace");
}
void CanvasView::sampleColor(QPointF point) {
    const QImage composite = m_document->composite();
    const QPoint p = point.toPoint();
    if (composite.rect().contains(p)) {
        m_foreground = composite.pixelColor(p);
        emit colorPicked(m_foreground);
    }
}

void CanvasView::applyImage(const QImage &input, QPoint documentOrigin, bool erase) {
    Layer *layer = m_document->activeLayer();
    if (!layer || layer->locked || input.isNull())
        return;
    QImage image = input.depth() > 32 ? input.copy() : input.convertToFormat(QImage::Format_ARGB32);
    const QRect docBounds = QRect(documentOrigin, image.size()).intersected(documentRect(m_document));
    if (docBounds.isEmpty())
        return;
    const bool selectionActive = m_selectionActive;
    if (m_quickMask) {
        QImage mask = normalizeMask(m_document->state.selection);
        if (mask.isNull())
            mask = makeMask(m_document->state.size, m_document->state.bitDepth, 1);
        for (int y = 0; y < image.height(); ++y) {
            const int sy = y + documentOrigin.y();
            if (sy < 0 || sy >= mask.height())
                continue;
            for (int x = 0; x < image.width(); ++x) {
                const int sx = x + documentOrigin.x();
                if (sx < 0 || sx >= mask.width())
                    continue;
                const QColor pixel = image.pixelColor(x, y);
                const qreal alpha = pixel.alphaF();
                const qreal gray = (pixel.redF() * 11 + pixel.greenF() * 16 + pixel.blueF() * 5) / 32;
                setMaskSample(mask, sx, sy,
                              maskSample(mask, sx, sy) * (1 - alpha) + (erase ? 1 : gray) * alpha);
            }
        }
        m_document->state.selection = mask;
        m_document->touch();
        return;
    }
    if (selectionActive)
        for (int y = 0; y < image.height(); ++y) {
            auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const int sx = x + documentOrigin.x(), sy = y + documentOrigin.y();
                if (image.depth() > 32) {
                    QColor pixel = image.pixelColor(x, y);
                    pixel.setAlphaF(pixel.alphaF() * maskSample(m_document->state.selection, sx, sy));
                    image.setPixelColor(x, y, pixel);
                } else {
                    const int a = qRound(qAlpha(row[x]) * maskSample(m_document->state.selection, sx, sy));
                    row[x] = qRgba(qRed(row[x]), qGreen(row[x]), qBlue(row[x]), a);
                }
            }
        }
    QPoint localOrigin = documentOrigin - effectiveOffset(m_document, *layer).toPoint();
    if (layer->maskTarget && !layer->mask.isNull()) {
        if (!layer->maskLinked)
            localOrigin -= layer->maskOffset.toPoint();
        QImage mask = normalizeMask(layer->mask);
        for (int y = 0; y < image.height(); ++y) {
            const int ly = y + localOrigin.y();
            if (ly < 0 || ly >= mask.height())
                continue;
            for (int x = 0; x < image.width(); ++x) {
                const int lx = x + localOrigin.x();
                if (lx < 0 || lx >= mask.width())
                    continue;
                const QColor pixel = image.pixelColor(x, y);
                const qreal alpha = pixel.alphaF();
                const qreal value =
                    erase ? 1 : (pixel.redF() * 11 + pixel.greenF() * 16 + pixel.blueF() * 5) / 32;
                setMaskSample(mask, lx, ly, maskSample(mask, lx, ly) * (1 - alpha) + value * alpha);
            }
        }
        layer->mask = mask;
    } else {
        if (layer->kind == LayerKind::Group || layer->kind == LayerKind::Artboard ||
            layer->kind == LayerKind::Adjustment)
            return;
        if (erase && layer->lockAlpha)
            return;
        if (layer->kind != LayerKind::Pixel && layer->kind != LayerKind::SmartObject) {
            const QImage raster = m_document->layerImage(*layer);
            layer->pixels.setImage(raster);
            layer->kind = LayerKind::Pixel;
        }
        if (!erase && !layer->lockAlpha) {
            QRect localBounds = docBounds.translated(-effectiveOffset(m_document, *layer).toPoint());
            const QPoint shift(localBounds.left() < 0 ? ((-localBounds.left() + 255) / 256) * 256 : 0,
                               localBounds.top() < 0 ? ((-localBounds.top() + 255) / 256) * 256 : 0);
            if (!shift.isNull()) {
                shiftTiles(layer->pixels, shift);
                shiftTiles(m_originalPixels, shift);
                layer->offset -= shift;
                localOrigin += shift;
                localBounds.translate(shift);
                if (!layer->mask.isNull()) {
                    QImage mask(layer->pixels.size, layer->mask.format());
                    mask.fill(Qt::white);
                    QPainter painter(&mask);
                    painter.drawImage(shift, layer->mask);
                    painter.end();
                    layer->mask = mask;
                }
            }
            layer->pixels.size =
                layer->pixels.size.expandedTo(QSize(localBounds.right() + 1, localBounds.bottom() + 1));
        }
        const QRect bounds =
            QRect(localOrigin, image.size()).intersected(QRect(QPoint(), layer->pixels.size));
        if (m_dragging && named(m_tool, {"History Brush", "Art History Brush"}) &&
            !m_historySource.isNull()) {
            QImage restored = layer->pixels.region(QRect(localOrigin, image.size()));
            const QImage source =
                m_historySource.convertToFormat(restored.format()).copy(QRect(localOrigin, image.size()));
            for (int y = 0; y < restored.height(); ++y)
                for (int x = 0; x < restored.width(); ++x) {
                    const qreal amount = image.pixelColor(x, y).alphaF();
                    if (amount <= 0)
                        continue;
                    if (restored.format() == QImage::Format_RGBA32FPx4) {
                        float *target = reinterpret_cast<float *>(restored.scanLine(y)) + x * 4;
                        const float *sample =
                            reinterpret_cast<const float *>(source.constScanLine(y)) + x * 4;
                        for (int channel = 0; channel < (layer->lockAlpha ? 3 : 4); ++channel)
                            target[channel] =
                                float(target[channel] * (1 - amount) + sample[channel] * amount);
                    } else if (restored.format() == QImage::Format_RGBA64) {
                        auto *target = reinterpret_cast<QRgba64 *>(restored.scanLine(y)) + x;
                        const auto sample = reinterpret_cast<const QRgba64 *>(source.constScanLine(y))[x];
                        *target = QRgba64::fromRgba64(
                            qRound(target->red() * (1 - amount) + sample.red() * amount),
                            qRound(target->green() * (1 - amount) + sample.green() * amount),
                            qRound(target->blue() * (1 - amount) + sample.blue() * amount),
                            layer->lockAlpha
                                ? target->alpha()
                                : qRound(target->alpha() * (1 - amount) + sample.alpha() * amount));
                    } else {
                        uchar *target = restored.scanLine(y) + x * 4;
                        const uchar *sample = source.constScanLine(y) + x * 4;
                        for (int channel = 0; channel < (layer->lockAlpha ? 3 : 4); ++channel)
                            target[channel] =
                                uchar(qRound(target[channel] * (1 - amount) + sample[channel] * amount));
                    }
                }
            layer->pixels.paint(bounds, [&](QPainter &painter) {
                painter.setCompositionMode(QPainter::CompositionMode_Source);
                painter.drawImage(localOrigin, restored);
            });
            m_document->touch();
            return;
        }
        const QString brushMode =
            paintTool(m_tool) && m_dragging ? property("brushBlendMode").toString() : QString();
        const bool clearBrush = brushMode == "Clear";
        const bool behindBrush = brushMode == "Behind";
        if (layer->lockAlpha && (clearBrush || behindBrush))
            return;
        if (!erase && !brushMode.isEmpty() && brushMode != "Normal" && !clearBrush && !behindBrush) {
            const QImage backdrop = layer->pixels.region(QRect(localOrigin, image.size()));
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const QColor destination = backdrop.pixelColor(x, y), source = image.pixelColor(x, y);
                    if (destination.alphaF() > 0 && source.alphaF() > 0)
                        image.setPixelColor(x, y, blendColor(destination, source, brushMode));
                }
        }
        layer->pixels.paint(bounds, [&](QPainter &painter) {
            painter.setCompositionMode(erase || clearBrush ? QPainter::CompositionMode_DestinationOut
                                       : behindBrush       ? QPainter::CompositionMode_DestinationOver
                                       : layer->lockAlpha  ? QPainter::CompositionMode_SourceAtop
                                                           : QPainter::CompositionMode_SourceOver);
            painter.drawImage(localOrigin, image);
        });
    }
    m_document->touch();
}

void CanvasView::dab(const BrushDab &sample) {
    const QPointF point = sample.position;
    if (sample.opacity <= 0 || sample.flow <= 0)
        return;
    if (sample.opacity <= 0 || sample.flow <= 0)
        return;
    if (!documentRect(m_document).contains(point.toPoint()))
        return;
    if (m_tool == "Quick Selection") {
        QImage mask(m_document->state.size, QImage::Format_Grayscale8);
        mask.fill(0);
        const QImage image = m_document->composite();
        const QColor seed = image.pixelColor(point.toPoint());
        const qreal radius = sample.diameter / 2.0;
        const QRect bounds = QRectF(point - QPointF(radius, radius), QSizeF(radius * 2, radius * 2))
                                 .toAlignedRect()
                                 .intersected(image.rect());
        for (int y = bounds.top(); y <= bounds.bottom(); ++y)
            for (int x = bounds.left(); x <= bounds.right(); ++x) {
                const QColor sample = image.pixelColor(x, y);
                const int difference = std::abs(sample.red() - seed.red()) +
                                       std::abs(sample.green() - seed.green()) +
                                       std::abs(sample.blue() - seed.blue());
                if (QLineF(point, QPointF(x, y)).length() <= radius && difference < 110)
                    mask.scanLine(y)[x] = 255;
            }
        m_document->setSelection(mask, m_selectionOperation == "subtract" ? "subtract" : "add");
        return;
    }
    if (named(m_tool, {"Spot Healing", "Patch", "Content-Aware Move"})) {
        if (m_healMask.isNull()) {
            m_healMask = QImage(m_document->state.size, QImage::Format_Grayscale8);
            m_healMask.fill(0);
        }
        QPainter painter(&m_healMask);
        painter.setBrush(Qt::white);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(point, sample.diameter / 2.0, sample.diameter / 2.0);
        return;
    }
    const qreal radius = std::max(qreal(0.5), sample.diameter / 2);
    const int extent = int(std::ceil(radius)) + 1;
    const QPoint origin(int(std::floor(point.x())) - extent, int(std::floor(point.y())) - extent);
    QImage image(extent * 2 + 1, extent * 2 + 1, paintingFormat(m_document));
    image.fill(Qt::transparent);
    const Layer *layer = m_document->activeLayer();
    if (!layer || layer->locked)
        return;
    const bool erase = named(m_tool, {"Eraser", "Background Eraser"});
    const bool pixelFilter = named(m_tool, {"Blur", "Sharpen", "Smudge", "Dodge", "Burn", "Sponge",
                                            "Color Replacement", "Mixer Brush", "Red Eye"});
    QImage region;
    if (pixelFilter || m_tool == "Background Eraser")
        region = m_originalPixels.region(
            QRect(origin - effectiveOffset(m_document, *layer).toPoint() - QPoint(2, 2),
                  image.size() + QSize(4, 4)));
    QImage stamp;
    if (named(m_tool, {"Clone Stamp", "Healing Brush"}) && m_hasCloneSource) {
        const QPoint sourceOrigin =
            (m_cloneSource + QPointF(origin) - m_start - effectiveOffset(m_document, *layer)).toPoint();
        stamp = m_originalPixels.region(QRect(sourceOrigin, image.size()));
    }
    QPoint healingDifference;
    qreal healingBlueDifference = 0;
    if (m_tool == "Healing Brush" && !stamp.isNull()) {
        const QImage targetRegion = m_originalPixels.region(
            QRect(origin - effectiveOffset(m_document, *layer).toPoint(), image.size()));
        qreal tr = 0, tg = 0, tb = 0, sr = 0, sg = 0, sb = 0;
        int samples = 0;
        for (int y = 0; y < stamp.height(); y += 3)
            for (int x = 0; x < stamp.width(); x += 3) {
                const QColor target = targetRegion.pixelColor(x, y), sample = stamp.pixelColor(x, y);
                if (target.alpha() && sample.alpha()) {
                    tr += target.red();
                    tg += target.green();
                    tb += target.blue();
                    sr += sample.red();
                    sg += sample.green();
                    sb += sample.blue();
                    ++samples;
                }
            }
        if (samples) {
            healingDifference = QPoint(int((tr - sr) / samples), int((tg - sg) / samples));
            healingBlueDifference = (tb - sb) / samples;
        }
    }
    const qreal radians = sample.angle * std::numbers::pi / 180;
    const qreal cosine = std::cos(radians), sine = std::sin(radians);
    for (int y = 0; y < image.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const qreal dx = origin.x() + x + 0.5 - point.x(), dy = origin.y() + y + 0.5 - point.y();
            const qreal bx = dx * cosine + dy * sine, by = (-dx * sine + dy * cosine) / sample.roundness;
            const qreal distance = std::sqrt(bx * bx + by * by) / radius;
            if (distance > 1)
                continue;
            const qreal edge =
                m_tool == "Pencil"
                    ? 1
                    : (distance <= sample.hardness
                           ? 1
                           : std::pow((1 - distance) / std::max(qreal(0.001), 1 - sample.hardness), 2));
            qreal alpha = edge * sample.flow * sample.opacity;
            QColor color = m_foreground;
            if (!stamp.isNull())
                color = stamp.pixelColor(x, y);
            else if (m_tool == "Clone Stamp" || m_tool == "Healing Brush")
                continue;
            else if (m_tool == "Pattern Stamp")
                color = ((origin.x() + x) / 12 + (origin.y() + y) / 12) % 2 ? m_foreground : m_background;
            else if (named(m_tool, {"History Brush", "Art History Brush"}) && !m_historySource.isNull()) {
                const QPoint p = origin + QPoint(x, y) - effectiveOffset(m_document, *layer).toPoint();
                if (m_historySource.rect().contains(p))
                    color = m_historySource.pixelColor(p);
                color.setAlphaF(1);
            }
            if (m_tool == "Healing Brush") {
                color.setRed(std::clamp(color.red() + healingDifference.x(), 0, 255));
                color.setGreen(std::clamp(color.green() + healingDifference.y(), 0, 255));
                color.setBlue(std::clamp(color.blue() + int(healingBlueDifference), 0, 255));
            }
            if (pixelFilter && !region.isNull()) {
                const QColor old = region.pixelColor(x + 2, y + 2);
                color = old;
                if (m_tool == "Dodge" || m_tool == "Burn") {
                    const qreal amount = m_tool == "Dodge" ? 0.12 : -0.12;
                    color =
                        QColor::fromRgbF(std::clamp(old.redF() + amount, qreal(0), qreal(1)),
                                         std::clamp(old.greenF() + amount, qreal(0), qreal(1)),
                                         std::clamp(old.blueF() + amount, qreal(0), qreal(1)), old.alphaF());
                } else if (m_tool == "Sponge") {
                    const qreal gray = old.redF() * 0.2126 + old.greenF() * 0.7152 + old.blueF() * 0.0722;
                    color =
                        QColor::fromRgbF(old.redF() * 0.85 + gray * 0.15, old.greenF() * 0.85 + gray * 0.15,
                                         old.blueF() * 0.85 + gray * 0.15, old.alphaF());
                } else if (m_tool == "Color Replacement") {
                    color = m_foreground;
                    color.setHslF(m_foreground.hslHueF(), m_foreground.hslSaturationF(), old.lightnessF(),
                                  old.alphaF());
                } else if (m_tool == "Mixer Brush")
                    color = weightedColor(old, m_foreground, 0.25);
                else if (m_tool == "Smudge") {
                    const QPoint delta = (m_last - point).toPoint();
                    color = region.pixelColor(std::clamp(x + 2 + delta.x(), 0, region.width() - 1),
                                              std::clamp(y + 2 + delta.y(), 0, region.height() - 1));
                } else if (m_tool == "Red Eye") {
                    if (old.red() > old.green() * 1.4 && old.red() > old.blue() * 1.4)
                        color.setRed((old.green() + old.blue()) / 2);
                    else
                        alpha = 0;
                } else {
                    qreal r = 0, g = 0, b = 0, a = 0;
                    for (int sy = -1; sy <= 1; ++sy)
                        for (int sx = -1; sx <= 1; ++sx) {
                            const QColor sample = region.pixelColor(x + 2 + sx, y + 2 + sy);
                            r += sample.redF();
                            g += sample.greenF();
                            b += sample.blueF();
                            a += sample.alphaF();
                        }
                    r /= 9;
                    g /= 9;
                    b /= 9;
                    a /= 9;
                    if (m_tool == "Sharpen") {
                        r = std::clamp(old.redF() * 2 - r, qreal(0), qreal(1));
                        g = std::clamp(old.greenF() * 2 - g, qreal(0), qreal(1));
                        b = std::clamp(old.blueF() * 2 - b, qreal(0), qreal(1));
                        a = old.alphaF();
                    }
                    color = QColor::fromRgbF(r, g, b, a);
                }
            }
            if (m_tool == "Background Eraser" && !region.isNull()) {
                const QColor old = region.pixelColor(x + 2, y + 2);
                const int distanceColor = std::abs(old.red() - m_background.red()) +
                                          std::abs(old.green() - m_background.green()) +
                                          std::abs(old.blue() - m_background.blue());
                if (distanceColor > 100)
                    alpha = 0;
            }
            if (alpha <= 0 || color.alphaF() <= 0)
                continue;
            const int docX = origin.x() + x, docY = origin.y() + y;
            if (docX < 0 || docY < 0 || docX >= m_document->state.size.width() ||
                docY >= m_document->state.size.height())
                continue;
            const quint64 key = TileImage::key(docX / 256, docY / 256);
            auto coverage = m_strokeCoverage.find(key);
            if (coverage == m_strokeCoverage.end()) {
                const QImage tile = makeMask({256, 256}, m_document->state.bitDepth);
                coverage = m_strokeCoverage.insert(key, tile);
            }
            const qreal before = maskSample(coverage.value(), docX % 256, docY % 256);
            const qreal cap = std::min(qreal(1), sample.opacity * color.alphaF());
            const qreal after = before + std::max(qreal(0), cap - before) * edge * sample.flow;
            const qreal increment = before < 1 ? (after - before) / (1 - before) : 0;
            // Opacity caps a complete gesture; flow controls accumulation between dabs.
            alpha = increment;
            setMaskSample(coverage.value(), docX % 256, docY % 256, after);
            color.setAlphaF(std::clamp(alpha, qreal(0), qreal(1)));
            if (image.depth() > 32)
                image.setPixelColor(x, y, color);
            else
                row[x] = color.rgba();
        }
    }
    applyImage(image, origin, erase);
}
void CanvasView::drawStroke(QPointF to) {
    for (const BrushDab &sample : m_brushDynamics.append({to, m_pressure, m_tabletTilt}))
        dab(sample);
}

void CanvasView::mousePressEvent(QMouseEvent *event) {
    setFocus();
    m_cursor = event->position();
    const QPointF point = toDocument(event->position());
    const bool temporaryMove = property("externalShortcuts").toBool()
                                   ? property("temporaryMove").toBool()
                                   : event->modifiers().testFlag(Qt::ControlModifier);
    if (event->button() == Qt::RightButton && event->modifiers().testFlag(Qt::AltModifier)) {
        m_hudBrush = true;
        m_start = event->position();
        m_initialBrushSize = m_brushSize;
        m_initialHardness = m_hardness;
        event->accept();
        return;
    }
    if (event->button() == Qt::MiddleButton || m_space || m_tool == "Hand") {
        m_panning = true;
        m_last = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    if (m_transformActive) {
        m_transformHandle = transformHandleAt(point);
        if (m_transformMode == "Rotate" && m_transformHandle != 9)
            m_transformHandle = 10;
        m_initialTransformQuad = m_transformQuad;
        m_initialTransformPivot = m_transformPivot;
        m_start = point;
        m_last = point;
        m_dragging = true;
        update();
        return;
    }
    if (m_tool == "Crop") {
        m_start = point;
        m_last = point;
        m_initialCropRect = m_cropRect;
        m_initialCropAngle = m_cropSettings.angle;
        if (m_straightenArmed) {
            m_straightening = true;
            m_dragging = true;
            return;
        }
        m_cropHandle = cropHandleAt(point);
        if (m_cropHandle == 9) {
            m_cropRotating = true;
            m_dragging = true;
        } else {
            if (m_cropHandle == -1) {
                m_cropHandle = 10;
                m_cropRect = QRectF(point, QSizeF(1, 1));
                m_cropActive = true;
            }
            m_dragging = true;
        }
        update();
        return;
    }
    if (m_rulers && (event->position().x() < 20 || event->position().y() < 20)) {
        m_guideDrag = event->position().x() < 20 ? 1 : 2;
        m_guidePosition = m_guideDrag == 1 ? point.x() : point.y();
        return;
    }
    if (m_tool == "Rotate View") {
        m_rotating = true;
        m_start = event->position();
        m_initialRotation = m_rotation;
        return;
    }
    if (m_tool == "Zoom") {
        const QPointF anchor = point;
        setZoom(m_zoom * (event->modifiers().testFlag(Qt::AltModifier) ? 0.8 : 1.25));
        m_pan += event->position() - fromDocument(anchor);
        update();
        return;
    }
    if (named(m_tool, {"Eyedropper", "3D Material Eyedropper", "Color Sampler"}) ||
        (paintTool(m_tool) && event->modifiers().testFlag(Qt::AltModifier) &&
         !named(m_tool, {"Clone Stamp", "Healing Brush"}))) {
        sampleColor(point);
        return;
    }
    if (named(m_tool, {"Clone Stamp", "Healing Brush"}) && event->modifiers().testFlag(Qt::AltModifier)) {
        m_cloneSource = point;
        m_hasCloneSource = true;
        return;
    }
    if (named(m_tool,
              {"Type", "Horizontal Type", "Vertical Type", "Horizontal Type Mask", "Vertical Type Mask"})) {
        bool accepted = false;
        const QString text =
            QInputDialog::getMultiLineText(this, tr("Type"), tr("Text"), tr("Serika"), &accepted);
        if (accepted && !text.isEmpty())
            m_document->mutate("Type", [&] {
                m_document->addLayer("Text: " + text.left(24), LayerKind::Text);
                Layer *layer = m_document->activeLayer();
                layer->text = text;
                layer->font = property("typeFont").isValid() ? qvariant_cast<QFont>(property("typeFont"))
                                                             : QFont("Arial");
                const int typeSize = property("typeSize").toInt();
                layer->font.setPointSize(typeSize > 0 ? typeSize : 48);
                layer->color = m_foreground;
                layer->offset = point;
                if (m_tool == "Vertical Type")
                    layer->parameters["vertical"] = true;
            });
        return;
    }
    if (named(m_tool,
              {"Pen", "Freeform Pen", "Curvature Pen", "Add Anchor", "Delete Anchor", "Convert Point"})) {
        m_document->mutate("Pen path", [&] {
            if (!m_penLayerId) {
                m_document->addLayer("Pen path", LayerKind::Shape);
                m_penLayerId = m_document->activeLayer()->id;
                m_penPath.moveTo(point);
            } else if (m_tool == "Delete Anchor" && m_penPath.elementCount() > 1) {
                QPainterPath path;
                for (int i = 0; i < m_penPath.elementCount() - 1; ++i) {
                    const auto e = m_penPath.elementAt(i);
                    if (!i)
                        path.moveTo(e.x, e.y);
                    else
                        path.lineTo(e.x, e.y);
                }
                m_penPath = path;
            } else if (m_tool == "Curvature Pen") {
                const QPointF previous = m_penPath.currentPosition();
                const QPointF delta = point - previous;
                QPointF tangent = delta / 3;
                if (m_penPath.elementCount() > 1) {
                    const auto element = m_penPath.elementAt(m_penPath.elementCount() - 2);
                    tangent = (point - QPointF(element.x, element.y)) / 6;
                }
                m_penPath.cubicTo(previous + tangent, point - delta / 3, point);
            } else
                m_penPath.lineTo(point);
            const int index = m_document->indexForId(m_penLayerId);
            if (index >= 0) {
                Layer &layer = m_document->state.layers[index];
                layer.shape = m_penPath;
                layer.color = Qt::transparent;
                layer.stroke = m_foreground;
                layer.strokeWidth = std::max(1, m_brushSize / 10);
            }
        });
        update();
        return;
    }
    if (m_tool == "Note" || m_tool == "Count") {
        m_document->mutate(m_tool, [&] {
            const QString key = m_tool == "Note" ? "notes" : "counts";
            QJsonArray array = m_document->state.metadata.value(key).toArray();
            QJsonObject note{{"x", point.x()}, {"y", point.y()}};
            if (m_tool == "Note") {
                bool accepted;
                const QString text =
                    QInputDialog::getMultiLineText(this, tr("Note"), tr("Note text"), {}, &accepted);
                if (!accepted)
                    return;
                note["text"] = text;
            }
            array.append(note);
            m_document->state.metadata[key] = array;
        });
        return;
    }
    chooseSelectionOperation(event->modifiers());
    if (m_tool == "Perspective Crop") {
        if (m_perspectivePoints.size() < 4) {
            m_perspectivePoints.append(point);
            emit cropPreviewChanged(m_perspectivePoints.size() == 4);
        } else {
            m_perspectiveHandle = -1;
            for (int i = 0; i < 4; ++i)
                if (QLineF(point, m_perspectivePoints[i]).length() < 10 / m_zoom)
                    m_perspectiveHandle = i;
            if (m_perspectiveHandle < 0 && m_perspectivePoints.containsPoint(point, Qt::OddEvenFill))
                m_perspectiveHandle = 4;
            if (m_perspectiveHandle >= 0) {
                m_dragging = true;
                m_start = point;
                m_last = point;
            }
        }
        update();
        return;
    }
    if (transportTool(m_tool) && !temporaryMove) {
        Layer *layer = m_document->activeLayer();
        if (!layer || layer->locked || layer->maskTarget)
            return;
        m_dragging = true;
        m_start = point;
        m_last = point;
        m_transportDelta = {};
        if (m_document->hasSelection() && selectionAt(m_document, qRound(point.x()), qRound(point.y())) > 0) {
            m_transporting = true;
            m_transportMask = m_document->state.selection;
            m_originalPixels = layer->pixels;
            m_strokeSource = m_document->layerImage(*layer);
            m_transportOrigin = effectiveOffset(m_document, *layer);
            m_transportPreview = m_document->composite();
            for (int y = 0; y < m_transportPreview.height(); ++y)
                for (int x = 0; x < m_transportPreview.width(); ++x) {
                    QColor color = m_transportPreview.pixelColor(x, y);
                    color.setAlphaF(color.alphaF() * maskSample(m_transportMask, x, y));
                    m_transportPreview.setPixelColor(x, y, color);
                }
        } else {
            m_transportSelecting = true;
            m_lasso = {point};
        }
        update();
        return;
    }
    if (m_tool == "Direct Selection") {
        Layer *layer = m_document->activeLayer();
        if (layer && layer->kind == LayerKind::Shape && !layer->locked) {
            qreal nearest = 14 / m_zoom;
            const QPointF local = point - effectiveOffset(m_document, *layer);
            m_pathElement = -1;
            for (int i = 0; i < layer->shape.elementCount(); ++i) {
                const auto anchor = layer->shape.elementAt(i);
                const qreal distance = QLineF(local, QPointF(anchor.x, anchor.y)).length();
                if (distance < nearest) {
                    nearest = distance;
                    m_pathElement = i;
                }
            }
            if (m_pathElement >= 0) {
                m_document->beginTransaction("Edit path anchor");
                m_initialPath = layer->shape;
                m_start = point;
                m_dragging = true;
                return;
            }
        }
    }
    if (m_tool == "Magic Wand" || m_tool == "Magic Eraser" || m_tool == "Paint Bucket") {
        m_document->beginTransaction(m_tool);
        bucket(point.toPoint());
        m_document->endTransaction();
        return;
    }
    if (m_tool == "Object Selection") {
        selectSubject();
        return;
    }
    if (named(m_tool, {"Polygonal Lasso", "Magnetic Lasso"})) {
        if (!m_dragging) {
            m_dragging = true;
            m_start = point;
            m_lasso.clear();
        }
        m_lasso.append(point);
        m_last = point;
        update();
        return;
    }
    m_dragging = true;
    m_moving = false;
    m_start = point;
    m_last = point;
    m_dragRect = QRectF(point, point);
    m_lasso.clear();
    m_lasso.append(point);
    if (m_tool == "Ruler")
        return;
    if (named(m_tool, {"Move", "Path Selection", "Direct Selection"}) || temporaryMove) {
        const bool moveTool = m_tool == "Move" || temporaryMove;
        QVector<quint64> chosen = moveTool ? selectedMoveRoots() : QVector<quint64>{};
        if (moveTool && (!property("autoSelect").isValid() || property("autoSelect").toBool())) {
            for (int i = int(m_document->state.layers.size()) - 1; i >= 0; --i) {
                const Layer &candidate = m_document->state.layers[i];
                if (!candidate.visible || candidate.opacity <= 0 || candidate.kind == LayerKind::Group ||
                    candidate.kind == LayerKind::Artboard || candidate.kind == LayerKind::Adjustment)
                    continue;
                bool visible = true;
                quint64 parent = candidate.parentId;
                QSet<quint64> visited{candidate.id};
                while (parent && !visited.contains(parent)) {
                    visited.insert(parent);
                    const int ancestor = m_document->indexForId(parent);
                    if (ancestor < 0)
                        break;
                    const Layer &group = m_document->state.layers[ancestor];
                    if (!group.visible || group.opacity <= 0) {
                        visible = false;
                        break;
                    }
                    parent = group.parentId;
                }
                if (!visible)
                    continue;
                const QPoint local = (point - effectiveOffset(m_document, candidate)).toPoint();
                QColor color;
                if (candidate.kind == LayerKind::Pixel || candidate.kind == LayerKind::SmartObject)
                    color = candidate.pixels.region(QRect(local, QSize(1, 1))).pixelColor(0, 0);
                else {
                    const QImage image = m_document->layerImage(candidate);
                    if (image.rect().contains(local))
                        color = image.pixelColor(local);
                }
                if (color.alpha() < 5)
                    continue;
                if (candidate.maskEnabled && !candidate.mask.isNull()) {
                    const QPoint maskPoint =
                        local - (candidate.maskLinked ? QPoint() : candidate.maskOffset.toPoint());
                    if (1 - candidate.maskDensity *
                                (1 - maskSample(candidate.mask, maskPoint.x(), maskPoint.y())) <
                        .02)
                        continue;
                }
                if (candidate.vectorMaskEnabled && !candidate.vectorMask.isEmpty() &&
                    candidate.vectorMaskDensity > .98 &&
                    !candidate.vectorMask.contains(QPointF(local) - (candidate.vectorMaskLinked
                                                                         ? QPointF()
                                                                         : candidate.vectorMaskOffset)))
                    continue;
                if (!selectedAncestor(m_document, candidate.id, chosen))
                    chosen = {candidate.id};
                m_document->setActiveIndex(i);
                break;
            }
        }
        Layer *layer = m_document->activeLayer();
        if (!layer || layer->locked || layer->lockPosition) {
            m_dragging = false;
            return;
        }
        if (chosen.isEmpty())
            chosen = {layer->id};
        m_moveLayerIds = selectedLayerRoots(m_document, chosen);
        m_moveAppliedDelta = {};
        m_moveMaskOnly = moveTool && m_moveLayerIds.size() == 1 && m_moveLayerIds.first() == layer->id &&
                         layer->maskTarget && !layer->mask.isNull() && !layer->maskLinked;
        m_moving = true;
        m_document->beginTransaction(m_moveMaskOnly              ? "Move layer mask"
                                     : m_moveLayerIds.size() > 1 ? "Move layers"
                                                                 : "Move layer");
        return;
    }
    if (paintTool(m_tool)) {
        Layer *layer = m_document->activeLayer();
        if (!layer || layer->locked) {
            m_dragging = false;
            return;
        }
        m_document->beginTransaction(m_tool);
        m_originalPixels = layer->pixels;
        if (named(m_tool, {"History Brush", "Art History Brush"})) {
            const int sourceIndex =
                property("historySourceIndex").isValid() ? property("historySourceIndex").toInt() : -1;
            m_historySource = m_document->historyLayerImage(layer->id, sourceIndex);
            if (m_historySource.isNull()) {
                m_document->cancelTransaction();
                m_dragging = false;
                return;
            }
        }
        m_healMask = {};
        m_strokeCoverage.clear();
        const bool straightLine = event->modifiers().testFlag(Qt::ShiftModifier) && m_hasLastStrokePoint;
        const quint64 seed = property("brushRandomSeed").isValid() ? property("brushRandomSeed").toULongLong()
                                                                   : QRandomGenerator::global()->generate64();
        const QPointF first = straightLine ? m_lastStrokePoint : point;
        for (const BrushDab &sample :
             m_brushDynamics.begin(brushPreset(), {first, m_pressure, m_tabletTilt}, seed))
            dab(sample);
        if (straightLine)
            drawStroke(point);
    }
    update();
}

void CanvasView::mouseMoveEvent(QMouseEvent *event) {
    m_cursor = event->position();
    const QPointF point = toDocument(m_cursor);
    const QImage composite = m_document->composite();
    const QPoint sample = point.toPoint();
    emit cursorInfo(point, composite.rect().contains(sample) ? composite.pixelColor(sample)
                                                             : QColor(Qt::transparent));
    if (m_hudBrush) {
        setBrushSize(m_initialBrushSize + int(event->position().x() - m_start.x()));
        setBrushHardness(m_initialHardness - (event->position().y() - m_start.y()) / 200);
        return;
    }
    if (m_panning) {
        m_pan += event->position() - m_last;
        m_last = event->position();
        update();
        return;
    }
    if (m_rotating) {
        const QPointF center(width() / 2.0, height() / 2.0);
        const qreal first = std::atan2(m_start.y() - center.y(), m_start.x() - center.x());
        const qreal current = std::atan2(m_cursor.y() - center.y(), m_cursor.x() - center.x());
        rotateView(m_initialRotation + (current - first) * 180 / std::numbers::pi);
        return;
    }
    if (m_guideDrag) {
        m_guidePosition = m_guideDrag == 1 ? point.x() : point.y();
        update();
        return;
    }
    if (m_transformActive && m_dragging && m_transformHandle >= 0) {
        updateTransformDrag(point, event->modifiers());
        return;
    }
    if (m_straightening) {
        update();
        return;
    }
    if (m_cropRotating) {
        const QPointF center = QRectF(documentRect(m_document)).center();
        const qreal first = std::atan2(m_start.y() - center.y(), m_start.x() - center.x());
        const qreal current = std::atan2(point.y() - center.y(), point.x() - center.x());
        m_cropSettings.angle =
            m_initialCropAngle + std::remainder((current - first) * 180 / std::numbers::pi, 360.0);
        if (event->modifiers().testFlag(Qt::ShiftModifier))
            m_cropSettings.angle = qRound(m_cropSettings.angle / 15) * 15;
        emit cropSettingsChanged();
        update();
        return;
    }
    if (m_dragging && m_cropHandle >= 0) {
        updateCropDrag(point, event->modifiers());
        return;
    }
    if (m_dragging && m_perspectiveHandle >= 0) {
        if (m_perspectiveHandle < 4)
            m_perspectivePoints[m_perspectiveHandle] = point;
        else
            m_perspectivePoints.translate(point - m_last);
        m_last = point;
        update();
        return;
    }
    if (m_transporting) {
        m_transportDelta = point - m_start;
        if (event->modifiers().testFlag(Qt::ShiftModifier)) {
            if (std::abs(m_transportDelta.x()) >= std::abs(m_transportDelta.y()))
                m_transportDelta.setY(0);
            else
                m_transportDelta.setX(0);
        }
        update();
        return;
    }
    if (m_transportSelecting) {
        if (QLineF(m_lasso.last(), point).length() > 1 / m_zoom)
            m_lasso.append(point);
        m_dragRect = QRectF(m_start, point).normalized();
        update();
        return;
    }
    if (!m_dragging) {
        update();
        return;
    }
    if (m_pathElement >= 0) {
        Layer *layer = m_document->activeLayer();
        if (layer) {
            layer->shape = m_initialPath;
            const auto anchor = m_initialPath.elementAt(m_pathElement);
            const QPointF delta = point - m_start;
            layer->shape.setElementPositionAt(m_pathElement, anchor.x + delta.x(), anchor.y + delta.y());
            const int last = m_initialPath.elementCount() - 1;
            if (last > 0 && (m_pathElement == 0 || m_pathElement == last)) {
                const auto first = m_initialPath.elementAt(0), end = m_initialPath.elementAt(last);
                if (first.x == end.x && first.y == end.y)
                    layer->shape.setElementPositionAt(m_pathElement == 0 ? last : 0, anchor.x + delta.x(),
                                                      anchor.y + delta.y());
            }
            m_document->touch();
        }
    } else if (m_moving) {
        updateMove(point, event->modifiers());
    } else if (paintTool(m_tool)) {
        QPointF target = point;
        if (event->modifiers().testFlag(Qt::ShiftModifier)) {
            const QPointF delta = point - m_start;
            target = std::abs(delta.x()) >= std::abs(delta.y()) ? QPointF(point.x(), m_start.y())
                                                                : QPointF(m_start.x(), point.y());
        }
        drawStroke(target);
        m_last = target;
        update();
        return;
    } else if (m_tool == "Lasso" || m_tool == "Magnetic Lasso") {
        QPointF snapped = point;
        if (m_tool == "Magnetic Lasso" && composite.rect().contains(sample)) {
            int strongest = 0;
            for (int dy = -5; dy <= 5; ++dy)
                for (int dx = -5; dx <= 5; ++dx) {
                    const QPoint p = sample + QPoint(dx, dy);
                    if (!composite.rect().adjusted(1, 1, -1, -1).contains(p))
                        continue;
                    const int strength = std::abs(qGray(composite.pixel(p + QPoint(1, 0))) -
                                                  qGray(composite.pixel(p - QPoint(1, 0)))) +
                                         std::abs(qGray(composite.pixel(p + QPoint(0, 1))) -
                                                  qGray(composite.pixel(p - QPoint(0, 1))));
                    if (strength > strongest) {
                        strongest = strength;
                        snapped = p;
                    }
                }
        }
        if (QLineF(m_lasso.last(), snapped).length() > 1 / m_zoom)
            m_lasso.append(snapped);
    } else {
        QPointF end = point;
        if (event->modifiers().testFlag(Qt::ShiftModifier) && !event->modifiers().testFlag(Qt::AltModifier)) {
            const qreal extent =
                std::max(std::abs(point.x() - m_start.x()), std::abs(point.y() - m_start.y()));
            end = m_start + QPointF(point.x() >= m_start.x() ? extent : -extent,
                                    point.y() >= m_start.y() ? extent : -extent);
        }
        m_dragRect = QRectF(m_start, end).normalized();
        if (m_tool == "Single Row")
            m_dragRect = QRectF(0, point.y(), m_document->state.size.width(), 1);
        if (m_tool == "Single Column")
            m_dragRect = QRectF(point.x(), 0, 1, m_document->state.size.height());
    }
    m_last = point;
    update();
}

void CanvasView::finishSelection() {
    const QImage mask =
        paintCoverage(m_document->state.size, m_document->state.bitDepth, [&](QPainter &painter) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(Qt::white);
            if (named(m_tool, {"Lasso", "Polygonal Lasso", "Magnetic Lasso"}))
                painter.drawPolygon(m_lasso);
            else if (m_tool == "Elliptical Marquee")
                painter.drawEllipse(m_dragRect);
            else
                painter.drawRect(m_dragRect);
        });
    m_document->setSelection(mask, m_selectionOperation);
    m_lasso.clear();
}
void CanvasView::finishTransport() {
    const QPoint delta = m_transportDelta.toPoint();
    if (delta.isNull() || m_strokeSource.isNull() || m_transportMask.isNull())
        return;
    Layer *active = m_document->activeLayer();
    if (!active || active->locked)
        return;
    const bool contentMove = m_tool == "Content-Aware Move";
    const QPoint offset = m_transportOrigin.toPoint();
    const QImage localMask =
        paintCoverage(m_strokeSource.size(), m_document->state.bitDepth,
                      [&](QPainter &painter) { painter.drawImage(-offset, m_transportMask); });
    const QImage original = m_strokeSource;
    QImage repaired = contentMove ? synthesizePatches(original, localMask) : original;
    QImage cutout(original.size(), original.format());
    cutout.fill(Qt::transparent);
    qreal donorRed = 0, donorGreen = 0, donorBlue = 0, targetRed = 0, targetGreen = 0, targetBlue = 0,
          weight = 0;
    if (!contentMove)
        for (int y = 0; y < original.height(); ++y)
            for (int x = 0; x < original.width(); ++x) {
                const qreal coverage = maskSample(localMask, x, y);
                const QPoint donor = QPoint(x, y) + delta;
                if (coverage <= 0 || !original.rect().contains(donor))
                    continue;
                const QColor source = original.pixelColor(donor), target = original.pixelColor(x, y);
                donorRed += source.redF() * coverage;
                donorGreen += source.greenF() * coverage;
                donorBlue += source.blueF() * coverage;
                targetRed += target.redF() * coverage;
                targetGreen += target.greenF() * coverage;
                targetBlue += target.blueF() * coverage;
                weight += coverage;
            }
    const qreal adaptation = std::clamp(
        property("patchColorAdaptation").isValid() ? property("patchColorAdaptation").toDouble() : .7, 0.,
        1.);
    auto mix = [](QColor a, QColor b, qreal amount) {
        return QColor::fromRgbF(
            a.redF() * (1 - amount) + b.redF() * amount, a.greenF() * (1 - amount) + b.greenF() * amount,
            a.blueF() * (1 - amount) + b.blueF() * amount, a.alphaF() * (1 - amount) + b.alphaF() * amount);
    };
    for (int y = 0; y < original.height(); ++y)
        for (int x = 0; x < original.width(); ++x) {
            const qreal coverage = maskSample(localMask, x, y);
            if (coverage <= 0)
                continue;
            const QColor target = original.pixelColor(x, y);
            if (contentMove) {
                repaired.setPixelColor(x, y, mix(target, repaired.pixelColor(x, y), coverage));
                QColor moving = target;
                moving.setAlphaF(moving.alphaF() * coverage);
                cutout.setPixelColor(x, y, moving);
            } else {
                const QPoint donor = QPoint(x, y) + delta;
                if (!original.rect().contains(donor))
                    continue;
                QColor color = original.pixelColor(donor);
                if (weight > 0) {
                    color.setRedF(
                        std::clamp(color.redF() + (targetRed - donorRed) / weight * adaptation, 0., 1.));
                    color.setGreenF(std::clamp(
                        color.greenF() + (targetGreen - donorGreen) / weight * adaptation, 0., 1.));
                    color.setBlueF(
                        std::clamp(color.blueF() + (targetBlue - donorBlue) / weight * adaptation, 0., 1.));
                }
                repaired.setPixelColor(x, y, mix(target, color, coverage));
            }
        }
    m_document->mutate(contentMove ? "Content-Aware Move" : "Patch", [&] {
        Layer *layer = m_document->activeLayer();
        if (contentMove) {
            const QRect destination =
                m_document->selectionBounds().translated(delta).intersected(documentRect(m_document));
            const QRect extent = original.rect().united(destination.translated(-offset));
            QImage result(extent.size(), original.format());
            result.fill(Qt::transparent);
            QPainter painter(&result);
            painter.drawImage(-extent.topLeft(), repaired);
            painter.drawImage(delta - extent.topLeft(), cutout);
            painter.end();
            layer->pixels.setImage(result);
            layer->offset += extent.topLeft();
            if (!extent.topLeft().isNull()) {
                if (!layer->mask.isNull()) {
                    layer->maskOffset -= extent.topLeft();
                    layer->maskLinked = false;
                }
                QTransform translation;
                translation.translate(-extent.left(), -extent.top());
                layer->vectorMask = translation.map(layer->vectorMask);
            }
            const QImage movedMask = paintCoverage(
                m_document->state.size, m_document->state.bitDepth,
                [&](QPainter &selectionPainter) { selectionPainter.drawImage(delta, m_transportMask); });
            m_document->setSelection(movedMask);
        } else
            layer->pixels.setImage(repaired);
        if (layer->kind != LayerKind::SmartObject)
            layer->kind = LayerKind::Pixel;
        m_document->touch();
    });
}

void CanvasView::mouseReleaseEvent(QMouseEvent *event) {
    if (m_hudBrush) {
        m_hudBrush = false;
        return;
    }
    if (m_panning) {
        m_panning = false;
        setCursor(m_tool == "Hand" ? Qt::OpenHandCursor : Qt::CrossCursor);
        return;
    }
    if (m_rotating) {
        m_rotating = false;
        return;
    }
    if (m_guideDrag) {
        const int direction = m_guideDrag;
        const qreal position = m_guidePosition;
        m_guideDrag = 0;
        if (event->position().x() > 20 && event->position().y() > 20)
            m_document->mutate("New guide",
                               [&] { m_document->state.guides.append({direction == 1, position}); });
        update();
        return;
    }
    if (event->button() == Qt::LeftButton && m_straightening) {
        const QPointF end = toDocument(event->position());
        if (QLineF(m_start, end).length() > 2) {
            const qreal angle =
                std::atan2(end.y() - m_start.y(), end.x() - m_start.x()) * 180 / std::numbers::pi;
            m_cropSettings.angle = -std::remainder(angle, 90.0);
            setCropPreviewRect(
                largestInscribedRotatedRectangle(m_document->state.size, m_cropSettings.angle));
            emit cropSettingsChanged();
        }
        m_straightening = false;
        m_straightenArmed = false;
        m_dragging = false;
        update();
        return;
    }
    if (event->button() == Qt::LeftButton && m_transformActive && m_transformHandle >= 0) {
        updateTransformDrag(toDocument(event->position()), event->modifiers());
        m_transformHandle = -1;
        m_dragging = false;
        update();
        return;
    }
    if (event->button() == Qt::LeftButton &&
        (m_cropHandle >= 0 || m_cropRotating || m_perspectiveHandle >= 0)) {
        if (m_cropHandle >= 0)
            updateCropDrag(toDocument(event->position()), event->modifiers());
        m_cropHandle = -1;
        m_perspectiveHandle = -1;
        m_cropRotating = false;
        m_dragging = false;
        update();
        return;
    }
    if (event->button() == Qt::LeftButton && m_transportSelecting) {
        const QImage mask =
            paintCoverage(m_document->state.size, m_document->state.bitDepth, [&](QPainter &painter) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(Qt::white);
                if (m_lasso.size() > 2)
                    painter.drawPolygon(m_lasso);
                else
                    painter.drawRect(QRectF(m_start, toDocument(event->position())).normalized());
            });
        m_document->setSelection(mask, m_selectionOperation);
        m_transportSelecting = false;
        m_dragging = false;
        m_lasso.clear();
        update();
        return;
    }
    if (event->button() == Qt::LeftButton && m_transporting) {
        finishTransport();
        m_transporting = false;
        m_dragging = false;
        m_transportPreview = {};
        m_transportMask = {};
        update();
        return;
    }
    if (!m_dragging || event->button() != Qt::LeftButton)
        return;
    if (named(m_tool, {"Polygonal Lasso", "Magnetic Lasso"}))
        return;
    const QPointF end = toDocument(event->position());
    if (m_pathElement >= 0) {
        m_document->endTransaction();
        m_pathElement = -1;
    } else if (m_moving) {
        if (updateMove(end, event->modifiers()))
            m_document->endTransaction();
        m_moving = false;
        m_moveLayerIds.clear();
    } else if (selectionTool(m_tool))
        finishSelection();
    else if (paintTool(m_tool)) {
        QPointF brushEnd = end;
        if (event->modifiers().testFlag(Qt::ShiftModifier)) {
            const QPointF delta = end - m_start;
            brushEnd = std::abs(delta.x()) >= std::abs(delta.y()) ? QPointF(end.x(), m_start.y())
                                                                  : QPointF(m_start.x(), end.y());
        }
        for (const BrushDab &sample : m_brushDynamics.finish({brushEnd, m_pressure, m_tabletTilt}))
            dab(sample);
        m_last = brushEnd;
        if (!m_healMask.isNull()) {
            Layer *layer = m_document->activeLayer();
            if (layer) {
                QImage localMask(layer->pixels.size, QImage::Format_Grayscale8);
                localMask.fill(0);
                QPainter painter(&localMask);
                painter.drawImage(-effectiveOffset(m_document, *layer), m_healMask);
                painter.end();
                const QImage source = layer->pixels.image();
                const QImage synthesized = synthesizePatches(source, localMask);
                QImage patch = synthesized;
                for (int y = 0; y < patch.height(); ++y) {
                    auto *row = reinterpret_cast<QRgb *>(patch.scanLine(y));
                    for (int x = 0; x < patch.width(); ++x) {
                        const int alpha = qGray(localMask.pixel(x, y));
                        if (patch.depth() > 32) {
                            QColor pixel = patch.pixelColor(x, y);
                            pixel.setAlphaF(pixel.alphaF() * alpha / 255.0);
                            patch.setPixelColor(x, y, pixel);
                        } else
                            row[x] = qRgba(qRed(row[x]), qGreen(row[x]), qBlue(row[x]),
                                           qAlpha(row[x]) * alpha / 255);
                    }
                }
                applyImage(patch, effectiveOffset(m_document, *layer).toPoint());
            }
        }
        m_document->endTransaction();
        m_healMask = {};
        m_lastStrokePoint = m_last;
        m_hasLastStrokePoint = true;
    } else if (named(m_tool, {"Move", "Path Selection", "Direct Selection"}))
        m_document->endTransaction();
    else if (m_tool == "Gradient")
        applyGradient();
    else if (shapeTool(m_tool) && QLineF(m_start, end).length() > 1) {
        m_document->mutate(m_tool, [&] {
            m_document->addLayer(m_tool, m_tool == "Artboard" ? LayerKind::Artboard : LayerKind::Shape);
            Layer *layer = m_document->activeLayer();
            QPainterPath path;
            if (m_tool == "Ellipse")
                path.addEllipse(m_dragRect);
            else if (m_tool == "Triangle" || m_tool == "Polygon")
                path = polygonShape(m_dragRect, m_tool == "Triangle" ? 3 : 6);
            else if (m_tool == "Line") {
                path.moveTo(m_start);
                path.lineTo(end);
            } else
                path.addRect(m_dragRect);
            layer->shape = path;
            layer->color = m_tool == "Line" || m_tool == "Frame" ? QColor(Qt::transparent) : m_foreground;
            layer->stroke = m_tool == "Line" || m_tool == "Frame" ? m_foreground : QColor(Qt::transparent);
            layer->strokeWidth = std::max(1, m_brushSize / 10);
        });
    } else if (m_tool == "Ruler") {
        m_document->state.metadata["rulerDistance"] = QLineF(m_start, end).length();
        m_document->touch();
    } else if (m_tool == "Slice" || m_tool == "Slice Select")
        m_document->mutate("Slice", [&] {
            QJsonArray slices = m_document->state.metadata.value("slices").toArray();
            slices.append(QJsonObject{{"x", m_dragRect.x()},
                                      {"y", m_dragRect.y()},
                                      {"width", m_dragRect.width()},
                                      {"height", m_dragRect.height()}});
            m_document->state.metadata["slices"] = slices;
        });
    else
        m_document->endTransaction();
    m_dragging = false;
    m_dragRect = {};
    m_pressure = 1;
    update();
}
void CanvasView::mouseDoubleClickEvent(QMouseEvent *event) {
    if (m_transformActive) {
        commitTransform();
        event->accept();
    } else if (named(m_tool, {"Crop", "Perspective Crop"}) && hasCropPreview()) {
        commitCrop();
        event->accept();
    } else if (named(m_tool, {"Polygonal Lasso", "Magnetic Lasso"}) && m_dragging) {
        m_lasso.append(toDocument(event->position()));
        finishSelection();
        m_dragging = false;
        update();
    } else if (named(m_tool, {"Pen", "Curvature Pen", "Freeform Pen"}) && !m_penPath.isEmpty()) {
        m_document->mutate("Close path", [&] {
            m_penPath.closeSubpath();
            const int index = m_document->indexForId(m_penLayerId);
            if (index >= 0) {
                m_document->state.layers[index].shape = m_penPath;
                m_document->state.layers[index].color = m_foreground;
            }
        });
        m_penLayerId = 0;
        m_penPath = {};
        m_perspectivePoints.clear();
        m_pathElement = -1;
        update();
    } else if (m_tool == "Hand")
        fitToView();
    else if (m_tool == "Zoom")
        resetView();
}

void CanvasView::wheelEvent(QWheelEvent *event) {
    const QPointF anchor = toDocument(event->position());
    const qreal delta = event->angleDelta().y() / 120.0;
    if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        m_pan += QPointF(event->angleDelta().y() / 2.0, event->angleDelta().x() / 2.0);
        update();
    } else {
        setZoom(m_zoom * std::pow(1.15, delta));
        m_pan += event->position() - fromDocument(anchor);
        update();
    }
    event->accept();
}
void CanvasView::keyPressEvent(QKeyEvent *event) {
    const bool nativeBinding =
        !property("externalShortcuts").toBool() || property("shortcutForwarding").toBool();
    if (event->key() == Qt::Key_Space && nativeBinding && !event->isAutoRepeat()) {
        m_space = true;
        setCursor(Qt::OpenHandCursor);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && nativeBinding) {
        cancelInteraction();
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && nativeBinding &&
        m_transformActive) {
        commitTransform();
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && nativeBinding &&
        hasCropPreview()) {
        commitCrop();
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && nativeBinding && m_dragging &&
        named(m_tool, {"Polygonal Lasso", "Magnetic Lasso"})) {
        finishSelection();
        m_dragging = false;
        update();
        event->accept();
        return;
    }
    if (nativeBinding && named(m_tool, {"Move", "Artboard", "Path Selection", "Direct Selection"}) &&
        event->key() >= Qt::Key_Left && event->key() <= Qt::Key_Down &&
        !event->modifiers().testFlag(Qt::ControlModifier) && !event->modifiers().testFlag(Qt::AltModifier)) {
        const qreal step = event->modifiers().testFlag(Qt::ShiftModifier) ? 10 : 1;
        const QPointF delta(event->key() == Qt::Key_Left    ? -step
                            : event->key() == Qt::Key_Right ? step
                                                            : 0,
                            event->key() == Qt::Key_Up     ? -step
                            : event->key() == Qt::Key_Down ? step
                                                           : 0);
        nudgeSelectedLayers(delta);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_BracketLeft && nativeBinding) {
        if (event->modifiers().testFlag(Qt::ShiftModifier))
            setBrushHardness(m_hardness - .25);
        else
            setBrushSize(int(m_brushSize / 1.15));
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_BracketRight && nativeBinding) {
        if (event->modifiers().testFlag(Qt::ShiftModifier))
            setBrushHardness(m_hardness + .25);
        else
            setBrushSize(int(m_brushSize * 1.15) + 1);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_X && nativeBinding && event->modifiers() == Qt::NoModifier) {
        std::swap(m_foreground, m_background);
        emit colorPicked(m_foreground);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_D && nativeBinding && event->modifiers() == Qt::NoModifier) {
        m_foreground = Qt::black;
        m_background = Qt::white;
        emit colorPicked(m_foreground);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Q && nativeBinding && event->modifiers() == Qt::NoModifier) {
        setQuickMask(!m_quickMask);
        event->accept();
        return;
    }
    if (nativeBinding && event->key() >= Qt::Key_0 && event->key() <= Qt::Key_9 && paintTool(m_tool) &&
        !event->modifiers().testFlag(Qt::ControlModifier) && !event->modifiers().testFlag(Qt::AltModifier)) {
        setBrushOpacityByNumber(event->key() - Qt::Key_0, event->modifiers().testFlag(Qt::ShiftModifier));
        event->accept();
        return;
    }
    if (nativeBinding && event->key() == Qt::Key_O && m_cropActive) {
        cycleCropOverlay();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}
void CanvasView::keyReleaseEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat() &&
        (!property("externalShortcuts").toBool() || property("shortcutForwarding").toBool())) {
        m_space = false;
        setCursor(m_tool == "Hand" ? Qt::OpenHandCursor : Qt::CrossCursor);
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}
void CanvasView::tabletEvent(QTabletEvent *event) {
    m_pressure = event->pressure();
    m_tabletTilt = {qreal(event->xTilt()), qreal(event->yTilt())};
    if (event->type() == QEvent::TabletPress) {
        QMouseEvent mouse(QEvent::MouseButtonPress, event->position(), event->globalPosition(),
                          Qt::LeftButton, Qt::LeftButton, event->modifiers());
        mousePressEvent(&mouse);
    } else if (event->type() == QEvent::TabletMove) {
        QMouseEvent mouse(QEvent::MouseMove, event->position(), event->globalPosition(), Qt::NoButton,
                          Qt::LeftButton, event->modifiers());
        mouseMoveEvent(&mouse);
    } else if (event->type() == QEvent::TabletRelease) {
        QMouseEvent mouse(QEvent::MouseButtonRelease, event->position(), event->globalPosition(),
                          Qt::LeftButton, Qt::NoButton, event->modifiers());
        mouseReleaseEvent(&mouse);
        m_pressure = 1;
        m_tabletTilt = {};
    }
    event->accept();
}

void CanvasView::fillSelection(QColor color) {
    if (!m_document->activeLayer())
        return;
    m_document->mutate("Fill", [&] {
        const QRect bounds =
            m_document->hasSelection() ? m_document->selectionBounds() : documentRect(m_document);
        if (bounds.isEmpty())
            return;
        QImage image(bounds.size(), paintingFormat(m_document));
        color.setAlphaF(color.alphaF() * m_opacity);
        image.fill(color);
        applyImage(image, bounds.topLeft());
    });
}
void CanvasView::strokeSelection(QColor color, int width) {
    if (!m_document->hasSelection())
        return;
    updateSelectionOutline();
    m_document->mutate("Stroke selection", [&] {
        const QRect bounds = m_document->selectionBounds()
                                 .adjusted(-width, -width, width, width)
                                 .intersected(documentRect(m_document));
        QImage image(bounds.size(), paintingFormat(m_document));
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.translate(-bounds.topLeft());
        painter.setPen(QPen(color, width));
        painter.drawPath(m_selectionOutline);
        painter.end();
        applyImage(image, bounds.topLeft());
    });
}
void CanvasView::applyGradient() {
    if (!m_document->activeLayer())
        return;
    m_document->mutate("Gradient", [&] {
        const QRect bounds =
            m_document->hasSelection() ? m_document->selectionBounds() : documentRect(m_document);
        if (bounds.isEmpty())
            return;
        QImage image(bounds.size(), paintingFormat(m_document));
        image.fill(Qt::transparent);
        QPainter painter(&image);
        QLinearGradient gradient(m_start - QPointF(bounds.topLeft()), m_last - QPointF(bounds.topLeft()));
        gradient.setColorAt(0, m_foreground);
        gradient.setColorAt(1, m_background);
        painter.setOpacity(m_opacity);
        painter.fillRect(image.rect(), gradient);
        painter.end();
        applyImage(image, bounds.topLeft());
    });
}
void CanvasView::bucket(QPoint point) {
    const QImage source = m_document->composite().convertToFormat(QImage::Format_ARGB32);
    if (!source.rect().contains(point))
        return;
    const QRgb target = source.pixel(point);
    const int tolerance = 32;
    QImage mask(source.size(), QImage::Format_Grayscale8);
    mask.fill(0);
    QVector<QPoint> queue;
    queue.append(point);
    mask.scanLine(point.y())[point.x()] = 255;
    auto enqueue = [&](int x, int y) {
        if (mask.constScanLine(y)[x])
            return;
        const QRgb sample = source.pixel(x, y);
        if (std::abs(qRed(sample) - qRed(target)) + std::abs(qGreen(sample) - qGreen(target)) +
                    std::abs(qBlue(sample) - qBlue(target)) >
                tolerance * 3 ||
            std::abs(qAlpha(sample) - qAlpha(target)) > tolerance)
            return;
        mask.scanLine(y)[x] = 255;
        queue.append(QPoint(x, y));
    };
    int left = point.x(), top = point.y(), right = point.x(), bottom = point.y();
    for (qsizetype i = 0; i < queue.size(); ++i) {
        const QPoint p = queue[i];
        left = std::min(left, p.x());
        top = std::min(top, p.y());
        right = std::max(right, p.x());
        bottom = std::max(bottom, p.y());
        if (p.x() > 0)
            enqueue(p.x() - 1, p.y());
        if (p.x() + 1 < source.width())
            enqueue(p.x() + 1, p.y());
        if (p.y() > 0)
            enqueue(p.x(), p.y() - 1);
        if (p.y() + 1 < source.height())
            enqueue(p.x(), p.y() + 1);
    }
    if (m_tool == "Magic Wand") {
        m_document->setSelection(mask, m_selectionOperation);
        return;
    }
    const QRect bounds(QPoint(left, top), QPoint(right, bottom));
    QImage image(bounds.size(), paintingFormat(m_document));
    image.fill(Qt::transparent);
    for (const QPoint p : queue) {
        QColor color = m_foreground;
        color.setAlphaF(m_opacity);
        image.setPixelColor(p - bounds.topLeft(), color);
    }
    applyImage(image, bounds.topLeft(), m_tool == "Magic Eraser");
}
void CanvasView::selectSubject() {
    const QImage mask = selectSubjectLocally(m_document->composite());
    if (!mask.isNull())
        m_document->setSelection(mask);
}
QRectF CanvasView::activeLayerBounds() const {
    const Layer *active = m_document->activeLayer();
    if (!active)
        return {};
    if (active->kind != LayerKind::Group)
        return layerBounds(m_document, *active);
    QSet<quint64> descendants{active->id};
    bool found = true;
    while (found) {
        found = false;
        for (const Layer &layer : m_document->state.layers)
            if (descendants.contains(layer.parentId) && !descendants.contains(layer.id)) {
                descendants.insert(layer.id);
                found = true;
            }
    }
    QRectF bounds;
    for (const Layer &layer : m_document->state.layers)
        if (descendants.contains(layer.id) && layer.kind != LayerKind::Group)
            bounds = bounds.united(layerBounds(m_document, layer));
    return bounds;
}
void CanvasView::perspectiveCrop() {
    if (m_perspectivePoints.size() != 4)
        return;
    const QPolygonF quad = m_perspectivePoints;
    m_perspectivePoints.clear();
    const int width =
        m_cropSettings.outputSize.isEmpty()
            ? std::max(1, qRound((QLineF(quad[0], quad[1]).length() + QLineF(quad[3], quad[2]).length()) / 2))
            : m_cropSettings.outputSize.width();
    const int height =
        m_cropSettings.outputSize.isEmpty()
            ? std::max(1, qRound((QLineF(quad[0], quad[3]).length() + QLineF(quad[1], quad[2]).length()) / 2))
            : m_cropSettings.outputSize.height();
    QTransform transform;
    if (!QTransform::quadToQuad(
            quad, QPolygonF{{0, 0}, {qreal(width), 0}, {qreal(width), qreal(height)}, {0, qreal(height)}},
            transform))
        return;
    m_document->mutate("Perspective Crop", [&] {
        QVector<QPointF> offsets;
        for (const Layer &layer : m_document->state.layers)
            offsets.append(effectiveOffset(m_document, layer));
        for (int i = 0; i < m_document->state.layers.size(); ++i) {
            Layer &layer = m_document->state.layers[i];
            const QPointF offset = offsets[i];
            if (layer.kind == LayerKind::Shape || layer.kind == LayerKind::Artboard) {
                QTransform position;
                position.translate(offset.x(), offset.y());
                layer.shape = transform.map(position.map(layer.shape));
            } else if (layer.kind != LayerKind::Group && layer.kind != LayerKind::Adjustment) {
                QImage output(width, height, paintingFormat(m_document));
                output.fill(Qt::transparent);
                QPainter painter(&output);
                painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                painter.setTransform(transform);
                painter.drawImage(offset, m_document->layerImage(layer));
                painter.end();
                layer.pixels.setImage(output);
                if (layer.kind == LayerKind::Text)
                    m_document->importReport.append("Perspective crop rasterized a text layer: " +
                                                    layer.name);
                layer.kind = LayerKind::Pixel;
            }
            if (!layer.mask.isNull()) {
                const QImage mask =
                    paintCoverage({width, height}, layer.mask.depth(), [&](QPainter &painter) {
                        painter.setTransform(transform);
                        painter.drawImage(offset + (layer.maskLinked ? QPointF() : layer.maskOffset),
                                          layer.mask);
                    });
                layer.mask = mask;
                layer.maskOffset = {};
            }
            if (!layer.vectorMask.isEmpty()) {
                QTransform position;
                const QPointF vectorOffset =
                    offset + (layer.vectorMaskLinked ? QPointF() : layer.vectorMaskOffset);
                position.translate(vectorOffset.x(), vectorOffset.y());
                layer.vectorMask = transform.map(position.map(layer.vectorMask));
                layer.vectorMaskOffset = {};
            }
            layer.offset = {};
        }
        m_document->state.size = {width, height};
        if (m_cropSettings.resolution > 0)
            m_document->state.resolution = m_cropSettings.resolution;
        m_document->state.selection = {};
        m_document->state.guides.clear();
        QJsonArray points;
        for (const QPointF point : quad)
            points.append(QJsonArray{point.x(), point.y()});
        m_document->state.metadata["perspectiveCropCorners"] = points;
        m_document->touch();
    });
    fitToView();
}
void CanvasView::removeBackground() {
    const Layer *active = m_document->activeLayer();
    if (!active || active->locked)
        return;
    const QImage selection = selectSubjectLocally(m_document->composite());
    if (selection.isNull())
        return;
    m_document->mutate("Remove background", [&] {
        Layer *layer = m_document->activeLayer();
        const QSize size = !layer->mask.isNull()          ? layer->mask.size()
                           : layer->pixels.size.isEmpty() ? m_document->state.size
                                                          : layer->pixels.size;
        QImage mask = paintCoverage(size, m_document->state.bitDepth, [&](QPainter &painter) {
            painter.drawImage(-effectiveOffset(m_document, *layer) -
                                  (layer->maskLinked ? QPointF() : layer->maskOffset),
                              selection);
        });
        if (!layer->mask.isNull()) {
            for (int y = 0; y < mask.height(); ++y)
                for (int x = 0; x < mask.width(); ++x)
                    setMaskSample(mask, x, y, maskSample(mask, x, y) * maskSample(layer->mask, x, y));
        }
        layer->mask = mask;
        layer->maskEnabled = true;
        m_document->touch();
    });
}
void CanvasView::contentAwareFill() {
    Layer *active = m_document->activeLayer();
    if (!active || active->locked || !m_document->hasSelection())
        return;
    m_document->mutate("Content-aware fill", [&] {
        Layer *layer = m_document->activeLayer();
        const QImage source = m_document->layerImage(*layer);
        const QImage localMask =
            paintCoverage(source.size(), m_document->state.bitDepth, [&](QPainter &painter) {
                painter.drawImage(-effectiveOffset(m_document, *layer), m_document->state.selection);
            });
        QImage result = synthesizePatches(source, localMask);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x) {
                const qreal coverage = maskSample(localMask, x, y);
                const QColor a = source.pixelColor(x, y), b = result.pixelColor(x, y);
                result.setPixelColor(x, y,
                                     QColor::fromRgbF(a.redF() * (1 - coverage) + b.redF() * coverage,
                                                      a.greenF() * (1 - coverage) + b.greenF() * coverage,
                                                      a.blueF() * (1 - coverage) + b.blueF() * coverage,
                                                      a.alphaF() * (1 - coverage) + b.alphaF() * coverage));
            }
        layer->pixels.setImage(result);
        layer->kind = LayerKind::Pixel;
        m_document->touch();
    });
}
void CanvasView::transformActive(qreal scale, qreal angle) {
    Layer *active = m_document->activeLayer();
    if (!active || active->locked)
        return;
    m_document->mutate("Transform", [&] {
        Layer *layer = m_document->activeLayer();
        QTransform transform;
        const QPointF center(m_document->state.size.width() / 2.0, m_document->state.size.height() / 2.0);
        transform.translate(center.x(), center.y());
        transform.rotate(angle);
        transform.scale(scale, scale);
        transform.translate(-center.x(), -center.y());
        if (layer->kind == LayerKind::Shape) {
            layer->shape = transform.map(layer->shape);
        } else {
            QImage image(m_document->state.size, paintingFormat(m_document));
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.setTransform(transform);
            painter.drawImage(QPoint(), m_document->layerImage(*layer));
            painter.end();
            layer->pixels.setImage(image);
            layer->kind = LayerKind::Pixel;
        }
        m_document->touch();
    });
}

QStringList toolNames() {
    return {"Move",
            "Artboard",
            "Rectangular Marquee",
            "Elliptical Marquee",
            "Single Row",
            "Single Column",
            "Lasso",
            "Polygonal Lasso",
            "Magnetic Lasso",
            "Object Selection",
            "Quick Selection",
            "Magic Wand",
            "Crop",
            "Perspective Crop",
            "Slice",
            "Slice Select",
            "Frame",
            "Eyedropper",
            "3D Material Eyedropper",
            "Color Sampler",
            "Ruler",
            "Note",
            "Count",
            "Spot Healing",
            "Healing Brush",
            "Patch",
            "Content-Aware Move",
            "Red Eye",
            "Brush",
            "Pencil",
            "Color Replacement",
            "Mixer Brush",
            "Clone Stamp",
            "Pattern Stamp",
            "History Brush",
            "Art History Brush",
            "Eraser",
            "Background Eraser",
            "Magic Eraser",
            "Gradient",
            "Paint Bucket",
            "Blur",
            "Sharpen",
            "Smudge",
            "Dodge",
            "Burn",
            "Sponge",
            "Pen",
            "Freeform Pen",
            "Curvature Pen",
            "Add Anchor",
            "Delete Anchor",
            "Convert Point",
            "Horizontal Type",
            "Vertical Type",
            "Horizontal Type Mask",
            "Vertical Type Mask",
            "Path Selection",
            "Direct Selection",
            "Rectangle",
            "Ellipse",
            "Triangle",
            "Polygon",
            "Line",
            "Custom Shape",
            "Hand",
            "Rotate View",
            "Zoom"};
}

namespace {
// Layer panel glyphs use a "layer:" prefix so they never collide with tool names below.
bool paintLayerGlyph(QPainter &painter, QPen pen, const QString &name, const QColor &color) {
    if (name == "layer:eye") {
        QPainterPath eye;
        eye.moveTo(2, 11);
        eye.cubicTo(5, 5.5, 17, 5.5, 20, 11);
        eye.cubicTo(17, 16.5, 5, 16.5, 2, 11);
        painter.drawPath(eye);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(11, 11), 2.4, 2.4);
    } else if (name == "layer:lock") {
        painter.drawRoundedRect(QRectF(5, 10, 12, 9), 1.5, 1.5);
        painter.drawArc(QRectF(7.5, 4, 7, 10), 0, 180 * 16);
        painter.drawLine(QPointF(7.5, 9), QPointF(7.5, 10));
        painter.drawLine(QPointF(14.5, 9), QPointF(14.5, 10));
    } else if (name == "layer:transparency") {
        painter.drawRect(4, 4, 14, 14);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                painter.drawRect(QRectF(4 + x * 7 + (y % 2 ? 3.5 : 0), 4 + y * 7, 3.5, 3.5));
        painter.drawRect(QRectF(7.5, 7.5, 3.5, 3.5));
        painter.drawRect(QRectF(14.5, 7.5, 3.5, 3.5));
        painter.drawRect(QRectF(4, 14.5, 3.5, 3.5));
        painter.drawRect(QRectF(11, 14.5, 3.5, 3.5));
    } else if (name == "layer:link") {
        painter.drawRoundedRect(QRectF(2.5, 8, 9, 6), 3, 3);
        painter.drawRoundedRect(QRectF(10.5, 8, 9, 6), 3, 3);
    } else if (name == "layer:fx") {
        QFont font = painter.font();
        font.setPixelSize(12);
        font.setItalic(true);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(QRectF(0, 0, 22, 22), Qt::AlignCenter, "fx");
    } else if (name == "layer:mask") {
        painter.drawRect(3, 5, 16, 12);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(11, 11), 3.5, 3.5);
    } else if (name == "layer:adjustment") {
        painter.drawEllipse(QPointF(11, 11), 7.5, 7.5);
        QPainterPath half;
        half.moveTo(11, 3.5);
        half.arcTo(QRectF(3.5, 3.5, 15, 15), 90, 180);
        half.closeSubpath();
        painter.setBrush(color);
        painter.drawPath(half);
    } else if (name == "layer:folder") {
        QPainterPath folder;
        folder.moveTo(3, 6);
        folder.lineTo(9, 6);
        folder.lineTo(11, 8);
        folder.lineTo(19, 8);
        folder.lineTo(19, 17);
        folder.lineTo(3, 17);
        folder.closeSubpath();
        painter.drawPath(folder);
    } else if (name == "layer:new") {
        painter.drawRect(4, 4, 14, 14);
        painter.drawLine(11, 7.5, 11, 14.5);
        painter.drawLine(7.5, 11, 14.5, 11);
    } else if (name == "layer:trash") {
        painter.drawLine(QPointF(4, 6), QPointF(18, 6));
        painter.drawLine(QPointF(9, 6), QPointF(9.5, 3.5));
        painter.drawLine(QPointF(9.5, 3.5), QPointF(12.5, 3.5));
        painter.drawLine(QPointF(12.5, 3.5), QPointF(13, 6));
        painter.drawPolyline(QPolygonF{{5.5, 6}, {6.5, 19}, {15.5, 19}, {16.5, 6}});
        painter.drawLine(QPointF(9, 9), QPointF(9.3, 16));
        painter.drawLine(QPointF(13, 9), QPointF(12.7, 16));
    } else
        return false;
    Q_UNUSED(pen);
    return true;
}
// Icons for the sixteen adjustments shown in the Adjustments panel ("adj:" + adjustment name).
bool paintAdjustmentGlyph(QPainter &painter, const QString &name, const QColor &color) {
    if (!name.startsWith("adj:"))
        return false;
    const QString kind = name.mid(4);
    const QRectF box(4, 4, 14, 14);
    if (kind == "Brightness/Contrast") {
        painter.drawEllipse(QPointF(11, 11), 4, 4);
        QPainterPath half;
        half.moveTo(11, 7);
        half.arcTo(QRectF(7, 7, 8, 8), 90, 180);
        half.closeSubpath();
        painter.fillPath(half, color);
        for (int i = 0; i < 8; ++i) {
            const qreal a = i * std::numbers::pi / 4;
            painter.drawLine(QPointF(11 + 6.5 * std::cos(a), 11 + 6.5 * std::sin(a)),
                             QPointF(11 + 8.5 * std::cos(a), 11 + 8.5 * std::sin(a)));
        }
    } else if (kind == "Levels") {
        painter.drawLine(QPointF(3, 18.5), QPointF(19, 18.5));
        const qreal heights[] = {3, 7, 11, 8, 12, 6, 3};
        for (int i = 0; i < 7; ++i)
            painter.drawLine(QPointF(5 + i * 2, 18.5), QPointF(5 + i * 2, 18.5 - heights[i]));
    } else if (kind == "Curves") {
        painter.drawRect(box);
        QPainterPath curve;
        curve.moveTo(4, 18);
        curve.cubicTo(10, 18, 12, 4, 18, 4);
        painter.drawPath(curve);
    } else if (kind == "Exposure") {
        painter.drawRect(box);
        painter.drawLine(QPointF(4, 18), QPointF(18, 4));
        painter.drawLine(QPointF(6.5, 8.5), QPointF(10.5, 8.5));
        painter.drawLine(QPointF(8.5, 6.5), QPointF(8.5, 10.5));
        painter.drawLine(QPointF(11.5, 14.5), QPointF(15.5, 14.5));
    } else if (kind == "Vibrance") {
        painter.drawPolygon(QPolygonF{{3.5, 5}, {18.5, 5}, {11, 18}});
        painter.drawLine(QPointF(7.5, 9), QPointF(14.5, 9));
    } else if (kind == "Hue/Saturation") {
        for (int i = 0; i < 3; ++i) {
            const qreal y = 6 + i * 5;
            painter.drawLine(QPointF(3, y), QPointF(19, y));
            painter.setBrush(color);
            painter.drawEllipse(QPointF(i == 1 ? 14 : i == 0 ? 8 : 11, y), 1.8, 1.8);
            painter.setBrush(Qt::NoBrush);
        }
    } else if (kind == "Color Balance") {
        painter.drawLine(QPointF(11, 4), QPointF(11, 18));
        painter.drawLine(QPointF(7, 18), QPointF(15, 18));
        painter.drawLine(QPointF(4, 7), QPointF(18, 7));
        painter.drawArc(QRectF(2, 8, 6, 6), 180 * 16, 180 * 16);
        painter.drawArc(QRectF(14, 8, 6, 6), 180 * 16, 180 * 16);
    } else if (kind == "Black & White") {
        painter.drawRect(box);
        painter.setBrush(color);
        painter.drawPolygon(QPolygonF{{4, 18}, {18, 4}, {18, 18}});
    } else if (kind == "Photo Filter") {
        painter.drawRoundedRect(QRectF(3, 7, 16, 11), 2, 2);
        painter.drawPolyline(QPolygonF{{8, 7}, {9.5, 4.5}, {12.5, 4.5}, {14, 7}});
        painter.drawEllipse(QPointF(11, 12.5), 3, 3);
    } else if (kind == "Channel Mixer") {
        painter.drawEllipse(QPointF(11, 8), 4.5, 4.5);
        painter.drawEllipse(QPointF(8, 13.5), 4.5, 4.5);
        painter.drawEllipse(QPointF(14, 13.5), 4.5, 4.5);
    } else if (kind == "Color Lookup") {
        painter.drawRect(box);
        for (const qreal at : {8.67, 13.33}) {
            painter.drawLine(QPointF(at, 4), QPointF(at, 18));
            painter.drawLine(QPointF(4, at), QPointF(18, at));
        }
    } else if (kind == "Invert") {
        painter.drawRect(box);
        painter.fillRect(QRectF(4, 4, 7, 14), color);
        painter.drawEllipse(QPointF(11, 11), 3.5, 3.5);
    } else if (kind == "Posterize") {
        painter.drawPolyline(
            QPolygonF{{3, 19}, {3, 15}, {7, 15}, {7, 11}, {11, 11}, {11, 7}, {15, 7}, {15, 3}, {19, 3}});
    } else if (kind == "Threshold") {
        painter.drawRect(box);
        painter.setBrush(color);
        painter.drawPolygon(QPolygonF{{4, 18}, {4, 13}, {8, 9}, {11, 13}, {14, 8}, {18, 12}, {18, 18}});
    } else if (kind == "Gradient Map") {
        QLinearGradient gradient(4, 0, 18, 0);
        QColor clear = color;
        clear.setAlphaF(0);
        gradient.setColorAt(0, clear);
        gradient.setColorAt(1, color);
        painter.fillRect(box, gradient);
        painter.drawRect(box);
    } else if (kind == "Selective Color") {
        painter.drawEllipse(QPointF(11, 11), 7.5, 7.5);
        for (int i = 0; i < 3; ++i) {
            const qreal a = i * std::numbers::pi / 3;
            painter.drawLine(QPointF(11 - 7.5 * std::cos(a), 11 - 7.5 * std::sin(a)),
                             QPointF(11 + 7.5 * std::cos(a), 11 + 7.5 * std::sin(a)));
        }
    } else {
        painter.drawRect(box);
    }
    return true;
}
// Tool glyphs are drawn on a 24-unit grid with 1.5-unit strokes and filled accents, in the spirit of
// Photoshop's toolbar: one distinct silhouette per tool, variants marked with a small badge.
namespace glyph {
QPainterPath cursorArrow(QPointF at, qreal scale = 1.0) {
    QPainterPath p;
    const QPolygonF shape{{0, 0}, {0, 15}, {3.6, 11.6}, {6.2, 17.4}, {8.6, 16.3}, {6.0, 10.6}, {11, 10.6}};
    QTransform t;
    t.translate(at.x(), at.y());
    t.scale(scale, scale);
    p.addPolygon(t.map(shape));
    p.closeSubpath();
    return p;
}
QPainterPath brush() {
    // Vertical brush pointing down, drawn around the origin; callers rotate it.
    QPainterPath p;
    p.addRoundedRect(QRectF(-1.5, -11, 3, 8.5), 1.5, 1.5);
    return p;
}
QPainterPath brushTip() {
    QPainterPath p;
    p.addRect(QRectF(-2.6, -2.5, 5.2, 3));
    p.moveTo(-2.6, 0.5);
    p.lineTo(2.6, 0.5);
    p.cubicTo(2.6, 4.5, 1.2, 7.5, 0, 10.5);
    p.cubicTo(-1.2, 7.5, -2.6, 4.5, -2.6, 0.5);
    return p;
}
QPainterPath drop(QPointF center, qreal size) {
    // A teardrop whose round bottom has radius `size`, centred on `center`.
    QPainterPath p;
    const qreal x = center.x(), y = center.y();
    p.moveTo(x, y - size * 2.1);
    p.cubicTo(x + size * 0.4, y - size * 1.3, x + size, y - size * 0.7, x + size, y);
    p.arcTo(QRectF(x - size, y - size, size * 2, size * 2), 0, -180);
    p.cubicTo(x - size, y - size * 0.7, x - size * 0.4, y - size * 1.3, x, y - size * 2.1);
    return p;
}
QPainterPath star(QPointF c, qreal outer, qreal inner, int points = 4) {
    QPainterPath p;
    for (int i = 0; i < points * 2; ++i) {
        const qreal r = i % 2 ? inner : outer;
        const qreal a = -std::numbers::pi / 2 + i * std::numbers::pi / points;
        const QPointF pt(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
        i ? p.lineTo(pt) : p.moveTo(pt);
    }
    p.closeSubpath();
    return p;
}
} // namespace glyph

void paintToolGlyph(QPainter &painter, QPen pen, const QString &name, const QColor &color) {
    if (paintLayerGlyph(painter, pen, name, color))
        return;
    if (paintAdjustmentGlyph(painter, name, color))
        return;
    painter.save();
    painter.scale(22.0 / 24.0, 22.0 / 24.0);
    pen.setWidthF(1.5);
    painter.setPen(pen);
    QColor soft = color;
    soft.setAlphaF(color.alphaF() * 0.35);
    QPen dashed = pen;
    dashed.setCapStyle(Qt::FlatCap);
    dashed.setJoinStyle(Qt::MiterJoin);
    dashed.setDashPattern({1.6, 1.4});
    const auto fill = [&](const QPainterPath &path, const QColor &with) { painter.fillPath(path, with); };
    const auto solid = [&](const QPainterPath &path) {
        fill(path, color);
        painter.drawPath(path);
    };
    const auto rotated = [&](QPointF center, qreal degrees, const std::function<void()> &draw) {
        painter.save();
        painter.translate(center);
        painter.rotate(degrees);
        draw();
        painter.restore();
    };
    const auto drawBrush = [&](QPointF center, qreal scale) {
        rotated(center, 45, [&] {
            painter.scale(scale, scale);
            painter.drawPath(glyph::brush());
            fill(glyph::brushTip(), color);
        });
    };
    const auto drawPen = [&](QPointF center, qreal scale) {
        rotated(center, 45, [&] {
            painter.scale(scale, scale);
            QPainterPath nib;
            nib.moveTo(0, 10.5);
            nib.lineTo(-5, 0);
            nib.lineTo(-3, -5.5);
            nib.lineTo(3, -5.5);
            nib.lineTo(5, 0);
            nib.closeSubpath();
            painter.drawPath(nib);
            painter.drawLine(QPointF(0, 10.5), QPointF(0, 2.5));
            painter.setBrush(color);
            painter.drawEllipse(QPointF(0, 1.2), 1.2, 1.2);
            painter.setBrush(Qt::NoBrush);
            painter.drawLine(QPointF(-3.5, -8.5), QPointF(3.5, -8.5));
        });
    };
    const auto drawEraser = [&](QPointF center, qreal scale) {
        rotated(center, 45, [&] {
            painter.scale(scale, scale);
            painter.drawRoundedRect(QRectF(-4.5, -9.5, 9, 18), 1.5, 1.5);
            QPainterPath tip;
            tip.addRoundedRect(QRectF(-4.5, 1.5, 9, 7), 1.5, 1.5);
            fill(tip, color);
        });
    };
    const auto drawBandage = [&](QPointF center, qreal scale) {
        rotated(center, -45, [&] {
            painter.scale(scale, scale);
            painter.drawRoundedRect(QRectF(-10, -4.2, 20, 8.4), 4.2, 4.2);
            painter.drawRect(QRectF(-3.6, -4.2, 7.2, 8.4));
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            for (const QPointF dot :
                 {QPointF(-1.4, -1.4), QPointF(1.4, -1.4), QPointF(-1.4, 1.4), QPointF(1.4, 1.4)})
                painter.drawEllipse(dot, 0.75, 0.75);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(pen);
        });
    };
    const auto drawStamp = [&](bool pattern) {
        painter.drawEllipse(QPointF(12, 5.5), 3.2, 3.2);
        painter.drawLine(QPointF(10.5, 8.6), QPointF(10.5, 12));
        painter.drawLine(QPointF(13.5, 8.6), QPointF(13.5, 12));
        QPainterPath base;
        base.addRoundedRect(QRectF(4, 12, 16, 5), 1.5, 1.5);
        if (pattern) {
            painter.drawPath(base);
            for (int i = 0; i < 4; ++i)
                painter.fillRect(QRectF(5 + i * 3.75, 20, 2.2, 2.2), color);
        } else {
            solid(base);
            painter.drawLine(QPointF(5, 20.5), QPointF(19, 20.5));
        }
    };
    const auto drawEyedropper = [&](QPointF center, qreal scale) {
        rotated(center, 45, [&] {
            painter.scale(scale, scale);
            QPainterPath bulb;
            bulb.addRoundedRect(QRectF(-2.8, -11, 5.6, 6.5), 2.8, 2.8);
            fill(bulb, color);
            painter.drawLine(QPointF(-4.2, -4.2), QPointF(4.2, -4.2));
            painter.drawRect(QRectF(-1.8, -4.2, 3.6, 10));
            painter.drawLine(QPointF(0, 5.8), QPointF(0, 10));
        });
    };
    const auto drawLassoLoop = [&](const QRectF &loop) {
        painter.drawEllipse(loop);
        QPainterPath tail;
        const QPointF knot(loop.left() + loop.width() * 0.2, loop.bottom() - loop.height() * 0.08);
        tail.moveTo(knot);
        tail.cubicTo(knot + QPointF(-2.5, 3), knot + QPointF(1.5, 4.5), knot + QPointF(-0.5, 7.5));
        painter.drawPath(tail);
        painter.setBrush(color);
        painter.drawEllipse(knot, 1.3, 1.3);
        painter.setBrush(Qt::NoBrush);
    };
    const auto drawTextT = [&](const QRectF &r) {
        const qreal mid = r.center().x();
        painter.drawLine(QPointF(r.left(), r.top()), QPointF(r.right(), r.top()));
        painter.drawLine(QPointF(r.left(), r.top()), QPointF(r.left(), r.top() + 2.5));
        painter.drawLine(QPointF(r.right(), r.top()), QPointF(r.right(), r.top() + 2.5));
        painter.drawLine(QPointF(mid, r.top()), QPointF(mid, r.bottom()));
        painter.drawLine(QPointF(mid - 3, r.bottom()), QPointF(mid + 3, r.bottom()));
    };
    const auto badgePlus = [&](QPointF c, bool plus) {
        painter.drawLine(c + QPointF(-3, 0), c + QPointF(3, 0));
        if (plus)
            painter.drawLine(c + QPointF(0, -3), c + QPointF(0, 3));
    };
    const auto curvedArrow = [&](const QRectF &circle, int startDegrees, int spanDegrees) {
        painter.drawArc(circle, startDegrees * 16, spanDegrees * 16);
        const qreal end = (startDegrees + spanDegrees) * std::numbers::pi / 180;
        const QPointF tip(circle.center().x() + circle.width() / 2 * std::cos(end),
                          circle.center().y() - circle.height() / 2 * std::sin(end));
        const qreal turn = spanDegrees > 0 ? 1 : -1;
        const QPointF along(-std::sin(end) * turn, -std::cos(end) * turn);
        const QPointF across(along.y(), -along.x());
        QPainterPath head;
        head.moveTo(tip + along * 2.6);
        head.lineTo(tip - along * 0.8 + across * 2.4);
        head.lineTo(tip - along * 0.8 - across * 2.4);
        head.closeSubpath();
        fill(head, color);
    };

    if (name == "Move" || name == "Content-Aware Move") {
        const bool aware = name == "Content-Aware Move";
        const QPointF c = aware ? QPointF(15, 15) : QPointF(12, 12);
        const qreal r = aware ? 6.5 : 9.5, head = aware ? 2.2 : 3;
        if (aware) {
            painter.save();
            painter.setPen(dashed);
            painter.drawRect(QRectF(2.5, 2.5, 11, 11));
            painter.restore();
        }
        painter.drawLine(c + QPointF(0, -r + head), c + QPointF(0, r - head));
        painter.drawLine(c + QPointF(-r + head, 0), c + QPointF(r - head, 0));
        for (int i = 0; i < 4; ++i)
            rotated(c, i * 90, [&] {
                QPainterPath tip;
                tip.moveTo(0, -r);
                tip.lineTo(-head, -r + head * 1.25);
                tip.lineTo(head, -r + head * 1.25);
                tip.closeSubpath();
                solid(tip);
            });
    } else if (name == "Artboard") {
        painter.drawRect(QRectF(7, 7, 10, 10));
        for (const QPointF corner : {QPointF(7, 7), QPointF(17, 7), QPointF(7, 17), QPointF(17, 17)}) {
            const qreal dx = corner.x() < 12 ? -4 : 4, dy = corner.y() < 12 ? -4 : 4;
            painter.drawLine(corner, corner + QPointF(dx, 0));
            painter.drawLine(corner, corner + QPointF(0, dy));
        }
    } else if (name == "Rectangular Marquee") {
        painter.setPen(dashed);
        painter.drawRect(QRectF(3.75, 3.75, 16.5, 16.5));
    } else if (name == "Elliptical Marquee") {
        painter.setPen(dashed);
        painter.drawEllipse(QRectF(3, 5, 18, 14));
    } else if (name == "Single Row" || name == "Single Column") {
        const bool row = name == "Single Row";
        painter.setPen(dashed);
        painter.drawRect(row ? QRectF(2.75, 9.75, 18.5, 4.5) : QRectF(9.75, 2.75, 4.5, 18.5));
        painter.setPen(pen);
        painter.drawLine(row ? QLineF(5, 12, 19, 12) : QLineF(12, 5, 12, 19));
    } else if (name == "Lasso") {
        drawLassoLoop(QRectF(4, 3, 17, 11));
    } else if (name == "Polygonal Lasso") {
        painter.drawPolygon(QPolygonF{{5, 13}, {7, 3.5}, {15, 3}, {21, 8.5}, {16, 14}});
        QPainterPath tail;
        tail.moveTo(5, 13);
        tail.cubicTo(2.5, 16, 6.5, 17.5, 4.5, 21);
        painter.drawPath(tail);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(5, 13), 1.3, 1.3);
        painter.setBrush(Qt::NoBrush);
    } else if (name == "Magnetic Lasso") {
        drawLassoLoop(QRectF(2.5, 2.5, 14, 9.5));
        QPainterPath magnet;
        magnet.moveTo(14, 13);
        magnet.lineTo(14, 17.5);
        magnet.arcTo(QRectF(14, 14.5, 7, 7), 180, 180);
        magnet.lineTo(21, 13);
        painter.drawPath(magnet);
        painter.fillRect(QRectF(13.25, 12.5, 1.5, 2.5), color);
        painter.fillRect(QRectF(20.25, 12.5, 1.5, 2.5), color);
    } else if (name == "Object Selection") {
        painter.setPen(dashed);
        painter.drawPolyline(QPolygonF{{12, 3.75}, {3.75, 3.75}, {3.75, 16}, {9, 16}});
        painter.drawPolyline(QPolygonF{{16, 3.75}, {20.25, 3.75}, {20.25, 9}});
        painter.setPen(pen);
        solid(glyph::cursorArrow({11, 8.5}, 0.82));
    } else if (name == "Quick Selection") {
        painter.save();
        painter.setPen(dashed);
        painter.drawEllipse(QRectF(2.5, 2.5, 11, 11));
        painter.restore();
        drawBrush({15.5, 15.5}, 0.62);
    } else if (name == "Magic Wand") {
        QPen thick = pen;
        thick.setWidthF(2.4);
        painter.setPen(thick);
        painter.drawLine(QPointF(4, 20), QPointF(13.5, 10.5));
        painter.setPen(pen);
        solid(glyph::star({16.5, 7.5}, 4.6, 1.4));
        solid(glyph::star({20.5, 14}, 2.2, 0.7));
        solid(glyph::star({9.5, 3.5}, 2.2, 0.7));
    } else if (name == "Crop" || name == "Perspective Crop") {
        painter.drawPolyline(QPolygonF{{6.5, 2}, {6.5, 17.5}, {22, 17.5}});
        painter.drawPolyline(QPolygonF{{2, 6.5}, {17.5, 6.5}, {17.5, 22}});
        if (name == "Perspective Crop") {
            painter.setPen(QPen(color, 1.0));
            painter.drawLine(QPointF(10, 6.5), QPointF(9.5, 17.5));
            painter.drawLine(QPointF(14, 6.5), QPointF(14.5, 17.5));
        }
    } else if (name == "Slice" || name == "Slice Select") {
        const bool select = name == "Slice Select";
        if (select) {
            painter.save();
            painter.translate(-1.5, -1.5);
            painter.scale(0.78, 0.78);
        }
        QPainterPath blade;
        blade.moveTo(3.5, 20.5);
        blade.lineTo(15.5, 8.5);
        blade.cubicTo(18, 6, 20, 4, 21, 3.5);
        blade.cubicTo(20.5, 6.5, 18.5, 9.5, 8.5, 20.5);
        blade.closeSubpath();
        painter.drawPath(blade);
        painter.drawLine(QPointF(3.5, 20.5), QPointF(2, 22));
        if (select) {
            painter.restore();
            solid(glyph::cursorArrow({13, 10}, 0.72));
        }
    } else if (name == "Frame") {
        painter.drawRect(QRectF(3.75, 3.75, 16.5, 16.5));
        painter.setPen(QPen(color, 1.0));
        painter.drawLine(QPointF(3.75, 3.75), QPointF(20.25, 20.25));
        painter.drawLine(QPointF(20.25, 3.75), QPointF(3.75, 20.25));
    } else if (name == "Eyedropper" || name == "3D Material Eyedropper") {
        drawEyedropper({12, 12}, 1.0);
    } else if (name == "Color Sampler") {
        drawEyedropper({14, 10}, 0.8);
        painter.drawEllipse(QPointF(6.5, 17.5), 3.2, 3.2);
        painter.drawLine(QPointF(6.5, 12.5), QPointF(6.5, 22.5));
        painter.drawLine(QPointF(1.5, 17.5), QPointF(11.5, 17.5));
    } else if (name == "Ruler") {
        rotated({12, 12}, -45, [&] {
            painter.drawRect(QRectF(-11, -3.5, 22, 7));
            for (int i = 0; i < 6; ++i) {
                const qreal x = -8 + i * 3.2;
                painter.drawLine(QPointF(x, -3.5), QPointF(x, i % 2 ? -1.5 : 0));
            }
        });
    } else if (name == "Note") {
        painter.drawPolygon(QPolygonF{{4, 3}, {20, 3}, {20, 15}, {14, 21}, {4, 21}});
        painter.drawPolyline(QPolygonF{{14, 21}, {14, 15}, {20, 15}});
        painter.drawLine(QPointF(7.5, 7.5), QPointF(16.5, 7.5));
        painter.drawLine(QPointF(7.5, 11), QPointF(16.5, 11));
    } else if (name == "Count") {
        solid(glyph::cursorArrow({3, 3}, 0.8));
        QFont font = painter.font();
        font.setPixelSize(11);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(QRectF(10, 10, 14, 13), Qt::AlignCenter, "12");
    } else if (name == "Spot Healing" || name == "Healing Brush") {
        drawBandage({12, 12}, name == "Spot Healing" ? 0.9 : 1.0);
        if (name == "Spot Healing") {
            painter.setPen(dashed);
            painter.drawArc(QRectF(1.5, 1.5, 21, 21), 100 * 16, 70 * 16);
            painter.drawArc(QRectF(1.5, 1.5, 21, 21), 280 * 16, 70 * 16);
        }
    } else if (name == "Patch") {
        painter.drawRoundedRect(QRectF(3.5, 3.5, 17, 17), 3, 3);
        painter.setPen(dashed);
        painter.drawRoundedRect(QRectF(7, 7, 10, 10), 1, 1);
    } else if (name == "Red Eye") {
        QPainterPath eye;
        eye.moveTo(2, 13);
        eye.cubicTo(6, 6.5, 18, 6.5, 22, 13);
        eye.cubicTo(18, 19.5, 6, 19.5, 2, 13);
        painter.drawPath(eye);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(12, 13), 3, 3);
        painter.setBrush(Qt::NoBrush);
        badgePlus({19, 4}, true);
    } else if (name == "Brush") {
        drawBrush({12, 12}, 1.0);
    } else if (name == "Pencil") {
        rotated({12, 12}, 45, [&] {
            painter.drawRect(QRectF(-2.4, -10.5, 4.8, 13));
            painter.drawLine(QPointF(-2.4, -7.5), QPointF(2.4, -7.5));
            QPainterPath cone;
            cone.moveTo(-2.4, 2.5);
            cone.lineTo(2.4, 2.5);
            cone.lineTo(0, 9.5);
            cone.closeSubpath();
            painter.drawPath(cone);
            QPainterPath lead;
            lead.moveTo(-0.9, 6.9);
            lead.lineTo(0.9, 6.9);
            lead.lineTo(0, 9.5);
            lead.closeSubpath();
            fill(lead, color);
        });
    } else if (name == "Color Replacement") {
        drawBrush({14, 10}, 0.78);
        painter.drawEllipse(QPointF(6.5, 17.5), 4, 4);
        QPainterPath half;
        half.moveTo(6.5, 13.5);
        half.arcTo(QRectF(2.5, 13.5, 8, 8), 90, 180);
        half.closeSubpath();
        fill(half, color);
    } else if (name == "Mixer Brush") {
        drawBrush({11, 11}, 0.82);
        solid(glyph::drop({18.5, 18.5}, 2.6));
    } else if (name == "Clone Stamp" || name == "Pattern Stamp") {
        drawStamp(name == "Pattern Stamp");
    } else if (name == "History Brush" || name == "Art History Brush") {
        drawBrush({15, 9}, 0.72);
        if (name == "History Brush") {
            curvedArrow(QRectF(2.5, 12.5, 9, 9), 60, 270);
        } else {
            QPainterPath curl;
            curl.moveTo(2.5, 20);
            curl.cubicTo(2.5, 13, 9.5, 13, 8.5, 17);
            curl.cubicTo(7.5, 21, 3.5, 18, 6.5, 14.5);
            painter.drawPath(curl);
        }
    } else if (name == "Eraser") {
        drawEraser({12, 11}, 1.0);
        painter.drawLine(QPointF(9, 21.5), QPointF(21, 21.5));
    } else if (name == "Background Eraser") {
        drawEraser({11, 10}, 0.85);
        painter.setPen(dashed);
        painter.drawEllipse(QPointF(18, 18), 3.5, 3.5);
    } else if (name == "Magic Eraser") {
        drawEraser({11, 12}, 0.85);
        solid(glyph::star({18.5, 5}, 3.6, 1.1));
    } else if (name == "Gradient") {
        QLinearGradient gradient(4, 0, 20, 0);
        gradient.setColorAt(0, color);
        QColor clear = color;
        clear.setAlphaF(0);
        gradient.setColorAt(1, clear);
        painter.fillRect(QRectF(3.75, 5.75, 16.5, 12.5), gradient);
        painter.drawRoundedRect(QRectF(3.75, 5.75, 16.5, 12.5), 1, 1);
    } else if (name == "Paint Bucket") {
        rotated({11, 11}, -40, [&] {
            QPainterPath bucket;
            bucket.moveTo(-6, -3);
            bucket.lineTo(6, -3);
            bucket.lineTo(5, 7);
            bucket.lineTo(-5, 7);
            bucket.closeSubpath();
            painter.drawPath(bucket);
            painter.drawArc(QRectF(-4.5, -9, 9, 10), 0, 180 * 16);
            QPainterPath paint;
            paint.addRect(QRectF(-6, -3, 12, 3.2));
            fill(paint, color);
        });
        solid(glyph::drop({19.5, 18.5}, 2.4));
    } else if (name == "Blur") {
        const auto water = glyph::drop({12, 15}, 6.2);
        painter.drawPath(water);
        painter.drawArc(QRectF(8.2, 11.5, 7.6, 7.6), 200 * 16, 70 * 16);
    } else if (name == "Sharpen") {
        QPainterPath cone;
        cone.moveTo(12, 2.5);
        cone.lineTo(18.5, 21);
        cone.lineTo(5.5, 21);
        cone.closeSubpath();
        painter.drawPath(cone);
        QPainterPath half;
        half.moveTo(12, 2.5);
        half.lineTo(12, 21);
        half.lineTo(5.5, 21);
        half.closeSubpath();
        fill(half, color);
    } else if (name == "Smudge") {
        rotated({12, 12}, -45, [&] {
            painter.drawRoundedRect(QRectF(-2, -11, 4, 10), 2, 2);
            QPainterPath hand;
            hand.moveTo(-2, -2);
            hand.lineTo(-2, 3);
            hand.cubicTo(-6, 2, -7, 5, -4.5, 7);
            hand.lineTo(-1, 10);
            hand.lineTo(6, 10);
            hand.cubicTo(8, 7, 8, 3, 7, -1);
            hand.cubicTo(6, -2, 4.5, -2, 4, -0.5);
            hand.cubicTo(3.5, -2.5, 2, -2.5, 2, -1);
            painter.drawPath(hand);
        });
    } else if (name == "Dodge") {
        painter.setBrush(soft);
        painter.drawEllipse(QPointF(9, 9), 6, 6);
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(QPointF(13.2, 13.2), QPointF(21, 21));
    } else if (name == "Burn") {
        QPainterPath flame;
        flame.moveTo(12, 2.5);
        flame.cubicTo(13, 7, 19, 9, 19, 15);
        flame.cubicTo(19, 19, 16, 21.5, 12, 21.5);
        flame.cubicTo(8, 21.5, 5, 19, 5, 15);
        flame.cubicTo(5, 11.5, 7.5, 10, 8.5, 7.5);
        flame.cubicTo(9.5, 10, 10.5, 10.5, 11, 10.5);
        flame.cubicTo(11, 8, 11, 5, 12, 2.5);
        painter.drawPath(flame);
        fill(glyph::drop({12, 17.2}, 2.8), color);
    } else if (name == "Sponge") {
        painter.drawRoundedRect(QRectF(3, 7, 18, 11.5), 3.5, 3.5);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (const QPointF hole : {QPointF(8, 11), QPointF(13, 10.5), QPointF(17, 13), QPointF(10, 15)})
            painter.drawEllipse(hole, 1.1, 1.1);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(pen);
    } else if (name == "Pen") {
        drawPen({12, 12}, 1.0);
    } else if (name == "Freeform Pen") {
        drawPen({14.5, 9.5}, 0.75);
        QPainterPath squiggle;
        squiggle.moveTo(2, 21);
        squiggle.cubicTo(4, 15, 7, 23, 9, 17);
        painter.drawPath(squiggle);
    } else if (name == "Curvature Pen") {
        drawPen({14.5, 9.5}, 0.75);
        painter.drawArc(QRectF(1.5, 14, 11, 12), 30 * 16, 120 * 16);
        painter.fillRect(QRectF(5.5, 13, 2.5, 2.5), color);
    } else if (name == "Add Anchor" || name == "Delete Anchor") {
        drawPen({13, 11}, 0.82);
        badgePlus({5, 19}, name == "Add Anchor");
    } else if (name == "Convert Point") {
        painter.drawPolyline(QPolygonF{{3.5, 20}, {12, 5}, {20.5, 20}});
        painter.fillRect(QRectF(10.25, 3.25, 3.5, 3.5), color);
    } else if (name == "Horizontal Type" || name == "Horizontal Type Mask") {
        if (name.endsWith("Mask"))
            painter.setPen(dashed);
        drawTextT(QRectF(5, 4, 14, 16));
    } else if (name == "Vertical Type" || name == "Vertical Type Mask") {
        if (name.endsWith("Mask"))
            painter.setPen(dashed);
        drawTextT(QRectF(3, 4, 11, 15));
        painter.setPen(pen);
        painter.drawLine(QPointF(19.5, 4), QPointF(19.5, 19));
        painter.drawPolyline(QPolygonF{{17, 16.5}, {19.5, 19.5}, {22, 16.5}});
    } else if (name == "Path Selection") {
        solid(glyph::cursorArrow({6, 3}, 1.05));
    } else if (name == "Direct Selection") {
        painter.drawPath(glyph::cursorArrow({6, 3}, 1.05));
    } else if (name == "Rectangle") {
        painter.drawRoundedRect(QRectF(3.75, 5.75, 16.5, 12.5), 1.5, 1.5);
    } else if (name == "Ellipse") {
        painter.drawEllipse(QRectF(3.75, 4.75, 16.5, 14.5));
    } else if (name == "Triangle") {
        painter.drawPolygon(QPolygonF{{12, 3.5}, {21, 19.5}, {3, 19.5}});
    } else if (name == "Polygon") {
        painter.drawPath(polygonShape(QRectF(3, 3, 18, 18), 6));
    } else if (name == "Line") {
        painter.drawLine(QPointF(5, 19), QPointF(19, 5));
        painter.fillRect(QRectF(3, 17, 4, 4), color);
        painter.fillRect(QRectF(17, 3, 4, 4), color);
    } else if (name == "Custom Shape") {
        painter.drawPath(glyph::star({12, 12.8}, 9.5, 4, 5));
    } else if (name == "Hand") {
        QPainterPath p;
        p.moveTo(8, 21);
        p.lineTo(4, 14);
        p.cubicTo(3, 12, 5.5, 11, 7, 13);
        p.lineTo(7.5, 13.5);
        p.lineTo(7.5, 5.5);
        p.cubicTo(7.5, 3.8, 10, 3.8, 10, 5.5);
        p.lineTo(10, 11);
        p.lineTo(10, 3.5);
        p.cubicTo(10, 1.8, 12.5, 1.8, 12.5, 3.5);
        p.lineTo(12.5, 11);
        p.lineTo(12.5, 4.5);
        p.cubicTo(12.5, 2.8, 15, 2.8, 15, 4.5);
        p.lineTo(15, 11.5);
        p.lineTo(15, 7);
        p.cubicTo(15, 5.3, 17.5, 5.3, 17.5, 7);
        p.lineTo(17.5, 15.5);
        p.cubicTo(17.5, 18.5, 16, 21, 15, 21);
        p.closeSubpath();
        painter.drawPath(p);
    } else if (name == "Rotate View") {
        curvedArrow(QRectF(4, 4, 16, 16), 110, 290);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(12, 12), 1.6, 1.6);
        painter.setBrush(Qt::NoBrush);
    } else if (name == "Zoom") {
        painter.drawEllipse(QPointF(10, 10), 6.5, 6.5);
        QPen handle = pen;
        handle.setWidthF(2.6);
        painter.setPen(handle);
        painter.drawLine(QPointF(15, 15), QPointF(20.5, 20.5));
        painter.setPen(pen);
        badgePlus({10, 10}, true);
    } else if (name == "ui:quick-mask") {
        painter.drawRoundedRect(QRectF(3.75, 4.75, 16.5, 14.5), 2, 2);
        painter.setPen(dashed);
        painter.drawEllipse(QPointF(12, 12), 4.2, 4.2);
    } else if (name == "ui:screen-mode") {
        painter.drawRoundedRect(QRectF(3, 4.5, 18, 15), 2, 2);
        painter.fillRect(QRectF(5.5, 9, 13, 8), soft);
        painter.drawLine(QPointF(3, 7.5), QPointF(21, 7.5));
    } else if (name == "ui:expand") {
        painter.drawPolyline(QPolygonF{{6, 7}, {11, 12}, {6, 17}});
        painter.drawPolyline(QPolygonF{{13, 7}, {18, 12}, {13, 17}});
    } else if (name == "ui:collapse") {
        painter.drawPolyline(QPolygonF{{11, 7}, {6, 12}, {11, 17}});
        painter.drawPolyline(QPolygonF{{18, 7}, {13, 12}, {18, 17}});
    } else if (name == "ctx:subject") {
        painter.setPen(dashed);
        painter.drawRoundedRect(QRectF(2.75, 2.75, 18.5, 18.5), 2, 2);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(12, 9.5), 3.2, 3.2);
        QPainterPath body;
        body.moveTo(5.5, 19);
        body.cubicTo(6, 14.5, 9, 13.5, 12, 13.5);
        body.cubicTo(15, 13.5, 18, 14.5, 18.5, 19);
        body.closeSubpath();
        painter.drawPath(body);
    } else if (name == "ctx:remove-background") {
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                if ((x + y) % 2)
                    painter.fillRect(QRectF(3 + x * 4.5, 3 + y * 4.5, 4.5, 4.5), soft);
        painter.drawRoundedRect(QRectF(2.75, 2.75, 18.5, 18.5), 2, 2);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(12, 9.5), 3.2, 3.2);
        QPainterPath body;
        body.moveTo(5.5, 21);
        body.cubicTo(6, 15, 9, 13.5, 12, 13.5);
        body.cubicTo(15, 13.5, 18, 15, 18.5, 21);
        body.closeSubpath();
        painter.drawPath(body);
    } else if (name == "ctx:invert") {
        painter.setPen(dashed);
        painter.drawRect(QRectF(3.75, 3.75, 16.5, 16.5));
        painter.setPen(pen);
        QPainterPath inside;
        inside.addRect(QRectF(3.75, 3.75, 16.5, 16.5));
        QPainterPath hole;
        hole.addEllipse(QPointF(12, 12), 4.5, 4.5);
        fill(inside.subtracted(hole), soft);
        painter.drawEllipse(QPointF(12, 12), 4.5, 4.5);
    } else if (name == "ctx:deselect") {
        painter.setPen(dashed);
        painter.drawRect(QRectF(3.75, 3.75, 16.5, 16.5));
        painter.setPen(pen);
        painter.drawLine(QPointF(9, 9), QPointF(15, 15));
        painter.drawLine(QPointF(15, 9), QPointF(9, 15));
    } else {
        painter.drawRoundedRect(QRectF(4, 4, 16, 16), 2, 2);
    }
    painter.restore();
}
// Draws the glyph as vectors at whatever size and device pixel ratio the widget requests, so
// icons stay sharp at fractional display scaling instead of resampling a fixed bitmap.
class GlyphIconEngine final : public QIconEngine {
  public:
    GlyphIconEngine(QString name, QColor color, bool corner = false)
        : m_name(std::move(name)), m_color(std::move(color)), m_corner(corner) {}
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::TextAntialiasing);
        const qreal side = std::min(rect.width(), rect.height());
        painter->translate(rect.x() + (rect.width() - side) / 2.0, rect.y() + (rect.height() - side) / 2.0);
        painter->scale(side / 22.0, side / 22.0);
        QColor color = m_color;
        if (mode == QIcon::Disabled)
            color.setAlphaF(color.alphaF() * 0.35);
        QPen pen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        paintToolGlyph(*painter, pen, m_name, color);
        if (m_corner) {
            // Photoshop marks tools with hidden variants by a small triangle in the corner.
            QPainterPath triangle;
            triangle.moveTo(22, 18.5);
            triangle.lineTo(22, 22);
            triangle.lineTo(18.5, 22);
            triangle.closeSubpath();
            painter->fillPath(triangle, color);
        }
        painter->restore();
    }
    QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override { return size; }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        QPixmap pixmap((QSizeF(size) * scale).toSize());
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return pixmap;
    }
    QIconEngine *clone() const override { return new GlyphIconEngine(m_name, m_color, m_corner); }
    QString key() const override { return QStringLiteral("SerikaGlyph"); }

  private:
    QString m_name;
    QColor m_color;
    bool m_corner = false;
};
} // namespace
QIcon toolIcon(const QString &name, QColor color) {
    if (!color.isValid())
        color = qApp ? qApp->palette().color(QPalette::Text) : QColor("#FAFAFA");
    return QIcon(new GlyphIconEngine(name, color));
}
QIcon toolGroupIcon(const QString &name, QColor color) {
    if (!color.isValid())
        color = qApp ? qApp->palette().color(QPalette::Text) : QColor("#FAFAFA");
    return QIcon(new GlyphIconEngine(name, color, true));
}
} // namespace serika
