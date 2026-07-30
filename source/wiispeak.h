/* Wii Speak (P-Mic) driver for libogc homebrew.
 *
 * Reimplements the RVL SDK's PMIC library on top of libogc's /dev/usb/oh0
 * wrapper.  Register numbers and the boot sequence were recovered from
 * pmic.a and cross-checked against Dolphin's emulated device.
 */

#ifndef WIISPEAK_H
#define WIISPEAK_H

#include <gctypes.h>

#define WS_VID 0x057E
#define WS_PID 0x0308

/* Sampling frequency (SAMPLER_FREQ). */
#define WS_FREQ_8KHZ  0x0000
#define WS_FREQ_11KHZ 0x0001
#define WS_FREQ_16KHZ 0x0003 /* default */

/* Microphone ADC gain (SAMPLER_GAIN). */
#define WS_GAIN_00dB 0x0000
#define WS_GAIN_15dB 0x0001
#define WS_GAIN_30dB 0x0002
#define WS_GAIN_36dB 0x0003 /* default */

/* Errors.  USB-level failures are returned as-is (always negative). */
#define WS_OK          0
#define WS_ENODEV     -1
#define WS_EINVAL     -2
#define WS_ESTATE     -3
#define WS_EFIRMWARE  -4
#define WS_ENOMEM     -5
#define WS_ETIMEOUT   -6

/* Returns the sample rate in Hz for a WS_FREQ_* constant. */
u32 WS_FreqToHz(u16 freq);

/* Opens the device and boots it with `firmware` (PMIC_FIRM_DATA from the
 * SDK's pmic.a -- see tools/extract_pmic_firmware.py).  The mic ignores every
 * other request until this has completed. */
s32 WS_Open(const void *firmware, u32 firmware_size);
s32 WS_Close(void);

/* printf()s the IOS version, which USB interfaces IOS is offering, and every
 * device it can see.  Call this when WS_Open() reports WS_ENODEV. */
void WS_Diagnose(void);

/* Settings.  Only valid between WS_Open() and WS_Close(). */
s32 WS_SetFreq(u16 freq);
s32 WS_GetFreq(u16 *freq);
s32 WS_SetGain(u16 gain);
s32 WS_SetMute(bool mute);
s32 WS_SetEcReset(bool reset);

/* Acoustic signal processing (echo canceller + AGC).  With it enabled the
 * usable band is capped near 11 kHz even when sampling at 16 kHz, so a plain
 * recorder wants it off. */
s32 WS_SetSignalProcessing(bool enable);

/* Instantaneous send level, as used by the SDK's level meter. */
s32 WS_GetSendLevel(u16 *level);

/* Streaming.  WS_Start() spawns the transfer threads; samples then accumulate
 * in an internal ring buffer until read. */
s32 WS_Start(void);
s32 WS_Stop(void);

/* Copies up to `max_samples` host-endian 16-bit mono samples out of the ring
 * buffer and returns how many were taken. */
u32 WS_Read(s16 *dst, u32 max_samples);

/* Samples currently buffered, and the count dropped to overrun since
 * WS_Start().  A non-zero drop count means the consumer is too slow. */
u32 WS_Available(void);
u32 WS_Dropped(void);

#endif /* WIISPEAK_H */
