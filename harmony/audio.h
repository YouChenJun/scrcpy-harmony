/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SCRCPY_HARMONY_AUDIO_H
#define SCRCPY_HARMONY_AUDIO_H
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <multimedia/player_framework/native_avcodec_audiocodec.h>
#include <multimedia/player_framework/native_avformat.h>
#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiocapturer.h>
#include "wire.h"
namespace harmony {
class Audio {
    struct Input { uint32_t index; OH_AVBuffer *buffer; };
    struct Chunk { std::vector<uint8_t> bytes; int64_t pts; };
    int fd;
    OH_AVCodec *codec = nullptr;
    bool raw = false;
    std::atomic<bool> alive{false};
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<Input> inputs;
    std::deque<Chunk> chunks;
    std::thread feeder;
    std::atomic<int64_t> origin{0};
    int64_t encoded_offset = 0, last_encoded_pts = -1;
    bool have_encoded_offset = false;
    uint64_t samples = 0;
    std::atomic<int> peak{0};
    static void error(OH_AVCodec *, int32_t code, void *context) {
        auto &self = *static_cast<Audio *>(context);
        fprintf(stderr, "[harmony] audio encoder error %d\n", code);
        self.alive = false; self.condition.notify_all();
        shutdown(self.fd, SHUT_RDWR);
    }
    static void changed(OH_AVCodec *, OH_AVFormat *, void *) {}
    static void input(OH_AVCodec *, uint32_t index, OH_AVBuffer *buffer, void *context) {
        auto &self = *static_cast<Audio *>(context);
        std::lock_guard<std::mutex> lock(self.mutex);
        self.inputs.push_back({index, buffer}); self.condition.notify_one();
    }
    static void output(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *context) {
        auto &self = *static_cast<Audio *>(context);
        OH_AVCodecBufferAttr attr{};
        if (OH_AVBuffer_GetBufferAttr(buffer, &attr) == 0 && attr.size > 0 && self.alive) {
            auto bytes = OH_AVBuffer_GetAddr(buffer);
            int cap = OH_AVBuffer_GetCapacity(buffer);
            uint64_t pts;
            if (attr.flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA) { pts = uint64_t(1) << 62; }
            else {
                // FLAC starts PTS at zero on this platform; AAC/Opus preserve
                // the input clock. Align relative output to the same monotonic
                // origin as video, before the recorder chooses its time base.
                if (!self.have_encoded_offset) {
                    int64_t input_origin = self.origin.load();
                    self.encoded_offset = attr.pts < input_origin - 1000000 ? input_origin : 0;
                    self.have_encoded_offset = true;
                }
                int64_t value = std::max(self.last_encoded_pts + 1, attr.pts + self.encoded_offset);
                self.last_encoded_pts = value; pts = uint64_t(value);
            }
            if (!bytes || attr.offset < 0 || attr.size > cap - attr.offset
                    || !packet(self.fd, pts, bytes + attr.offset, attr.size)) {
                self.alive = false; self.condition.notify_all();
            }
        }
        OH_AudioCodec_FreeOutputBuffer(codec, index);
    }
    void feed() {
        while (alive) {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [&] { return !alive || (!chunks.empty() && (raw || !inputs.empty())); });
            if (!alive) { break; }
            Chunk chunk = std::move(chunks.front()); chunks.pop_front();
            Input in{};
            if (!raw) { in = inputs.front(); inputs.pop_front(); }
            lock.unlock();
            if (raw) {
                if (!packet(fd, chunk.pts, chunk.bytes.data(), chunk.bytes.size())) { alive = false; }
                continue;
            }
            int capacity = OH_AVBuffer_GetCapacity(in.buffer);
            auto address = OH_AVBuffer_GetAddr(in.buffer);
            if (!address || capacity <= 0) { alive = false; break; }
            size_t size = std::min<size_t>(capacity & ~3, chunk.bytes.size());
            memcpy(address, chunk.bytes.data(), size);
            OH_AVCodecBufferAttr attr{chunk.pts, int32_t(size), 0, AVCODEC_BUFFER_FLAGS_NONE};
            OH_AVBuffer_SetBufferAttr(in.buffer, &attr);
            if (size < chunk.bytes.size()) {
                Chunk rest{{chunk.bytes.begin() + size, chunk.bytes.end()}, chunk.pts + int64_t(size / 4) * 1000000 / 48000};
                std::lock_guard<std::mutex> again(mutex); chunks.push_front(std::move(rest));
            }
            if (OH_AudioCodec_PushInputBuffer(codec, in.index)) { alive = false; }
        }
    }
public:
    explicit Audio(int socket) : fd(socket) {}
    ~Audio() { stop(); }
    bool start(const std::string &name, int bitrate) {
        raw = name == "raw";
        const char *mime = name == "opus" ? "audio/opus" : name == "aac" ? "audio/mp4a-latm" : "audio/flac";
        const char *id = name == "opus" ? "opus" : name == "aac" ? "\0aac" : name == "flac" ? "flac" : "\0raw";
        if (!raw) {
            codec = OH_AudioCodec_CreateByMime(mime, true);
            if (!codec) { return false; }
            OH_AVCodecCallback callbacks{error, changed, input, output};
            if (OH_AudioCodec_RegisterCallback(codec, callbacks, this)) { return false; }
            auto format = OH_AVFormat_Create();
            if (!format) { return false; }
            OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE, 48000);
            OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT, 2);
            OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUDIO_SAMPLE_FORMAT, SAMPLE_S16LE);
            OH_AVFormat_SetLongValue(format, OH_MD_KEY_CHANNEL_LAYOUT, CH_LAYOUT_STEREO);
            OH_AVFormat_SetLongValue(format, OH_MD_KEY_BITRATE, bitrate);
            if (name == "flac") {
                OH_AVFormat_SetLongValue(format, OH_MD_KEY_COMPLIANCE_LEVEL, 0);
                OH_AVFormat_SetIntValue(format, OH_MD_KEY_BITS_PER_CODED_SAMPLE, SAMPLE_S16LE);
            }
            int result = OH_AudioCodec_Configure(codec, format);
            OH_AVFormat_Destroy(format);
            if (result || OH_AudioCodec_Prepare(codec)) {
                fprintf(stderr, "[harmony] configure %s audio failed: %d\n", name.c_str(), result); return false;
            }
        }
        alive = true;
        if (codec && OH_AudioCodec_Start(codec)) { alive = false; return false; }
        if (!send_all(fd, id, 4)) { alive = false; return false; }
        // Native OH encoders may omit codec-data output buffers. scrcpy's
        // recorder requires a configuration packet before encoded audio.
        if (name == "opus") {
            const uint8_t head[] = {'O','p','u','s','H','e','a','d',1,2,0,0,0x80,0xbb,0,0,0,0,0};
            if (!packet(fd, uint64_t(1) << 62, head, sizeof(head))) { return false; }
        } else if (name == "aac") {
            const uint8_t asc[] = {0x11, 0x90}; // AAC-LC, 48 kHz, stereo.
            if (!packet(fd, uint64_t(1) << 62, asc, sizeof(asc))) { return false; }
        } else if (name == "flac") {
            uint8_t info[34]{};
            info[0] = info[2] = 0x10; // 4096-sample min/max block size.
            uint64_t format = (uint64_t(48000) << 44) | (uint64_t(1) << 41) | (uint64_t(15) << 36);
            put64(info + 10, format);
            auto description = OH_AudioCodec_GetOutputDescription(codec);
            uint8_t *configuration = nullptr; size_t size = 0;
            if (description && OH_AVFormat_GetBuffer(description, OH_MD_KEY_CODEC_CONFIG, &configuration, &size)
                    && configuration) {
                if (size == sizeof(info)) { memcpy(info, configuration, sizeof(info)); }
                else if (size >= sizeof(info) + 8 && !memcmp(configuration, "fLaC", 4)) {
                    memcpy(info, configuration + 8, sizeof(info));
                }
            }
            if (description) { OH_AVFormat_Destroy(description); }
            if (!packet(fd, uint64_t(1) << 62, info, sizeof(info))) { return false; }
        }
        feeder = std::thread([this] { feed(); });
        fprintf(stderr, "[harmony] %s audio 48000 Hz stereo ready\n", name.c_str());
        return true;
    }
    void push(const uint8_t *bytes, size_t size) {
        if (!alive || !size) { return; }
        std::lock_guard<std::mutex> lock(mutex);
        for (size_t i = 0; i + 1 < size; i += 2) {
            int value = std::abs(int(int16_t(uint16_t(bytes[i]) | uint16_t(bytes[i + 1]) << 8)));
            if (value > peak) { peak = value; }
        }
        if (!origin) { origin = now_us(); }
        // Sample count avoids clock drift between PCM chunks. Video uses the
        // same monotonic clock, so audio and video share a time origin.
        int64_t pts = origin + int64_t(samples) * 1000000 / 48000;
        samples += size / 4;
        if (chunks.size() >= 50) { chunks.pop_front(); } // Bound latency under backpressure.
        chunks.push_back({{bytes, bytes + (size & ~size_t(3))}, pts});
        condition.notify_one();
    }
    void stop() {
        alive = false; condition.notify_all();
        if (feeder.joinable()) { feeder.join(); }
        if (codec) { OH_AudioCodec_Stop(codec); OH_AudioCodec_Destroy(codec); codec = nullptr; }
        if (samples) { fprintf(stderr, "[harmony] audio captured %llu stereo samples, peak %d\n", (unsigned long long) samples, peak.load()); samples = 0; }
    }
};
class AudioCapture {
    OH_AudioCapturer *capturer = nullptr;
    Audio *output = nullptr;
    static int32_t read(OH_AudioCapturer *, void *context, void *buffer, int32_t bytes) {
        auto &self = *static_cast<AudioCapture *>(context);
        if (self.output && buffer && bytes > 0) { self.output->push(static_cast<uint8_t *>(buffer), bytes); }
        return 0;
    }
public:
    ~AudioCapture() { stop(); }
    bool start(Audio &audio, bool microphone) {
        OH_AudioStreamBuilder *builder = nullptr;
        if (OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_CAPTURER) || !builder) { return false; }
        OH_AudioStreamBuilder_SetSamplingRate(builder, 48000);
        OH_AudioStreamBuilder_SetChannelCount(builder, 2);
        OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE);
        OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
        OH_AudioStreamBuilder_SetCapturerInfo(builder,
            microphone ? AUDIOSTREAM_SOURCE_TYPE_MIC : AUDIOSTREAM_SOURCE_TYPE_PLAYBACK_CAPTURE);
        OH_AudioCapturer_Callbacks callbacks{}; callbacks.OH_AudioCapturer_OnReadData = read;
        OH_AudioStreamBuilder_SetCapturerCallback(builder, callbacks, this);
        int result = OH_AudioStreamBuilder_GenerateCapturer(builder, &capturer);
        fprintf(stderr, "[harmony] audio capturer generation: %d\n", result);
        OH_AudioStreamBuilder_Destroy(builder);
        output = &audio;
        if (!result && capturer) { result = OH_AudioCapturer_Start(capturer); }
        fprintf(stderr, "[harmony] %s capturer: %d\n", microphone ? "microphone" : "playback", result);
        if (result || !capturer) { stop(); return false; }
        return true;
    }
    void stop() {
        if (capturer) { OH_AudioCapturer_Stop(capturer); OH_AudioCapturer_Release(capturer); capturer = nullptr; }
        output = nullptr;
    }
};
}
#endif
