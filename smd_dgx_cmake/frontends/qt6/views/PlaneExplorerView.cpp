#include "PlaneExplorerView.h"
#include <QPainter>
#include <QMouseEvent>
#include <QRadioButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QLabel>
#include <QScrollArea>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <algorithm>
#include <cstring>
#include <set>

static constexpr int kBufW = 1024;
static constexpr int kBufH = 1024;
static const char kPlaneChar[4] = { 'A', 'B', 'W', 'S' };

// ---------------------------------------------------------------- canvas ----

PlaneExplorerCanvas::PlaneExplorerCanvas(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFixedSize(kBufW, kBufH);
}

void PlaneExplorerCanvas::setImage(const QImage& img) { image_ = img; update(); }

void PlaneExplorerCanvas::clearImage()
{
    image_     = QImage();
    highlight_ = QRect();
    update();
}

void PlaneExplorerCanvas::setZoom(int z)
{
    zoom_ = z;
    setFixedSize(kBufW * z, kBufH * z);
    update();
}

void PlaneExplorerCanvas::setHighlight(const QRect& r)
{
    if (highlight_ != r) { highlight_ = r; update(); }
}

void PlaneExplorerCanvas::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0x1E, 0x1E, 0x1E));
    if (image_.isNull()) return;

    p.drawImage(QRect(0, 0, image_.width() * zoom_, image_.height() * zoom_), image_);

    if (!highlight_.isNull()) {
        QPen pen(Qt::white);
        pen.setStyle(Qt::DashLine);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRect(highlight_.x() * zoom_, highlight_.y() * zoom_,
                         highlight_.width() * zoom_ - 1, highlight_.height() * zoom_ - 1));
    }
}

void PlaneExplorerCanvas::emitHover(const QPoint& p)
{
    if (image_.isNull()) { emit hoverMoved(-1, -1); return; }
    const int x = p.x() / zoom_, y = p.y() / zoom_;
    if (x < 0 || y < 0 || x >= image_.width() || y >= image_.height())
        emit hoverMoved(-1, -1);
    else
        emit hoverMoved(x, y);
}

void PlaneExplorerCanvas::mouseMoveEvent(QMouseEvent* e)  { emitHover(e->pos()); }
void PlaneExplorerCanvas::mousePressEvent(QMouseEvent* e) { emitHover(e->pos()); }
void PlaneExplorerCanvas::leaveEvent(QEvent*)             { emit hoverMoved(-1, -1); }

// ------------------------------------------------------------------ view ----

PlaneExplorerView::PlaneExplorerView(QWidget* parent) : QWidget(parent)
{
    buf_.resize(size_t(kBufW) * kBufH);
    colorTable_ = QList<QRgb>(256, qRgb(0, 0, 0));

    infoLabel_ = new QLabel(this);
    infoLabel_->setFont(QFont(QStringLiteral("Courier New"), 9));
    infoLabel_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    infoLabel_->setTextFormat(Qt::PlainText);
    infoLabel_->setMinimumWidth(170);

    auto* group = new QGroupBox(QStringLiteral("Plane"), this);
    auto* gl    = new QVBoxLayout(group);
    static const char* kNames[4] = { "Plane A", "Plane B", "Window", "Sprites" };
    auto* bg = new QButtonGroup(this);
    for (int i = 0; i < 4; ++i) {
        radios_[i] = new QRadioButton(QString::fromLatin1(kNames[i]), group);
        bg->addButton(radios_[i], i);
        gl->addWidget(radios_[i]);
    }
    radios_[0]->setChecked(true);
    connect(bg, &QButtonGroup::idClicked, this, &PlaneExplorerView::onPlane);

    transCheck_ = new QCheckBox(QStringLiteral("Transparency"), this);
    connect(transCheck_, &QCheckBox::toggled, this, &PlaneExplorerView::onTransparency);

    zoomCheck_ = new QCheckBox(QStringLiteral("Zoom 2x"), this);
    connect(zoomCheck_, &QCheckBox::toggled, this, &PlaneExplorerView::onZoom);

    canvas_ = new PlaneExplorerCanvas;
    connect(canvas_, &PlaneExplorerCanvas::hoverMoved, this, &PlaneExplorerView::onHover);

    scrollArea_ = new QScrollArea(this);
    scrollArea_->setWidget(canvas_);
    scrollArea_->setWidgetResizable(false);
    QPalette vp = scrollArea_->viewport()->palette();
    vp.setColor(QPalette::Window, QColor(0x1E, 0x1E, 0x1E));
    scrollArea_->viewport()->setPalette(vp);
    scrollArea_->viewport()->setAutoFillBackground(true);

    auto* left = new QVBoxLayout;
    left->addWidget(infoLabel_, 1);
    left->addWidget(group);
    left->addWidget(transCheck_);
    left->addWidget(zoomCheck_);

    auto* main = new QHBoxLayout(this);
    main->setContentsMargins(4, 4, 4, 4);
    main->addLayout(left);
    main->addWidget(scrollArea_, 1);

    updateControls();
}

