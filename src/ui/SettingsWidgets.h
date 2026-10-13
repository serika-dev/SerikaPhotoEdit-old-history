#pragma once
#include <QCheckBox>
#include <QFrame>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <functional>

namespace serika {
// An on/off switch. It stays a QCheckBox so code and tests treat it like one.
class ToggleSwitch final : public QCheckBox {
  public:
    explicit ToggleSwitch(QWidget *parent = nullptr) : QCheckBox(parent) {
        setCursor(Qt::PointingHandCursor);
    }
    QSize sizeHint() const override { return {40, 24}; }
    QSize minimumSizeHint() const override { return sizeHint(); }

  protected:
    bool hitButton(const QPoint &point) const override { return rect().contains(point); }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QPalette &colors = palette();
        const QRectF track(2, (height() - 20) / 2.0, 36, 20);
        QColor fill = isChecked() ? colors.color(QPalette::Highlight) : colors.color(QPalette::Mid);
        if (!isEnabled())
            fill.setAlphaF(0.35);
        if (hasFocus()) {
            painter.setPen(QPen(colors.color(QPalette::Link), 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(track.adjusted(-2, -2, 2, 2), 12, 12);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(track, 10, 10);
        painter.setBrush(QColor(isEnabled() ? "#FFFFFF" : "#D4D4D8"));
        const qreal knob = isChecked() ? track.right() - 18 : track.left() + 2;
        painter.drawEllipse(QRectF(knob, track.top() + 2, 16, 16));
    }
};

// A selectable tile (theme, engine, model) that behaves like a radio button.
class ChoiceTile final : public QFrame {
  public:
    std::function<void()> clicked;
    explicit ChoiceTile(QWidget *parent = nullptr) : QFrame(parent) {
        setObjectName("prefsChoice");
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
    }
    void setChecked(bool checked) {
        setProperty("checked", checked);
        style()->unpolish(this);
        style()->polish(this);
    }

  protected:
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()) && clicked)
            clicked();
    }
    void keyPressEvent(QKeyEvent *event) override {
        if ((event->key() == Qt::Key_Space || event->key() == Qt::Key_Return) && clicked)
            clicked();
        else
            QFrame::keyPressEvent(event);
    }
};
} // namespace serika
