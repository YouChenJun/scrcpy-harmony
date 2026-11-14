#!/usr/bin/env python3
"""Record a live Harmony screen and verify decoding, duration and timestamps."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=8)
    parser.add_argument("--file", type=Path, help="verify an existing recording instead of recording again")
    args = parser.parse_args()
    if args.seconds < 3:
        parser.error("--seconds must be at least 3")
    recording = args.file or ROOT / "build-harmony/verification.mkv"
    if not args.file:
        subprocess.run([str(ROOT / "scrcpy-harmony"), "--no-control", "--no-window",
                        f"--record={recording}", f"--time-limit={args.seconds}"], check=True)
    result = subprocess.check_output([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames",
        "-show_entries", "stream=codec_name,width,height,nb_read_frames:packet=pts_time",
        "-of", "json", str(recording)], text=True)
    data = json.loads(result)
    stream = data["streams"][0]
    pts = [float(packet["pts_time"]) for packet in data["packets"]]
    if stream["codec_name"] != "h264" or int(stream["nb_read_frames"]) < 2:
        raise RuntimeError("H.264 frames did not decode")
    if not all(b > a for a, b in zip(pts, pts[1:])):
        raise RuntimeError("video timestamps did not increase strictly")
    duration = pts[-1] - pts[0]
    if not args.seconds - 2 < duration < args.seconds + 2:
        raise RuntimeError(f"expected approximately {args.seconds}s of video, got {duration:.3f}s")
    print(f"PASS: {stream['width']}x{stream['height']}, {stream['nb_read_frames']} decoded frames, "
          f"{duration:.3f}s, strictly increasing timestamps")


if __name__ == "__main__":
    main()
