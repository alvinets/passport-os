/* Local text/encoding helpers for the WorkBuddy adapter. */
#include "passport_workbuddy.h"
#include <string.h>

void workbuddy_wav_header(uint8_t h[44], uint32_t rate) {
    static const uint8_t riff[4] = {'R','I','F','F'};
    static const uint8_t wave[8] = {'W','A','V','E','f','m','t',' '};
    static const uint8_t data[4] = {'d','a','t','a'};
    memcpy(h, riff, 4);
    for (int i = 0; i < 4; ++i) h[4 + i] = 0xff;   // streaming length unknown
    memcpy(h + 8, wave, 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0;
    h[20] = 1; h[21] = 0;                          // PCM
    h[22] = 1; h[23] = 0;                          // mono
    for (int i = 0; i < 4; ++i) { h[24 + i] = (uint8_t)(rate >> (8 * i)); h[28 + i] = (uint8_t)((rate * 2) >> (8 * i)); }
    h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, data, 4);
    for (int i = 0; i < 4; ++i) h[40 + i] = 0xff;
}

size_t workbuddy_base64(const uint8_t *in, size_t n, char *out) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[k++] = alphabet[v >> 18];
        out[k++] = alphabet[(v >> 12) & 63];
        out[k++] = i + 1 < n ? alphabet[(v >> 6) & 63] : '=';
        out[k++] = i + 2 < n ? alphabet[v & 63] : '=';
    }
    return k;
}

static size_t utf8_bytes(const char *p) {
    unsigned char c = (unsigned char)*p;
    size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    for (size_t i = 1; i < n; ++i)
        if (!p[i] || ((unsigned char)p[i] & 0xc0) != 0x80) return 1;
    return n;
}

size_t workbuddy_reply_page(const char *text, size_t at, char *out, size_t cap) {
    if (!cap) return at;
    out[0] = 0;
    size_t len = strlen(text);
    if (at >= len) return len;
    while (at && ((unsigned char)text[at] & 0xc0) == 0x80) --at;
    size_t used = 0;
    unsigned col = 0, lines = 1;
    while (at < len) {
        size_t n = utf8_bytes(text + at);
        static const char narrow[] = " !\"'()*,-./:;?I[\\]fijlrstxz{|}";
        unsigned width = n == 1 && strchr(narrow, text[at]) ? 1 : 2;
        bool newline = text[at] == '\n';
        if (newline || (!newline && col + width > 26)) {
            if (lines == 5) { if (newline) ++at; break; }
            if (used + 1 >= cap) break;
            out[used++] = '\n'; ++lines; col = 0;
            if (newline) { ++at; continue; }
        }
        if (used + n >= cap) break;
        memcpy(out + used, text + at, n);
        used += n; at += n; col += width;
    }
    out[used] = 0;
    return at;
}
