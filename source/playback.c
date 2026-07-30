/* Streaming WAV playback through the DSP.  See playback.h. */

#include <asndlib.h>
#include <malloc.h>
#include <ogc/cache.h>
#include <stdio.h>
#include <string.h>

#include "playback.h"

/* 8 KB is a quarter second at 16 kHz mono; two of them give the main loop
 * half a second of slack before the DSP runs dry. */
#define CHUNK 8192

static bool pb_ready;
static bool pb_playing;

static FILE *pb_fp;
static s32 pb_voice = -1;
static u8 *pb_buf[2];
static int pb_next;      /* buffer to fill on the next refill */
static u32 pb_rate;
static u32 pb_data_left; /* bytes of the data chunk still unread */
static u32 pb_data_size;

static u32 rd_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 rd_le16(const u8 *p)
{
	return (u16)(p[0] | (p[1] << 8));
}

bool PB_Init(void)
{
	if (pb_ready)
		return true;

	pb_buf[0] = memalign(32, CHUNK);
	pb_buf[1] = memalign(32, CHUNK);
	if (!pb_buf[0] || !pb_buf[1]) {
		free(pb_buf[0]);
		free(pb_buf[1]);
		pb_buf[0] = pb_buf[1] = NULL;
		return false;
	}

	ASND_Init();
	ASND_Pause(0);

	pb_ready = true;
	return true;
}

void PB_Shutdown(void)
{
	if (!pb_ready)
		return;

	PB_Stop();
	ASND_End();

	free(pb_buf[0]);
	free(pb_buf[1]);
	pb_buf[0] = pb_buf[1] = NULL;
	pb_ready = false;
}

/* Walks the RIFF chunks looking for `fmt ` and `data`.  Leaves the file
 * positioned at the first sample. */
static bool parse_wav(FILE *fp, u32 *rate, u32 *data_size)
{
	u8 hdr[12];
	u16 channels = 0, bits = 0;
	bool have_fmt = false;

	if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr))
		return false;
	if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
		return false;

	while (1) {
		u8 chunk[8];
		u32 size;

		if (fread(chunk, 1, sizeof(chunk), fp) != sizeof(chunk))
			return false;

		size = rd_le32(chunk + 4);

		if (memcmp(chunk, "fmt ", 4) == 0) {
			u8 fmt[16];

			if (size < sizeof(fmt))
				return false;
			if (fread(fmt, 1, sizeof(fmt), fp) != sizeof(fmt))
				return false;

			if (rd_le16(fmt + 0) != 1) /* PCM only */
				return false;

			channels = rd_le16(fmt + 2);
			*rate = rd_le32(fmt + 4);
			bits = rd_le16(fmt + 14);
			have_fmt = true;

			if (size > sizeof(fmt) &&
			    fseek(fp, (long)(size - sizeof(fmt)), SEEK_CUR) != 0)
				return false;
		} else if (memcmp(chunk, "data", 4) == 0) {
			if (!have_fmt || channels != 1 || bits != 16)
				return false;
			*data_size = size;
			return true;
		} else {
			if (fseek(fp, (long)(size + (size & 1)), SEEK_CUR) != 0)
				return false;
		}
	}
}

/* Reads up to CHUNK bytes of sample data into buf[slot]. */
static u32 fill(int slot)
{
	u32 want = pb_data_left < CHUNK ? pb_data_left : CHUNK;
	size_t got;

	if (want == 0)
		return 0;

	got = fread(pb_buf[slot], 1, want, pb_fp);
	if (got == 0)
		return 0;

	pb_data_left -= (u32)got;
	DCFlushRange(pb_buf[slot], got);
	return (u32)got;
}

bool PB_Start(const char *path)
{
	u32 first;

	if (!pb_ready)
		return false;

	PB_Stop();

	pb_fp = fopen(path, "rb");
	if (!pb_fp)
		return false;

	if (!parse_wav(pb_fp, &pb_rate, &pb_data_size)) {
		fclose(pb_fp);
		pb_fp = NULL;
		return false;
	}

	pb_data_left = pb_data_size;

	pb_voice = ASND_GetFirstUnusedVoice();
	if (pb_voice == SND_INVALID) {
		fclose(pb_fp);
		pb_fp = NULL;
		return false;
	}

	first = fill(0);
	if (first == 0) {
		fclose(pb_fp);
		pb_fp = NULL;
		pb_voice = -1;
		return false;
	}

	/* The WAV holds little-endian samples; the DSP can take them as-is.
	 * A NULL callback lets the voice stop on its own at end of file. */
	if (ASND_SetVoice(pb_voice, VOICE_MONO_16BIT_LE, (s32)pb_rate, 0,
	                  pb_buf[0], (s32)first, 255, 255, NULL) != SND_OK) {
		fclose(pb_fp);
		pb_fp = NULL;
		pb_voice = -1;
		return false;
	}

	pb_next = 1;
	pb_playing = true;

	/* Queue the second buffer straight away so there is no initial gap. */
	PB_Pump();
	return true;
}

void PB_Stop(void)
{
	if (pb_voice >= 0) {
		ASND_StopVoice(pb_voice);
		pb_voice = -1;
	}
	if (pb_fp) {
		fclose(pb_fp);
		pb_fp = NULL;
	}

	pb_playing = false;
	pb_data_left = 0;
	pb_data_size = 0;
}

void PB_Pump(void)
{
	if (!pb_playing)
		return;

	/* Out of file: let the queued buffers drain, then tidy up. */
	if (pb_data_left == 0) {
		if (ASND_StatusVoice(pb_voice) == SND_UNUSED)
			PB_Stop();
		return;
	}

	while (ASND_TestVoiceBufferReady(pb_voice) == 1) {
		u32 got = fill(pb_next);

		if (got == 0) { /* short read: nothing more is coming */
			pb_data_left = 0;
			return;
		}

		if (ASND_AddVoice(pb_voice, pb_buf[pb_next], (s32)got) != SND_OK)
			return;

		pb_next ^= 1;
	}
}

bool PB_IsPlaying(void)
{
	return pb_playing;
}

u32 PB_ElapsedSecs(void)
{
	if (!pb_playing)
		return 0;
	return ASND_GetTimerVoice(pb_voice) / 1000;
}

u32 PB_TotalSecs(void)
{
	if (pb_rate == 0)
		return 0;
	return pb_data_size / 2 / pb_rate;
}
