# scrcpy-harmony

**简体中文** | [English](README.en.md)

`scrcpy-harmony` 是一个面向 HarmonyOS / OpenHarmony 的实验性投屏与控制工具，基于 [scrcpy](https://github.com/Genymobile/scrcpy) **v5.0.1** 修改。项目保留 scrcpy 的电脑端窗口、FFmpeg 解码、SDL 渲染、录制和控制协议，并为鸿蒙设备重新实现了 hdc 部署链路和原生设备端服务。

> 当前版本仅支持已 root 的 arm64 鸿蒙设备。未 root 的零售设备尚未完成验证，暂不在支持范围内。
>
> 电脑端目前主要验证了 macOS。Windows 和 Linux 的鸿蒙启动器尚未经过完整测试。
>
> 本项目是独立维护的第三方衍生项目，不是 Genymobile 或华为官方项目。

## 主要功能

| 功能 | 当前状态 |
| --- | --- |
| 屏幕投屏 | 支持 H.264；设备具备相应编码器时可使用 H.265 |
| 画面参数 | 可限制分辨率、设置目标帧率和视频码率 |
| 电脑端操作 | 全屏、缩放、截图、录像，沿用 scrcpy 的窗口与快捷键 |
| 输入控制 | 鼠标点击、拖动、滚轮、键盘、多点触控和文字输入 |
| 剪贴板 | 双向同步文本 |
| 文件传输 | 通过 hdc 上传文件，并可安装鸿蒙 `.hap` 应用包 |
| 设备操作 | 可请求电源、旋转等操作，实际效果受系统策略约束 |
| 扩展采集 | 手机端配套应用采集、摄像头、虚拟屏幕和 UHID，具体支持取决于设备 |
| 音频 | 功能仍属实验性，尚未验证真实声音输出；目前建议关闭音频 |
| 非 root 控制 | 尚未实现，当前发布版不支持 |

帧率和码率参数表示编码目标，不代表在任意画面或设备上都能达到。即使设备已 root，部分系统接口仍可能因固件版本不同而不可用。

## 安装

### 方式一：Homebrew（推荐，macOS Apple Silicon）

发布包已经包含电脑端程序和 arm64 设备端程序，不需要本地安装 HarmonyOS SDK，也不需要自行编译。

```sh
brew tap YouChenJun/tap
brew install scrcpy-harmony
```

安装后直接运行：

```sh
scrcpy-harmony --no-audio -w
```

Homebrew 安装版默认使用 root 后端。首次连接前请确认：

1. 电脑已经安装 `hdc`，并且可以在终端中执行 `hdc list targets`。
2. 手机已开启开发者模式和 USB 调试。
3. 手机已经信任当前电脑。
4. 设备已经 root，并允许本工具访问所需的输入接口。

`--no-audio` 用于关闭当前尚未验收的音频功能，`-w` 用于在投屏期间临时避免自动休眠。

升级和卸载：

```sh
brew update
brew upgrade scrcpy-harmony

brew uninstall scrcpy-harmony
```

当前 Homebrew 包只提供 root 后端。摄像头、手机端配套应用等扩展采集能力仍需要源码、HarmonyOS SDK 和签名环境。

### 方式二：从源码运行

适合开发者调试设备端功能。电脑端构建需要 Python 3.9+、Meson、Ninja、pkg-config、SDL3、FFmpeg 和 libusb。

macOS 可先安装电脑端依赖：

```sh
brew install python meson ninja pkg-config sdl3 ffmpeg libusb
```

还需要准备 arm64 OpenHarmony Native SDK 和 `hdc`。如果启动器不能自动找到 SDK，可以手动设置：

```sh
export OHOS_NDK_HOME="/path/to/openharmony/native"
export PATH="/path/to/openharmony/toolchains:$PATH"
# 也可以直接指定 hdc：export HDC="/path/to/hdc"
```

然后获取并运行源码：

```sh
git clone https://github.com/YouChenJun/scrcpy-harmony.git
cd scrcpy-harmony
./scrcpy-harmony --backend=root --no-audio -w
```

源码运行时会自动构建电脑端客户端，并交叉编译设备端服务。第一次构建所需时间会稍长。

## 使用方式

### 投屏并控制设备

Homebrew 安装版可以直接运行：

```sh
scrcpy-harmony --no-audio -w
```

使用源码时请加上 `--backend=root`：

```sh
./scrcpy-harmony --backend=root --no-audio -w
```

这种模式会把设备端程序临时上传到手机并启动，不会在手机桌面留下配套应用图标。退出时会停止本会话设备进程、删除临时文件并清理 hdc 端口转发。

### 调整画质和帧率

```sh
# 最长边限制为 1600，目标帧率 60 fps，视频码率 6 Mbit/s
scrcpy-harmony --no-audio -w -m 1600 --max-fps=60 -b 6M

# 使用 H.265（设备必须具备对应硬件编码器）
scrcpy-harmony --no-audio --video-codec=h265

# 全屏显示
scrcpy-harmony --no-audio --fullscreen
```

默认不缩小画面，视频码率为 8 Mbit/s。未指定帧率时，启动器会根据屏幕刷新率请求帧率，最高为 120 fps。

### 录制或只看不控制

```sh
mkdir -p recordings

# 投屏并同时录像
scrcpy-harmony --no-audio --record=recordings/screen.mkv

# 只观看，不向设备发送控制事件
scrcpy-harmony --no-audio --no-control

# 不开窗口，录制 10 秒
scrcpy-harmony --no-audio --no-control --no-window \
  --record=recordings/screen.mkv --time-limit=10
```

只有显式传入 `--record` 时才会写录像文件。

### 显示下拉通知栏和系统面板

默认采集会开启鸿蒙的“屏幕隐私保护”。系统会屏蔽通知栏、控制中心、输入法等系统窗口，因此手机实际已经下拉，电脑画面却仍可能停留在桌面。这是系统的隐私保护行为。

如果设备已 root，并且明确需要在投屏中包含这些系统窗口，可以启用直接显示镜像：

```sh
scrcpy-harmony --include-system-overlays --no-audio -w
```

使用源码运行时要固定 root 后端：

```sh
./scrcpy-harmony --backend=root --include-system-overlays --no-audio -w
```

> **隐私提示：** 该模式会绕过系统窗口的默认遮罩，通知内容、验证码、聊天预览和输入法内容都可能出现在投屏及录像中。只应在自己控制且明确知道风险的设备上使用。

该选项目前仅支持 root 后端、主屏幕和 `--no-audio`，不能与 `--new-display` 或非 0 的 `--display-id` 同时使用。默认仍保留系统隐私保护。

设备端的鼠标指针会在投屏和录像中隐藏，手机上仍可正常显示；电脑端使用本机光标。若固件不支持隐藏指针，启动日志会提示。

### 使用手机端配套应用采集

源码模式可以使用手机端配套应用 **Scrcpy Harmony** 完成媒体采集，同时由独立的 root 进程负责控制：

```sh
./scrcpy-harmony --backend=agent --no-audio -w
```

这种方式需要 HarmonyOS 命令行工具、Java、SDK 以及设备可接受的开发签名。手机出现录屏、摄像头或麦克风授权提示时，需要按实际功能进行确认。安装手机端配套应用并不会让当前版本支持非 root 控制。

### 摄像头

```sh
./scrcpy-harmony --backend=agent --video-source=camera \
  --camera-facing=front --camera-size=1280x720 \
  --no-audio --no-control
```

可用摄像头、分辨率和帧率取决于设备。当前示例仍需在支持范围允许的 root 设备上使用。

### 独立虚拟屏幕

```sh
./scrcpy-harmony --backend=agent --no-audio \
  --new-display=1280x720 --start-app=com.example.app
```

将 `com.example.app` 替换为设备上已经安装的应用包名。虚拟屏幕、应用启动和输入路由依赖设备固件支持。

### 连接多台设备

同时连接多台设备时，需要使用 `-s` 指定 hdc 目标：

```sh
scrcpy-harmony -s <HDC_TARGET> --no-audio -w
```

不指定时，启动器只会在恰好连接一台设备的情况下继续，不会自动猜测目标设备。

## 操作说明

| 操作 | 作用 |
| --- | --- |
| 鼠标左键、拖动 | 点击、滑动、长按 |
| 鼠标滚轮 | 上下滚动 |
| 鼠标右键 | 返回 |
| 鼠标中键 | 回到桌面 |
| `MOD+h` | 回到桌面 |
| `MOD+b` | 返回 |
| `MOD+s` | 最近任务 |
| `MOD+n` | 打开通知栏 |
| `MOD+Shift+n` | 打开快捷设置 |
| `MOD+v` | 粘贴文字 |

`MOD` 默认是左 Command 或左 Alt。更多按键请查看继承自 scrcpy 的[快捷键文档](doc/shortcuts.md)。

默认键盘模式会读取电脑输入法已经提交的文字。数字、英文和常见符号会转换为真实按键，因此普通输入和验证码输入不依赖剪贴板粘贴。中文等 Unicode 文字在主屏幕上通过 UITest 输入，可能更新设备剪贴板。主动使用粘贴快捷键时，仍需遵守目标应用自己的粘贴限制。

将文件拖入投屏窗口后，文件会通过 hdc 上传；`.hap` 文件会执行安装。Android APK 只会作为普通文件传输，不能作为原生鸿蒙应用安装。

## 架构

```text
设备屏幕 / 摄像头
    → 原生采集
    → 硬件视频编码
    → scrcpy 视频协议
    → hdc 端口转发
    → 电脑端 FFmpeg 解码 / SDL 显示

电脑键盘和鼠标
    → scrcpy 控制协议
    → 鸿蒙控制服务
    → uinput / UHID / 系统输入接口
```

主要目录：

- `scrcpy-harmony`：安装检查、设备选择、部署、启动和退出清理。
- `harmony/`：鸿蒙设备端媒体与控制服务、系统适配和手机端配套应用源码。
- `app/src/`：基于上游 scrcpy 修改的客户端连接与文件传输代码。
- [doc/harmony.md](doc/harmony.md)：鸿蒙后端实现细节和限制。
- [doc/develop.md](doc/develop.md)：继承自 scrcpy 的开发文档。

## 从源码构建和测试

```sh
./scrcpy-harmony --build-only
meson setup build-harmony/tests -Dcompile_server=false -Dbuildtype=debug
meson test -C build-harmony/tests --print-errorlogs
python3 -m unittest harmony.test_launch_agent -v
```

`--build-only` 需要 OpenHarmony Native SDK。电脑端回归测试不需要设备 SDK，但无法代替真机和固件兼容性验证。

可选的真机测试会操作设备界面、剪贴板或输入设备，只能在明确用于测试的 root 设备上运行，具体命令见 [doc/harmony.md](doc/harmony.md)。

## 已知限制

- 当前只支持 root 设备，非 root 零售机尚未验收。
- 音频采集仍属实验性功能，真实声音输出尚未验证。
- 未 root 设备的控制后端和 DevEco / HOScrcpy 接入尚未实现。
- AV1、旋转、息屏采集、UHID 和虚拟屏幕依赖设备固件与硬件能力。
- 某些应用可能强制保持指定屏幕方向，因此旋转请求不一定会持续生效。
- 与 Android 版 scrcpy 的全部功能和兼容性仍存在差距。

## 反馈问题

鸿蒙适配相关问题请提交到[本仓库 Issues](https://github.com/YouChenJun/scrcpy-harmony/issues)，并提供：

- 使用的后端（`root` 或 `agent`）
- 提交版本或 Release 版本
- 电脑系统版本
- OpenHarmony SDK 和设备系统版本
- 能稳定复现问题的最少步骤

发布日志、截图或录像前，请先移除设备序列号、UDID、IP 地址、账号信息、本机路径、签名配置和凭据。该分支专属的问题请不要提交给上游 scrcpy。

## 上游来源、版权与许可证

本仓库保留上游 scrcpy 的源码历史和版权声明。鸿蒙适配基于 scrcpy **v5.0.1**，起始提交为 [`a60891ae`](https://github.com/Genymobile/scrcpy/commit/a60891aea193d92e7e5c3942700eca63f9d19a5f)。

上游项目及本仓库的修改均采用 **Apache License 2.0**，详见 [LICENSE](LICENSE) 和 [NOTICE](NOTICE)。

感谢 Genymobile、Romain Vimont 以及所有 scrcpy 贡献者。本仓库不附带第三方 SDK、DevEco 组件、签名私钥或用户设备的签名配置。
