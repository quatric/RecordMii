/* Wii Speak Recorder -- records the Wii Speak to WAV files on the SD card. */

#include <fat.h>
#include <gccore.h>
#include <malloc.h>
#include <ogc/ios.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wiiuse/wpad.h>
#include <input_wii.h>

#include "playback.h"
#include "wav.h"
#include "wiispeak.h"

#define REC_DIR "sd:/wiispeak"

/* `make firmware SDK=...` drops the blob in data/, which the build converts
 * into this header.  Without it the app reads the blob off the SD card. */
#if defined(__has_include)
#  if __has_include("pmic_firm_bin.h")
#    include "pmic_firm_bin.h"
#    define HAVE_EMBEDDED_FIRMWARE 1
#  endif
#endif

/* Where the extracted PMIC_FIRM_DATA blob is looked for, in order. */
static const char *FIRMWARE_PATHS[] = {
	"sd:/apps/wiispeakrec/pmic_firm.bin",
	"sd:/wiispeak/pmic_firm.bin",
	"pmic_firm.bin",
};

#define PUMP_SAMPLES 4096

static void *xfb;
static GXRModeObj *rmode;

static const u16 GAIN_DB[] = { 0, 15, 30, 36 };
static const u16 FREQS[] = { WS_FREQ_8KHZ, WS_FREQ_11KHZ, WS_FREQ_16KHZ };

static void video_init(void)
{
	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
	             rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
}

/* The Wii Speak needs /dev/usb/oh0 (USBV0), which the modern USBV5 IOSes the
 * Homebrew Channel runs (57/58/61) do not expose -- and their /dev/usb/ven
 * only answers standard device-recipient control reads, so the mic's vendor
 * register protocol cannot work there.  Reload to an oh0-capable IOS.
 *
 * Must run before WPAD and FAT are brought up, since reloading IOS tears down
 * whatever they opened.  IOS 56 is the Wii Speak Channel's own IOS and the
 * first choice; 36 is present on essentially every console as a fallback. */
static void ensure_usb_ios(void)
{
	static const int candidates[] = { 56, 55, 53, 38, 37, 36 };
	s32 fd;
	unsigned i;

	fd = IOS_Open("/dev/usb/oh0", 0);
	if (fd >= 0) {
		IOS_Close(fd);
		printf("USB: oh0 present under IOS%d\n", (int)IOS_GetVersion());
		return;
	}

	printf("USB: IOS%d has no oh0; reloading...\n", (int)IOS_GetVersion());

	for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		if (IOS_ReloadIOS(candidates[i]) < 0)
			continue;

		fd = IOS_Open("/dev/usb/oh0", 0);
		if (fd >= 0) {
			IOS_Close(fd);
			printf("USB: now on IOS%d (oh0 present)\n",
			       (int)IOS_GetVersion());
			return;
		}
	}

	printf("USB: no oh0-capable IOS found; staying on IOS%d\n",
	       (int)IOS_GetVersion());
}

static void wait_for_home(void)
{
	printf("\nPress HOME to exit.\n");
	while (1) {
		INPUT_Update();
		if (INPUT_ButtonsDown() & WPAD_BUTTON_HOME)
			return;
		VIDEO_WaitVSync();
	}
}

/* Returns the firmware and its size.  *owned says whether the caller has to
 * free it, since the embedded copy lives in the binary. */
static const u8 *load_firmware(u32 *size_out, bool *owned)
{
	*owned = false;

#ifdef HAVE_EMBEDDED_FIRMWARE
	printf("Firmware: built in (%u bytes)\n", (unsigned)pmic_firm_bin_size);
	*size_out = (u32)pmic_firm_bin_size;
	return (const u8 *)pmic_firm_bin;
#else
	unsigned i;

	for (i = 0; i < sizeof(FIRMWARE_PATHS) / sizeof(FIRMWARE_PATHS[0]); i++) {
		FILE *fp = fopen(FIRMWARE_PATHS[i], "rb");
		long size;
		u8 *buf;

		if (!fp)
			continue;

		fseek(fp, 0, SEEK_END);
		size = ftell(fp);
		fseek(fp, 0, SEEK_SET);

		if (size <= 0) {
			fclose(fp);
			continue;
		}

		buf = malloc((size_t)size);
		if (!buf) {
			fclose(fp);
			return NULL;
		}

		if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
			free(buf);
			fclose(fp);
			continue;
		}

		fclose(fp);
		printf("Firmware: %s (%ld bytes)\n", FIRMWARE_PATHS[i], size);
		*size_out = (u32)size;
		*owned = true;
		return buf;
	}

	return NULL;
