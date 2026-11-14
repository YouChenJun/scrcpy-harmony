/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SCRCPY_HARMONY_CLIPBOARD_H
#define SCRCPY_HARMONY_CLIPBOARD_H
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <database/pasteboard/oh_pasteboard.h>
#include <database/udmf/udmf.h>
#include <database/udmf/uds.h>
#include "wire.h"
namespace harmony {
class Clipboard {
    OH_Pasteboard *board = nullptr;
    std::mutex mutex;
    std::string last_text;
    uint32_t changes = 0;
    std::atomic<bool> running{false};
    std::thread observer;
    bool get_locked(std::string &text) {
        if (!board) { return false; }
        if (!OH_Pasteboard_HasData(board)) { text.clear(); return true; }
        int status;
        auto data = OH_Pasteboard_GetData(board, &status);
        if (!data) { return false; }
        auto plain = OH_UdsPlainText_Create();
        bool ok = plain && OH_UdmfData_GetPrimaryPlainText(data, plain) == 0;
        if (ok) {
            auto content = OH_UdsPlainText_GetContent(plain);
            text = content ? content : "";
        }
        if (plain) { OH_UdsPlainText_Destroy(plain); }
        OH_UdmfData_Destroy(data);
        return ok;
    }
public:
    Clipboard() : board(OH_Pasteboard_Create()) {}
    ~Clipboard() { stop(); if (board) { OH_Pasteboard_Destroy(board); } }
    bool get(std::string &text) { std::lock_guard<std::mutex> lock(mutex); return get_locked(text); }
    bool set(const std::string &text) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!board) { return false; }
        auto data = OH_UdmfData_Create(); auto record = OH_UdmfRecord_Create(); auto plain = OH_UdsPlainText_Create();
        bool ok = data && record && plain
            && OH_UdsPlainText_SetContent(plain, text.c_str()) == 0
            && OH_UdmfRecord_AddPlainText(record, plain) == 0
            && OH_UdmfData_AddRecord(data, record) == 0
            && OH_Pasteboard_SetData(board, data) == 0;
        if (plain) { OH_UdsPlainText_Destroy(plain); }
        if (record) { OH_UdmfRecord_Destroy(record); }
        if (data) { OH_UdmfData_Destroy(data); }
        if (ok) { last_text = text; changes = OH_Pasteboard_GetChangeCount(board); }
        return ok;
    }
    void watch(ControlWriter &writer) {
        if (!board) { return; }
        { std::lock_guard<std::mutex> lock(mutex); get_locked(last_text); changes = OH_Pasteboard_GetChangeCount(board); }
        running = true;
        observer = std::thread([this, &writer] {
            while (running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                std::string text;
                bool changed = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    auto count = OH_Pasteboard_GetChangeCount(board);
                    if (count != changes) {
                        changes = count;
                        if (get_locked(text) && text != last_text) { last_text = text; changed = true; }
                    }
                }
                if (changed && !writer.clipboard(text)) { running = false; }
            }
        });
    }
    void stop() { running = false; if (observer.joinable()) { observer.join(); } }
};
}
#endif
