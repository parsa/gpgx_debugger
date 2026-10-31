# Headless IDA debugging (macOS IDA 9.5)

The installed `smd_dgx_ida.dylib` now supports **headless `idat`** as well as GUI IDA. This was verified on native arm64 IDA Pro 9.5, built against the 9.5 SDK and linked to IDA 9.5's namespaced Qt 6.12 frameworks. Separate `idalib` embedding and other operating systems are not covered by these runtime tests.

## Restart the existing headless worker

Save your database, release/close its MCP session, and reopen it in a **new headless worker**. Do not recreate the database or change its loader. A worker which already attempted to load the old plugin may retain the failed load; an existing GUI process may still have the old binary mapped too.

Use the database's original processor and analysis. A raw-binary database works: it need not have been loaded through SMD DGX. The database must use the 68000 processor module (the Z80 half is a different debugger).

In the new session, execute through MCP's IDAPython tool:

```python
import ida_dbg
import ida_idd
import ida_nalt

print(ida_dbg.load_debugger("smd_dgx_ida.dylib", False))  # True
print(ida_idd.dbg_get_name())                          # SMD DGX
print(ida_nalt.get_input_file_path())
```

Use `load_debugger`, not `load_plugin`. The complete macOS filename including `.dylib` is required.

Then start using the full game ROM path:

```python
rc = ida_dbg.start_process("/absolute/path/to/Pocahontas.md", "", "/working/directory")
print(rc)  # 1 means started; 0 means cancelled; -1 means creation failed
```

The plugin currently prefers the **database input path**, if nonempty, over the executable argument. That input must therefore point to an existing complete game ROM (`.bin` is fine too), not a missing file or extracted fragment. Passing another executable path alone does not override it. Inspect it before changing anything in a valuable database. The IDAPython setter in IDA 9.5 is `ida_nalt.set_root_filename(full_rom_path)` (not `set_input_file_path`). Existing labels, functions, and loader-created segment metadata do not determine whether the emulator can load the ROM.

IDA's startup includes PROCESS_STARTED and PROCESS_SUSPENDED events. Drain startup events before stepping; the smoke test demonstrates this. Native `ida_dbg` calls for register/memory reads, single stepping, execute breakpoints, resume, pause, and exit work without a GUI or special ROM configuration file. Timeout parameters to `wait_for_next_event` are **seconds**.

## Verified through this session's actual MCP server

A copy of the user's saved raw-binary database was opened through `mcp__ida.open_database`, headless backend `idat`, instance **`95c1c2a35cec`**. It retained **930 functions**. Every debugger operation was then performed through `mcp__ida.execute_python`, not an external test launcher.

Its saved input pointed to a nonexistent `pocahontas-decomp/extracted/ida/Pocahontas.bin`, causing `start_process()` to return 0 despite a valid executable argument. Only the workspace copy's input path was changed to `disk/Pocahontas.md`, using `ida_nalt.set_root_filename`. Startup then returned 1. MCP verified ROM reads, PC `$200 → $206`, execute breakpoint `$210`, explicit pause `$214`, exit event, and successful restart paused at `$200`. Function count remained 930. The corrected copy was saved at `.setup/runtime/headless/Pocahontas-mcp.i64`; the original user database was untouched.

Evidence: `.setup/runtime/headless/mcp-verification.json`. The instance was intentionally left running and paused for further use; its ID is session-dependent, not a permanent identifier. The user explicitly allowed this existing MCP server's runtime logs/state under `~/.idapro/`; our database and evidence remain workspace-local.

## Setting the 68000 PC

The rebuilt plugin exposes PC as writable while paused:

```python
assert ida_dbg.get_process_state() == ida_dbg.DSTATE_SUSP
assert ida_dbg.set_reg_val("PC", 0x208)
print(hex(int(ida_dbg.get_reg_val("PC"))))
```

This updates the emulator's actual execution PC (24-bit address), not merely a database cursor or cached display. A subsequent single step executes from the new address. The regression writes `$208` in Pocahontas and steps its six-byte TST.W instruction to `$20E`, preserving all other CPU registers during the PC write. Read-only address pseudo-registers and Z80 PC are unchanged. The subsequent A7/SP fix below is also included in the latest build.

A fresh MCP worker verified this with all 930 functions preserved: instance `2844d253b51b`, database `.setup/runtime/headless/Pocahontas-pc-write.i64`, evidence `.setup/runtime/headless/mcp-pc-write.json`. Existing workers retain their previously mapped plugin and need a new worker to gain PC-write support; installation replaces the module atomically rather than overwriting mapped code. The original `95c1c2a35cec` instance was not closed or reset by this test.

## Setting A7 / SP and switching stack banks

The latest backend fixes sparse A7/SP writes being undone by stale USP/ISP snapshot fields. An edited A7 updates the bank selected by the target SR; the inactive bank is preserved. SR-only changes still select the saved bank, and bank-only USP/ISP edits remain supported.

```python
assert ida_dbg.get_process_state() == ida_dbg.DSTATE_SUSP
assert ida_dbg.set_reg_val("A7", 0xFF0400)  # "SP" also works
print(hex(int(ida_dbg.get_reg_val("SP"))))  # immediately current; no invalidation needed
```

The plugin now invalidates IDA's register cache internally after successful writes. Reading SP immediately after writing A7, A7 after writing SP, or the active bank after switching SR needs no caller workaround. Address views and VDP-derived registers such as DMA_LEN are refreshed too. A7 is a full 32-bit register; the SP address view is masked to the 24-bit bus, so values above the bus width can intentionally differ.

