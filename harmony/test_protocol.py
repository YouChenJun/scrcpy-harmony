#!/usr/bin/env python3
"""Live protocol regression: restart, clipboard round-trip, UHID and native resolution."""
import json
from pathlib import Path
import queue
import re
import runpy
import select
import shlex
import socket
import struct
import subprocess
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def recv(sock, size):
    out = b""
    while len(out) < size:
        piece = sock.recv(size - len(out))
        if not piece:
            raise EOFError("device disconnected")
        out += piece
    return out


def main():
    run(str(ROOT / "scrcpy-harmony"), "--build-only")
    target = subprocess.check_output(["hdc", "list", "targets"], text=True).strip().splitlines()
    if len(target) != 1 or target == ["[Empty]"]:
        raise RuntimeError("connect exactly one test device")
    hdc = ["hdc", "-t", target[0]]

    def device(*args):
        return subprocess.check_output([*hdc, *args], text=True)

    sdk = runpy.run_path(str(ROOT / "scrcpy-harmony"))["native_sdk"](None)
    fixture = ROOT / "build-harmony/device-fixture"
    run(str(sdk / "llvm/bin/aarch64-unknown-linux-ohos-clang++"), "-std=c++17", "-O2", "-static-libstdc++",
        str(ROOT / "harmony/device_fixture.cpp"), "-o", str(fixture), "-lpasteboard", "-ludmf", "-lohaudio",
        "-lnative_display_manager")
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    remote = "/data/local/tmp/scrcpy-protocol-" + uuid.uuid4().hex
    server = guard = None
    video = control = None
    sessions = queue.Queue()
    messages = queue.Queue()
    errors = []
    stopping = threading.Event()
    frames = []
    auth = uuid.uuid4().hex.encode()
    try:
        device("shell", f"mkdir -m 700 {remote}")
        device("file", "send", str(ROOT / "build-harmony/scrcpy-harmony-server"), remote + "/server")
        device("file", "send", str(fixture), remote + "/fixture")
        device("shell", f"chmod 700 {remote}/server {remote}/fixture")
        guard = subprocess.Popen([*hdc, "shell", f"{remote}/fixture clipboard-guard {remote}"], stdout=subprocess.DEVNULL)
        for _ in range(50):
            if "ready" in device("shell", f"cat {remote}/guard.ready 2>/dev/null"):
                break
            time.sleep(.1)
        else:
            raise RuntimeError("clipboard guard could not preserve the original clipboard")
        device("fport", f"tcp:{port}", f"tcp:{port}")
        log = (ROOT / "build-harmony/protocol-server.log").open("w")
        server = subprocess.Popen([*hdc, "shell", f"exec {remote}/server {port} 0 0 8000000 1 auth={auth.decode()}"],
                                  stdout=log, stderr=subprocess.STDOUT)
        for _ in range(50):
            if "ready on device loopback" in (ROOT / "build-harmony/protocol-server.log").read_text():
                break
            time.sleep(.1)
        # A wrong nonce must not consume the video connection slot.
        with socket.create_connection(("127.0.0.1", port), timeout=5) as rejected:
            rejected.sendall(b"x" * 32)
            assert rejected.recv(1) == b"", "unauthenticated connection accepted"
        video = socket.create_connection(("127.0.0.1", port), timeout=15)
        video.sendall(auth)
        hello = recv(video, 69)
        assert hello[0] == 0 and hello[65:] == b"h264", hello
        control = socket.create_connection(("127.0.0.1", port), timeout=15)
        control.sendall(auth)
        video.settimeout(None)
        control.settimeout(None)

        def video_reader():
            try:
                with (ROOT / "build-harmony/protocol-video.h264").open("wb") as stream:
                    while not stopping.is_set():
                        pts, length = struct.unpack(">QI", recv(video, 12))
                        if pts >> 63:
                            sessions.put((pts & 0xffffffff, length))
                        else:
                            stream.write(recv(video, length))
                            if not pts & (1 << 62):
                                frames.append(pts & ((1 << 61) - 1))
            except (OSError, EOFError) as exc:
                if not stopping.is_set():
                    errors.append(repr(exc))

        def control_reader():
            try:
                while not stopping.is_set():
                    kind = recv(control, 1)[0]
                    if kind == 0:
                        size = struct.unpack(">I", recv(control, 4))[0]
                        messages.put(("clipboard", recv(control, size).decode()))
                    elif kind == 1:
                        messages.put(("ack", struct.unpack(">Q", recv(control, 8))[0]))
                    elif kind == 2:
                        dev, size = struct.unpack(">HH", recv(control, 4))
                        messages.put(("hid", (dev, recv(control, size).hex())))
                    else:
                        raise RuntimeError(f"invalid device message {kind}")
            except (OSError, EOFError) as exc:
                if not stopping.is_set():
                    errors.append(repr(exc))

        threads = [threading.Thread(target=video_reader, daemon=True), threading.Thread(target=control_reader, daemon=True)]
        for thread in threads:
            thread.start()

        def expect(kind, value, timeout=10):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                name, content = messages.get(timeout=max(.1, deadline - time.monotonic()))
                if name == kind and content == value:
                    return
            raise AssertionError((kind, value))

        width, height = sessions.get(timeout=15)
        expected = tuple(int(value) & ~1 for value in device("shell", f"{remote}/fixture display-size").split())
        assert (width, height) == expected, ((width, height), expected)
        control.sendall(bytes([17]))
        assert sessions.get(timeout=15) == (width, height)
        marker = "scrcpy clipboard 往返 " + uuid.uuid4().hex
        encoded = marker.encode()
        control.sendall(struct.pack(">BQBI", 9, 7, 0, len(encoded)) + encoded)
        expect("ack", 7)
        control.sendall(bytes([8, 0]))
        expect("clipboard", marker)
        marker2 = marker + " device change"
        local = ROOT / "build-harmony/clipboard-marker.txt"
        local.write_text(marker2)
        device("file", "send", str(local), remote + "/guard.set")
        expect("clipboard", marker2)
        # Standard USB boot keyboard descriptor, then a release report. This
        # verifies real UHID creation without typing into the user's apps.
        desc = bytes.fromhex("05010906a101050719e029e71500250175019508810295017508810195057501050819012905910295017503910195067508150025650507190029658100c0")
        name = b"scrcpy protocol keyboard"
        control.sendall(struct.pack(">BHHHB", 12, 42, 0x18d1, 1, len(name)) + name + struct.pack(">H", len(desc)) + desc)
        time.sleep(.5)
        devices = device("shell", "cat /proc/bus/input/devices")
        assert name.decode() in devices, "UHID keyboard did not appear"
        control.sendall(struct.pack(">BHH", 13, 42, 8) + bytes(8))
        control.sendall(struct.pack(">BH", 14, 42))
        for _ in range(30):
            if name.decode() not in device("shell", "cat /proc/bus/input/devices"):
                break
            time.sleep(.1)
        else:
            raise AssertionError("UHID keyboard did not disappear")
        assert len(frames) > 1 and all(b > a for a, b in zip(frames, frames[1:])), frames[:5]
        assert not errors, errors
        report = {"resolution": [width, height], "reset_video": "pass", "clipboard_host_to_device": "pass",
                  "clipboard_device_to_host": "pass", "uhid_keyboard": "pass", "frames": len(frames), "pts_monotonic": True}
        (ROOT / "build-harmony/protocol-results.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, ensure_ascii=False, indent=2))
    finally:
        stopping.set()
        for sock in (control, video):
            if sock:
                sock.shutdown(socket.SHUT_RDWR)
                sock.close()
        if server:
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.terminate()
        device("shell", f"touch {remote}/guard.done")
        if guard:
            guard.wait(timeout=5)
        device("fport", "rm", f"tcp:{port}", f"tcp:{port}")
        device("shell", f"rm -f {remote}/server {remote}/fixture {remote}/guard.ready {remote}/guard.done {remote}/guard.set; rmdir {remote}")


if __name__ == "__main__":
    main()
