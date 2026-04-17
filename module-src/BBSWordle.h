#pragma once
#include <cctype>
#include <cstring>
#include <stdint.h>
#include "BBSWordleData.h"

// ── External flash word list support ─────────────────────────────────────────
#ifdef WORDLE_ON_EXTFLASH
#include "BBSExtFlash.h"
using Adafruit_LittleFS_Namespace::File;
using Adafruit_LittleFS_Namespace::FILE_O_READ;

#define WORDLE_BIN_PATH "/bbs/kb/wordle.bin"

// Read a single word by index from external flash.
// File format: 2000 × 6 bytes (5 chars + null), no header.
static bool wordleReadWord(uint32_t idx, char *out) {
    if (idx >= WORDLE_WORD_COUNT) return false;
    File f = bbsExtFS().open(WORDLE_BIN_PATH, FILE_O_READ);
    if (!f) return false;
    f.seek(idx * 6);
    f.read((uint8_t *)out, 5);
    out[5] = '\0';
    f.close();
    return true;
}

// Check if wordle word list is available on external flash.
static bool wordleDataAvailable() {
    File f = bbsExtFS().open(WORDLE_BIN_PATH, FILE_O_READ);
    if (!f) return false;
    bool ok = (f.size() >= WORDLE_WORD_COUNT * 6);
    f.close();
    return ok;
}
#endif // WORDLE_ON_EXTFLASH

// ── Word validation ──────────────────────────────────────────────────────────

#ifndef WORDLE_ON_EXTFLASH
static inline uint32_t _wbl_fnv1a(const char *s, uint32_t basis) {
    uint32_t h = basis;
    while (*s) { h = ((h ^ (uint8_t)*s++) * 16777619u) & 0xFFFFFFFFu; }
    return h;
}
#endif

// Returns true if word is a valid guess.
static bool wordleIsValid(const char *word) {
    if (!word || strlen(word) != 5) return false;
    for (int i = 0; i < 5; i++) {
        if (!isalpha((unsigned char)word[i])) return false;
    }
#ifndef WORDLE_ON_EXTFLASH
    char w[6];
    for (int i = 0; i < 5; i++)
        w[i] = (char)tolower((unsigned char)word[i]);
    w[5] = '\0';
    uint32_t h1 = _wbl_fnv1a(w, 2166136261u);
    uint32_t h2 = _wbl_fnv1a(w, 0xdeadbeef);
    uint32_t m  = WORDLE_BLOOM_BYTES * 8;
    for (uint32_t i = 0; i < WORDLE_BLOOM_K; i++) {
        uint32_t bit = (h1 + i * h2) % m;
        if (!(WORDLE_BLOOM[bit >> 3] & (1u << (bit & 7)))) return false;
    }
#endif
    return true;
}

// Pick today's answer (deterministic by day number).
static bool wordlePickWord(uint32_t day, char *out) {
#ifdef WORDLE_ON_EXTFLASH
    return wordleReadWord(day % WORDLE_WORD_COUNT, out);
#else
    strncpy(out, WORDLE_WORDS[day % WORDLE_WORD_COUNT], 5);
    out[5] = '\0';
    return true;
#endif
}

// Per-letter feedback: G=right place, Y=wrong place, X=not in word
static void wordleFeedback(const char *guess, const char *target, char *fb) {
    bool used[5] = {};
    for (int i = 0; i < 5; i++) {
        if (tolower((unsigned char)guess[i]) == tolower((unsigned char)target[i])) {
            fb[i] = 'G'; used[i] = true;
        } else {
            fb[i] = 'X';
        }
    }
    for (int i = 0; i < 5; i++) {
        if (fb[i] == 'G') continue;
        for (int j = 0; j < 5; j++) {
            if (!used[j] &&
                tolower((unsigned char)guess[i]) == tolower((unsigned char)target[j])) {
                fb[i] = 'Y'; used[j] = true; break;
            }
        }
    }
}

// BBSWordleScore is defined in BBSStorage.h
