#pragma once
#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPainter>
#include <QPainterPath>
#include <QVector>
#include <functional>

namespace serika {
enum class LayerKind {
    Pixel,
    Group,
    Text,
    Shape,
    Adjustment,
    SolidFill,
    GradientFill,
    SmartObject,
    Artboard,
    PatternFill
};
QStringList blendModeNames();
QStringList adjustmentNames();
struct TileImage {
    static constexpr int TileSize = 256;
    QSize size;
    QImage::Format format = QImage::Format_RGBA8888;
    QHash<quint64, QImage> tiles;
    static quint64 key(int x, int y) { return (quint64(quint32(x)) << 32) | quint32(y); }
    static TileImage fromImage(const QImage &image);
    QImage image() const;
    QImage region(const QRect &rect) const;
    void paint(const QRect &bounds, const std::function<void(QPainter &)> &draw);
    void setImage(const QImage &image);
    bool empty() const { return tiles.isEmpty(); }
};
struct Layer {
    quint64 id = 0;
    QString name = "Layer";
    LayerKind kind = LayerKind::Pixel;
    TileImage pixels;
    QImage mask;
    bool maskEnabled = true;
    bool maskTarget = false;
    qreal maskDensity = 1.0;
    qreal maskFeather = 0.0;
    bool maskLinked = true;
    QPointF maskOffset;
    QPainterPath vectorMask;
    bool vectorMaskEnabled = true;
    qreal vectorMaskDensity = 1.0;
    qreal vectorMaskFeather = 0.0;
    bool vectorMaskLinked = true;
    QPointF vectorMaskOffset;
    bool visible = true;
    bool locked = false;
    bool lockAlpha = false;
    bool lockPosition = false;
    bool clipped = false;
    qreal opacity = 1.0;
    qreal fill = 1.0;
    QString blendMode = "Normal";
    quint64 parentId = 0;
    QPointF offset;
    QString text;
    QFont font;
    QColor color = Qt::black;
    QPainterPath shape;
    QColor stroke = Qt::transparent;
    qreal strokeWidth = 1.0;
    QString adjustment;
    QJsonObject parameters;
    QJsonObject effects;
    QString linkedPath;
    // Native editable contents, or the last loaded snapshot of a linked original.
    QByteArray embeddedDocument;
    // Smart-object pixels remain the retained source. Filters are evaluated at render time.
    QJsonArray smartFilters;
};
struct Guide {
    bool vertical = false;
    qreal position = 0;
};
struct DocumentState {
    QSize size = QSize(1920, 1080);
    QVector<Layer> layers;
    int activeIndex = 0;
    QImage selection;
    QVector<Guide> guides;
    int bitDepth = 8;
    QString colorMode = "RGB";
    qreal resolution = 72;
    QByteArray iccProfile;
    QJsonObject metadata;
};
class Document : public QObject {
    Q_OBJECT
  public:
    explicit Document(QObject *parent = nullptr);
    DocumentState state;
    QString title = "Untitled";
    QString filePath;
    QStringList importReport;
    int historyLimit = 50;
    bool blendLinear = false;
    static Document *create(QSize size, QColor background, int bitDepth = 8, QObject *parent = nullptr);
    Layer *activeLayer();
    const Layer *activeLayer() const;
    int indexForId(quint64 id) const;
    QPointF effectiveLayerOffset(const Layer &layer) const;
    void setActiveIndex(int index);
    quint64 addLayer(QString name = QString(), LayerKind kind = LayerKind::Pixel);
    void removeActiveLayer();
    void duplicateActiveLayer();
    void moveLayer(int from, int to);
    void mergeDown();
    void flatten();
    void addMask();
    void removeMask(bool apply = false);
    void invertMask();
    void addVectorMask(const QPainterPath &path);
    void loadMaskSelection(bool vector = false, const QString &operation = "replace");
    void convertToSmartObject();
    void addSmartFilter(const QString &name, const QJsonObject &parameters = {});
    bool updateSmartFilter(int index, const QJsonObject &entry);
    bool removeSmartFilter(int index);
    void addAdjustment(const QString &name, const QJsonObject &params = {});
    void beginTransaction(const QString &name);
    void endTransaction();
    void cancelTransaction();
    void mutate(const QString &name, const std::function<void()> &operation);
    void touch();
    void undo();
    void redo();
    bool canUndo() const;
    bool canRedo() const;
    QStringList historyNames() const;
    void clearHistory();
    void markSaved();
    bool isModified() const;
    QImage composite() const;
    QImage layerImage(const Layer &layer) const;
    QRect selectionBounds() const;
    bool hasSelection() const;
    void setSelection(const QImage &mask, const QString &operation = "replace");
    void selectAll();
    void deselect();
    bool reselect();
    void invertSelection();
    void refineSelection(const QJsonObject &parameters);
    void saveSelection(const QString &name);
    bool loadSelection(const QString &name, const QString &operation = "replace");
    QStringList savedSelectionNames() const;
    QJsonArray layerComps() const;
    int captureLayerComp(const QString &name, bool visibility = true, bool position = true,
                         bool appearance = true);
    bool applyLayerComp(int index);
    bool deleteLayerComp(int index);
    QImage historyLayerImage(quint64 layerId, int historyIndex) const;
    bool restoreHistoryPixels(quint64 layerId, const QImage &coverage, int historyIndex);
    void resizeImage(QSize newSize, Qt::TransformationMode mode = Qt::SmoothTransformation);
    // Resizes with pixels already resampled elsewhere (e.g. AI super resolution), keyed by layer id;
    // other layers are scaled with `mode`. resizedLayerSize() gives the size each image must have.
    void resizeImage(QSize newSize, Qt::TransformationMode mode, const QHash<quint64, QImage> &resampled,
                     const QString &step);
    static QSize resizedLayerSize(QSize layerSize, QSize from, QSize to);
    void resizeCanvas(QSize newSize, QPoint anchorOffset = {});
    void crop(const QRect &rect, bool deletePixels = false);
  signals:
    void changed();
    void activeLayerChanged();
    void historyChanged();