void PlaneExplorerView::setBackend(IDebugBackend* b)
{
    backend_ = b;
    refresh();
}

void PlaneExplorerView::refresh()
{
    hasData_ = false;
    if (backend_ && backend_->isRunning()) {
        const VdpState v = backend_->getVdpState();
        if (v.vram && v.cram) {
            std::memcpy(vram_, v.vram, sizeof(vram_));
            std::memcpy(cram_, v.cram, sizeof(cram_));
            std::memcpy(regs_, v.reg,  sizeof(regs_));
            hasData_ = true;
        }
    }
    updateControls();
    rebuild();
}

void PlaneExplorerView::updateControls()
{
    const bool en = backend_ != nullptr;
    for (auto* r : radios_) r->setEnabled(en);
    transCheck_->setEnabled(en);
    zoomCheck_->setEnabled(en);
}

void PlaneExplorerView::decodeMode()
{
    const uint8_t set4 = regs_[0x0C];
    const uint8_t scr  = regs_[0x10];
    h40_   = (set4 & 0x01) != 0;
    im2_   = (set4 & 0x06) == 0x06;
    tileH_ = im2_ ? 16 : 8;

    switch (plane_) {
    case 0: base_ = uint32_t(regs_[0x02] & 0x38) << 10; break;
    case 1: base_ = uint32_t(regs_[0x04] & 0x07) << 13; break;
    case 2: base_ = uint32_t(regs_[0x03] & (h40_ ? 0x3C : 0x3E)) << 10; break;
    case 3: base_ = uint32_t(regs_[0x05] & (h40_ ? 0x7E : 0x7F)) << 9;  break;
    }

    const int hsz = scr & 3, vsz = (scr >> 4) & 3;
    const int mh  = hsz;
    const int mv  = ((vsz & 0x1) & ((~hsz & 0x02) >> 1))
                  | ((vsz & 0x02) & ((~hsz & 0x01) << 1));
    planeW_ = (mh + 1) * 32;
    planeH_ = (mv + 1) * 32;
    if (mh == 2)      { planeW_ = 32; planeH_ = 1;  }   // prohibited H value
    else if (mv == 2) { planeW_ = 32; planeH_ = 32; }   // prohibited V value

    if (plane_ == 2)      { planeW_ = h40_ ? 64 : 32; planeH_ = 32; }
    else if (plane_ == 3) { planeW_ = 64;             planeH_ = im2_ ? 128 : 64; }
}

void PlaneExplorerView::buildColorTable()
{
    for (int i = 0; i < 256; ++i) colorTable_[i] = qRgb(0, 0, 0);
    for (int i = 0; i < 64; ++i) {
        const uint16_t c = cw(i);
        colorTable_[i] = qRgb(((c >> 0) & 0xE) << 4,
                              ((c >> 4) & 0xE) << 4,
                              ((c >> 8) & 0xE) << 4);
    }
    colorTable_[253] = qRgb(0x33, 0x33, 0x33);
    colorTable_[254] = qRgb(0x44, 0x44, 0x44);
    colorTable_[255] = qRgb(0x55, 0x55, 0x55);
}

