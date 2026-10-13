#pragma once
#include <QDropEvent>
#include <QMouseEvent>
#include <QSet>
#include <QTreeWidget>
#include <functional>
namespace serika {
// Layers panel tree. Column 0 is the visibility eye, column 1 the thumbnail and name, column 2 the
// mask thumbnail and column 3 the lock/effects badges. Rows carry the layer id in column 1's
// Qt::UserRole. The tree never rearranges its own rows: drops are reported through layersDropped
// and the panel is rebuilt from the document, so the document stays the only source of truth.
class LayerTree final : public QTreeWidget {
  public:
    enum class DropPlacement { Above, Below, Inside, Bottom };
    using QTreeWidget::QTreeWidget;
    static constexpr int EyeColumn = 0;
    static constexpr int VisibleRole = Qt::UserRole + 1;
    static constexpr int ContainerRole = Qt::UserRole + 2;
    bool dragging = false;
    QIcon eyeIcon;
    // Called for each row the pointer crosses while a press that began on an eye is held down.
    std::function<void(quint64 id, bool visible)> visibilityChanged;
    // Alt-click on an eye: show only this layer, or restore every layer if it is already alone.
    std::function<void(quint64 id)> soloRequested;
    std::function<void()> visibilityStrokeStarted;
    std::function<void()> visibilityStrokeFinished;
    std::function<void(const QVector<quint64> &ids, quint64 targetId, DropPlacement placement)> layersDropped;
    // Called when any drag ends, dropped or cancelled, so the rows are rebuilt from the document.
    std::function<void()> dragFinished;

    static quint64 idOf(const QTreeWidgetItem *item) { return item->data(1, Qt::UserRole).toULongLong(); }
    void setEyeVisible(QTreeWidgetItem *item, bool visible) const {
        item->setData(EyeColumn, VisibleRole, visible);
        item->setIcon(EyeColumn, visible ? eyeIcon : QIcon());
        item->setToolTip(EyeColumn, visible ? "Hide layer (Alt-click: show only this layer)"
                                            : "Show layer (Alt-click: show only this layer)");
    }

  protected:
    void mousePressEvent(QMouseEvent *event) override {
        auto *item = itemAt(event->position().toPoint());
        const bool onEye = columnAt(event->position().toPoint().x()) == EyeColumn;
        if (event->button() == Qt::LeftButton && item && onEye) {
            if (event->modifiers().testFlag(Qt::AltModifier)) {
                if (soloRequested)
                    soloRequested(idOf(item));
            } else {
                m_strokeVisible = !item->data(EyeColumn, VisibleRole).toBool();
                m_stroked.clear();
                m_stroking = true;
                if (visibilityStrokeStarted)
                    visibilityStrokeStarted();
                applyStroke(item);
            }
            event->accept();
            return;
        }
        QTreeWidget::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (m_stroking) {
            const QPoint eye(columnViewportPosition(EyeColumn) + 2, event->position().toPoint().y());
            if (auto *item = itemAt(eye))
                applyStroke(item);
            event->accept();
            return;
        }
        QTreeWidget::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (m_stroking) {
            m_stroking = false;
            if (visibilityStrokeFinished)
                visibilityStrokeFinished();
            event->accept();
            return;
        }
        QTreeWidget::mouseReleaseEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        // Rapid eye clicks must toggle twice rather than open the rename editor.
        if (columnAt(event->position().toPoint().x()) == EyeColumn) {
            mousePressEvent(event);
            return;
        }
        QTreeWidget::mouseDoubleClickEvent(event);
    }
    void startDrag(Qt::DropActions actions) override {
        dragging = true;
        QTreeWidget::startDrag(actions);
        dragging = false;
        // Some platforms report the drop as a move, after which QAbstractItemView deletes the
        // dragged rows itself; rebuilding restores them whatever the document decided.
        if (dragFinished)
            dragFinished();
    }
    void dropEvent(QDropEvent *event) override {
        if (event->source() != this) {
            event->ignore();
            return;
        }
        QVector<quint64> ids;
        for (auto *item : selectedItems())
            ids.append(idOf(item));
        auto *target = itemAt(event->position().toPoint());
        DropPlacement placement = DropPlacement::Bottom;
        switch (dropIndicatorPosition()) {
        case AboveItem:
            placement = DropPlacement::Above;
            break;
        case BelowItem:
            // Below an expanded group the line is drawn above its first child, so the drop lands inside.
            placement = target && target->isExpanded() && target->childCount() ? DropPlacement::Inside
                                                                               : DropPlacement::Below;
            break;
        case OnItem:
            placement = DropPlacement::Inside;
            break;
        case OnViewport:
            target = nullptr;
            break;
        }
        // CopyAction stops QAbstractItemView from deleting the dragged rows after the drop.
        event->setDropAction(Qt::CopyAction);
        event->accept();
        stopAutoScroll();
        setState(NoState);
        viewport()->update();
        if (layersDropped && !ids.isEmpty())
            layersDropped(ids, target ? idOf(target) : 0, placement);
    }

  private:
    bool m_stroking = false;
    bool m_strokeVisible = true;
    QSet<quint64> m_stroked;
    void applyStroke(QTreeWidgetItem *item) {
        const quint64 id = idOf(item);
        if (m_stroked.contains(id))
            return;
        m_stroked.insert(id);
        setEyeVisible(item, m_strokeVisible);
        if (visibilityChanged)
            visibilityChanged(id, m_strokeVisible);
    }
};
} // namespace serika