  private:
    struct HistoryEntry {
        QString name;
        DocumentState before;
        DocumentState after;
        quint64 beforeRevision;
        quint64 afterRevision;
    };
    QVector<HistoryEntry> m_history;
    int m_historyCursor = 0;
    quint64 m_revision = 0;
    quint64 m_savedRevision = 0;
    quint64 m_nextRevision = 1;
    quint64 m_nextId = 1;
    bool m_inTransaction = false;
    bool m_transactionChanged = false;
    quint64 m_beforeRevision = 0;
    DocumentState m_before;
    QString m_transactionName;
    mutable QImage m_composite;
    mutable bool m_dirty = true;
    mutable bool m_cachedLinear = false;
    mutable qint64 m_selectionCacheKey = -1;
    mutable QRect m_cachedSelectionBounds;
};
QImage compositeDocument(const DocumentState &state, bool linear = false);
QImage applyAdjustment(const QImage &source, const QString &name, const QJsonObject &parameters);
// Normalized precision-safe selection helpers. Out-of-bounds coverage is zero.
qreal maskSample(const QImage &mask, int x, int y);
void setMaskSample(QImage &mask, int x, int y, qreal value);
QImage makeMask(QSize size, int bitDepth = 8, qreal value = 0);
QImage normalizeMask(const QImage &mask, int bitDepth = 0);
QImage refineMask(const QImage &mask, const QJsonObject &parameters, const QImage &source = {});
QImage renderedLayerMask(const Layer &layer, QSize canvas, int bitDepth = 8, bool includeRaster = true,
                         bool includeVector = true);
QColor blendColor(QColor backdrop, QColor source, const QString &mode);
} // namespace serika
