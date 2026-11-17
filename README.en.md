# scrcpy-harmony

[简体中文](README.md) | **English**

Experimental HarmonyOS / OpenHarmony screen mirroring and control, derived from
[scrcpy](https://github.com/Genymobile/scrcpy) **v5.0.1**. This project adds an
hdc deployment path and a native device server while reusing scrcpy's desktop
client, FFmpeg decoding, SDL display, recording, and control protocol.

**Current supported scope: rooted devices only.** Non-root retail devices have
not been validated and are not supported by this release. macOS is the validated
host platform; the Harmony launcher has not been validated on Windows or Linux.
This is an independent derivative, not an official Genymobile or Huawei project.

## Features and status

| Capability | Status |
| --- | --- |
| Display mirroring | H.264; optional H.265 when the device provides an encoder |
| Capture settings | Resolution limit, frame-rate target, video bit rate |
| Desktop features | Native scrcpy window, fullscreen, scaling, screenshots, recording |
| Input | Mouse clicks, dragging, scrolling, keyboard input, multi-touch, text paste |
| Clipboard | Bidirectional text synchronization |
| Files | File transfer over hdc; `.hap` installation |
| Device features | Screen-power control and rotation requests, subject to system policies |
| Additional backends | Companion-app media capture, camera capture, virtual displays, UHID; device dependent |
| Audio | Experimental; audible capture is not validated. Use `--no-audio` |
| Non-root control | Not supported by the current release; DevEco/HOScrcpy integration is not implemented |

Requests such as `--max-fps=60` are encoder targets, not measured performance
guarantees. Root access alone does not guarantee compatibility: some optional
features rely on system interfaces that vary by firmware.

## Requirements

- A rooted **arm64 HarmonyOS / OpenHarmony** device with USB debugging enabled.
- An authorized hdc connection. The native control backend requires access to
  `/dev/uinput`; UHID modes additionally require `/dev/uhid`.
- Python **3.9+**, Meson, Ninja, pkg-config, SDL3, FFmpeg, and libusb on the host.
- An arm64 OpenHarmony Native SDK; API **21** headers are recommended for this source.
- `hdc` on `PATH`, or its executable path set through `HDC`.
- For the companion backend: HarmonyOS command-line tools with `hvigorw`, `ohpm`,
  Java, and a development signing setup accepted by the device.

On macOS, install the desktop build dependencies:

```sh
brew install python meson ninja pkg-config sdl3 ffmpeg libusb
```

Install the SDK and hdc separately. Specify the Native SDK directory if it is
not detected automatically:

```sh
export OHOS_NDK_HOME="/path/to/openharmony/native"
export PATH="/path/to/openharmony/toolchains:$PATH"
# Alternatively: export HDC="/path/to/hdc"
```

## Homebrew (macOS arm64)

The packaged root backend does not require a local HarmonyOS SDK or source build:

```sh
brew tap YouChenJun/tap
brew install scrcpy-harmony
scrcpy-harmony --backend=root --no-audio -w
```

`hdc` and device debugging authorization are still required. The companion-app
backend needs HarmonyOS command-line tools, signing support, and the source tree;
the Homebrew package is intended for the root backend first.

## Quick start: root backend, no companion app

```sh
git clone https://github.com/YouChenJun/scrcpy-harmony.git
cd scrcpy-harmony
hdc list targets
./scrcpy-harmony --backend=root --no-audio -w
```

The launcher builds the client and native server, deploys a temporary device
process, creates hdc forwards, and opens the mirror window. **The root backend
does not install the companion app.** A system or firmware may impose additional
requirements; an unlocked device and an authorized debug connection are still needed.

Close the window or press `Ctrl+C` in the terminal to disconnect. Session-owned
forwards, uploaded server files, and virtual input devices are cleaned up on exit.
`-w` applies a temporary screen-timeout override and restores it when the session ends.

If multiple devices are connected, select one with `-s <HDC_TARGET>`; the launcher
will not choose one arbitrarily.

## Usage examples

### Adjust video quality

```sh
./scrcpy-harmony --backend=root --no-audio -w -m 1600 --max-fps=60 -b 6M
./scrcpy-harmony --backend=root --no-audio --video-codec=h265
./scrcpy-harmony --backend=root --no-audio --fullscreen
```

The defaults are no resolution reduction (`-m 0`), 8 Mbit/s video, and a requested
frame rate derived from the display refresh rate, capped at 120 fps.

### Record video or view without control

```sh
mkdir -p recordings
./scrcpy-harmony --backend=root --no-audio --record=recordings/screen.mkv
./scrcpy-harmony --backend=root --no-audio --no-control
./scrcpy-harmony --backend=root --no-audio --no-control --no-window \
  --record=recordings/screen.mkv --time-limit=10
```

Recording is opt-in. No recording file is created unless `--record` is supplied.

### Mirror system panels and notifications

Harmony's screen capture privacy protection masks system windows such as the
notification panel, control center, and input method by default. The physical
screen may therefore show a panel while the mirrored image remains on the app or
home screen.

On a rooted device, an explicit opt-in direct-display mode can include those
windows:

```sh
scrcpy-harmony --include-system-overlays --no-audio -w
```

From a source checkout, specify the root backend:

```sh
./scrcpy-harmony --backend=root --include-system-overlays --no-audio -w
```

> **Privacy warning:** This mode disables the default masking of system windows.
> Notification contents, verification codes, message previews, and input-method
> content may appear in the mirror and in recordings. Use it only on a device you
> control and only when you accept that exposure.

This option currently supports only the root backend, the primary display, and
`--no-audio`. It cannot be combined with `--new-display` or a non-zero
`--display-id`. The default capture path keeps system privacy protection enabled.

### Use the companion media backend

```sh
./scrcpy-harmony --backend=agent --no-audio -w
```

`agent` is currently the launcher's default backend. It builds and installs
**Scrcpy Harmony** for media capture and runs a separate root process for control.
Follow the device's screen-sharing, camera, or microphone prompts as applicable.
Installing an app does not remove the current root requirement for control.

The build helper can use development signing material bundled with a compatible
SDK. Whether that signature can be installed depends on the device's policies;
it is not a universal retail-device signing solution. Device-bound signing
profiles and build artifacts stay in ignored build output.

### Camera or virtual display

```sh
./scrcpy-harmony --backend=agent --video-source=camera --camera-facing=front \
  --camera-size=1280x720 --no-audio --no-control

# Replace the example bundle with an installed application.
./scrcpy-harmony --backend=agent --no-audio --new-display=1280x720 \
  --start-app=com.example.app
```

Camera permissions, available camera profiles, virtual-display support, and
input routing depend on the device. These examples remain within the root-device
support scope of this project.

### Choose the SDK or build only

```sh
./scrcpy-harmony --sdk /path/to/openharmony/native --backend=root --no-audio
./scrcpy-harmony --build-only
./scrcpy-harmony --help
```

Other compatible desktop options are forwarded to scrcpy. Android-specific or
unimplemented capture options may be rejected. On macOS the build also creates
`build-harmony/Scrcpy Harmony.app`; this local wrapper references the checkout
and its build dependencies and is not a standalone binary distribution.

## Controls

- Left click / drag: touch input; mouse wheel: scrolling.
- Right click: Back; middle click: Home.
- `MOD+h`: Home; `MOD+b`: Back; `MOD+s`: recent apps.
- `MOD+n`: notification panel; `MOD+Shift+n`: quick settings.
- `MOD+v`: paste text. `MOD` defaults to left Command or left Alt.

See the inherited [shortcut reference](doc/shortcuts.md). Some device-side
shortcuts differ by platform. Files dropped into the mirror window are uploaded
through hdc; `.hap` files are installed. Android APK files are transferred as
files and are not installed as native HarmonyOS applications.

The SDK keyboard uses committed text from the desktop input method by default.
Digits, Latin letters, and printable ASCII symbols are injected as key strokes,
so ordinary typing does not require clipboard access or paste support. Unicode
text on the main display uses UITest, which may update the device clipboard.
Explicit paste shortcuts still follow the target application's paste policy.
`--raw-key-events` remains available for applications requiring physical key
events, but it does not forward committed desktop IME text.

## Architecture

```text
Device screen / camera -> native capture -> hardware video encoder
    -> hdc forwarding -> scrcpy protocol -> FFmpeg / SDL desktop client
Desktop input -> control socket -> native Harmony control backend
```

- `scrcpy-harmony`: build, device selection, deployment, and cleanup.
- `harmony/`: native media/control server, platform adapters, companion application.
- `app/src/`: modified upstream client connection and file-transfer paths.
- [Harmony implementation notes](doc/harmony.md): backend details and limitations.
- [Upstream development documentation](doc/develop.md): inherited scrcpy architecture.

## Build and verification

```sh
./scrcpy-harmony --build-only
meson setup build-harmony/tests -Dcompile_server=false -Dbuildtype=debug
meson test -C build-harmony/tests --print-errorlogs
python3 -m unittest harmony.test_launch_agent -v
```

The native SDK is required by `--build-only`. Desktop tests can run without the
device SDK. Optional live-device checks are documented in [doc/harmony.md](doc/harmony.md).
They may change the device UI or temporarily use the clipboard; run them only on
a device you intend to test. Passing desktop tests does not establish firmware compatibility.

## Limitations and contributions

Audio, firmware compatibility, and parity with Android scrcpy remain ongoing work.
Non-root retail control is a research direction, not a released backend. AV1,
rotation, screen-off capture, UHID, and virtual-display behavior depend on system
capabilities; fixed-orientation apps may override rotation requests.

Open issues and pull requests in **this repository** for Harmony adaptation
problems. Include the backend, commit, host environment, SDK version, and a
minimal reproduction. Redact serial numbers, UDIDs, IP addresses, account data,
local paths, signing profiles, and credentials before posting logs or media.
Please do not report adaptation-specific problems as upstream scrcpy bugs.

## Upstream, attribution, and license

This repository retains the upstream scrcpy source and history. The adaptation
starts from scrcpy **v5.0.1**, commit
[`a60891ae`](https://github.com/Genymobile/scrcpy/commit/a60891aea193d92e7e5c3942700eca63f9d19a5f).
Upstream copyright notices and the **Apache License 2.0** are preserved.
Harmony adaptation changes are also released under Apache-2.0; see [LICENSE](LICENSE)
and [NOTICE](NOTICE).

Thanks to Genymobile, Romain Vimont, and all scrcpy contributors. Third-party SDKs,
DevEco components, downloaded research projects, signing keys, and local device
test artifacts are not bundled in this repository and retain their respective terms.
