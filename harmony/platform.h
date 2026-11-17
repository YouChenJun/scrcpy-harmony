/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SCRCPY_HARMONY_PLATFORM_H
#define SCRCPY_HARMONY_PLATFORM_H
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <map>
#include <string>
#include <vector>
#include <window_manager/oh_display_manager.h>
#include <native_window/external_window.h>
#include "native_ref.h"
#include "user.h"
#include "native_string.h"
namespace harmony {
// These optional platform APIs are resolved at runtime. No private object
// layout is read: ScreenRef mirrors the one-pointer intrusive sptr return ABI.
// Missing symbols fail the operation explicitly instead of guessing offsets.
class PlatformDisplay {
    void *dm = dlopen("libdm.z.so", RTLD_NOW | RTLD_LOCAL);
    void *utils = dlopen("libutils.z.so", RTLD_NOW | RTLD_LOCAL);
    template<typename T> T symbol(const char *name) { return dm ? reinterpret_cast<T>(dlsym(dm, name)) : nullptr; }
    struct ScreenRef {
        void *pointer = nullptr;
        ScreenRef() = default;
        ScreenRef(const ScreenRef &) = delete;
        ~ScreenRef() {
            if (pointer) {
                auto lib = dlopen("libutils.z.so", RTLD_NOW | RTLD_LOCAL);
                auto release = lib ? reinterpret_cast<void(*)(void *, const void *)>(dlsym(lib, "_ZN4OHOS7RefBase12DecStrongRefEPKv")) : nullptr;
                if (release) { release(pointer, this); }
                if (lib) { dlclose(lib); }
            }
        }
    };
    bool changed_power = false;
    bool changed_rotation = false, original_rotation_lock = false;
    uint32_t original_orientation = 0;
    uint64_t owned_virtual = UINT64_MAX;
    uint64_t mirrored_group = UINT64_MAX;
    // OpenHarmony 6.0 ScreenManager's published inner-kit option ABI. Keep the
    // strings short (SSO) and pass only immutable STL values across this bridge.
    struct VirtualOption {
        NativeString name;
        VirtualOption(const char *screen_name = "GeneralUniqueScreen") : name(screen_name) {}
        uint32_t width = 0, height = 0;
        float density = 1.0f;
        void *surface = nullptr;
        int32_t flags = 0;
        bool for_shot = false;
        std::vector<uint64_t> missions;
        uint32_t type = 3; // SUPER_DESKTOP: an independent interactive desktop.
        bool secure = true;
        uint32_t flag = 0;
        bool supports_focus = true, supports_input = true;
        NativeString serial_number, bundle_name;
        uint32_t physical_width = 0, physical_height = 0;
        int32_t user_id = -1, requested_screen_id = -1;
        uint32_t render_width = 0, render_height = 0, caller = 5;
    };
    struct UniqueRotation {
        bool locked = false;
        int32_t rotation = -1;
        std::map<int32_t, int32_t> orientations;
    };
public:
    ~PlatformDisplay() {
        destroy_virtual();
        if (changed_power) { power(true); }
        if (changed_rotation) {
            orientation(original_orientation ? int(original_orientation) - 1 : -1);
            rotation_lock(original_rotation_lock);
        }
        if (dm) { dlclose(dm); } if (utils) { dlclose(utils); }
    }
    bool power(bool on) {
        auto instance = symbol<void *(*)()>("_ZN4OHOS5Rosen14DisplayManager11GetInstanceEv");
        auto set = symbol<bool(*)(void *, uint64_t, uint32_t, uint32_t)>(
            "_ZN4OHOS5Rosen14DisplayManager18SetScreenPowerByIdEmNS0_16ScreenPowerStateENS0_22PowerStateChangeReasonE");
        bool ok = instance && set && set(instance(), 0, on ? 0 : 3, 100);
        if (ok) { changed_power = !on; }
        fprintf(stderr, "[harmony] physical display %s: %s\n", on ? "on" : "off", ok ? "ok" : "unavailable");
        return ok;
    }
    bool orientation(int quarter_turn) {
        auto instance = symbol<void *(*)()>("_ZN4OHOS5Rosen13ScreenManager11GetInstanceEv");
        auto get = symbol<ScreenRef(*)(void *, uint64_t)>("_ZN4OHOS5Rosen13ScreenManager13GetScreenByIdEm");
        auto set = symbol<int32_t(*)(void *, uint32_t)>("_ZNK4OHOS5Rosen6Screen20SetScreenOrientationENS0_11OrientationE");
        if (!instance || !get || !set) { return false; }
        auto screen = get(instance(), 0);
        if (!screen.pointer) { return false; }
        int result = set(screen.pointer, quarter_turn < 0 ? 0 : (quarter_turn & 3) + 1);
        fprintf(stderr, "[harmony] display orientation %d: %d\n", quarter_turn < 0 ? -1 : (quarter_turn & 3) * 90, result);
        return result == 0;
    }
    bool rotate() {
        if (!changed_rotation) {
            auto instance = symbol<void *(*)()>("_ZN4OHOS5Rosen13ScreenManager11GetInstanceEv");
            auto get_lock = symbol<int32_t(*)(void *, bool &)>("_ZN4OHOS5Rosen13ScreenManager22IsScreenRotationLockedERb");
            auto get = symbol<ScreenRef(*)(void *, uint64_t)>("_ZN4OHOS5Rosen13ScreenManager13GetScreenByIdEm");
            auto get_orientation = symbol<uint32_t(*)(void *)>("_ZNK4OHOS5Rosen6Screen14GetOrientationEv");
            if (!instance || !get_lock || !get || !get_orientation || get_lock(instance(), original_rotation_lock)) { return false; }
            auto screen = get(instance(), 0);
            if (!screen.pointer) { return false; }
            original_orientation = get_orientation(screen.pointer);
            if (original_orientation > 4 || !rotation_lock(true)) { return false; }
            changed_rotation = true;
        }
        NativeDisplayManager_Rotation rotation{};
        if (OH_NativeDisplayManager_GetDefaultDisplayRotation(&rotation)) { return false; }
        return orientation((int(rotation) + 1) & 3);
    }
    bool rotation_lock(bool locked) {
        auto instance = symbol<void *(*)()>("_ZN4OHOS5Rosen13ScreenManager11GetInstanceEv");
        auto set = symbol<int32_t(*)(void *, bool)>("_ZN4OHOS5Rosen13ScreenManager29SetScreenRotationLockedFromJsEb");
        return instance && set && set(instance(), locked) == 0;
    }
    bool off() const { return changed_power; }
    uint64_t create_virtual(uint32_t width, uint32_t height) {
        auto instance = symbol<void *(*)()>("_ZN4OHOS5Rosen13ScreenManager11GetInstanceEv");
        auto create = symbol<uint64_t(*)(void *, VirtualOption)>(
            "_ZN4OHOS5Rosen13ScreenManager19CreateVirtualScreenENS0_19VirtualScreenOptionE");
        if (!instance || !create || !NativeString::available() || owned_virtual != UINT64_MAX) { return UINT64_MAX; }
        static_assert(sizeof(std::string) == 24 && sizeof(std::vector<uint64_t>) == 24,
                      "unsupported platform C++ value ABI");
        static_assert(offsetof(VirtualOption, requested_screen_id) == 156 && sizeof(VirtualOption) == 176,
                      "unsupported VirtualScreenOption ABI");
        VirtualOption option; option.width = width; option.height = height; option.user_id = foreground_user();
        owned_virtual = create(instance(), option);
        fprintf(stderr, "[harmony] virtual display create: %llu\n", (unsigned long long) owned_virtual);
        if (owned_virtual == UINT64_MAX) { return owned_virtual; }
        std::vector<uint64_t> screens{owned_virtual};
        auto unique = symbol<int32_t(*)(void *, const std::vector<uint64_t> &)>(
            "_ZN4OHOS5Rosen13ScreenManager16MakeUniqueScreenERKNSt3__h6vectorImNS2_9allocatorImEEEE");
        int result = -1;
        if (unique) { result = unique(instance(), screens); }
        else {
            auto unique_rotation = symbol<int32_t(*)(void *, const std::vector<uint64_t> &, const UniqueRotation &)>(
                "_ZN4OHOS5Rosen13ScreenManager16MakeUniqueScreenERKNSt3__h6vectorImNS2_9allocatorImEEEERKNS0_27UniqueScreenRotationOptionsE");
            if (unique_rotation) { UniqueRotation rotation; result = unique_rotation(instance(), screens, rotation); }
        }
        fprintf(stderr, "[harmony] virtual display unique mode: %d\n", result);
        if (result) { destroy_virtual(); }
        return owned_virtual;
    }
    uint64_t create_record_screen(uint32_t width, uint32_t height) {
        auto instance = symbol<void *(*)()>("_ZN4OHOS5Rosen13ScreenManager11GetInstanceEv");
        auto create = symbol<uint64_t(*)(void *, VirtualOption)>(
            "_ZN4OHOS5Rosen13ScreenManager19CreateVirtualScreenENS0_19VirtualScreenOptionE");
        if (!instance || !create || !NativeString::available() || owned_virtual != UINT64_MAX) { return UINT64_MAX; }
        VirtualOption option("screen_capture");
        option.width = width; option.height = height; option.user_id = foreground_user();
        option.type = 2; // SCREEN_RECORDING
        option.for_shot = true;
        // Retain the normal recording-screen security setting. Disabling it
        // also bypasses the compositor path that applies cursor exclusions.
        option.supports_focus = false;
        option.supports_input = false;
        owned_virtual = create(instance(), option);
        return owned_virtual;
    }
    bool mirror_primary() {
        auto instance = symbol<void *(*)()>("_ZN4OHOS5Rosen13ScreenManager11GetInstanceEv");
        auto mirror = symbol<int32_t(*)(void *, uint64_t, std::vector<uint64_t>, uint64_t &)>(
            "_ZN4OHOS5Rosen13ScreenManager10MakeMirrorEmNSt3__h6vectorImNS2_9allocatorImEEEERm");
        if (!instance || !mirror || owned_virtual == UINT64_MAX) { return false; }
        std::vector<uint64_t> targets{owned_virtual};
        int result = mirror(instance(), 0, std::move(targets), mirrored_group);
        fprintf(stderr, "[harmony] direct display mirror: %d, group=%llu\n", result,
                (unsigned long long)mirrored_group);
        return result == 0;
    }
    bool hide_mirror_cursor() {
        auto instance = symbol<void *(*)()>("_ZN4OHOS5Rosen14DisplayManager11GetInstanceEv");
        auto convert = symbol<bool(*)(void *, uint64_t, uint64_t &)>(
            "_ZN4OHOS5Rosen14DisplayManager27ConvertScreenIdToRsScreenIdEmRm");
        static void *render = dlopen("librender_service_client.z.so", RTLD_NOW | RTLD_LOCAL);
        auto render_instance = render ? reinterpret_cast<void *(*)()>(dlsym(render,
            "_ZN4OHOS5Rosen12RSInterfaces11GetInstanceEv")) : nullptr;
        auto filter = render ? reinterpret_cast<int32_t(*)(void *, uint64_t, std::vector<uint8_t> &)>(dlsym(render,
            "_ZN4OHOS5Rosen12RSInterfaces29SetVirtualScreenTypeBlackListEmRNSt3__h6vectorIhNS2_9allocatorIhEEEE")) : nullptr;
        uint64_t render_screen = UINT64_MAX;
        if (!instance || !convert || !render_instance || !filter || owned_virtual == UINT64_MAX
                || !convert(instance(), owned_virtual, render_screen)) { return false; }
        // Match AVScreenCapture::ShowCursorInner: exclude CURSOR_NODE (11 in
        // the validated HarmonyOS firmware ABI; upstream enum values differ)
        // from this virtual screen only. Leave notifications and other system
        // windows visible, and keep the physical display's cursor unchanged.
        std::vector<uint8_t> types{11};
        int32_t result = filter(render_instance(), render_screen, types);
        fprintf(stderr, "[harmony] mirror cursor exclusion: %d\n", result);
        return result == 0;
    }
    bool stop_mirror() {
        if (mirrored_group == UINT64_MAX || owned_virtual == UINT64_MAX) { return true; }
        auto instance = symbol<void *(*)()>("_ZN4OHOS5Rosen13ScreenManager11GetInstanceEv");
        auto stop = symbol<int32_t(*)(void *, const std::vector<uint64_t> &)>(
            "_ZN4OHOS5Rosen13ScreenManager10StopMirrorERKNSt3__h6vectorImNS2_9allocatorImEEEE");
        std::vector<uint64_t> targets{owned_virtual};
        int result = instance && stop ? stop(instance(), targets) : -1;
        mirrored_group = UINT64_MAX;
        return result == 0;
    }
    uint64_t virtual_screen_id() const { return owned_virtual; }
    void destroy_record_screen() {
        stop_mirror();
        destroy_virtual();
    }
    bool resize_virtual(uint32_t width, uint32_t height) {
        auto instance = symbol<void *(*)()>("_ZN4OHOS5Rosen13ScreenManager11GetInstanceEv");
        auto resize = symbol<int32_t(*)(void *, uint64_t, uint32_t, uint32_t)>(
            "_ZN4OHOS5Rosen13ScreenManager19ResizeVirtualScreenEmjj");
        return instance && resize && owned_virtual != UINT64_MAX && resize(instance(), owned_virtual, width, height) == 0;
    }
    bool surface(OHNativeWindow *window) {
        auto instance = symbol<void *(*)()>("_ZN4OHOS5Rosen13ScreenManager11GetInstanceEv");
        auto set = symbol<int32_t(*)(void *, uint64_t, NativeStrong)>(
            "_ZN4OHOS5Rosen13ScreenManager23SetVirtualScreenSurfaceEmNS_4sptrINS_7SurfaceEEE");
        if (!instance || !set || owned_virtual == UINT64_MAX) { return false; }
        if (!window) { return set(instance(), owned_virtual, NativeStrong{}) == 0; }
        static void *lib = dlopen("libsurface.z.so", RTLD_NOW | RTLD_LOCAL);
        auto utilities = lib ? reinterpret_cast<void *(*)()>(dlsym(lib, "_ZN4OHOS12SurfaceUtils11GetInstanceEv")) : nullptr;
        auto get = lib ? reinterpret_cast<NativeStrong(*)(void *, uint64_t)>(dlsym(lib, "_ZN4OHOS12SurfaceUtils10GetSurfaceEm")) : nullptr;
        uint64_t id;
        if (!utilities || !get || OH_NativeWindow_GetSurfaceId(window, &id)) { return false; }
        auto native = get(utilities(), id);
        if (!native.pointer) { return false; }
        int result = set(instance(), owned_virtual, native);
        fprintf(stderr, "[harmony] virtual display encoder surface: %d\n", result);
        return result == 0;
    }
    void destroy_virtual() {
        if (owned_virtual == UINT64_MAX) { return; }
        auto instance = symbol<void *(*)()>("_ZN4OHOS5Rosen13ScreenManager11GetInstanceEv");
        auto destroy = symbol<int32_t(*)(void *, uint64_t, bool)>(
            "_ZN4OHOS5Rosen13ScreenManager20DestroyVirtualScreenEmb");
        if (instance && destroy) { destroy(instance(), owned_virtual, false); }
        owned_virtual = UINT64_MAX;
    }
};
}
#endif
