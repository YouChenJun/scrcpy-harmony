/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SCRCPY_HARMONY_ABILITY_H
#define SCRCPY_HARMONY_ABILITY_H
#include <array>
#include <string>
#include <dlfcn.h>
#include "native_ref.h"
#include "user.h"
#include "native_string.h"
namespace harmony {
// Launch on an independent display using the same AbilityStartSetting API as
// the platform's aa tool. A Want parameter alone does not select the display.
inline int start_on_display(const std::string &bundle, const std::string &ability, uint32_t display) {
    static void *manager = dlopen("libability_manager.z.so", RTLD_NOW | RTLD_LOCAL);
    static void *wants = dlopen("libwant.z.so", RTLD_NOW | RTLD_LOCAL);
    static void *settings = dlopen("libability_start_setting.z.so", RTLD_NOW | RTLD_LOCAL);
    static void *option_library = dlopen("libability_start_options.z.so", RTLD_NOW | RTLD_LOCAL);
    static void *utilities = dlopen("libutils.z.so", RTLD_NOW | RTLD_LOCAL);
    if (!manager || !wants || !settings) { return -1; }
    struct NullToken { void *pointer = nullptr; ~NullToken() {} };
    auto instance = reinterpret_cast<NativeShared(*)()>(dlsym(manager, "_ZN4OHOS5AAFwk20AbilityManagerClient11GetInstanceEv"));
    auto construct = reinterpret_cast<void(*)(void *)>(dlsym(wants, "_ZN4OHOS5AAFwk4WantC1Ev"));
    auto destroy = reinterpret_cast<void(*)(void *)>(dlsym(wants, "_ZN4OHOS5AAFwk4WantD1Ev"));
    auto set_element = reinterpret_cast<void(*)(void *, const void *, const void *)>(dlsym(wants,
        "_ZN4OHOS5AAFwk4Want14SetElementNameERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_"));
    auto setting = reinterpret_cast<NativeShared(*)()>(dlsym(settings, "_ZN4OHOS5AAFwk19AbilityStartSetting15GetEmptySettingEv"));
    auto add = reinterpret_cast<void(*)(void *, const void *, const void *)>(dlsym(settings,
        "_ZN4OHOS5AAFwk19AbilityStartSetting11AddPropertyERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_"));
    auto display_key = dlsym(settings, "_ZN4OHOS5AAFwk19AbilityStartSetting21WINDOW_DISPLAY_ID_KEYE");
    auto start = reinterpret_cast<int(*)(void *, const void *, const void *, NullToken, int, int)>(dlsym(manager,
        "_ZN4OHOS5AAFwk20AbilityManagerClient12StartAbilityERKNS0_4WantERKNS0_19AbilityStartSettingENS_4sptrINS_13IRemoteObjectEEEii"));
    if (!instance || !construct || !destroy || !set_element || !setting || !add || !display_key || !start) { return -1; }
    auto client = instance(); auto options = setting();
    if (!client.object || !options.object) { return -1; }
    NativeString native_display(std::to_string(display)), native_bundle(bundle), native_ability(ability);
    add(options.object, display_key, &native_display);
    alignas(16) std::array<uint8_t, 4096> want{};
    construct(want.data()); set_element(want.data(), &native_bundle, &native_ability);
    int result = -1;
    // Stage-model applications need StartOptions.displayId, rather than the
    // older AbilityStartSetting property used by FA applications.
    auto parcel_create = utilities ? reinterpret_cast<void(*)(void *)>(dlsym(utilities, "_ZN4OHOS6ParcelC1Ev")) : nullptr;
    auto parcel_destroy = utilities ? reinterpret_cast<void(*)(void *)>(dlsym(utilities, "_ZN4OHOS6ParcelD1Ev")) : nullptr;
    auto write_int = utilities ? reinterpret_cast<bool(*)(void *, int)>(dlsym(utilities, "_ZN4OHOS6Parcel10WriteInt32Ei")) : nullptr;
    auto from_parcel = option_library ? reinterpret_cast<void *(*)(void *)>(dlsym(option_library,
        "_ZN4OHOS5AAFwk12StartOptions13UnmarshallingERNS_6ParcelE")) : nullptr;
    auto start_options = reinterpret_cast<int(*)(void *, const void *, const void *, NullToken, int, int)>(dlsym(manager,
        "_ZN4OHOS5AAFwk20AbilityManagerClient12StartAbilityERKNS0_4WantERKNS0_12StartOptionsENS_4sptrINS_13IRemoteObjectEEEii"));
    if (parcel_create && parcel_destroy && write_int && from_parcel && start_options) {
        alignas(16) std::array<uint8_t, 4096> parcel{};
        parcel_create(parcel.data());
        for (int i = 0; i < 64; ++i) { write_int(parcel.data(), i == 1 ? display : (i == 0 || i == 11 ? 1 : 0)); }
        auto settings_object = from_parcel(parcel.data());
        if (settings_object) {
            result = start_options(client.object, want.data(), settings_object, NullToken{}, -1, foreground_user());
            // Parcelable has a virtual destructor. The native object remains
            // entirely owned by its platform implementation.
            struct Parcelable { virtual ~Parcelable() = default; };
            delete reinterpret_cast<Parcelable *>(settings_object);
        }
        parcel_destroy(parcel.data());
    } else { result = start(client.object, want.data(), options.object, NullToken{}, -1, -1); }
    destroy(want.data());
    return result;
}
}
#endif