#endif
}

/* Names a recording rec_YYYYMMDD_HHMMSS.wav from the Wii clock, disambiguating
 * with a counter if two land in the same second (or the clock looks unset). */
static void next_filename(char *out, size_t len)
{
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	char stamp[20];
	int n;

	if (tm == NULL || tm->tm_year < 100) {
		/* Clock not set: fall back to a plain counter. */
		for (n = 1; n < 10000; n++) {
			struct stat st;
			snprintf(out, len, REC_DIR "/rec_%04d.wav", n);
			if (stat(out, &st) != 0)
				return;
		}
		snprintf(out, len, REC_DIR "/rec_overflow.wav");
		return;
	}

	strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", tm);

	snprintf(out, len, REC_DIR "/rec_%s.wav", stamp);
	for (n = 1; n < 100; n++) {
		struct stat st;
		if (stat(out, &st) != 0)
			return;
		snprintf(out, len, REC_DIR "/rec_%s_%d.wav", stamp, n);
	}
}

/* Peak level of a block, as a 0..16 meter position. */
static int meter_level(const s16 *samples, u32 count)
{
	s32 peak = 0;
	u32 i;
	int bar;

	for (i = 0; i < count; i++) {
		s32 v = samples[i] < 0 ? -(s32)samples[i] : samples[i];
		if (v > peak)
			peak = v;
	}

	for (bar = 0; bar < 16; bar++) {
		if (peak < (1 << (bar + 1)) * 2)
			break;
	}

	return bar;
}

static void draw_meter(int level)
{
	int i;

	printf("  [");
	for (i = 0; i < 16; i++)
		putchar(i < level ? '>' : '.');
	printf("]");
}

