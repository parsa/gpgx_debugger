#include "MainWindow.h"
#include "EmulatorThread.h"
#include "views/EmulatorScreen.h"
#include "views/VdpRamView.h"
#include "views/VdpRegView.h"
#include "views/ScrollView.h"
#include "views/SaveStateView.h"
#include "views/VdpSpritesView.h"
#include "views/PlaneExplorerView.h"
#include "views/SoundDebugView.h"
#include "views/MemoryView.h"
#include "views/RamSearchView.h"
#include "views/RamWatchView.h"
#include "debugger/GpgxBackend.h"

#include <QMenuBar>
#include <QStatusBar>
#include <QDockWidget>
#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QFileInfo>
#include <QDir>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Genesis Plus GX Debugger"));
    resize(1440, 900);

    backend_ = new GpgxBackend();
    screen_  = new EmulatorScreen(this);
    screen_->setBackend(backend_);      // controller input goes through it
    setCentralWidget(screen_);
    screen_->setFocus();

    statusLabel_ = new QLabel(QStringLiteral("No ROM loaded"), this);
    statusBar()->addWidget(statusLabel_);

    buildMenus();
    buildDocks();

    backend_->onPaused([this](uint32_t pc) {
        QMetaObject::invokeMethod(this, [this, pc]{ onPaused(pc); }, Qt::QueuedConnection);
    });
    backend_->onResumed([this]() {
        QMetaObject::invokeMethod(this, [this]{ onResumed(); }, Qt::QueuedConnection);
    });

    // Live views: Gens updated its tool windows every frame; 10 Hz is enough
    // and keeps the GUI thread light. Reads of emulator memory while the core
    // runs are racy-but-benign, same as the original.
    refreshTimer_.setInterval(100);
    connect(&refreshTimer_, &QTimer::timeout, this, &MainWindow::refreshViews);
}

MainWindow::~MainWindow()
{
    if (emuThread_) {
        emuThread_->requestStop();
        emuThread_->wait();
    }
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (emuThread_) {
        emuThread_->requestStop();
        emuThread_->wait();
    }
    e->accept();
}

void MainWindow::buildMenus()
{
    auto* file = menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(QStringLiteral("&Open ROM..."), this, &MainWindow::openRom, QKeySequence::Open);
    file->addSeparator();
    file->addAction(QStringLiteral("E&xit"), this, &QWidget::close, QKeySequence::Quit);

    auto* debug = menuBar()->addMenu(QStringLiteral("&Debug"));
    actPause_    = debug->addAction(QStringLiteral("&Pause"),     this, &MainWindow::debugPause,    Qt::Key_F5);
    actResume_   = debug->addAction(QStringLiteral("&Resume"),    this, &MainWindow::debugResume,   Qt::Key_F9);
    actStepInto_ = debug->addAction(QStringLiteral("Step &Into"), this, &MainWindow::debugStepInto, Qt::Key_F7);
    actStepOver_ = debug->addAction(QStringLiteral("Step &Over"), this, &MainWindow::debugStepOver, Qt::Key_F8);

    actPause_->setEnabled(false);
    actResume_->setEnabled(false);
    actStepInto_->setEnabled(false);
    actStepOver_->setEnabled(false);
}

void MainWindow::buildDocks()
{
    vdpRamView_    = new VdpRamView(this);
    vdpRegView_    = new VdpRegView(this);
    scrollView_    = new ScrollView(this);
    statesView_    = new SaveStateView(this);
    spritesView_   = new VdpSpritesView(this);
    planeView_     = new PlaneExplorerView(this);
    soundView_     = new SoundDebugView(this);
    hexView_       = new MemoryView(this);
    ramSearchView_ = new RamSearchView(this);
    ramWatchView_  = new RamWatchView(this);

    vdpRamView_->setBackend(backend_);
    vdpRegView_->setBackend(backend_);
    scrollView_->setBackend(backend_);
    statesView_->setBackend(backend_);
    spritesView_->setBackend(backend_);
    planeView_->setBackend(backend_);
    soundView_->setBackend(backend_);
    hexView_->setBackend(backend_);
    ramSearchView_->setBackend(backend_);
    ramWatchView_->setBackend(backend_);

    connect(ramSearchView_, &RamSearchView::addWatchRequested,
            ramWatchView_,  &RamWatchView::addWatch);

    auto* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));

    auto addDock = [&](const QString& title, QWidget* w, Qt::DockWidgetArea area, bool visible) {
        auto* dock = new QDockWidget(title, this);
        dock->setObjectName(title);
        dock->setWidget(w);
        dock->setAllowedAreas(Qt::AllDockWidgetAreas);
        addDockWidget(area, dock);
        viewMenu->addAction(dock->toggleViewAction());
        dock->setVisible(visible);
        return dock;
    };

    auto* dTiles   = addDock(QStringLiteral("VDP Ram"),        vdpRamView_,    Qt::RightDockWidgetArea,  true);
    auto* dRegs    = addDock(QStringLiteral("VDP Registers"),  vdpRegView_,    Qt::RightDockWidgetArea,  false);
    auto* dSprites = addDock(QStringLiteral("VDP Sprites"),    spritesView_,   Qt::RightDockWidgetArea,  false);
    auto* dScroll  = addDock(QStringLiteral("Scroll"),         scrollView_,    Qt::RightDockWidgetArea,  false);
    tabifyDockWidget(dTiles, dRegs);
    tabifyDockWidget(dTiles, dSprites);
    tabifyDockWidget(dTiles, dScroll);
    dTiles->raise();

    auto* dHex    = addDock(QStringLiteral("Hex Editor"),      hexView_,       Qt::BottomDockWidgetArea, true);
    auto* dSearch = addDock(QStringLiteral("RAM Search"),      ramSearchView_, Qt::BottomDockWidgetArea, false);
    auto* dWatch  = addDock(QStringLiteral("RAM Watch"),       ramWatchView_,  Qt::BottomDockWidgetArea, false);
    tabifyDockWidget(dHex, dSearch);
    tabifyDockWidget(dHex, dWatch);
    dHex->raise();

    addDock(QStringLiteral("Save States"),     statesView_, Qt::LeftDockWidgetArea, false);
    addDock(QStringLiteral("Plane Explorer"),  planeView_, Qt::LeftDockWidgetArea, false);
    addDock(QStringLiteral("YM2612 && PSG"),   soundView_, Qt::LeftDockWidgetArea, false);
}

