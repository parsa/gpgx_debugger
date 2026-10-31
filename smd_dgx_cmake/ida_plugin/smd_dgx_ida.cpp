// SMD DGX — IDA 9.2+ debugger plugin hosting the Genesis Plus GX emulator
// in-process (static link, emulator on its own thread).
//
// This is the phase-2 port of Gensida's ida_debug.cpp: the debugger_t /
// HT_IDD surface is kept 1:1, but every gRPC client call is replaced by a
// direct call into EmuHost/IDebugBackend (valid while the emulation thread is
// paused) and events arrive through EmuHost's EventSink instead of a gRPC
// server.
//
// Marked VERIFY-9.3 where the API must be checked against the 9.3 SDK once it
// is installed (written against the 9.x idd.hpp surface Gensida used).

#include <atomic>
#include <chrono>
#include <thread>
#include <memory>
#include <deque>
#include <map>
#include <mutex>
#include <string>

// IDA SDK
#include <ida.hpp>
#include <idp.hpp>
#include <idd.hpp>
#include <dbg.hpp>
#include <auto.hpp>
#include <loader.hpp>
#include <segment.hpp>
#include <expr.hpp>

// emulator (NO Qt here)
#include "debugger/EmuHost.h"
#include "debugger/BridgeServer.h"
#include "debugger/HostRunSync.h"
#include "platform/AudioOutput.h"

#include "ida_registers.h"

// ROM patching actions (ida_patch.cpp — IDA side, no Qt)
void smd_dgx_register_patches();
void smd_dgx_unregister_patches();

// Constant decoding for the listing (ida_asm.cpp)
void smd_dgx_register_asm();
void smd_dgx_unregister_asm();

// Z80-database actions: dump the live driver RAM (ida_z80_loader.cpp)
void smd_dgx_register_z80_actions();
void smd_dgx_unregister_z80_actions();

#ifdef SMD_DGX_IDA_VIEWS
// Qt-side view builder + IDA-side dock glue. Plain declarations only: this TU
// must not see Qt headers (see ida_views_shared.h).
#include "ida_views_shared.h"
#endif

#define PLUGIN_NAME "SMD DGX"
#define BREAKPOINTS_BASE 0x00D00000u   // VDP pseudo-segments: VRAM +0x00000, CRAM +0x10000, VSRAM +0x20000

