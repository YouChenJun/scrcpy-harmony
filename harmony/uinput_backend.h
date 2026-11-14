/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SCRCPY_HARMONY_UINPUT_H
#define SCRCPY_HARMONY_UINPUT_H

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <window_manager/oh_display_manager.h>

// Unlike the app-level injection API, /dev/uinput delivers real input on the
// rooted test device. Devices exist only for the lifetime of this process.
class VirtualInput {
    int touch_fd = -1;
    int key_fd = -1;
    int mouse_fd = -1;
    int tracking_id = 0;
    std::array<bool, 10> pressed{};
    double wheel_x = 0, wheel_y = 0;

    static bool display(int32_t &w, int32_t &h, NativeDisplayManager_Rotation *rotation = nullptr) {
        NativeDisplayManager_DisplayInfo *info = nullptr;
        if (OH_NativeDisplayManager_CreatePrimaryDisplay(&info) || !info) { return false; }
        w = info->width; h = info->height;
        if (rotation) { *rotation = info->rotation; }
        OH_NativeDisplayManager_DestroyDisplay(info);
        return w > 1 && h > 1;
    }

    static bool enable(int fd, unsigned long request, int value) {
        return ioctl(fd, request, value) >= 0;
    }
    static bool emit(int fd, uint16_t type, uint16_t code, int value) {
        input_event event{}; event.type = type; event.code = code; event.value = value;
        ssize_t n;
        do { n = write(fd, &event, sizeof(event)); } while (n < 0 && errno == EINTR);
        return n == sizeof(event);
    }
    static int create(bool touchscreen) {
        int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
        if (fd < 0) { return -1; }
        bool ok = enable(fd, UI_SET_EVBIT, EV_KEY);
        uinput_user_dev device{};
        device.id.bustype = BUS_USB;
        device.id.vendor = 0x18d1;
        device.id.product = touchscreen ? 0x4ee7 : 0x4ee8;
        snprintf(device.name, sizeof(device.name), "scrcpy-harmony-%s", touchscreen ? "touch" : "keyboard");
        if (touchscreen) {
            ok &= enable(fd, UI_SET_EVBIT, EV_ABS) && enable(fd, UI_SET_KEYBIT, BTN_TOUCH)
               && enable(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
            for (int axis : {ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_X, ABS_Y}) {
                ok &= enable(fd, UI_SET_ABSBIT, axis);
                device.absmax[axis] = axis == ABS_MT_SLOT ? 9 : 65535;
            }
        } else {
            for (int key = 1; key <= KEY_MAX; ++key) { ok &= enable(fd, UI_SET_KEYBIT, key); }
        }
        if (!ok || write(fd, &device, sizeof(device)) != sizeof(device) || ioctl(fd, UI_DEV_CREATE) < 0) {
            close(fd); return -1;
        }
        return fd;
    }

public:
    VirtualInput() = default;
    VirtualInput(const VirtualInput &) = delete;
    VirtualInput &operator=(const VirtualInput &) = delete;
    ~VirtualInput() {
        for (int fd : {touch_fd, key_fd, mouse_fd}) {
            if (fd >= 0) { ioctl(fd, UI_DEV_DESTROY); close(fd); }
        }
    }
    bool init() {
        touch_fd = create(true);
        key_fd = create(false);
        if (touch_fd < 0 || key_fd < 0) {
            fprintf(stderr, "[harmony] /dev/uinput unavailable: root and virtual input support are required\n");
            return false;
        }
        usleep(500000); // Allow InputManager/libinput to discover both devices.
        fprintf(stderr, "[harmony] virtual touchscreen and keyboard ready\n");
        return true;
    }
    bool key(int code, bool down, bool repeat = false) {
        return emit(key_fd, EV_KEY, code, down ? repeat ? 2 : 1 : 0) && emit(key_fd, EV_SYN, SYN_REPORT, 0);
    }
    bool hover(int x, int y) {
        if (mouse_fd < 0) {
            mouse_fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
            if (mouse_fd < 0) { return false; }
            bool ok = enable(mouse_fd, UI_SET_EVBIT, EV_KEY)
                && enable(mouse_fd, UI_SET_KEYBIT, BTN_LEFT)
                && enable(mouse_fd, UI_SET_KEYBIT, BTN_RIGHT)
                && enable(mouse_fd, UI_SET_EVBIT, EV_REL)
                && enable(mouse_fd, UI_SET_RELBIT, REL_X)
                && enable(mouse_fd, UI_SET_RELBIT, REL_Y)
                && enable(mouse_fd, UI_SET_PROPBIT, INPUT_PROP_POINTER)
                && enable(mouse_fd, UI_SET_RELBIT, REL_WHEEL)
                && enable(mouse_fd, UI_SET_RELBIT, REL_HWHEEL)
                && enable(mouse_fd, UI_SET_EVBIT, EV_ABS)
                && enable(mouse_fd, UI_SET_ABSBIT, ABS_X)
                && enable(mouse_fd, UI_SET_ABSBIT, ABS_Y);
            uinput_user_dev device{};
            snprintf(device.name, sizeof(device.name), "scrcpy-harmony-mouse");
            device.id.bustype = BUS_USB; device.id.vendor = 0x18d1; device.id.product = 0x4ee9;
            device.absmax[ABS_X] = device.absmax[ABS_Y] = 65535;
            if (!ok || write(mouse_fd, &device, sizeof(device)) != sizeof(device)
                    || ioctl(mouse_fd, UI_DEV_CREATE) < 0) {
                close(mouse_fd); mouse_fd = -1; return false;
            }
            usleep(300000);
        }
        int32_t w, h;
        if (!display(w, h)) { return false; }
        return emit(mouse_fd, EV_ABS, ABS_X, std::clamp<int64_t>(int64_t(x) * 65535 / (w - 1), 0, 65535))
            && emit(mouse_fd, EV_ABS, ABS_Y, std::clamp<int64_t>(int64_t(y) * 65535 / (h - 1), 0, 65535))
            && emit(mouse_fd, EV_SYN, SYN_REPORT, 0);
    }
    bool scroll(int x, int y, double horizontal, double vertical) {
        if (!hover(x, y)) { return false; }
        wheel_x += horizontal; wheel_y += vertical;
        int dx = int(wheel_x), dy = int(wheel_y); wheel_x -= dx; wheel_y -= dy;
        bool ok = true;
        if (dx) { ok &= emit(mouse_fd, EV_REL, REL_HWHEEL, dx); }
        if (dy) { ok &= emit(mouse_fd, EV_REL, REL_WHEEL, dy); }
        return ok && emit(mouse_fd, EV_SYN, SYN_REPORT, 0);
    }
    // action: 0 cancel, 1 down, 2 move, 3 up (InputKit touch action values).
    bool touch(int slot, int action, int x, int y) {
        if (slot < 0 || slot >= int(pressed.size())) { return false; }
        int32_t w, h;
        NativeDisplayManager_Rotation rotation{};
        if (!display(w, h, &rotation)) { return false; }
        double nx = double(x) / (w - 1), ny = double(y) / (h - 1);
        // Kernel coordinates refer to the panel's natural orientation.
        double rx = nx, ry = ny;
        switch (int(rotation)) {
            case 1: rx = ny; ry = 1 - nx; break;
            case 2: rx = 1 - nx; ry = 1 - ny; break;
            case 3: rx = 1 - ny; ry = nx; break;
        }
        int raw_x = std::clamp(int(rx * 65535), 0, 65535);
        int raw_y = std::clamp(int(ry * 65535), 0, 65535);
        bool ok = emit(touch_fd, EV_ABS, ABS_MT_SLOT, slot);
        if (action == 1) {
            tracking_id = tracking_id % 65535 + 1;
            ok &= emit(touch_fd, EV_ABS, ABS_MT_TRACKING_ID, tracking_id);
            pressed[slot] = true;
        }
        if (action == 0 || action == 3) {
            ok &= emit(touch_fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
            pressed[slot] = false;
        } else {
            ok &= emit(touch_fd, EV_ABS, ABS_MT_POSITION_X, raw_x);
            ok &= emit(touch_fd, EV_ABS, ABS_MT_POSITION_Y, raw_y);
            if (!slot) {
                ok &= emit(touch_fd, EV_ABS, ABS_X, raw_x);
                ok &= emit(touch_fd, EV_ABS, ABS_Y, raw_y);
            }
        }
        bool any = std::any_of(pressed.begin(), pressed.end(), [](bool p) { return p; });
        ok &= emit(touch_fd, EV_KEY, BTN_TOUCH, any);
        ok &= emit(touch_fd, EV_SYN, SYN_REPORT, 0);
        return ok;
    }
};
#endif
