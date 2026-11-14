/* SPDX-License-Identifier: Apache-2.0 */
// OpenHarmony device server for the scrcpy 5 media/control wire protocol.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <regex>
#include <mutex>
#include <thread>
#include <time.h>
#include <vector>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <multimedia/player_framework/native_avscreen_capture.h>
#include <multimedia/player_framework/native_avcodec_videoencoder.h>
#include <multimedia/player_framework/native_avformat.h>
#include "uinput_backend.h"
#include "audio.h"
#include "clipboard.h"
#include "hid.h"
#include "platform.h"
#include "native_input.h"
#include "ability.h"
#include "camera.h"
#include <multimodalinput/oh_key_code.h>

enum { TOUCH_ACTION_CANCEL, TOUCH_ACTION_DOWN, TOUCH_ACTION_MOVE, TOUCH_ACTION_UP };
#include <native_window/external_window.h>
#include <window_manager/oh_display_manager.h>

extern "C" { void (*scrcpy_harmony_launch_hook)(const char *, uint32_t) = nullptr; }
extern "C" { void (*scrcpy_harmony_started_hook)() = nullptr; }

namespace {
static_assert(std::atomic<bool>::is_always_lock_free, "signal stop flag must be lock-free");
std::atomic<bool> interrupted{false};
void on_signal(int) { interrupted = 1; }
uint16_t be16(const uint8_t *p) { return uint16_t(p[0]) << 8 | p[1]; }
uint32_t be32(const uint8_t *p) { return uint32_t(be16(p)) << 16 | be16(p + 2); }
uint64_t be64(const uint8_t *p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
void put32(uint8_t *p, uint32_t v) {
    for (int i = 3; i >= 0; --i) { p[i] = v; v >>= 8; }
}
void put64(uint8_t *p, uint64_t v) { put32(p, v >> 32); put32(p + 4, v); }
bool read_all(int fd, void *buffer, size_t len) {
    auto p = static_cast<uint8_t *>(buffer);
    while (len) {
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0 && errno == EINTR && !interrupted) { continue; }
        if (n <= 0) { return false; }
        p += n; len -= n;
    }
    return true;
}
bool write_all(int fd, const void *buffer, size_t len) {
    auto p = static_cast<const uint8_t *>(buffer);
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR && !interrupted) { continue; }
        if (n <= 0) { return false; }
        p += n; len -= n;
    }
    return true;
}
int accept_client(int listener, const std::string &auth = "") {
    // Startup cannot leave a privileged server waiting forever.
    int64_t deadline = harmony::now_us() + 180000000;
    while (harmony::now_us() < deadline && !interrupted) {
        pollfd p{listener, POLLIN, 0};
        if (poll(&p, 1, 100) > 0) {
            int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0 || auth.empty()) { return fd; }
            timeval timeout{2, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            std::string supplied(auth.size(), '\0');
            bool valid = read_all(fd, supplied.data(), supplied.size()) && supplied == auth;
            timeout = {0, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            if (valid) { return fd; }
            close(fd);
        }
    }
    return -1;
}
bool dimensions(int32_t &w, int32_t &h) {
    NativeDisplayManager_DisplayInfo *info = nullptr;
    if (OH_NativeDisplayManager_CreatePrimaryDisplay(&info) || !info) { return false; }
    w = info->width; h = info->height;
    OH_NativeDisplayManager_DestroyDisplay(info);
    return w > 0 && h > 0;
}

struct Stream {
    int fd;
    std::atomic<bool> alive{true};
    std::atomic<unsigned> frames{0};
    std::atomic<bool> stopping{false};
    std::atomic<bool> reset{false};
    std::atomic<bool> capturing{false};
    std::mutex launch_mutex;
    std::deque<std::string> pending_launches;
    harmony::Audio *audio = nullptr;
    std::string video_codec = "h264";
    uint64_t display_id = UINT64_MAX;
    harmony::PlatformDisplay *virtual_owner = nullptr;
    harmony::Camera *camera_owner = nullptr;
    int64_t last_pts = 0;
    OH_AVCodec *encoder = nullptr;
    OH_AVScreenCapture *capture = nullptr;
    OHNativeWindow *surface = nullptr;
    std::mutex writer;

    explicit Stream(int socket) : fd(socket) {}
    void stop() {
        stopping = true;
        capturing = false;
        if (camera_owner) { camera_owner->stop(); }
        if (virtual_owner && surface) { virtual_owner->surface(nullptr); }
        if (capture) {
            OH_AVScreenCapture_StopScreenCapture(capture);
            OH_AVScreenCapture_Release(capture);
            capture = nullptr;
        }
        if (encoder) {
            OH_VideoEncoder_Stop(encoder);
            OH_VideoEncoder_Destroy(encoder);
            encoder = nullptr;
        }
        if (surface) { OH_NativeWindow_DestroyNativeWindow(surface); surface = nullptr; }
        stopping = false;
    }
    ~Stream() { stop(); }
    static void codec_error(OH_AVCodec *, int32_t code, void *ctx) {
        fprintf(stderr, "[harmony] encoder error: %d\n", code);
        static_cast<Stream *>(ctx)->alive = false;
    }
    static void capture_error(OH_AVScreenCapture *, int32_t code, void *ctx) {
        fprintf(stderr, "[harmony] capture error: %d\n", code);
        static_cast<Stream *>(ctx)->alive = false;
    }
    static void state_changed(OH_AVScreenCapture *, OH_AVScreenCaptureStateCode state, void *ctx) {
        fprintf(stderr, "[harmony] capture state: %d\n", state);
        if (state == OH_SCREEN_CAPTURE_STATE_STARTED) { static_cast<Stream *>(ctx)->capturing = true; }
        if (!static_cast<Stream *>(ctx)->stopping
                && ((state >= OH_SCREEN_CAPTURE_STATE_CANCELED && state <= OH_SCREEN_CAPTURE_STATE_STOPPED_BY_CALL)
                || state == OH_SCREEN_CAPTURE_STATE_STOPPED_BY_USER_SWITCHES)) {
            static_cast<Stream *>(ctx)->alive = false;
        }
    }
    static void format_changed(OH_AVCodec *, OH_AVFormat *, void *) {}
    static void capture_data(OH_AVScreenCapture *, OH_AVBuffer *buffer,
                             OH_AVScreenCaptureBufferType type, int64_t, void *context) {
        auto &self = *static_cast<Stream *>(context);
        if (!self.audio || type != OH_SCREEN_CAPTURE_BUFFERTYPE_AUDIO_INNER) { return; }
        OH_AVCodecBufferAttr attr{};
        if (OH_AVBuffer_GetBufferAttr(buffer, &attr) != 0 || attr.size <= 0) { return; }
        auto address = OH_AVBuffer_GetAddr(buffer);
        int capacity = OH_AVBuffer_GetCapacity(buffer);
        if (address && attr.offset >= 0 && attr.size <= capacity - attr.offset) {
            self.audio->push(address + attr.offset, attr.size);
        }
    }
    static void input_available(OH_AVCodec *, uint32_t, OH_AVBuffer *, void *) {}
    static void output_available(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *ctx) {
        auto &s = *static_cast<Stream *>(ctx);
        OH_AVCodecBufferAttr attr{};
        if (OH_AVBuffer_GetBufferAttr(buffer, &attr) != AV_ERR_OK) {
            s.alive = false;
        } else if (attr.size > 0 && s.alive) {
            // This device mixes ns compositor timestamps and us timestamps for
            // repeated frames. With B-frames disabled, timestamp output using
            // one monotonic clock instead; this also spans rotation restarts.
            timespec now{}; clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t now_us = int64_t(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
            s.last_pts = std::max(s.last_pts + 1, now_us);
            uint64_t flags = s.last_pts & ((uint64_t(1) << 61) - 1);
            if (attr.flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA) { flags = uint64_t(1) << 62; }
            if (attr.flags & AVCODEC_BUFFER_FLAGS_SYNC_FRAME) { flags |= uint64_t(1) << 61; }
            uint8_t header[12]; put64(header, flags); put32(header + 8, attr.size);
            auto data = OH_AVBuffer_GetAddr(buffer);
            auto capacity = OH_AVBuffer_GetCapacity(buffer);
            std::lock_guard<std::mutex> lock(s.writer);
            if (!data || attr.offset < 0 || attr.size > capacity - attr.offset
                    || !write_all(s.fd, header, sizeof(header))
                    || !write_all(s.fd, data + attr.offset, attr.size)) {
                s.alive = false;
            } else if (!(attr.flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA)) {
                if (s.frames.fetch_add(1) == 0 && scrcpy_harmony_started_hook) { scrcpy_harmony_started_hook(); }
            }
        }
        OH_VideoEncoder_FreeOutputBuffer(codec, index);
    }
    bool start(int w, int h, int bitrate, int fps) {
        const char *mime = video_codec == "h265" ? "video/hevc" : video_codec == "av1" ? "video/av01" : "video/avc";
        encoder = OH_VideoEncoder_CreateByMime(mime);
        if (!encoder) { fprintf(stderr, "[harmony] %s encoder unavailable on this device\n", video_codec.c_str()); return false; }
        OH_AVCodecCallback cb{codec_error, format_changed, input_available, output_available};
        if (OH_VideoEncoder_RegisterCallback(encoder, cb, this)) { return false; }
        OH_AVFormat *format = OH_AVFormat_Create();
        if (!format) { return false; }
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, w);
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, h);
        OH_AVFormat_SetDoubleValue(format, OH_MD_KEY_FRAME_RATE, fps);
        OH_AVFormat_SetLongValue(format, OH_MD_KEY_BITRATE, bitrate);
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_I_FRAME_INTERVAL, 1);
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY, 1);
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENCODER_REPEAT_PREVIOUS_FRAME_AFTER, 100);
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENCODER_REPEAT_PREVIOUS_MAX_COUNT, 0x7fffffff);
        int result = OH_VideoEncoder_Configure(encoder, format);
        OH_AVFormat_Destroy(format);
        if (result) { fprintf(stderr, "[harmony] encoder configure: %d\n", result); return false; }
        if (OH_VideoEncoder_GetSurface(encoder, &surface) || OH_VideoEncoder_Prepare(encoder)) { return false; }
        if (virtual_owner || camera_owner) {
            uint8_t session[12]{}; session[0] = 0x80;
            put32(session + 4, w); put32(session + 8, h);
            if (!write_all(fd, session, sizeof(session)) || OH_VideoEncoder_Start(encoder)) { return false; }
            bool ok = camera_owner ? camera_owner->start(surface) : virtual_owner->surface(surface);
            capturing = ok;
            return ok;
        }
        capture = OH_AVScreenCapture_Create();
        if (!capture) { return false; }
        if (audio && OH_AVScreenCapture_SetDataCallback(capture, capture_data, this)) { return false; }
        OH_AVScreenCapture_SetErrorCallback(capture, capture_error, this);
        OH_AVScreenCapture_SetStateCallback(capture, state_changed, this);
        OH_AVScreenCaptureConfig cfg{};
        cfg.captureMode = OH_CAPTURE_HOME_SCREEN;
        if (display_id != UINT64_MAX) {
            cfg.captureMode = OH_CAPTURE_SPECIFIED_SCREEN;
            cfg.videoInfo.videoCapInfo.displayId = display_id;
        }
        cfg.dataType = OH_ORIGINAL_STREAM;
        cfg.videoInfo.videoCapInfo.videoFrameWidth = w;
        cfg.videoInfo.videoCapInfo.videoFrameHeight = h;
        cfg.videoInfo.videoCapInfo.videoSource = OH_VIDEO_SOURCE_SURFACE_RGBA;
        if (audio) {
            cfg.audioInfo.innerCapInfo = {48000, 2, OH_ALL_PLAYBACK};
            cfg.audioInfo.micCapInfo = {48000, 2, OH_MIC};
        }
        result = OH_AVScreenCapture_Init(capture, cfg);
        if (result) { fprintf(stderr, "[harmony] capture init: %d\n", result); return false; }
        // SDL draws the desktop cursor. Do not encode the device's virtual
        // mouse cursor a second time into the mirrored image or recordings.
        int cursor_result = OH_AVScreenCapture_ShowCursor(capture, false);
        fprintf(stderr, "[harmony] hide capture cursor: %d\n", cursor_result);
        OH_AVScreenCapture_SetMicrophoneEnabled(capture, false);
        OH_AVScreenCapture_SetCanvasRotation(capture, true);
        OH_AVScreenCapture_SetMaxVideoFrameRate(capture, fps);
        // scrcpy 5 starts each encoding session with a size header.
        uint8_t session[12]{}; session[0] = 0x80;
        put32(session + 4, w); put32(session + 8, h);
        if (!write_all(fd, session, sizeof(session)) || OH_VideoEncoder_Start(encoder)) { return false; }
        result = OH_AVScreenCapture_StartScreenCaptureWithSurface(capture, surface);
        fprintf(stderr, "[harmony] %s %dx%d @ %d fps, %d bit/s; capture start: %d\n", video_codec.c_str(), w, h, fps, bitrate, result);
        return result == 0;
    }
};

