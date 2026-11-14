/* SPDX-License-Identifier: Apache-2.0 */
// On-device integration-test fixture. Not installed by the normal launcher.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <window_manager/oh_display_manager.h>
#include <database/pasteboard/oh_pasteboard.h>
#include <database/udmf/udmf.h>
#include "clipboard.h"
#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audio_volume_manager.h>
static int32_t tone(OH_AudioRenderer *, void *context, void *buffer, int32_t size) {
    auto &frame = *static_cast<uint64_t *>(context);
    auto out = static_cast<int16_t *>(buffer);
    for (int i = 0; i < size / 4; ++i, ++frame) {
        int16_t value = int16_t(1800 * sin(2 * 3.141592653589793 * 440 * frame / 48000));
        out[2 * i] = value; out[2 * i + 1] = value;
    }
    return 0;
}
int main(int argc, char **argv) {
    if (argc < 2) { return 2; }
    if (!strcmp(argv[1], "display-size")) {
        NativeDisplayManager_DisplayInfo *info = nullptr;
        if (OH_NativeDisplayManager_CreatePrimaryDisplay(&info) || !info) { return 1; }
        printf("%u %u\n", info->width, info->height);
        OH_NativeDisplayManager_DestroyDisplay(info);
        return 0;
    }
    if (!strcmp(argv[1], "tone")) {
        if (argc > 3 && !strcmp(argv[3], "shell")) { setgid(2000); setuid(2000); }
        OH_AudioStreamBuilder *builder = nullptr;
        OH_AudioRenderer *renderer = nullptr;
        uint64_t frame = 0;
        OH_AudioVolumeManager *volume = nullptr;
        int32_t level = -1;
        OH_AudioManager_GetAudioVolumeManager(&volume);
        if (volume) { OH_AudioVolumeManager_GetVolumeByUsage(volume, AUDIOSTREAM_USAGE_MUSIC, &level); }
        fprintf(stderr, "media volume level: %d\n", level);
        OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER);
        if (!builder) { return 1; }
        OH_AudioStreamBuilder_SetSamplingRate(builder, 48000);
        OH_AudioStreamBuilder_SetChannelCount(builder, 2);
        OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE);
        OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
        OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_MUSIC);
        OH_AudioStreamBuilder_SetRendererPrivacy(builder, AUDIO_STREAM_PRIVACY_TYPE_PUBLIC);
        OH_AudioRenderer_Callbacks callbacks{}; callbacks.OH_AudioRenderer_OnWriteData = tone;
        OH_AudioStreamBuilder_SetRendererCallback(builder, callbacks, &frame);
        int result = OH_AudioStreamBuilder_GenerateRenderer(builder, &renderer);
        OH_AudioStreamBuilder_Destroy(builder);
        if (result || !renderer || OH_AudioRenderer_Start(renderer)) { return 1; }
        sleep(argc > 2 ? atoi(argv[2]) : 3);
        OH_AudioRenderer_Stop(renderer); OH_AudioRenderer_Release(renderer);
        fprintf(stderr, "tone frames: %llu\n", (unsigned long long) frame);
        return frame ? 0 : 1;
    }
    if (!strcmp(argv[1], "clipboard-guard") && argc == 3) {
        std::string root = argv[2];
        auto board = OH_Pasteboard_Create();
        bool had = OH_Pasteboard_HasData(board); int status;
        auto saved = OH_Pasteboard_GetData(board, &status);
        if (had && !saved) { OH_Pasteboard_Destroy(board); return 1; }
        harmony::Clipboard clipboard;
        std::ofstream(root + "/guard.ready") << "ready\n";
        auto deadline = harmony::now_us() + 120000000;
        while (harmony::now_us() < deadline && access((root + "/guard.done").c_str(), F_OK)) {
            std::ifstream in(root + "/guard.set");
            if (in) {
                std::string text((std::istreambuf_iterator<char>(in)), {});
                if (!clipboard.set(text)) { break; }
                unlink((root + "/guard.set").c_str());
            }
            usleep(50000);
        }
        int result = saved ? OH_Pasteboard_SetData(board, saved) : OH_Pasteboard_ClearData(board);
        if (saved) { OH_UdmfData_Destroy(saved); }
        OH_Pasteboard_Destroy(board);
        return result ? 1 : 0;
    }
    return 2;
}
