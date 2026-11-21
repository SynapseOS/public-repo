// Synapse Recorder, пометки поверх рабочего стола, 21.11.2026
#pragma once
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWidget>
#include <vector>
class Ink : public QWidget {
    std::vector<QPainterPath> strokes;
    bool drawing=false;
public:
    explicit Ink() : QWidget(nullptr,Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_DeleteOnClose);
        setCursor(Qt::CrossCursor); setWindowTitle("Synapse Recorder Ink");
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(rect(),QColor(0,0,0,1));
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor("#ff896e"),5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        for (const auto &stroke : strokes) p.drawPath(stroke);
        if (!windowFlags().testFlag(Qt::WindowTransparentForInput)) {
            p.fillRect(QRect(12,12,490,32),QColor("#17212a")); p.setPen(Qt::white);
            p.drawText(23,34,"Перо     Ctrl+Z  Отмена     Enter  Закрепить     Esc  Закрыть");
        }
    }
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button()!=Qt::LeftButton) return;
        strokes.emplace_back(e->position()); drawing=true; update();
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (drawing) { strokes.back().lineTo(e->position()); update(); }
    }
    void mouseReleaseEvent(QMouseEvent *) override { drawing=false; }
    void keyPressEvent(QKeyEvent *e) override {
        if (e->key()==Qt::Key_Escape) close();
        if (e->matches(QKeySequence::Undo) && !strokes.empty()) { drawing=false; strokes.pop_back(); update(); }
        if (e->key()==Qt::Key_Return) {
            drawing=false; setWindowFlag(Qt::WindowTransparentForInput,true); showFullScreen(); update();
        }
    }
};

