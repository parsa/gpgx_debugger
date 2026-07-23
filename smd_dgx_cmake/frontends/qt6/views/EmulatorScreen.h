#pragma once
#include <QWidget>
#include <QImage>
#include <QMutex>
#include <cstdint>

class EmulatorScreen : public QWidget {
    Q_OBJECT
public:
    explicit EmulatorScreen(QWidget* parent = nullptr);
    void pushFrame(const uint8_t* data, int srcW, int srcH, int pitch,
                   int vpX, int vpY, int vpW, int vpH);
    void setKeepAspect(bool v) { keepAspect_ = v; }
protected:
    void paintEvent(QPaintEvent*) override;
    QSize sizeHint() const override { return {640, 480}; }
private:
    QMutex  mutex_;
    QImage  back_, front_;
    bool    newFrame_    = false;
    bool    keepAspect_  = true;
    int     vpX_=0, vpY_=0, vpW_=320, vpH_=224;
};
