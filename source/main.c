/* RecordMii -- records the Wii Speak, or a USB microphone, to WAV files on the
 * SD card. */

#include <asndlib.h>
#include <fat.h>
#include <gccore.h>
#include <malloc.h>
#include <math.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/ios.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wiiuse/wpad.h>
#include <input_wii.h>
#include <wiiupdate.h>

#include "escape.h"
#include "netdbg.h"
#include "playback.h"
#include "wav.h"
#include <input_usbmic.h>
#include <input_wiispeak.h>

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

/* App state, at file scope so the debug console can drive it. */
static wav_t wav;
static bool recording;
static bool sp_on;
static bool muted;
static bool audio_ok;
static bool quit;
static bool sd_ok;
static int gain_idx = 2;  /* 30 dB */
static int freq_idx = 2;  /* 16 kHz */
static char path[64];

/* A class-compliant USB mic, used instead of the Wii Speak when one is
 * plugged in. */
static bool usb_mic;
static um_info umic;
static int vol_pct;

/* ---- whichever mic is in use --------------------------------------------- */

static u32 mic_rate(void)
{
	return usb_mic ? umic.rate : WS_FreqToHz(FREQS[freq_idx]);
}

static u32 mic_available(void)
{
	return usb_mic ? UM_Available() : WS_Available();
}

static u32 mic_read(s16 *dst, u32 max)
{
	return usb_mic ? UM_Read(dst, max) : WS_Read(dst, max);
}

static u32 mic_dropped(void)
{
	return usb_mic ? UM_Dropped() : WS_Dropped();
}

static void mic_quiesce(void)
{
	UM_Quiesce();
	WS_Quiesce();
}

static s32 set_volume(int pct)
{
	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	vol_pct = pct;
	return UM_SetVolume((u32)pct);
}

/* ---- test tone ------------------------------------------------------------ */

/* One second at 48 kHz holds a whole number of periods of any integer
 * frequency, so it loops without a click. */
#define TONE_RATE 48000

static s16 *tone_buf;
static s32 tone_voice = -1;
static u64 tone_end;

static void tone_stop(void)
{
	if (tone_voice >= 0) {
		ASND_StopVoice(tone_voice);
		tone_voice = -1;
	}
}

static bool tone_start(u32 hz, u32 ms, u32 vol)
{
	u32 i;

	if (!audio_ok || hz == 0 || hz >= TONE_RATE / 2)
		return false;

	if (!tone_buf) {
		tone_buf = memalign(32, TONE_RATE * sizeof(s16));
		if (!tone_buf)
			return false;
	}

	tone_stop();

	for (i = 0; i < TONE_RATE; i++)
		tone_buf[i] = (s16)(sinf(2.0f * (float)M_PI * (float)((u64)hz * i % TONE_RATE)
		                         / TONE_RATE) * 30000.0f);
	DCFlushRange(tone_buf, TONE_RATE * sizeof(s16));

	tone_voice = ASND_GetFirstUnusedVoice();
	if (tone_voice == SND_INVALID) {
		tone_voice = -1;
		return false;
	}

	if (vol > 255)
		vol = 255;
	if (ASND_SetInfiniteVoice(tone_voice, VOICE_MONO_16BIT, TONE_RATE, 0,
	                          tone_buf, TONE_RATE * sizeof(s16),
	                          (s32)vol, (s32)vol) != SND_OK) {
		tone_voice = -1;
		return false;
	}

	tone_end = gettime() + millisecs_to_ticks(ms);
	return true;
}