int main(int argc, char **argv)
{
	const u8 *firmware;
	u32 firmware_size = 0;
	bool firmware_owned = false;
	s16 *pump;
	wav_t wav;
	bool recording = false;
	bool sp_on = false;
	int gain_idx = 2;  /* 30 dB */
	int freq_idx = 2;  /* 16 kHz */
	int level = 0;
	u32 captured = 0;
	char path[64] = "";
	s32 ret;

	(void)argc;
	(void)argv;

	video_init();

	printf("\x1b[2;0H");
	printf("Wii Speak Recorder\n");
	printf("==================\n\n");

	/* Before WPAD/FAT: an IOS reload here would tear them down. */
	ensure_usb_ios();

	/* Buttons only: no IR pointer and no mouse. */
	INPUT_InitEx(INPUT_INIT_NO_POINTER | INPUT_INIT_NO_MOUSE);

	if (!fatInitDefault()) {
		printf("Could not mount the SD card.\n");
		wait_for_home();
		return 1;
	}

	mkdir(REC_DIR, 0777);

	if (!PB_Init())
		printf("Audio init failed; playback disabled.\n");

	firmware = load_firmware(&firmware_size, &firmware_owned);
	if (!firmware) {
		printf("Missing pmic_firm.bin.\n\n");
		printf("The Wii Speak boots from RAM and will not record until\n");
		printf("the PMIC firmware is uploaded to it. Extract it from an\n");
		printf("RVL SDK pmic.a with tools/extract_pmic_firmware.py and\n");
		printf("copy the result to:\n  %s\n", FIRMWARE_PATHS[0]);
		printf("\nOr build it in with: make firmware SDK=<pmic.a>\n");
		wait_for_home();
		return 1;
	}

	pump = memalign(32, PUMP_SAMPLES * sizeof(s16));
	if (!pump) {
		printf("Out of memory.\n");
		wait_for_home();
		return 1;
	}

	printf("Opening the Wii Speak...\n");
	ret = WS_Open(firmware, firmware_size);
	if (firmware_owned)
		free((void *)firmware);

	if (ret < 0) {
		if (ret == WS_ENODEV) {
			printf("No Wii Speak found.\n\n");
			WS_Diagnose();
			printf("\nIf the mic is not listed, try the other USB port.\n");
		}
		else if (ret == WS_EFIRMWARE)
			printf("The mic did not accept the firmware (it never\n"
			       "reported ready). Is pmic_firm.bin intact?\n");
		else
			printf("WS_Open failed: %d\n", (int)ret);
		wait_for_home();
		return 1;
	}

	printf("cfg gain...\n");   WS_SetGain(gain_idx);
	printf("cfg freq...\n");   WS_SetFreq(FREQS[freq_idx]);
	printf("cfg sp...\n");     WS_SetSignalProcessing(sp_on);
	printf("cfg mute...\n");   WS_SetMute(false);
	printf("starting...\n");

	ret = WS_Start();
	if (ret < 0) {
		printf("WS_Start failed: %d\n", (int)ret);
		WS_Close();
		wait_for_home();
		return 1;
	}

	while (1) {
		u32 got, held;
		u32 pressed;

		INPUT_Update();
		pressed = INPUT_ButtonsDown();

		if (pressed & WPAD_BUTTON_HOME)
			break;

		if (pressed & WPAD_BUTTON_A) {
			if (!recording) {
				PB_Stop();
				next_filename(path, sizeof(path));
				if (WAV_Open(&wav, path, WS_FreqToHz(FREQS[freq_idx])))
					recording = true;
			} else {
				WAV_Close(&wav);
				recording = false;
			}
		}

		/* 1 plays back the take just recorded, 2 stops it. */
		if ((pressed & WPAD_BUTTON_1) && !recording && path[0]) {
			if (!PB_Start(path))
				printf("\x1b[16;0HCould not play %-30s\n", path);
		}
		if (pressed & WPAD_BUTTON_2)
			PB_Stop();

		PB_Pump();

		/* Settings only apply between recordings so a file keeps one
		 * sample rate throughout. */
		if (!recording) {
			if (pressed & WPAD_BUTTON_RIGHT) {
				if (freq_idx < 2) {
					freq_idx++;
					WS_SetFreq(FREQS[freq_idx]);
				}
			}
			if (pressed & WPAD_BUTTON_LEFT) {
				if (freq_idx > 0) {
					freq_idx--;
					WS_SetFreq(FREQS[freq_idx]);
				}
			}
			if (pressed & WPAD_BUTTON_UP) {
				if (gain_idx < 3) {
					gain_idx++;
					WS_SetGain(gain_idx);
				}
			}
			if (pressed & WPAD_BUTTON_DOWN) {
				if (gain_idx > 0) {
					gain_idx--;
					WS_SetGain(gain_idx);
				}
			}
			if (pressed & WPAD_BUTTON_B) {
				sp_on = !sp_on;
				WS_SetSignalProcessing(sp_on);
			}
		}

		/* Drain the mic every frame, whether or not we are recording, so
		 * the ring buffer never backs up. */
		held = WS_Available();
		while (held > 0) {
			got = WS_Read(pump, PUMP_SAMPLES);
			if (got == 0)
				break;

			level = meter_level(pump, got);
			captured += got;

			if (recording && !WAV_Write(&wav, pump, got)) {
				WAV_Close(&wav);
				recording = false;
			}

			held = held > got ? held - got : 0;
		}

		printf("\x1b[5;0H");
		printf("Rate  : %5u Hz   %s\n", (unsigned)WS_FreqToHz(FREQS[freq_idx]),
		       recording ? "" : "(LEFT/RIGHT)   ");
		printf("Gain  : %5u dB   %s\n", GAIN_DB[gain_idx],
		       recording ? "" : "(UP/DOWN)      ");
		printf("Echo  : %-5s      %s\n", sp_on ? "on" : "off",
		       recording ? "" : "(B toggles)    ");
		printf("\n");
		printf("Level :");
		draw_meter(level);
		printf("\n\n");

		if (recording) {
			u32 secs = wav.samples / WS_FreqToHz(FREQS[freq_idx]);
			printf("RECORDING  %02u:%02u  %-28s\n",
			       (unsigned)(secs / 60), (unsigned)(secs % 60), path);
		} else if (PB_IsPlaying()) {
			u32 at = PB_ElapsedSecs(), of = PB_TotalSecs();
			printf("PLAYING    %02u:%02u / %02u:%02u  %-20s\n",
			       (unsigned)(at / 60), (unsigned)(at % 60),
			       (unsigned)(of / 60), (unsigned)(of % 60), path);
		} else if (path[0]) {
			printf("Saved %-40s\n", path);
		} else {
			printf("%-46s\n", "Press A to start recording.");
		}

		printf("Captured: %u  dropped: %u%-14s\n",
		       (unsigned)captured, (unsigned)WS_Dropped(), "");
		printf("\nA: record/stop   1: play  2: stop   HOME: exit\n");

		VIDEO_WaitVSync();
	}

	if (recording)
		WAV_Close(&wav);

	PB_Shutdown();
	WS_Stop();
	WS_Close();
	free(pump);

	printf("\nDone.\n");
	return 0;
}
