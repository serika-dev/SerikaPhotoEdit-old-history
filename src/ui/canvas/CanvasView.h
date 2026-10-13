#pragma once
#include "document/Document.h"
#include "tools/BrushDynamics.h"
#include <QIcon>
#include <QPolygonF>
#include <QTimer>
#include <QTransform>
#include <QWidget>
namespace serika {
struct CropSettings {
    QSizeF ratio;
    QSize outputSize;
    qreal resolution = 0;
    bool deleteCroppedPixels = false;
    QString overlay = "Thirds";
    qreal angle = 0;
};
class CanvasView : public QWidget {
    Q_OBJECT
  public:
    explicit CanvasView(Document *document, QWidget *parent = nullptr);
    Document *document() const { return m_document; }
    QString tool() const { return m_tool; }
    void setTool(const QString &tool);
    void setForeground(QColor color);
    QColor foreground() const;
    void setBackground(QColor color);
    void setBrushSize(int size);
    void setBrushHardness(qreal hardness);
    void setBrushOpacity(qreal opacity);
    void setBrushFlow(qreal flow);
    void setBrushSpacing(qreal spacing);
    void setBrushAngle(qreal degrees);
    void setBrushRoundness(qreal roundness);
    void setBrushSmoothing(qreal smoothing);
    BrushPreset brushPreset() const;
    void setBrushPreset(const BrushPreset &preset);
    void nudgeSelectedLayers(QPointF delta);
    void setShowRulers(bool enabled);
    void setShowGrid(bool enabled);
    void setShowGuides(bool enabled);
    void setQuickMask(bool enabled);
    void setSurround(QColor color);
    void setZoom(qreal zoom);
    qreal zoom() const;
    void fitToView();
    void fitSelection();
    void resetView();
    void rotateView(qreal degrees);
    void flipView();
    QPointF toDocument(QPointF point) const;
    QPointF fromDocument(QPointF point) const;
    void fillSelection(QColor color);
    void strokeSelection(QColor color, int width);
    void selectSubject();
    void removeBackground();
    // Apply a subject mask computed elsewhere (for example by the AI engine on a worker thread).
    void setSubjectSelection(const QImage &mask);
    void removeBackground(const QImage &subject);
    void contentAwareFill();
    void transformActive(qreal scale, qreal angle);
    CropSettings cropSettings() const { return m_cropSettings; }
    void setCropSettings(const CropSettings &settings);
    bool hasCropPreview() const { return m_cropActive || m_perspectivePoints.size() == 4; }
    QRectF cropPreviewRect() const { return m_cropRect; }
    void setCropPreviewRect(QRectF rectangle);
    void resetCrop();
    void swapCropRatio();
    void cycleCropOverlay();
    void beginStraighten();
    bool commitCrop();
    void cancelCrop();
    bool autoCropTransparent(bool preview = true);
    bool autoCropContent(bool preview = true);
    bool autoStraighten(bool preview = true);
    enum class MaskPreview { None, Grayscale, Overlay };
    void setMaskPreview(MaskPreview preview);
    MaskPreview maskPreview() const { return m_layerMaskPreview; }
    bool hasPendingInteraction() const;
    void cancelInteraction();
    void setBrushOpacityByNumber(int number, bool flow = false);
    bool beginTransform(const QString &mode = "Free Transform");
    bool hasTransformPreview() const { return m_transformActive; }
    bool commitTransform();
    void cancelTransform();
    QTransform transformPreview() const { return m_previewTransform; }
    qreal brushOpacity() const { return m_opacity; }
    qreal brushFlow() const { return m_flow; }
    int brushSize() const { return m_brushSize; }
    qreal brushHardness() const { return m_hardness; }
  signals:
    void zoomChanged(qreal zoom);
    void colorPicked(QColor color);
    void cursorInfo(QPointF position, QColor color);
    void toolChanged(const QString &tool);
    void cropPreviewChanged(bool active);
    void cropSettingsChanged();
    void brushSettingsChanged();
    void interactionError(const QString &error);
    void transformPreviewChanged(bool active);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void keyReleaseEvent(QKeyEvent *) override;
    void tabletEvent(QTabletEvent *) override;
    void resizeEvent(QResizeEvent *) override;

