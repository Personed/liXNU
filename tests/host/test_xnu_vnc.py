#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Unit and socket-level protocol tests for tools/hosted/xnu-vnc.py."""

import importlib.util
import os
import pathlib
import socket
import struct
import tempfile
import threading
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("xnu_vnc", ROOT / "tools" / "hosted" / "xnu-vnc.py")
xnu_vnc = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(xnu_vnc)


class TestXnuVnc(unittest.TestCase):
    def test_keysym_mapping(self):
        self.assertEqual(xnu_vnc.keysym_to_evdev(ord("a")), 30)
        self.assertEqual(xnu_vnc.keysym_to_evdev(ord("A")), 30)
        self.assertEqual(xnu_vnc.keysym_to_evdev(0xFF0D), 28)   # Return
        self.assertEqual(xnu_vnc.keysym_to_evdev(0xFFEB), 125)  # Super_L -> Command
        self.assertEqual(xnu_vnc.keysym_to_evdev(0xFF52), 103)  # Up

    def test_bgra_conversion(self):
        bgra = bytes([0x11, 0x22, 0x33, 0xFF])
        self.assertEqual(xnu_vnc.bgra_to_pixel_bytes(bgra, 32, 0, 16, 8, 0), bgra)
        # RGBX (red_shift=0, green_shift=8, blue_shift=16) little-endian
        rgbx = xnu_vnc.bgra_to_pixel_bytes(bgra, 32, 0, 0, 8, 16)
        self.assertEqual(rgbx, bytes([0x33, 0x22, 0x11, 0x00]))

    def test_rfb_handshake_update_and_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            fb_path = os.path.join(tmp, "fb.raw")
            in_path = os.path.join(tmp, "in.ev")
            # 2x2 BGRA framebuffer
            fb_data = bytes([0x10, 0x20, 0x30, 0xFF] * 4)
            with open(fb_path, "wb") as f:
                f.write(fb_data)

            writer = xnu_vnc.InputWriter(in_path, 2, 2)
            s_srv, s_cli = socket.socketpair()
            def run_srv():
                try:
                    xnu_vnc.serve_client(s_srv, fb_path, writer, 2, 2, 30)
                except EOFError:
                    pass

            t = threading.Thread(target=run_srv, daemon=True)
            t.start()
            try:
                ver = xnu_vnc.recv_exact(s_cli, 12)
                self.assertEqual(ver, b"RFB 003.008\n")
                s_cli.sendall(b"RFB 003.008\n")

                n_sec, sec_type = struct.unpack("!BB", xnu_vnc.recv_exact(s_cli, 2))
                self.assertEqual((n_sec, sec_type), (1, 1))
                s_cli.sendall(b"\x01")
                self.assertEqual(struct.unpack("!I", xnu_vnc.recv_exact(s_cli, 4))[0], 0)

                s_cli.sendall(b"\x01")  # ClientInit
                srv_init = xnu_vnc.recv_exact(s_cli, 24)
                w, h = struct.unpack("!HH", srv_init[:4])
                name_len = struct.unpack("!I", srv_init[20:24])[0]
                name = xnu_vnc.recv_exact(s_cli, name_len)
                self.assertEqual((w, h, name), (2, 2, b"xnulinux"))

                # Request non-incremental framebuffer update
                s_cli.sendall(struct.pack("!BBHHHH", 3, 0, 0, 0, 2, 2))
                msg_type, _, n_rects = struct.unpack("!BBH", xnu_vnc.recv_exact(s_cli, 4))
                self.assertEqual((msg_type, n_rects), (0, 1))
                rx, ry, rw, rh, enc = struct.unpack("!HHHHi", xnu_vnc.recv_exact(s_cli, 12))
                self.assertEqual((rx, ry, rw, rh, enc), (0, 0, 2, 2, 0))
                pixels = xnu_vnc.recv_exact(s_cli, 16)
                self.assertEqual(pixels, fb_data)

                # Send PointerEvent (left click at 1, 1) and KeyEvent ('a' down)
                s_cli.sendall(struct.pack("!BBHH", 5, 1, 1, 1))
                s_cli.sendall(struct.pack("!BB2xI", 4, 1, ord("a")))
            finally:
                s_cli.close()
                t.join(timeout=2)
                s_srv.close()

            with open(in_path, "rb") as f:
                raw_ev = f.read()
            events = [struct.unpack("<HHi", raw_ev[i:i + 8]) for i in range(0, len(raw_ev), 8)]
            self.assertIn((xnu_vnc.EV_ABS, 0, 32767), events)
            self.assertIn((xnu_vnc.EV_KEY, xnu_vnc.BTN_LEFT, 1), events)
            self.assertIn((xnu_vnc.EV_KEY, 30, 1), events)


if __name__ == "__main__":
    unittest.main()
