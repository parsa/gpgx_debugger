// IDA-side dock glue for the embedded views. Includes IDA headers ONLY — no Qt
// (see ida_views_shared.h for why the two must stay in separate TUs).
//
// Creates one dockable empty widget per view; the Qt side (ida_views.cpp) fills
// it in. Adds a menu action per view under Debugger windows.
// VERIFY-9.3 markers: check these APIs against the installed 9.3 SDK.

#include <ida.hpp>
#include <kernwin.hpp>

#include "ida_views_shared.h"

namespace {

void open_view(int idx)
{
    const char* title = smd_dgx_view_titles[idx];
    if (TWidget* existing = find_widget(title)) {
        activate_widget(existing, true);               // VERIFY-9.3
        return;
    }
    TWidget* tw = create_empty_widget(title);          // VERIFY-9.3
    if (!tw) return;
    // The empty widget's TWidget* is its QWidget* in IDA's Qt build.
    smd_dgx_view_attach(idx, (void*)tw);
    display_widget(tw, WOPN_DP_RIGHT | WOPN_PERSIST | WOPN_RESTORE);  // VERIFY-9.3
}

struct open_view_ah_t : public action_handler_t {
    int idx;
    explicit open_view_ah_t(int i) : idx(i) {}
    int idaapi activate(action_activation_ctx_t*) override { open_view(idx); return 1; }
    action_state_t idaapi update(action_update_ctx_t*) override { return AST_ENABLE_ALWAYS; }
};

open_view_ah_t* g_handlers[SMD_DGX_VIEW_COUNT] = {};
char g_action_names[SMD_DGX_VIEW_COUNT][32];

} // namespace

void smd_dgx_register_views()
{
    for (int i = 0; i < SMD_DGX_VIEW_COUNT; ++i) {
        qsnprintf(g_action_names[i], sizeof(g_action_names[i]), "smd_dgx:view%d", i);
        g_handlers[i] = new open_view_ah_t(i);
        action_desc_t desc = ACTION_DESC_LITERAL_PLUGMOD(   // VERIFY-9.3
            g_action_names[i], smd_dgx_view_titles[i], g_handlers[i],
            nullptr, nullptr, nullptr, -1);
        register_action(desc);
        attach_action_to_menu("Debugger/Debugger windows/", g_action_names[i], SETMENU_APP);
    }
}

void smd_dgx_unregister_views()
{
    smd_dgx_view_detach_all();
    for (int i = 0; i < SMD_DGX_VIEW_COUNT; ++i) {
        unregister_action(g_action_names[i]);
        delete g_handlers[i];
        g_handlers[i] = nullptr;
    }
}
