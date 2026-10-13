#include "document/LayerOperations.h"
#include "ui/MainWindow.h"
#include "ui/panels/LayerTree.h"
#include <QAction>
#include <QActionGroup>
#include <QDockWidget>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QSet>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace serika {
namespace {
const QList<QPair<QString, LayerAlignment>> alignments = {
    {"Align Left Edges", LayerAlignment::Left},
    {"Align Horizontal Centers", LayerAlignment::HorizontalCenter},
    {"Align Right Edges", LayerAlignment::Right},
    {"Align Top Edges", LayerAlignment::Top},
    {"Align Vertical Centers", LayerAlignment::VerticalCenter},
    {"Align Bottom Edges", LayerAlignment::Bottom}};
const QList<QPair<QString, LayerDistribution>> distributions = {
    {"Distribute Horizontal Centers", LayerDistribution::HorizontalCenters},
    {"Distribute Vertical Centers", LayerDistribution::VerticalCenters},
    {"Distribute Left Edges", LayerDistribution::LeftEdges},
    {"Distribute Right Edges", LayerDistribution::RightEdges},
    {"Distribute Top Edges", LayerDistribution::TopEdges},
    {"Distribute Bottom Edges", LayerDistribution::BottomEdges},
    {"Distribute Horizontal Gaps", LayerDistribution::HorizontalGaps},
    {"Distribute Vertical Gaps", LayerDistribution::VerticalGaps}};
class LayerSelectionState : public QObject {
  public:
    using QObject::QObject;
    QPointer<Document> document;
    QHash<Document *, QVector<quint64>> selected;
    QSet<Document *> observed;
    quint64 active = 0;
};
void trackDocument(LayerSelectionState *state, Document *document) {
    if (!state || !document || state->observed.contains(document))
        return;
    state->observed.insert(document);
    QObject::connect(document, &QObject::destroyed, state, [state, document] {
        state->selected.remove(document);
        state->observed.remove(document);
    });
}
LayerSelectionState *selectionState(MainWindow *window) {
    return static_cast<LayerSelectionState *>(window->findChild<QObject *>("layerSelectionState"));
}
void walkItems(QTreeWidget *tree, const std::function<void(QTreeWidgetItem *)> &operation) {
    std::function<void(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *item) {
        operation(item);
        for (int index = 0; index < item->childCount(); ++index)
            walk(item->child(index));
    };
    for (int index = 0; index < tree->topLevelItemCount(); ++index)
        walk(tree->topLevelItem(index));
}
bool selectedAncestor(const Document *document, quint64 id, const QVector<quint64> &selected) {
    auto candidates = selected;
    candidates.append(id);
    return selected.contains(id) || !selectedLayerRoots(document, candidates).contains(id);
}
} // namespace