void PlaneExplorerView::rebuild()
{
    if (!hasData_) { canvas_->clearImage(); return; }

    decodeMode();
    buildColorTable();

    uint8_t* d = buf_.data();
    for (int j = 0; j < kBufH; ++j)
        for (int i = 0; i < kBufW; ++i)
            d[j * kBufW + i] = uint8_t((((j ^ i) >> 2) & 1) + 253);

    if (plane_ < 3) {
        for (int j = 0; j < planeH_; ++j)
            for (int i = 0; i < planeW_; ++i) {
                const int trans = showTrans_ ? int((((j ^ i) >> 1) & 1) + 254) : -1;
                drawTile(vw(base_ + uint32_t(j * planeW_ + i) * 2), i, j, trans);
            }
    } else {
        std::vector<int> order = spriteLinkOrder();
        std::sort(order.begin(), order.end(), [](int a, int b) { return a > b; });
        for (int no : order)                       // reverse numeric order: lowest painted last
            drawSprite(readSprite(no));
    }

    QImage img(buf_.data(), kBufW, kBufH, kBufW, QImage::Format_Indexed8);
    img.setColorTable(colorTable_);
    canvas_->setImage(img);
}

void PlaneExplorerView::drawTile(uint16_t entry, int tx, int ty, int transColor)
{
    const int      tileIdx  = entry & 0x7FF;
    const bool     hf       = (entry & 0x0800) != 0;
    const bool     vf       = (entry & 0x1000) != 0;
    const uint8_t  palBase  = uint8_t(((entry >> 13) & 3) << 4);
    const uint32_t tileAddr = uint32_t(tileIdx * tileH_ * 4) & 0xFFFFu;

    uint8_t* d = buf_.data();
    for (int row = 0; row < tileH_; ++row) {
        const int py = ty * tileH_ + row;
        if (py >= kBufH) return;
        const uint32_t rowAddr = tileAddr + uint32_t(vf ? (tileH_ - 1 - row) : row) * 4;
        uint8_t* out = d + py * kBufW + tx * 8;
        for (int k = 0; k < 8; ++k) {
            const int     col = hf ? (7 - k) : k;
            const uint8_t b   = vb(rowAddr + col / 2);
            const uint8_t nib = (col & 1) ? uint8_t(b & 0xF) : uint8_t(b >> 4);
            out[k] = (nib == 0 && transColor >= 0) ? uint8_t(transColor)
                                                   : uint8_t(nib | palBase);
        }
    }
}

PlaneExplorerView::Sprite PlaneExplorerView::readSprite(int no) const
{
    const uint32_t a  = base_ + uint32_t(no) * 8;
    const uint16_t w0 = vw(a), w1 = vw(a + 2), w2 = vw(a + 4), w3 = vw(a + 6);
    Sprite s;
    s.no     = no;
    s.ypos   = w0;
    s.wCells = ((w1 >> 10) & 3) + 1;
    s.hCells = ((w1 >> 8) & 3) + 1;
    s.link   = w1 & 0x7F;
    s.prio   = (w2 & 0x8000) != 0;
    s.pal    = (w2 >> 13) & 3;
    s.vf     = (w2 & 0x1000) != 0;
    s.hf     = (w2 & 0x0800) != 0;
    s.block  = w2 & 0x7FF;
    s.xpos   = w3;
    return s;
}

std::vector<int> PlaneExplorerView::spriteLinkOrder() const
{
    const int maxSpr = h40_ ? 80 : 64;
    std::vector<int> order;
    std::set<int> seen;
    int cur = 0;
    for (;;) {
        if (seen.count(cur)) break;                 // cycle guard
        seen.insert(cur);
        order.push_back(cur);
        const int link = vw(base_ + uint32_t(cur) * 8 + 2) & 0x7F;
        if (link == 0 || link >= maxSpr) break;
        cur = link;
    }
    return order;
}

void PlaneExplorerView::drawSprite(const Sprite& s)
{
    const int startX = 0x80;
    const int startY = im2_ ? 0x100 : 0x80;
    const int endX   = h40_ ? 0x1BF : 0x17F;
    const int endY   = im2_ ? 0x2DF : 0x2BF;
    const int w = s.wCells * 8, h = s.hCells * tileH_;

    uint8_t* d = buf_.data();
    for (int j = 0; j < h; ++j) {
        const int sy = s.ypos + j;
        if (sy < startY || sy >= endY) continue;
        for (int i = 0; i < w; ++i) {
            const int sx = s.xpos + i;
            if (sx < startX || sx >= endX) continue;
            const int pr = s.vf ? (h - 1 - j) : j;
            const int pc = s.hf ? (w - 1 - i) : i;
            const int blockOffset = (pc / 8) * s.hCells + (pr / tileH_);
            const uint32_t rowAddr = uint32_t(s.block + blockOffset) * uint32_t(4 * tileH_)
                                   + uint32_t(pr % tileH_) * 4;
            const uint8_t b   = vb(rowAddr + (pc % 8) / 2);
            const uint8_t nib = (pc & 1) ? uint8_t(b & 0xF) : uint8_t(b >> 4);
            if (nib == 0) continue;                 // sprites always skip index 0
            d[(sy - startY) * kBufW + (sx - startX)] = uint8_t(nib | (s.pal << 4));
        }
    }
}

