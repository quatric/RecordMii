/* Streaming 16-bit mono WAV writer.  See wav.h. */

#include <string.h>

#include "wav.h"

#define HEADER_SIZE 44
#define SWAP_CHUNK  1024

static void put_le16(u8 *p, u16 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
}

static void put_le32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

static void build_header(u8 *hdr, u32 sample_rate, u32 samples)
{
	const u32 data_bytes = samples * 2;
	const u32 byte_rate = sample_rate * 2;

	memcpy(hdr + 0, "RIFF", 4);
	put_le32(hdr + 4, 36 + data_bytes);
	memcpy(hdr + 8, "WAVEfmt ", 8);
	put_le32(hdr + 16, 16);   /* fmt chunk size */
	put_le16(hdr + 20, 1);    /* PCM */
	put_le16(hdr + 22, 1);    /* mono */
	put_le32(hdr + 24, sample_rate);
	put_le32(hdr + 28, byte_rate);
	put_le16(hdr + 32, 2);    /* block align */
	put_le16(hdr + 34, 16);   /* bits per sample */
	memcpy(hdr + 36, "data", 4);
	put_le32(hdr + 40, data_bytes);
}

bool WAV_Open(wav_t *w, const char *path, u32 sample_rate)
{
	u8 hdr[HEADER_SIZE];

	w->fp = fopen(path, "wb");
	if (!w->fp)
		return false;

	w->sample_rate = sample_rate;
	w->samples = 0;

	build_header(hdr, sample_rate, 0);
	if (fwrite(hdr, 1, sizeof(hdr), w->fp) != sizeof(hdr)) {
		fclose(w->fp);
		w->fp = NULL;
		return false;
	}

	return true;
}

bool WAV_Write(wav_t *w, const s16 *samples, u32 count)
{
	u8 buf[SWAP_CHUNK * 2];
	u32 done = 0;

	if (!w->fp)
		return false;

	while (done < count) {
		u32 n = count - done;
		u32 i;

		if (n > SWAP_CHUNK)
			n = SWAP_CHUNK;

		for (i = 0; i < n; i++)
			put_le16(buf + i * 2, (u16)samples[done + i]);

		if (fwrite(buf, 2, n, w->fp) != n)
			return false;

		done += n;
		w->samples += n;
	}

	return true;
}

bool WAV_Close(wav_t *w)
{
	u8 hdr[HEADER_SIZE];
	bool ok = true;

	if (!w->fp)
		return false;

	build_header(hdr, w->sample_rate, w->samples);

	if (fseek(w->fp, 0, SEEK_SET) != 0 ||
	    fwrite(hdr, 1, sizeof(hdr), w->fp) != sizeof(hdr))
		ok = false;

	if (fclose(w->fp) != 0)
		ok = false;

	w->fp = NULL;
	return ok;
}
