/* SPDX-License-Identifier: Apache-2.0 */
#include <napi/native_api.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
extern int scrcpy_harmony_server_main(int, char **);
extern "C" void scrcpy_harmony_request_stop();
extern "C" void (*scrcpy_harmony_launch_hook)(const char *, uint32_t);
extern "C" void (*scrcpy_harmony_started_hook)();
namespace {
std::atomic<bool> running{false};
std::thread worker;
napi_threadsafe_function launcher = nullptr;
napi_threadsafe_function end_callback = nullptr;
napi_threadsafe_function ready_callback = nullptr;
void ReadyOnJs(napi_env env, napi_value callback, void *, void *) {
    if (env && callback) {
        napi_value global, result; napi_get_global(env, &global);
        napi_call_function(env, global, callback, 0, nullptr, &result);
    }
}
void NotifyReady() {
    if (ready_callback) { napi_call_threadsafe_function(ready_callback, nullptr, napi_tsfn_nonblocking); }
}
napi_value SetReadyCallback(napi_env env, napi_callback_info info) {
    size_t count = 1; napi_value args[1], resource;
    napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
    if (count != 1 || ready_callback) { return nullptr; }
    napi_create_string_utf8(env, "scrcpy-ready", NAPI_AUTO_LENGTH, &resource);
    napi_create_threadsafe_function(env, args[0], nullptr, resource, 1, 1, nullptr, nullptr, nullptr, ReadyOnJs, &ready_callback);
    scrcpy_harmony_started_hook = NotifyReady;
    return nullptr;
}
struct Launch { std::string name; uint32_t display; };
void LaunchOnJs(napi_env env, napi_value callback, void *, void *pointer) {
    auto data = static_cast<Launch *>(pointer);
    if (env && callback) {
        napi_value args[2], result;
        napi_create_string_utf8(env, data->name.c_str(), data->name.size(), &args[0]);
        napi_create_uint32(env, data->display, &args[1]);
        napi_value global; napi_get_global(env, &global);
        napi_call_function(env, global, callback, 2, args, &result);
    }
    delete data;
}
void RequestLaunch(const char *name, uint32_t display) {
    if (!launcher) { return; }
    auto request = new Launch{name, display};
    if (napi_call_threadsafe_function(launcher, request, napi_tsfn_nonblocking) != napi_ok) { delete request; }
}
void EndOnJs(napi_env env, napi_value callback, void *, void *pointer) {
    auto code = static_cast<int *>(pointer);
    if (env && callback) {
        napi_value argument, result, global; napi_create_int32(env, *code, &argument); napi_get_global(env, &global);
        napi_call_function(env, global, callback, 1, &argument, &result);
    }
    delete code;
}
napi_value SetEndCallback(napi_env env, napi_callback_info info) {
    size_t count = 1; napi_value args[1], resource;
    napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
    if (count != 1 || end_callback) { return nullptr; }
    napi_create_string_utf8(env, "scrcpy-ended", NAPI_AUTO_LENGTH, &resource);
    napi_create_threadsafe_function(env, args[0], nullptr, resource, 4, 1, nullptr, nullptr, nullptr, EndOnJs, &end_callback);
    return nullptr;
}
napi_value SetLauncher(napi_env env, napi_callback_info info) {
    size_t count = 1; napi_value args[1], resource;
    napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
    if (count != 1 || launcher) { return nullptr; }
    napi_create_string_utf8(env, "scrcpy-launch", NAPI_AUTO_LENGTH, &resource);
    napi_create_threadsafe_function(env, args[0], nullptr, resource, 32, 1, nullptr, nullptr, nullptr, LaunchOnJs, &launcher);
    scrcpy_harmony_launch_hook = RequestLaunch;
    return nullptr;
}
napi_value Start(napi_env env, napi_callback_info info) {
    size_t count = 1; napi_value args[1], result;
    napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
    if (count != 1 || running) { napi_get_boolean(env, false, &result); return result; }
    uint32_t size = 0; napi_get_array_length(env, args[0], &size);
    if (size < 5 || size > 32) { napi_get_boolean(env, false, &result); return result; }
    std::vector<std::string> values{"scrcpy-harmony-agent"};
    for (uint32_t i = 0; i < size; ++i) {
        napi_value element; napi_get_element(env, args[0], i, &element);
        size_t length = 0; napi_get_value_string_utf8(env, element, nullptr, 0, &length);
        if (length > 4096) { napi_get_boolean(env, false, &result); return result; }
        std::vector<char> buffer(length + 1);
        napi_get_value_string_utf8(env, element, buffer.data(), buffer.size(), &length);
        values.emplace_back(buffer.data(), length);
    }
    if (worker.joinable()) { worker.join(); }
    running = true;
    worker = std::thread([values = std::move(values)]() mutable {
        std::vector<char *> argv;
        for (auto &value : values) { argv.push_back(value.data()); }
        argv.push_back(nullptr);
        int code = scrcpy_harmony_server_main(int(values.size()), argv.data());
        fprintf(stderr, "[harmony-agent] media stopped: %d\n", code);
        running = false;
        if (end_callback) {
            auto result = new int(code);
            if (napi_call_threadsafe_function(end_callback, result, napi_tsfn_nonblocking) != napi_ok) { delete result; }
        }
    });
    napi_get_boolean(env, true, &result); return result;
}
napi_value IsRunning(napi_env env, napi_callback_info) {
    napi_value value; napi_get_boolean(env, running, &value); return value;
}
napi_value Stop(napi_env env, napi_callback_info) {
    scrcpy_harmony_request_stop(); napi_value value; napi_get_undefined(env, &value); return value;
}
napi_value SetLogFile(napi_env env, napi_callback_info info) {
    size_t count = 1; napi_value args[1]; napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
    if (count != 1) { return nullptr; }
    char path[4096]; size_t length = 0;
    if (napi_get_value_string_utf8(env, args[0], path, sizeof(path), &length) != napi_ok || !length) { return nullptr; }
    auto file = freopen(path, "w", stderr);
    if (file) { setvbuf(stderr, nullptr, _IOLBF, 0); }
    return nullptr;
}
void Cleanup(void *) {
    scrcpy_harmony_request_stop();
    if (worker.joinable()) { worker.join(); }
    if (launcher) { napi_release_threadsafe_function(launcher, napi_tsfn_abort); launcher = nullptr; }
    if (end_callback) { napi_release_threadsafe_function(end_callback, napi_tsfn_abort); end_callback = nullptr; }
    if (ready_callback) { napi_release_threadsafe_function(ready_callback, napi_tsfn_abort); ready_callback = nullptr; }
}
napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor methods[] = {
        {"start", nullptr, Start, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stop", nullptr, Stop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setLauncher", nullptr, SetLauncher, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setLogFile", nullptr, SetLogFile, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setEndCallback", nullptr, SetEndCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setReadyCallback", nullptr, SetReadyCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isRunning", nullptr, IsRunning, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(methods) / sizeof(methods[0]), methods);
    napi_add_env_cleanup_hook(env, Cleanup, nullptr);
    return exports;
}
napi_module module = {1, 0, nullptr, Init, "scrcpy_agent", nullptr, {0}};
}
extern "C" __attribute__((constructor)) void Register() { napi_module_register(&module); }