int keycode(uint32_t android) {
    static const int letters[] = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G,
        KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q,
        KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
    if (android == 7) { return KEY_0; }
    if (android >= 8 && android <= 16) { return KEY_1 + android - 8; }
    if (android >= 29 && android <= 54) { return letters[android - 29]; }
    if (android >= 131 && android <= 140) { return KEY_F1 + android - 131; }
    switch (android) {
        case 19: return KEY_UP; case 20: return KEY_DOWN;
        case 21: return KEY_LEFT; case 22: return KEY_RIGHT;
        case 23: return KEY_ENTER;
        case 24: return KEY_VOLUMEUP; case 25: return KEY_VOLUMEDOWN;
        case 26: return KEY_POWER;
        case 55: return KEY_COMMA; case 56: return KEY_DOT;
        case 57: return KEY_LEFTALT; case 58: return KEY_RIGHTALT;
        case 59: return KEY_LEFTSHIFT; case 60: return KEY_RIGHTSHIFT;
        case 61: return KEY_TAB; case 62: return KEY_SPACE;
        case 66: case 160: return KEY_ENTER; case 67: return KEY_BACKSPACE;
        case 68: return KEY_GRAVE; case 69: return KEY_MINUS;
        case 70: return KEY_EQUAL; case 71: return KEY_LEFTBRACE;
        case 72: return KEY_RIGHTBRACE; case 73: return KEY_BACKSLASH;
        case 74: return KEY_SEMICOLON; case 75: return KEY_APOSTROPHE;
        case 76: return KEY_SLASH; case 81: return KEY_KPPLUS;
        case 82: return KEY_MENU; case 92: return KEY_PAGEUP;
        case 93: return KEY_PAGEDOWN; case 111: return KEY_ESC;
        case 112: return KEY_DELETE; case 113: return KEY_LEFTCTRL;
        case 114: return KEY_RIGHTCTRL; case 115: return KEY_CAPSLOCK;
        case 117: return KEY_LEFTMETA; case 118: return KEY_RIGHTMETA;
        case 122: return KEY_HOME; case 123: return KEY_END;
        case 124: return KEY_INSERT; case 141: return KEY_F11;
        case 142: return KEY_F12; case 143: return KEY_NUMLOCK;
        case 144: return KEY_KP0; case 145: return KEY_KP1;
        case 146: return KEY_KP2; case 147: return KEY_KP3;
        case 148: return KEY_KP4; case 149: return KEY_KP5;
        case 150: return KEY_KP6; case 151: return KEY_KP7;
        case 152: return KEY_KP8; case 153: return KEY_KP9;
        case 154: return KEY_KPSLASH; case 155: return KEY_KPASTERISK;
        case 156: return KEY_KPMINUS; case 157: return KEY_KPPLUS;
        case 158: return KEY_KPDOT; case 161: return KEY_KPEQUAL;
        case 116: return KEY_SCROLLLOCK; case 121: return KEY_PAUSE;
        default: return -1;
    }
}
int native_keycode(int evdev_code) {
    static const int letters[] = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
        KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V,
        KEY_W, KEY_X, KEY_Y, KEY_Z};
    for (int i = 0; i < 26; ++i) { if (evdev_code == letters[i]) { return KEYCODE_A + i; } }
    if (evdev_code == KEY_0) { return KEYCODE_0; }
    if (evdev_code >= KEY_1 && evdev_code <= KEY_9) { return KEYCODE_1 + evdev_code - KEY_1; }
    if (evdev_code >= KEY_F1 && evdev_code <= KEY_F10) { return KEYCODE_F1 + evdev_code - KEY_F1; }
    switch (evdev_code) {
        case KEY_LEFTCTRL: return KEYCODE_CTRL_LEFT; case KEY_RIGHTCTRL: return KEYCODE_CTRL_RIGHT;
        case KEY_LEFTSHIFT: return KEYCODE_SHIFT_LEFT; case KEY_RIGHTSHIFT: return KEYCODE_SHIFT_RIGHT;
        case KEY_LEFTALT: return KEYCODE_ALT_LEFT; case KEY_RIGHTALT: return KEYCODE_ALT_RIGHT;
        case KEY_LEFTMETA: return KEYCODE_META_LEFT; case KEY_RIGHTMETA: return KEYCODE_META_RIGHT;
        case KEY_BACKSPACE: return KEYCODE_DEL; case KEY_DELETE: return KEYCODE_FORWARD_DEL;
        case KEY_ENTER: return KEYCODE_ENTER; case KEY_TAB: return KEYCODE_TAB;
        case KEY_SPACE: return KEYCODE_SPACE; case KEY_ESC: return KEYCODE_ESCAPE;
        case KEY_UP: return KEYCODE_DPAD_UP; case KEY_DOWN: return KEYCODE_DPAD_DOWN;
        case KEY_LEFT: return KEYCODE_DPAD_LEFT; case KEY_RIGHT: return KEYCODE_DPAD_RIGHT;
        case KEY_HOME: return KEYCODE_MOVE_HOME; case KEY_END: return KEYCODE_MOVE_END;
        case KEY_PAGEUP: return KEYCODE_PAGE_UP; case KEY_PAGEDOWN: return KEYCODE_PAGE_DOWN;
        case KEY_COMMA: return KEYCODE_COMMA; case KEY_DOT: return KEYCODE_PERIOD;
        case KEY_MINUS: return KEYCODE_MINUS; case KEY_EQUAL: return KEYCODE_EQUALS;
        case KEY_LEFTBRACE: return KEYCODE_LEFT_BRACKET; case KEY_RIGHTBRACE: return KEYCODE_RIGHT_BRACKET;
        case KEY_BACKSLASH: return KEYCODE_BACKSLASH; case KEY_SEMICOLON: return KEYCODE_SEMICOLON;
        case KEY_APOSTROPHE: return KEYCODE_APOSTROPHE; case KEY_GRAVE: return KEYCODE_GRAVE;
        case KEY_SLASH: return KEYCODE_SLASH; case KEY_CAPSLOCK: return KEYCODE_CAPS_LOCK;
        case KEY_SCROLLLOCK: return KEYCODE_SCROLL_LOCK; case KEY_INSERT: return KEYCODE_INSERT;
        case KEY_NUMLOCK: return KEYCODE_NUM_LOCK; case KEY_PAUSE: return KEYCODE_BREAK;
        case KEY_F11: return KEYCODE_F11; case KEY_F12: return KEYCODE_F12;
        case KEY_KP0: return KEYCODE_NUMPAD_0; case KEY_KP1: return KEYCODE_NUMPAD_1;
        case KEY_KP2: return KEYCODE_NUMPAD_2; case KEY_KP3: return KEYCODE_NUMPAD_3;
        case KEY_KP4: return KEYCODE_NUMPAD_4; case KEY_KP5: return KEYCODE_NUMPAD_5;
        case KEY_KP6: return KEYCODE_NUMPAD_6; case KEY_KP7: return KEYCODE_NUMPAD_7;
        case KEY_KP8: return KEYCODE_NUMPAD_8; case KEY_KP9: return KEYCODE_NUMPAD_9;
        case KEY_KPSLASH: return KEYCODE_NUMPAD_DIVIDE; case KEY_KPASTERISK: return KEYCODE_NUMPAD_MULTIPLY;
        case KEY_KPMINUS: return KEYCODE_NUMPAD_SUBTRACT; case KEY_KPPLUS: return KEYCODE_NUMPAD_ADD;
        case KEY_KPDOT: return KEYCODE_NUMPAD_DOT; case KEY_KPEQUAL: return KEYCODE_NUMPAD_EQUALS;
        case KEY_POWER: return KEYCODE_POWER; case KEY_VOLUMEUP: return KEYCODE_VOLUME_UP;
        case KEY_VOLUMEDOWN: return KEYCODE_VOLUME_DOWN;
        default: return -1;
    }
}
// Device commands use argv directly; text and filenames never enter a shell.
bool command(const std::vector<std::string> &args, std::string *output = nullptr) {
    if (args.empty()) { return false; }
    std::vector<char *> argv;
    for (const auto &arg : args) { argv.push_back(const_cast<char *>(arg.c_str())); }
    argv.push_back(nullptr);
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC)) { return false; }
    pid_t pid = fork();
    if (!pid) {
        close(pipes[0]); dup2(pipes[1], STDOUT_FILENO); close(pipes[1]);
        int null = open("/dev/null", O_WRONLY); if (null >= 0) { dup2(null, STDERR_FILENO); close(null); }
        execv(argv[0], argv.data()); _exit(127);
    }
    close(pipes[1]);
    if (pid < 0) { close(pipes[0]); return false; }
    int64_t deadline = harmony::now_us() + 10000000;
    char buffer[4096]; bool ended = false;
    while (harmony::now_us() < deadline) {
        pollfd poller{pipes[0], POLLIN, 0};
        if (poll(&poller, 1, 100) <= 0) { continue; }
        auto count = read(pipes[0], buffer, sizeof(buffer));
        if (count <= 0) { ended = true; break; }
        if (output && output->size() < 2 * 1024 * 1024) { output->append(buffer, count); }
    }
    close(pipes[0]);
    if (!ended) { kill(pid, SIGKILL); }
    int status = 0; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return ended && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