  private:
    Document *m_document;
    QString m_tool = "Move";
    QColor m_foreground = Qt::black;
    QColor m_background = Qt::white;
    QColor m_surround = QColor("#262626");
    int m_brushSize = 40;
    qreal m_hardness = 0.8;
    qreal m_opacity = 1;
    qreal m_flow = 1;
    qreal m_spacing = 0.1;
    qreal m_brushAngle = 0;
    qreal m_roundness = 1;
    qreal m_smoothing = 0;
    BrushPreset m_brushPreset;
    BrushDynamics m_brushDynamics;
    QPointF m_tabletTilt;
    QHash<quint64, QImage> m_strokeCoverage;
    qreal m_zoom = 1;
    qreal m_rotation = 0;
    qreal m_pressure = 1;
    QPointF m_pan;
    bool m_rulers = true;
    bool m_grid = false;
    bool m_guides = true;
    bool m_quickMask = false;
    bool m_flip = false;
    bool m_space = false;
    bool m_dragging = false;
    bool m_panning = false;
    bool m_fitPending = true;
    QPointF m_start;
    QPointF m_last;
    QPointF m_cursor;
    QVector<quint64> m_moveLayerIds;
    QPointF m_moveAppliedDelta;
    bool m_moveMaskOnly = false;
    QPointF m_cloneSource;
    bool m_hasCloneSource = false;
    QImage m_strokeSource;
    QPolygonF m_lasso;
    QRectF m_dragRect;
    QString m_selectionOperation;
    QTimer m_ants;
    int m_antOffset = 0;
    qint64 m_selectionKey = -1;
    bool m_selectionActive = false;
    bool m_moving = false;
    QPainterPath m_selectionOutline;
    QImage m_maskOverlay;
    QImage m_proofPreview;
    qint64 m_proofSourceKey = 0;
    QJsonObject m_proofSettings;
    QByteArray m_proofSourceIcc;
    QImage proofPreview(const QImage &source);
    int m_guideDrag = 0;
    qreal m_guidePosition = 0;
    bool m_rotating = false;
    bool m_hudBrush = false;
    qreal m_initialRotation = 0;
    int m_initialBrushSize = 40;
    qreal m_initialHardness = 0.8;
    TileImage m_originalPixels;
    QImage m_healMask;
    quint64 m_penLayerId = 0;
    QPainterPath m_penPath;
    QImage m_historySource;
    int m_pathElement = -1;
    QPainterPath m_initialPath;
    QPolygonF m_perspectivePoints;
    CropSettings m_cropSettings;
    QRectF m_cropRect;
    QRectF m_initialCropRect;
    bool m_cropActive = false;
    int m_cropHandle = -1;
    int m_perspectiveHandle = -1;
    bool m_straightenArmed = false;
    bool m_straightening = false;
    bool m_cropRotating = false;
    qreal m_initialCropAngle = 0;
    MaskPreview m_layerMaskPreview = MaskPreview::None;
    mutable QImage m_layerMaskPreviewCache;
    mutable QImage m_layerMaskOverlayCache;
    mutable Layer m_cachedMaskLayer;
    mutable QSize m_cachedMaskCanvas;
    bool m_transporting = false;
    bool m_transportSelecting = false;
    QImage m_transportMask;
    QImage m_transportPreview;
    QPointF m_transportDelta;
    QPointF m_transportOrigin;
    QPointF m_lastStrokePoint;
    bool m_hasLastStrokePoint = false;
    int m_numericOpacity = -1;
    qint64 m_numericOpacityTime = 0;
    bool m_numericOpacityFlow = false;
    bool m_transformActive = false;
    bool m_committingTransform = false;
    QString m_transformMode;
    quint64 m_transformLayerId = 0;
    QPolygonF m_transformQuad;
    QPolygonF m_initialTransformQuad;
    QPointF m_transformPivot;
    QPointF m_initialTransformPivot;
    int m_transformHandle = -1;
    QTransform m_previewTransform;
    QImage m_transformSource;
    QImage m_transformPreview;
    DocumentState m_transformState;
    QRectF m_transformBounds;
    QRectF activeLayerBounds() const;
    void dab(const BrushDab &sample);
    void drawStroke(QPointF to);
    QVector<quint64> selectedMoveRoots() const;
    bool updateMove(QPointF point, Qt::KeyboardModifiers modifiers);
    void finishSelection();
    void applyGradient();
    void bucket(QPoint point);
    void applyImage(const QImage &image, QPoint documentOrigin, bool erase = false);
    void updateSelectionOutline();
    void chooseSelectionOperation(Qt::KeyboardModifiers modifiers);
    void sampleColor(QPointF point);
    void perspectiveCrop();
    int cropHandleAt(QPointF point) const;
    void updateCropDrag(QPointF point, Qt::KeyboardModifiers modifiers);
    void drawCropPreview(QPainter &painter);
    void rotateDocumentContent(qreal angle);
    void finishTransport();
    QImage activeMaskPreview() const;
    int transformHandleAt(QPointF point) const;
    void updateTransformDrag(QPointF point, Qt::KeyboardModifiers modifiers);
    void renderTransformPreview();
    void drawTransformPreview(QPainter &painter);
    QTransform viewTransform() const;
};
QStringList toolNames();
QIcon toolIcon(const QString &name, QColor color = QColor());
// The tool glyph with a corner triangle marking a toolbar slot that holds more tools.
QIcon toolGroupIcon(const QString &name, QColor color = QColor());
} // namespace serika
