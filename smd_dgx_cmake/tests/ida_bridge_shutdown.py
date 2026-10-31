"""Run on a disposable IDA database: exit with a queued bridge mutation."""
import json
import os
from pathlib import Path
import socket
import threading
import time
import ida_dbg
import ida_idd
import ida_pro

root = Path(os.environ['SMD_DGX_WORKSPACE'])
assert ida_dbg.load_debugger('smd_dgx_ida.dylib', False)
assert ida_dbg.start_process(str(root / 'disk/Pocahontas.md'), '', str(root / '.setup/runtime/headless')) == 1
for _ in range(5):
    event = ida_dbg.wait_for_next_event(ida_dbg.WFNE_ANY | ida_dbg.WFNE_SILENT | ida_dbg.WFNE_CONT, 5)
    if event == ida_idd.PROCESS_SUSPENDED:
        break
assert ida_dbg.get_process_state() == ida_dbg.DSTATE_SUSP
sent = threading.Event()
replied = threading.Event()

def mutate():
    with socket.create_connection(('127.0.0.1', int(os.environ['SMD_DGX_PORT'])), timeout=5) as client:
        with client.makefile('rwb') as stream:
            stream.write(b'ping\n'); stream.flush()
            assert stream.readline().startswith(b'ok')
            stream.write(b'bpadd x 300 300\n'); stream.flush()
            sent.set()
            stream.readline()
            replied.set()

threading.Thread(target=mutate, daemon=True).start()
assert sent.wait(5)
# Keep IDA's main thread busy so MFF_WRITE cannot run before teardown.
time.sleep(0.2)
assert not replied.is_set()
Path(os.environ['SMD_DGX_RESULT_PATH']).write_text(json.dumps({'queued_bridge_request': True, 'exit_requested': True}))
ida_pro.qexit(0)