void uitest(const char *op, const char *arg) {
    if (!command({"/system/bin/uitest", "uiInput", op, arg})) { fprintf(stderr, "[harmony] UITest %s failed\n", op); }
}
bool resolve_app(std::string name, std::string &bundle, std::string &ability) {
    while (!name.empty() && (name[0] == '+' || name[0] == '?')) { name.erase(0, 1); }
    auto slash = name.find('/');
    bundle = name.substr(0, slash);
    if (bundle.empty() || bundle.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos) { return false; }
    ability.clear();
    if (slash != std::string::npos) { ability = name.substr(slash + 1); }
    if (ability.empty()) {
        std::string metadata;
        if (!command({"/system/bin/bm", "dump", "-n", bundle}, &metadata)) { return false; }
        std::smatch match;
        if (std::regex_search(metadata, match, std::regex("\"(?:mainAbility|mainElementName)\"\\s*:\\s*\"([^\"]+)\""))) { ability = match[1]; }
    }
    if (ability.empty()) { return false; }
    if (slash == std::string::npos && ability.find('.') != std::string::npos) {
        auto short_name = ability.substr(ability.rfind('.') + 1);
        std::string metadata;
        if (command({"/system/bin/bm", "dump", "-n", bundle}, &metadata)
                && std::regex_search(metadata, std::regex("\"name\"\\s*:\\s*\"" + short_name + "\""))) { ability = short_name; }
    }
    return true;
}
bool start_app(std::string name, uint32_t display = 0) {
    std::string bundle, ability;
    if (!resolve_app(name, bundle, ability)) { return false; }
    if (scrcpy_harmony_launch_hook) {
        auto resolved = bundle + "/" + ability;
        fprintf(stderr, "[harmony] launching %s on display %u\n", resolved.c_str(), display);
        scrcpy_harmony_launch_hook(resolved.c_str(), display); return true;
    }
    if (display) {
        int result = harmony::start_on_display(bundle, ability, display);
        fprintf(stderr, "[harmony] app %s on display %u: result %d\n", bundle.c_str(), display, result);
        return result == 0;
    }
    std::vector<std::string> args{"/system/bin/aa", "start", "-b", bundle, "-a", ability};
    if (display) { args.insert(args.end(), {"--pi", "ohos.aafwk.param.displayId", std::to_string(display)}); }
    std::string result;
    bool ok = command(args, &result) && result.find("start ability successfully") != std::string::npos;
    if (!ok && name.find('/') == std::string::npos && ability.find('.') != std::string::npos) {
        args[5] = ability.substr(ability.rfind('.') + 1);
        result.clear();
        ok = command(args, &result) && result.find("start ability successfully") != std::string::npos;
    }
    fprintf(stderr, "[harmony] app %s on display %u: %s\n", bundle.c_str(), display, ok ? "started" : result.c_str());
    return ok;
}
struct Controller {
    int fd;
    int media_fd = -1;
    Stream &stream;
    VirtualInput &input;
    bool autosync;
    harmony::ControlWriter writer;
    harmony::Clipboard clipboard;
    harmony::Hid hid;
    harmony::PlatformDisplay display;
    harmony::NativeInput native_input;
    uint32_t target_display = 0;
    std::atomic<int> target_width{0}, target_height{0};
    std::map<uint64_t, int> fingers;
    std::map<int, bool> keys;
    int last_x = 0, last_y = 0;
    std::atomic<int> resize_width{0}, resize_height{0};
    bool wakeful = false;
    Controller(int socket, Stream &video, VirtualInput &devices, bool sync)
        : fd(socket), stream(video), input(devices), autosync(sync), writer(socket), hid(writer) {}
    ~Controller() { if (wakeful) { command({"/system/bin/power-shell", "timeout", "-r"}); } }
    void stay_awake() {
        if (command({"/system/bin/power-shell", "timeout", "-o", "3600000"})) { wakeful = true; }
    }
    void key(int code, bool down, bool repeat = false) {
        bool ok;
        if (target_display || display.off()) {
            int mapped = native_keycode(code);
            ok = mapped >= 0 && native_input.key(target_display, mapped, down, repeat);
        } else { ok = input.key(code, down, repeat); }
        if (!ok) { fprintf(stderr, "[harmony] key injection failed for %d\n", code); }
        if (down) { keys[code] = true; } else { keys.erase(code); }
    }
    void chord(int modifier, int code) {
        key(modifier, true); key(code, true); key(code, false); key(modifier, false);
    }
    void touch(int finger, int action, int x, int y) {
        bool ok = target_display || display.off()
            ? native_input.touch(target_display, finger, action, x, y) : input.touch(finger, action, x, y);
        if (!ok) { stream.alive = false; }
        last_x = x; last_y = y;
    }
    void swipe(int x, int y, int end_x, int end_y, bool hold = false) {
        if (!fingers.empty()) { return; }
        touch(9, TOUCH_ACTION_DOWN, x, y);
        for (int i = 1; i <= 12; ++i) {
            touch(9, TOUCH_ACTION_MOVE, x + (end_x - x) * i / 12, y + (end_y - y) * i / 12);
            usleep(16000);
        }
        if (hold) { usleep(500000); }
        touch(9, TOUCH_ACTION_UP, end_x, end_y);
    }
    bool position(const uint8_t *p, int &x, int &y) {
        int32_t w, h;
        int vw = be16(p + 8), vh = be16(p + 10);
        if (!vw || !vh) { return false; }
        if (target_display) { w = target_width; h = target_height; }
        else if (!dimensions(w, h)) { return false; }
        x = std::clamp<int64_t>(int64_t(int32_t(be32(p))) * w / vw, 0, w - 1);
        y = std::clamp<int64_t>(int64_t(int32_t(be32(p + 4))) * h / vh, 0, h - 1);
        return true;
    }
    bool string(std::string &out, bool tiny = false) {
        uint8_t length[4];
        if (!read_all(fd, length, tiny ? 1 : 4)) { return false; }
        auto size = tiny ? length[0] : be32(length);
        if (size > 256 * 1024 - 14) { return false; }
        out.resize(size);
        return read_all(fd, out.data(), size);
    }
    bool blob(std::vector<uint8_t> &out, uint16_t size) {
        out.resize(size); return read_all(fd, out.data(), size);
    }
    void paste(const std::string &text) {
        if (clipboard.set(text)) { chord(KEY_LEFTCTRL, KEY_V); }
        else { uitest("text", text.c_str()); }
    }
    void run() {
        if (autosync) { clipboard.watch(writer); }
        uint8_t type, b[32];
        while (stream.alive && read_all(fd, &type, 1)) {
            if (type == 0) {
                if (!read_all(fd, b, 13)) { break; }
                auto code = be32(b + 1);
                if (code == 3 || code == 4 || code == 187) {
                    if (target_display) {
                        native_input.navigation(target_display, code, !b[0]);
                    } else if (!b[0]) {
                        if (code == 187) {
                            int32_t w, h;
                            if (dimensions(w, h)) { swipe(w / 2, h - 2, w / 2, h / 2, true); }
                        } else { uitest("keyEvent", code == 3 ? "Home" : "Back"); }
                    }
                } else { int mapped = keycode(code); if (mapped >= 0) { key(mapped, !b[0], be32(b + 5) != 0); } }
            } else if (type == 1 || type == 9) {
                if (type == 9 && !read_all(fd, b, 9)) { break; }
                std::string text;
                if (!string(text)) { break; }
                bool ok;
                if (type == 1) { paste(text); ok = true; }
                else {
                    ok = clipboard.set(text);
                    if (ok && b[8]) { chord(KEY_LEFTCTRL, KEY_V); }
                    if (ok && !writer.ack(be64(b))) { break; }
                }
                if (!ok) { fprintf(stderr, "[harmony] clipboard write failed\n"); }
            } else if (type == 2) {
                if (!read_all(fd, b, 31)) { break; }
                uint64_t id = be64(b + 1); int x, y;
                int action = b[0] & 0xff;
                if (!position(b + 9, x, y)) { continue; }
                if (action == 7) {
                    if (!native_input.hover(target_display, x, y) && !target_display && !display.off()) {
                        input.hover(x, y);
                    }
                    continue;
                }
                if (action == 0 || action == 5) {
                    if (!fingers.count(id)) {
                        int finger = 0;
                        for (;; ++finger) {
                            bool used = false;
                            for (auto &f : fingers) { if (f.second == finger) { used = true; } }
                            if (!used) { break; }
                        }
                        if (finger >= 10) { continue; }
                        fingers[id] = finger;
                    }
                    touch(fingers[id], TOUCH_ACTION_DOWN, x, y);
                } else if (fingers.count(id)) {
                    touch(fingers[id], action == 2 ? TOUCH_ACTION_MOVE : action == 3 ? TOUCH_ACTION_CANCEL : TOUCH_ACTION_UP, x, y);
                    if (action != 2) { fingers.erase(id); }
                }
            } else if (type == 3) {
                if (!read_all(fd, b, 20)) { break; }
                int x, y;
                if (position(b, x, y)) {
                    double horizontal = int16_t(be16(b + 12)) / 2048.0, vertical = int16_t(be16(b + 14)) / 2048.0;
                    if (!native_input.scroll(target_display, x, y, horizontal, vertical)
                            && !target_display && !display.off()) {
                        input.scroll(x, y, horizontal, vertical);
                    }
                }
            } else if (type == 4) {
                if (!read_all(fd, b, 1)) { break; }
                if (target_display) { native_input.navigation(target_display, 4, !b[0]); }
                else if (!b[0]) { if (display.off()) { display.power(true); } else { uitest("keyEvent", "Back"); } }
            } else if (type == 5 || type == 6) {
                int32_t w, h;
                if (dimensions(w, h)) { int x = type == 5 ? w / 4 : 3 * w / 4; swipe(x, 2, x, 3 * h / 4); }
            } else if (type == 7) {
                uitest("keyEvent", "Back");
            } else if (type == 8) {
                if (!read_all(fd, b, 1)) { break; }
                if (b[0]) { chord(KEY_LEFTCTRL, b[0] == 2 ? KEY_X : KEY_C); usleep(100000); }
                std::string text;
                if (clipboard.get(text)) { if (!writer.clipboard(text)) { break; } }
            } else if (type == 10) {
                if (!read_all(fd, b, 1)) { break; }
                display.power(b[0] != 0);
            } else if (type == 11) {
                if (target_display) {
                    resize_width = target_height.load(); resize_height = target_width.load();
                } else if (!display.rotate()) { fprintf(stderr, "[harmony] display rotation unavailable\n"); }
            } else if (type == 12) {
                if (!read_all(fd, b, 6)) { break; }
                uint16_t id = be16(b), vendor = be16(b + 2), product = be16(b + 4);
                std::string name;
                if (!string(name, true) || !read_all(fd, b, 2)) { break; }
                std::vector<uint8_t> descriptor;
                if (!blob(descriptor, be16(b))) { break; }
                if (!hid.create(id, vendor, product, name, descriptor)) { fprintf(stderr, "[harmony] UHID creation failed\n"); }
            } else if (type == 13) {
                if (!read_all(fd, b, 4)) { break; }
                uint16_t id = be16(b), size = be16(b + 2);
                std::vector<uint8_t> report;
                if (!blob(report, size)) { break; }
                hid.input(id, report);
            } else if (type == 14) {
                if (!read_all(fd, b, 2)) { break; } hid.destroy(be16(b));
            } else if (type == 15) {
                start_app("com.huawei.hmos.settings");
            } else if (type == 16) {
                std::string name; if (!string(name, true)) { break; }
                fprintf(stderr, "[harmony] requested app: %s\n", name.c_str());
                if (media_fd >= 0) {
                    std::string bundle, ability;
                    if (!resolve_app(name, bundle, ability)) { fprintf(stderr, "[harmony] app resolution failed\n"); continue; }
                    name = bundle + "/" + ability;
                    if (name.size() > 255) { continue; }
                    uint8_t header[2] = {16, uint8_t(name.size())};
                    harmony::send_all(media_fd, header, 2); harmony::send_all(media_fd, name.data(), name.size());
                } else if (!start_app(name, target_display)) { fprintf(stderr, "[harmony] app start failed\n"); }
            } else if (type == 17) {
                if (media_fd >= 0) { harmony::send_all(media_fd, &type, 1); }
                else { stream.reset = true; }
            } else if (type == 18) {
                if (!read_all(fd, b, 1)) { break; }
                if (media_fd >= 0) { uint8_t request[2] = {type, b[0]}; harmony::send_all(media_fd, request, 2); }
                else if (stream.camera_owner) { stream.camera_owner->torch(b[0]); }
                else { fprintf(stderr, "[harmony] torch requires a camera capture session\n"); }
            } else if (type == 19 || type == 20) {
                if (media_fd >= 0) { harmony::send_all(media_fd, &type, 1); }
                else if (stream.camera_owner) { stream.camera_owner->zoom(type == 19); }
                else { fprintf(stderr, "[harmony] camera zoom requires a camera capture session\n"); }
            } else if (type == 21) {
                if (!read_all(fd, b, 4)) { break; }
                resize_width = be16(b); resize_height = be16(b + 2); stream.reset = true;
            } else if (type == 22) {
                std::string path; if (!string(path)) { break; }
                command({"/system/bin/mediatool", "send", path});
            } else {
                fprintf(stderr, "[harmony] invalid control message %u; closing control\n", type);
                break;
            }
        }
        clipboard.stop();
        for (auto &f : fingers) { touch(f.second, TOUCH_ACTION_CANCEL, last_x, last_y); }
        auto held = keys;
        for (auto &k : held) { key(k.first, false); }
        stream.alive = false;
    }
};
int number(const char *value, int low, int high) {
    char *end; long v = strtol(value, &end, 10);
    if (!*value || *end || v < low || v > high) { return -1; }
    return int(v);
}
int control_only(int argc, char **argv) {
    if (argc < 5) { return 2; }
    int port = number(argv[2], 1024, 65535), media_port = number(argv[3], 1024, 65535);
    bool control = !strcmp(argv[4], "1"), autosync = true;
    if (port < 0 || media_port < 0) { return 2; }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);
    fprintf(stderr, "[harmony] pid: %d\n", getpid());
    Stream stream(-1); VirtualInput input;
    if (control && !input.init()) { return 1; }
    Controller controller(-1, stream, input, autosync);
    int virtual_width = 0, virtual_height = 0;
    std::string auth;
    for (int i = 5; i < argc; ++i) {
        if (!strcmp(argv[i], "clipboard=false")) { controller.autosync = false; }
        else if (!strcmp(argv[i], "stay_awake=true")) { controller.stay_awake(); }
        else if (!strncmp(argv[i], "auth=", 5)) { auth = argv[i] + 5; }
        else if (sscanf(argv[i], "new_display=%dx%d", &virtual_width, &virtual_height) == 2) {
            if (virtual_width < 128 || virtual_height < 128 || virtual_width > 8192 || virtual_height > 8192) { return 2; }
        } else if (!strncmp(argv[i], "display_id=", 11)) {
            int id = number(argv[i] + 11, 0, 100000);
            NativeDisplayManager_DisplayInfo *info = nullptr;
            if (id < 0 || OH_NativeDisplayManager_CreateDisplayById(id, &info) || !info) { return 2; }
            controller.target_display = id; controller.target_width = info->width; controller.target_height = info->height;
            OH_NativeDisplayManager_DestroyDisplay(info);
        } else { return 2; }
    }
    if (virtual_width) {
        auto id = controller.display.create_virtual(virtual_width, virtual_height);
        if (id == UINT64_MAX) { return 1; }
        controller.target_display = id; controller.target_width = virtual_width; controller.target_height = virtual_height;
        fprintf(stderr, "[harmony] virtual_display=%llu\n", (unsigned long long) id);
    }
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
    if (listener < 0 || bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) || listen(listener, 1)) { return 1; }
    fprintf(stderr, "[harmony] control ready on device loopback:%d\n", port);
    if (!control) {
        while (!interrupted) { usleep(100000); }
        close(listener); return 0;
    }
    int fd = accept_client(listener, auth); close(listener);
    if (fd < 0) { return 1; }
    int media = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    address.sin_port = htons(media_port);
    if (media < 0 || connect(media, reinterpret_cast<sockaddr *>(&address), sizeof(address))) { close(fd); if (media >= 0) { close(media); } return 1; }
    if (!auth.empty() && !harmony::send_all(media, auth.data(), auth.size())) { close(fd); close(media); return 1; }
    timeval timeout{5, 0}; setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(media, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    controller.fd = fd; controller.writer.set_socket(fd); controller.media_fd = media;
    std::thread worker([&] { controller.run(); });
    while (!interrupted && stream.alive) {
        pollfd p{media, POLLIN, 0};
        if (poll(&p, 1, 100) > 0) { uint8_t byte; if (recv(media, &byte, 1, MSG_PEEK) <= 0) { break; } }
        int w = controller.resize_width.exchange(0), h = controller.resize_height.exchange(0);
        if (w >= 128 && h >= 128 && controller.display.resize_virtual(w, h)) {
            controller.target_width = w; controller.target_height = h;
            uint8_t reset = 17; harmony::send_all(media, &reset, 1);
        }
    }
    shutdown(fd, SHUT_RDWR); shutdown(media, SHUT_RDWR); worker.join(); close(fd); close(media);
    return 0;
}
} // namespace

