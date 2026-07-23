// Qt-side widget builder for the embedded views. Includes Qt + our view headers
// ONLY — NO IDA headers (see ida_views_shared.h). The IDA TWidget arrives as a
// void* that is really a QWidget* in IDA's stock-Qt build.
//
// IDA 9.2+ ships the same Qt 6.8.2 our views target, so the widgets are used
// as-is and run on the Qt DLLs IDA has already loaded.

#include <QWidget>
#include <QVBoxLayout>
#include <QTimer>
#include <QPointer>
#include <QObject>

#include "debugger/EmuHost.h"
#include "ida_views_shared.h"

#include "views/VdpRamView.h"
#include "views/VdpRegView.h"
#include "views/VdpSpritesView.h"
#include "views/PlaneExplorerView.h"
#include "views/SoundDebugView.h"
#include "views/MemoryView.h"
#include "views/RamSearchView.h"
#include "views/RamWatchView.h"

extern EmuHost* smd_dgx_host();   // smd_dgx_ida.cpp

const char* const smd_dgx_view_titles[SMD_DGX_VIEW_COUNT] = {
    "SMD VDP Ram",
    "SMD VDP Registers",
    "SMD VDP Sprites",
    "SMD Plane Explorer",
    "SMD YM2612 & PSG",
    "SMD Hex Editor",
    "SMD RAM Search",
    "SMD RAM Watch",
};

namespace {

struct Dock {
    QWidget* (*make)(QWidget* parent);
    void (*refresh)(QWidget*);
    QPointer<QWidget> widget;
};

template <typename V>
QWidget* make_view(QWidget* parent)
{
    auto* v = new V(parent);
    if (EmuHost* h = smd_dgx_host())
        v->setBackend(h->backend());
    return v;
}

template <typename V>
void refresh_view(QWidget* w)
{
    if (auto* v = qobject_cast<V*>(w)) {
        if (EmuHost* h = smd_dgx_host())
            v->setBackend(h->backend());   // rebind if the host restarted
        v->refresh();
    }
}

Dock g_docks[SMD_DGX_VIEW_COUNT] = {
    { &make_view<VdpRamView>,        &refresh_view<VdpRamView>,        {} },
    { &make_view<VdpRegView>,        &refresh_view<VdpRegView>,        {} },
    { &make_view<VdpSpritesView>,    &refresh_view<VdpSpritesView>,    {} },
    { &make_view<PlaneExplorerView>, &refresh_view<PlaneExplorerView>, {} },
    { &make_view<SoundDebugView>,    &refresh_view<SoundDebugView>,    {} },
    { &make_view<MemoryView>,        &refresh_view<MemoryView>,        {} },
    { &make_view<RamSearchView>,     &refresh_view<RamSearchView>,     {} },
    { &make_view<RamWatchView>,      &refresh_view<RamWatchView>,      {} },
};

QTimer* g_timer = nullptr;

void ensure_timer()
{
    if (g_timer) return;
    g_timer = new QTimer;
    g_timer->setInterval(100);   // 10 Hz, matches the standalone app
    QObject::connect(g_timer, &QTimer::timeout, [] {
        for (auto& d : g_docks)
            if (d.widget && d.widget->isVisible())
                d.refresh(d.widget);
    });
    g_timer->start();
}

} // namespace

void smd_dgx_view_attach(int idx, void* twidget_as_qwidget)
{
    if (idx < 0 || idx >= SMD_DGX_VIEW_COUNT) return;
    auto* host = reinterpret_cast<QWidget*>(twidget_as_qwidget);
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    g_docks[idx].widget = g_docks[idx].make(host);
    layout->addWidget(g_docks[idx].widget);
    ensure_timer();
}

void smd_dgx_view_detach_all()
{
    if (g_timer) { g_timer->stop(); g_timer->deleteLater(); g_timer = nullptr; }
    for (auto& d : g_docks) d.widget = nullptr;
}
