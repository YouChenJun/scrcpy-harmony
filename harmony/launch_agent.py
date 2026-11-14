"""Run app media alone for viewing, or with a root daemon for input control."""
import os
import json
from pathlib import Path
import re
import shlex
import socket
import subprocess
import threading
import time
import uuid
from harmony.build_agent import BUNDLE, build_agent

ROOT = Path(__file__).resolve().parent.parent


def run_agent(args, extra, forwarded, sdk, target, hdc, server, client):
    base = [hdc, "-t", target]

    def device(*command, timeout=15):
        result = subprocess.run([*base, *command], check=True, capture_output=True, text=True, timeout=timeout)
        if "[Fail]" in result.stdout:
            raise RuntimeError(result.stdout.strip())
        return result.stdout

    needs_root = not args.no_control or bool(args.new_display)
    if needs_root and device("shell", "id -u").strip() != "0":
        raise RuntimeError("Device control and new displays require root. For app-only viewing use --no-control --no-audio (without -w).")
    build_agent(sdk, target, hdc)

    def port():
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            return reservation.getsockname()[1]

    media_port, control_port = port(), port()
    while media_port == control_port:
        control_port = port()
    auth = uuid.uuid4().hex
    remote_dir = "/data/local/tmp/scrcpy-harmony-" + uuid.uuid4().hex
    remote = remote_dir + "/server"
    process = desktop = pump = None
    root_pid = []
    ready = threading.Event()
    virtual_id = []
    forwards = []
    remote_session = None
    remote_created = False
    sandbox_files = "./data/storage/el2/base/haps/entry/files"
    files_dir = sandbox_files

    def app_shell(command):
        return device("shell", "-b", BUNDLE, command)

    def media_log():
        command = "tail -n 30 " + files_dir + "/scrcpy-media.log 2>/dev/null"
        return device("shell", command) if needs_root else app_shell(command)
    try:
        if needs_root:
            device("shell", f"mkdir -m 700 {remote_dir}")
            remote_created = True
            device("file", "send", str(server), remote)
            device("shell", f"chmod 700 {remote}; mkdir -p /storage/media/100/local/files/Docs/scrcpy")
            command = [remote, "--control-only", str(control_port), str(media_port), "0" if args.no_control else "1", "auth=" + auth]
            if args.no_clipboard_autosync:
                command.append("clipboard=false")
            if args.stay_awake:
                command.append("stay_awake=true")
            if args.new_display:
                command.append("new_display=" + args.new_display)
            elif args.display_id is not None:
                command.append("display_id=" + str(args.display_id))
            process = subprocess.Popen([*base, "shell", "exec " + shlex.join(command)], stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, bufsize=1)

            def read_logs():
                for line in process.stdout:
                    print(line.rstrip(), flush=True)
                    match = re.search(r"\[harmony\] pid: ([0-9]+)", line)
                    if match:
                        root_pid.append(int(match[1]))
                    match = re.search(r"\[harmony\] virtual_display=([0-9]+)", line)
                    if match:
                        virtual_id.append(int(match[1]))
                    if "control ready on device loopback:" in line:
                        ready.set()

            pump = threading.Thread(target=read_logs, daemon=True); pump.start()
            deadline = time.monotonic() + 15
            while not ready.wait(.1):
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("Root input daemon failed to start")
        else:
            print("[harmony] 仅投屏模式：不启动 root 控制服务。", flush=True)
        media_args = [str(media_port), str(args.max_size), str(args.max_fps), str(args.video_bit_rate),
                      "0" if args.no_control else "1", "media_only=true", "auth=" + auth,
                      "audio=false" if args.no_audio else "audio=true", "audio_codec=" + args.audio_codec,
                      "audio_bitrate=" + str(args.audio_bit_rate)]
        media_args.append("audio_source=" + args.audio_source)
        media_args.append("video_codec=" + getattr(args, "video_codec", "h264"))
        if args.video_source == "camera":
            media_args.append("video_source=camera")
            for option in ("camera_id", "camera_facing", "camera_size", "camera_fps"):
                value = getattr(args, option)
                if value is not None: media_args.append(option + "=" + str(value))
            if args.camera_torch: media_args.append("camera_torch=true")
        if virtual_id:
            media_args.append("display_id=" + str(virtual_id[0]))
        elif args.display_id is not None:
            media_args.append("display_id=" + str(args.display_id))
        # Only an authorized hdc debugger (or root) can write this one-use
        # private session file. Other apps still cannot supply capture args.
        session_id = uuid.uuid4().hex
        media_args.append("session_id=" + session_id)
        local_session = ROOT / "build-harmony" / (session_id + ".json")
        local_session.write_text(json.dumps({"args": media_args})); local_session.chmod(0o600)
        try:
            if needs_root:
                metadata = device("shell", "bm dump -n " + BUNDLE)
                match = re.search(r'"uid"\s*:\s*([0-9]+)', metadata)
                if not match:
                    raise RuntimeError("Could not determine the companion application identity")
                uid = int(match[1]); user = uid // 200000
                files_dir = f"/data/app/el2/{user}/base/{BUNDLE}/haps/entry/files"
                remote_session = files_dir + "/scrcpy-sessions/" + session_id + ".json"
                device("shell", f"mkdir -p {files_dir}/scrcpy-sessions; chown {uid} {files_dir}/scrcpy-sessions; chmod 700 {files_dir}/scrcpy-sessions")
                device("file", "send", str(local_session), remote_session)
                device("shell", f"chown {uid} {remote_session}; chmod 400 {remote_session}")
            else:
                remote_session = sandbox_files + "/scrcpy-sessions/" + session_id + ".json"
                app_shell(f"mkdir -p {sandbox_files}/scrcpy-sessions; chmod 700 {sandbox_files}/scrcpy-sessions")
                device("file", "send", "-b", BUNDLE, str(local_session), remote_session)
                app_shell(shlex.join(["chmod", "600", remote_session]))
        finally:
            local_session.unlink(missing_ok=True)
        start = ["/system/bin/aa", "start", "-b", BUNDLE, "-a", "EntryAbility", "--ps", "scrcpySession", session_id]
        result = device("shell", shlex.join(start))
        if "start ability successfully" not in result:
            raise RuntimeError("Could not open the media companion: " + result.strip())
        deadline = time.monotonic() + 120
        marker = "[harmony] session ready: " + session_id
        while time.monotonic() < deadline:
            if marker in media_log():
                break
            time.sleep(.2)
        else:
            raise RuntimeError("The media companion did not open its socket. " + media_log()[-1500:])
        for number in ([media_port, control_port] if not args.no_control else [media_port]):
            device("fport", f"tcp:{number}", f"tcp:{number}"); forwards.append(number)
        env = os.environ.copy()
        env["HDC"] = hdc; env["SCRCPY_HARMONY_SERIAL"] = target
        env["SCRCPY_HARMONY_AUTH"] = auth
        if args.no_control:
            env.pop("SCRCPY_HARMONY_CONTROL_PORT", None)
        else:
            env["SCRCPY_HARMONY_CONTROL_PORT"] = str(control_port)
        env.setdefault("SCRCPY_ICON_DIR", str(ROOT / "app/data"))
        print("[harmony] 媒体应用已连接；如出现系统录屏提示，请在设备上允许。", flush=True)
        print(f"[harmony] media_port={media_port}", flush=True)
        desktop = subprocess.Popen([str(client), f"--harmony-port={media_port}", *forwarded, *extra], env=env)
        result = desktop.wait()
        if result:
            print(media_log(), flush=True)
        return result
    finally:
        if desktop and desktop.poll() is None:
            desktop.terminate()
            try:
                desktop.wait(timeout=5)
            except subprocess.TimeoutExpired:
                desktop.kill(); desktop.wait()
        if root_pid:
            subprocess.run([*base, "shell", "kill -TERM " + str(root_pid[0])], capture_output=True, timeout=5)
        if process:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()
            if pump:
                pump.join(timeout=2)
            process.stdout.close()
        for number in forwards:
            subprocess.run([*base, "fport", "rm", f"tcp:{number}", f"tcp:{number}"], capture_output=True, timeout=5)
        if remote_session:
            cleanup = ["shell"] if needs_root else ["shell", "-b", BUNDLE]
            subprocess.run([*base, *cleanup, shlex.join(["rm", "-f", remote_session])], capture_output=True, timeout=5)
        if remote_created:
            subprocess.run([*base, "shell", f"rm -f {remote}; rmdir {remote_dir}"], capture_output=True, timeout=5)
