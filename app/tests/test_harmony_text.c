/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stddef.h>

#include "../../harmony/text_input.h"

static void check(unsigned char c, uint32_t keycode, bool shift) {
    struct sc_harmony_text_key key;
    assert(sc_harmony_ascii_key(c, &key));
    assert(key.keycode == keycode);
    assert(key.shift == shift);
}

int main(void) {
    /* OTP digits must be physical digit keys, including leading zero. */
    for (unsigned i = 0; i < 10; ++i) { check('0' + i, 7 + i, false); }
    for (unsigned i = 0; i < 26; ++i) {
        check('a' + i, 29 + i, false);
        check('A' + i, 29 + i, true);
    }
    check(' ', 62, false);
    check('!', 8, true);
    check('@', 9, true);
    check('(', 16, true);
    check(')', 7, true);
    check('"', 75, true);
    check('\'', 75, false);
    check('+', 70, true);
    check('=', 70, false);
    check('\\', 73, false);
    check('|', 73, true);
    check('~', 68, true);

    struct sc_harmony_text_key key;
    for (unsigned c = 32; c < 127; ++c) { assert(sc_harmony_ascii_key(c, &key)); }
    /* Control characters are not Enter/Tab commands. UTF-8 must use the text
     * path as a whole, never inject a prefix and then duplicate it by pasting. */
    const unsigned char non_ascii[] = {0, 9, 10, 13, 31, 127, 0xc3, 0xe4, 0xf0, 0xff};
    for (size_t i = 0; i < sizeof(non_ascii); ++i) {
        assert(!sc_harmony_ascii_key(non_ascii[i], &key));
    }
    return 0;
}