void PlaneExplorerView::onPlane(int idx)
{
    if (plane_ == idx) return;
    plane_      = idx;
    spriteRect_ = QRect();
    clearHover();
    rebuild();
}

void PlaneExplorerView::onTransparency(bool on)
{
    showTrans_ = on;
    rebuild();
}

void PlaneExplorerView::onZoom(bool on)
{
    zoom_ = on ? 2 : 1;
    canvas_->setZoom(zoom_);
}

void PlaneExplorerView::clearHover()
{
    infoLabel_->clear();
    canvas_->setHighlight(QRect());
}

void PlaneExplorerView::onHover(int x, int y)
{
    if (x < 0 || y < 0 || !hasData_) { clearHover(); return; }
    const int W = planeW_ * 8, H = planeH_ * tileH_;
    if (x >= W || y >= H) { clearHover(); return; }

    if (plane_ < 3) {
        const int      tx   = x / 8, ty = y / tileH_;
        const uint32_t addr = (base_ + uint32_t(ty * planeW_ + tx) * 2) & 0xFFFFu;
        const uint16_t val  = vw(addr);
        const int      idx  = val & 0x7FF;
        infoLabel_->setText(QString::asprintf(
            "X/Y: %dx%d;\n\nPLANE: %c (0x%04X);\n\nADDRESS: 0x%04X;\n\nVALUE: 0x%04X;\n\n"
            "INDEX: 0x%04X(%d);\n\nPALETTE: %d;\n\nHFLIP: %s; VFLIP: %s;\n\nPRIORITY: %s",
            x & ~7, y & ~(tileH_ - 1), kPlaneChar[plane_], unsigned(base_), unsigned(addr),
            unsigned(val), idx, idx, (val >> 13) & 3,
            (val & 0x0800) ? "YES" : "NO",
            (val & 0x1000) ? "YES" : "NO",
            (val & 0x8000) ? "YES" : "NO"));
        canvas_->setHighlight(QRect(x & ~7, y & ~(tileH_ - 1), 8, tileH_));
    } else {
        const int startY = im2_ ? 0x100 : 0x80;
        for (int no : spriteLinkOrder()) {          // link order: first hit wins
            const Sprite s = readSprite(no);
            const int w = s.wCells * 8, h = s.hCells * tileH_;
            const int minX = s.xpos - 0x80, minY = s.ypos - startY;
            const int maxX = minX + w - 1, maxY = minY + h - 1;
            if (minX < 0 && maxX >= W && minY < 0 && maxY >= H) continue;   // full-cover skip
            if (x < minX || x > maxX || y < minY || y > maxY) continue;
            infoLabel_->setText(QString::asprintf(
                "INDEX: %d;\n\nSCREEN X/Y: %dx%d;\n\nPLANE: %c (0x%04X);\n\nADDRESS: 0x%04X;\n\n"
                "SPRITE X/Y: %dx%d;\n\nSIZE W/H: %dx%d;\n\nTILE ID: 0x%04X(%d)\n\nLINK: %d;\n\n"
                "PALETTE: %d;\n\nHFLIP: %s; VFLIP: %s;\n\nPRIORITY: %s",
                s.no, x, y, kPlaneChar[3], unsigned(base_),
                unsigned((base_ + uint32_t(s.no) * 8) & 0xFFFFu),
                s.xpos, s.ypos, w, h, s.block, s.block, s.link, s.pal,
                s.hf ? "YES" : "NO", s.vf ? "YES" : "NO", s.prio ? "YES" : "NO"));
            spriteRect_ = QRect(minX, minY, w, h);
            break;
        }
        canvas_->setHighlight(spriteRect_);         // stale rect persists on miss
    }
}
