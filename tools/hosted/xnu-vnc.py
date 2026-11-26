#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Lightweight RFB/VNC server for xnulinux's framebuffer dump and evdev input file.

Uses only the Python 3 standard library so any PC with a VNC client can view the
hosted macOS/PureDarwin screen and send mouse/keyboard input without extra packages.

    xnu-vnc.py [--fb /var/tmp/xnu-fb.raw] [--input /var/tmp/xnu-input]
               [--screen 1280x800] [--listen 127.0.0.1] [--port 5900]
"""

import argparse
import os
import select
import socket
import struct
import sys
import time

EV_SYN, EV_KEY, EV_REL, EV_ABS = 0, 1, 2, 3
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE = 0x110, 0x111, 0x112
REL_WHEEL = 8

# X11 keysym -> Linux evdev keycode (input-event-codes.h)
KEYSYM_TO_EVDEV = {
    0xFF1B: 1,    # Escape
    0xFF08: 14,   # BackSpace
    0xFF09: 15,   # Tab
    0xFF0D: 28,   # Return
    0xFF8D: 28,   # KP_Enter
    0xFFE3: 29,   # Control_L
    0xFFE4: 97,   # Control_R
    0xFFE1: 42,   # Shift_L
    0xFFE2: 54,   # Shift_R
    0xFFE9: 56,   # Alt_L
    0xFFEA: 100,  # Alt_R
    0xFFE7: 125,  # Meta_L -> Command
    0xFFE8: 126,  # Meta_R
    0xFFEB: 125,  # Super_L -> Command
    0xFFEC: 126,  # Super_R
    0xFFE5: 58,   # Caps_Lock
    0x0020: 57,   # space
    0xFF50: 102,  # Home
    0xFF51: 105,  # Left
    0xFF52: 103,  # Up
    0xFF53: 106,  # Right
    0xFF54: 108,  # Down
    0xFF55: 104,  # Page_Up
    0xFF56: 109,  # Page_Down
    0xFF57: 107,  # End
    0xFFFF: 111,  # Delete (forward)
}
for _i, _ch in enumerate("1234567890"):
    KEYSYM_TO_EVDEV[ord(_ch)] = 2 + _i
for _row, _start in (("qwertyuiop", 16), ("asdfghjkl", 30), ("zxcvbnm", 44)):
    for _i, _ch in enumerate(_row):
        KEYSYM_TO_EVDEV[ord(_ch)] = _start + _i
        KEYSYM_TO_EVDEV[ord(_ch.upper())] = _start + _i
for _ch, _code in (
    ("-", 12), ("_", 12), ("=", 13), ("+", 13), ("[", 26), ("{", 26), ("]", 27), ("}", 27),
    (";", 39), (":", 39), ("'", 40), ('"', 40), ("`", 41), ("~", 41), ("\\", 43), ("|", 43),
    (",", 51), ("<", 51), (".", 52), (">", 52), ("/", 53), ("?", 53),
):
    KEYSYM_TO_EVDEV[ord(_ch)] = _code
for _sym, _code in zip("!@#$%^&*()", range(2, 12)):
    KEYSYM_TO_EVDEV[ord(_sym)] = _code
for _i in range(1, 11):
    KEYSYM_TO_EVDEV[0xFFBD + _i] = 58 + _i  # F1..F10
KEYSYM_TO_EVDEV[0xFFC8] = 87  # F11
KEYSYM_TO_EVDEV[0xFFC9] = 88  # F12


def keysym_to_evdev(keysym):
    return KEYSYM_TO_EVDEV.get(keysym)


def bgra_to_pixel_bytes(raw_bgra, bpp, big_endian, red_shift, green_shift, blue_shift):
    """Convert raw 32-bit BGRA framebuffer bytes to the client's requested pixel format."""
    if bpp == 32 and not big_endian and red_shift == 16 and green_shift == 8 and blue_shift == 0:
        return raw_bgra
    if bpp != 32:
        raise ValueError(f"unsupported bpp={bpp} (only 32-bit true-color supported)")
    n = len(raw_bgra) // 4
    out = bytearray(n * 4)
    endian = ">" if big_endian else "<"
    for i in range(n):
        b = raw_bgra[i * 4]
        g = raw_bgra[i * 4 + 1]
        r = raw_bgra[i * 4 + 2]
        px = (r << red_shift) | (g << green_shift) | (b << blue_shift)
        struct.pack_into(endian + "I", out, i * 4, px)
    return bytes(out)


class InputWriter:
    def __init__(self, path, width, height):
        self.path = path
        self.width = max(2, width)
        self.height = max(2, height)
        self.prev_buttons = 0

    def _write_events(self, events):
        if not self.path or not events:
            return
        buf = b"".join(struct.pack("<HHi", t, c, v) for t, c, v in events)
        with open(self.path, "ab", buffering=0) as f:
            f.write(buf)

    def handle_key(self, down, keysym):
        code = keysym_to_evdev(keysym)
        if code is None:
            return
        self._write_events([(EV_KEY, code, 1 if down else 0), (EV_SYN, 0, 0)])

    def handle_pointer(self, mask, x, y):
        x = max(0, min(self.width - 1, x))
        y = max(0, min(self.height - 1, y))
        events = [
            (EV_ABS, 0, int(x * 32767 / (self.width - 1))),
            (EV_ABS, 1, int(y * 32767 / (self.height - 1))),
        ]
        for bit, code in ((1, BTN_LEFT), (2, BTN_MIDDLE), (4, BTN_RIGHT)):
            cur = 1 if (mask & bit) else 0
            prev = 1 if (self.prev_buttons & bit) else 0
            if cur != prev:
                events.append((EV_KEY, code, cur))
        if mask & 8:
            events.append((EV_REL, REL_WHEEL, 1))
        if mask & 16:
            events.append((EV_REL, REL_WHEEL, -1))
        self.prev_buttons = mask & 7
        events.append((EV_SYN, 0, 0))
        self._write_events(events)


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("client disconnected")
        buf.extend(chunk)
    return bytes(buf)


