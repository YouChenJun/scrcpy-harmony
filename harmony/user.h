/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SCRCPY_HARMONY_USER_H
#define SCRCPY_HARMONY_USER_H
#include <dlfcn.h>
namespace harmony {
inline int foreground_user() {
    static void *library = dlopen("libos_account_innerkits.z.so", RTLD_NOW | RTLD_LOCAL);
    auto get = library ? reinterpret_cast<int(*)(int &)>(dlsym(library,
        "_ZN4OHOS9AccountSA16OsAccountManager29GetForegroundOsAccountLocalIdERi")) : nullptr;
    int user = -1;
    return get && get(user) == 0 ? user : -1;
}
}
#endif
