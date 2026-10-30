"""IDAPython regression, supplied with IDA's -S option (launcher via uv).

Requires SMD_DGX_WORKSPACE and SMD_DGX_RESULT_PATH inside that workspace.
It opens no database itself and does not change analysis or input paths.
Run only on a disposable database; runtime breakpoints are added/removed.
"""
import json
import os
from pathlib import Path
import traceback

import ida_bytes
import ida_dbg
import ida_idd
import ida_ida
import ida_kernwin
import ida_loader
import ida_nalt
import ida_pro

root = Path(os.environ['SMD_DGX_WORKSPACE']).resolve()
output = Path(os.environ['SMD_DGX_RESULT_PATH']).resolve()
assert output.is_relative_to(root)
rom = root / 'disk/Pocahontas.md'
result = {'status': 'failed'}
try:
    result['gui'] = bool(ida_kernwin.is_idaq())
    result['format'] = ida_loader.get_file_type_name()
    result['processor'] = ida_ida.inf_get_procname()
    result['input'] = ida_nalt.get_input_file_path()
    assert ida_bytes.get_bytes(0x100, 16) == rom.read_bytes()[0x100:0x110]
    result['loaded'] = bool(ida_dbg.load_debugger('smd_dgx_ida.dylib', False))
    result['debugger'] = ida_idd.dbg_get_name()
    assert result['loaded'] and result['debugger'] == 'SMD DGX', result
    result['start'] = ida_dbg.start_process(str(rom), '', str(output.parent))
    assert result['start'] == 1, result
    result['startup_events'] = []
    for _ in range(5):
        event = int(ida_dbg.wait_for_next_event(ida_dbg.WFNE_ANY | ida_dbg.WFNE_SILENT | ida_dbg.WFNE_CONT, 5))
        result['startup_events'].append(event)
        if event == ida_idd.PROCESS_SUSPENDED:
            break
    assert event == ida_idd.PROCESS_SUSPENDED, result
    assert ida_dbg.get_process_state() == ida_dbg.DSTATE_SUSP, result
    result['pc_before'] = int(ida_dbg.get_reg_val('PC'))
    result['rom_bytes'] = ida_idd.dbg_read_memory(0x100, 16).hex()
    assert result['rom_bytes'] == rom.read_bytes()[0x100:0x110].hex()
    assert ida_dbg.step_into()
    result['step_event'] = int(ida_dbg.wait_for_next_event(ida_dbg.WFNE_SUSP, 5))
    result['pc_after'] = int(ida_dbg.get_reg_val('PC'))
    assert result['pc_after'] != result['pc_before'], result
    # $208 is a six-byte TST.W instruction: prove a PC write redirects actual
    # execution, not just IDA's cached register display, and preserves CPU state.
    preserved_names = [f'D{i}' for i in range(8)] + [f'A{i}' for i in range(8)] + ['SP', 'SR']
    preserved = {name: int(ida_dbg.get_reg_val(name)) for name in preserved_names}
    result['pc_write'] = bool(ida_dbg.set_reg_val('PC', 0x208))
    assert result['pc_write'], result
    result['pc_after_write'] = int(ida_dbg.get_reg_val('PC'))
    assert result['pc_after_write'] == 0x208, result
    assert {name: int(ida_dbg.get_reg_val(name)) for name in preserved_names} == preserved
    assert ida_dbg.step_into()
    result['redirect_step_event'] = int(ida_dbg.wait_for_next_event(ida_dbg.WFNE_SUSP, 5))
    result['pc_after_redirect_step'] = int(ida_dbg.get_reg_val('PC'))
    assert result['pc_after_redirect_step'] == 0x20E, result
    # Prime derived values, write their underlying registers, and read them
    # immediately in the same script without pumping UI or invalidating caches.
    original_a0 = int(ida_dbg.get_reg_val('A0'))
    ida_dbg.get_reg_val('A0_ADDR')
    assert ida_dbg.set_reg_val('A0', 0xFF0120)
    assert int(ida_dbg.get_reg_val('A0_ADDR')) == 0xFF0120
    assert ida_dbg.set_reg_val('A0', original_a0)
    assert int(ida_dbg.get_reg_val('A0_ADDR')) == (original_a0 & 0xFFFFFF)
    result['address_alias_current'] = True
    lo = int(ida_dbg.get_reg_val('LenLo'))
    hi = int(ida_dbg.get_reg_val('LenHi'))
    ida_dbg.get_reg_val('DMA_LEN')
    assert ida_dbg.set_reg_val('LenLo', lo ^ 1)
    assert int(ida_dbg.get_reg_val('DMA_LEN')) == ((hi << 8) | (lo ^ 1))
    assert ida_dbg.set_reg_val('LenHi', hi ^ 1)
    assert int(ida_dbg.get_reg_val('DMA_LEN')) == (((hi ^ 1) << 8) | (lo ^ 1))
    assert ida_dbg.set_reg_val('LenLo', lo)
    assert ida_dbg.set_reg_val('LenHi', hi)
    assert int(ida_dbg.get_reg_val('DMA_LEN')) == ((hi << 8) | lo)
    result['vdp_alias_current'] = True
    def set_stack_reg(name, value):
        assert ida_dbg.set_reg_val(name, value)
        # The plugin must keep aliases current without caller invalidation.
    before_stack = {name: int(ida_dbg.get_reg_val(name)) for name in preserved_names}
    original_sr = before_stack['SR']
    assert original_sr & 0x2000
    result['stack_writes'] = []
    for name, value in [('A7', 0xFF0400), ('SP', 0xFF0500)]:
        set_stack_reg(name, value)
        assert int(ida_dbg.get_reg_val('A7')) == value
        assert int(ida_dbg.get_reg_val('SP')) == value
        result['stack_writes'].append({'mode': 'supervisor', 'name': name, 'value': value})
    set_stack_reg('SR', original_sr & ~0x2000)
    original_user_sp = int(ida_dbg.get_reg_val('A7'))
    for name, value in [('A7', 0xFF0600), ('SP', 0xFF0700)]:
        set_stack_reg(name, value)
        assert int(ida_dbg.get_reg_val('A7')) == value
        assert int(ida_dbg.get_reg_val('SP')) == value
        result['stack_writes'].append({'mode': 'user', 'name': name, 'value': value})
    # Editing the active bank must preserve the inactive bank across SR changes.
    set_stack_reg('SR', original_sr)
    assert int(ida_dbg.get_reg_val('A7')) == 0xFF0500
    set_stack_reg('A7', before_stack['A7'])
    set_stack_reg('SR', original_sr & ~0x2000)
    assert int(ida_dbg.get_reg_val('A7')) == 0xFF0700
    set_stack_reg('A7', original_user_sp)
    set_stack_reg('SR', original_sr)
    assert {name: int(ida_dbg.get_reg_val(name)) for name in preserved_names} == before_stack
    result['stack_banks_restored'] = True
    assert ida_dbg.add_bpt(0x210, 0, ida_idd.BPT_EXEC)
    assert ida_dbg.continue_process()
    result['breakpoint_event'] = int(ida_dbg.wait_for_next_event(ida_dbg.WFNE_SUSP, 5))
    result['pc_at_breakpoint'] = int(ida_dbg.get_reg_val('PC'))
    assert result['pc_at_breakpoint'] == 0x210, result
    assert ida_dbg.del_bpt(0x210)
    assert ida_dbg.continue_process()
    assert ida_dbg.suspend_process()
    result['pause_event'] = int(ida_dbg.wait_for_next_event(ida_dbg.WFNE_SUSP, 5))
    assert ida_dbg.get_process_state() == ida_dbg.DSTATE_SUSP, result
    result['pc_after_pause'] = int(ida_dbg.get_reg_val('PC'))
    assert ida_dbg.exit_process()
    result['exit_event'] = int(ida_dbg.wait_for_next_event(ida_dbg.WFNE_ANY, 5))
    assert result['exit_event'] == ida_idd.PROCESS_EXITED, result
    result['status'] = 'passed'
except BaseException:
    result['error'] = traceback.format_exc()
finally:
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2))
    print('SMD_DGX_SMOKE:', json.dumps(result), flush=True)
    # On failure, qexit asks the debugger to terminate too.
    ida_pro.qexit(0 if result['status'] == 'passed' else 1)
