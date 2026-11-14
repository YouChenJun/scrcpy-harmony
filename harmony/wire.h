/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SCRCPY_HARMONY_WIRE_H
#define SCRCPY_HARMONY_WIRE_H
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <time.h>
namespace harmony {
inline int64_t now_us() {
    timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1000000 + t.tv_nsec / 1000;
}
inline void put32(uint8_t *p, uint32_t v) { for (int i = 3; i >= 0; --i) { p[i] = v; v >>= 8; } }
inline void put64(uint8_t *p, uint64_t v) { put32(p, v >> 32); put32(p + 4, v); }
inline bool send_all(int fd, const void *buffer, size_t size) {
    auto p = static_cast<const uint8_t *>(buffer);
    while (size) {
        auto n = send(fd, p, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) { continue; }
        if (n <= 0) { return false; }
        p += n; size -= n;
    }
    return true;
}
inline bool packet(int fd, uint64_t pts, const void *bytes, size_t length) {
    uint8_t header[12]; put64(header, pts); put32(header + 8, length);
    return send_all(fd, header, 12) && send_all(fd, bytes, length);
}
class ControlWriter {
    int fd;
    std::mutex mutex;
public:
    explicit ControlWriter(int socket) : fd(socket) {}
    void set_socket(int socket) { std::lock_guard<std::mutex> lock(mutex); fd = socket; }
    bool clipboard(const std::string &text) {
        size_t n = std::min<size_t>(text.size(), 256 * 1024 - 5);
        // Do not split a UTF-8 code point at the protocol size limit.
        while (n < text.size() && n && (uint8_t(text[n]) & 0xc0) == 0x80) { --n; }
        uint8_t header[5]{}; put32(header + 1, n);
        std::lock_guard<std::mutex> lock(mutex);
        return send_all(fd, header, 5) && send_all(fd, text.data(), n);
    }
    bool ack(uint64_t sequence) {
        if (!sequence) { return true; }
        uint8_t bytes[9]; bytes[0] = 1; put64(bytes + 1, sequence);
        std::lock_guard<std::mutex> lock(mutex);
        return send_all(fd, bytes, 9);
    }
    bool hid(uint16_t id, const void *bytes, uint16_t size) {
        uint8_t header[5] = {2, uint8_t(id >> 8), uint8_t(id), uint8_t(size >> 8), uint8_t(size)};
        std::lock_guard<std::mutex> lock(mutex);
        return send_all(fd, header, 5) && send_all(fd, bytes, size);
    }
};
}
#endif
