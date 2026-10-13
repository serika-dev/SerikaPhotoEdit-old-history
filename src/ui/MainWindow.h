#pragma once
#include "document/Document.h"
#include "ui/canvas/CanvasView.h"
#include "ui/shortcuts/ShortcutRegistry.h"
#include <QHash>
#include <QJsonArray>
#include <QMainWindow>
#include <QPointer>
#include <QSettings>
#include <QTimer>
class QTabWidget;
class QTreeWidget;
class QListWidget;
class QStackedWidget;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QLabel;
class QToolBar;
class QToolButton;
class QCheckBox;
namespace serika {
class BrushSettingsWidget;
enum class SubjectEngine;
class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(QWidget *parent = nullptr);
    Document *currentDocument() const;
    CanvasView *currentCanvas() const;
    void openFile(const QString &path);
    void newDocument();
    void openDemo();
    void applyTheme(const QString &name);
    void setWorkspace(const QString &name, bool reset = false);
    QStringList dockNames() const;
    void runCommand(const QString &name);
    void saveWorkspace();
    ShortcutRegistry *shortcutRegistry() const { return m_shortcuts; }
    // Live MCP sessions (mcp/McpLiveBridge) work on the documents in this window's tabs.
    QList<Document *> openDocuments() const;
    void adoptDocument(Document *document);
    bool closeDocumentFor(Document *document, bool discard, QString *error);
    bool focusDocument(Document *document);
    // Never shows a dialog: a note in the document status line, plus a title-bar marker while connected.
    void setAgentStatus(const QString &text);
    void setAgentConnected(bool connected);

  protected:
    void closeEvent(QCloseEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;

  private:
    QStackedWidget *m_center = nullptr;
    QWidget *m_home = nullptr;
    QTabWidget *m_tabs = nullptr;
    QToolBar *m_options = nullptr;
    QDockWidget *m_toolDock = nullptr;
    QWidget *m_toolWidget = nullptr;
    QHash<QString, QDockWidget *> m_docks;
    QHash<QString, QAction *> m_commands;
    QHash<QString, QToolButton *> m_tools;
    ShortcutRegistry *m_shortcuts = nullptr;
    QHash<QString, QString> m_lastToolInGroup;
    QElapsedTimer m_opacityTimer;
    QString m_opacityDigits;
    QString m_opacityTarget;
    QHash<QString, QPointer<CanvasView>> m_holdCanvases;
    QTreeWidget *m_layers = nullptr;
    QComboBox *m_blend = nullptr;
    QSpinBox *m_layerOpacity = nullptr;
    QSpinBox *m_layerFill = nullptr;
    QListWidget *m_history = nullptr;
    QListWidget *m_actionsList = nullptr;
    QLabel *m_info = nullptr;
    QLabel *m_histogram = nullptr;
    QLabel *m_navigator = nullptr;
    QWidget *m_properties = nullptr;
    BrushSettingsWidget *m_brushSettings = nullptr;
    QLabel *m_fgSwatch = nullptr;
    QColor m_foreground = QColor("#8B5CF6");
    QColor m_background = Qt::white;
    QString m_tool = "Move";
    QString m_theme = "Dark";
    QString m_workspace = "Essentials";
    QString m_lastFilter = "Gaussian Blur";
    QJsonObject m_lastFilterParameters;
    QStringList m_recent;
    QStringList m_recorded;
    QJsonArray m_actionSteps;
    bool m_recording = false;
    bool m_refreshing = false;
    bool m_editing = false;
    bool m_twoColumns = false;
    bool m_panelsHidden = false;
    bool m_docksHidden = false;
    bool m_rulers = true;
    bool m_grid = false;
    bool m_guides = true;
    bool m_quickMask = false;
    bool m_agentConnected = false;
    int m_screenMode = 0;
    QTimer m_recovery;
    QTimer m_refreshTimer;
    QSettings m_settings;
    QByteArray m_essentialState;
    QHash<QString, bool> m_visibilityBeforeHide;
    // Layer visibility saved by Alt-clicking an eye, keyed by document address and soloed layer id.
    QHash<QString, QHash<quint64, bool>> m_soloVisibility;
    QPointer<QWidget> m_draggedPage;
    void detachDocument(int index);
    void buildMenus();
    void initializeLayerOperations();
    bool runLayerOperation(const QString &name);
    QVector<quint64> selectedLayerIds() const;
    void selectLayerIds(const QVector<quint64> &ids);
    void updateLayerOperationUi();
    void initializeShortcuts();
    void activateShortcut(const ShortcutEntry &entry, bool released);
    void updateShortcutLabels();
    void buildCropOptions();
    void applyCropOptions();
    void addMaskProperties(Document *document, quint64 layerId, bool vector);
    void addSmartFilterProperties(Document *document, quint64 layerId);
    void buildHome();
    void buildTools();
    void buildOptions();
    void buildDocks();
    void buildBrushSettings();
    void syncBrushSettings(const BrushPreset &preset);
    void buildLayers(QWidget *parent);
    QAction *command(QMenu *menu, const QString &name, const QKeySequence &shortcut = {});
    QDockWidget *dock(const QString &name, QWidget *widget, bool visible = false);
    void addDocument(Document *document);
    bool closeDocument(int index);
    bool saveDocument(bool saveAs = false, bool copy = false);
    bool runSmartObjectCommand(const QString &name);
    bool runTypographyCommand(const QString &name);
    bool runProofingCommand(const QString &name);
    bool isSmartObjectContents(const Document *document) const;
    bool saveSmartObjectContents(Document *document, bool exportCopy = false);
    void refresh();
    void refreshLayers();
    void refreshProperties();
    void selectTool(const QString &tool);
    void adjust(const QString &name, bool destructive = false);
    void editFill(const QString &command);
    void filter(const QString &name, bool repeat = false);
    void develop(Document *document);
    void preferences(const QString &initialPage = {});
    // Select Subject / Remove Background with the Fast or local AI engine (MainWindowAi.cpp).
    bool runSubjectCommand(const QString &name);
    QImage computeSubject(const QImage &composite, SubjectEngine engine, QString *error);
    bool downloadAiModel();
    void transformDialog();
    void layerStyles();
    void exportAs();
    void batchDialog();
    void palette();
    void keyboardShortcuts();
    void showImportReport(Document *document);
    void addRecent(const QString &path);
    void showMessage(const QString &text);
    void renameLayer();
    void selectAndMask();
    void saveRecovery();
    void setPanelsVisible(bool toolbarAlso);
    void recordStep(const QString &command, const QString &name = {}, const QJsonObject &parameters = {});
};
} // namespace serika
