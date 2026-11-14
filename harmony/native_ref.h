/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SCRCPY_HARMONY_NATIVE_REF_H
#define SCRCPY_HARMONY_NATIVE_REF_H
#include <dlfcn.h>
namespace harmony {
struct NativeShared {
        void *object = nullptr, *counter = nullptr;
        static void change(void *counter, bool retain) {
            if (!counter) { return; }
            static void *lib = dlopen("libc++.so", RTLD_NOW | RTLD_LOCAL);
            auto function = lib ? reinterpret_cast<void(*)(void *)>(dlsym(lib, retain
                ? "_ZNSt3__h14__shared_count12__add_sharedEv"
                : "_ZNSt3__h19__shared_weak_count16__release_sharedEv")) : nullptr;
            if (function) { function(counter); }
        }
        NativeShared() = default;
        NativeShared(const NativeShared &other) : object(other.object), counter(other.counter) { change(counter, true); }
        NativeShared(NativeShared &&other) : object(other.object), counter(other.counter) { other.object = other.counter = nullptr; }
        ~NativeShared() { change(counter, false); }
    };
struct NativeStrong {
    void *pointer = nullptr;
    static void change(void *object, const void *owner, bool retain) {
        if (!object) { return; }
        static void *lib = dlopen("libutils.z.so", RTLD_NOW | RTLD_LOCAL);
        auto function = lib ? reinterpret_cast<void(*)(void *, const void *)>(dlsym(lib, retain
            ? "_ZN4OHOS7RefBase12IncStrongRefEPKv" : "_ZN4OHOS7RefBase12DecStrongRefEPKv")) : nullptr;
        if (function) { function(object, owner); }
    }
    NativeStrong() = default;
    NativeStrong(const NativeStrong &other) : pointer(other.pointer) { change(pointer, this, true); }
    NativeStrong(NativeStrong &&other) : pointer(other.pointer) { other.pointer = nullptr; }
    ~NativeStrong() { change(pointer, this, false); }
};
}
#endif
