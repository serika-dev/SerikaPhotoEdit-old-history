#include "MainWindow.h"
#include "actions/ActionRunner.h"
#include "compositor/GpuProcessor.h"
#include "document/TransformOperations.h"
#include "io/FormatIO.h"
#include "tools/LocalAlgorithms.h"
#include "ui/dialogs/LiquifyDialog.h"
#include "ui/dialogs/SmartFilterDialog.h"
#include "ui/dialogs/TransformDialog.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QColorDialog>
#include <QColorSpace>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPageLayout>
#include <QPrintDialog>
#include <QPrinter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
namespace serika {
static QImage alphaImage(const QImage &selection) {
    QImage image(selection.size(), QImage::Format_RGBA32FPx4);
    image.fill(Qt::transparent);
    for (int y = 0; y < image.height(); y++) {
        auto *row = reinterpret_cast<float *>(image.scanLine(y));
        for (int x = 0; x < image.width(); x++)
            row[x * 4 + 3] = float(maskSample(selection, x, y));
    }
    return image;
}
void MainWindow::runCommand(const QString &name) {
    auto *d = currentDocument();
    auto *c = currentCanvas();
    if (runTypographyCommand(name) || runProofingCommand(name))
        return;
    if (name == "Contextual Task Bar") {
        // Settings are the source of truth so the menu, palette and shortcuts all toggle alike.
        const bool on = !m_settings.value("contextTaskBar", false).toBool();
        m_settings.setValue("contextTaskBar", on);
        m_commands[name]->setChecked(on);
        for (auto *bar : findChildren<QFrame *>("contextBar"))
            bar->setVisible(on);
        return;
    }
    if (name == "GPU Diagnostics") {
        QMessageBox::information(this, "GPU processing", GpuProcessor::instance().diagnostics().summary());
        return;
    }
    if (m_recording && d &&
        QStringList{"New Layer", "Merge Down", "Flatten Image", "Merge Visible", "Convert to Smart Object",
                    "Rasterize Layer", "Add Layer Mask", "Delete Layer Mask", "Create Clipping Mask",
                    "Flip Horizontal", "Flip Vertical", "All", "Deselect", "Inverse"}
            .contains(name))
        recordStep(name);
    if (runLayerOperation(name))
        return;
    if (runSubjectCommand(name))
        return;
    if (name == "Super Resolution..." || name == "Upscale Layer...") {
        superResolution(name == "Upscale Layer...");
        return;
    }
    if (runSmartObjectCommand(name))
        return;
    if (name == "New...") {
        newDocument();
        return;
    }
    if (name == "Open..." || name == "Open As...") {
        auto path = QFileDialog::getOpenFileName(this, "Open image or document", {}, FormatIO::openFilter());
        if (!path.isEmpty())
            openFile(path);
        return;
    }
    if (name == "Browse in Folder...") {
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(d && !d->filePath.isEmpty()
                                    ? QFileInfo(d->filePath).absolutePath()
                                    : QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)));
        return;
    }
    if (name == "Exit") {
        close();
        return;
    }
    if (name == "Welcome Project") {
        openDemo();
        return;
    }
    if (name == "Preferences...") {
        preferences();
        return;
    }
    if (name == "Command Palette...") {
        palette();
        return;
    }
    if (name == "Keyboard Shortcuts..." || name == "Menus...") {
        keyboardShortcuts();
        return;
    }
    if (name.startsWith("Workspace: ")) {
        setWorkspace(name.mid(11));
        return;
    }
    if (name == "Reset Workspace") {
        setWorkspace(m_workspace, true);
        return;
    }
    if (name == "Save Workspace...") {
        saveWorkspace();
        showMessage("Saved " + m_workspace + " workspace.");
        return;
    }
    if (m_docks.contains(name)) {
        m_docks[name]->show();
        m_docks[name]->raise();
        return;
    }
    if (name == "Collapse Panels to Icons") {
        for (auto *dock : m_docks)
            if (dock->isVisible()) {
                bool collapsed = dock->minimumWidth() == 24;
                if (collapsed) {
                    dock->setMinimumWidth(240);
                    dock->setMaximumWidth(QWIDGETSIZE_MAX);
                    dock->widget()->show();
                } else {
                    dock->widget()->hide();
                    dock->setMinimumWidth(24);
                    dock->setMaximumWidth(28);
                }
            }
        return;
    }
    if (name == "About Serika PhotoEdit") {
        QMessageBox::about(
            this, "About Serika PhotoEdit",
            "<p><img src=\":/serika/logo.png\" width=\"112\" height=\"112\"></p>"
            "<h2>Serika PhotoEdit</h2><p>0.0.1 · Serika</p><p>Native photo editing. Layers, masks, "
            "RAW, PSD.</p><p>C++20 · Qt " +
                QString(qVersion()) +
                " · CPU compositor with optional GPU image processing</p><p>Original interface icons. MIT "
                "application source.</p>");
        return;
    }
    if (name == "Format Support") {
        QMessageBox::information(this, "Format support",
                                 "Read: " + FormatIO::readableFormats().join(", ") +
                                     "\n\nWrite: " + FormatIO::writableFormats().join(", ") + "\n\nLibRaw: " +
                                     (FormatIO::rawAvailable() ? "available" : "not linked in this build") +
                                     "\n\nPSD/PSB supports pixel layers, masks, groups and blend modes. "
                                     "Unsupported tagged blocks appear in the import report.\n\nSee "
                                     "docs/IMPLEMENTATION-STATUS.md for the complete feature inventory.");
        return;
    }
    if (name == "Keyboard Reference") {
        QMessageBox::information(
            this, "Keyboard reference",
            "V Move    M Marquee    L Lasso    W Selection\nC Crop    I Eyedropper    J Healing    B "
            "Brush\nS Clone    Y History    E Eraser    G Gradient\nO Dodge    P Pen    T Type    U Shape\nH "
            "Hand    R Rotate    Z Zoom\n\nSpace: temporary hand · Alt: sample / clone source\nX: swap "
            "colors · D: default colors · Q: quick mask\nF: screen modes · Tab: hide panels\nCtrl+Z: undo · "
            "Ctrl+Shift+Z: redo\nCtrl+0: fit · Ctrl+1: 100%\nCtrl+Shift+P: command palette\n\nHold a toolbar "
            "tool to open its flyout. Shift+tool key cycles variants.");
        return;
    }
    if (name == "3D Workspace Information") {
        QMessageBox::information(this, "3D workspace",
                                 "3D editing is not implemented in Serika PhotoEdit 0.0.1.");
        return;
    }
    if (name == "Neural Filters...") {
        QMessageBox::information(this, "Neural filters",
                                 "No on-device neural model is installed.\nSubject selection and background "
                                 "removal use a local color model.\nNo image is sent to a network service.");
        return;
    }
    if (name == "Screen Mode") {
        m_screenMode = (m_screenMode + 1) % 3;
        if (m_screenMode == 0) {
            showNormal();
            menuBar()->show();
            m_options->show();
            m_toolDock->show();
            setWorkspace(m_workspace);
        } else {
            showFullScreen();
            if (m_screenMode == 2) {
                menuBar()->hide();
                m_options->hide();
                m_toolDock->hide();
                for (auto *dock : m_docks)
                    dock->hide();
            }
        }
        return;
    }
    if (name == "Batch...") {
        batchDialog();
        return;
    }
    if (!d) {
        showMessage("Open an image or create a new document first.");
        return;
    }
    if (name == "Undo" || name == "Step Backward") {
        d->undo();
        return;
    }
    if (name == "Redo" || name == "Step Forward") {
        d->redo();
        return;
    }
    if (name == "Toggle Last State") {
        if (d->canRedo())
            d->redo();
        else
            d->undo();
        return;
    }
    if (name == "Save") {
        saveDocument();
        return;
    }
    if (name == "Save As...") {
        saveDocument(true);
        return;
    }
    if (name == "Save a Copy...") {
        saveDocument(true, true);
        return;
    }
    if (name == "Close") {
        closeDocument(m_tabs->currentIndex());
        return;
    }
    if (name == "Close All") {
        while (m_tabs->count())
            if (!closeDocument(m_tabs->count() - 1))
                break;
        return;
    }
    if (name == "Close Others") {
        auto *current = m_tabs->currentWidget();
        for (int i = m_tabs->count() - 1; i >= 0; i--)
            if (m_tabs->widget(i) != current && !closeDocument(i))
                break;
        return;
    }
    if (name == "Export As...") {
        exportAs();
        return;
    }
    if (name == "Quick Export PNG") {
        auto path = QFileDialog::getSaveFileName(
            this, "Quick Export PNG", QFileInfo(d->title).completeBaseName() + ".png", "PNG (*.png)");
        if (path.isEmpty())
            return;
        if (!d->composite().save(path, "PNG"))
            QMessageBox::warning(this, "Export failed", "The image could not be written.");
        return;
    }
    if (name == "Export Layers..." || name == "Image Assets") {
        auto folder = QFileDialog::getExistingDirectory(this, "Export layers to folder");
        if (folder.isEmpty())
            return;
        QStringList errors;
        for (const auto &l : d->state.layers) {
            if (l.kind == LayerKind::Group)
                continue;
            QString filename = l.name;
            filename.replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");
            if (!filename.endsWith(".png", Qt::CaseInsensitive))
                filename += ".png";
            if (!d->layerImage(l).save(folder + "/" + filename, "PNG"))
                errors << filename;
        }
        showMessage(errors.isEmpty() ? "Exported layer assets." : "Could not export: " + errors.join(", "));
        return;
    }
    if (name == "Package...") {
        auto folder = QFileDialog::getExistingDirectory(this, "Package document and linked assets");
        if (folder.isEmpty())
            return;
        QString error;
        if (!FormatIO::saveNative(d, folder + "/" + QFileInfo(d->title).completeBaseName() + ".spe",
                                  &error)) {
            QMessageBox::warning(this, "Package failed", error);
            return;
        }
        for (const auto &l : d->state.layers)
            if (!l.linkedPath.isEmpty())
                QFile::copy(l.linkedPath, folder + "/" + QFileInfo(l.linkedPath).fileName());
        showMessage("Packaged document and linked assets.");
        return;
    }
    if (name == "File Info...") {
        QDialog dialog(this);
        dialog.setWindowTitle("File Info");
        auto *v = new QVBoxLayout(&dialog);
        auto *edit = new QTextEdit;
        edit->setPlainText(QString::fromUtf8(QJsonDocument(d->state.metadata).toJson()));
        v->addWidget(new QLabel("Document metadata (JSON)"));
        v->addWidget(edit);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        v->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.resize(600, 430);
        if (dialog.exec() == QDialog::Accepted) {
            QJsonParseError error;
            auto json = QJsonDocument::fromJson(edit->toPlainText().toUtf8(), &error);
            if (error.error != QJsonParseError::NoError || !json.isObject()) {
                QMessageBox::warning(this, "Metadata", "Enter a valid JSON object.");
                return;
            }
            d->mutate("File info", [d, json] { d->state.metadata = json.object(); });
        }
        return;
    }
    if (name == "Place Embedded..." || name == "Place Linked...") {
        auto path = QFileDialog::getOpenFileName(this, "Place image", {}, FormatIO::openFilter());
        if (path.isEmpty())
            return;
        QString error;
        auto *source = FormatIO::open(path, &error);
        if (!source) {
            QMessageBox::warning(this, "Place failed", error);
            return;
        }
        QImage image = source->composite();
        delete source;
        d->mutate("Place image", [d, image, path, name] {
            d->addLayer(QFileInfo(path).fileName(), LayerKind::SmartObject);
            auto *l = d->activeLayer();
            if (image.width() > d->state.size.width() || image.height() > d->state.size.height())
                l->pixels.setImage(
                    image.scaled(d->state.size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            else
                l->pixels.setImage(image);
            if (name == "Place Linked...")
                l->linkedPath = path;
        });
        return;
    }
    if (name == "Print..." || name == "Print One Copy") {
        QPrinter printer(QPrinter::HighResolution);
        if (name == "Print...") {
            QPrintDialog dialog(&printer, this);
            if (dialog.exec() != QDialog::Accepted)
                return;
        }
        QPainter painter;
        if (!painter.begin(&printer)) {
            QMessageBox::warning(this, "Print", "Could not start the printer.");
            return;
        }
        auto rect = printer.pageLayout().paintRectPixels(printer.resolution());
        auto image = d->composite();
        QSize fit = image.size();
        fit.scale(rect.size(), Qt::KeepAspectRatio);
        painter.drawImage(
            QRect(QPoint(rect.center().x() - fit.width() / 2, rect.center().y() - fit.height() / 2), fit),
            image);
        painter.end();
        return;
    }
    if (name == "New Layer" || name == "New Layer...") {
        QString layerName;
        if (name.endsWith("...")) {
            bool ok;
            layerName = QInputDialog::getText(this, "New layer", "Name", QLineEdit::Normal,
                                              "Layer " + QString::number(d->state.layers.size() + 1), &ok);
            if (!ok)
                return;
        }
        d->addLayer(layerName);
        return;
    }
    if (name == "Layer via Copy") {
        if (!d->hasSelection()) {
            d->duplicateActiveLayer();
            return;
        }
        auto *sourceLayer = d->activeLayer();
        if (!sourceLayer)
            return;
        auto image = d->layerImage(*sourceLayer).convertToFormat(QImage::Format_RGBA32FPx4);
        const auto selectionOrigin = d->effectiveLayerOffset(*sourceLayer);
        for (int y = 0; y < image.height(); ++y) {
            auto *row = reinterpret_cast<float *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x)
                row[4 * x + 3] *= maskSample(d->state.selection, qRound(x + selectionOrigin.x()),
                                             qRound(y + selectionOrigin.y()));
        }
        const auto offset = sourceLayer->offset;
        const auto title = sourceLayer->name + " copy";
        image = image.convertToFormat(d->state.bitDepth == 32   ? QImage::Format_RGBA32FPx4
                                      : d->state.bitDepth == 16 ? QImage::Format_RGBA64
                                                                : QImage::Format_RGBA8888);
        d->mutate(name, [d, image, offset, title] {
            d->addLayer(title);
            d->activeLayer()->pixels.setImage(image);
            d->activeLayer()->offset = offset;
        });
        return;
    }
    if (name == "Duplicate Layer") {
        d->duplicateActiveLayer();
        return;
    }
    if (name == "Delete Layer") {
        d->removeActiveLayer();
        return;
    }
    if (name == "Rename Layer...") {
        renameLayer();
        return;
    }
    if (name.startsWith("Adjustment: ")) {
        adjust(name.mid(12));
        return;
    }
    if (adjustmentNames().contains(name)) {
        adjust(name, true);
        return;
    }
    if (name.startsWith("Filter: ")) {
        filter(name.mid(8));
        return;
    }
    if (name == "Last Filter") {
        filter(m_lastFilter, true);
        return;
    }
    auto *layer = d->activeLayer();
    if (!layer)
        return;
    if (name == "Add Layer Mask") {
        d->addMask();
        return;
    }
    if (name == "Delete Layer Mask" || name == "Apply Layer Mask") {
        d->removeMask(name == "Apply Layer Mask");
        return;
    }
    if (name == "Toggle Layer Mask") {
        d->mutate("Toggle mask", [d] { d->activeLayer()->maskEnabled = !d->activeLayer()->maskEnabled; });
        return;
    }
    if (name == "Invert Layer Mask") {
        d->invertMask();
        return;
    }
    if (name == "Load Selection from Layer Mask" || name == "Load Selection from Vector Mask") {
        d->loadMaskSelection(name.contains("Vector"));
        return;
    }
    if (name == "Add Vector Mask") {
        QPainterPath path = layer->shape;
        if (path.isEmpty()) {
            const QRect bounds = d->hasSelection() ? d->selectionBounds() : QRect(QPoint(), d->state.size);
            path.addRect(QRectF(bounds).translated(-d->effectiveLayerOffset(*layer)));
        }
        d->addVectorMask(path);
        return;
    }
    if (name == "Delete Vector Mask" || name == "Toggle Vector Mask") {
        d->mutate(name, [d, name] {
            if (name.startsWith("Delete"))
                d->activeLayer()->vectorMask = {};
            else
                d->activeLayer()->vectorMaskEnabled = !d->activeLayer()->vectorMaskEnabled;
        });
        return;
    }
    if (name == "Apply Vector Mask") {
        if (layer->vectorMask.isEmpty())
            return;
        Layer local = *layer;
        local.offset = {};
        const auto coverage =
            renderedLayerMask(local, d->layerImage(*layer).size(), d->state.bitDepth, false, true);
        d->mutate(name, [d, coverage] {
            auto *l = d->activeLayer();
            auto image = d->layerImage(*l).convertToFormat(QImage::Format_RGBA32FPx4);
            for (int y = 0; !coverage.isNull() && y < image.height(); ++y) {
                auto *row = reinterpret_cast<float *>(image.scanLine(y));
                for (int x = 0; x < image.width(); ++x)
                    row[4 * x + 3] *= maskSample(coverage, x, y);
            }
            image = image.convertToFormat(d->state.bitDepth == 32   ? QImage::Format_RGBA32FPx4
                                          : d->state.bitDepth == 16 ? QImage::Format_RGBA64
                                                                    : QImage::Format_RGBA8888);
            l->pixels.setImage(image);
            l->kind = LayerKind::Pixel;
            l->parameters.remove("contentTransform");
            l->smartFilters = {};
            l->vectorMask = {};
        });
        return;
    }
    if (name == "Delete Selected Content" || name == "Fill Foreground" || name == "Fill Background") {
        if (name == "Delete Selected Content" && !d->hasSelection() && !layer->maskTarget &&
            c->tool() == "Move") {
            d->removeActiveLayer();
            return;
        }
        QString error;
        QJsonObject step{
            {"command", name == "Delete Selected Content" ? "clear" : "fill"},
            {"parameters",
             QJsonObject{{"color",
                          (name == "Fill Background" ? m_background : m_foreground).name(QColor::HexArgb)}}}};
        if (!ActionRunner::execute(d, step, &error))
            showMessage(error);
        return;
    }
    if (name.startsWith("Select ") && name.endsWith(" Layer")) {
        const int target = name == "Select Previous Layer" ? d->state.activeIndex - 1
                           : name == "Select Next Layer"   ? d->state.activeIndex + 1
                           : name == "Select Top Layer"    ? d->state.layers.size() - 1
                           : name == "Select Bottom Layer" ? 0
                                                           : -1;
        if (target >= 0 && target < d->state.layers.size())
            d->setActiveIndex(target);
        return;
    }
    if (name == "Fit Selection") {
        c->fitSelection();
        return;
    }
    if (name == "Auto Crop Transparent" || name == "Auto Crop Content" || name == "Auto Straighten") {
        selectTool("Crop");
        const bool found = name == "Auto Crop Transparent" ? c->autoCropTransparent()
                           : name == "Auto Crop Content"   ? c->autoCropContent()
                                                           : c->autoStraighten();
        if (!found)
            showMessage("No crop boundary or straightening angle was detected.");
        return;
    }
    if (name == "Commit Crop") {
        c->commitCrop();
        return;
    }
    if (name == "Cancel Crop") {
        c->cancelCrop();
        return;
    }
    if (name == "Create Clipping Mask") {
        d->mutate("Clipping mask", [d] { d->activeLayer()->clipped = !d->activeLayer()->clipped; });
        return;
    }
    if (name == "Layer Style...") {
        layerStyles();
        return;
    }
    if (name == "Merge Down") {
        d->mergeDown();
        return;
    }
    if (name == "Flatten Image") {
        d->flatten();
        return;
    }
    if (name == "Merge Visible") {
        const auto image = d->composite();
        QSet<quint64> keep;
        for (const auto &candidate : d->state.layers) {
            bool visible = candidate.visible;
            quint64 parent = candidate.parentId;
            QSet<quint64> visited;
            while (parent && !visited.contains(parent)) {
                visited.insert(parent);
                const int index = d->indexForId(parent);
                if (index < 0)
                    break;
                visible = visible && d->state.layers[index].visible;
                parent = d->state.layers[index].parentId;
            }
            if (!visible) {
                keep.insert(candidate.id);
                keep.unite(visited);
            }
        }
        d->mutate("Merge visible", [&] {
            for (int index = int(d->state.layers.size()) - 1; index >= 0; --index)
                if (!keep.contains(d->state.layers[index].id))
                    d->state.layers.removeAt(index);
            d->state.activeIndex = int(d->state.layers.size()) - 1;
            d->addLayer("Merged visible");
            d->activeLayer()->parentId = 0;
            d->activeLayer()->pixels.setImage(image);
        });
        return;
    }
    if (name == "Stamp Visible") {
        auto image = d->composite();
        d->mutate("Stamp visible", [d, image] {
            d->addLayer("Stamp visible");
            d->activeLayer()->parentId = 0;
            d->activeLayer()->pixels.setImage(image);
        });
        return;
    }
    if (name == "Group Layers") {
        QVector<quint64> ids;
        for (auto *i : m_layers->selectedItems())
            ids << i->data(1, Qt::UserRole).toULongLong();
        if (!ids.contains(layer->id))
            ids = {layer->id};
        QSet<quint64> selected(ids.begin(), ids.end());
        QHash<quint64, QPointF> positions;
        quint64 commonParent = 0;
        bool first = true;
        for (quint64 id : ids) {
            const int index = d->indexForId(id);
            if (index < 0)
                continue;
            const auto &candidate = d->state.layers[index];
            quint64 parent = candidate.parentId;
            QSet<quint64> visited;
            bool nested = false;
            while (parent && !visited.contains(parent)) {
                visited.insert(parent);
                if (selected.contains(parent)) {
                    nested = true;
                    break;
                }
                const int ancestor = d->indexForId(parent);
                parent = ancestor < 0 ? 0 : d->state.layers[ancestor].parentId;
            }
            if (nested)
                continue;
            positions[id] = d->effectiveLayerOffset(candidate);
            commonParent = first ? candidate.parentId : commonParent == candidate.parentId ? commonParent : 0;
            first = false;
        }
        d->mutate("Group layers", [d, positions, commonParent] {
            d->addLayer("Group " + QString::number(d->state.layers.size()), LayerKind::Group);
            quint64 group = d->activeLayer()->id;
            d->activeLayer()->parentId = commonParent;
            d->activeLayer()->blendMode = "Pass Through";
            const auto origin = d->effectiveLayerOffset(*d->activeLayer());
            for (auto id : positions.keys()) {
                int i = d->indexForId(id);
                if (i >= 0) {
                    d->state.layers[i].parentId = group;
                    d->state.layers[i].offset = positions[id] - origin;
                }
            }
        });
        return;
    }
    if (name == "Ungroup Layers") {
        d->mutate("Ungroup", [d] {
            auto *l = d->activeLayer();
            quint64 id = l->kind == LayerKind::Group ? l->id : l->parentId;
            if (!id)
                return;
            quint64 parent = 0;
            int group = d->indexForId(id);
            if (group >= 0)
                parent = d->state.layers[group].parentId;
            const int ancestor = d->indexForId(parent);
            const QPointF parentOrigin =
                ancestor < 0 ? QPointF() : d->effectiveLayerOffset(d->state.layers[ancestor]);
            QHash<quint64, QPointF> origins;
            for (const auto &child : d->state.layers)
                if (child.parentId == id)
                    origins[child.id] = d->effectiveLayerOffset(child);
            for (auto &child : d->state.layers)
                if (child.parentId == id) {
                    child.parentId = parent;
                    child.offset = origins[child.id] - parentOrigin;
                }
            if (group >= 0)
                d->state.layers.removeAt(group);
            d->state.activeIndex = std::max(0, int(d->state.layers.size()) - 1);
        });
        return;
    }
    if (name == "Bring Forward" || name == "Send Backward" || name == "Bring to Front" ||
        name == "Send to Back") {
        int current = d->state.activeIndex;
        int target = name == "Bring Forward"    ? current + 1
                     : name == "Send Backward"  ? current - 1
                     : name == "Bring to Front" ? int(d->state.layers.size()) - 1
                                                : 0;
        d->moveLayer(current, std::clamp(target, 0, int(d->state.layers.size()) - 1));
        return;
    }
    if (name == "Convert to Smart Object" || name == "Convert for Smart Filters") {
        d->convertToSmartObject();
        return;
    }
    if (name == "Edit Smart Filters...") {
        if (layer->kind != LayerKind::SmartObject || layer->locked)
            return;
        SmartFilterDialog dialog(d, this);
        if (dialog.exec() == QDialog::Accepted)
            d->mutate("Edit Smart Filters", [&] { d->activeLayer()->smartFilters = dialog.filters(); });
        return;
    }
    if (name == "Rasterize Layer") {
        d->mutate("Rasterize", [d] {
            auto *l = d->activeLayer();
            l->pixels.setImage(d->layerImage(*l));
            l->kind = LayerKind::Pixel;
            l->smartFilters = {};
            l->embeddedDocument.clear();
            l->parameters.remove("contentTransform");
        });
        return;
    }
    if (name == "Update Linked Content") {
        if (layer->linkedPath.isEmpty()) {
            showMessage("The active layer has no linked file.");
            return;
        }
        QString error;
        auto *source = FormatIO::open(layer->linkedPath, &error);
        if (!source) {
            QMessageBox::warning(this, "Linked content", error);
            return;
        }
        auto image = source->composite();
        delete source;
        d->mutate("Update linked content", [d, image] { d->activeLayer()->pixels.setImage(image); });
        return;
    }
    if (name == "Solid Color..." || name == "Gradient Fill..." || name == "Pattern Fill..." ||
        name == "Edit Fill...") {
        editFill(name);
        return;
    }
    if (name == "All") {
        d->selectAll();
        return;
    }
    if (name == "Deselect") {
        d->deselect();
        return;
    }
    if (name == "Reselect") {
        if (d->reselect())
            return;
        QImage mask;
        mask.loadFromData(QByteArray::fromBase64(d->state.metadata["lastSelection"].toString().toLatin1()),
                          "PNG");
        if (!mask.isNull())
            d->setSelection(mask);
        return;
    }
    if (name == "Inverse") {
        d->invertSelection();
        return;
    }
    if (name == "All Layers") {
        m_layers->selectAll();
        return;
    }
    if (name == "Focus Area...") {
        d->setSelection(selectFocusAreaLocally(d->composite()));
        selectAndMask();
        return;
    }
    if (name == "Select and Mask...") {
        selectAndMask();
        return;
    }
    if (name == "New Selection from Layer") {
        auto image = d->layerImage(*layer);
        const auto origin = d->effectiveLayerOffset(*layer);
        QImage mask = makeMask(d->state.size, d->state.bitDepth);
        for (int y = 0; y < image.height(); y++)
            for (int x = 0; x < image.width(); x++) {
                int sx = qRound(x + origin.x()), sy = qRound(y + origin.y());
                if (mask.rect().contains(sx, sy))
                    setMaskSample(mask, sx, sy, image.pixelColor(x, y).alphaF());
            }
        d->setSelection(mask);
        return;
    }
    if (name == "Save Selection...") {
        if (!d->hasSelection())
            return;
        bool ok;
        auto n =
            QInputDialog::getText(this, "Save selection", "Channel name", QLineEdit::Normal, "Alpha 1", &ok);
        if (!ok || n.isEmpty())
            return;
        d->saveSelection(n);
        return;
    }
    if (name == "Load Selection...") {
        const auto names = d->savedSelectionNames();
        if (names.isEmpty()) {
            showMessage("No saved selection channels.");
            return;
        }
        bool ok;
        const auto n = QInputDialog::getItem(this, "Load selection", "Channel", names, 0, false, &ok);
        if (ok)
            d->loadSelection(n);
        return;
    }
    if (name == "Color Range...") {
        auto target = QColorDialog::getColor(m_foreground, this, "Color range");
        if (!target.isValid())
            return;
        bool ok;
        int tolerance = QInputDialog::getInt(this, "Color range", "Fuzziness", 40, 1, 255, 1, &ok);
        if (!ok)
            return;
        auto image = d->composite();
        QImage mask(image.size(), QImage::Format_Grayscale8);
        for (int y = 0; y < image.height(); y++)
            for (int x = 0; x < image.width(); x++) {
                auto p = image.pixelColor(x, y);
                int distance =
                    std::max({std::abs(p.red() - target.red()), std::abs(p.green() - target.green()),
                              std::abs(p.blue() - target.blue())});
                mask.scanLine(y)[x] = uchar(std::clamp((tolerance - distance) * 255 / tolerance, 0, 255));
            }
        d->setSelection(mask);
        return;
    }
    if (QStringList{"Feather...", "Expand...", "Contract...", "Smooth...", "Border...", "Grow", "Similar"}
            .contains(name)) {
        if (!d->hasSelection())
            return;
        bool ok = true;
        int r = name == "Grow" || name == "Similar"
                    ? 4
                    : QInputDialog::getInt(this, name, "Radius (px)", 3, 1, 100, 1, &ok);
        if (!ok)
            return;
        QJsonObject params;
        if (name == "Feather...")
            params["feather"] = r;
        else if (name == "Smooth...")
            params["smooth"] = r;
        else
            params["shiftEdge"] = name == "Contract..." ? -r : r;
        if (name == "Border...") {
            const auto outer = refineMask(d->state.selection, QJsonObject{{"shiftEdge", r}}),
                       inner = refineMask(d->state.selection, QJsonObject{{"shiftEdge", -r}});
            auto result = makeMask(d->state.size, d->state.bitDepth);
            for (int y = 0; y < result.height(); ++y)
                for (int x = 0; x < result.width(); ++x)
                    setMaskSample(result, x, y, qMax(0., maskSample(outer, x, y) - maskSample(inner, x, y)));
            d->setSelection(result);
        } else
            d->refineSelection(params);
        return;
    }
    if (name == "Fill...") {
        auto color = QColorDialog::getColor(m_foreground, this, "Fill");
        if (color.isValid())
            c->fillSelection(color);
        return;
    }
    if (name == "Stroke...") {
        bool ok;
        int w = QInputDialog::getInt(this, "Stroke selection", "Width (px)", 2, 1, 1000, 1, &ok);
        if (ok)
            c->strokeSelection(m_foreground, w);
        return;
    }
    if (name == "Content-Aware Fill") {
        c->contentAwareFill();
        return;
    }
    if (name == "Copy" || name == "Copy Merged" || name == "Cut" || name == "Layer via Cut") {
        auto image = name == "Copy Merged" ? d->composite() : d->layerImage(*layer);
        QImage placed(d->state.size, image.format());
        placed.fill(Qt::transparent);
        QPainter p(&placed);
        p.drawImage(name == "Copy Merged" ? QPointF() : d->effectiveLayerOffset(*layer), image);
        if (d->hasSelection()) {
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.drawImage(0, 0, alphaImage(d->state.selection));
        }
        p.end();
        auto bounds = d->hasSelection() ? d->selectionBounds() : placed.rect();
        auto *mime = new QMimeData;
        mime->setImageData(placed.copy(bounds));
        mime->setData("application/x-serika-clipboard-origin",
                      QJsonDocument(QJsonObject{{"x", bounds.x()}, {"y", bounds.y()}}).toJson());
        qApp->clipboard()->setMimeData(mime);
        if (name == "Cut" || name == "Layer via Cut") {
            if (layer->locked || layer->lockAlpha)
                return;
            d->mutate("Cut", [d, bounds, placed, name] {
                auto *l = d->activeLayer();
                auto source = d->layerImage(*l);
                QPainter paint(&source);
                paint.setCompositionMode(QPainter::CompositionMode_DestinationOut);
                if (d->hasSelection())
                    paint.drawImage(-d->effectiveLayerOffset(*l), alphaImage(d->state.selection));
                else
                    paint.fillRect(source.rect(), Qt::black);
                paint.end();
                l->pixels.setImage(source);
                l->kind = LayerKind::Pixel;
                l->parameters.remove("contentTransform");
                l->smartFilters = {};
                if (name == "Layer via Cut") {
                    d->addLayer("Cut layer");
                    d->activeLayer()->pixels.setImage(placed.copy(bounds));
                    d->activeLayer()->offset = bounds.topLeft() - d->effectiveLayerOffset(*d->activeLayer());
                }
            });
        }
        return;
    }
    if (name == "Paste" || name == "Paste in Place" || name == "Paste Into" || name == "Paste Outside") {
        auto image = qApp->clipboard()->image();
        if (image.isNull()) {
            showMessage("The clipboard contains no image.");
            return;
        }
        const auto originData = QJsonDocument::fromJson(qApp->clipboard()->mimeData()->data(
                                                            "application/x-serika-clipboard-origin"))
                                    .object();
        const QPointF originalPosition(originData["x"].toDouble(), originData["y"].toDouble());
        d->mutate("Paste", [d, image, name, originalPosition] {
            auto selection = d->state.selection;
            d->addLayer("Pasted layer");
            auto *l = d->activeLayer();
            l->pixels.setImage(image);
            const auto parentOffset = d->effectiveLayerOffset(*l);
            if (name == "Paste in Place")
                l->offset = originalPosition - parentOffset;
            else
                l->offset = QPointF((d->state.size.width() - image.width()) / 2.,
                                    (d->state.size.height() - image.height()) / 2.) -
                            parentOffset;
            if (name == "Paste Into" || name == "Paste Outside") {
                const QPoint origin = d->effectiveLayerOffset(*l).toPoint();
                l->mask = makeMask(image.size(), d->state.bitDepth);
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x) {
                        const qreal coverage =
                            selection.isNull() ? 1 : maskSample(selection, x + origin.x(), y + origin.y());
                        setMaskSample(l->mask, x, y, name == "Paste Outside" ? 1 - coverage : coverage);
                    }
            }
        });
        return;
    }
    if (name == "Camera Raw Filter...") {
        develop(d);
        d->state.metadata.remove("developCancelled");
        return;
    }
    if (name == "Liquify..." || name == "Puppet Warp..." || name == "Warp...") {
        auto image = LiquifyDialog::edit(d->layerImage(*layer), this);
        if (!image.isNull())
            d->mutate(name, [d, image] {
                d->activeLayer()->pixels.setImage(image);
                d->activeLayer()->kind = LayerKind::Pixel;
                d->activeLayer()->parameters.remove("contentTransform");
                d->activeLayer()->smartFilters = {};
            });
        return;
    }
    if (name == "Free Transform..." || name == "Scale..." || name == "Rotate..." || name == "Distort..." ||
        name == "Skew..." || name == "Perspective...") {
        if (!c->beginTransform(name.left(name.size() - 3)))
            showMessage("Select an unlocked pixel, text, shape, or Smart Object layer to transform.");
        return;
    }
    if (name == "Warp Text...") {
        TransformDialog dialog(d->layerImage(*layer), name.left(name.size() - 3), this);
        if (dialog.exec() == QDialog::Accepted) {
            QString error;
            const auto matrix = dialog.resultTransform();
            if (!applyLayerTransform(d, matrix, &error))
                showMessage(error);
            else
                recordStep("transform", {}, QJsonObject{{"matrix", transformJson(matrix)}});
        }
        return;
    }
    if (name == "Transform Selection...") {
        if (!d->hasSelection())
            return;
        TransformDialog dialog(d->state.selection, "Transform Selection", this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        QImage result = makeMask(d->state.size, 32);
        QPainter p(&result);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setTransform(dialog.resultTransform());
        p.drawImage(QPoint(), d->state.selection);
        p.end();
        d->setSelection(normalizeMask(result, d->state.bitDepth));
        return;
    }
    if (name == "Flip Horizontal" || name == "Flip Vertical") {
        const auto extent =
            layer->maskTarget && !layer->mask.isNull() ? layer->mask.size() : d->layerImage(*layer).size();
        const bool horizontal = name == "Flip Horizontal";
        const auto transform =
            QTransform::fromScale(horizontal ? -1 : 1, horizontal ? 1 : -1) *
            QTransform::fromTranslate(horizontal ? extent.width() : 0, horizontal ? 0 : extent.height());
        QString error;
        if (!applyLayerTransform(d, transform, &error))
            showMessage(error);
        else
            recordStep("transform", {}, {{"matrix", transformJson(transform)}});
        return;
    }
    if (name == "Image Size..." || name == "Canvas Size...") {
        QDialog dialog(this);
        dialog.setWindowTitle(name);
        auto *v = new QVBoxLayout(&dialog);
        auto *f = new QFormLayout;
        auto *w = new QSpinBox;
        auto *h = new QSpinBox;
        w->setRange(1, 30000);
        h->setRange(1, 30000);
        w->setValue(d->state.size.width());
        h->setValue(d->state.size.height());
        auto *resample = new QComboBox;
        resample->addItems({"Smooth (bicubic)", "Nearest"});
        f->addRow("Width (px)", w);
        f->addRow("Height (px)", h);
        if (name == "Image Size...")
            f->addRow("Resample", resample);
        v->addLayout(f);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        v->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() == QDialog::Accepted) {
            QSize target(w->value(), h->value());
            if (qint64(target.width()) * target.height() > 80000000) {
                showMessage("Choose dimensions below 80 million pixels.");
                return;
            }
            if (name == "Image Size...")
                d->resizeImage(target, resample->currentIndex() == 1 ? Qt::FastTransformation
                                                                     : Qt::SmoothTransformation);
            else
                d->resizeCanvas(target, QPoint((target.width() - d->state.size.width()) / 2,
                                               (target.height() - d->state.size.height()) / 2));
        }
        return;
    }
    if (name == "Crop to Selection") {
        if (d->hasSelection())
            d->crop(d->selectionBounds());
        return;
    }
    if (name == "Trim") {
        auto image = d->composite();
        QRect bounds;
        for (int y = 0; y < image.height(); y++)
            for (int x = 0; x < image.width(); x++)
                if (image.pixelColor(x, y).alpha() > 0)
                    bounds = bounds.united(QRect(x, y, 1, 1));
        if (!bounds.isEmpty())
            d->crop(bounds);
        return;
    }
    if (name == "Reveal All") {
        QRect bounds(QPoint(), d->state.size);
        for (const auto &l : d->state.layers)
            bounds = bounds.united(QRect(l.offset.toPoint(), l.pixels.size));
        d->resizeCanvas(bounds.size(), -bounds.topLeft());
        return;
    }
    if (name == "180°" || name == "90° Clockwise" || name == "90° Counterclockwise") {
        int angle = name == "180°" ? 180 : name == "90° Clockwise" ? 90 : -90;
        d->mutate("Rotate image", [d, angle] {
            QTransform t;
            t.rotate(angle);
            QSize old = d->state.size;
            for (auto &l : d->state.layers) {
                l.pixels.setImage(d->layerImage(l).transformed(t));
                l.kind = LayerKind::Pixel;
                l.offset = {};
                if (!l.mask.isNull())
                    l.mask = l.mask.transformed(t);
            }
            if (!d->state.selection.isNull())
                d->state.selection = d->state.selection.transformed(t);
            if (std::abs(angle) == 90)
                d->state.size = QSize(old.height(), old.width());
        });
        c->fitToView();
        return;
    }
    if (name == "8 Bits/Channel" || name == "16 Bits/Channel" || name == "32 Bits/Channel") {
        int bits = name.section(' ', 0, 0).toInt();
        d->mutate("Bit depth", [d, bits] {
            auto format = bits == 16   ? QImage::Format_RGBA64
                          : bits == 32 ? QImage::Format_RGBA32FPx4
                                       : QImage::Format_RGBA8888;
            for (auto &l : d->state.layers)
                if (!l.pixels.empty())
                    l.pixels.setImage(l.pixels.image().convertToFormat(format));
            d->state.bitDepth = bits;
        });
        return;
    }
    if (name == "RGB") {
        d->mutate("RGB mode", [d] { d->state.colorMode = "RGB"; });
        return;
    }
    if (name == "Grayscale" || name == "Bitmap" || name == "Indexed") {
        d->mutate(name, [d, name] {
            for (auto &l : d->state.layers) {
                auto image = d->layerImage(l);
                image = applyAdjustment(image,
                                        name == "Bitmap"    ? "Threshold"
                                        : name == "Indexed" ? "Posterize"
                                                            : "Desaturate",
                                        QJsonObject{{"levels", 16}});
                l.pixels.setImage(image);
                l.kind = LayerKind::Pixel;
            }
            d->state.colorMode = name;
        });
        return;
    }
    if (name == "Lab" || name == "Multichannel") {
        QMessageBox::information(
            this, "Color mode",
            "This build edits RGB, Grayscale, Bitmap, and Indexed images. CMYK and Lab documents are "
            "imported through their RGB composite. Native CMYK/Lab editing is not implemented.");
        return;
    }
    if (name == "Horizontal Orientation" || name == "Vertical Orientation") {
        d->mutate("Type orientation",
                  [d, name] { d->activeLayer()->parameters["vertical"] = name.startsWith("Vertical"); });
        return;
    }
    if (name == "Fit on Screen") {
        c->fitToView();
        return;
    }
    if (name == "Zoom In") {
        c->setZoom(c->zoom() * 1.25);
        return;
    }
    if (name == "Zoom Out") {
        c->setZoom(c->zoom() / 1.25);
        return;
    }
    if (name == "100%" || name == "200%") {
        c->setZoom(name == "100%" ? 1 : 2);
        return;
    }
    if (name == "Flip Horizontal View") {
        c->flipView();
        return;
    }
    if (name == "Rulers") {
        m_rulers = !m_rulers;
        c->setShowRulers(m_rulers);
        return;
    }
    if (name == "Grid") {
        m_grid = !m_grid;
        c->setShowGrid(m_grid);
        return;
    }
    if (name == "Guides") {
        m_guides = !m_guides;
        c->setShowGuides(m_guides);
        return;
    }
    if (name == "Extras") {
        c->setProperty("showExtras",
                       c->property("showExtras").isValid() && !c->property("showExtras").toBool());
        c->update();
        return;
    }
    if (name == "New Guide...") {
        bool ok;
        auto orientation = QInputDialog::getItem(this, "New guide", "Orientation", {"Horizontal", "Vertical"},
                                                 0, false, &ok);
        if (!ok)
            return;
        double pos = QInputDialog::getDouble(this, "New guide", "Position (px)", 0, 0, 300000, 1, &ok);
        if (ok)
            d->mutate("New guide",
                      [d, pos, orientation] { d->state.guides.append({orientation == "Vertical", pos}); });
        return;
    }
    if (name == "Clear Guides") {
        d->mutate("Clear guides", [d] { d->state.guides.clear(); });
        return;
    }
    if (name == "Lock Guides" || name == "Snap") {
        d->state.metadata[name] = m_commands[name]->isChecked();
        return;
    }
    if (name == "Define Brush Preset..." || name == "Define Pattern...") {
        auto path = QFileDialog::getSaveFileName(
            this, name, {}, name == "Define Pattern..." ? "PNG (*.png)" : "Serika brush (*.spebrush)");
        if (path.isEmpty())
            return;
        if (name == "Define Pattern...")
            d->layerImage(*layer)
                .copy(d->hasSelection() ? d->selectionBounds() : d->layerImage(*layer).rect())
                .save(path, "PNG");
        else {
            QSaveFile file(path);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(
                    QJsonDocument(
                        QJsonObject{{"version", 1},
                                    {"size", m_options->findChild<QSpinBox *>("brushSize")->value()},
                                    {"hardness", m_options->findChild<QSpinBox *>("hardness")->value()},
                                    {"opacity", m_options->findChild<QSpinBox *>("brushOpacity")->value()}})
                        .toJson());
                file.commit();
            }
        }
        return;
    }
    if (name == "Fade...") {
        if (layer->kind != LayerKind::Pixel || layer->locked || !d->canUndo()) {
            showMessage("Fade is available after editing pixels on an unlocked raster layer.");
            return;
        }
        const auto previous = d->historyLayerImage(layer->id, int(d->historyNames().size()) - 2);
        const auto current = layer->pixels.image();
        if (previous.isNull() || previous.size() != current.size()) {
            showMessage("The last operation changed the layer dimensions and cannot be faded.");
            return;
        }
        bool ok;
        int opacity =
            QInputDialog::getInt(this, "Fade last pixel edit", "Effect opacity (%)", 100, 0, 100, 1, &ok);
        if (ok) {
            QImage result = current.convertToFormat(QImage::Format_RGBA32FPx4);
            const auto before = previous.convertToFormat(QImage::Format_RGBA32FPx4);
            const float amount = opacity / 100.f;
            for (int y = 0; y < result.height(); ++y) {
                auto *row = reinterpret_cast<float *>(result.scanLine(y));
                const auto *old = reinterpret_cast<const float *>(before.constScanLine(y));
                for (int x = 0; x < result.width(); ++x) {
                    const float oldAlpha = old[x * 4 + 3], newAlpha = row[x * 4 + 3];
                    const float alpha = oldAlpha * (1 - amount) + newAlpha * amount;
                    for (int channel = 0; channel < 3; ++channel)
                        row[x * 4 + channel] = alpha > 0 ? (old[x * 4 + channel] * oldAlpha * (1 - amount) +
                                                            row[x * 4 + channel] * newAlpha * amount) /
                                                               alpha
                                                         : 0;
                    row[x * 4 + 3] = alpha;
                }
            }
            d->mutate("Fade",
                      [&] { d->activeLayer()->pixels.setImage(result.convertToFormat(current.format())); });
        }
        return;
    }
    if (name == "Color Settings...") {
        auto path =
            QFileDialog::getOpenFileName(this, "Assign RGB ICC profile", {}, "ICC profiles (*.icc *.icm)");
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            auto bytes = f.readAll();
            auto cs = QColorSpace::fromIccProfile(bytes);
            if (!cs.isValid()) {
                QMessageBox::warning(this, "Profile", "The profile is invalid or unsupported by Qt.");
                return;
            }
            d->mutate("Assign profile", [d, bytes] { d->state.iccProfile = bytes; });
        }
        return;
    }
    if (name == "Purge History") {
        d->clearHistory();
        showMessage("Cleared undo and redo history.");
        return;
    }
    showMessage("Command is not implemented in this build: " + name);
}
void MainWindow::recordStep(const QString &command, const QString &name, const QJsonObject &parameters) {
    if (!m_recording)
        return;
    m_actionSteps.append(QJsonObject{{"command", command}, {"name", name}, {"parameters", parameters}});
    auto label = name.isEmpty() ? command : command + ": " + name;
    m_recorded.append(label);
    m_actionsList->addItem(label);
}
} // namespace serika