static void tone_pump(void)
{
	if (tone_voice >= 0 && gettime() >= tone_end)
		tone_stop();
}

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
static void ensure_usb_ios(int first)
{
	static int candidates[] = { 0, 56, 55, 53, 38, 37, 36 };
	s32 fd;
	unsigned i;

	candidates[0] = first;

	fd = IOS_Open("/dev/usb/oh0", 0);
	if (fd >= 0 && (first == 0 || first == IOS_GetVersion())) {
		IOS_Close(fd);
		printf("USB: oh0 present under IOS%d\n", (int)IOS_GetVersion());
		return;
	}
	if (fd >= 0)
		IOS_Close(fd);

	printf("USB: IOS%d has no oh0; reloading...\n", (int)IOS_GetVersion());

	for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		if (candidates[i] == 0 || IOS_ReloadIOS(candidates[i]) < 0)
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

/* Only `exit` makes sense once startup has failed. */
static bool fail_command(const char *line, char *reply, size_t size)
{
	if (!strcmp(line, "exit")) {
		quit = true;
		snprintf(reply, size, "exiting\n");
		return true;
	}
	snprintf(reply, size, "startup failed; only `exit` works");
	return false;
}

static void __attribute__((noreturn)) wait_for_home(void)
{
	printf("\nPress HOME to exit.\n");
	while (!quit) {
		INPUT_Update();
		if (INPUT_ButtonsDown() & WPAD_BUTTON_HOME)
			break;
		NDBG_Poll(fail_command);
		VIDEO_WaitVSync();
	}
	ESC_Now();
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

/* Before the main loop there is nothing worth tidying, so HOME leaves at
 * once wherever startup happens to be. */
static bool startup_home(void)
{
	INPUT_Update();
	if (INPUT_ButtonsDown() & WPAD_BUTTON_HOME)
		ESC_Request();
	return false;
}

static u32 update_buttons(void)
{
	u32 down;

	INPUT_Update();
	down = INPUT_ButtonsDown();
	if (down & WPAD_BUTTON_HOME)
		ESC_Request();
	return down;
}

static void start_recording(void)
{
	if (!sd_ok)
		return;
	PB_Stop();
	next_filename(path, sizeof(path));
	if (WAV_Open(&wav, path, mic_rate()))
		recording = true;
}

static void stop_recording(void)
{
	WAV_Close(&wav);
	recording = false;
}

static int freq_index(u32 khz)
{
	switch (khz) {
	case 8:  return 0;
	case 11: return 1;
	case 16: return 2;
	default: return -1;
	}
}

/* Main-loop half of the debug console; see netdbg.h. */
static bool debug_command(const char *line, char *reply, size_t size)
{
	char cmd[16] = "", arg[64] = "";
	unsigned a = 0, b = 0, c = 255;

	sscanf(line, "%15s %63[^\n]", cmd, arg);

	if (!strcmp(cmd, "status") && usb_mic) {
		snprintf(reply, size,
		         "usb mic %s (%04x:%04x)\n"
		         "rate %u, %u ch %u-bit, if %u alt %u ep %02x pkt %u\n"
		         "volume %s %d%% mute %d\n"
		         "recording %d path %s\n"
		         "playing %d tone %d\n",
		         umic.name, umic.vid, umic.pid, (unsigned)umic.rate,
		         umic.channels, umic.bits, umic.interface, umic.alt,
		         umic.endpoint, umic.max_packet,
		         umic.has_volume ? "yes" : "no", vol_pct, muted,
		         recording, path[0] ? path : "-",
		         PB_IsPlaying(), tone_voice >= 0);
		return true;
	}

	if (!strcmp(cmd, "status")) {
		u16 lvl = 0, fq = 0;
		s32 r1 = WS_GetSendLevel(&lvl);
		s32 r2 = WS_GetFreq(&fq);

		snprintf(reply, size,
		         "rate %u gain %u dB sp %d mute %d\n"
		         "recording %d path %s\n"
		         "playing %d tone %d\n"
		         "mic freq reg %04x (ret %d) send level %u (ret %d)\n",
		         (unsigned)WS_FreqToHz(FREQS[freq_idx]), GAIN_DB[gain_idx],
		         sp_on, muted, recording, path[0] ? path : "-",
		         PB_IsPlaying(), tone_voice >= 0,
		         fq, (int)r2, lvl, (int)r1);
		return true;
	}

	if (!strcmp(cmd, "exit")) {
		quit = true;
		snprintf(reply, size, "exiting\n");
		return true;
	}

	if (!strcmp(cmd, "tone")) {
		if (!strcmp(arg, "off")) {
			tone_stop();
			snprintf(reply, size, "tone off\n");
			return true;
		}
		if (sscanf(arg, "%u %u %u", &a, &b, &c) < 2) {
			snprintf(reply, size, "usage: tone <hz> <ms> [vol] | tone off");
			return false;
		}
		if (!tone_start(a, b, c)) {
			snprintf(reply, size, "could not start the tone");
			return false;
		}
		snprintf(reply, size, "tone %u Hz for %u ms at %u\n", a, b,
		         c > 255 ? 255 : c);
		return true;
	}

	if (!strcmp(cmd, "rec")) {
		if (!strcmp(arg, "start") && !recording)
			start_recording();
		else if (!strcmp(arg, "stop") && recording)
			stop_recording();
		snprintf(reply, size, "recording %d %s\n", recording, path);
		return true;
	}

	if (!strcmp(cmd, "play")) {
		const char *p = arg[0] ? arg : path;
		if (recording || !p[0] || !PB_Start(p)) {
			snprintf(reply, size, "cannot play %s", p[0] ? p : "(nothing)");
			return false;
		}
		snprintf(reply, size, "playing %s\n", p);
		return true;
	}

	if (!strcmp(cmd, "stop")) {
		PB_Stop();
		tone_stop();
		snprintf(reply, size, "stopped\n");
		return true;
	}

	/* A USB mic has a volume control and maybe a mute, nothing else. */
	if (usb_mic && (!strcmp(cmd, "vol") || !strcmp(cmd, "mute"))) {
		s32 ret;

		a = (unsigned)strtoul(arg, NULL, 10);
		if (!strcmp(cmd, "vol")) {
			ret = set_volume((int)a);
		} else {
			muted = a != 0;
			ret = UM_SetMute(muted);
		}
		snprintf(reply, size, "%s %u -> %d\n", cmd, a, (int)ret);
		return ret >= 0;
	}
	if (usb_mic && (!strcmp(cmd, "gain") || !strcmp(cmd, "freq") ||
	                !strcmp(cmd, "sp"))) {
		snprintf(reply, size, "`%s` is for the Wii Speak; a USB mic has vol", cmd);
		return false;
	}

	/* Settings, refused mid-recording like their buttons. */
	if (!strcmp(cmd, "gain") || !strcmp(cmd, "freq") ||
	    !strcmp(cmd, "sp") || !strcmp(cmd, "mute")) {
		s32 ret;

		a = (unsigned)strtoul(arg, NULL, 10);
		if (recording) {
			snprintf(reply, size, "stop recording first");
			return false;
		}

		if (!strcmp(cmd, "gain")) {
			if (a > 3) {
				snprintf(reply, size, "gain is 0-3");
				return false;
			}
			gain_idx = (int)a;
			ret = WS_SetGain((u16)a);
		} else if (!strcmp(cmd, "freq")) {
			int i = freq_index(a);
			if (i < 0) {
				snprintf(reply, size, "freq is 8, 11 or 16");
				return false;
			}
			freq_idx = i;
			ret = WS_SetFreq(FREQS[i]);
		} else if (!strcmp(cmd, "sp")) {
			sp_on = a != 0;
			ret = WS_SetSignalProcessing(sp_on);
		} else {
			muted = a != 0;
			ret = WS_SetMute(muted);
		}

		snprintf(reply, size, "%s %u -> %d\n", cmd, a, (int)ret);
		return ret >= 0;
	}

	snprintf(reply, size, "unknown command `%s` (try help)", cmd);
	return false;
}

/* Boots the Wii Speak and starts it streaming, or stays on an error screen. */
static void open_wii_speak(void)
{
	const u8 *firmware;
	u32 firmware_size = 0;
	bool firmware_owned = false;
	s32 ret;

	firmware = load_firmware(&firmware_size, &firmware_owned);
	if (!firmware) {
		printf("Missing pmic_firm.bin.\n\n");
		printf("The Wii Speak boots from RAM and will not record until\n");
		printf("the PMIC firmware is uploaded to it. Extract it from an\n");
		printf("RVL SDK pmic.a with tools/extract_pmic_firmware.py and\n");
		printf("copy the result to:\n  %s\n", FIRMWARE_PATHS[0]);
		printf("\nOr build it in with: make firmware SDK=<pmic.a>\n");
		printf("\nA class-compliant USB mic needs none of this:\n"
		       "plug one in and start RecordMii again.\n");
		wait_for_home();
	}

	printf("Opening the Wii Speak... (HOME or RESET exits)\n");
	ret = WS_Open(firmware, firmware_size);
	if (firmware_owned)
		free((void *)firmware);

	if (ret < 0) {
		if (ret == WS_ENODEV) {
			printf("No microphone found: no USB mic and no Wii Speak.\n\n");
			WS_Diagnose();
			printf("\nIf the mic is not listed, try the other USB port.\n");
		}
		else if (ret == WS_ECANCEL)
			ESC_Now();
		else if (ret == WS_EFIRMWARE)
			printf("The mic did not accept the firmware (it never\n"
			       "reported ready). Is pmic_firm.bin intact?\n");
		else
			printf("WS_Open failed: %d\n", (int)ret);
		wait_for_home();
	}

	printf("cfg gain %d\n", (int)WS_SetGain(gain_idx));
	printf("cfg freq %d\n", (int)WS_SetFreq(FREQS[freq_idx]));
	printf("cfg sp %d\n", (int)WS_SetSignalProcessing(sp_on));
	printf("cfg mute %d\n", (int)WS_SetMute(false));
	printf("starting...\n");

	ret = WS_Start();
	if (ret < 0) {
		printf("WS_Start failed: %d\n", (int)ret);
		WS_Close();
		wait_for_home();
	}
}

int main(int argc, char **argv)
{
	s16 *pump;
	int level = 0;
	u32 captured = 0;
	s32 ret;

	video_init();
	ESC_Init();
	ESC_SetQuiesce(mic_quiesce);
#ifdef DEADMAN_MS
	/* Unattended test builds: leave unless the debug client keeps talking. */
	ESC_SetDeadman(DEADMAN_MS);
#endif
	NDBG_HookStdout();

	printf("\x1b[2;0H");
	printf("RecordMii\n");
	printf("=========\n\n");

	/* Experiment knobs, passed as wiiload arguments: ios=NN slots=1|2
	 * out=0|1. */
	{
		int i, ios = 0, slots = 2, out = 1;

		for (i = 1; i < argc; i++) {
			sscanf(argv[i], "ios=%d", &ios);
			sscanf(argv[i], "slots=%d", &slots);
			sscanf(argv[i], "out=%d", &out);
		}
		if (argc > 1)
			printf("Args: ios=%d slots=%d out=%d\n", ios, slots, out);
		WS_SetStreams(slots, out != 0);

		/* Before WPAD/FAT: an IOS reload here would tear them down. */
		ensure_usb_ios(ios);
	}

	/* Buttons only: no IR pointer and no mouse. */
	INPUT_InitEx(INPUT_INIT_NO_POINTER | INPUT_INIT_NO_MOUSE);
	ESC_WpadReady();
	WS_SetLog(printf);
	WS_SetCancel(startup_home);
	UM_SetLog(printf);

	/* Without a card the mic still works -- the debug console streams it --
	 * there is just nowhere to save takes. */
	sd_ok = fatInitDefault();
	if (sd_ok)
		mkdir(REC_DIR, 0777);
	else
		printf("No SD card: recording to file is off.\n");

	/* One update check at boot: B skips it, failures are silent.  An
	 * update installs to the card, so skip it without one. */
	if (sd_ok) {
		WIIUPDATE_SetAppPath(argc > 0 ? argv[0] : NULL);
		if (WIIUPDATE_CheckConsole("quatric", "RecordMii", APP_VERSION, update_buttons) == WIIUPDATE_UPDATED) {
			INPUT_Shutdown();
			ESC_Now();
		}
	}
	printf("\n");

	/* After the IOS reload and the update check, which may already have
	 * brought the network up. */
	NDBG_Start();

	audio_ok = PB_Init();
	if (!audio_ok)
		printf("Audio init failed; playback disabled.\n");

	pump = memalign(32, PUMP_SAMPLES * sizeof(s16));
	if (!pump) {
		printf("Out of memory.\n");
		wait_for_home();
	}

	/* A USB mic needs no firmware, so it is looked for first. */
	printf("Looking for a USB microphone...\n");
	ret = UM_Open(&umic);
	if (ret == UM_OK) {
		usb_mic = true;
		vol_pct = 100;
		if (umic.has_volume && umic.vol_max > umic.vol_min)
			vol_pct = (int)(((s32)umic.vol_cur - umic.vol_min) * 100 /
			                ((s32)umic.vol_max - umic.vol_min));
		printf("USB mic: %s, %u Hz\n", umic.name, (unsigned)umic.rate);

		ret = UM_Start();
		if (ret < 0) {
			printf("UM_Start failed: %d\n", (int)ret);
			UM_Close();
			wait_for_home();
		}
	} else {
		if (ret != UM_ENODEV)
			printf("USB mic failed: %d\n", (int)ret);
		else
			printf("None found; trying the Wii Speak.\n");
		open_wii_speak();
	}

	/* Clear the boot log off the screen; the debug console keeps it. */
	printf("\x1b[2J");

	while (!quit) {
		u32 got, held;
		u32 pressed;

		INPUT_Update();
		pressed = INPUT_ButtonsDown();

		if (pressed & WPAD_BUTTON_HOME)
			break;

		NDBG_Poll(debug_command);
		tone_pump();

		if (pressed & WPAD_BUTTON_A) {
			if (!recording)
				start_recording();
			else
				stop_recording();
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
		if (!recording && usb_mic) {
			if ((pressed & WPAD_BUTTON_UP) && umic.has_volume)
				set_volume(vol_pct + 10 - vol_pct % 10);
			if ((pressed & WPAD_BUTTON_DOWN) && umic.has_volume)
				set_volume(vol_pct % 10 ? vol_pct - vol_pct % 10 : vol_pct - 10);
		} else if (!recording) {
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
		held = mic_available();
		while (held > 0) {
			got = mic_read(pump, PUMP_SAMPLES);
			if (got == 0)
				break;

			level = meter_level(pump, got);
			captured += got;
			NDBG_FeedPcm(pump, got, mic_rate());

			if (recording && !WAV_Write(&wav, pump, got))
				stop_recording();

			held = held > got ? held - got : 0;
		}

		NDBG_Quiet(true);
		printf("\x1b[2;0H");
		printf("RecordMii\n");
		printf("=========\n\n");
		if (usb_mic) {
			printf("Mic   : %-38.38s\n", umic.name);
			printf("Rate  : %5u Hz                \n", (unsigned)umic.rate);
			if (umic.has_volume)
				printf("Volume: %5d %%    %-15s\n", vol_pct,
				       recording ? "" : "(UP/DOWN)");
			else
				printf("Volume: fixed                 \n");
		} else {
			printf("Rate  : %5u Hz   %s\n", (unsigned)WS_FreqToHz(FREQS[freq_idx]),
			       recording ? "" : "(LEFT/RIGHT)   ");
			printf("Gain  : %5u dB   %s\n", GAIN_DB[gain_idx],
			       recording ? "" : "(UP/DOWN)      ");
			printf("Echo  : %-5s      %s\n", sp_on ? "on" : "off",
			       recording ? "" : "(B toggles)    ");
		}
		printf("\n");
		printf("Level :");
		draw_meter(level);
		printf("\n\n");

		if (recording) {
			u32 secs = wav.samples / mic_rate();
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
			printf("%-46s\n", sd_ok ? "Press A to start recording."
			                         : "No SD card: nothing to record to.");
		}

		printf("Captured: %u  dropped: %u%-14s\n",
		       (unsigned)captured, (unsigned)mic_dropped(), "");
		printf("Debug : %-22s%-8s\n",
		       NDBG_Address() ? NDBG_Address() : "no network",
		       NDBG_GeckoActive() ? " +gecko" : "");
		printf("\nA: record/stop   1: play  2: stop   HOME: exit\n");
		NDBG_Quiet(false);

		VIDEO_WaitVSync();
	}

	/* Each step reports itself, on screen and to the debug log, so a hang
	 * on the way out shows exactly where -- and the watchdog leaves anyway
	 * if it takes too long. */
	ESC_Arm(5000);
	printf("\x1b[2J\x1b[2;0HExiting... (RESET forces it)\n");

	if (recording) {
		stop_recording();
		printf("exit: recording closed\n");
	}

	tone_stop();
	PB_Shutdown();
	printf("exit: audio down\n");

	if (usb_mic)
		UM_Close();
	else
		WS_Close();
	printf("exit: mic closed\n");
	free(pump);

	printf("exit: input...\n");
	INPUT_Shutdown();
	printf("exit: input down\n");

	if (sd_ok) {
		fatUnmount("sd:");
		printf("exit: sd unmounted\n");
	}

	NDBG_Stop();
	printf("exit: network down, returning to loader\n");

	/* Not `return`: libogc's exit stops the thread system first, watchdog
	 * included, so if that hung nothing could rescue it. */
	ESC_Now();
}