Four new backend regressions verify supervisor/user A7 writes, saved-bank switching via SR, and explicit bank-only writes. Both GUI and headless IDA smoke tests write through A7 and SP in both modes, immediately read the aliases without invalidation, verify inactive banks survive, restore both banks, and retain PC-write/stepping/breakpoint/pause/exit checks. All 51 core cases pass.

Same-server MCP verification: instance **`c2991f080f23`**, database `.setup/runtime/headless/Pocahontas-registers.i64`, 930 functions retained. Evidence: `.setup/runtime/headless/mcp-stack-write.json`. Previous instances have older mapped modules; this new worker includes both PC and stack fixes. The relocated standalone bundle also passes a live A7/active-bank write-and-restore check (`.setup/logs/sp-bundle.log`).

## Automatic IDA alias-cache refresh

A subsequent regression removed caller-side invalidation and reproduced stale SP after an A7 write. Successful CPU and VDP register writes now synchronously marshal IDA's register-cache invalidation to the main thread with `execute_sync(MFF_FAST)`, without changing process state. FAST performs only UI-cache work (no database access), so completion does not wait for database idle; the write reports success only after the refresh completes. Immediate same-script reads pass for A7/SP in both modes, SR-selected banks, A0_ADDR after A0 writes, and DMA_LEN after VDP LenLo/LenHi writes. No deferred UI tick, caller invalidation, or instruction execution is required.

Latest verified MCP worker: **`e32b439c32dd`** on `.setup/runtime/headless/Pocahontas-aliases.i64`, 930 functions preserved, paused at `$200` with original stack banks restored. Evidence `.setup/runtime/headless/mcp-alias-write.json` explicitly records `manual_invalidation: false`. Native/GUI regression logs: `.setup/logs/alias-raw.log`, `.setup/logs/alias-gui.log`; before-fix failure: `.setup/logs/alias-red.log`. Older working instances retain their plugin mappings and were left untouched. A temporary prototype worker was closed before verifying the final main-thread-marshalled implementation.

## Stop reasons, exceptions and overrides from scripts

IDA's events carry no cause, so the plugin registers IDC functions (callable from IDAPython with `ida_expr.eval_idc_expr`):

- `smd_dgx_stop()` describes the last stop: `reason=<breakpoint|watchpoint|step|pause|exception|halted|stopped|frame|return|address|budget>` plus its details (watchpoint access, exception vector/fault PC/SR/access) and `pc= cpu= bp=`.
- `smd_dgx_catch("none|faults|all|<hex mask>")` chooses which 68000 exception vectors stop. A caught exception arrives as an IDA `EXCEPTION` event whose code is the vector, with the same description as its info. The exceptions are pre-defined with break+log flags, so no dialog appears.
- `smd_dgx_override("none|z80bus|vdp <mask> <value>|io <addr> <1|2> <mask> <value>")` sets hardware-wait overrides (off by default).

A halted (double fault) or STOPped 68000 now suspends: `suspend_process()` lands within a frame and `smd_dgx_stop()` says `halted` or `stopped`. Setting PC restarts such a CPU.

Headless IDA processes debugger events only inside a wait. A script that lets a bridge client drive run control (for example `runcase`) must keep calling `wait_for_next_event(WFNE_ANY, 0)` meanwhile, or IDA never sees the stops.

## What was fixed

Previously the macOS plugin was linked with `-undefined dynamic_lookup`, relying on GUI IDA to preload its namespaced Qt libraries. Headless IDA did not preload Qt, so its loader rejected the plugin with:

```
symbol not found in flat namespace '__ZN2QT10QByteArray6_emptyE'
```

It now explicitly links **IDA's own QtCore/QtGui/QtWidgets frameworks**, resolving them through the application bundle. It does not link Homebrew Qt or deploy another Qt into IDA. Headless initialization skips dock-view registration. No QApplication/window creation or DYLD preload workaround is required.

CMake locates the frameworks relative to `IDA_LIB`; if linking against an SDK stub instead of the installed library, set `IDA_QT_FRAMEWORK_DIR` to `IDA.app/Contents/Frameworks`.

## Verification

All tests used disposable workspace databases; no user analysis database was modified.

- `.setup/logs/headless-smd.log`: fresh idat database using the SMD DGX ROM loader.
- `.setup/logs/headless-raw.log`: fresh idat raw Binary file database, processor 68000.
- Both: no DYLD/Qt preload or search overrides; plugin loaded, process start returned 1, ROM reads matched, PC stepped `$200 → $206`, resumed to execute breakpoint `$210`, explicitly paused at `$214`, process exited, and idat exited 0.
- `.setup/logs/headless-gui-regression.log`: the same installed plugin still passes GUI run control and clean teardown.
- `.setup/logs/headless-ctest.log`: both CTest targets pass (47 core cases).

Runtime regression code: `smd_dgx_cmake/tests/ida_debugger_smoke.py`, executed by IDAPython via `-S`. Launchers run through uv and set workspace-local output/profile/cache paths:

```bash
source .setup/env.sh
uv run --no-project --python /opt/homebrew/bin/python3.11 python .setup/run_headless_smoke.py smd
uv run --no-project --python /opt/homebrew/bin/python3.11 python .setup/run_headless_smoke.py raw
```

The plugin was rebuilt and installed at `/Applications/IDA Professional 9.5.app/Contents/MacOS/plugins/smd_dgx_ida.dylib`.
