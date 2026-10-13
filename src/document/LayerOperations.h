#pragma once
#include "Document.h"
#include <QRectF>

namespace serika {
enum class LayerAlignment { Left, HorizontalCenter, Right, Top, VerticalCenter, Bottom, Center };
enum class LayerAlignmentReference { SelectedLayers, Canvas, Selection };
enum class LayerDistribution {
    HorizontalCenters,
    VerticalCenters,
    LeftEdges,
    RightEdges,
    TopEdges,
    BottomEdges,
    HorizontalGaps,
    VerticalGaps
};
enum class LayerOrder { Forward, Backward, Front, Back };
struct LayerOperationResult {
    bool changed = false;
    QVector<quint64> selection;
    QString error;
    explicit operator bool() const { return error.isEmpty(); }
};
struct LayerPlacement {
    quint64 id = 0;
    quint64 parentId = 0;
};

// IDs are returned in stack order; a selected descendant of a selected group is omitted.
QVector<quint64> selectedLayerRoots(const Document *document, const QVector<quint64> &ids);
// Content bounds exclude layer effects. Raster bounds inspect native alpha rather than canvas extent.
QRectF layerContentBounds(const Document *document, quint64 id);
LayerOperationResult translateLayers(Document *document, const QVector<quint64> &ids, QPointF delta);
LayerOperationResult alignLayers(Document *document, const QVector<quint64> &ids, LayerAlignment alignment,
                                 LayerAlignmentReference reference = LayerAlignmentReference::SelectedLayers);
LayerOperationResult distributeLayers(Document *document, const QVector<quint64> &ids,
                                      LayerDistribution distribution);
LayerOperationResult duplicateLayers(Document *document, const QVector<quint64> &ids);
LayerOperationResult deleteLayers(Document *document, const QVector<quint64> &ids);
LayerOperationResult groupLayers(Document *document, const QVector<quint64> &ids, const QString &name = {});
LayerOperationResult ungroupLayers(Document *document, const QVector<quint64> &ids);
LayerOperationResult orderLayers(Document *document, const QVector<quint64> &ids, LayerOrder order);
LayerOperationResult reparentLayers(Document *document, const QVector<quint64> &ids, quint64 parentId,
                                    quint64 beforeSiblingId = 0);
// Where dropped layers go relative to a target: directly above or below it among its siblings,
// inside it (groups and artboards only, at the top), or at the top/bottom of the whole stack.
enum class LayerDrop { Above, Below, Inside, Top, Bottom };
LayerOperationResult moveLayers(Document *document, const QVector<quint64> &ids, quint64 targetId,
                                LayerDrop drop);
// Placements contain every layer, in bottom-to-top sibling order. Reparenting preserves world position.
LayerOperationResult applyLayerTree(Document *document, const QVector<LayerPlacement> &placements,
                                    const QVector<quint64> &movedIds);
} // namespace serika
