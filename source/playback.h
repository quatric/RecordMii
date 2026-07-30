/* Streaming WAV playback through the DSP, so a take can be checked without
 * pulling the SD card. */

#ifndef PLAYBACK_H
#define PLAYBACK_H

#include <gctypes.h>

/* Brings up ASND.  Call once, before any other PB_ function. */
bool PB_Init(void);
void PB_Shutdown(void);

/* Starts streaming `path`.  Only 16-bit mono PCM is accepted, which is what
 * this app records.  Any playback already running is stopped first. */
bool PB_Start(const char *path);
void PB_Stop(void);

/* Refills the DSP from the file.  Must be called regularly (once a frame is
 * plenty) while playing, and does nothing when idle. */
void PB_Pump(void);

bool PB_IsPlaying(void);

/* Elapsed and total seconds of the file being played. */
u32 PB_ElapsedSecs(void);
u32 PB_TotalSecs(void);

#endif /* PLAYBACK_H */
