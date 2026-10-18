/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GOW3_MOVIE_HOOK_H
#define GOW3_MOVIE_HOOK_H
#include <stdint.h>
#include <string.h>
#include "signature.h"

/* Uncharted 1-3 cutscenes are pre-rendered movies (build/main/movie1/ps4/<name>.bsf) played by ndlib's
 * movie-player.cpp. Its frame step (U1 0x4a0e62, U2 0xb84805, U3 0xdefd55) shows the next frame
 * once per game frame while the game clock runs, then catches up with the movie's audio clock
 * when it is behind; it never waits when ahead. The PS4 runs one game frame per 60 Hz vblank,
 * so above 60 FPS the movies played fps/60 times too fast (2.0x at 120 FPS, measured by the
 * camera cuts) while their audio kept real time.
 * The hook paces the step by game time instead: the clock's dt accumulates, a frame is shown once
 * half a vblank is owed and pays one vblank, at most one per game frame as before. A frame-limited
 * 60 FPS (phase noise under +-4 ms) still advances every frame, and so does anything slower; the
 * audio catch-up is unchanged.
 * At the site: rbx = the player, eax = its frame counter (+0x2254c), rcx = the game clock whose
 * dt the code just tested (U1 +0x6c, U2 +0x8c, U3 +0x9c); rdx, xmm0 and the flags are free. */
enum { GOW3_MOVIE_HOOK_LENGTH = 8 };
static const unsigned char gow3_movie_step_original[GOW3_MOVIE_HOOK_LENGTH] = {
    0xff, 0xc0,                        /* inc eax */
    0x89, 0x83, 0x4c, 0x25, 0x02, 0x00 /* mov [rbx+0x2254c], eax */
};
/* The clock test before the step: a 32-bit displacement in U2/U3, an 8-bit one in U1. */
static const Gow3Signature gow3_movie_sites[2] = {
    {"48 8d 0d ?? ?? ?? ?? c5 f8 57 c0 c5 f8 2e 81 ?? ?? ?? ?? 74 08 ff c0 89 83 4c 25 02 00 "
     "8b 8b 50 25 02 00 83 f9 02", 21},
    {"48 8d 0d ?? ?? ?? ?? c5 f8 57 c0 c5 f8 2e 41 ?? 74 08 ff c0 89 83 4c 25 02 00 "
     "8b 8b 50 25 02 00 83 f9 02", 18},
};

/* The step's image offset and the clock's dt displacement, or UINT64_MAX when not found. */
static inline uint64_t gow3_movie_find(const unsigned char *image, uint64_t size, int32_t *dt_offset) {
    uint64_t at = gow3_signature_find(image, size, &gow3_movie_sites[0]);
    if (at != UINT64_MAX) {
        memcpy(dt_offset, image + at - 6, 4);
        return at;
    }
    at = gow3_signature_find(image, size, &gow3_movie_sites[1]);
    if (at != UINT64_MAX) {
        *dt_offset = (int8_t)image[at - 3];
        return at;
    }
    return UINT64_MAX;
}

/* The stub's data (16 bytes, writable): owed time, then its constants. */
static inline void gow3_movie_init_data(float data[4]) {
    data[0] = 0.0f;                 /* owed game time, seconds */
    data[1] = 0.5f / 60.0f;         /* a frame is shown from half a vblank owed */
    data[2] = 1.0f / 60.0f;         /* each shown frame pays one vblank */
    data[3] = 0.5f / 60.0f;         /* what may stay owed (catching up is the audio clock's job) */
}

/* The step, called from the site; returns the bytes it wrote. */
static inline unsigned gow3_movie_build_stub(unsigned char *out, int32_t dt_offset, uintptr_t data) {
    static const unsigned char code[] = {
        0xc5, 0xfa, 0x10, 0x81, 0, 0, 0, 0,     /* vmovss xmm0, [rcx+dt] */
        0x48, 0xba, 0, 0, 0, 0, 0, 0, 0, 0,     /* movabs rdx, data */
        0xc5, 0xfa, 0x58, 0x02,                 /* vaddss xmm0, xmm0, [rdx] */
        0xc5, 0xf8, 0x2e, 0x42, 0x04,           /* vucomiss xmm0, [rdx+4] */
        0x72, 0x0c,                             /* jb store */
        0xff, 0xc0,                             /* inc eax */
        0xc5, 0xfa, 0x5c, 0x42, 0x08,           /* vsubss xmm0, xmm0, [rdx+8] */
        0xc5, 0xfa, 0x5d, 0x42, 0x0c,           /* vminss xmm0, xmm0, [rdx+12] */
        0xc5, 0xfa, 0x11, 0x02,                 /* store: vmovss [rdx], xmm0 */
        0x89, 0x83, 0x4c, 0x25, 0x02, 0x00,     /* mov [rbx+0x2254c], eax */
        0xc3                                    /* ret */
    };
    memcpy(out, code, sizeof(code));
    memcpy(out + 4, &dt_offset, 4);
    memcpy(out + 10, &data, 8);
    return (unsigned)sizeof(code);
}

/* The site's replacement: call rel32 to the stub, then a 3-byte nop. */
static inline void gow3_movie_build_call(unsigned char out[GOW3_MOVIE_HOOK_LENGTH], int32_t relative) {
    out[0] = 0xe8;
    memcpy(out + 1, &relative, 4);
    out[5] = 0x0f; out[6] = 0x1f; out[7] = 0x00;
}
#endif