void MainWindow::openRom()
{
    QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open ROM"), QString(),
        QStringLiteral("ROM files (*.bin *.gen *.md *.smd);;All (*)"));
    if (path.isEmpty()) return;
    openRomFile(path);
}

void MainWindow::openRomFile(const QString& path)
{
    if (emuThread_) {
        emuThread_->requestStop();
        emuThread_->wait();
        delete emuThread_;
        emuThread_ = nullptr;
    }

    emuThread_ = new EmulatorThread(screen_, backend_, this);
    connect(emuThread_, &EmulatorThread::romLoaded, this, &MainWindow::onRomLoaded);

    if (!emuThread_->loadRom(path)) {
        delete emuThread_;
        emuThread_ = nullptr;
        return;
    }

    // States live beside the ROM here; the IDA host puts them beside the database.
    {
        QFileInfo fi(path);
        statesView_->setStatesDir(fi.dir().filePath(fi.completeBaseName() + QStringLiteral("_states")));
    }

    emuThread_->start();
    refreshTimer_.start();
    statusLabel_->setText(QStringLiteral("Running: ") + path);
    actPause_->setEnabled(true);
    actResume_->setEnabled(false);
}

void MainWindow::onRomLoaded(bool ok, const QString& msg)
{
    if (!ok) QMessageBox::critical(this, QStringLiteral("Error"), msg);
}

void MainWindow::refreshViews()
{
    // Only visible views pay the refresh cost.
    auto refreshIfVisible = [](auto* v) { if (v && v->isVisible()) v->refresh(); };
    refreshIfVisible(vdpRamView_);
    refreshIfVisible(vdpRegView_);
    refreshIfVisible(scrollView_);
    refreshIfVisible(statesView_);
    refreshIfVisible(spritesView_);
    refreshIfVisible(planeView_);
    refreshIfVisible(soundView_);
    refreshIfVisible(hexView_);
    refreshIfVisible(ramSearchView_);
    refreshIfVisible(ramWatchView_);
}

void MainWindow::onPaused(uint32_t pc)
{
    // On pause refresh everything, visible or not, so states are consistent
    vdpRamView_->refresh();
    vdpRegView_->refresh();
    scrollView_->refresh();
    statesView_->refresh();
    spritesView_->refresh();
    planeView_->refresh();
    soundView_->refresh();
    hexView_->refresh();
    ramSearchView_->refresh();
    ramWatchView_->refresh();

    actPause_->setEnabled(false);
    actResume_->setEnabled(true);
    actStepInto_->setEnabled(true);
    actStepOver_->setEnabled(true);
    statusLabel_->setText(QStringLiteral("Paused at %1").arg(pc, 6, 16, QLatin1Char('0')).toUpper());
}

void MainWindow::onResumed()
{
    actPause_->setEnabled(true);
    actResume_->setEnabled(false);
    actStepInto_->setEnabled(false);
    actStepOver_->setEnabled(false);
    statusLabel_->setText(QStringLiteral("Running"));
}

void MainWindow::debugPause()   { if (emuThread_ && emuThread_->isRunning()) backend_->pause(); }
void MainWindow::debugResume()  { backend_->resume(); }
void MainWindow::debugStepInto(){ backend_->stepInto(); }
void MainWindow::debugStepOver(){ backend_->stepOver(); }
