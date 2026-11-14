/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SCRCPY_HARMONY_HID_H
#define SCRCPY_HARMONY_HID_H
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/uhid.h>
#include <linux/input.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "wire.h"
namespace harmony {
class Hid {
    std::map<uint16_t, int> devices;
    std::mutex mutex;
    std::atomic<bool> running{true};
    ControlWriter &writer;
    std::thread reader;
    static bool event(int fd, const uhid_event &e) {
        ssize_t n;
        do { n = write(fd, &e, sizeof(e)); } while (n < 0 && errno == EINTR);
        return n == sizeof(e);
    }
    void read_outputs() {
        while (running) {
            // Keep the map locked while polling and reading: a destroyed fd
            // cannot be reused by another device during this batch.
            std::unique_lock<std::mutex> lock(mutex);
            std::vector<pollfd> polls;
            std::vector<uint16_t> ids;
            for (auto &d : devices) { polls.push_back({d.second, POLLIN, 0}); ids.push_back(d.first); }
            if (polls.empty()) { lock.unlock(); usleep(20000); continue; }
            int count = poll(polls.data(), polls.size(), 20);
            if (count <= 0) { lock.unlock(); usleep(1000); continue; }
            for (size_t i = 0; i < polls.size(); ++i) {
                if (!(polls[i].revents & POLLIN)) { continue; }
                uhid_event e{};
                if (read(polls[i].fd, &e, sizeof(e)) <= 0) { continue; }
                if (e.type == UHID_OUTPUT && e.u.output.size <= UHID_DATA_MAX) {
                    writer.hid(ids[i], e.u.output.data, e.u.output.size);
                } else if (e.type == UHID_GET_REPORT) {
                    uhid_event reply{}; reply.type = UHID_GET_REPORT_REPLY;
                    reply.u.get_report_reply.id = e.u.get_report.id;
                    reply.u.get_report_reply.err = EOPNOTSUPP;
                    event(polls[i].fd, reply);
                } else if (e.type == UHID_SET_REPORT) {
                    writer.hid(ids[i], e.u.set_report.data, e.u.set_report.size);
                    uhid_event reply{}; reply.type = UHID_SET_REPORT_REPLY;
                    reply.u.set_report_reply.id = e.u.set_report.id;
                    event(polls[i].fd, reply);
                }
            }
            lock.unlock(); usleep(1000);
        }
    }
public:
    explicit Hid(ControlWriter &channel) : writer(channel), reader([this] { read_outputs(); }) {}
    ~Hid() {
        running = false; if (reader.joinable()) { reader.join(); }
        for (auto &d : devices) { uhid_event e{}; e.type = UHID_DESTROY; event(d.second, e); close(d.second); }
    }
    bool create(uint16_t id, uint16_t vendor, uint16_t product, const std::string &name,
                const std::vector<uint8_t> &descriptor) {
        if (descriptor.empty() || descriptor.size() > HID_MAX_DESCRIPTOR_SIZE) { return false; }
        destroy(id);
        int fd = open("/dev/uhid", O_RDWR | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) { perror("[harmony] open /dev/uhid"); return false; }
        uhid_event e{}; e.type = UHID_CREATE2;
        snprintf(reinterpret_cast<char *>(e.u.create2.name), sizeof(e.u.create2.name), "%s", name.c_str());
        e.u.create2.rd_size = descriptor.size(); e.u.create2.bus = BUS_USB;
        e.u.create2.vendor = vendor; e.u.create2.product = product;
        memcpy(e.u.create2.rd_data, descriptor.data(), descriptor.size());
        if (!event(fd, e)) { close(fd); return false; }
        std::lock_guard<std::mutex> lock(mutex); devices[id] = fd;
        fprintf(stderr, "[harmony] UHID device %u created\n", id);
        return true;
    }
    bool input(uint16_t id, const std::vector<uint8_t> &bytes) {
        if (bytes.size() > UHID_DATA_MAX) { return false; }
        std::lock_guard<std::mutex> lock(mutex);
        auto device = devices.find(id); if (device == devices.end()) { return false; }
        uhid_event e{}; e.type = UHID_INPUT2; e.u.input2.size = bytes.size();
        memcpy(e.u.input2.data, bytes.data(), bytes.size());
        return event(device->second, e);
    }
    void destroy(uint16_t id) {
        std::lock_guard<std::mutex> lock(mutex);
        auto device = devices.find(id);
        if (device != devices.end()) {
            uhid_event e{}; e.type = UHID_DESTROY; event(device->second, e);
            close(device->second); devices.erase(device);
            fprintf(stderr, "[harmony] UHID device %u destroyed\n", id);
        }
    }
};
}
#endif