extern "C" void scrcpy_harmony_request_stop() { interrupted = 1; }

int main(int argc, char **argv) {
    interrupted = 0;
    if (argc >= 2 && !strcmp(argv[1], "--control-only")) { return control_only(argc, argv); }
    if (argc < 6) {
        fprintf(stderr, "Usage: scrcpy-harmony-server PORT MAX_SIZE FPS BITRATE CONTROL(0|1)\n"); return 2;
    }
    int port = number(argv[1], 1024, 65535), max_size = number(argv[2], 0, 8192);
    int fps = number(argv[3], 0, 120), bitrate = number(argv[4], 64000, 100000000);
    int control = number(argv[5], 0, 1);
    if (port < 0 || max_size < 0 || fps < 0 || bitrate < 0 || control < 0) { return 2; }
    bool audio_enabled = false, clipboard_autosync = true, media_only = false, stay_awake = false;
    int audio_bitrate = 128000;
    std::string audio_codec = "opus", audio_source = "output", camera_id, video_codec = "h264";
    bool camera_mode = false, camera_front = false, camera_torch = false;
    int camera_width = 0, camera_height = 0, camera_fps = 0;
    std::string auth, session_id;
    int virtual_width = 0, virtual_height = 0;
    uint64_t selected_display = UINT64_MAX;
    for (int i = 6; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "audio=true") { audio_enabled = true; }
        else if (arg == "stay_awake=true") { stay_awake = true; }
        else if (arg.rfind("auth=", 0) == 0) { auth = arg.substr(5); }
        else if (arg.rfind("session_id=", 0) == 0) {
            session_id = arg.substr(11);
            if (!std::regex_match(session_id, std::regex("[a-f0-9]{32}"))) { return 2; }
        }
        else if (arg.rfind("video_codec=", 0) == 0) { video_codec = arg.substr(12); }
        else if (arg == "media_only=true") { media_only = true; }
        else if (arg == "audio=false") { audio_enabled = false; }
        else if (arg == "clipboard=false") { clipboard_autosync = false; }
        else if (arg == "video_source=camera") { camera_mode = true; }
        else if (arg == "camera_facing=front") { camera_front = true; }
        else if (arg == "camera_facing=back") { camera_front = false; }
        else if (arg == "camera_torch=true") { camera_torch = true; }
        else if (arg.rfind("camera_id=", 0) == 0) { camera_id = arg.substr(10); }
        else if (arg.rfind("camera_size=", 0) == 0) {
            if (sscanf(arg.c_str() + 12, "%dx%d", &camera_width, &camera_height) != 2) { return 2; }
        }
        else if (arg.rfind("camera_fps=", 0) == 0) { camera_fps = number(arg.c_str() + 11, 1, 120); if (camera_fps < 0) { return 2; } }
        else if (arg.rfind("audio_source=", 0) == 0) { audio_source = arg.substr(13); if (audio_source != "mic" && audio_source != "output") { return 2; } }
        else if (arg.rfind("audio_codec=", 0) == 0) { audio_codec = arg.substr(12); }
        else if (arg.rfind("audio_bitrate=", 0) == 0) { audio_bitrate = number(arg.c_str() + 14, 6000, 1000000); }
        else if (arg.rfind("new_display=", 0) == 0) {
            if (sscanf(arg.c_str() + 12, "%dx%d", &virtual_width, &virtual_height) != 2
                    || virtual_width < 128 || virtual_height < 128 || virtual_width > 8192 || virtual_height > 8192) { return 2; }
        }
        else if (arg.rfind("display_id=", 0) == 0) {
            int value = number(arg.c_str() + 11, 0, 100000); if (value < 0) { return 2; } selected_display = value;
        }
        else { fprintf(stderr, "[harmony] unknown option %s\n", argv[i]); return 2; }
    }
    if (audio_codec != "opus" && audio_codec != "aac" && audio_codec != "flac" && audio_codec != "raw") { return 2; }
    if (audio_bitrate < 0) { return 2; }
    if (video_codec != "h264" && video_codec != "h265" && video_codec != "av1") { return 2; }
    if (!fps) {
        uint32_t refresh = 60; OH_NativeDisplayManager_GetDefaultDisplayRefreshRate(&refresh);
        fps = std::clamp<int>(refresh, 1, 120);
    }
    if (!media_only) { signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN); }
    fprintf(stderr, "[harmony] pid: %d\n", getpid());
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (listener < 0 || bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) || listen(listener, 2)) {
        perror("[harmony] listen"); return 1;
    }
    fprintf(stderr, "[harmony] ready on device loopback:%d\n", port);
    if (!session_id.empty()) { fprintf(stderr, "[harmony] session ready: %s\n", session_id.c_str()); }
    int video = accept_client(listener, auth);
    if (video < 0) { close(listener); return 1; }
    timeval timeout{5, 0}; setsockopt(video, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    uint8_t hello[69]{}; memcpy(hello + 1, "HarmonyOS", 9);
    memcpy(hello + 65, video_codec == "av1" ? "\0av1" : video_codec.c_str(), 4);
    if (!write_all(video, hello, sizeof(hello))) { close(video); close(listener); return 1; }
    int audio_socket = audio_enabled ? accept_client(listener, auth) : -1;
    if (audio_enabled && audio_socket < 0) { close(video); close(listener); return 1; }
    if (audio_socket >= 0) { setsockopt(audio_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)); }
    int input = control ? accept_client(listener, auth) : -1;
    close(listener);
    if (control && input < 0) { close(video); return 1; }
    harmony::Camera camera;
    if (camera_mode && !camera.open(camera_id, camera_front, camera_width, camera_height, camera_fps, max_size)) {
        shutdown(video, SHUT_RDWR); close(video);
        if (input >= 0) { shutdown(input, SHUT_RDWR); close(input); }
        if (audio_socket >= 0) { shutdown(audio_socket, SHUT_RDWR); close(audio_socket); }
        return 1;
    }
    harmony::Audio audio(audio_socket);
    harmony::AudioCapture independent_audio;
    if (audio_enabled && ((audio_source == "mic" && !independent_audio.start(audio, true))
            || !audio.start(audio_codec, audio_bitrate))) {
        independent_audio.stop();
        uint8_t disabled[4]{}; harmony::send_all(audio_socket, disabled, 4);
        fprintf(stderr, "[harmony] audio unavailable; continuing video\n");
        audio_enabled = false;
    }
    Stream stream(video);
    stream.video_codec = video_codec;
    // AVScreenCapture owns the authorized playback filter. A standalone
    // PLAYBACK_CAPTURE capturer has no populated filter in a normal app.
    if (audio_enabled && audio_source == "output") { stream.audio = &audio; }
    if (camera_mode) { stream.camera_owner = &camera; }
    VirtualInput virtual_input;
    if (control && !media_only && !virtual_input.init()) {
        close(input); close(video); return 1;
    }
    Controller controller(input, stream, virtual_input, clipboard_autosync);
    if (stay_awake && !media_only) { controller.stay_awake(); }
    if (virtual_width) {
        selected_display = controller.display.create_virtual(virtual_width, virtual_height);
        if (selected_display == UINT64_MAX) {
            fprintf(stderr, "[harmony] virtual display creation failed\n");
            close(video); if (input >= 0) { close(input); } return 1;
        }
        stream.virtual_owner = &controller.display;
    }
    stream.display_id = selected_display;
    if (selected_display != UINT64_MAX && selected_display != 0) {
        controller.target_display = selected_display;
        controller.target_width = virtual_width; controller.target_height = virtual_height;
        if (!media_only && !controller.native_input.available()) { fprintf(stderr, "[harmony] targeted display input unavailable\n"); return 1; }
    }
    std::thread controls;
    if (control) {
        if (media_only) {
            controls = std::thread([&] {
                uint8_t type;
                while (stream.alive && read_all(input, &type, 1)) {
                    if (type == 17) { stream.reset = true; }
                    else if (type == 16) {
                        std::string name; if (!controller.string(name, true)) { break; }
                        std::lock_guard<std::mutex> lock(stream.launch_mutex);
                        if (stream.pending_launches.size() < 8) { stream.pending_launches.push_back(std::move(name)); }
                    }
                    else if (type == 18) { uint8_t enabled; if (!read_all(input, &enabled, 1)) { break; } if (stream.camera_owner) { stream.camera_owner->torch(enabled); } }
                    else if (type == 19 || type == 20) { if (stream.camera_owner) { stream.camera_owner->zoom(type == 19); } }
                    else if (type != 19 && type != 20) { break; }
                }
                stream.alive = false;
            });
        } else { controls = std::thread([&] { controller.run(); }); }
    }
    int32_t old_w = 0, old_h = 0;
    unsigned session_frames = 0;
    int first_frame_wait = 0;
    bool ok = true;
    while (!interrupted && stream.alive) {
        int32_t w, h;
        if (camera_mode) {
            if (!camera.healthy()) { ok = false; break; }
            w = camera.width; h = camera.height;
        } else if (selected_display != UINT64_MAX && selected_display != 0) {
            NativeDisplayManager_DisplayInfo *info = nullptr;
            if (OH_NativeDisplayManager_CreateDisplayById(selected_display, &info) || !info) { ok = false; break; }
            w = info->width; h = info->height; OH_NativeDisplayManager_DestroyDisplay(info);
            int resize_w = controller.resize_width.exchange(0), resize_h = controller.resize_height.exchange(0);
            if (resize_w >= 128 && resize_h >= 128 && controller.display.resize_virtual(resize_w, resize_h)) {
                w = resize_w; h = resize_h;
            }
            controller.target_width = w; controller.target_height = h;
        } else if (!dimensions(w, h)) { ok = false; break; }
        if (w != old_w || h != old_h || stream.reset.exchange(false)) {
            stream.stop();
            if (!stream.alive) { break; }
            double scale = max_size && !camera_mode ? std::min(1.0, double(max_size) / std::max(w, h)) : 1.0;
            int vw = std::max(2, int(w * scale) & ~1), vh = std::max(2, int(h * scale) & ~1);
            if (!stream.start(vw, vh, bitrate, camera_mode ? camera.fps : fps)) { ok = false; break; }
            if (camera_mode && camera_torch) { camera.torch(true); camera_torch = false; }
            old_w = w; old_h = h;
            session_frames = stream.frames.load();
            first_frame_wait = 0;
        }
        if (stream.frames == session_frames && ++first_frame_wait >= 1800) {
            fprintf(stderr, "[harmony] no video frames within 180s; check capture authorization\n");
            ok = false; break;
        }
        if (stream.capturing) {
            std::string launch;
            { std::lock_guard<std::mutex> lock(stream.launch_mutex);
              if (!stream.pending_launches.empty()) { launch = std::move(stream.pending_launches.front()); stream.pending_launches.pop_front(); } }
            if (!launch.empty()) { start_app(launch, controller.target_display); }
        }
        // Detect peer loss even when the compositor produces no new frames.
        pollfd p{video, POLLIN, 0};
        if (poll(&p, 1, 100) > 0 && (p.revents & (POLLHUP | POLLERR | POLLIN))) { break; }
    }
    stream.alive = false;
    shutdown(video, SHUT_RDWR);
    if (input >= 0) { shutdown(input, SHUT_RDWR); controls.join(); close(input); }
    stream.stop();
    independent_audio.stop();
    if (audio_socket >= 0) { shutdown(audio_socket, SHUT_RDWR); audio.stop(); close(audio_socket); }
    close(video);
    fprintf(stderr, "[harmony] stopped; sent %u video frames\n", stream.frames.load());
    return ok ? 0 : 1;
}
