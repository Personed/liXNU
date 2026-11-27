#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Unit and socket-level protocol tests for tools/hosted/xnu-vnc.py."""

import importlib.util
import os
import pathlib
import socket
import struct
import subprocess
import sys
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

    def test_bgra_conversion_32_16_8(self):
        bgra = bytes([0x11, 0x22, 0x33, 0xFF])
        self.assertEqual(xnu_vnc.bgra_to_pixel_bytes(bgra, 32, 0, 16, 8, 0), bgra)
        # RGBX (red_shift=0, green_shift=8, blue_shift=16) little-endian
        rgbx = xnu_vnc.bgra_to_pixel_bytes(bgra, 32, 0, 0, 8, 16)
        self.assertEqual(rgbx, bytes([0x33, 0x22, 0x11, 0x00]))
        # 16-bit RGB565 (R=255, G=0, B=0 -> 0xF800)
        red_bgra = bytes([0x00, 0x00, 0xFF, 0xFF])
        rgb565 = xnu_vnc.bgra_to_pixel_bytes(red_bgra, 16, 0, 11, 5, 0, 1, 31, 63, 31)
        self.assertEqual(struct.unpack("<H", rgb565)[0], 0xF800)
        # 8-bit RGB233 (R=255, G=0, B=0 -> 0x07 at shift 0)
        rgb8 = xnu_vnc.bgra_to_pixel_bytes(red_bgra, 8, 0, 0, 3, 6, 1, 7, 7, 3)
        self.assertEqual(rgb8, bytes([0x07]))
        # Non-true-color raises ValueError
        with self.assertRaises(ValueError):
            xnu_vnc.bgra_to_pixel_bytes(red_bgra, 8, 0, 0, 0, 0, true_color=0)

    def test_refuses_non_loopback_without_flag(self):
        res = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "hosted" / "xnu-vnc.py"), "--listen", "0.0.0.0"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(res.returncode, 2)
        self.assertIn("refusing to bind", res.stderr)

    def test_rfb_handshake_update_lowcolor_and_release_on_disconnect(self):
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

                # Request non-incremental 32-bit framebuffer update
                s_cli.sendall(struct.pack("!BBHHHH", 3, 0, 0, 0, 2, 2))
                msg_type, _, n_rects = struct.unpack("!BBH", xnu_vnc.recv_exact(s_cli, 4))
                self.assertEqual((msg_type, n_rects), (0, 1))
                rx, ry, rw, rh, enc = struct.unpack("!HHHHi", xnu_vnc.recv_exact(s_cli, 12))
                self.assertEqual((rx, ry, rw, rh, enc), (0, 0, 2, 2, 0))
                pixels = xnu_vnc.recv_exact(s_cli, 16)
                self.assertEqual(pixels, fb_data)

                # Switch to 8-bit low-color true-color format (like TigerVNC low-color mode)
                pf_8 = struct.pack("!BBBBHHHBBB3x", 8, 8, 0, 1, 7, 7, 3, 0, 3, 6)
                s_cli.sendall(b"\x00\x00\x00\x00" + pf_8)
                s_cli.sendall(struct.pack("!BBHHHH", 3, 0, 0, 0, 2, 2))
                msg_type, _, n_rects = struct.unpack("!BBH", xnu_vnc.recv_exact(s_cli, 4))
                self.assertEqual((msg_type, n_rects), (0, 1))
                xnu_vnc.recv_exact(s_cli, 12)
                pixels_8 = xnu_vnc.recv_exact(s_cli, 4)
                self.assertEqual(len(pixels_8), 4)

                # Send PointerEvent (left button down) and KeyEvent ('a' down) WITHOUT releasing before disconnect
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
            # Disconnecting must release both held key and held mouse button
            self.assertIn((xnu_vnc.EV_KEY, xnu_vnc.BTN_LEFT, 0), events)
            self.assertIn((xnu_vnc.EV_KEY, 30, 0), events)

    def test_big_endian_conversion(self):
        red_bgra = bytes([0x00, 0x00, 0xFF, 0xFF])
        # 16-bit big-endian RGB565 (R=255 -> 0xF800 -> b"\xF8\x00")
        rgb565_be = xnu_vnc.bgra_to_pixel_bytes(red_bgra, 16, 1, 11, 5, 0, 1, 31, 63, 31)
        self.assertEqual(rgb565_be, b"\xf8\x00")
        # 32-bit big-endian XRGB (red_shift=16, green_shift=8, blue_shift=0 -> b"\x00\xFF\x00\x00")
        xrgb_be = xnu_vnc.bgra_to_pixel_bytes(red_bgra, 32, 1, 16, 8, 0, 1, 255, 255, 255)
        self.assertEqual(xrgb_be, b"\x00\xff\x00\x00")

    def test_console_local_falls_back_when_journal_empty(self):
        with tempfile.TemporaryDirectory() as tmp:
            bin_dir = pathlib.Path(tmp) / "bin"
            bin_dir.mkdir()
            console_in = pathlib.Path(tmp) / "console-in"
            console_in.touch()

            # Stub sudo, journalctl (returns empty, simulating ReadKMsg=no), and dmesg
            (bin_dir / "sudo").write_text('#!/bin/sh\nexec "$@"\n', encoding="utf-8")
            (bin_dir / "journalctl").write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            (bin_dir / "dmesg").write_text(
                '#!/bin/sh\nprintf "[    1.234567] fki: darwin console: fallback-ok\\r\\n"\n',
                encoding="utf-8",
            )
            for name in ("sudo", "journalctl", "dmesg"):
                os.chmod(bin_dir / name, 0o755)

            env = {
                **os.environ,
                "PATH": f"{bin_dir}:{os.environ.get('PATH', '')}",
                "CONSOLE_IN": str(console_in),
            }
            proc = subprocess.Popen(
                ["bash", str(ROOT / "scripts" / "xnu-console.sh"), "--local"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=env,
                text=True,
            )
            try:
                line = proc.stdout.readline()
                self.assertEqual(line.strip(), "fallback-ok")
                proc.stdin.write("echo hi\n")
                proc.stdin.close()
                proc.wait(timeout=5)
                self.assertEqual(proc.returncode, 0)
            finally:
                proc.stdout.close()
                proc.stderr.close()
            self.assertEqual(console_in.read_text(encoding="utf-8"), "echo hi\n")


if __name__ == "__main__":
    unittest.main()
