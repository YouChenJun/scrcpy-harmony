/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SCRCPY_HARMONY_NATIVE_INPUT_H
#define SCRCPY_HARMONY_NATIVE_INPUT_H
#include <array>
#include <cstdint>
#include <dlfcn.h>
#include <map>
#include <utility>
#include "wire.h"
#include "native_ref.h"
namespace harmony {
// Optional OpenHarmony inner-kit bridge for input targeted at a virtual display.
// Objects are created/updated/destroyed by the platform library; no field offsets
// are inspected. Native shared_ptr retain/release functions preserve its ABI.
class NativeInput {
    void *library = dlopen("libmmi-client.z.so", RTLD_NOW | RTLD_LOCAL);
    template<typename T> T symbol(const char *name) {
        return library ? reinterpret_cast<T>(dlsym(library, name)) : nullptr;
    }
    using Shared = NativeShared;
    struct Point { int x, y; int64_t down; bool pressed; };
    std::map<int, Point> points;
    std::map<int, Point> mouse_points;
    std::map<int, int64_t> keys;
    int64_t down_time = 0;
public:
    // InputManager owns background IPC threads. Keep its library loaded until
    // process exit; unloading it while those threads run is unsafe.
    ~NativeInput() = default;
    bool available() {
        return symbol<void *(*)()>("_ZN4OHOS3MMI12InputManager11GetInstanceEv")
            && symbol<void *>("_ZN4OHOS3MMI12PointerEvent6CreateEv")
            && symbol<void *>("_ZN4OHOS3MMI12InputManager18SimulateInputEventENSt3__h10shared_ptrINS0_12PointerEventEEEbi");
    }
    bool navigation(uint32_t display, int android, bool down) {
        auto code = symbol<const int *>(android == 3 ? "_ZN4OHOS3MMI8KeyEvent12KEYCODE_HOMEE"
            : android == 187 ? "_ZN4OHOS3MMI8KeyEvent14KEYCODE_RECENTE" : "_ZN4OHOS3MMI8KeyEvent12KEYCODE_BACKE");
        return code && key(display, *code, down);
    }
    bool key(uint32_t display, int code, bool down, bool repeated = false) {
        auto create = symbol<Shared(*)()>("_ZN4OHOS3MMI8KeyEvent6CreateEv");
        auto instance = symbol<void *(*)()>("_ZN4OHOS3MMI12InputManager11GetInstanceEv");
        auto inject = symbol<void(*)(void *, Shared)>("_ZN4OHOS3MMI12InputManager18SimulateInputEventENSt3__h10shared_ptrINS0_8KeyEventEEE");
        auto target = symbol<void(*)(void *, int)>("_ZN4OHOS3MMI10InputEvent18SetTargetDisplayIdEi");
        auto time = symbol<void(*)(void *, int64_t)>("_ZN4OHOS3MMI10InputEvent13SetActionTimeEl");
        auto action = symbol<void(*)(void *, int)>("_ZN4OHOS3MMI8KeyEvent12SetKeyActionEi");
        auto base_action = symbol<void(*)(void *, int)>("_ZN4OHOS3MMI10InputEvent9SetActionEi");
        auto set_code = symbol<void(*)(void *, int)>("_ZN4OHOS3MMI8KeyEvent10SetKeyCodeEi");
        auto repeat = symbol<void(*)(void *, bool)>("_ZN4OHOS3MMI8KeyEvent9SetRepeatEb");
        auto construct = symbol<void(*)(void *)>("_ZN4OHOS3MMI8KeyEvent7KeyItemC1Ev");
        auto destroy = symbol<void(*)(void *)>("_ZN4OHOS3MMI8KeyEvent7KeyItemD1Ev");
        auto item_code = symbol<void(*)(void *, int)>("_ZN4OHOS3MMI8KeyEvent7KeyItem10SetKeyCodeEi");
        auto item_pressed = symbol<void(*)(void *, bool)>("_ZN4OHOS3MMI8KeyEvent7KeyItem10SetPressedEb");
        auto item_time = symbol<void(*)(void *, int64_t)>("_ZN4OHOS3MMI8KeyEvent7KeyItem11SetDownTimeEl");
        auto add = symbol<void(*)(void *, const void *)>("_ZN4OHOS3MMI8KeyEvent10AddKeyItemERKNS1_7KeyItemE");
        auto action_value = symbol<const int *>(down ? "_ZN4OHOS3MMI8KeyEvent15KEY_ACTION_DOWNE" : "_ZN4OHOS3MMI8KeyEvent13KEY_ACTION_UPE");
        if (!create || !instance || !inject || !target || !time || !action || !set_code || !base_action
                || !construct || !destroy || !item_code || !item_pressed || !item_time || !add || !action_value) { return false; }
        int64_t now = now_us();
        if (!keys.count(code)) { keys[code] = now; }
        auto event = create(); if (!event.object) { return false; }
        target(event.object, display); time(event.object, now);
        action(event.object, *action_value); base_action(event.object, *action_value); set_code(event.object, code);
        if (repeat) { repeat(event.object, repeated); }
        for (auto &key : keys) {
            alignas(16) std::array<uint8_t, 128> storage{};
            construct(storage.data()); item_code(storage.data(), key.first);
            item_pressed(storage.data(), key.first != code || down); item_time(storage.data(), key.second);
            add(event.object, storage.data()); destroy(storage.data());
        }
        inject(instance(), event);
        if (!down) { keys.erase(code); }
        return true;
    }
    bool touch(uint32_t display, int id, int action, int x, int y,
               bool mouse = false, double horizontal = 0, double vertical = 0) {
        auto &active_points = mouse ? mouse_points : points;
        auto create = symbol<Shared(*)()>("_ZN4OHOS3MMI12PointerEvent6CreateEv");
        auto instance = symbol<void *(*)()>("_ZN4OHOS3MMI12InputManager11GetInstanceEv");
        auto inject = symbol<void(*)(void *, Shared, bool, int)>(
            "_ZN4OHOS3MMI12InputManager18SimulateInputEventENSt3__h10shared_ptrINS0_12PointerEventEEEbi");
        auto construct = symbol<void(*)(void *)>("_ZN4OHOS3MMI12PointerEvent11PointerItemC1Ev");
        auto destroy = symbol<void(*)(void *)>("_ZN4OHOS3MMI12PointerEvent11PointerItemD1Ev");
        auto item_id = symbol<void(*)(void *, int)>("_ZN4OHOS3MMI12PointerEvent11PointerItem12SetPointerIdEi");
        auto item_x = symbol<void(*)(void *, int)>("_ZN4OHOS3MMI12PointerEvent11PointerItem11SetDisplayXEi");
        auto item_y = symbol<void(*)(void *, int)>("_ZN4OHOS3MMI12PointerEvent11PointerItem11SetDisplayYEi");
        auto precise_x = symbol<void(*)(void *, double)>("_ZN4OHOS3MMI12PointerEvent11PointerItem14SetDisplayXPosEd");
        auto precise_y = symbol<void(*)(void *, double)>("_ZN4OHOS3MMI12PointerEvent11PointerItem14SetDisplayYPosEd");
        auto item_pressed = symbol<void(*)(void *, bool)>("_ZN4OHOS3MMI12PointerEvent11PointerItem10SetPressedEb");
        auto item_time = symbol<void(*)(void *, int64_t)>("_ZN4OHOS3MMI12PointerEvent11PointerItem11SetDownTimeEl");
        auto item_tool = symbol<void(*)(void *, int)>("_ZN4OHOS3MMI12PointerEvent11PointerItem11SetToolTypeEi");
        auto add = symbol<void(*)(void *, void *)>("_ZN4OHOS3MMI12PointerEvent14AddPointerItemERNS1_11PointerItemE");
        auto event_id = symbol<void(*)(void *, int)>("_ZN4OHOS3MMI12PointerEvent12SetPointerIdEi");
        auto event_action = symbol<void(*)(void *, int)>("_ZN4OHOS3MMI12PointerEvent16SetPointerActionEi");
        auto source = symbol<void(*)(void *, int)>("_ZN4OHOS3MMI12PointerEvent13SetSourceTypeEi");
        auto target = symbol<void(*)(void *, int)>("_ZN4OHOS3MMI10InputEvent18SetTargetDisplayIdEi");
        auto time = symbol<void(*)(void *, int64_t)>("_ZN4OHOS3MMI10InputEvent13SetActionTimeEl");
        auto start = symbol<void(*)(void *, int64_t)>("_ZN4OHOS3MMI10InputEvent18SetActionStartTimeEl");
        auto axis = symbol<void(*)(void *, int, double)>("_ZN4OHOS3MMI12PointerEvent12SetAxisValueENS1_8AxisTypeEd");
        auto axis_type = symbol<void(*)(void *, int)>("_ZN4OHOS3MMI12PointerEvent16SetAxisEventTypeEi");
        if (!create || !instance || !inject || !construct || !destroy || !item_id || !item_x || !item_y
                || !item_pressed || !item_time || !item_tool || !add || !event_id || !event_action
                || !source || !target || !time || !start) { return false; }
        int64_t now = now_us();
        if (mouse && (horizontal || vertical) && !axis) { return false; }
        if (active_points.empty()) { down_time = now; }
        if (mouse || action == 1) { active_points[id] = {x, y, now, !mouse}; }
        else if (!active_points.count(id)) { return false; }
        auto &point = active_points[id]; point.x = x; point.y = y;
        if (!mouse && (action == 0 || action == 3)) { point.pressed = false; }
        auto event = create();
        if (!event.object) { return false; }
        source(event.object, mouse ? 1 : 2); target(event.object, display);
        event_id(event.object, id); event_action(event.object, mouse ? action : action == 0 ? 1 : action + 1);
        if (mouse && axis) {
            if (axis_type && action >= 5) { axis_type(event.object, 2); }
            // MMI wheel axes use pixels; scrcpy transmits wheel detents.
            axis(event.object, 1, -vertical * 120);
            axis(event.object, 2, horizontal * 120);
        }
        time(event.object, now); start(event.object, down_time);
        for (auto &p : active_points) {
            // PointerItem is 280 bytes in this platform ABI. Extra storage
            // leaves room for compatible trailing fields; platform methods
            // own every initialized member, including its containers.
            alignas(16) std::array<uint8_t, 512> storage{};
            construct(storage.data());
            item_id(storage.data(), p.first); item_x(storage.data(), p.second.x); item_y(storage.data(), p.second.y);
            if (precise_x) { precise_x(storage.data(), p.second.x); }
            if (precise_y) { precise_y(storage.data(), p.second.y); }
            item_pressed(storage.data(), p.second.pressed); item_time(storage.data(), p.second.down); item_tool(storage.data(), mouse ? 6 : 0);
            add(event.object, storage.data()); destroy(storage.data());
        }
        inject(instance(), event, false, 0);
        if (mouse || action == 0 || action == 3) { active_points.erase(id); }
        return true;
    }
    bool hover(uint32_t display, int x, int y) {
        return touch(display, 0, 3, x, y, true);
    }
    bool scroll(uint32_t display, int x, int y, double horizontal, double vertical) {
        return touch(display, 0, 5, x, y, true)
            && touch(display, 0, 6, x, y, true, horizontal, vertical)
            && touch(display, 0, 7, x, y, true);
    }
};
}
#endif
