#include "EmulatorScreen.h"
#include <QPainter>
#include <QMutexLocker>
#include <QKeyEvent>
#include <cstring>

EmulatorScreen::EmulatorScreen(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(320, 224);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QPalette pal = palette(); pal.setColor(QPalette::Window, Qt::black);
    setAutoFillBackground(true); setPalette(pal);
    // Needed to receive key events at all; click-to-focus so the host's other
    // panes keep working normally.
    setFocusPolicy(Qt::StrongFocus);
}

// --------------------------------------------------------------------------
// input
// --------------------------------------------------------------------------
void EmulatorScreen::applyKey(QKeyEvent* e, bool pressed)
{
    if (e->isAutoRepeat()) { e->accept(); return; }

    uint16_t bit = 0;
    switch (e->key()) {
    case Qt::Key_Up:        bit = PAD_UP;    break;
    case Qt::Key_Down:      bit = PAD_DOWN;  break;
    case Qt::Key_Left:      bit = PAD_LEFT;  break;
    case Qt::Key_Right:     bit = PAD_RIGHT; break;
    case Qt::Key_Z:         bit = PAD_A;     break;
    case Qt::Key_X:         bit = PAD_B;     break;
    case Qt::Key_C:         bit = PAD_C;     break;
    case Qt::Key_A:         bit = PAD_X;     break;
    case Qt::Key_S:         bit = PAD_Y;     break;
    case Qt::Key_D:         bit = PAD_Z;     break;
    case Qt::Key_Return:
    case Qt::Key_Enter:     bit = PAD_START; break;
    case Qt::Key_Backspace: bit = PAD_MODE;  break;
    default:
        e->ignore();
        return;
    }

    pad_ = pressed ? (pad_ | bit) : (uint16_t)(pad_ & ~bit);
    if (backend_) backend_->setPad(0, pad_);
    e->accept();
}

void EmulatorScreen::keyPressEvent(QKeyEvent* e)   { applyKey(e, true);  }
void EmulatorScreen::keyReleaseEvent(QKeyEvent* e) { applyKey(e, false); }

void EmulatorScreen::focusOutEvent(QFocusEvent*)
{
    // Losing focus mid-press would latch a button down forever.
    pad_ = 0;
    if (backend_) backend_->setPad(0, 0);
}

void EmulatorScreen::pushFrame(const uint8_t* data, int srcW, int srcH, int pitch,
                                int vpX, int vpY, int vpW, int vpH)
{
    QMutexLocker lk(&mutex_);
    if (back_.width() != srcW || back_.height() != srcH)
        back_ = QImage(srcW, srcH, QImage::Format_RGB16);
    for (int y = 0; y < srcH; ++y)
        std::memcpy(back_.scanLine(y), data + y*pitch, (size_t)srcW*2);
    vpX_=vpX; vpY_=vpY; vpW_=vpW; vpH_=vpH; newFrame_=true;
    QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
}

void EmulatorScreen::paintEvent(QPaintEvent*)
{
    { QMutexLocker lk(&mutex_); if (newFrame_) { front_ = back_.copy(vpX_,vpY_,vpW_,vpH_); newFrame_=false; } }
    if (front_.isNull()) return;

    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QRectF dst;
    if (keepAspect_) {
        double sa = (double)front_.width()/front_.height(), da = (double)width()/height();
        if (da > sa) { double h=height(), w=h*sa; dst={((double)width()-w)/2.,0.,w,h}; }
        else         { double w=width(),  h=w/sa; dst={0.,(double)(height()-h)/2.,w,h}; }
        p.fillRect(rect(), Qt::black);
    } else { dst = QRectF(rect()); }
    p.drawImage(dst, front_);
}
