#include "MainWindow.h"
#include <QTabWidget>

namespace serika {
QList<Document *> MainWindow::openDocuments() const {
    QList<Document *> documents;
    for (int index = 0; index < m_tabs->count(); ++index)
        if (auto *canvas = m_tabs->widget(index)->findChild<CanvasView *>())
            documents.append(canvas->document());
    return documents;
}
void MainWindow::adoptDocument(Document *document) {
    addDocument(document);
    refresh();
}
bool MainWindow::closeDocumentFor(Document *document, bool discard, QString *error) {
    for (int index = 0; index < m_tabs->count(); ++index) {
        auto *canvas = m_tabs->widget(index)->findChild<CanvasView *>();
        if (!canvas || canvas->document() != document)
            continue;
        // An agent never triggers the Save prompt: unsaved work stays open unless it explicitly discards.
        if (document->isModified() && !discard) {
            *error = "Document has unsaved edits; save it or explicitly set discard=true.";
            return false;
        }
        document->markSaved();
        return closeDocument(index);
    }
    *error = "That document is no longer open.";
    return false;
}
bool MainWindow::focusDocument(Document *document) {
    for (int index = 0; index < m_tabs->count(); ++index)
        if (auto *canvas = m_tabs->widget(index)->findChild<CanvasView *>();
            canvas && canvas->document() == document) {
            m_tabs->setCurrentIndex(index);
            raise();
            activateWindow();
            return true;
        }
    return false;
}
void MainWindow::setAgentStatus(const QString &text) {
    if (!text.isEmpty())
        showMessage(text);
}
} // namespace serika
