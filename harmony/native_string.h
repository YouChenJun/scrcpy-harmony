/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SCRCPY_HARMONY_NATIVE_STRING_H
#define SCRCPY_HARMONY_NATIVE_STRING_H
#include <array>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <string>
namespace harmony {
// Huawei's inner-kit libraries use their own libc++ ABI namespace. Construct
// their strings with that library, rather than assuming the NDK string ABI.
class NativeString {
    alignas(8) std::array<uint8_t, 24> storage{};
    static void *library() { static void *lib = dlopen("libc++.so", RTLD_NOW | RTLD_LOCAL); return lib; }
    static auto init() {
        return reinterpret_cast<void(*)(void *, const char *, size_t)>(dlsym(library(),
            "_ZNSt3__h12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcm"));
    }
    static auto copy() {
        return reinterpret_cast<void(*)(void *, const void *)>(dlsym(library(),
            "_ZNSt3__h12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_"));
    }
    static auto destroy() {
        return reinterpret_cast<void(*)(void *)>(dlsym(library(),
            "_ZNSt3__h12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev"));
    }
public:
    NativeString() { if (library() && init()) { init()(storage.data(), "", 0); } }
    NativeString(const char *text) { if (library() && init()) { init()(storage.data(), text, strlen(text)); } }
    explicit NativeString(const std::string &text) { if (library() && init()) { init()(storage.data(), text.data(), text.size()); } }
    NativeString(const NativeString &other) { if (library() && copy()) { copy()(storage.data(), other.storage.data()); } }
    ~NativeString() { if (library() && destroy()) { destroy()(storage.data()); } }
    static bool available() { return library() && init() && copy() && destroy(); }
};
static_assert(sizeof(NativeString) == 24, "unsupported native string ABI");
}
#endif
