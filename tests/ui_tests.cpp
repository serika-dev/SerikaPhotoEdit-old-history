#include "ui/MainWindow.h"
#include "ui/panels/LayerTree.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QtTest>

using namespace serika;
class UiTests : public QObject {
    Q_OBJECT
    QTemporaryDir m_settingsDirectory;
    static void show(MainWindow &window) {
        window.resize(1440, 900);
        window.show();
        window.activateWindow();
        QTest::qWait(20);
        window.setWorkspace("Essentials", true);
        QCoreApplication::processEvents();
    }
    static QTreeWidgetItem *itemFor(QTreeWidget *tree, quint64 id) {
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if (LayerTree::idOf(*it) == id)
                return *it;
        return nullptr;
    }
    static QPoint eyePoint(QTreeWidget *tree, quint64 id) {
        auto *item = itemFor(tree, id);
        return item ? QPoint(tree->columnViewportPosition(LayerTree::EyeColumn) + tree->columnWidth(0) / 2,
                             tree->visualItemRect(item).center().y())
                    : QPoint();
    }
    static QVector<quint64> stack(const Document *doc, quint64 parent = 0) {
        QVector<quint64> ids; // bottom-to-top
        for (const auto &layer : doc->state.layers)
            if (layer.parentId == parent)
                ids.append(layer.id);
        return ids;
    }
  private slots:
    void initTestCase() {
        QVERIFY(m_settingsDirectory.isValid());
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDirectory.path());
    }
    void essentialsCreatesFourDocksAndHome() {
        MainWindow window;
        show(window);
        QVERIFY(!window.currentDocument());
        QWidget *home = window.findChild<QWidget *>("home");
        QVERIFY(home);
        QVERIFY(home->isVisible());
        for (const auto &name : QStringList{"Color", "Adjustments", "Layers", "Properties"}) {
            QVERIFY(window.dockNames().contains(name));
            QDockWidget *dock = window.findChild<QDockWidget *>("panel_" + name);
            QVERIFY(dock);
            QVERIFY(!dock->isVisible());
            QCOMPARE(window.dockWidgetArea(dock), Qt::RightDockWidgetArea);
        }
        QDockWidget *tools = window.findChild<QDockWidget *>("tools");
        QVERIFY(tools);
        QVERIFY(!tools->isVisible());
        QCOMPARE(window.dockWidgetArea(tools), Qt::LeftDockWidgetArea);
        auto *tabs = window.findChild<QTabWidget *>("documentTabs");
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 0);
        window.openDemo();
        window.setWorkspace("Essentials", true);
        QCoreApplication::processEvents();
        // Photoshop's Essentials: three stacks, with Properties as a tab beside Adjustments.
        const QStringList order{"Color", "Adjustments", "Layers"};
        auto *adjustments = window.findChild<QDockWidget *>("panel_Adjustments");
        QVERIFY(window.tabifiedDockWidgets(adjustments)
                    .contains(window.findChild<QDockWidget *>("panel_Properties")));
        for (const QString &name : order)
            QVERIFY(window.findChild<QDockWidget *>("panel_" + name)->isVisible());
        for (int i = 1; i < order.size(); ++i) {
            const QRect upper = window.findChild<QDockWidget *>("panel_" + order[i - 1])->geometry();
            const QRect lower = window.findChild<QDockWidget *>("panel_" + order[i])->geometry();
            QVERIFY2(upper.bottom() < lower.top(),
                     qPrintable(QString("%1 at y=%2..%3 must be above %4 at y=%5..%6")
                                    .arg(order[i - 1])
                                    .arg(upper.top())
                                    .arg(upper.bottom())
                                    .arg(order[i])
                                    .arg(lower.top())
                                    .arg(lower.bottom())));
            QVERIFY(upper != lower);
        }
    }
    void newDocumentDialogCreatesSixteenBitCanvas() {
        MainWindow window;
        show(window);
        bool accepted = false;
        QTimer::singleShot(3000, &window, [] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->windowTitle(), QString("New document"));
            const auto dimensions = dialog->findChildren<QSpinBox *>();
            QCOMPARE(dimensions.size(), 2);
            dimensions[0]->setValue(320);
            dimensions[1]->setValue(240);
            for (auto *combo : dialog->findChildren<QComboBox *>()) {
                const int sixteen = combo->findText("16 bit"), transparent = combo->findText("Transparent");
                if (sixteen >= 0)
                    combo->setCurrentIndex(sixteen);
                if (transparent >= 0)
                    combo->setCurrentIndex(transparent);
            }
            for (auto *edit : dialog->findChildren<QLineEdit *>())
                if (edit->text() == "Untitled-1")
                    edit->setText("UI test canvas");
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            QVERIFY(buttons);
            accepted = true;
            QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
        });
        window.newDocument();
        QVERIFY(accepted);
        QVERIFY(window.currentDocument());
        QVERIFY(window.currentCanvas());
        QCoreApplication::processEvents();
        Document *doc = window.currentDocument();
        QCOMPARE(doc->title, QString("UI test canvas"));
        QCOMPARE(doc->state.size, QSize(320, 240));
        QCOMPARE(doc->state.bitDepth, 16);
        QVERIFY(!doc->isModified());
        QCOMPARE(doc->composite().format(), QImage::Format_RGBA64);
        QCOMPARE(doc->composite().pixelColor(0, 0).alpha(), 0);
        QVERIFY(window.currentCanvas()->isVisible());
    }
    void homeWelcomeProjectOpensCanvas() {
        MainWindow window;
        show(window);
        window.openDemo();
        QCoreApplication::processEvents();
        QVERIFY(window.currentDocument());
        QVERIFY(window.currentCanvas());
        QVERIFY(window.currentCanvas()->isVisible());
        QCOMPARE(window.currentDocument()->state.size, QSize(1600, 1000));
        QVERIFY(window.currentDocument()->state.layers.size() >= 5);
        QVERIFY(!window.currentDocument()->isModified());
        QVERIFY(!window.findChild<QWidget *>("home")->isVisible());
        QCOMPARE(window.findChild<QTabWidget *>("documentTabs")->count(), 1);
        QCOMPARE(window.currentDocument()->composite().size(), QSize(1600, 1000));
    }
    void brushGestureChangesPixelsAndKeyboardUndo() {
        MainWindow window;
        show(window);
        window.openDemo();
        Document *doc = window.currentDocument();
        doc->resizeImage({320, 200}, Qt::FastTransformation);
        window.runCommand("New Layer");
        CanvasView *canvas = window.currentCanvas();
        QCoreApplication::processEvents();
        QVERIFY(doc->activeLayer());
        QCOMPARE(doc->activeLayer()->kind, LayerKind::Pixel);
        canvas->setShowRulers(false);
        canvas->setZoom(1);
        canvas->setTool("Brush");
        canvas->setBrushSize(20);
        canvas->setBrushHardness(1);
        canvas->setBrushOpacity(1);
        canvas->setForeground(Qt::red);
        doc->markSaved();
        const int historyBefore = doc->historyNames().size();
        const quint64 layerId = doc->activeLayer()->id;
        const QPoint start = canvas->fromDocument({140, 100}).toPoint(),
                     end = canvas->fromDocument({180, 100}).toPoint();
        QVERIFY(canvas->rect().contains(start));
        QVERIFY(canvas->rect().contains(end));
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(canvas, end, 5);
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, end);
        QCOMPARE(doc->historyNames().size(), historyBefore + 1);
        QVERIFY(doc->isModified());
        QVERIFY(doc->state.layers[doc->indexForId(layerId)]
                    .pixels.region({140, 100, 1, 1})
                    .pixelColor(0, 0)
                    .red() > 200);
        canvas->setFocus();
        QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier);
        QCoreApplication::processEvents();
        QCOMPARE(doc->historyNames().size(), historyBefore);
        QVERIFY(doc->state.layers[doc->indexForId(layerId)].pixels.empty());
        QVERIFY(!doc->isModified());
        QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCoreApplication::processEvents();
        QCOMPARE(doc->historyNames().size(), historyBefore + 1);
        QVERIFY(!doc->state.layers[doc->indexForId(layerId)].pixels.empty());
    }
    void layerVisibilityAndReorderCommands() {
        MainWindow window;
        show(window);
        window.openDemo();
        Document *doc = window.currentDocument();
        doc->resizeImage({160, 100}, Qt::FastTransformation);
        window.runCommand("New Layer");
        const quint64 topId = doc->activeLayer()->id;
        const int original = doc->state.activeIndex;
        auto *tree = window.findChild<QTreeWidget *>("layersTree");
        QVERIFY(tree);
        QTRY_VERIFY(tree->topLevelItemCount() > 1);
        QTRY_COMPARE(tree->topLevelItem(0)->data(1, Qt::UserRole).toULongLong(), topId);
        // Clicking the eye hides the layer, clicking the empty eye cell shows it again.
        const int historyBefore = doc->historyNames().size();
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, eyePoint(tree, topId));
        QVERIFY(!doc->state.layers[doc->indexForId(topId)].visible);
        QTRY_VERIFY(itemFor(tree, topId) && itemFor(tree, topId)->icon(0).isNull());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, eyePoint(tree, topId));
        QVERIFY(doc->state.layers[doc->indexForId(topId)].visible);
        QTRY_VERIFY(itemFor(tree, topId) && !itemFor(tree, topId)->icon(0).isNull());
        QCOMPARE(doc->historyNames().size(), historyBefore + 2);
        window.runCommand("Undo");
        QVERIFY(!doc->state.layers[doc->indexForId(topId)].visible);
        window.runCommand("Undo");
        QVERIFY(doc->state.layers[doc->indexForId(topId)].visible);
        doc->setActiveIndex(doc->indexForId(topId));
        window.runCommand("Send Backward");
        QCOMPARE(doc->indexForId(topId), original - 1);
        window.runCommand("Undo");
        QCOMPARE(doc->indexForId(topId), original);
    }
    void eyeStrokeAndSoloFollowPhotoshop() {
        MainWindow window;
        show(window);
        window.openDemo();
        Document *doc = window.currentDocument();
        doc->resizeImage({160, 100}, Qt::FastTransformation);
        auto *tree = window.findChild<QTreeWidget *>("layersTree");
        QTRY_VERIFY(tree->topLevelItemCount() > 2);
        const quint64 first = LayerTree::idOf(tree->topLevelItem(0));
        const quint64 second = LayerTree::idOf(tree->topLevelItem(1));
        const quint64 third = LayerTree::idOf(tree->topLevelItem(2));
        // Dragging down the eye column hides every row crossed, in a single history step.
        const int historyBefore = doc->historyNames().size();
        QTest::mousePress(tree->viewport(), Qt::LeftButton, {}, eyePoint(tree, first));
        QTest::mouseMove(tree->viewport(), eyePoint(tree, second));
        QTest::mouseMove(tree->viewport(), eyePoint(tree, third));
        QTest::mouseRelease(tree->viewport(), Qt::LeftButton, {}, eyePoint(tree, third));
        for (auto id : {first, second, third})
            QVERIFY(!doc->state.layers[doc->indexForId(id)].visible);
        QCOMPARE(doc->historyNames().size(), historyBefore + 1);
        window.runCommand("Undo");
        for (auto id : {first, second, third})
            QVERIFY(doc->state.layers[doc->indexForId(id)].visible);
        // Alt-click shows only that layer; Alt-click again restores the previous visibility.
        doc->mutate("Hide", [&] { doc->state.layers[doc->indexForId(third)].visible = false; });
        QTRY_VERIFY(itemFor(tree, second) && itemFor(tree, third)->icon(0).isNull());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::AltModifier, eyePoint(tree, second));
        for (const auto &layer : doc->state.layers)
            QCOMPARE(layer.visible, layer.id == second);
        QTRY_VERIFY(itemFor(tree, first) && itemFor(tree, first)->icon(0).isNull());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::AltModifier, eyePoint(tree, second));
        QVERIFY(doc->state.layers[doc->indexForId(first)].visible);
        QVERIFY(!doc->state.layers[doc->indexForId(third)].visible);
        // Clicking an eye never selects the row or opens the rename editor.
        doc->setActiveIndex(doc->indexForId(first));
        QTRY_VERIFY(itemFor(tree, third) && !itemFor(tree, first)->icon(0).isNull());
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton, {}, eyePoint(tree, third));
        QCOMPARE(doc->activeLayer()->id, first);
    }
    void dragDropReordersAndNestsLayers() {
        MainWindow window;
        show(window);
        window.openDemo();
        Document *doc = window.currentDocument();
        doc->resizeImage({160, 100}, Qt::FastTransformation);
        auto *tree = static_cast<LayerTree *>(window.findChild<QTreeWidget *>("layersTree"));
        QTRY_VERIFY(tree->topLevelItemCount() > 2);
        const auto drop = [&](QVector<quint64> ids, quint64 target, LayerTree::DropPlacement placement) {
            tree->layersDropped(ids, target, placement);
            QCoreApplication::processEvents();
        };
        auto top = stack(doc);
        const quint64 bottom = top.first(), highest = top.last();
        // Dragging the bottom layer above the top layer moves it to the top of the stack.
        drop({bottom}, highest, LayerTree::DropPlacement::Above);
        QCOMPARE(stack(doc).last(), bottom);
        QCOMPARE(doc->historyNames().last(), QString("Reorder layers"));
        // Dropping below a layer places it directly underneath.
        drop({bottom}, highest, LayerTree::DropPlacement::Below);
        top = stack(doc);
        QCOMPARE(top[top.size() - 2], bottom);
        QCOMPARE(top.last(), highest);
        // Dropping onto empty space sends it to the bottom.
        drop({bottom}, 0, LayerTree::DropPlacement::Bottom);
        QCOMPARE(stack(doc).first(), bottom);
        window.runCommand("Undo");
        top = stack(doc);
        QCOMPARE(top[top.size() - 2], bottom);
        // Groups accept drops onto the row; ordinary layers never adopt children.
        doc->setActiveIndex(doc->indexForId(highest));
        window.runCommand("Group Layers");
        quint64 group = 0;
        for (const auto &layer : doc->state.layers)
            if (layer.kind == LayerKind::Group)
                group = layer.id;
        QVERIFY(group);
        drop({bottom}, group, LayerTree::DropPlacement::Inside);
        QCOMPARE(doc->state.layers[doc->indexForId(bottom)].parentId, group);
        QCOMPARE(stack(doc, group).last(), bottom);
        const quint64 other = stack(doc).first();
        drop({other}, bottom, LayerTree::DropPlacement::Inside);
        QCOMPARE(doc->state.layers[doc->indexForId(other)].parentId, group);
        QCOMPARE(doc->state.layers[doc->indexForId(bottom)].parentId, group);
        QTRY_VERIFY(itemFor(tree, bottom) && itemFor(tree, bottom)->parent());
        QVERIFY(!(itemFor(tree, bottom)->flags() & Qt::ItemIsDropEnabled));
        QVERIFY(itemFor(tree, group)->flags() & Qt::ItemIsDropEnabled);
        // Dropping a group inside its own child is refused without changing anything.
        const auto before = doc->historyNames().size();
        drop({group}, bottom, LayerTree::DropPlacement::Above);
        QCOMPARE(doc->historyNames().size(), before);
        // If the platform reports a refused drop as a move, Qt deletes the dragged row itself;
        // the end of every drag rebuilds the panel so the row comes back.
        delete itemFor(tree, other);
        QVERIFY(!itemFor(tree, other));
        tree->dragFinished();
        QTRY_VERIFY(itemFor(tree, other));
    }
    void layerPanelUsesIconsNotLetters() {
        MainWindow window;
        show(window);
        window.openDemo();
        for (const auto &name : QStringList{"fx", "mask", "adjustment", "folder", "new", "trash"}) {
            auto *button = window.findChild<QToolButton *>("layerFooter_" + name);
            QVERIFY2(button && !button->icon().isNull() && button->text().isEmpty(), qPrintable(name));
        }
        for (const auto &name : QStringList{"lockAlpha", "lockPixels", "lockPosition"})
            QVERIFY(!window.findChild<QToolButton *>(name)->icon().isNull());
        for (const auto &name : adjustmentNames().mid(0, 16)) {
            auto *button = window.findChild<QToolButton *>("adjustment_" + name);
            QVERIFY2(button && !button->icon().isNull(), qPrintable(name));
        }
        // Icons are drawn as vectors at the requested scale rather than resampled from one bitmap.
        const QPixmap crisp = toolIcon("layer:eye").pixmap(QSize(16, 16), 1.5);
        QCOMPARE(crisp.size(), QSize(24, 24));
    }
    void primaryShortcutsAndToolButtonsAreRegistered() {
        MainWindow window;
        show(window);
        for (const auto &name : QStringList{"Brush", "Move", "Rectangular Marquee", "Horizontal Type",
                                            "Eraser", "Crop", "Gradient", "Eyedropper", "Clone Stamp"})
            QVERIFY2(window.findChild<QToolButton *>("tool_" + name), qPrintable(name));
        bool undo = false, save = false, open = false;
        for (const auto *action : window.findChildren<QAction *>()) {
            if (action->text() == "Undo")
                undo = action->shortcut() == QKeySequence::Undo;
            if (action->text() == "Save")
                save = action->shortcut() == QKeySequence::Save;
            if (action->text() == "Open...")
                open = action->shortcut() == QKeySequence::Open;
        }
        QVERIFY(undo);
        QVERIFY(save);
        QVERIFY(open);
    }
};
QTEST_MAIN(UiTests)
#include "ui_tests.moc"