// ---------------------------------------------------------------------------
// Event queue (Gensida's eventlist_t + a mutex; events are produced on the
// emulation thread and drained by IDA's debthread via ev_get_debug_event)
// ---------------------------------------------------------------------------
namespace {

// Events for IDA, and IDA's view of the run state kept consistent with the
// emulator without bridge clients ever waiting for IDA (HostRunSync.h).
HostRunSync<debug_event_t> g_sync;
EmuHost*  g_host = nullptr;

// Why the process last stopped, in the bridge's key=value format (see
// describeStop). IDA has no event field for it, so scripts read it with the
// IDC function smd_dgx_stop(). Written on the emulation thread.
std::mutex  g_stopMx;
std::string g_lastStop;

error_t idaapi idc_smd_dgx_stop(idc_value_t*, idc_value_t* res)
{
    std::lock_guard<std::mutex> lock(g_stopMx);
    res->set_string(g_lastStop.c_str());
    return eOk;
}
const char kNoArgs[] = { 0 };
const ext_idcfunc_t kStopFunc = { "smd_dgx_stop", idc_smd_dgx_stop, kNoArgs, nullptr, 0, 0 };

// smd_dgx_catch([spec]): which 68000 exceptions stop the process (see
// parseCatchSpec: none | faults | all | hex mask). Returns "mask=<hex>", or
// "error: ..." for a bad spec. Every exception is reported either way.
error_t idaapi idc_smd_dgx_catch(idc_value_t* argv, idc_value_t* res)
{
    if (!g_host) { res->set_string("error: no process"); return eOk; }
    if (argv[0].vtype == VT_STR && argv[0].c_str()[0]) {      // no argument: just report
        uint64_t mask = 0;
        if (!parseCatchSpec(argv[0].c_str(), mask)) {
            res->set_string("error: catch spec is none|faults|all|<hex mask>");
            return eOk;
        }
        g_host->gpgx()->setExceptionCatch(mask);
    }
    char out[40];
    qsnprintf(out, sizeof out, "mask=%llx", (unsigned long long)g_host->gpgx()->exceptionCatch());
    res->set_string(out);
    return eOk;
}
// smd_dgx_override([spec]): hardware-wait overrides (see applyOverrideSpec:
// none | z80bus | vdp <mask> <value> | io <addr> <1|2> <mask> <value>).
// Returns "n=<count>" or "error: ...".
error_t idaapi idc_smd_dgx_override(idc_value_t* argv, idc_value_t* res)
{
    if (!g_host) { res->set_string("error: no process"); return eOk; }
    std::vector<ReadOverride> set = g_host->gpgx()->readOverrides();
    if (!applyOverrideSpec(argv[0].vtype == VT_STR ? argv[0].c_str() : "", set)) {
        res->set_string("error: override spec is none|z80bus|vdp <mask> <value>|io <addr> <1|2> <mask> <value>");
        return eOk;
    }
    g_host->gpgx()->setReadOverrides(set);
    res->set_string(("n=" + std::to_string(set.size())).c_str());
    return eOk;
}
const char kOverrideArgs[] = { VT_STR, 0 };
const idc_value_t kOverrideDefault("");
const ext_idcfunc_t kOverrideFunc = { "smd_dgx_override", idc_smd_dgx_override, kOverrideArgs, &kOverrideDefault, 1, 0 };

const char kCatchArgs[] = { VT_STR, 0 };
const idc_value_t kCatchDefault("");
const ext_idcfunc_t kCatchFunc = { "smd_dgx_catch", idc_smd_dgx_catch, kCatchArgs, &kCatchDefault, 1, 0 };

// IDA shows a warning for an exception it does not know. Define every vector
// the core reports, so a caught one is logged and stops without a dialog.
void define_m68k_exceptions()
{
    for (unsigned v = 2; v < 64; ++v) {
        if (v >= 24 && v < 32) continue;              // interrupts: not reported
        char name[32];
        qsnprintf(name, sizeof name, "m68k_%s_%u", exceptionName(v), v);
        define_exception(v, name, name, EXC_BREAK | EXC_MSG);
    }
}

// Backend breakpoint identity (type,start,end,is_vdp): IDA's update_bpts
// names breakpoints by address/type, the backend by id.
struct BpKey {
    uint8_t type; uint8_t vdp; uint32_t start; uint32_t end;
    bool operator<(const BpKey& o) const {
        return std::tie(type, vdp, start, end) < std::tie(o.type, o.vdp, o.start, o.end);
    }
    bool operator==(const BpKey& o) const {
        return std::tie(type, vdp, start, end) == std::tie(o.type, o.vdp, o.start, o.end);
    }
};

// One IDA breakpoint and its backend breakpoint(s) (two for read+write).
//
// The backend id lives exactly as long as the IDA breakpoint. IDA's own
// notifications (dbg_bpt_changed: added/changed/removed) create the backend
// breakpoint at once and edit it in place: a condition edit, a disable or
// enable, a move, or IDA writing/unwriting it (update_bpts) never changes the
// id. Only deleting the IDA breakpoint, bpclear or a new process frees it.
// IDA writes a breakpoint added while suspended only on resume; the bridge
// still needs its id now, which is why the notifications, not update_bpts,
// create it. bptid 0: an IDA-internal breakpoint (e.g. step) known only to
// update_bpts.
struct BpPart { BpKey key; int id; };
struct BpRec  { inode_t bptid; std::vector<BpPart> parts; };
std::vector<BpRec> g_bps;
// Guards g_bps: update_bpts runs on the debugger thread, bridge requests and
// notifications on the main thread. Never held across an IDA call.
std::mutex g_bpMx;

// Audio device, alive for as long as the emulator runs. Written to from the
// emulation thread only (via the audio sink), created/destroyed around it.
AudioOutput* g_audio = nullptr;

// Control socket for external agents (the MCP server). Loopback only.
BridgeServer* g_bridge = nullptr;

// Cleared on every start_process; see region_rw.
uint32_t g_romSize = 0;

// ---------------------------------------------------------------------------
// codemap -> auto_make_code on the main thread
// ---------------------------------------------------------------------------
std::map<int, std::shared_ptr<std::atomic<bool>>> g_codemap_requests;
std::mutex g_codemap_mutex;

struct apply_codemap_req : public exec_request_t {
    std::shared_ptr<std::atomic<bool>> completed;
    std::map<uint32_t, uint32_t> changed;
    apply_codemap_req(std::map<uint32_t, uint32_t> c, std::shared_ptr<std::atomic<bool>> done)
        : completed(std::move(done)), changed(std::move(c)) {}
    ssize_t idaapi execute() override {
        for (const auto& kv : changed) {
            auto_make_code((ea_t)kv.first);
            plan_ea((ea_t)kv.first);
        }
        completed->store(true);
        // As in the SDK IDAPython wrapper, NOWAIT requests self-destruct.
        delete this;
        return 0;
    }
};

void apply_codemap(std::map<uint32_t, uint32_t> changed)
{
    if (changed.empty()) return;
    // IDA may be waiting for our pause/exit event. Never wait for its UI here.
    // MFF_WRITE preserves database write safety; NOWAIT avoids the wait cycle.
    std::lock_guard<std::mutex> lock(g_codemap_mutex);
    for (auto it = g_codemap_requests.begin(); it != g_codemap_requests.end();)
        if (it->second->load()) it = g_codemap_requests.erase(it);
        else ++it;
    auto completed = std::make_shared<std::atomic<bool>>(false);
    auto* req = new apply_codemap_req(std::move(changed), completed);
    int id = int(execute_sync(*req, MFF_WRITE | MFF_NOWAIT));
    if (!completed->load()) {
        if (id >= 0) g_codemap_requests.emplace(id, completed);
        else delete req;
    }
}

void cancel_codemap_requests()
{
    std::map<int, std::shared_ptr<std::atomic<bool>>> pending;
    {
        std::lock_guard<std::mutex> lock(g_codemap_mutex);
        pending.swap(g_codemap_requests);
    }
    for (const auto& entry : pending)
        // IDA destroys successfully cancelled requests (verified on 9.4 and 9.5).
        if (!entry.second->load()) cancel_exec_request(entry.first);
}

// ---------------------------------------------------------------------------
// EmuHost event -> IDA debug_event_t
// ---------------------------------------------------------------------------
ea_t ida_bpt_ea(int backendId, bool* hardware);

void on_emu_event(const DebugEvent& ev)
{
    debug_event_t ida_ev;               // VERIFY-9.3: field/ctor surface
    ida_ev.pid = 1;
    ida_ev.tid = 1;
    ida_ev.handled = true;

    switch (ev.type) {
    case DebugEvent::Type::Started: {
        modinfo_t& mi = ida_ev.set_modinfo(PROCESS_STARTED);   // VERIFY-9.3
        ida_ev.ea = BADADDR;
        mi.name = "GPGX";
        mi.base = 0;
        mi.size = 0;
        mi.rebase_to = BADADDR;
        g_sync.pushOther(ida_ev);
        // No synthetic suspend here: start_process() requests a real pause, so
        // the emulation thread reports PROCESS_SUSPENDED at the actual PC a
        // moment later. Enqueueing one as well made IDA stop twice on startup.
        break;
    }
    case DebugEvent::Type::Paused:
        // Nothing refills the device while suspended; silence it instead of
        // letting the last buffers drone on.
        if (g_audio) g_audio->pause(true);
        apply_codemap(ev.changed);
        {
            // The bridge's wait line carries pc/cpu/bp itself; IDA scripts get
            // them here too.
            const std::string why = describeStop(ev)
                + " pc=" + hexDigits(ev.pc & 0xFFFFFF, 6)
                + " cpu=" + (ev.cpu == Cpu::Z80 ? "z80" : "m68k")
                + " bp=" + std::to_string(ev.bpId);
            {
                std::lock_guard<std::mutex> lock(g_stopMx);
                g_lastStop = why;
            }
            // A CPU that runs nothing is worth saying out loud: it is why a
            // routine never returned.
            if (ev.reason == StopReason::Halted || ev.reason == StopReason::Stopped)
                msg(PLUGIN_NAME ": 68000 %s at %06X (%s)\n",
                    ev.reason == StopReason::Halted ? "halted (double fault)" : "stopped (STOP/wait)",
                    unsigned(ev.pc & 0xFFFFFF), why.c_str());
        }
        // A hit on one of IDA's breakpoints is IDA's BREAKPOINT event: IDA
        // then knows which breakpoint it was (and applies its own settings to
        // it: no-break/trace breakpoints, hit counts). Execute breakpoints are
        // found by the event address; a watchpoint by hea, its own address,
        // while the event address is the instruction that made the access.
        {
            bool hardware = false;
            const ea_t bptEa = (ev.reason == StopReason::Breakpoint || ev.reason == StopReason::Watchpoint)
                             ? ida_bpt_ea(ev.bpId, &hardware) : BADADDR;
            if (ev.reason == StopReason::Exception) {
                excinfo_t& exc = ida_ev.set_exception();
                exc.code = ev.exc.vector;
                exc.can_cont = true;
                exc.ea = ev.exc.access != ExcAccess::None ? ea_t(ev.exc.address) : BADADDR;
                exc.info = describeException(ev.exc).c_str();
            } else if (bptEa != BADADDR) {
                // An execute breakpoint over a range (or overlapping the
                // opcode word) can be hit by an instruction that does not
                // start at its address: then name it through hea too.
                bptaddr_t& b = ida_ev.set_bpt();
                b.hea = (hardware || bptEa != ea_t(ev.pc & 0xFFFFFF)) ? bptEa : BADADDR;
                b.kea = BADADDR;
            } else {
                ida_ev.set_eid(PROCESS_SUSPENDED);
            }
        }
        ida_ev.ea = ev.pc;
        g_sync.pushStop(ida_ev, ev.runEpoch);
        break;
    case DebugEvent::Type::Stopped:
        apply_codemap(ev.changed);
        ida_ev.set_exit_code(PROCESS_EXITED, 0);               // VERIFY-9.3
        ida_ev.ea = BADADDR;
        g_sync.pushOther(ida_ev);
        break;
    case DebugEvent::Type::Resumed:
        if (g_audio) g_audio->pause(false);
        break;
    case DebugEvent::Type::Exception:
        break;          // not caught: only stops reach IDA (and the bridge sees all)
    }
}

// ---------------------------------------------------------------------------
// Breakpoint conditions
//
// The expression is IDA's (IDC or Python), so IDA must evaluate it — and only
// on its own thread. We are called from the emulation thread while it is
// stopped at the breakpoint: the emulator is not holding anything IDA needs.
// ---------------------------------------------------------------------------
struct eval_cond_req : public exec_request_t {
    uint32_t    elang;
    std::string expr;
    bool        result = true;      // on any failure, stop rather than skip
    eval_cond_req(uint32_t l, std::string e) : elang(l), expr(std::move(e)) {}

