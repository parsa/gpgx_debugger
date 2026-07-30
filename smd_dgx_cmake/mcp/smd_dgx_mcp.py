"""MCP server for the SMD DGX debugger.

Talks to the control socket opened by the IDA plugin (or any other host that
runs a BridgeServer), so an agent drives the *same* emulator session the user
is looking at, not a private copy.

The C++ side speaks a deliberately dumb line protocol; everything structured
lives here. Start the emulator in IDA first ("start process"), then run this.

    pip install mcp
    python smd_dgx_mcp.py
"""

from __future__ import annotations

import base64
import os
import socket
import struct
import threading
import zlib

from mcp.server.fastmcp import FastMCP, Image

HOST = os.environ.get("SMD_DGX_HOST", "127.0.0.1")
PORT = int(os.environ.get("SMD_DGX_PORT", "27042"))

mcp = FastMCP("smd-dgx")


class Bridge:
    """One reconnecting line-protocol connection, serialised by a lock.

    The socket serves a single client at a time, so concurrent tool calls must
    not interleave on it.
    """

    def __init__(self) -> None:
        self._sock: socket.socket | None = None
        self._buf = b""
        self._lock = threading.Lock()

    def _connect(self) -> socket.socket:
        if self._sock is None:
            s = socket.create_connection((HOST, PORT), timeout=10)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self._sock, self._buf = s, b""
        return self._sock

    def _drop(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock, self._buf = None, b""

    def command(self, line: str) -> str:
        """Send one command, return the reply without its 'ok ' prefix.

        Raises RuntimeError on an 'err ...' reply or a dead connection.
        """
        with self._lock:
            for attempt in (1, 2):  # one silent retry: IDA may have restarted
                try:
                    sock = self._connect()
                    sock.sendall(line.encode() + b"\n")
                    while b"\n" not in self._buf:
                        chunk = sock.recv(65536)
                        if not chunk:
                            raise ConnectionError("bridge closed")
                        self._buf += chunk
                    raw, self._buf = self._buf.split(b"\n", 1)
                    break
                except (OSError, ConnectionError):
                    self._drop()
                    if attempt == 2:
                        raise RuntimeError(
                            f"cannot reach the SMD DGX bridge at {HOST}:{PORT} — "
                            "is the emulator started in IDA?"
                        )

        reply = raw.decode(errors="replace").strip()
        if reply.startswith("err"):
            raise RuntimeError(reply[3:].strip() or "command failed")
        return reply[2:].strip() if reply.startswith("ok") else reply


bridge = Bridge()


def _kv(reply: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for tok in reply.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def _png(rgb565: bytes, w: int, h: int) -> bytes:
    """Encode RGB565 as PNG using only the stdlib."""
    rows = bytearray()
    for y in range(h):
        rows.append(0)  # filter type: none
        row = rgb565[y * w * 2 : (y + 1) * w * 2]
        for x in range(w):
            p = row[x * 2] | (row[x * 2 + 1] << 8)
            r, g, b = (p >> 11) & 0x1F, (p >> 5) & 0x3F, p & 0x1F
            # widen 5/6-bit channels so full-scale stays full-scale
            rows += bytes(((r * 255) // 31, (g * 255) // 63, (b * 255) // 31))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 6))
            + chunk(b"IEND", b""))


def _hex(value: str) -> str:
    """Normalise a hex address for the wire.

    Not str.lstrip('0x'): that strips *characters*, so "0" and "0x0" both become
    the empty string, the bridge sees one argument fewer than it expects, and
    every later argument shifts by one. Address zero is not an exotic input —
    it is the top of the 68000 vector table.
    """
    s = str(value).strip()
    if s[:2].lower() == "0x":
        s = s[2:]
    s = s.lstrip("0")
    return s or "0"


# ---------------------------------------------------------------------------
# state
# ---------------------------------------------------------------------------
@mcp.tool()
def status() -> str:
    """Whether the emulator is running, whether it is paused, and the current PC."""
    kv = _kv(bridge.command("status"))
    state = "paused" if kv.get("paused") == "1" else "running"
    if kv.get("running") != "1":
        state = "not running"
    return f"{state}, pc={kv.get('pc', '?')}"


@mcp.tool()
def get_registers(cpu: str = "m68k") -> dict[str, str]:
    """Read CPU registers. cpu is 'm68k' (default) or 'z80'. Values are hex."""
    return _kv(bridge.command("regsz80" if cpu.lower() in ("z80", "z-80") else "regs68k"))


@mcp.tool()
def get_vdp_state() -> dict[str, str]:
    """Read VDP registers (reg = 24 bytes hex), status, and DMA length/source/type."""
    return _kv(bridge.command("vdp"))


# ---------------------------------------------------------------------------
# memory
# ---------------------------------------------------------------------------
@mcp.tool()
def read_memory(address: str, size: int) -> str:
    """Read from the 68k bus. address is hex (e.g. 'FF0000'); returns hex bytes."""
    return bridge.command(f"read {_hex(address)} {size}")


@mcp.tool()
def write_memory(address: str, data_hex: str) -> str:
    """Write hex bytes to the 68k bus. address is hex."""
    bridge.command(f"write {_hex(address)} {data_hex}")
    return "written"


@mcp.tool()
def list_regions() -> list[dict[str, str]]:
    """List memory regions (ROM, RAM 68K, RAM Z80, VRAM, CRAM, VSRAM, register banks)."""
    out = []
    for tok in bridge.command("regions").split():
        parts = tok.split(":")
        if len(parts) == 5:
            out.append({"id": parts[0], "name": parts[1], "base": parts[2],
                        "size": parts[3], "writable": parts[4]})
    return out


@mcp.tool()
def read_region(region_id: int, offset: str = "0", size: int = 256) -> str:
    """Read from a region by id (see list_regions). offset is hex; returns hex bytes."""
    return bridge.command(f"readregion {region_id} {_hex(offset)} {size}")


# ---------------------------------------------------------------------------
# run control
# ---------------------------------------------------------------------------
@mcp.tool()
def pause() -> str:
    """Pause the emulator at the next instruction."""
    bridge.command("pause")
    return status()


@mcp.tool()
def resume() -> str:
    """Resume execution."""
    bridge.command("resume")
    return "resumed"


@mcp.tool()
def step(over: bool = False) -> str:
    """Execute one instruction. over=True steps over jsr/bsr."""
    bridge.command("stepo" if over else "stepi")
    return status()


@mcp.tool()
def add_breakpoint(address: str, kind: str = "x", end: str | None = None) -> str:
    """Add a breakpoint. kind: 'x' execute, 'r' read, 'w' write. Addresses are hex."""
    a = _hex(address)
    return "breakpoint id " + bridge.command(f"bpadd {kind} {a} {_hex(end or address)}")


@mcp.tool()
def list_breakpoints() -> list[dict[str, str]]:
    """List breakpoints: id, kind (x/r/w), start, end, cpu, vdp, enabled."""
    out = []
    for tok in bridge.command("bplist").split():
        p = tok.split(",")
        # The server sends seven fields; requiring exactly four made this
        # return an empty list for every breakpoint that has ever existed.
        # Accept four so an older host still works, and fill the rest in.
        if len(p) < 4:
            continue
        out.append({
            "id": p[0], "kind": p[1], "start": p[2], "end": p[3],
            "cpu":     p[4] if len(p) > 4 else "m68k",
            "vdp":     p[5] if len(p) > 5 else "0",
            "enabled": p[6] if len(p) > 6 else "1",
        })
    return out


@mcp.tool()
def remove_breakpoint(bp_id: int) -> str:
    """Remove a breakpoint by id."""
    bridge.command(f"bpdel {bp_id}")
    return "removed"


# ---------------------------------------------------------------------------
# interaction
# ---------------------------------------------------------------------------
BUTTONS = {"up": 0x001, "down": 0x002, "left": 0x004, "right": 0x008,
           "b": 0x010, "c": 0x020, "a": 0x040, "start": 0x080,
           "z": 0x100, "y": 0x200, "x": 0x400, "mode": 0x800}


@mcp.tool()
def set_buttons(buttons: list[str]) -> str:
    """Hold exactly these controller buttons (empty list releases everything).

    Names: up, down, left, right, a, b, c, x, y, z, start, mode.
    The pad stays in this state until changed, so press then release to tap.
    """
    mask = 0
    for name in buttons:
        key = name.strip().lower()
        if key not in BUTTONS:
            raise ValueError(f"unknown button {name!r}; valid: {', '.join(BUTTONS)}")
        mask |= BUTTONS[key]
    bridge.command(f"pad {mask:x}")
    return f"pad = {sorted(b.lower() for b in buttons) or 'released'}"


@mcp.tool()
def screenshot() -> Image:
    """Capture what is on screen right now, as a PNG."""
    reply = bridge.command("frame")
    w_s, h_s, b64 = reply.split(" ", 2)
    w, h = int(w_s), int(h_s)
    return Image(data=_png(base64.b64decode(b64), w, h), format="png")


# ---------------------------------------------------------------------------
# save states
# ---------------------------------------------------------------------------
@mcp.tool()
def save_state(path: str) -> str:
    """Save a save state to an absolute path. Use it to bookmark a scene."""
    bridge.command(f"savestate {path}")
    return f"saved to {path}"


@mcp.tool()
def load_state(path: str) -> str:
    """Restore a save state previously written by save_state."""
    bridge.command(f"loadstate {path}")
    return f"loaded {path}"


if __name__ == "__main__":
    mcp.run()
