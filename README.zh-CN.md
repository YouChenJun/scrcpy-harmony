# scrcpy-harmony

[English](README.md) | [简体中文](README.zh-CN.md)

基于 [scrcpy](https://github.com/Genymobile/scrcpy) **v5.0.1** 修改的实验性
HarmonyOS / OpenHarmony 投屏与控制工具。新增 hdc 部署链路和原生设备服务端，
复用 scrcpy 的电脑客户端、FFmpeg 解码、SDL 显示、录制和控制协议。

**当前仅支持 root 设备。** 非 root 零售设备尚未完成验收，不属于本版本支持范围。
电脑端目前验证了 macOS，鸿蒙启动器尚未验证 Windows / Linux。
本项目为独立衍生项目，并非 Genymobile 或华为官方项目。

## 功能与状态

| 能力 | 当前状态 |
| --- | --- |
| 屏幕投屏 | H.264；设备提供对应编码器时可选择 H.265 |
| 画面设置 | 分辨率上限、目标帧率、视频码率 |
| 电脑端功能 | scrcpy 原生窗口、全屏、缩放、截图、录制 |
| 输入控制 | 鼠标点击、拖动、滚轮、键盘、多点触控、文字粘贴 |
| 剪贴板 | 双向文本同步 |
| 文件 | hdc 上传文件、安装 `.hap` |
| 设备操作 | 屏幕电源和旋转请求，受系统策略约束 |
| 扩展能力 | 伴随应用采集、摄像头、虚拟屏幕、UHID，依赖设备支持 |
| 音频 | 实验性，有声采集尚未验收；建议使用 `--no-audio` |
| 非 root 控制 | 本版本不支持；DevEco/HOScrcpy 后端尚未实现 |

`--max-fps=60` 等参数是编码目标，不是实测性能保证。root 权限也不等于任意固件
都能兼容，部分扩展功能使用的系统接口可能随版本变化。

## 环境要求

- 已 root 的 **arm64 HarmonyOS / OpenHarmony** 设备，开启 USB 调试。
- 已授权的 hdc 连接。原生控制需要访问 `/dev/uinput`，UHID 另需 `/dev/uhid`。
- 电脑安装 Python **3.9+**、Meson、Ninja、pkg-config、SDL3、FFmpeg、libusb。
- arm64 OpenHarmony Native SDK；建议使用 API **21** 头文件构建当前源码。
- 将 `hdc` 加入 `PATH`，或通过 `HDC` 环境变量指定可执行文件。
- 使用伴随应用后端时，还需带有 `hvigorw`、`ohpm` 的 HarmonyOS 命令行工具、Java，
  以及设备接受的开发签名环境。

macOS 电脑端构建依赖：

```sh
brew install python meson ninja pkg-config sdl3 ffmpeg libusb
```

SDK 和 hdc 需另行安装。自动检测不到 SDK 时可指定路径：

```sh
export OHOS_NDK_HOME="/path/to/openharmony/native"
export PATH="/path/to/openharmony/toolchains:$PATH"
# 也可设置：export HDC="/path/to/hdc"
```

## 快速开始：root 后端，免安装伴随应用

```sh
git clone https://github.com/YouChenJun/scrcpy-harmony.git
cd scrcpy-harmony
hdc list targets
./scrcpy-harmony --backend=root --no-audio -w
```

启动器会构建客户端和原生服务端、临时部署设备进程、建立 hdc 转发并打开窗口。
**root 后端不会安装伴随应用。** 仍需设备解锁、授权调试连接，并满足固件自身的要求。

关闭窗口或在终端按 `Ctrl+C` 即可退出。退出时清理本会话的转发、临时服务端和
虚拟输入设备。`-w` 会临时覆盖自动休眠超时，并在会话结束时恢复。

连接多台设备时必须通过 `-s <HDC_TARGET>` 选择，启动器不会随意挑选设备。

## 使用方式

### 调整画质、编码与窗口

```sh
./scrcpy-harmony --backend=root --no-audio -w -m 1600 --max-fps=60 -b 6M
./scrcpy-harmony --backend=root --no-audio --video-codec=h265
./scrcpy-harmony --backend=root --no-audio --fullscreen
```

默认不缩小分辨率（`-m 0`），视频码率为 8 Mbit/s；默认根据显示刷新率请求帧率，
上限为 120 fps。实际表现取决于设备、画面变化及编码器。

### 录制或只观看

```sh
mkdir -p recordings
./scrcpy-harmony --backend=root --no-audio --record=recordings/screen.mkv
./scrcpy-harmony --backend=root --no-audio --no-control
./scrcpy-harmony --backend=root --no-audio --no-control --no-window \
  --record=recordings/screen.mkv --time-limit=10
```

录制需要显式传入 `--record`；默认不会保存录制文件。

### 使用伴随应用采集

```sh
./scrcpy-harmony --backend=agent --no-audio -w
```

`agent` 是当前启动器的默认后端：构建并安装 **Scrcpy Harmony** 负责媒体采集，
同时启动独立的 root 进程负责控制。设备出现录屏、相机或麦克风提示时，按需授权。
安装伴随应用并不代表本版本已经支持非 root 控制。

构建脚本可使用兼容 SDK 附带的开发签名材料，设备能否接受由其安装策略决定，
这不是通用于零售机的签名方案。绑定设备的签名 profile 和构建产物保存在忽略的
构建目录内。

### 摄像头与虚拟屏幕

```sh
./scrcpy-harmony --backend=agent --video-source=camera --camera-facing=front \
  --camera-size=1280x720 --no-audio --no-control

# 将示例包名替换为设备已安装的应用。
./scrcpy-harmony --backend=agent --no-audio --new-display=1280x720 \
  --start-app=com.example.app
```

摄像头权限、可选规格、虚拟显示及输入路由均依赖设备支持；上述方式仍属于
本项目当前的 root 设备支持范围。

### 指定 SDK 或仅构建

```sh
./scrcpy-harmony --sdk /path/to/openharmony/native --backend=root --no-audio
./scrcpy-harmony --build-only
./scrcpy-harmony --help
```

其他兼容的电脑端参数会透传给 scrcpy，Android 专属或尚未实现的采集参数可能报错。
macOS 构建还会生成 `build-harmony/Scrcpy Harmony.app`；该本地启动包装引用仓库
路径和构建依赖，并非可独立分发的二进制安装包。

## 基本操作

- 左键和拖动：触摸操作；滚轮：滚动。
- 右键：返回；中键：桌面。
- `MOD+h`：桌面；`MOD+b`：返回；`MOD+s`：最近任务。
- `MOD+n`：通知栏；`MOD+Shift+n`：快捷设置。
- `MOD+v`：粘贴文字。`MOD` 默认为左 Command 或左 Alt。

完整按键可参考继承的[快捷键文档](doc/shortcuts.md)，部分设备端操作存在平台差异。
将文件拖入窗口后会通过 hdc 上传；`.hap` 会安装，Android APK 只会作为普通文件
传输，不能据此安装原生鸿蒙应用。

## 架构

```text
设备屏幕 / 相机 → 原生采集 → 硬件视频编码
    → hdc 转发 → scrcpy 协议 → FFmpeg / SDL 电脑客户端
电脑输入 → 控制 socket → 鸿蒙原生控制后端
```

- `scrcpy-harmony`：构建、设备选择、部署和退出清理。
- `harmony/`：原生音视频与控制服务、平台适配、伴随应用源码。
- `app/src/`：基于上游修改的客户端连接与文件传输路径。
- [鸿蒙实现说明](doc/harmony.md)：后端细节和限制。
- [上游开发文档](doc/develop.md)：继承的 scrcpy 架构。

## 构建与验证

```sh
./scrcpy-harmony --build-only
meson setup build-harmony/tests -Dcompile_server=false -Dbuildtype=debug
meson test -C build-harmony/tests --print-errorlogs
python3 -m unittest harmony.test_launch_agent -v
```

`--build-only` 需要 Native SDK；电脑端测试本身不需要设备 SDK。
可选真机测试见 [doc/harmony.md](doc/harmony.md)，可能操作设备界面或临时使用剪贴板，
应在明确用于测试的设备上运行。电脑端测试通过并不代表固件兼容性已全部验证。

## 限制与参与

音频、固件兼容性及与 Android scrcpy 的功能一致性仍在完善。非 root 零售机控制
属于调研方向，尚未提供对应后端。AV1、旋转、息屏采集、UHID、虚拟显示等能力
受系统支持限制，固定方向的应用也可能覆盖旋转请求。

鸿蒙适配问题请提交到**本仓库**的 Issues / Pull Requests，提供后端、提交版本、
电脑环境、SDK 版本及最小复现。发布日志或媒体前，请移除设备序列号、UDID、IP、
账号信息、本机路径、签名 profile 和凭据，不要把本分支专属问题报给上游 scrcpy。

## 上游来源、版权与许可证

本仓库保留上游 scrcpy 源码和历史，适配基线为 **v5.0.1**，提交
[`a60891ae`](https://github.com/Genymobile/scrcpy/commit/a60891aea193d92e7e5c3942700eca63f9d19a5f)。
上游版权声明及 **Apache License 2.0** 保持保留；鸿蒙适配修改也以 Apache-2.0 开源，
详见 [LICENSE](LICENSE) 和 [NOTICE](NOTICE)。

感谢 Genymobile、Romain Vimont 和所有 scrcpy 贡献者。仓库不附带第三方 SDK、
DevEco 组件、下载的调研项目、签名私钥或本地设备测试产物；这些组件遵循各自条款。