QVector<quint64> MainWindow::selectedLayerIds() const {
    QVector<quint64> ids;
    const auto *document = currentDocument();
    if (!document || !m_layers)
        return ids;
    QSet<quint64> selected;
    for (auto *item : m_layers->selectedItems())
        selected.insert(item->data(1, Qt::UserRole).toULongLong());
    for (const auto &layer : document->state.layers)
        if (selected.contains(layer.id))
            ids.append(layer.id);
    if (ids.isEmpty() && document->activeLayer())
        ids.append(document->activeLayer()->id);
    return ids;
}
void MainWindow::selectLayerIds(const QVector<quint64> &ids) {
    auto *document = currentDocument();
    if (!document || !m_layers)
        return;
    if (auto *state = selectionState(this)) {
        state->document = document;
        state->selected[document] = ids;
        state->active = document->activeLayer() ? document->activeLayer()->id : 0;
    }
    refresh();
    QSignalBlocker blocker(m_layers);
    m_layers->clearSelection();
    walkItems(m_layers, [&](QTreeWidgetItem *item) {
        item->setSelected(ids.contains(item->data(1, Qt::UserRole).toULongLong()));
    });
    updateLayerOperationUi();
}
void MainWindow::initializeLayerOperations() {
    auto *state = new LayerSelectionState(this);
    state->setObjectName("layerSelectionState");
    setProperty("layerAlignmentReference", int(LayerAlignmentReference::SelectedLayers));
    QMenu *layer = nullptr;
    for (auto *action : menuBar()->actions())
        if (action->text().remove('&') == "Layer") {
            layer = action->menu();
            break;
        }
    if (!layer)
        return;
    auto *align = layer->addMenu("Align Selected Layers");
    align->setObjectName("layerAlignMenu");
    auto *reference = align->addMenu("Align To");
    auto *references = new QActionGroup(reference);
    references->setExclusive(true);
    const QList<QPair<QString, LayerAlignmentReference>> targets = {
        {"Selected layer bounds", LayerAlignmentReference::SelectedLayers},
        {"Canvas", LayerAlignmentReference::Canvas},
        {"Pixel selection", LayerAlignmentReference::Selection}};
    for (const auto &target : targets) {
        auto *action = reference->addAction(target.first);
        action->setCheckable(true);
        action->setChecked(target.second == LayerAlignmentReference::SelectedLayers);
        references->addAction(action);
        connect(action, &QAction::triggered, this, [this, target] {
            setProperty("layerAlignmentReference", int(target.second));
            updateLayerOperationUi();
        });
    }
    align->addSeparator();
    for (const auto &alignment : alignments)
        command(align, alignment.first);
    auto *distribute = layer->addMenu("Distribute Selected Layers");
    distribute->setObjectName("layerDistributeMenu");
    for (const auto &distribution : distributions)
        command(distribute, distribution.first);
    // Docks/options are built after menus. Install their extension once construction completes.
    QTimer::singleShot(0, this, [this, align, distribute] {
        if (!m_layers || !m_options)
            return;
        auto *label = new QLabel;
        label->setObjectName("selectedLayerCount");
        label->setToolTip(
            "Ctrl-click toggles layers; Shift-click selects a range. Selected groups move as units.");
        if (auto *layout = qobject_cast<QVBoxLayout *>(m_docks["Layers"]->widget()->layout()))
            layout->insertWidget(std::max(0, layout->indexOf(m_layers)), label);
        for (auto *menu : {align, distribute}) {
            auto *button = new QToolButton;
            button->setText(menu == align ? "Align" : "Distribute");
            button->setObjectName(menu == align ? "moveAlign" : "moveDistribute");
            button->setMenu(menu);
            button->setPopupMode(QToolButton::InstantPopup);
            auto *action = m_options->addWidget(button);
            action->setProperty("moveOption", true);
            action->setVisible(m_tool == "Move" || m_tool == "Artboard");
        }
        connect(m_layers, &QTreeWidget::itemSelectionChanged, this, [this] {
            if (m_refreshing)
                return;
            auto *document = currentDocument();
            auto *state = selectionState(this);
            if (state && document) {
                state->document = document;
                state->selected[document] = selectedLayerIds();
                state->active = document->activeLayer() ? document->activeLayer()->id : 0;
            }
            updateLayerOperationUi();
        });
        updateLayerOperationUi();
    });
}
void MainWindow::updateLayerOperationUi() {
    if (!m_layers)
        return;
    auto *state = selectionState(this);
    auto *document = currentDocument();
    QVector<quint64> chosen;
    if (state && document) {
        trackDocument(state, document);
        chosen = state->selected.value(document);
        chosen.erase(std::remove_if(chosen.begin(), chosen.end(),
                                    [&](quint64 id) { return document->indexForId(id) < 0; }),
                     chosen.end());
        const quint64 active = document->activeLayer() ? document->activeLayer()->id : 0;
        if (chosen.isEmpty() || (state->document == document && active != state->active && active &&
                                 !selectedAncestor(document, active, chosen)))
            chosen = active ? QVector<quint64>{active} : QVector<quint64>{};
        state->document = document;
        state->selected[document] = chosen;
        state->active = active;
        QSignalBlocker blocker(m_layers);
        walkItems(m_layers, [&](QTreeWidgetItem *item) {
            item->setSelected(chosen.contains(item->data(1, Qt::UserRole).toULongLong()));
        });
    }
    const auto roots = selectedLayerRoots(document, chosen);
    if (auto *label = findChild<QLabel *>("selectedLayerCount"))
        label->setText(
            chosen.size() > 1
                ? QString("%1 layers selected · %2 independent targets").arg(chosen.size()).arg(roots.size())
            : chosen.size() == 1 ? "1 layer selected"
                                 : "No layers selected");
    if (auto *canvas = currentCanvas()) {
        QVariantList ids;
        for (quint64 id : chosen)
            ids.append(QVariant::fromValue(id));
        canvas->setProperty("selectedLayerIds", ids);
    }
    const auto reference = LayerAlignmentReference(property("layerAlignmentReference").toInt());
    for (const auto &alignment : alignments)
        if (auto *action = m_commands.value(alignment.first))
            action->setEnabled(
                document && roots.size() >= (reference == LayerAlignmentReference::SelectedLayers ? 2 : 1) &&
                (reference != LayerAlignmentReference::Selection || document->hasSelection()));
    for (const auto &distribution : distributions)
        if (auto *action = m_commands.value(distribution.first))
            action->setEnabled(document && roots.size() >= 3);
}
bool MainWindow::runLayerOperation(const QString &name) {
    auto *document = currentDocument();
    LayerOperationResult result;
    const auto ids = selectedLayerIds();
    const quint64 previousActive = document && document->activeLayer() ? document->activeLayer()->id : 0;
    const int previousSelectedIndex = document && ids.size() == 1 ? document->indexForId(ids.first()) : -1;
    const auto reference = LayerAlignmentReference(property("layerAlignmentReference").toInt());
    bool recognized = false;
    for (const auto &alignment : alignments)
        if (name == alignment.first) {
            result = alignLayers(document, ids, alignment.second, reference);
            recognized = true;
            break;
        }
    if (!recognized)
        for (const auto &distribution : distributions)
            if (name == distribution.first) {
                result = distributeLayers(document, ids, distribution.second);
                recognized = true;
                break;
            }
    if (name == "All Layers") {
        if (document) {
            QVector<quint64> all;
            for (const Layer &layer : document->state.layers)
                all.append(layer.id);
            selectLayerIds(all);
        }
        return true;
    }
    if (!recognized && name == "Duplicate Layer") {
        result = duplicateLayers(document, ids);
        recognized = true;
    } else if (!recognized && name == "Layer via Copy" && ids.size() > 1 && document &&
               !document->hasSelection()) {
        result = duplicateLayers(document, ids);
        recognized = true;
    } else if (!recognized && name == "Delete Layer") {
        result = deleteLayers(document, ids);
        recognized = true;
    } else if (!recognized && name == "Group Layers") {
        result = groupLayers(document, ids);
        recognized = true;
    } else if (!recognized && name == "Ungroup Layers") {
        result = ungroupLayers(document, ids);
        recognized = true;
    } else if (!recognized &&
               QStringList{"Bring Forward", "Send Backward", "Bring to Front", "Send to Back"}.contains(
                   name)) {
        const auto order = name == "Bring Forward"    ? LayerOrder::Forward
                           : name == "Send Backward"  ? LayerOrder::Backward
                           : name == "Bring to Front" ? LayerOrder::Front
                                                      : LayerOrder::Back;
        result = orderLayers(document, ids, order);
        recognized = true;
    } else if (!recognized && name == "Auto-Align Layers") {
        result = alignLayers(document, ids, LayerAlignment::Center, LayerAlignmentReference::Canvas);
        recognized = true;
    }
    if (!recognized)
        return false;
    if (!result.error.isEmpty())
        showMessage(result.error);
    else {
        selectLayerIds(result.selection);
        QString message;
        if (name == "Auto-Align Layers")
            message = "Centered selected layer content on the canvas. Photo registration is not implemented.";
        else if (result.changed)
            message = QString("%1 · %2 selected targets").arg(name).arg(result.selection.size());
        if (m_recording && result.changed) {
            if (ids.size() == 1 && (name == "Duplicate Layer" || name == "Delete Layer")) {
                if (previousActive != ids.first())
                    recordStep("Select Layer", {}, QJsonObject{{"index", previousSelectedIndex}});
                recordStep(name);
            } else {
                message += " · Not recorded: action sets do not support this selected-layer operation.";
            }
        }
        if (!message.isEmpty())
            showMessage(message);
    }
    return true;
}
} // namespace serika
