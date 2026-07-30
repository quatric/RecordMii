/* Streaming 16-bit mono WAV writer. */

#ifndef WAV_H
#define WAV_H

#include <gctypes.h>
#include <stdio.h>

typedef struct {
	FILE *fp;
	u32 sample_rate;
	u32 samples;
} wav_t;

/* Creates `path` and reserves the header; sizes are patched in by WAV_Close. */
bool WAV_Open(wav_t *w, const char *path, u32 sample_rate);

/* Appends host-endian samples, byte-swapping into WAV's little-endian order. */
bool WAV_Write(wav_t *w, const s16 *samples, u32 count);

bool WAV_Close(wav_t *w);

#endif /* WAV_H */
