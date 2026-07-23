#include "EmulatorScreen.h"
#include <QPainter>
#include <QMutexLocker>
#include <cstring>

EmulatorScreen::EmulatorScreen(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(320, 224);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QPalette pal = palette(); pal.setColor(QPalette::Window, Qt::black);
    setAutoFillBackground(true); setPalette(pal);
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
