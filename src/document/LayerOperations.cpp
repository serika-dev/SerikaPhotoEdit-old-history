#include "LayerOperations.h"
#include "TextLayout.h"
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>

namespace serika {
namespace {
bool container(const Layer &layer) {
    return layer.kind == LayerKind::Group || layer.kind == LayerKind::Artboard;
}
bool nonzero(QPointF p) { return std::abs(p.x()) > 1e-9 || std::abs(p.y()) > 1e-9; }
bool finite(QPointF p) { return std::isfinite(p.x()) && std::isfinite(p.y()); }
struct Tree {
    QHash<quint64, Layer> layers;
    QHash<quint64, QVector<quint64>> children;
    QString error;
    explicit Tree(const Document *document) {
        if (!document) {
            error = "Open a document first.";
            return;
        }
        for (const Layer &layer : document->state.layers) {
            if (!layer.id || layers.contains(layer.id)) {
                error = "The layer tree contains duplicate or invalid IDs.";
                return;
            }
            layers.insert(layer.id, layer);
        }
        for (const Layer &layer : document->state.layers) {
            quint64 parent = layer.parentId;
            if (parent && (!layers.contains(parent) || !container(layers[parent]))) {
                error = "A layer has an invalid parent.";
                return;
            }
            QSet<quint64> ancestors{layer.id};
            while (parent) {
                if (ancestors.contains(parent)) {
                    error = "The layer tree contains a parent cycle.";
                    return;
                }
                ancestors.insert(parent);
                parent = layers[parent].parentId;
            }
            children[layer.parentId].append(layer.id);
        }
    }
    QVector<Layer> flattened() const {
        QVector<Layer> result;
        std::function<void(quint64)> visit = [&](quint64 parent) {
            for (quint64 id : children.value(parent)) {
                visit(id);
                result.append(layers.value(id));
            }
        };
        visit(0);
        return result;
    }
    QVector<quint64> subtree(quint64 root) const {
        QVector<quint64> result{root};
        for (qsizetype index = 0; index < result.size(); ++index)
            result.append(children.value(result[index]));
        return result;
    }
    QPointF worldOffset(quint64 id) const {
        QPointF result;
        QSet<quint64> visited;
        while (id && layers.contains(id) && !visited.contains(id)) {
            visited.insert(id);
            result += layers.value(id).offset;
            id = layers.value(id).parentId;
        }
        return result;
    }
    QString movable(quint64 id, bool position = true) const {
        quint64 ancestor = id;
        while (ancestor) {
            const Layer &layer = layers[ancestor];
            if (layer.locked || (position && layer.lockPosition))
                return "Unlock “" + layer.name + "” before changing these layers.";
            ancestor = layer.parentId;
        }
        for (quint64 child : subtree(id)) {
            const Layer &layer = layers[child];
            if (layer.locked || (position && layer.lockPosition))
                return "Unlock “" + layer.name + "” before changing its group.";
        }
        return {};
    }
    quint64 nextId() const {
        quint64 next = 1;
        for (auto it = layers.cbegin(); it != layers.cend(); ++it)
            next = std::max(next, it.key() + 1);
        return next;
    }
};
LayerOperationResult fail(const QString &message, QVector<quint64> selection = {}) {
    return {false, std::move(selection), message};
}
quint64 activeId(const Document *document) {
    return document && document->activeLayer() ? document->activeLayer()->id : 0;
}
void commit(Document *document, const QString &name, const Tree &tree, quint64 active) {
    const QVector<Layer> flattened = tree.flattened();
    const int previousIndex = document->state.activeIndex;
    document->mutate(name, [&] {
        document->state.layers = flattened;
        const int index = document->indexForId(active);
        document->state.activeIndex = flattened.isEmpty() ? -1
                                      : index >= 0 ? index
                                                   : std::clamp(previousIndex, 0, int(flattened.size()) - 1);
    });
    emit document->activeLayerChanged();
}
QRect alphaBounds(const QImage &image) {
    int left = image.width(), top = image.height(), right = -1, bottom = -1;
    const auto format = image.format();
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            bool covered;
            if (format == QImage::Format_RGBA64 || format == QImage::Format_RGBA64_Premultiplied)
                covered = reinterpret_cast<const QRgba64 *>(image.constScanLine(y))[x].alpha() != 0;
            else if (format == QImage::Format_RGBA32FPx4 || format == QImage::Format_RGBA32FPx4_Premultiplied)
                covered = reinterpret_cast<const float *>(image.constScanLine(y))[4 * x + 3] > 0;
            else if (format == QImage::Format_RGBA8888 || format == QImage::Format_RGBA8888_Premultiplied)
                covered = image.constScanLine(y)[4 * x + 3] != 0;
            else
                covered = image.pixelColor(x, y).alphaF() > 0;
            if (covered) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    return right >= left ? QRect(QPoint(left, top), QPoint(right, bottom)) : QRect();
}
QRectF bounds(const Document *document, const Tree &tree, quint64 id) {
    if (!tree.layers.contains(id))
        return {};
    const Layer &layer = tree.layers[id];
    const QPointF origin = tree.worldOffset(id);
    if (layer.kind == LayerKind::Artboard) {
        const auto rect = layer.parameters["rect"].toArray();
        QRectF area = rect.size() == 4 ? QRectF(rect[0].toDouble(), rect[1].toDouble(), rect[2].toDouble(),
                                                rect[3].toDouble())
                      : layer.shape.isEmpty() ? QRectF(QPointF(), document->state.size)
                                              : layer.shape.boundingRect();
        return area.translated(origin);
    }
    if (layer.kind == LayerKind::Group) {
        QRectF area;
        for (quint64 child : tree.children.value(id))
            area = area.united(bounds(document, tree, child));
        return area;
    }
    if (layer.kind == LayerKind::Adjustment)
        return {};
    QRectF area;
    if (layer.kind == LayerKind::Text && hasRichTypography(layer) &&
        !layer.parameters.contains("contentTransform")) {
        area = textLayerBounds(layer, document->state.size);
    } else if ((layer.kind == LayerKind::Pixel || layer.kind == LayerKind::SmartObject) &&
        !layer.parameters.contains("contentTransform") && layer.smartFilters.isEmpty()) {
        for (auto it = layer.pixels.tiles.cbegin(); it != layer.pixels.tiles.cend(); ++it) {
            const QRect tileBounds = alphaBounds(it.value());
            if (!tileBounds.isEmpty()) {
                const QPoint tileOrigin(int(quint32(it.key() >> 32)) * TileImage::TileSize,
                                        int(quint32(it.key())) * TileImage::TileSize);
                area = area.united(QRectF(tileBounds.translated(tileOrigin)));
            }
        }
    } else
        area = alphaBounds(document->layerImage(layer));
    return area.isEmpty() ? QRectF() : area.translated(origin);
}
void translate(Tree &tree, quint64 id, QPointF delta) {
    tree.layers[id].offset += delta;
    // Unlinked masks stay in document space, including masks on descendants of a moved group.
    for (quint64 child : tree.subtree(id)) {
        Layer &layer = tree.layers[child];
        if (!layer.maskLinked)
            layer.maskOffset -= delta;
        if (!layer.vectorMaskLinked)
            layer.vectorMaskOffset -= delta;
    }
}
LayerOperationResult applyTranslations(Document *document, const Tree &original,
                                       const QVector<quint64> &roots,
                                       const QHash<quint64, QPointF> &displacements, const QString &name) {
    Tree tree = original;
    bool changed = false;
    for (quint64 id : roots) {
        const QPointF delta = displacements.value(id);
        if (!finite(delta))
            return fail("Layer positions must be finite.", roots);
        if (!nonzero(delta))
            continue;
        const QString lock = tree.movable(id);
        if (!lock.isEmpty())
            return fail(lock, roots);
        translate(tree, id, delta);
        changed = true;
    }
    if (changed)
        commit(document, name, tree, activeId(document));
    return {changed, roots, {}};
}
QString validateSelection(const Tree &tree, const QVector<quint64> &roots, int minimum = 1) {
    if (!tree.error.isEmpty())
        return tree.error;
    if (roots.size() < minimum)
        return minimum > 1 ? QString("Select at least %1 independent layers or groups.").arg(minimum)
                           : QString("Select a layer first.");
    return {};
}
} // namespace

QVector<quint64> selectedLayerRoots(const Document *document, const QVector<quint64> &ids) {
    QVector<quint64> result;
    if (!document)
        return result;
    const QSet<quint64> selected(ids.cbegin(), ids.cend());
    for (const Layer &layer : document->state.layers) {
        if (!selected.contains(layer.id))
            continue;
        QSet<quint64> visited{layer.id};
        quint64 parent = layer.parentId;
        bool nested = false;
        while (parent && !visited.contains(parent)) {
            visited.insert(parent);
            if (selected.contains(parent)) {
                nested = true;
                break;
            }
            const int index = document->indexForId(parent);
            parent = index >= 0 ? document->state.layers[index].parentId : 0;
        }
        if (!nested)
            result.append(layer.id);
    }
    return result;
}
QRectF layerContentBounds(const Document *document, quint64 id) {
    const Tree tree(document);
    return tree.error.isEmpty() ? bounds(document, tree, id) : QRectF();
}
LayerOperationResult translateLayers(Document *document, const QVector<quint64> &ids, QPointF delta) {
    const Tree tree(document);
    const auto roots = selectedLayerRoots(document, ids);
    const QString error = validateSelection(tree, roots);
    if (!error.isEmpty())
        return fail(error, roots);
    QHash<quint64, QPointF> moves;
    for (quint64 id : roots)
        moves[id] = delta;
    return applyTranslations(document, tree, roots, moves, "Move selected layers");
}
LayerOperationResult alignLayers(Document *document, const QVector<quint64> &ids, LayerAlignment alignment,
                                 LayerAlignmentReference reference) {
    const Tree tree(document);
    const auto roots = selectedLayerRoots(document, ids);
    const QString error =
        validateSelection(tree, roots, reference == LayerAlignmentReference::SelectedLayers ? 2 : 1);
    if (!error.isEmpty())
        return fail(error, roots);
    QHash<quint64, QRectF> rectangles;
    QRectF target;
    for (quint64 id : roots) {
        const QRectF area = bounds(document, tree, id);
        if (area.isEmpty())
            return fail("“" + tree.layers[id].name + "” has no content to align.", roots);
        rectangles[id] = area;
        target = target.united(area);
    }
    if (reference == LayerAlignmentReference::Canvas)
        target = QRectF(QPointF(), document->state.size);
    else if (reference == LayerAlignmentReference::Selection) {
        if (!document->hasSelection())
            return fail("Create a pixel selection to align to its bounds.", roots);
        target = document->selectionBounds();
    }
    QHash<quint64, QPointF> moves;
    for (quint64 id : roots) {
        const auto rect = rectangles[id];
        QPointF delta;
        switch (alignment) {
        case LayerAlignment::Left:
            delta.setX(target.left() - rect.left());
            break;
        case LayerAlignment::HorizontalCenter:
            delta.setX(target.center().x() - rect.center().x());
            break;
        case LayerAlignment::Right:
            delta.setX(target.right() - rect.right());
            break;
        case LayerAlignment::Top:
            delta.setY(target.top() - rect.top());
            break;
        case LayerAlignment::VerticalCenter:
            delta.setY(target.center().y() - rect.center().y());
            break;
        case LayerAlignment::Bottom:
            delta.setY(target.bottom() - rect.bottom());
            break;
        case LayerAlignment::Center:
            delta = target.center() - rect.center();
            break;
        }
        moves[id] = delta;
    }
    return applyTranslations(document, tree, roots, moves, "Align selected layers");
}
LayerOperationResult distributeLayers(Document *document, const QVector<quint64> &ids,
                                      LayerDistribution distribution) {
    const Tree tree(document);
    auto roots = selectedLayerRoots(document, ids);
    const QString error = validateSelection(tree, roots, 3);
    if (!error.isEmpty())
        return fail(error, roots);
    const bool horizontal = distribution == LayerDistribution::HorizontalCenters ||
                            distribution == LayerDistribution::LeftEdges ||
                            distribution == LayerDistribution::RightEdges ||
                            distribution == LayerDistribution::HorizontalGaps;
    const bool gaps =
        distribution == LayerDistribution::HorizontalGaps || distribution == LayerDistribution::VerticalGaps;
    QHash<quint64, QRectF> rectangles;
    auto coordinate = [&](quint64 id) {
        const QRectF area = rectangles[id];
        switch (distribution) {
        case LayerDistribution::LeftEdges:
            return area.left();
        case LayerDistribution::RightEdges:
            return area.right();
        case LayerDistribution::TopEdges:
            return area.top();
        case LayerDistribution::BottomEdges:
            return area.bottom();
        default:
            return horizontal ? area.center().x() : area.center().y();
        }
    };
    for (quint64 id : roots) {
        const QRectF area = bounds(document, tree, id);
        if (area.isEmpty())
            return fail("“" + tree.layers[id].name + "” has no content to distribute.", roots);
        rectangles[id] = area;
    }
    std::stable_sort(roots.begin(), roots.end(), [&](quint64 a, quint64 b) {
        if (gaps)
            return horizontal ? rectangles[a].left() < rectangles[b].left()
                              : rectangles[a].top() < rectangles[b].top();
        return coordinate(a) < coordinate(b);
    });
    QHash<quint64, QPointF> moves;
    if (gaps) {
        qreal sum = 0;
        for (quint64 id : roots)
            sum += horizontal ? rectangles[id].width() : rectangles[id].height();
        const auto first = rectangles[roots.first()], last = rectangles[roots.last()];
        const qreal start = horizontal ? first.left() : first.top();
        const qreal end = horizontal ? last.right() : last.bottom();
        const qreal gap = (end - start - sum) / (roots.size() - 1);
        qreal next = start;
        for (quint64 id : roots) {
            const auto area = rectangles[id];
            const qreal delta = next - (horizontal ? area.left() : area.top());
            moves[id] = horizontal ? QPointF(delta, 0) : QPointF(0, delta);
            next += (horizontal ? area.width() : area.height()) + gap;
        }
    } else {
        const qreal start = coordinate(roots.first()), end = coordinate(roots.last());
        for (qsizetype index = 0; index < roots.size(); ++index) {
            const qreal delta = start + (end - start) * index / (roots.size() - 1) - coordinate(roots[index]);
            moves[roots[index]] = horizontal ? QPointF(delta, 0) : QPointF(0, delta);
        }
    }
    return applyTranslations(document, tree, roots, moves, "Distribute selected layers");
}
LayerOperationResult duplicateLayers(Document *document, const QVector<quint64> &ids) {
    Tree tree(document);
    const auto roots = selectedLayerRoots(document, ids);
    const QString error = validateSelection(tree, roots);
    if (!error.isEmpty())
        return fail(error, roots);
    quint64 next = tree.nextId();
    QHash<quint64, quint64> mapping;
    QVector<quint64> copies;
    for (quint64 root : roots)
        for (quint64 id : tree.subtree(root))
            mapping[id] = next++;
    const Tree original = tree;
    for (auto it = mapping.cbegin(); it != mapping.cend(); ++it) {
        Layer copy = original.layers[it.key()];
        copy.id = it.value();
        copy.parentId = mapping.value(copy.parentId, copy.parentId);
        if (roots.contains(it.key()))
            copy.name += " copy";
        tree.layers[copy.id] = copy;
        QVector<quint64> children;
        for (quint64 child : original.children.value(it.key()))
            children.append(mapping[child]);
        tree.children[copy.id] = children;
    }
    for (quint64 root : roots) {
        const quint64 parent = original.layers[root].parentId;
        auto &siblings = tree.children[parent];
        siblings.insert(siblings.indexOf(root) + 1, mapping[root]);
        copies.append(mapping[root]);
    }
    commit(document, "Duplicate selected layers", tree, mapping.value(activeId(document), copies.last()));
    return {true, copies, {}};
}
LayerOperationResult deleteLayers(Document *document, const QVector<quint64> &ids) {
    Tree tree(document);
    const auto roots = selectedLayerRoots(document, ids);
    const QString error = validateSelection(tree, roots);
    if (!error.isEmpty())
        return fail(error, roots);
    for (quint64 id : roots) {
        const QString lock = tree.movable(id, false);
        if (!lock.isEmpty())
            return fail(lock, roots);
    }
    for (quint64 root : roots) {
        tree.children[tree.layers[root].parentId].removeAll(root);
        for (quint64 id : tree.subtree(root)) {
            tree.layers.remove(id);
            tree.children.remove(id);
        }
    }
    commit(document, "Delete selected layers", tree, activeId(document));
    return {true, activeId(document) ? QVector<quint64>{activeId(document)} : QVector<quint64>{}, {}};
}
LayerOperationResult groupLayers(Document *document, const QVector<quint64> &ids, const QString &name) {
    Tree tree(document);
    const auto roots = selectedLayerRoots(document, ids);
    const QString error = validateSelection(tree, roots);
    if (!error.isEmpty())
        return fail(error, roots);
    for (quint64 id : roots) {
        const QString lock = tree.movable(id);
        if (!lock.isEmpty())
            return fail(lock, roots);
    }
    QVector<quint64> candidates;
    quint64 common = tree.layers[roots.first()].parentId;
    while (common) {
        candidates.append(common);
        common = tree.layers[common].parentId;
    }
    candidates.append(0);
    for (quint64 candidate : candidates) {
        bool shared = true;
        for (quint64 root : roots) {
            quint64 parent = tree.layers[root].parentId;
            while (parent && parent != candidate)
                parent = tree.layers[parent].parentId;
            if (parent != candidate) {
                shared = false;
                break;
            }
        }
        if (shared) {
            common = candidate;
            break;
        }
    }
    int insertion = 0;
    for (quint64 root : roots) {
        quint64 branch = root;
        while (tree.layers[branch].parentId != common)
            branch = tree.layers[branch].parentId;
        insertion = std::max(insertion, int(tree.children[common].indexOf(branch)) + 1);
    }
    QHash<quint64, QPointF> origins;
    for (quint64 root : roots)
        origins[root] = tree.worldOffset(root);
    Layer group;
    group.id = tree.nextId();
    group.kind = LayerKind::Group;
    group.blendMode = "Pass Through";
    group.parentId = common;
    group.name = name.isEmpty() ? "Group " + QString::number(tree.layers.size() + 1) : name;
    const QPointF parentOrigin = tree.worldOffset(common);
    for (quint64 root : roots) {
        const quint64 previous = tree.layers[root].parentId;
        const int index = tree.children[previous].indexOf(root);
        if (previous == common && index < insertion)
            --insertion;
        tree.children[previous].removeAll(root);
        tree.layers[root].parentId = group.id;
        tree.layers[root].offset = origins[root] - parentOrigin;
    }
    tree.layers[group.id] = group;
    tree.children[group.id] = roots;
    tree.children[common].insert(std::clamp(insertion, 0, int(tree.children[common].size())), group.id);
    commit(document, "Group selected layers", tree, group.id);
    return {true, {group.id}, {}};
}
LayerOperationResult ungroupLayers(Document *document, const QVector<quint64> &ids) {
    Tree tree(document);
    QVector<quint64> groups;
    for (quint64 id : ids) {
        if (!tree.layers.contains(id))
            continue;
        if (tree.layers[id].kind != LayerKind::Group)
            id = tree.layers[id].parentId;
        if (id && tree.layers[id].kind == LayerKind::Group && !groups.contains(id))
            groups.append(id);
    }
    const auto roots = selectedLayerRoots(document, groups);
    const QString error = validateSelection(tree, roots);
    if (!error.isEmpty())
        return fail(error == "Select a layer first." ? "Select a group or one of its children to ungroup."
                                                     : error);
    for (quint64 id : roots) {
        const QString lock = tree.movable(id);
        if (!lock.isEmpty())
            return fail(lock, roots);
    }
    QVector<quint64> selected;
    for (quint64 group : roots) {
        const quint64 parent = tree.layers[group].parentId;
        const QPointF origin = tree.worldOffset(parent);
        const auto children = tree.children.value(group);
        int index = tree.children[parent].indexOf(group);
        tree.children[parent].removeAt(index);
        for (quint64 child : children) {
            const QPointF world = tree.worldOffset(child);
            tree.layers[child].parentId = parent;
            tree.layers[child].offset = world - origin;
            tree.children[parent].insert(index++, child);
            selected.append(child);
        }
        tree.layers.remove(group);
        tree.children.remove(group);
    }
    commit(document, "Ungroup selected layers", tree,
           selected.isEmpty() ? activeId(document) : selected.last());
    return {true, selected, {}};
}
LayerOperationResult orderLayers(Document *document, const QVector<quint64> &ids, LayerOrder order) {
    Tree tree(document);
    const auto roots = selectedLayerRoots(document, ids);
    const QString error = validateSelection(tree, roots);
    if (!error.isEmpty())
        return fail(error, roots);
    const QSet<quint64> chosen(roots.cbegin(), roots.cend());
    QSet<quint64> parents;
    for (quint64 id : roots)
        parents.insert(tree.layers[id].parentId);
    bool changed = false;
    for (quint64 parent : parents) {
        auto &siblings = tree.children[parent];
        const auto before = siblings;
        if (order == LayerOrder::Forward) {
            for (int i = int(siblings.size()) - 2; i >= 0; --i)
                if (chosen.contains(siblings[i]) && !chosen.contains(siblings[i + 1]))
                    siblings.swapItemsAt(i, i + 1);
        } else if (order == LayerOrder::Backward) {
            for (int i = 1; i < siblings.size(); ++i)
                if (chosen.contains(siblings[i]) && !chosen.contains(siblings[i - 1]))
                    siblings.swapItemsAt(i, i - 1);
        } else {
            QVector<quint64> selected, others;
            for (quint64 id : siblings)
                (chosen.contains(id) ? selected : others).append(id);
            siblings = order == LayerOrder::Front ? others + selected : selected + others;
        }
        if (before != siblings) {
            for (quint64 id : siblings)
                if (chosen.contains(id) && before.indexOf(id) != siblings.indexOf(id)) {
                    const QString lock = tree.movable(id);
                    if (!lock.isEmpty())
                        return fail(lock, roots);
                }
            changed = true;
        }
    }
    if (changed)
        commit(document, "Arrange selected layers", tree, activeId(document));
    return {changed, roots, {}};
}
LayerOperationResult reparentLayers(Document *document, const QVector<quint64> &ids, quint64 parentId,
                                    quint64 beforeSiblingId) {
    Tree tree(document);
    const auto roots = selectedLayerRoots(document, ids);
    const QString error = validateSelection(tree, roots);
    if (!error.isEmpty())
        return fail(error, roots);
    if (parentId && (!tree.layers.contains(parentId) || !container(tree.layers[parentId])))
        return fail("Layers can be placed only inside groups or artboards.", roots);
    if (beforeSiblingId &&
        (!tree.children.value(parentId).contains(beforeSiblingId) || roots.contains(beforeSiblingId)))
        return fail("The insertion layer must be an unselected sibling in the destination group.", roots);
    if (parentId) {
        const QString lock = tree.movable(parentId);
        if (!lock.isEmpty())
            return fail(lock, roots);
    }
    for (quint64 id : roots) {
        if (tree.subtree(id).contains(parentId))
            return fail("A group cannot be moved inside itself or its descendants.", roots);
        const QString lock = tree.movable(id);
        if (!lock.isEmpty())
            return fail(lock, roots);
    }
    const Tree before = tree;
    const QPointF origin = tree.worldOffset(parentId);
    for (quint64 id : roots) {
        tree.children[tree.layers[id].parentId].removeAll(id);
        tree.layers[id].parentId = parentId;
        tree.layers[id].offset = before.worldOffset(id) - origin;
    }
    int index = beforeSiblingId ? tree.children[parentId].indexOf(beforeSiblingId)
                                : int(tree.children[parentId].size());
    for (quint64 id : roots)
        tree.children[parentId].insert(index++, id);
    if (tree.children == before.children)
        return {false, roots, {}};
    const bool reparented = std::any_of(roots.cbegin(), roots.cend(), [&](quint64 id) {
        return before.layers[id].parentId != parentId;
    });
    commit(document, reparented ? "Move layers into group" : "Reorder layers", tree, activeId(document));
    return {true, roots, {}};
}
LayerOperationResult applyLayerTree(Document *document, const QVector<LayerPlacement> &placements,
                                    const QVector<quint64> &movedIds) {
    Tree tree(document);
    const auto roots = selectedLayerRoots(document, movedIds);
    const QString error = validateSelection(tree, roots);
    if (!error.isEmpty())
        return fail(error, roots);
    if (placements.size() != tree.layers.size())
        return fail("The drop did not retain the complete layer tree.", roots);
    Tree changed = tree;
    changed.children.clear();
    QSet<quint64> present;
    for (const auto &placement : placements) {
        if (!tree.layers.contains(placement.id) || present.contains(placement.id))
            return fail("The drop contains a missing or duplicate layer.", roots);
        if (placement.parentId &&
            (!tree.layers.contains(placement.parentId) || !container(tree.layers[placement.parentId])))
            return fail("Layers can be placed only inside groups or artboards.", roots);
        present.insert(placement.id);
        changed.layers[placement.id].parentId = placement.parentId;
        changed.children[placement.parentId].append(placement.id);
    }
    for (const LayerPlacement &placement : placements) {
        QSet<quint64> ancestors{placement.id};
        quint64 parent = placement.parentId;
        while (parent) {
            if (ancestors.contains(parent))
                return fail("A group cannot be moved inside itself or its descendants.", roots);
            ancestors.insert(parent);
            parent = changed.layers[parent].parentId;
        }
    }
    if (tree.children == changed.children)
        return {false, roots, {}};
    for (quint64 id : roots) {
        const QString lock = tree.movable(id);
        if (!lock.isEmpty())
            return fail(lock, roots);
    }
    for (const LayerPlacement &placement : placements) {
        const quint64 parent = placement.parentId;
        if (parent != tree.layers[placement.id].parentId) {
            const QString lock = tree.movable(placement.id);
            if (!lock.isEmpty())
                return fail(lock, roots);
            quint64 ancestor = parent;
            while (ancestor) {
                if (changed.layers[ancestor].locked || changed.layers[ancestor].lockPosition)
                    return fail("Unlock the destination group before moving layers into it.", roots);
                ancestor = changed.layers[ancestor].parentId;
            }
            // Ancestors may also have been reparented, but every old world origin is retained.
            changed.layers[placement.id].offset = tree.worldOffset(placement.id) - tree.worldOffset(parent);
        }
    }
    commit(document, "Reorder selected layers", changed, activeId(document));
    return {true, roots, {}};
}
} // namespace serika