def read_fb(path, width, height):
    expected = width * height * 4
    try:
        with open(path, "rb") as f:
            data = f.read(expected)
    except OSError:
        data = b""
    if len(data) < expected:
        data = data + b"\x00" * (expected - len(data))
    return data


def serve_client(conn, fb_path, input_writer, width, height, fps):
    # 1. RFB handshake
    conn.sendall(b"RFB 003.008\n")
    ver = recv_exact(conn, 12)
    if not ver.startswith(b"RFB "):
        return
    minor = int(ver[8:11])
    if minor >= 7:
        conn.sendall(struct.pack("!BB", 1, 1))  # 1 security type: None (1)
        sec = recv_exact(conn, 1)[0]
        if sec != 1:
            return
        conn.sendall(struct.pack("!I", 0))  # OK
    else:
        conn.sendall(struct.pack("!I", 1))  # SecurityType None

    recv_exact(conn, 1)  # ClientInit (shared flag)

    # ServerInit: 32-bit little-endian BGRA (red_shift=16, green_shift=8, blue_shift=0)
    bpp, depth, big_endian, true_color = 32, 24, 0, 1
    r_shift, g_shift, b_shift = 16, 8, 0
    pixfmt = struct.pack("!BBBBHHHBBB3x", bpp, depth, big_endian, true_color, 255, 255, 255, r_shift, g_shift, b_shift)
    name = b"xnulinux"
    conn.sendall(struct.pack("!HH", width, height) + pixfmt + struct.pack("!I", len(name)) + name)

    last_fb = None
    pending_update = False
    pending_incremental = True
    interval = 1.0 / max(1, fps)
    last_send = 0.0

    while True:
        timeout = interval if pending_update else 0.25
        r, _, _ = select.select([conn], [], [], timeout)
        if r:
            msg_type = recv_exact(conn, 1)[0]
            if msg_type == 0:  # SetPixelFormat
                recv_exact(conn, 3)
                pf = recv_exact(conn, 16)
                bpp, depth, big_endian, true_color, _, _, _, r_shift, g_shift, b_shift = struct.unpack(
                    "!BBBBHHHBBB3x", pf
                )
                last_fb = None
            elif msg_type == 2:  # SetEncodings
                recv_exact(conn, 1)
                n_enc = struct.unpack("!H", recv_exact(conn, 2))[0]
                recv_exact(conn, n_enc * 4)
            elif msg_type == 3:  # FramebufferUpdateRequest
                inc, _x, _y, _w, _h = struct.unpack("!BHHHH", recv_exact(conn, 9))
                pending_update = True
                pending_incremental = bool(inc)
            elif msg_type == 4:  # KeyEvent
                down = recv_exact(conn, 1)[0]
                recv_exact(conn, 2)
                keysym = struct.unpack("!I", recv_exact(conn, 4))[0]
                input_writer.handle_key(bool(down), keysym)
            elif msg_type == 5:  # PointerEvent
                mask, px, py = struct.unpack("!BHH", recv_exact(conn, 5))
                input_writer.handle_pointer(mask, px, py)
            elif msg_type == 6:  # ClientCutText
                recv_exact(conn, 3)
                clen = struct.unpack("!I", recv_exact(conn, 4))[0]
                recv_exact(conn, clen)
            else:
                break

        now = time.monotonic()
        if pending_update and (not pending_incremental or now - last_send >= interval):
            raw = read_fb(fb_path, width, height)
            if not pending_incremental or raw != last_fb:
                pixels = bgra_to_pixel_bytes(raw, bpp, big_endian, r_shift, g_shift, b_shift)
                hdr = struct.pack("!BxH", 0, 1) + struct.pack("!HHHHi", 0, 0, width, height, 0)
                conn.sendall(hdr + pixels)
                last_fb = raw
                last_send = now
                pending_update = False


def main():
    p = argparse.ArgumentParser(description="VNC server for xnulinux framebuffer and evdev input")
    p.add_argument("--fb", default=os.environ.get("FB_DUMP", "/var/tmp/xnu-fb.raw"))
    p.add_argument("--input", default=os.environ.get("INPUT_IN", "/var/tmp/xnu-input"))
    p.add_argument("--screen", default="1280x800")
    p.add_argument("--listen", default="127.0.0.1")
    p.add_argument("--port", type=int, default=5900)
    p.add_argument("--fps", type=int, default=15)
    args = p.parse_args()

    w_s, h_s = args.screen.lower().split("x", 1)
    width, height = int(w_s), int(h_s)
    writer = InputWriter(args.input, width, height)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.listen, args.port))
    srv.listen(4)
    print(f"xnu-vnc: listening on {args.listen}:{args.port} ({width}x{height}, fb={args.fb}, input={args.input})", flush=True)

    try:
        while True:
            conn, addr = srv.accept()
            try:
                serve_client(conn, args.fb, writer, width, height, args.fps)
            except (EOFError, BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                conn.close()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()


if __name__ == "__main__":
    main()