    ssize_t idaapi execute() override {
        const extlang_object_t el = find_extlang_by_index(int(elang));
        qstring errbuf;
        idc_value_t rv;
        bool ok = false;
        if (el != nullptr)
            ok = el->eval_expr(&rv, BADADDR, expr.c_str(), &errbuf);
        else
            ok = eval_idc_expr(&rv, BADADDR, expr.c_str(), &errbuf);   // default: IDC

        if (!ok) {
            msg(PLUGIN_NAME ": breakpoint condition failed to evaluate: %s\n",
                errbuf.empty() ? "unknown error" : errbuf.c_str());
            result = true;          // a broken condition must not hide the hit
            return 0;
        }
        result = (rv.num != 0);
        return 0;
    }
};

bool execute_bridge_ui(exec_request_t& request);

bool evaluate_condition(uint32_t elang, const std::string& expr)
{
    // Cancellable like the bridge's requests: exiting the process joins this
    // (emulation) thread from IDA's thread, which therefore cannot also serve
    // a condition still waiting for it. Cancelled = stop, as on any failure.
    eval_cond_req req(elang, expr);
    if (!execute_bridge_ui(req)) return true;
    return req.result;
}


// ---------------------------------------------------------------------------
// Bridge mutations, performed the way IDA performs them
//
// An external client (the MCP server, the Z80 database) must not reach into the
// backend behind IDA's back: IDA owns the Breakpoints window and the run state,
// and there is no debug event that means "resumed" (idd.hpp event_id_t), so it
// would have no way to notice. Instead the bridge asks us, we do it through
// IDA's own API on IDA's own thread, and IDA arrives at the backend by its
// normal path — ev_resume, ev_update_bpts. One road, so nothing goes stale and
// nothing loops.
//
// These run on a bridge client thread and marshal with execute_sync. They must
// never call EmuHost::invoke: the emulation thread can itself be waiting on
// IDA (apply_codemap, eval_cond_req), and the two waits would deadlock. None of
// the IDA calls below touch the emulation thread.
// ---------------------------------------------------------------------------
// A bridge client must not block teardown by waiting for IDA's main thread.
// Submit NOWAIT, wait on shared completion, and cancel before joining clients.
struct bridge_ui_state {
    std::mutex mx;
    std::condition_variable cv;
    bool done = false;
    bool executed = false;
};
std::mutex g_bridge_ui_mutex;
bool g_bridge_ui_closed = false;
std::map<int, std::shared_ptr<bridge_ui_state>> g_bridge_ui_pending;

struct bridge_ui_req : exec_request_t {
    exec_request_t& inner;
    std::shared_ptr<bridge_ui_state> state;
    bridge_ui_req(exec_request_t& request, std::shared_ptr<bridge_ui_state> completion)
        : inner(request), state(std::move(completion)) {}
    ~bridge_ui_req() override {
        { std::lock_guard<std::mutex> lock(state->mx); state->done = true; }
        state->cv.notify_all();
    }
    ssize_t idaapi execute() override {
        try {
            inner.execute();
            std::lock_guard<std::mutex> lock(state->mx);
            state->executed = true;
        } catch (...) {
            // Never strand the waiting caller or throw across IDA's callback ABI.
        }
        delete this; // SDK NOWAIT ownership pattern; cancellation destroys it too.
        return 0;
    }
};

bool execute_bridge_ui(exec_request_t& request)
{
    auto state = std::make_shared<bridge_ui_state>();
    bridge_ui_req* wrapper = nullptr;
    int id = -1;
    bool failed = false;
    {
        std::lock_guard<std::mutex> lock(g_bridge_ui_mutex);
        if (g_bridge_ui_closed) return false;
        wrapper = new bridge_ui_req(request, state);
        id = int(execute_sync(*wrapper, MFF_WRITE | MFF_NOWAIT));
        std::lock_guard<std::mutex> completion(state->mx);
        if (!state->done) {
            if (id >= 0) g_bridge_ui_pending.emplace(id, state);
            else failed = true;
        }
    }
    if (failed) { delete wrapper; return false; }
    bool executed;
    {
        std::unique_lock<std::mutex> lock(state->mx);
        state->cv.wait(lock, [&] { return state->done; });
        executed = state->executed;
    }
    if (id >= 0) {
        std::lock_guard<std::mutex> lock(g_bridge_ui_mutex);
        g_bridge_ui_pending.erase(id);
    }
    return executed;
}

void cancel_bridge_ui_requests()
{
    std::map<int, std::shared_ptr<bridge_ui_state>> pending;
    {
        std::lock_guard<std::mutex> lock(g_bridge_ui_mutex);
        g_bridge_ui_closed = true;
        pending.swap(g_bridge_ui_pending);
    }
    for (const auto& item : pending) {
        bool done;
        { std::lock_guard<std::mutex> lock(item.second->mx); done = item.second->done; }
        if (!done) cancel_exec_request(item.first);
    }
}

// IDA holds a stop that a bridge client has run past: continue it, so it picks
// up the newer stop instead (HostRunSync). Posted without waiting; runs on the
// main thread whenever IDA gets to it. Should IDA not have applied the stop
// yet, dbg_suspend_process posts it again once it has.
std::mutex g_continue_mutex;
std::map<int, std::shared_ptr<std::atomic<bool>>> g_continue_requests;
bool g_continue_closed = false;

struct continue_req : public exec_request_t {
    std::shared_ptr<std::atomic<bool>> completed;
    explicit continue_req(std::shared_ptr<std::atomic<bool>> done) : completed(std::move(done)) {}
    ssize_t idaapi execute() override {
        if (g_sync.continueWanted() && get_process_state() == DSTATE_SUSP)
            continue_process();
        completed->store(true);
        delete this;    // NOWAIT ownership, as apply_codemap_req
        return 0;
    }
};

void post_continue()
{
    std::lock_guard<std::mutex> lock(g_continue_mutex);
    if (g_continue_closed) return;
    for (auto it = g_continue_requests.begin(); it != g_continue_requests.end();)
        if (it->second->load()) it = g_continue_requests.erase(it);
        else ++it;
    auto completed = std::make_shared<std::atomic<bool>>(false);
    auto* req = new continue_req(completed);
    const int id = int(execute_sync(*req, MFF_WRITE | MFF_NOWAIT));
    if (!completed->load()) {
        if (id >= 0) g_continue_requests.emplace(id, completed);
        else delete req;
    }
}

void cancel_continue_requests(bool close)
{
    std::map<int, std::shared_ptr<std::atomic<bool>>> pending;
    {
        std::lock_guard<std::mutex> lock(g_continue_mutex);
        g_continue_closed = close;
        pending.swap(g_continue_requests);
    }
    for (const auto& entry : pending)
        if (!entry.second->load()) cancel_exec_request(entry.first);
}

uint64_t begin_run() { return g_host ? g_host->gpgx()->beginRun() : 0; }

// A bridge client runs the machine. Never waits for IDA's main thread.
bool host_resume()
{
    if (!g_host) return false;
    g_sync.run([] { if (g_host) g_host->gpgx()->startRunning(); },
               [] { if (g_host) g_host->gpgx()->announceResumed(); });
    return true;
}

void translate_bpt(ea_t ea, int size, bpttype_t itype,
                   uint8_t& t1, uint8_t& t2, uint8_t& vdp, uint32_t& start, uint32_t& end);

// What the backend must mirror of one IDA breakpoint.
struct BptState {
    inode_t bptid = 0;
    std::vector<BpKey> keys;
    bool enabled = true;
    std::string cond;
    uint32_t elang = 0;
};

std::vector<BpKey> bpt_keys(ea_t ea, int size, bpttype_t type)
{
    uint8_t t1, t2, vdp; uint32_t start, end;
    translate_bpt(ea, size, type, t1, t2, vdp, start, end);
    std::vector<BpKey> keys{ BpKey{ t1, vdp, start, end } };
    if (t2) keys.push_back(BpKey{ t2, vdp, start, end });
    return keys;
}

BptState bpt_state(const bpt_t& b)
{
    BptState s;
    s.bptid   = b.bptid;
    s.keys    = bpt_keys(b.ea, b.size, b.type);
    s.enabled = b.enabled();
    // Conditions stay in IDA's language; we only carry them, and ask IDA to
    // judge them when the breakpoint is hit.
    if (!b.cndbody.empty()) {
        s.cond  = b.cndbody.c_str();
        s.elang = uint32_t(b.get_cnd_elang_idx());
    }
    return s;
}

// Is bpt_t::ea a real address? Not when unresolved (BADADDR), and never for a
// source-level breakpoint, whose ea is an index into IDA's own tables; such a
// breakpoint's address arrives through update_bpts when IDA installs it.
bool has_address(const bpt_t& b)
{
    return b.ea != BADADDR && b.loc.type() != BPLT_SRC;
}

// The functions below hold g_bpMx (caller) and make no IDA calls.
BpRec* find_rec_bptid(inode_t bptid)
{
    if (!bptid) return nullptr;
    for (auto& r : g_bps) if (r.bptid == bptid) return &r;
    return nullptr;
}

BpRec* find_rec_key(const BpKey& key)
{
    for (auto& r : g_bps)
        for (const auto& part : r.parts) if (part.key == key) return &r;
    return nullptr;
}

BpRec* find_rec_id(int id)
{
    for (auto& r : g_bps)
        for (const auto& part : r.parts) if (part.id == id) return &r;
    return nullptr;
}

// The IDA breakpoint address behind a backend breakpoint, or BADADDR when IDA
// has none (it never came through IDA). Emulation thread; no IDA calls.
ea_t ida_bpt_ea(int backendId, bool* hardware)
{
    if (backendId < 0) return BADADDR;
    std::lock_guard<std::mutex> lock(g_bpMx);
    const BpRec* r = find_rec_id(backendId);
    if (!r) return BADADDR;
    for (const auto& part : r->parts) {
        if (part.id != backendId) continue;
        *hardware = part.key.type != uint8_t(BpType::PC);
        return ea_t(part.key.start) + (part.key.vdp ? BREAKPOINTS_BASE : 0);
    }
    return BADADDR;
}

void remove_rec_locked(BpRec* r)
{
    if (g_host) for (const auto& part : r->parts) g_host->backend()->removeBreakpoint(part.id);
    g_bps.erase(g_bps.begin() + (r - g_bps.data()));
}

// Create the backend breakpoint(s) for an IDA breakpoint, or edit the
// existing ones in place so their ids survive. Returns the (first) id.
int sync_locked(const BptState& s)
{
    if (!g_host || s.keys.empty()) return -1;
    BpRec* r = find_rec_bptid(s.bptid);
    if (!r) {
        // Recorded by update_bpts before its IDA identity was known.
        for (const auto& key : s.keys) {
            BpRec* byKey = find_rec_key(key);
            if (byKey && (!byKey->bptid || byKey->bptid == s.bptid)) { r = byKey; break; }
        }
    }
    if (!r) { g_bps.push_back(BpRec{ s.bptid, {} }); r = &g_bps.back(); }
    if (s.bptid) r->bptid = s.bptid;
    IDebugBackend* be = g_host->backend();
    // Reuse ids by access kind first (write stays write when a watchpoint
    // becomes read+write), then any leftover, so edits never churn ids.
    std::vector<BpPart> old = r->parts, parts;
    std::vector<bool> used(old.size(), false);
    auto take = [&](uint8_t type, bool anyType) -> int {
        for (size_t j = 0; j < old.size(); ++j)
            if (!used[j] && (anyType || old[j].key.type == type)) { used[j] = true; return old[j].id; }
        return -1;
    };
    std::vector<int> ids(s.keys.size(), -1);
    for (size_t i = 0; i < s.keys.size(); ++i) ids[i] = take(s.keys[i].type, false);
    for (size_t i = 0; i < s.keys.size(); ++i) if (ids[i] < 0) ids[i] = take(0, true);
    for (size_t i = 0; i < s.keys.size(); ++i) {
        Breakpoint bp;
        bp.type      = (BpType)s.keys[i].type;
        bp.is_vdp    = s.keys[i].vdp != 0;
        bp.start     = s.keys[i].start;
        bp.end       = s.keys[i].end;
        bp.enabled   = s.enabled;
        bp.condition = s.cond;
        bp.elang     = s.elang;
        bp.id        = ids[i];
        if (bp.id < 0 || !be->updateBreakpoint(bp)) bp.id = be->addBreakpoint(bp);
        parts.push_back(BpPart{ s.keys[i], bp.id });
    }
    for (size_t j = 0; j < old.size(); ++j)
        if (!used[j]) be->removeBreakpoint(old[j].id);
    r->parts = std::move(parts);
    return r->parts[0].id;
}

// Main thread only: the backend id for IDA's breakpoint at `ea`, created now
// if IDA has not written it yet.
int track_ida_bpt_at(ea_t ea)
{
    bpt_t ib;
    if (!g_host || !get_bpt(ea, &ib) || !has_address(ib)) return -1;
    const BptState s = bpt_state(ib);
    std::lock_guard<std::mutex> lock(g_bpMx);
    return sync_locked(s);
}

// dbg_bpt_changed: mirror every IDA breakpoint edit at once, keeping the id.
void on_bpt_changed(int code, const bpt_t* b)
{
    if (!b || !g_host) return;
    // A symbolic or module-relative breakpoint IDA cannot resolve yet has no
    // address (BADADDR): nothing may be armed for it. Keep any id it already
    // has, disarmed, until IDA reports a real location.
    const bool resolved = has_address(*b);
    BptState s = bpt_state(*b);
    if (!resolved) s.keys.clear();
    std::lock_guard<std::mutex> lock(g_bpMx);
    BpRec* r = find_rec_bptid(s.bptid);
    if (!r && resolved) {
        r = find_rec_key(s.keys[0]);
        if (r && r->bptid && r->bptid != s.bptid) r = nullptr;    // someone else's
    }
    if (code == BPTEV_REMOVED) {
        if (r) remove_rec_locked(r);
        return;
    }
    if (!resolved) {
        if (!r) return;
        // No address of its own: edit only what IDA's breakpoint decides:
        // the condition, and disarming when disabled or unresolved. Whether
        // each installed location is armed is update_bpts' business.
        const bool disarm = b->ea == BADADDR || !s.enabled;
        std::vector<Breakpoint> have = g_host->backend()->getBreakpoints();
        for (const auto& part : r->parts)
            for (Breakpoint bp : have) {
                if (bp.id != part.id) continue;
                bp.condition = s.cond;
                bp.elang     = s.elang;
                if (disarm) bp.enabled = false;
                g_host->backend()->updateBreakpoint(bp);
            }
        return;
    }
    sync_locked(s);                                   // BPTEV_ADDED / BPTEV_CHANGED
}

struct add_bpt_req : public exec_request_t {
    Breakpoint bp;
    bool ok = false;
    int backendId = -1;
    explicit add_bpt_req(const Breakpoint& b) : bp(b) {}
    ssize_t idaapi execute() override {
        ea_t ea = bp.start;
        if (bp.is_vdp) ea += BREAKPOINTS_BASE;
        const asize_t size = bp.end >= bp.start ? (bp.end - bp.start + 1) : 1;
        const bpttype_t t = bp.type == BpType::Read  ? BPT_READ
                          : bp.type == BpType::Write ? BPT_WRITE
                                                     : BPT_EXEC;
        // dbg_bpt_changed has normally created the backend breakpoint already;
        // tracking is idempotent, so this only reads (or makes) its id.
        ok = add_bpt(ea, t == BPT_EXEC ? 0 : size, t);
        if (ok) backendId = track_ida_bpt_at(ea);
        return 0;
    }
};

// A Z80 breakpoint has no place in a 68000 database — its addresses belong to
// a different machine — so it goes straight to the backend and IDA is not
// involved. The Z80 database keeps its own list for those.
bool host_add_bpt(const Breakpoint& bp, int* idOut)
{
    if (!g_host || bp.cpu != Cpu::M68K) return false;

    add_bpt_req req(bp);
    if (!execute_bridge_ui(req) || !req.ok) return false;

    // The IDA-thread request reads the map before another mutation can alter it.
    if (idOut) *idOut = req.backendId;
    return true;
}

struct del_bpt_req : public exec_request_t {
    int backendId;
    bool found = false;
    explicit del_bpt_req(int id) : backendId(id) {}
    ssize_t idaapi execute() override {
        inode_t bptid = 0;
        {
            std::lock_guard<std::mutex> lock(g_bpMx);
            const BpRec* r = find_rec_id(backendId);
            if (!r) return 0;                    // not ours: Z80 or backend-only
            bptid = r->bptid;
        }
        found = true;
        // Find IDA's breakpoint by identity, not by the backend range: an
        // unresolved or source breakpoint has no usable address of its own.
        for (int i = 0; bptid && i < get_bpt_qty(); ++i) {
            bpt_t b;
            if (!getn_bpt(i, &b) || b.bptid != bptid) continue;
            // dbg_bpt_changed drops the record. If IDA refuses, keep both, so
            // the backend still matches IDA (the bridge reports the failure).
            if (!del_bpt(b.loc)) return 0;
            break;
        }
        // IDA-internal, gone from IDA already, or no notification: drop it here.
        std::lock_guard<std::mutex> lock(g_bpMx);
        if (BpRec* r = find_rec_id(backendId)) remove_rec_locked(r);
        return 0;
    }
};

bool host_del_bpt(int id)
{
    if (!g_host) return false;
    del_bpt_req req(id);
    return execute_bridge_ui(req) && req.found; // Z80 ids fall through
}

struct clear_bpts_req : public exec_request_t {
    ssize_t idaapi execute() override {
        // Walk backwards: deleting renumbers the ones after it.
        for (int i = get_bpt_qty() - 1; i >= 0; --i) {
            bpt_t b;
            if (getn_bpt(i, &b)) del_bpt(b.loc); // by location: ea may be no address
        }
        std::lock_guard<std::mutex> lock(g_bpMx);
        g_bps.clear();                           // the backend is cleared next
        return 0;
    }
};

bool host_clear_bpts()
{
    if (!g_host) return false;
    clear_bpts_req req;
    if (!execute_bridge_ui(req)) return false;
    // IDA only knows about the 68000 ones; anything else (Z80, added straight
    // to the backend) still has to go.
    g_host->backend()->clearBreakpoints();
    return true;
}

// ---------------------------------------------------------------------------
// memory routing: side-effect-free reads via typed regions
// (never touches IO read handlers; unmapped bytes read as 0, like Gens)
// ---------------------------------------------------------------------------
ssize_t region_rw(ea_t ea, void* buf, size_t size, bool write)
{
    if (!g_host) return 0;
    IDebugBackend* be = g_host->backend();
    uint8_t* p = (uint8_t*)buf;

    // ROM size from the region table. Session state, not a one-shot static:
    // a second ROM loaded into the same IDA session is a different size, and a
    // cached one from the previous game makes the new one read as zeros past
    // the old end — or shadow RAM that lives beyond it.
    if (!g_romSize)
        for (const auto& r : be->getMemRegions())
            if (r.id == 0) g_romSize = r.size;
    const uint32_t romSize = g_romSize;

    for (size_t i = 0; i < size; ++i) {
        uint32_t a = (uint32_t)(ea + i);
        int region = -1; uint32_t off = 0;
        if (a < romSize)                                { region = 0; off = a; }
        else if (a >= 0xFF0000 && a <= 0xFFFFFF)        { region = 1; off = a - 0xFF0000; }
        else if (a >= 0xA00000 && a <  0xA02000)        { region = 2; off = a - 0xA00000; }
        else if (a >= BREAKPOINTS_BASE && a < BREAKPOINTS_BASE + 0x10000)
                                                        { region = 3; off = a - BREAKPOINTS_BASE; }
        else if (a >= BREAKPOINTS_BASE + 0x10000 && a < BREAKPOINTS_BASE + 0x20000)
                                                        { region = 4; off = a - BREAKPOINTS_BASE - 0x10000; }
        else if (a >= BREAKPOINTS_BASE + 0x20000 && a < BREAKPOINTS_BASE + 0x30000)
                                                        { region = 5; off = a - BREAKPOINTS_BASE - 0x20000; }

        if (region < 0) { if (!write) p[i] = 0; continue; }
        if (write) {
            be->writeRegion(region, off, &p[i], 1);
        } else {
            auto b = be->readRegion(region, off, 1);
            p[i] = b.empty() ? 0 : b[0];
        }
    }
    return (ssize_t)size;
}

// ---------------------------------------------------------------------------
// registers
// ---------------------------------------------------------------------------
const char* const SRReg[] = {
    "C", "V", "Z", "N", "X", nullptr, nullptr, nullptr,
    "I", "I", "I", nullptr, nullptr, "S", nullptr, "T",
};

register_info_t registers[] = {
    { "D0", REGISTER_ADDRESS, RC_GENERAL, dt_dword, nullptr, 0 },
    { "D1", REGISTER_ADDRESS, RC_GENERAL, dt_dword, nullptr, 0 },
    { "D2", REGISTER_ADDRESS, RC_GENERAL, dt_dword, nullptr, 0 },
    { "D3", REGISTER_ADDRESS, RC_GENERAL, dt_dword, nullptr, 0 },
    { "D4", REGISTER_ADDRESS, RC_GENERAL, dt_dword, nullptr, 0 },
    { "D5", REGISTER_ADDRESS, RC_GENERAL, dt_dword, nullptr, 0 },
    { "D6", REGISTER_ADDRESS, RC_GENERAL, dt_dword, nullptr, 0 },
    { "D7", REGISTER_ADDRESS, RC_GENERAL, dt_dword, nullptr, 0 },

    { "A0", 0, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A0_ADDR", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A1", 0, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A1_ADDR", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A2", 0, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A2_ADDR", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A3", 0, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A3_ADDR", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A4", 0, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A4_ADDR", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A5", 0, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A5_ADDR", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A6", 0, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A6_ADDR", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A7", 0, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A7_ADDR", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },

    { "PC", REGISTER_ADDRESS | REGISTER_IP, RC_GENERAL, dt_dword, nullptr, 0 },
    { "SP", REGISTER_ADDRESS | REGISTER_SP, RC_GENERAL, dt_dword, nullptr, 0 },
    { "SR", 0, RC_GENERAL, dt_word, SRReg, 0xFFFF },

    { "DMA_LEN", REGISTER_READONLY, RC_GENERAL, dt_word, nullptr, 0 },
    { "DMA_SRC", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },
    { "VDP_DST", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },

    { "Set1",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "Set2",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "PlaneA", 0, RC_VDP, dt_byte, nullptr, 0 },
    { "Window", 0, RC_VDP, dt_byte, nullptr, 0 },
    { "PlaneB", 0, RC_VDP, dt_byte, nullptr, 0 },
    { "Sprite", 0, RC_VDP, dt_byte, nullptr, 0 },
    { "Reg6",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "BgClr",  0, RC_VDP, dt_byte, nullptr, 0 },
    { "Reg8",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "Reg9",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "HInt",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "Set3",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "Set4",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "HScrl",  0, RC_VDP, dt_byte, nullptr, 0 },
    { "Reg14",  0, RC_VDP, dt_byte, nullptr, 0 },
    { "WrInc",  0, RC_VDP, dt_byte, nullptr, 0 },
    { "ScrSz",  0, RC_VDP, dt_byte, nullptr, 0 },
    { "WinX",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "WinY",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "LenLo",  0, RC_VDP, dt_byte, nullptr, 0 },
    { "LenHi",  0, RC_VDP, dt_byte, nullptr, 0 },
    { "SrcLo",  0, RC_VDP, dt_byte, nullptr, 0 },
    { "SrcMid", 0, RC_VDP, dt_byte, nullptr, 0 },
    { "SrcHi",  0, RC_VDP, dt_byte, nullptr, 0 },
};

const char* register_classes[] = {
    "General Registers",
    "VDP Registers",
    nullptr,
};

drc_t read_registers(int clsmask, regval_t* values)
{
    if (!g_host) return DRC_FAILED;
    IDebugBackend* be = g_host->backend();

    if (clsmask & RC_GENERAL) {
        M68kRegs r = be->getM68kRegs();
        for (int i = 0; i < 8; ++i) values[R_D0 + i].ival = r.d[i];
        for (int i = 0; i < 8; ++i) {
            values[R_A0 + i * 2].ival     = r.a[i];
            values[R_A0_ADDR + i * 2].ival = r.a[i] & 0xFFFFFF;
        }
        values[R_PC].ival = r.pc & 0xFFFFFF;
        values[R_SP].ival = r.a[7] & 0xFFFFFF;
        values[R_SR].ival = r.sr;

        VdpState v = be->getVdpState();
        values[R_VDP_DMA_LEN].ival = v.dma_len;
        values[R_VDP_DMA_SRC].ival = v.dma_src;      // already a byte address
        // Where the next data-port write lands, as an address IDA can follow
        // into the VDP pseudo-segments.
        uint32_t dst = BREAKPOINTS_BASE;
        switch (v.vdp_code & 0x0F) {
        case 0x1: dst += VDP_BP_VRAM  + v.vdp_addr; break;   // VRAM write
        case 0x3: dst += VDP_BP_CRAM  + v.vdp_addr; break;   // CRAM write
        case 0x5: dst += VDP_BP_VSRAM + v.vdp_addr; break;   // VSRAM write
        default:  break;                                     // reads/idle: base
        }
        values[R_VDP_WRITE_ADDR].ival = dst;
    }
    if (clsmask & RC_VDP) {
        VdpState v = be->getVdpState();
        for (int i = 0; i < 24; ++i)
            values[R_V00 + i].ival = v.reg[i];
    }
    return DRC_OK;
}

bool refresh_register_cache()
{
    struct cache_req_t : exec_request_t {
        ssize_t idaapi execute() override {
            invalidate_dbg_state(DBGINV_REGS);
            return 0;
        }
    } req;
    // Register callbacks may run in debthread. This UI-cache operation does
    // not query/modify the database: FAST can run while IDA waits for the
    // callback, unlike READ/WRITE requests which can wait for database idle.
    // Wait for completion so a subsequent get_reg_val sees current aliases.
    return execute_sync(req, MFF_FAST) == 0;
}

drc_t write_register(int regidx, const regval_t* value)
{
    if (!g_host) return DRC_FAILED;
    IDebugBackend* be = g_host->backend();

    if (regidx >= R_D0 && regidx <= R_SR) {
        M68kRegs r = be->getM68kRegs();
        if (regidx <= R_D7)          r.d[regidx - R_D0] = (uint32_t)value->ival;
        else if (regidx < R_PC && ((regidx - R_A0) % 2) == 0)
                                     r.a[(regidx - R_A0) / 2] = (uint32_t)value->ival;
        else if (regidx == R_PC)     r.pc = (uint32_t)value->ival & 0xFFFFFF;
        else if (regidx == R_SP)     r.a[7] = (uint32_t)value->ival;
        else if (regidx == R_SR)     r.sr = (uint32_t)value->ival;
        else return DRC_OK;          // ADDR pseudo-regs are read-only
        be->setM68kRegs(r);
        // A7/SP, SR-selected banks and address views share underlying values.
        // Do not leave the other names in IDA's cache at their pre-write values.
        return refresh_register_cache() ? DRC_OK : DRC_FAILED;
    }
    if (regidx >= R_V00 && regidx <= R_V23) {
        be->setVdpReg(regidx - R_V00, (uint8_t)(value->ival & 0xFF));
        return refresh_register_cache() ? DRC_OK : DRC_FAILED;
    }
    return DRC_OK;
}

// ---------------------------------------------------------------------------
// breakpoints
// ---------------------------------------------------------------------------
void translate_bpt(ea_t ea, int size, bpttype_t itype,
                   uint8_t& t1, uint8_t& t2, uint8_t& vdp, uint32_t& start, uint32_t& end)
{
    start = (uint32_t)ea;
    end   = (uint32_t)(ea + (size ? size : 1) - 1);
    t2 = 0; vdp = 0;
    switch (itype) {
    case BPT_EXEC:  t1 = (uint8_t)BpType::PC;    break;
    case BPT_READ:  t1 = (uint8_t)BpType::Read;  break;
    case BPT_WRITE: t1 = (uint8_t)BpType::Write; break;
    case BPT_RDWR:  t1 = (uint8_t)BpType::Read; t2 = (uint8_t)BpType::Write; break;
    default:        t1 = (uint8_t)BpType::PC;    break;
    }
    if (start >= BREAKPOINTS_BASE && end < BREAKPOINTS_BASE + 0x30000) {
        start -= BREAKPOINTS_BASE;
        end   -= BREAKPOINTS_BASE;
        vdp = 1;
    }
    start &= 0xFFFFFF;
    end   &= 0xFFFFFF;
}

// update_bpts (debugger thread): IDA writing or unwriting breakpoints. The
// notifications normally created the backend breakpoint already; writing it
// enables it, unwriting a breakpoint IDA still has disables it, and only a
// breakpoint gone from IDA is removed. Its id never changes.
drc_t update_bpts(int* nbpts, update_bpt_info_t* bpts, int nadd, int ndel)
{
    if (!g_host) return DRC_FAILED;
    int ok = 0;
    for (int i = 0; i < nadd + ndel; ++i) {
        update_bpt_info_t& u = bpts[i];
        if (u.code == BPT_SKIP) continue;
        const bool add = i < nadd;
        const std::vector<BpKey> keys = bpt_keys(u.ea, (int)u.size, u.type);
        bpt_t ib;
        // get_bpt finds any breakpoint *containing* the address. Only the very
        // same breakpoint is this user breakpoint; anything else (an internal
        // step breakpoint inside a watched range) is tracked on its own.
        // A source breakpoint has no address of its own to compare.
        const bool known = get_bpt(u.ea, &ib)        // IDA call: before locking
                        && (has_address(ib) ? bpt_keys(ib.ea, ib.size, ib.type) == keys
                                            : ib.loc.type() == BPLT_SRC);
        BptState s = known ? bpt_state(ib) : BptState{};
        s.keys = keys;
        if (known && !has_address(ib)) {
            // A source breakpoint may be installed at several addresses, one
            // update at a time. Each installed location is its own part with
            // its own id; (un)writing one arms or disarms just that part.
            std::lock_guard<std::mutex> lock(g_bpMx);
            BpRec* r = find_rec_bptid(ib.bptid);
            if (!r && add) { g_bps.push_back(BpRec{ ib.bptid, {} }); r = &g_bps.back(); }
            for (size_t k = 0; r && k < keys.size(); ++k) {
                Breakpoint bp;
                bp.type      = (BpType)keys[k].type;
                bp.is_vdp    = keys[k].vdp != 0;
                bp.start     = keys[k].start;
                bp.end       = keys[k].end;
                bp.enabled   = add;
                bp.condition = s.cond;
                bp.elang     = s.elang;
                BpPart* part = nullptr;
                for (auto& p : r->parts) if (p.key == keys[k]) part = &p;
                if (part) {
                    bp.id = part->id;
                    if (!g_host->backend()->updateBreakpoint(bp))
                        part->id = g_host->backend()->addBreakpoint(bp);
                } else if (add) {
                    r->parts.push_back(BpPart{ keys[k], g_host->backend()->addBreakpoint(bp) });
                }
            }
            u.code = BPT_OK;
            ++ok;
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(g_bpMx);
            if (add) {
                s.enabled = true;                    // being written: it is live
                sync_locked(s);
            } else if (BpRec* r = find_rec_key(s.keys[0])) {
                if (!r->bptid) {
                    remove_rec_locked(r);            // IDA-internal: gone for good
                } else if (known && r->bptid == ib.bptid) {
                    s.enabled = false;               // unwritten but still in IDA
                    s.keys.clear();                  // keep its current range
                    for (const auto& part : r->parts) s.keys.push_back(part.key);
                    sync_locked(s);
                }
                // Otherwise IDA is removing an old installation of an edited
                // (retyped/moved) breakpoint: dbg_bpt_changed already synced
                // it, and only BPTEV_REMOVED deletes an IDA breakpoint.
            }
        }
        u.code = BPT_OK;
        ++ok;
    }
    *nbpts = ok;
    return DRC_OK;
}

// ---------------------------------------------------------------------------
// process lifecycle
// ---------------------------------------------------------------------------
void stop_audio()
{
    delete g_audio;
    g_audio = nullptr;
}

drc_t start_process(const char* path, const char* input_path)
{
    // The bridge first: its handlers hold this EmuHost*, and its event sink is
    // registered on it. Deleting the host underneath a live BridgeServer left
    // both dangling for as long as a client stayed connected.
    if (g_bridge) g_bridge->requestStop();
    cancel_bridge_ui_requests();
    delete g_bridge; g_bridge = nullptr;
    { std::lock_guard<std::mutex> lock(g_bridge_ui_mutex); g_bridge_ui_closed = false; }

    delete g_host;
    g_host = new EmuHost();
    g_romSize = 0;
    {
        std::lock_guard<std::mutex> lock(g_bpMx);
        g_bps.clear();
    }
    stop_audio();
    g_sync.clear();
    g_sync.setPostContinue(post_continue);
    g_sync.setEpochSource([] { return g_host ? g_host->gpgx()->runEpoch() : uint64_t(0); });
    cancel_continue_requests(false);

    g_host->addEventSink(on_emu_event);
    g_host->backend()->setConditionEvaluator(evaluate_condition);

    // Bridge clients must not mutate run state or breakpoints behind IDA's
    // back. Running goes through g_sync (never waiting for IDA); breakpoints
    // through IDA itself. See EmuHost::HostMutations.
    EmuHost::HostMutations hm;
    hm.resume           = host_resume;
    hm.addBreakpoint    = host_add_bpt;
    hm.removeBreakpoint = host_del_bpt;
    hm.clearBreakpoints = host_clear_bpts;
    g_host->setHostMutations(hm);
#ifdef SMD_DGX_IDA_VIEWS
    g_host->setFrameSink(smd_dgx_push_frame);   // no-op unless the Screen dock is open
#endif
    g_audio = new AudioOutput();
    if (!g_audio->isOpen())
        msg(PLUGIN_NAME ": no audio device — the emulator will run silently\n");
    g_host->setAudioSink([](const int16_t* stereo, int frames) {
        if (g_audio) g_audio->write(stereo, frames);
    });

    const char* rom = (input_path && input_path[0]) ? input_path : path;
    if (!g_host->start(rom ? rom : "")) {
        delete g_host;
        g_host = nullptr;
        stop_audio();
        return DRC_FAILED;
    }
    // start paused at the entry: request a pause right away
    g_host->backend()->pause();

    g_bridge = new BridgeServer(g_host);
    if (g_bridge->start())
        msg(PLUGIN_NAME ": control socket on 127.0.0.1:%u\n", g_bridge->port());
    else
        msg(PLUGIN_NAME ": control socket unavailable (port busy?)\n");

    return DRC_OK;
}

// ---------------------------------------------------------------------------
// HT_IDD callback — same dispatch table as Gensida
// ---------------------------------------------------------------------------
ssize_t idaapi debugger_callback(void*, int msgid, va_list va)
{
    drc_t retcode = DRC_NONE;

    switch (msgid) {
    case debugger_t::ev_init_debugger:
    case debugger_t::ev_term_debugger:
        retcode = DRC_OK;
        break;

    case debugger_t::ev_get_processes: {
        procinfo_vec_t* procs = va_arg(va, procinfo_vec_t*);
        process_info_t& pi = procs->push_back();   // VERIFY-9.3
        pi.pid = 1;
        pi.name = "GPGX";
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_start_process: {
        const char* path      = va_arg(va, const char*);
        const char* args      = va_arg(va, const char*);      (void)args;
        const char* startdir  = va_arg(va, const char*);      (void)startdir;
        uint32 flags          = va_arg(va, uint32);           (void)flags;
        const char* input     = va_arg(va, const char*);
        uint32 crc            = va_arg(va, uint32);           (void)crc;
        retcode = start_process(path, input);
        break;
    }

    case debugger_t::ev_get_debapp_attrs: {
        debapp_attrs_t* attrs = va_arg(va, debapp_attrs_t*);
        attrs->addrsize = 4;
        attrs->is_be = true;
        attrs->platform = "sega_md";
        attrs->cbsize = sizeof(debapp_attrs_t);
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_request_pause:
        if (g_host) g_host->backend()->pause();
        retcode = DRC_OK;
        break;

    case debugger_t::ev_exit_process:
        // Reject/cancel host-side work before joining any bridge client.
        if (g_bridge) g_bridge->requestStop();
        cancel_bridge_ui_requests();
        cancel_continue_requests(true);
        // Close the socket before the host: its handler holds an EmuHost*.
        delete g_bridge; g_bridge = nullptr;
        if (g_host) { g_host->stop(); }
        stop_audio();
        retcode = DRC_OK;
        break;

    case debugger_t::ev_get_debug_event: {
        gdecode_t* code = va_arg(va, gdecode_t*);
        debug_event_t* event = va_arg(va, debug_event_t*);
        bool more = false;
        *code = g_sync.retrieve(event, &more)
                    ? (more ? GDE_MANY_EVENTS : GDE_ONE_EVENT)
                    : GDE_NO_EVENT;
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_resume: {
        debug_event_t* event = va_arg(va, debug_event_t*);
        dbg_notification_t req = get_running_notification();
        const bool run = req == dbg_null || req == dbg_run_to;   // not a step: set_resume_mode did that
        bool stop = false;
        switch (event->eid()) {
        case STEP: case PROCESS_SUSPENDED: case EXCEPTION: case BREAKPOINT: stop = true; break;
        default: break;
        }
        // False when a bridge client already ran the machine past the stop IDA
        // is resuming from: it must not be resumed again from a newer one; and
        // resumeIfEpoch declines if one does so between here and there.
        uint64_t epoch = 0;
        const bool drive = g_sync.releaseHold(run && stop ? begin_run : std::function<uint64_t()>(), &epoch);
        switch (event->eid()) {
        case STEP:
        case PROCESS_SUSPENDED:
        case EXCEPTION:
        case BREAKPOINT:
            if (drive && run && g_host) g_host->gpgx()->resumeIfEpoch(epoch);
            break;
        case PROCESS_EXITED:
            break;
        default: break;
        }
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_thread_suspend:
        if (g_host) g_host->backend()->pause();
        retcode = DRC_OK;
        break;

    case debugger_t::ev_thread_continue: {
        uint64_t epoch = 0;
        if (g_sync.releaseHold(begin_run, &epoch) && g_host) g_host->gpgx()->resumeIfEpoch(epoch);
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_set_resume_mode: {
        thid_t tid = va_argi(va, thid_t);           (void)tid;
        resume_mode_t resmod = va_argi(va, resume_mode_t);
        if (!g_host) { retcode = DRC_FAILED; break; }
        // If the machine already left the stop IDA would step from (a bridge
        // client ran it), there is nothing to step: IDA's resume follows and
        // finds the newer stop queued, if there is one. stepIfEpoch declines
        // the same way if that happens between here and the step.
        uint64_t epoch = 0;
        if ((resmod == RESMOD_INTO || resmod == RESMOD_OVER) && !g_sync.releaseHold(begin_run, &epoch)) {
            retcode = DRC_OK;
            break;
        }
        switch (resmod) {
        case RESMOD_INTO: g_host->gpgx()->stepIfEpoch(Cpu::M68K, false, epoch); retcode = DRC_OK; break;
        case RESMOD_OVER: g_host->gpgx()->stepIfEpoch(Cpu::M68K, true, epoch);  retcode = DRC_OK; break;
        default:          retcode = DRC_FAILED; break;
        }
        break;
    }

    case debugger_t::ev_read_registers: {
        thid_t tid  = va_argi(va, thid_t);          (void)tid;
        int clsmask = va_arg(va, int);
        regval_t* values = va_arg(va, regval_t*);
        retcode = read_registers(clsmask, values);
        break;
    }

    case debugger_t::ev_write_register: {
        thid_t tid = va_argi(va, thid_t);           (void)tid;
        int regidx = va_arg(va, int);
        const regval_t* value = va_arg(va, const regval_t*);
        retcode = write_register(regidx, value);
        break;
    }

    case debugger_t::ev_get_memory_info: {
        meminfo_vec_t* ranges = va_arg(va, meminfo_vec_t*);
        memory_info_t info;
        for (int i = 0; i < get_segm_qty(); ++i) {
            segment_t* s = getnseg(i);
            info.start_ea = s->start_ea;
            info.end_ea   = s->end_ea;
            qstring buf;
            get_segm_name(&buf, s);  info.name = buf;
            get_segm_class(&buf, s); info.sclass = buf;
            info.sbase = 0;
            info.perm = SEGPERM_READ | SEGPERM_WRITE;
            info.bitness = 1;
            ranges->push_back(info);
        }
        static const char* const vdp_names[] = { "DBG_VDP_VRAM", "DBG_VDP_CRAM", "DBG_VDP_VSRAM" };
        for (int i = 0; i < 3; ++i) {
            info.name = vdp_names[i];
            info.start_ea = BREAKPOINTS_BASE + 0x10000u * i;
            info.end_ea   = info.start_ea + 0x10000;
            info.bitness = 1;
            ranges->push_back(info);
        }
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_read_memory: {
        size_t* nbytes = va_arg(va, size_t*);
        ea_t ea = va_arg(va, ea_t);
        void* buffer = va_arg(va, void*);
        size_t size = va_arg(va, size_t);
        *nbytes = (size_t)region_rw(ea, buffer, size, false);
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_write_memory: {
        size_t* nbytes = va_arg(va, size_t*);
        ea_t ea = va_arg(va, ea_t);
        void* buffer = va_arg(va, void*);
        size_t size = va_arg(va, size_t);
        *nbytes = (size_t)region_rw(ea, buffer, size, true);
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_check_bpt: {
        int* bptvc = va_arg(va, int*);
        bpttype_t type = va_argi(va, bpttype_t);
        switch (type) {
        case BPT_EXEC: case BPT_READ: case BPT_WRITE: case BPT_RDWR:
            *bptvc = BPT_OK; break;
        default:
            *bptvc = BPT_BAD_TYPE; break;
        }
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_update_bpts: {
        int* nbpts = va_arg(va, int*);
        update_bpt_info_t* bpts = va_arg(va, update_bpt_info_t*);
        int nadd = va_arg(va, int);
        int ndel = va_arg(va, int);
        retcode = update_bpts(nbpts, bpts, nadd, ndel);
        break;
    }

    // Callstack: deferred (backend does not track it yet) — DRC_NONE keeps
    // IDA's default behavior.
    case debugger_t::ev_update_call_stack: {
        thid_t tid = va_argi(va, thid_t);   (void)tid;
        call_stack_t* trace = va_arg(va, call_stack_t*);
        if (!g_host || !trace) { retcode = DRC_NONE; break; }
        trace->clear();
        // Backend order is outermost-first, which is what IDA expects.
        for (uint32_t ea : g_host->backend()->getCallstack(Cpu::M68K)) {
            call_stack_info_t& f = trace->push_back();
            f.callea = ea;
            f.funcea = BADADDR;
            f.fp     = BADADDR;
            f.funcok = true;
        }
        retcode = DRC_OK;
        break;
    }

    default:
        retcode = DRC_NONE;
        break;
    }
    return retcode;
}

} // namespace

// ---------------------------------------------------------------------------
// DEBUGGER DESCRIPTION BLOCK — layout kept from Gensida  (VERIFY-9.3)
// ---------------------------------------------------------------------------
debugger_t debugger = {
    IDD_INTERFACE_VERSION,
    PLUGIN_NAME,
    0x8000 + 1,
    "m68k",

    DBG_FLAG_NOHOST | DBG_FLAG_CAN_CONT_BPT | DBG_FLAG_SAFE | DBG_FLAG_FAKE_ATTACH
        | DBG_FLAG_NOPASSWORD | DBG_FLAG_NOSTARTDIR | DBG_FLAG_NOPARAMETERS
        | DBG_FLAG_ANYSIZE_HWBPT | DBG_FLAG_DEBTHREAD | DBG_FLAG_PREFER_SWBPTS
        | DBG_HAS_GET_PROCESSES | DBG_HAS_REQUEST_PAUSE | DBG_HAS_SET_RESUME_MODE
        | DBG_HAS_CHECK_BPT | DBG_HAS_THREAD_SUSPEND | DBG_HAS_THREAD_CONTINUE,

    register_classes,
    RC_GENERAL,
    registers,
    qnumber(registers),

    0x1000,        // memory page size

    nullptr, 0, 0, // bpt bytes

    DBG_RESMOD_STEP_INTO | DBG_RESMOD_STEP_OVER,
};

// ---------------------------------------------------------------------------
// plugin — modern 9.x idiom: PLUGIN_MULTI + plugmod_t that listens on HT_IDD
// (same structure as the SDK's own dbg modules, src/dbg/common_local_impl.cpp)
// ---------------------------------------------------------------------------

// accessor for the Qt-side view builder (this TU has no Qt, so the pointer
// crosses the boundary as-is)
EmuHost* smd_dgx_host() { return g_host; }


// --- Z80 half (smd_dgx_ida_z80.cpp) ---------------------------------------
// Same binary, different debugger_t: which one is registered depends on the
// database's processor, and the Z80 one attaches to the emulator this one runs.
extern debugger_t z80_debugger;
ssize_t idaapi smd_dgx_z80_event(int msgid, va_list va);
void smd_dgx_z80_shutdown();

struct smd_dgx_z80_plugmod_t : public plugmod_t, public event_listener_t {
    smd_dgx_z80_plugmod_t()
    {
        hook_event_listener(HT_IDD, this);
        dbg = &z80_debugger;
        smd_dgx_register_z80_actions();
        msg(PLUGIN_NAME ": Z80 debugger loaded (attaches to a running emulator)\n");
    }

    ~smd_dgx_z80_plugmod_t() override
    {
        smd_dgx_unregister_z80_actions();
        smd_dgx_z80_shutdown();
        if (dbg == &z80_debugger)
            dbg = nullptr;
    }

    // Same guard as the 68000 half: ::dbg is process-wide, and this table has
    // 26 registers where the other has 54.
    ssize_t idaapi on_event(ssize_t code, va_list va) override
    {
        if (dbg != &z80_debugger) return DRC_NONE;
        return smd_dgx_z80_event((int)code, va);
    }

    bool idaapi run(size_t) override { return false; }
};

struct smd_dgx_plugmod_t : public plugmod_t, public event_listener_t {
    // HT_DBG needs its own listener object: this one already answers HT_IDD.
    struct bpt_listener_t : public event_listener_t {
        ssize_t idaapi on_event(ssize_t code, va_list va) override
        {
            if (code == dbg_bpt_changed) {
                const int bptev = va_arg(va, int);
                const bpt_t* b = va_arg(va, bpt_t*);
                on_bpt_changed(bptev, b);
            } else if (code == dbg_suspend_process) {
                // IDA has now applied a stop that a bridge client already ran
                // past (the continue posted then found IDA not yet suspended).
                if (g_host && g_sync.continueWanted()) post_continue();
            }
            return 0;
        }
    } bpt_listener;      // unhooked automatically when destroyed

    smd_dgx_plugmod_t()
    {
        hook_event_listener(HT_IDD, this);
        hook_event_listener(HT_DBG, &bpt_listener);
        add_idc_func(kStopFunc);
        add_idc_func(kCatchFunc);
        add_idc_func(kOverrideFunc);
        define_m68k_exceptions();
        dbg = &debugger;
#ifdef SMD_DGX_IDA_VIEWS
        if (is_idaq()) smd_dgx_register_views();
#endif
        smd_dgx_register_patches();
        smd_dgx_register_asm();
        smd_dgx_register_z80_actions();
        msg(PLUGIN_NAME ": in-process GPGX debugger loaded\n");
    }

    ~smd_dgx_plugmod_t() override
    {
        if (g_bridge) g_bridge->requestStop();
        cancel_bridge_ui_requests();
        cancel_continue_requests(true);
        del_idc_func(kStopFunc.name);
        del_idc_func(kCatchFunc.name);
        del_idc_func(kOverrideFunc.name);
#ifdef SMD_DGX_IDA_VIEWS
        if (is_idaq()) smd_dgx_unregister_views();
#endif
        smd_dgx_unregister_patches();
        smd_dgx_unregister_asm();
        smd_dgx_unregister_z80_actions();
        delete g_bridge; g_bridge = nullptr;
        if (g_host) { g_host->stop(); delete g_host; g_host = nullptr; }
        cancel_codemap_requests();
        stop_audio();
        if (dbg == &debugger)
            dbg = nullptr;
        // event listeners hooked via plugmod_t are auto-unhooked on delete
    }

    // Only answer for the debugger IDA currently has selected.
    //
    // `dbg` is one slot for the whole process, and every debugger plugin
    // assigns to it — including this DLL's other half. IDA sizes its register
    // buffer from ::dbg (idd.hpp), and the two tables here are 54 registers and
    // 26, so answering an event meant for the other one writes past the end of
    // a buffer someone else allocated. This is not multi-database exotica: IDA
    // loads several debugger plugins for one database as a matter of course.
    bool is_ours() const { return dbg == &debugger; }

    ssize_t idaapi on_event(ssize_t code, va_list va) override
    {
        if (!is_ours()) return DRC_NONE;
        return debugger_callback(nullptr, (int)code, va);
    }

    bool idaapi run(size_t) override { return false; }
};

static plugmod_t* idaapi init()
{
    // One binary, two debuggers. A database is either the game (68000, runs
    // the emulator) or the sound driver (Z80, attaches to it); anything else
    // is none of our business.
    if (PH.id == PLFM_68K)
        return new smd_dgx_plugmod_t;
    if (PH.id == PLFM_Z80)
        return new smd_dgx_z80_plugmod_t;
    return nullptr;
}

plugin_t PLUGIN = {
    IDP_INTERFACE_VERSION,
    PLUGIN_MULTI | PLUGIN_HIDE | PLUGIN_DBG,
    init,
    nullptr,   // term: unused with PLUGIN_MULTI
    nullptr,   // run:  unused with PLUGIN_MULTI
    PLUGIN_NAME " debugger plugin (in-process Genesis Plus GX)",
    nullptr,
    PLUGIN_NAME " debugger",
    nullptr,
};
