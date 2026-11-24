#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "passport_workbuddy.h"

int main(void) {
    const uint8_t bytes[] = {0x00, 0x01, 0x02, 'W', 'B'};
    char b64[32];
    size_t n = workbuddy_base64(bytes, sizeof(bytes), b64);
    b64[n] = 0;
    assert(!strcmp(b64, "AAECV0I="));

    uint8_t wav[44];
    workbuddy_wav_header(wav, 16000);
    assert(!memcmp(wav, "RIFF", 4) && !memcmp(wav + 8, "WAVEfmt ", 8));
    assert(wav[24] == 0x80 && wav[25] == 0x3e);   // 16000 Hz little endian

    char page[256];
    const char *text = "Hello WorkBuddy 你好，这是回复文本。";
    size_t at = 0, pages = 0;
    do {
        size_t next = workbuddy_reply_page(text, at, page, sizeof(page));
        assert(page[0]);
        ++pages;
        if (next <= at) break;
        at = next;
    } while (text[at]);
    assert(pages >= 1);

    const char *emoji = "A😀B";
    size_t next = workbuddy_reply_page(emoji, 0, page, 5);
    assert(next > 0 && page[0] == 'A');
    printf("WorkBuddy text tests: PASS\n");
    return 0;
}
