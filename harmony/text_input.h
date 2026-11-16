/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SCRCPY_HARMONY_TEXT_INPUT_H
#define SCRCPY_HARMONY_TEXT_INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* Logical Android key identifiers used by the scrcpy control protocol. Keep
 * this mapping independent of Linux and the device SDK so it is host-testable.
 * Text is committed text from the desktop IME, with case already resolved. */
struct sc_harmony_text_key {
    uint32_t keycode;
    bool shift;
};

static inline bool
sc_harmony_ascii_key(unsigned char c, struct sc_harmony_text_key *out) {
    out->shift = false;
    if (c >= 'a' && c <= 'z') {
        out->keycode = 29 + c - 'a';
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        out->keycode = 29 + c - 'A';
        out->shift = true;
        return true;
    }
    if (c >= '0' && c <= '9') {
        out->keycode = 7 + c - '0';
        return true;
    }
    if (c == ' ') {
        out->keycode = 62;
        return true;
    }
    static const char number_symbols[] = ")!@#$%^&*(";
    for (unsigned i = 0; i < 10; ++i) {
        if (c == (unsigned char) number_symbols[i]) {
            out->keycode = 7 + i;
            out->shift = true;
            return true;
        }
    }
    static const char plain[] = "`-=[]\\;',./";
    static const char shifted[] = "~_+{}|:\"<>?";
    static const uint32_t codes[] = {68, 69, 70, 71, 72, 73, 74, 75, 55, 56, 76};
    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        if (c == (unsigned char) plain[i] || c == (unsigned char) shifted[i]) {
            out->keycode = codes[i];
            out->shift = c == (unsigned char) shifted[i];
            return true;
        }
    }
    return false;
}

#endif
