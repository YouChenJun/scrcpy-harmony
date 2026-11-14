/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SCRCPY_HARMONY_CAMERA_H
#define SCRCPY_HARMONY_CAMERA_H
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>
#include <ohcamera/camera.h>
#include <ohcamera/camera_manager.h>
#include <ohcamera/camera_input.h>
#include <ohcamera/capture_session.h>
#include <ohcamera/video_output.h>
#include <native_window/external_window.h>
namespace harmony {
class Camera {
    Camera_Manager *manager = nullptr;
    Camera_Input *input = nullptr;
    Camera_CaptureSession *session = nullptr;
    Camera_VideoOutput *output = nullptr;
    Camera_VideoProfile profile{};
    Camera_Device device{};
    std::string device_id;
    std::mutex mutex;
    std::atomic<bool> failed{false};
    bool torch_on = false;
    bool wanted_torch = false;
    float wanted_zoom = 0;
    inline static Camera *active = nullptr;
    static void frame_start(Camera_VideoOutput *) {}
    static void frame_end(Camera_VideoOutput *, int32_t) {}
    static void error(Camera_VideoOutput *, Camera_ErrorCode code) {
        fprintf(stderr, "[harmony] camera output error: %d\n", code);
        if (active) { active->failed = true; }
    }
public:
    int width = 0, height = 0, fps = 30;
    ~Camera() {
        stop();
        if (input) { OH_CameraInput_Close(input); OH_CameraInput_Release(input); }
        if (manager) { OH_Camera_DeleteCameraManager(manager); }
        if (active == this) { active = nullptr; }
    }
    bool open(std::string id, bool front, int wanted_width, int wanted_height, int wanted_fps, int max_size = 0) {
        int result = OH_Camera_GetCameraManager(&manager);
        if (result || !manager) { fprintf(stderr, "[harmony] camera manager: %d\n", result); return false; }
        Camera_Device *devices = nullptr; uint32_t size = 0;
        result = OH_CameraManager_GetSupportedCameras(manager, &devices, &size);
        if (result || !size) { fprintf(stderr, "[harmony] no cameras: %d\n", result); return false; }
        Camera_Device *chosen = nullptr;
        for (uint32_t i = 0; i < size; ++i) {
            if ((!id.empty() && (id == devices[i].cameraId || "device/" + id == devices[i].cameraId))
                    || (id.empty() && devices[i].cameraPosition == (front ? CAMERA_POSITION_FRONT : CAMERA_POSITION_BACK)
                    && devices[i].cameraType == CAMERA_TYPE_DEFAULT)) { chosen = &devices[i]; break; }
        }
        Camera_OutputCapability *capability = nullptr;
        if (chosen) { result = OH_CameraManager_GetSupportedCameraOutputCapabilityWithSceneMode(manager, chosen, NORMAL_VIDEO, &capability); }
        else { result = CAMERA_INVALID_ARGUMENT; }
        bool found = false;
        if (!result && capability) {
            double best = std::numeric_limits<double>::max();
            int goal_width = wanted_width ? wanted_width : 1920, goal_height = wanted_height ? wanted_height : 1080;
            int goal_fps = wanted_fps ? wanted_fps : 30;
            for (uint32_t i = 0; i < capability->videoProfilesSize; ++i) {
                auto p = capability->videoProfiles[i];
                if (!p || p->format != CAMERA_FORMAT_YUV_420_SP || goal_fps < int(p->range.min) || goal_fps > int(p->range.max)) { continue; }
                if (max_size && std::max(p->size.width, p->size.height) > uint32_t(max_size)) { continue; }
                if (wanted_width && (p->size.width != uint32_t(wanted_width) || p->size.height != uint32_t(wanted_height))) { continue; }
                double score = std::abs(double(p->size.width) * p->size.height - double(goal_width) * goal_height);
                if (score < best) { best = score; profile = *p; found = true; fps = goal_fps; }
            }
        }
        if (found) {
            device = *chosen; device_id = chosen->cameraId;
            device.cameraId = const_cast<char *>(device_id.c_str());
        }
        if (capability) { OH_CameraManager_DeleteSupportedCameraOutputCapability(manager, capability); }
        OH_CameraManager_DeleteSupportedCameras(manager, devices, size);
        if (!found || result) { fprintf(stderr, "[harmony] camera input/profile unavailable: %d\n", result); return false; }
        width = profile.size.width; height = profile.size.height; active = this;
        fprintf(stderr, "[harmony] camera ready: %dx%d @ %d fps\n", width, height, fps);
        return true;
    }
    bool start(OHNativeWindow *surface) {
        std::lock_guard<std::mutex> lock(mutex);
        failed = false;
        uint64_t surface_id;
        if (OH_NativeWindow_GetSurfaceId(surface, &surface_id)) { return false; }
        int result = OH_CameraManager_CreateCameraInput(manager, &device, &input);
        if (!result) { result = OH_CameraInput_Open(input); }
        if (!result) { result = OH_CameraManager_CreateCaptureSession(manager, &session); }
        if (!result) { result = OH_CaptureSession_SetSessionMode(session, NORMAL_VIDEO); }
        if (!result) { result = OH_CaptureSession_BeginConfig(session); }
        if (!result) { result = OH_CaptureSession_AddInput(session, input); }
        std::string id = std::to_string(surface_id);
        if (!result) { result = OH_CameraManager_CreateVideoOutput(manager, &profile, id.c_str(), &output); }
        if (!result) { result = OH_CaptureSession_AddVideoOutput(session, output); }
        if (!result) { result = OH_CaptureSession_CommitConfig(session); }
        if (!result) {
            if (wanted_zoom) { OH_CaptureSession_SetZoomRatio(session, wanted_zoom); }
            if (wanted_torch) {
                torch_on = OH_CaptureSession_SetFlashMode(session, FLASH_MODE_ALWAYS_OPEN) == 0;
            }
            VideoOutput_Callbacks callbacks{frame_start, frame_end, error};
            OH_VideoOutput_RegisterCallback(output, &callbacks);
            OH_VideoOutput_SetFrameRate(output, fps, fps);
            result = OH_CaptureSession_Start(session);
        }
        if (!result) { result = OH_VideoOutput_Start(output); }
        fprintf(stderr, "[harmony] camera surface start: %d\n", result);
        return result == 0;
    }
    bool torch(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!session) { return false; }
        int result = OH_CaptureSession_SetFlashMode(session, enabled ? FLASH_MODE_ALWAYS_OPEN : FLASH_MODE_CLOSE);
        if (!result) { torch_on = wanted_torch = enabled; }
        fprintf(stderr, "[harmony] camera torch %s: %d\n", enabled ? "on" : "off", result);
        return result == 0;
    }
    bool zoom(bool increase) {
        std::lock_guard<std::mutex> lock(mutex);
        float minimum = 1, maximum = 1, current = 1;
        if (!session || OH_CaptureSession_GetZoomRatioRange(session, &minimum, &maximum)
                || OH_CaptureSession_GetZoomRatio(session, &current)) { return false; }
        float wanted = std::clamp(increase ? current * 1.1f : current / 1.1f, minimum, maximum);
        int result = OH_CaptureSession_SetZoomRatio(session, wanted);
        if (!result) { wanted_zoom = wanted; }
        fprintf(stderr, "[harmony] camera zoom %.2f: %d\n", wanted, result);
        return result == 0;
    }
    bool healthy() const { return !failed; }
    void stop() {
        std::lock_guard<std::mutex> lock(mutex);
        if (torch_on && session) { OH_CaptureSession_SetFlashMode(session, FLASH_MODE_CLOSE); torch_on = false; }
        if (output) { OH_VideoOutput_Stop(output); }
        if (session) { OH_CaptureSession_Stop(session); }
        if (output) { OH_VideoOutput_Release(output); output = nullptr; }
        if (session) { OH_CaptureSession_Release(session); session = nullptr; }
        if (input) { OH_CameraInput_Close(input); OH_CameraInput_Release(input); input = nullptr; }
    }
};
}
#endif
