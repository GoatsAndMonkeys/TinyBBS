#pragma once
#include <cctype>
#include <cstring>
#include <stdint.h>
#include "BBSWordleData.h"

// ── Bloom filter word validation ─────────────────────────────────────────────
// FNV-1a double hashing — must match scripts/gen_wordle.py exactly.
// h(i, x) = (fnv1a(x, 2166136261) + i * fnv1a(x, 0xdeadbeef)) % (BLOOM_BYTES*8)

static inline uint32_t _wbl_fnv1a(const char *s, uint32_t basis) {
    uint32_t h = basis;
    while (*s) { h = ((h ^ (uint8_t)*s++) * 16777619u) & 0xFFFFFFFFu; }
    return h;
}

// Returns true if word is in the 2000-word dictionary (~0.27% false positives).
// Accepts upper or lowercase input.
static bool wordleIsValid(const char *word) {
    if (!word || strlen(word) != 5) return false;
    char w[6];
    for (int i = 0; i < 5; i++) {
        if (!isalpha((unsigned char)word[i])) return false;
        w[i] = (char)tolower((unsigned char)word[i]);
    }
    w[5] = '\0';
    uint32_t h1 = _wbl_fnv1a(w, 2166136261u);
    uint32_t h2 = _wbl_fnv1a(w, 0xdeadbeef);
    uint32_t m  = WORDLE_BLOOM_BYTES * 8;
    for (uint32_t i = 0; i < WORDLE_BLOOM_K; i++) {
        uint32_t bit = (h1 + i * h2) % m;
        if (!(WORDLE_BLOOM[bit >> 3] & (1u << (bit & 7)))) return false;
    }
    return true;
}

// Pick today's answer (deterministic by day number).
static const char *wordlePickWord(uint32_t day) {
    return WORDLE_WORDS[day % WORDLE_WORD_COUNT];
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
