/* Wii Speak (P-Mic) driver for libogc homebrew.  See wiispeak.h. */

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/cache.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/usb.h>

#include "wiispeak.h"

/* ---- vendor protocol ------------------------------------------------------
 *
 * Recovered from the RVL SDK's pmic.a (pmic_core.o and friends) and confirmed
 * against Dolphin's emulated device.
 *
 * Every setting is a 10-byte command written to the device and then read back
 * through the same buffer:
 *
 *     [0..1] opcode  0x9A00 | reg   (bit 0 of reg set means "read")
 *     [2..3] arg1
 *     [4..5] arg2
 *     [6..7] arg3
 *     [8..9] sequence number
 *
 * Reads return their result in arg1, except SP_STATE which answers in arg2.
 */
#define REQTYPE_VENDOR_OUT 0x41
#define REQTYPE_VENDOR_IN  0xC1

#define REQ_RESET   0 /* out, no data: puts the mic back in "not booted" */
#define REQ_CMD_PUT 1 /* out, 10 bytes: submit a command */
#define REQ_CMD_GET 2 /* in,  10 bytes: fetch the response */
#define REQ_STATUS  6 /* in,  1 byte:   0 = awaiting firmware, 1 = booted */

#define CMD_MAGIC 0x9A00

#define REG_SAMPLER_STATE 0x00
#define REG_SAMPLER_FREQ  0x02
#define REG_SAMPLER_GAIN  0x04
#define REG_SAMPLER_MUTE  0x0C
#define REG_EC_STATE      0x14
#define REG_SP_STATE      0x38
#define REG_READ          0x01 /* or'd into the register number */

/* arg1 values for REG_SP_STATE */
#define SP_ENABLE 0x1010
#define SP_SIN    0x2001
#define SP_SOUT   0x2004
#define SP_RIN    0x200D

#define CMD_LEN 10

/* ---- endpoints ------------------------------------------------------------
 *
 * Interface 0 alternate setting 1 exposes all three.  The SDK runs the
 * isochronous transfers double-buffered: 8x32 bytes in (128 mono samples, one
 * 8 ms frame) and 8x64 bytes out (echo-canceller reference audio).
 */
#define EP_AUDIO_IN  0x81
#define EP_DATA_OUT  0x02
#define EP_AUDIO_OUT 0x03

#define ISO_IN_PACKETS   8
#define ISO_IN_PKTSIZE   32
#define ISO_IN_BYTES     (ISO_IN_PACKETS * ISO_IN_PKTSIZE)

#define ISO_OUT_PACKETS  8
#define ISO_OUT_PKTSIZE  64
#define ISO_OUT_BYTES    (ISO_OUT_PACKETS * ISO_OUT_PKTSIZE)

/* Transfers kept in flight per direction, as the SDK does.  With only one,
 * every frame that passes while it is being resubmitted is lost. */
#define ISO_SLOTS 2

/* Fills IN buffers before each transfer, to tell received data from none. */
#define ISO_POISON 0xEE

#define FIRMWARE_CHUNK 1024
#define STATUS_RETRIES 5 /* advisory only; see ws_upload_firmware */

/* ~8 seconds of headroom at 16 kHz. */
#define RING_SAMPLES (1u << 17)
#define RING_MASK    (RING_SAMPLES - 1)

#define STACKSIZE 16384

/* On USBV5 the device fd handed back by USB_OpenDevice is really a device_id,
 * which can have its top bit set and so read as negative -- yet it is a valid
 * handle (control transfers route through the global ven_host, keyed by this
 * id in the payload).  So the fd's sign says nothing about whether we are
 * open; track that separately. */
static s32 ws_fd = -1;
static bool ws_opened = false;
static volatile bool ws_running = false;

static u8 *ws_cmd;      /* 10-byte command/response buffer, DMA aligned */
static u8 *ws_status;   /* 1-byte status buffer */
static u8 *ws_chunk;    /* firmware staging buffer */
static u8 *ws_in_buf[ISO_SLOTS];   /* isochronous IN landing buffers */
static u8 *ws_out_buf[ISO_SLOTS];  /* isochronous OUT (silence) */
static u16 *ws_in_sizes[ISO_SLOTS];
static u16 *ws_out_sizes[ISO_SLOTS];

static s16 *ws_ring;
static volatile u32 ws_ring_head; /* written by the reader thread */
static volatile u32 ws_ring_tail; /* written by the consumer */
static volatile u32 ws_drops;

static mutex_t ws_ring_lock = LWP_MUTEX_NULL;
static mutex_t ws_cmd_lock = LWP_MUTEX_NULL;

static lwp_t ws_in_thread = LWP_THREAD_NULL;
static lwp_t ws_out_thread = LWP_THREAD_NULL;
static u8 *ws_in_stack;
static u8 *ws_out_stack;

static u16 ws_seq;
static bool (*ws_cancel)(void);

/* Debug hooks and counters, see WS_GetStats() and WS_SetIsoTap(). */
static ws_iso_tap ws_tap;
static ws_stats ws_st;
static volatile int ws_layout = WS_LAYOUT_AUTO;
static volatile bool ws_swap;

/* One in-flight asynchronous transfer.  A transfer still pending when its
 * worker gives up keeps its DMA buffer alive, so WS_Close must not free it. */
typedef struct {
	volatile bool done;
	volatile bool pending; /* submitted and not yet collected */
	volatile s32 res;
} ws_xfer;

/* static: a late completion writes here */
static ws_xfer ws_in_x[ISO_SLOTS], ws_out_x[ISO_SLOTS];

/* Experiment knobs, see WS_SetStreams(). */
static int ws_in_slots = ISO_SLOTS;
static bool ws_out_on = true;

u32 WS_FreqToHz(u16 freq)
{
	switch (freq) {
	case WS_FREQ_8KHZ:  return 8000;
	case WS_FREQ_11KHZ: return 11025;
	default:            return 16000;
	}
}

/* ---- command layer ------------------------------------------------------ */

static void put16(u8 *p, u16 v)
{
	p[0] = (u8)(v >> 8);
	p[1] = (u8)v;
}

static u16 get16(const u8 *p)
{
	return (u16)((p[0] << 8) | p[1]);
}

/* A control transfer with a deadline.
 *
 * libogc's synchronous USB_*CtrlMsg calls never time out, so a single
 * transfer the device does not answer hangs the console with no way back.
 * The Wii Speak stops responding on its control endpoint for a moment after
 * it accepts firmware (it re-boots the on-board DSP), which is exactly when
 * the next command would otherwise wedge.  Issue the transfer asynchronously
 * and give up after `ms`; a late completion still lands in our persistent
 * buffer, harmlessly. */
static volatile s32 ws_async_res;
static volatile bool ws_async_done;

static s32 ws_async_cb(s32 result, void *arg)
{
	(void)arg;
	ws_async_res = result;
	ws_async_done = true;
	return 0;
}

static s32 ws_ctrl(u8 type, u8 req, u16 val, u16 idx, u16 len, void *data, int ms)
{
	s32 ret;
	int waited;

	ws_async_done = false;
	ws_async_res = -1;

	if (type & 0x80)
		ret = USB_ReadCtrlMsgAsync(ws_fd, type, req, val, idx, len, data,
		                           ws_async_cb, NULL);
	else
		ret = USB_WriteCtrlMsgAsync(ws_fd, type, req, val, idx, len, data,
		                            ws_async_cb, NULL);
	if (ret < 0)
		return ret;

	for (waited = 0; !ws_async_done && waited < ms; waited += 2)
		usleep(2 * 1000);

	return ws_async_done ? ws_async_res : WS_ETIMEOUT;
}

/* Submits one 10-byte command and reads its reply back into out1/out2,
 * either of which may be NULL. */
static s32 ws_command(u8 reg, u16 arg1, u16 arg2, u16 *out1, u16 *out2)
{
	s32 ret;

	if (!ws_opened)
		return WS_ESTATE;

	LWP_MutexLock(ws_cmd_lock);

	memset(ws_cmd, 0, CMD_LEN);
	put16(ws_cmd + 0, (u16)(CMD_MAGIC | reg));
	put16(ws_cmd + 2, arg1);
	put16(ws_cmd + 4, arg2);
	put16(ws_cmd + 8, ws_seq++);
	DCFlushRange(ws_cmd, 32);

	ret = ws_ctrl(REQTYPE_VENDOR_OUT, REQ_CMD_PUT, 0, 0, CMD_LEN, ws_cmd, 500);
	if (ret < 0)
		goto out;

	/* The SDK fetches the response to every command, sets included, and
	 * the mic may not take the next command until it has been. */
	ret = ws_ctrl(REQTYPE_VENDOR_IN, REQ_CMD_GET, 0, 0, CMD_LEN, ws_cmd, 500);
	DCInvalidateRange(ws_cmd, 32);
	if (ret < 0)
		goto out;

	if (out1)
		*out1 = get16(ws_cmd + 2);
	if (out2)
		*out2 = get16(ws_cmd + 4);
	ret = WS_OK;

out:
	LWP_MutexUnlock(ws_cmd_lock);
	return ret;
}

static s32 ws_set(u8 reg, u16 arg1, u16 arg2)
{
	return ws_command(reg, arg1, arg2, NULL, NULL);
}

s32 WS_RawCommand(u8 reg, u16 arg1, u16 arg2, u16 *out1, u16 *out2)
{
	return ws_command(reg, arg1, arg2, out1, out2);
}

/* ---- boot ---------------------------------------------------------------- */

/* Polls the status byte until it reports `want`, reporting what it actually
 * saw.  The SDK reads this once and treats anything unexpected as fatal; we
 * retry because a real device may need a moment, but the value it settles on
 * is the interesting part when this goes wrong. */
static s32 ws_wait_status(u8 want, const char *what)
{
	s32 last = -1;
	int i;

	s32 last_ret = 0;

	for (i = 0; i < STATUS_RETRIES; i++) {
		s32 ret;

		/* Not 0xFF: the device may genuinely answer 0xFF, and a sentinel
		 * that collides with a real value proves nothing. */
		memset(ws_status, 0xA5, 32);
		DCFlushRange(ws_status, 32);

		ret = ws_ctrl(REQTYPE_VENDOR_IN, REQ_STATUS, 0, 0, 1, ws_status, 200);
		DCInvalidateRange(ws_status, 32);

		if (ret < 0) {
			printf("  %s: read failed (%d)\n", what, (int)ret);
			return ret;
		}

		if (ws_status[0] == want) {
			printf("  %s: %u after %d poll(s)\n", what,
			       (unsigned)ws_status[0], i + 1);
			return WS_OK;
		}

		last = ws_status[0];
		last_ret = ret;
		usleep(20 * 1000);
	}

	/* ret is the byte count IOS reports; 0 here means the device answered
	 * nothing at all, which is a very different problem from a wrong value. */
	printf("  %s: wanted %u, got %d (xfer ret %d) after %d polls\n", what,
	       (unsigned)want, (int)last, (int)last_ret, STATUS_RETRIES);
	return WS_EFIRMWARE;
}

/* Compares a known-good standard control read against the vendor one, using a
 * sentinel that cannot be confused with a real answer.  If the standard read
 * fills the buffer and the vendor read does not, the buffer handling is fine
 * and the device is refusing the vendor request. */
static void ws_try(const char *label, u8 type, u8 req, u16 val, u16 idx, u16 len)
{
	s32 ret;

	memset(ws_status, 0xA5, 32);
	DCFlushRange(ws_status, 32);
	ret = USB_ReadCtrlMsg(ws_fd, type, req, val, idx, len, ws_status);
	DCInvalidateRange(ws_status, 32);

	printf("  %-11s ret %3d [%02x %02x]%s\n", label, (int)ret,
	       ws_status[0], ws_status[1],
	       ws_status[0] == 0xA5 ? " (no data)" : "");
}

static void ws_probe(void)
{
	/* Recipient=device reads are known to work, so they are the control.
	 * If none of the interface-recipient forms return data, the transport
	 * is refusing them rather than the device rejecting our command. */
	/* The one read that worked asked for 18 bytes; every read that came back
	 * empty asked for 1 or 8.  Vary length against a request known to work,
	 * to tell "IOS dislikes short transfers" apart from "IOS dislikes vendor
	 * or interface requests". */
	ws_try("desc  18", 0x80, USB_REQ_GETDESCRIPTOR, USB_DT_DEVICE << 8, 0, 18);
	ws_try("desc   1", 0x80, USB_REQ_GETDESCRIPTOR, USB_DT_DEVICE << 8, 0, 1);
	ws_try("desc   8", 0x80, USB_REQ_GETDESCRIPTOR, USB_DT_DEVICE << 8, 0, 8);
	ws_try("desc  32", 0x80, USB_REQ_GETDESCRIPTOR, USB_DT_DEVICE << 8, 0, 32);
	ws_try("vnd if 1", 0xC1, REQ_STATUS, 0, 0, 1);
	ws_try("vnd if18", 0xC1, REQ_STATUS, 0, 0, 18);
	ws_try("vnd if32", 0xC1, REQ_STATUS, 0, 0, 32);
	ws_try("if alt32", 0x81, USB_REQ_GETINTERFACE, 0, 0, 32);
}

/* The mic boots from RAM: it comes up bare and stays mute until the host
 * pushes PMIC_FIRM_DATA at it in 1 KB bulk chunks. */
static s32 ws_start_streams(void);
static s32 ws_select_alt1(const char *when);

static s32 ws_upload_firmware(const u8 *firmware, u32 size)
{
	u32 sent = 0;
	s32 ret;

	printf("  reset...\n");
	ret = ws_ctrl(REQTYPE_VENDOR_OUT, REQ_RESET, 0, 0, 0, NULL, 500);
	printf("  reset: %d\n", (int)ret);
	if (ret < 0 && ret != WS_ETIMEOUT)
		return ret;

	/* The status byte is advisory here, not a gate.
	 *
	 * It is the only single-byte read in the protocol, and on USBV5 a
	 * control read of exactly one byte never delivers its data -- the
	 * device answers (IOS reports the byte transferred) but it does not
	 * reach us.  Longer reads are fine, so the 10-byte register protocol
	 * is unaffected.  Poll once for the log, then proceed on timing and
	 * let the audio stream be the real test. */
	usleep(50 * 1000);
	(void)ws_wait_status(0, "status before upload");

	printf("  uploading %u bytes...\n", (unsigned)size);
	while (sent < size) {
		u32 n = size - sent;
		if (n > FIRMWARE_CHUNK)
			n = FIRMWARE_CHUNK;

		memcpy(ws_chunk, firmware + sent, n);
		ret = USB_WriteBlkMsg(ws_fd, EP_DATA_OUT, (u16)n, ws_chunk);
		if (ret < 0) {
			printf("  bulk write failed at %u/%u (%d)\n",
			       (unsigned)sent, (unsigned)size, (int)ret);
			return ret;
		}

		sent += n;
	}
	printf("  uploaded %u bytes\n", (unsigned)sent);

	/* Give the mic time to bring the firmware up before anything talks to
	 * it, since we cannot read the ready flag to find out. */
	usleep(300 * 1000);
	(void)ws_wait_status(1, "status after upload");

	/* The SDK starts the isochronous streams as soon as the mic reports
	 * ready, and only then sends commands: until the streams run, IOS
	 * fails every command with -22.  (The first build on hardware sent
	 * them first, so the sampler was never switched on; once the streams
	 * were up, the same commands went through.) */
	printf("  starting the streams\n");
	ret = ws_start_streams();
	if (ret < 0)
		return ret;

	{
		int ms;

		for (ms = 0; ms < 15000; ms += 100) {
			ret = ws_set(REG_SAMPLER_FREQ, WS_FREQ_16KHZ, 0);
			if (ret >= 0) {
				printf("  ready after %d ms\n", ms);
				return WS_OK;
			}
			if (ws_cancel && ws_cancel())
				return WS_ECANCEL;

			/* Fallback, logged so the console shows whether it helped. */
			if (ms == 3000)
				ws_select_alt1("retry");
			if (ms % 2000 == 1900)
				printf("  waiting for the mic... (%d)\n", (int)ret);
			usleep(100 * 1000);
		}
		printf("  device never accepted a command (%d)\n", (int)ret);
	}

	/* Carry on regardless, so the debug console can still poke at it. */
	return WS_OK;
}

/* ---- transfer threads ---------------------------------------------------- */

static void ws_push(const s16 *samples, u32 count)
{
	u32 i;

	LWP_MutexLock(ws_ring_lock);

	for (i = 0; i < count; i++) {
		u32 next = (ws_ring_head + 1) & RING_MASK;
		if (next == ws_ring_tail) {
			ws_drops += count - i;
			break;
		}
		ws_ring[ws_ring_head] = samples[i];
		ws_ring_head = next;
	}

	LWP_MutexUnlock(ws_ring_lock);
}

static s32 ws_xfer_cb(s32 result, void *arg)
{
	ws_xfer *x = arg;

	x->res = result;
	x->done = true;
	return 0;
}

/* Waits for a transfer for as long as streaming is on.  Isochronous transfers
 * complete every frame on working hardware, but a synchronous call that never
 * returns would leave WS_Stop() joining a thread forever -- which is how the
 * app used to hang on exit.  Returns false if streaming stopped first. */
static bool ws_xfer_wait(ws_xfer *x)
{
	while (!x->done) {
		if (!ws_running)
			return false;
		usleep(1000);
	}
	return true;
}

/* Copies the audio out of one IN transfer, returning the sample count.
 *
 * IOS lays isochronous packets out at their requested sizes, one after the
 * other, and writes each packet's actual length back into the size table.
 * A real Wii Speak sends short packets (16 of the 32 bytes asked for), so
 * the audio sits in the front half of every 32-byte slot with stale bytes
 * between -- reading `ret` bytes from the start of the buffer, as this used
 * to, interleaves real samples with garbage. */
static u32 ws_extract(s32 ret, const u8 *src, const u16 *sizes, s16 *out)
{
	u8 *dst = (u8 *)out;
	u32 bytes = 0;
	u32 sum = 0;
	bool rewritten = false;
	int layout = ws_layout;
	u32 i;

	for (i = 0; i < ISO_IN_PACKETS; i++) {
		sum += sizes[i];
		if (sizes[i] != ISO_IN_PKTSIZE)
			rewritten = true;
	}

	/* On /dev/usb/oh0 a successful isochronous read returns 0, not a byte
	 * count, and leaves the size table as it was: every packet came back
	 * full.  Hardware shows exactly that -- result 0, sizes untouched, all
	 * 256 bytes freshly written with smooth big-endian audio -- so read 0
	 * as success, and tell real data from nothing by the poison. */
	if (layout == WS_LAYOUT_AUTO)
		layout = (rewritten || ret == 0) ? WS_LAYOUT_SLOTS : WS_LAYOUT_PACKED;

	if (ret < 0)
		return 0;

	if (layout == WS_LAYOUT_SLOTS) {
		for (i = 0; i < ISO_IN_PACKETS; i++) {
			const u8 *pkt = src + i * ISO_IN_PKTSIZE;
			u32 n = sizes[i];
			u32 j;

			if (n > ISO_IN_PKTSIZE)
				n = ISO_IN_PKTSIZE;

			/* Still all poison: nothing arrived in this packet. */
			for (j = 0; j < n && pkt[j] == ISO_POISON; j++)
				;
			if (j == n)
				continue;

			memcpy(dst + bytes, pkt, n);
			bytes += n;
		}
	} else {
		bytes = (ret > 0 && (u32)ret <= ISO_IN_BYTES) ? (u32)ret
		      : (sum <= ISO_IN_BYTES ? sum : ISO_IN_BYTES);
		if (ret <= 0)
			bytes = 0;
		memcpy(dst, src, bytes);
	}

	ws_st.last_layout = layout;
	bytes &= ~1u;

	/* The mic is assumed to send big-endian samples, which is host order
	 * on PowerPC and what Dolphin's emulated device produces.  `swap`
	 * exists to test that assumption on real hardware. */
	if (ws_swap) {
		for (i = 0; i < bytes; i += 2) {
			u8 t = dst[i];
			dst[i] = dst[i + 1];
			dst[i + 1] = t;
		}
	}

	return bytes / sizeof(s16);
}

/* Queues IN transfer `slot`, its buffer poisoned so that stale data is
 * recognisable in a capture. */
static s32 ws_in_submit(int slot)
{
	ws_xfer *x = &ws_in_x[slot];
	s32 ret;
	u32 i;

	for (i = 0; i < ISO_IN_PACKETS; i++)
		ws_in_sizes[slot][i] = ISO_IN_PKTSIZE;
	memset(ws_in_buf[slot], ISO_POISON, ISO_IN_BYTES);
	DCFlushRange(ws_in_buf[slot], ISO_IN_BYTES);
	DCFlushRange(ws_in_sizes[slot], 32);

	x->done = false;
	x->pending = true;
	ret = USB_ReadIsoMsgAsync(ws_fd, EP_AUDIO_IN, ISO_IN_PACKETS,
	                          ws_in_sizes[slot], ws_in_buf[slot],
	                          ws_xfer_cb, x);
	if (ret < 0) {
		x->pending = false;
		ws_st.in_errors++;
		ws_st.last_in_ret = ret;
	}
	return ret;
}

/* Keeps ISO_SLOTS transfers queued and collects them in submission order. */
static void *ws_in_worker(void *arg)
{
	static s16 samples[ISO_IN_BYTES / sizeof(s16)];
	int slot;

	(void)arg;

	for (slot = 0; slot < ws_in_slots; slot++)
		ws_in_submit(slot);

	slot = 0;
	while (ws_running) {
		ws_xfer *x = &ws_in_x[slot];
		s32 ret;
		u32 n, i;

		if (!x->pending && ws_in_submit(slot) < 0) {
			usleep(1000);
			slot = (slot + 1) % ws_in_slots;
			continue;
		}

		if (!ws_xfer_wait(x))
			break;
		x->pending = false;

		ret = x->res;
		DCInvalidateRange(ws_in_buf[slot], ISO_IN_BYTES);
		DCInvalidateRange(ws_in_sizes[slot], 32);

		ws_st.in_calls++;
		ws_st.last_in_ret = ret;
		for (i = 0; i < ISO_IN_PACKETS; i++)
			ws_st.last_sizes[i] = ws_in_sizes[slot][i];

		if (ws_tap)
			ws_tap(ret, ws_in_sizes[slot], ws_in_buf[slot]);

		if (ret < 0) {
			/* Back off: failures can come back instantly, and this
			 * thread outranks the main loop and the network. */
			ws_st.in_errors++;
			usleep(1000);
		} else {
			n = ws_extract(ret, ws_in_buf[slot], ws_in_sizes[slot],
			               samples);
			ws_st.in_samples += n;
			if (n > 0)
				ws_push(samples, n);
		}

		if (ws_running)
			ws_in_submit(slot);
		slot = (slot + 1) % ws_in_slots;
	}

	return NULL;
}

static s32 ws_out_submit(int slot)
{
	ws_xfer *x = &ws_out_x[slot];
	s32 ret;
	u32 i;

	for (i = 0; i < ISO_OUT_PACKETS; i++)
		ws_out_sizes[slot][i] = ISO_OUT_PKTSIZE;
	DCFlushRange(ws_out_sizes[slot], 32);

	x->done = false;
	x->pending = true;
	ret = USB_WriteIsoMsgAsync(ws_fd, EP_AUDIO_OUT, ISO_OUT_PACKETS,
	                           ws_out_sizes[slot], ws_out_buf[slot],
	                           ws_xfer_cb, x);
	if (ret < 0) {
		x->pending = false;
		ws_st.out_errors++;
		ws_st.last_out_ret = ret;
	}
	return ret;
}

/* The device expects the reference-audio stream to keep running while it is
 * sampling; a recorder has nothing to play back, so it feeds silence. */
static void *ws_out_worker(void *arg)
{
	int slot;

	(void)arg;

	for (slot = 0; slot < ISO_SLOTS; slot++)
		ws_out_submit(slot);

	slot = 0;
	while (ws_running) {
		ws_xfer *x = &ws_out_x[slot];

		if (!x->pending && ws_out_submit(slot) < 0) {
			usleep(1000);
			slot = (slot + 1) % ISO_SLOTS;
			continue;
		}

		if (!ws_xfer_wait(x))
			break;
		x->pending = false;

		ws_st.out_calls++;
		ws_st.last_out_ret = x->res;
		if (x->res < 0) {
			ws_st.out_errors++;
			usleep(1000);
		}

		if (ws_running)
			ws_out_submit(slot);
		slot = (slot + 1) % ISO_SLOTS;
	}

	return NULL;
}

/* ---- lifecycle ----------------------------------------------------------- */

static void ws_free_buffers(void)
{
	int i;

	free(ws_cmd);       ws_cmd = NULL;
	free(ws_status);    ws_status = NULL;
	free(ws_chunk);     ws_chunk = NULL;
	/* A transfer that never completed may still land in these. */
	for (i = 0; i < ISO_SLOTS; i++) {
		if (!ws_in_x[i].pending) {
			free(ws_in_buf[i]);
			free(ws_in_sizes[i]);
		}
		if (!ws_out_x[i].pending) {
			free(ws_out_buf[i]);
			free(ws_out_sizes[i]);
		}
		ws_in_buf[i] = ws_out_buf[i] = NULL;
		ws_in_sizes[i] = ws_out_sizes[i] = NULL;
	}
	free(ws_in_stack);  ws_in_stack = NULL;
	free(ws_out_stack); ws_out_stack = NULL;
	free(ws_ring);      ws_ring = NULL;
}

static bool ws_alloc_buffers(void)
{
	bool ok = true;
	int i;

	ws_cmd       = memalign(32, 32);
	ws_status    = memalign(32, 32);
	ws_chunk     = memalign(32, FIRMWARE_CHUNK);
	ws_in_stack  = memalign(32, STACKSIZE);
	ws_out_stack = memalign(32, STACKSIZE);
	ws_ring      = memalign(32, RING_SAMPLES * sizeof(s16));

	/* The size tables get a whole cache line each: they are flushed and
	 * invalidated 32 bytes at a time. */
	for (i = 0; i < ISO_SLOTS; i++) {
		ws_in_buf[i]    = memalign(32, ISO_IN_BYTES);
		ws_out_buf[i]   = memalign(32, ISO_OUT_BYTES);
		ws_in_sizes[i]  = memalign(32, 32);
		ws_out_sizes[i] = memalign(32, 32);
		if (!ws_in_buf[i] || !ws_out_buf[i] || !ws_in_sizes[i] ||
		    !ws_out_sizes[i])
			ok = false;
		else
			memset(ws_out_buf[i], 0, ISO_OUT_BYTES);
	}

	if (!ok || !ws_cmd || !ws_status || !ws_chunk || !ws_in_stack ||
	    !ws_out_stack || !ws_ring) {
		ws_free_buffers();
		return false;
	}

	return true;
}

#define MAX_DEVICES 32

/* Finds and opens the mic.
 *
 * Which IOS is running decides how this has to be done.  USBV5 IOSes (57, 58,
 * 61, 80 -- what the Homebrew Channel normally runs) serve /dev/usb/ven and
 * have no /dev/usb/oh0 at all, and there a device has to be opened by the
 * device_id that enumeration handed back.  Older USBV0 IOSes have no device
 * list and instead want /dev/usb/oh0/VID/PID, which is what USB_OH0_DEVICE_ID
 * selects.  Try the modern path first and fall back. */
static s32 ws_open_device(void)
{
	usb_device_entry *list;
	u8 count = 0;
	s32 ret;
	s32 fd;
	int i;

	/* Prefer /dev/usb/oh0 when the running IOS still has it: that is the
	 * interface the SDK used, and the one this protocol is known to work
	 * over.  USBV5 refuses interface-recipient control transfers. */
	fd = IOS_Open("/dev/usb/oh0", 0);
	printf("  IOS %d, oh0 %s\n", (int)IOS_GetVersion(),
	       fd >= 0 ? "present" : "absent");
	if (fd >= 0) {
		IOS_Close(fd);

		ret = USB_OpenDevice(USB_OH0_DEVICE_ID, WS_VID, WS_PID, &ws_fd);
		printf("  open via oh0: %d\n", (int)ret);
		if (ret >= 0)
			return WS_OK;
	}

	list = memalign(32, MAX_DEVICES * sizeof(usb_device_entry));
	if (!list)
		return WS_ENOMEM;
	memset(list, 0, MAX_DEVICES * sizeof(usb_device_entry));

	if (USB_GetDeviceList(list, MAX_DEVICES, 0, &count) >= 0) {
		for (i = 0; i < count; i++) {
			if (list[i].vid != WS_VID || list[i].pid != WS_PID)
				continue;

			ret = USB_OpenDevice(list[i].device_id, WS_VID, WS_PID, &ws_fd);
			if (ret >= 0) {
				free(list);
				return WS_OK;
			}
		}
	}

	free(list);

	ret = USB_OpenDevice(USB_OH0_DEVICE_ID, WS_VID, WS_PID, &ws_fd);
	return ret < 0 ? ret : WS_OK;
}

void WS_Diagnose(void)
{
	usb_device_entry *list;
	u8 count = 0;
	s32 fd;
	int i;

	printf("IOS version: %d\n", (int)IOS_GetVersion());

	fd = IOS_Open("/dev/usb/oh0", 0);
	printf("  /dev/usb/oh0 : %s\n", fd >= 0 ? "yes" : "no");
	if (fd >= 0)
		IOS_Close(fd);

	fd = IOS_Open("/dev/usb/ven", 0);
	printf("  /dev/usb/ven : %s\n", fd >= 0 ? "yes" : "no");
	if (fd >= 0)
		IOS_Close(fd);

	list = memalign(32, MAX_DEVICES * sizeof(usb_device_entry));
	if (!list)
		return;
	memset(list, 0, MAX_DEVICES * sizeof(usb_device_entry));

	if (USB_GetDeviceList(list, MAX_DEVICES, 0, &count) < 0) {
		printf("USB_GetDeviceList failed.\n");
		free(list);
		return;
	}

	printf("USB devices seen: %d\n", (int)count);
	for (i = 0; i < count; i++) {
		printf("  %04x:%04x%s\n", list[i].vid, list[i].pid,
		       (list[i].vid == WS_VID && list[i].pid == WS_PID)
		               ? "  <- Wii Speak" : "");
	}

	free(list);
}

s32 WS_Open(const void *firmware, u32 firmware_size)
{
	s32 ret;

	if (ws_opened)
		return WS_ESTATE;
	if (!firmware || firmware_size == 0)
		return WS_EINVAL;

	if (!ws_alloc_buffers())
		return WS_ENOMEM;

	if (ws_ring_lock == LWP_MUTEX_NULL)
		LWP_MutexInit(&ws_ring_lock, false);
	if (ws_cmd_lock == LWP_MUTEX_NULL)
		LWP_MutexInit(&ws_cmd_lock, false);

	USB_Initialize();

	ret = ws_open_device();
	if (ret < 0) {
		ws_fd = -1;
		ws_free_buffers();
		return WS_ENODEV;
	}
	ws_opened = true;

	/* /dev/usb/oh0 configures a device as part of opening it; USBV5 does not
	 * necessarily, and an unconfigured device answers no interface-recipient
	 * request at all. */
	{
		u8 cfg = 0xFF;

		ret = USB_GetConfiguration(ws_fd, &cfg);
		printf("  get config: %d (cfg %u)\n", (int)ret, (unsigned)cfg);

		if (ret < 0 || cfg != 1) {
			ret = USB_SetConfiguration(ws_fd, 1);
			printf("  set config 1: %d\n", (int)ret);
		}
	}

	{
		usb_devdesc desc;

		if (USB_GetDescriptors(ws_fd, &desc) >= 0) {
			printf("  device %04x:%04x bcd %04x cfgs %u\n",
			       desc.idVendor, desc.idProduct, desc.bcdDevice,
			       (unsigned)desc.bNumConfigurations);
			USB_FreeDescriptors(&desc);
		} else {
			printf("  GetDescriptors failed\n");
		}
	}

	/* Select alt setting 1 BEFORE anything else touches the device.
	 *
	 * The SDK does this in its device-insertion callback, well before the
	 * firmware sequence, and the order matters: interface 0 alt 0 declares
	 * zero endpoints.  In alt 0 there is no EP 0x02 to upload firmware to
	 * and no endpoint servicing the vendor requests, so both silently do
	 * nothing. */
	ret = USB_SetAlternativeInterface(ws_fd, 0, 1);
	printf("  set alt 1 (usbv5): %d\n", (int)ret);
	if (ret < 0) {
		ret = USB_WriteCtrlMsg(ws_fd, USB_CTRLTYPE_DIR_HOST2DEVICE |
		                       USB_CTRLTYPE_TYPE_STANDARD |
		                       USB_CTRLTYPE_REC_INTERFACE,
		                       USB_REQ_SETINTERFACE, 1, 0, 0, NULL);
		printf("  set alt 1 (ctrl): %d\n", (int)ret);
	}
	if (ret < 0)
		goto fail;

	memset(ws_status, 0xA5, 32);
	DCFlushRange(ws_status, 32);
	ret = USB_ReadCtrlMsg(ws_fd, USB_CTRLTYPE_DIR_DEVICE2HOST |
	                      USB_CTRLTYPE_TYPE_STANDARD |
	                      USB_CTRLTYPE_REC_INTERFACE,
	                      USB_REQ_GETINTERFACE, 0, 0, 1, ws_status);
	DCInvalidateRange(ws_status, 32);
	printf("  get interface: %d (alt %u)\n", (int)ret,
	       (unsigned)ws_status[0]);

	ws_probe();

	ret = ws_upload_firmware(firmware, firmware_size);
	if (ret < 0)
		goto fail;

	ws_ring_head = ws_ring_tail = ws_drops = 0;
	return WS_OK;

fail:
	WS_Stop();
	USB_CloseDevice(&ws_fd);
	ws_fd = -1;
	ws_opened = false;
	ws_free_buffers();
	return ret;
}

s32 WS_Close(void)
{
	if (!ws_opened)
		return WS_ESTATE;

	WS_Stop();

	/* The synchronous form never times out, and this is the last thing
	 * between HOME and the Homebrew Channel. */
	printf("  ws: reset %d\n",
	       (int)ws_ctrl(REQTYPE_VENDOR_OUT, REQ_RESET, 0, 0, 0, NULL, 500));
	printf("  ws: close %d\n", (int)USB_CloseDevice(&ws_fd));
	ws_fd = -1;
	ws_opened = false;

	ws_free_buffers();
	return WS_OK;
}

s32 WS_SetFreq(u16 freq)
{
	if (freq != WS_FREQ_8KHZ && freq != WS_FREQ_11KHZ && freq != WS_FREQ_16KHZ)
		return WS_EINVAL;
	return ws_set(REG_SAMPLER_FREQ, freq, 0);
}

s32 WS_GetFreq(u16 *freq)
{
	if (!freq)
		return WS_EINVAL;
	return ws_command(REG_SAMPLER_FREQ | REG_READ, 0, 0, freq, NULL);
}

s32 WS_SetGain(u16 gain)
{
	if (gain > WS_GAIN_36dB)
		return WS_EINVAL;
	return ws_set(REG_SAMPLER_GAIN, gain, 0);
}

s32 WS_SetMute(bool mute)
{
	return ws_set(REG_SAMPLER_MUTE, mute ? 1 : 0, 0);
}

s32 WS_SetEcReset(bool reset)
{
	return ws_set(REG_EC_STATE, reset ? 1 : 0, 0);
}

s32 WS_SetSignalProcessing(bool enable)
{
	/* arg2 == 0 turns processing on, non-zero turns it off. */
	return ws_set(REG_SP_STATE, SP_ENABLE, enable ? 0 : 1);
}

s32 WS_GetSendLevel(u16 *level)
{
	if (!level)
		return WS_EINVAL;
	return ws_command(REG_SP_STATE | REG_READ, SP_SOUT, 0, NULL, level);
}

static s32 ws_select_alt1(const char *when)
{
	s32 ret = USB_SetAlternativeInterface(ws_fd, 0, 1);

	if (ret < 0)
		ret = USB_WriteCtrlMsg(ws_fd, USB_CTRLTYPE_DIR_HOST2DEVICE |
		                       USB_CTRLTYPE_TYPE_STANDARD |
		                       USB_CTRLTYPE_REC_INTERFACE,
		                       USB_REQ_SETINTERFACE, 1, 0, 0, NULL);
	printf("  set alt 1 (%s): %d\n", when, (int)ret);
	return ret;
}

static s32 ws_start_streams(void)
{
	if (ws_running)
		return WS_OK;

	ws_running = true;

	if (LWP_CreateThread(&ws_in_thread, ws_in_worker, NULL, ws_in_stack,
	                     STACKSIZE, 80) < 0) {
		ws_running = false;
		return WS_ENOMEM;
	}

	if (!ws_out_on) {
		ws_out_thread = LWP_THREAD_NULL;
	} else if (LWP_CreateThread(&ws_out_thread, ws_out_worker, NULL, ws_out_stack,
	                     STACKSIZE, 80) < 0) {
		/* The IN stream alone still records; just skip the reference feed. */
		ws_out_thread = LWP_THREAD_NULL;
	}

	return WS_OK;
}

s32 WS_Start(void)
{
	s32 ret;

	if (!ws_opened)
		return WS_ESTATE;

	/* Write-only: don't gate startup on a control-IN that might block, and
	 * let the on-screen sample counter be the real test of whether the mic
	 * is streaming. */
	{
		int tries;

		for (tries = 0; tries < 20; tries++) {
			ret = ws_set(REG_SAMPLER_STATE, 1, 0);
			if (ret >= 0)
				break;
			usleep(100 * 1000);
		}
		printf("  sampler on: %d\n", (int)ret);
	}

	/* Then unmute, as PMICStart does: the SDK sends exactly these two
	 * commands in this order.  With the unmute sent first, as it used to
	 * be, the LED stayed dark and the stream sat at its noise floor. */
	printf("  mute off: %d\n", (int)ws_set(REG_SAMPLER_MUTE, 0, 0));

	/* Normally already running, started straight after the firmware. */
	if (ws_start_streams() < 0) {
		ws_set(REG_SAMPLER_STATE, 0, 0);
		return WS_ENOMEM;
	}

	return WS_OK;
}

s32 WS_Stop(void)
{
	if (!ws_running)
		return WS_OK;

	/* While the streams still run: commands fail without them. */
	printf("  ws: sampler off %d\n", (int)ws_set(REG_SAMPLER_STATE, 0, 0));

	ws_running = false;

	/* The workers poll ws_running while they wait, so these return
	 * promptly even if a transfer never completes. */
	if (ws_in_thread != LWP_THREAD_NULL) {
		LWP_JoinThread(ws_in_thread, NULL);
		ws_in_thread = LWP_THREAD_NULL;
	}
	if (ws_out_thread != LWP_THREAD_NULL) {
		LWP_JoinThread(ws_out_thread, NULL);
		ws_out_thread = LWP_THREAD_NULL;
	}
	printf("  ws: threads joined (in %s, out %s)\n",
	       (ws_in_x[0].pending || ws_in_x[1].pending) ? "pending" : "idle",
	       (ws_out_x[0].pending || ws_out_x[1].pending) ? "pending" : "idle");

	return WS_OK;
}

void WS_SetStreams(int in_slots, bool out_stream)
{
	if (in_slots >= 1 && in_slots <= ISO_SLOTS)
		ws_in_slots = in_slots;
	ws_out_on = out_stream;
}

void WS_Quiesce(void)
{
	ws_running = false;
}

u32 WS_Read(s16 *dst, u32 max_samples)
{
	u32 taken = 0;

	if (!dst || !ws_ring)
		return 0;

	LWP_MutexLock(ws_ring_lock);

	while (taken < max_samples && ws_ring_tail != ws_ring_head) {
		dst[taken++] = ws_ring[ws_ring_tail];
		ws_ring_tail = (ws_ring_tail + 1) & RING_MASK;
	}

	LWP_MutexUnlock(ws_ring_lock);
	return taken;
}

u32 WS_Available(void)
{
	return (ws_ring_head - ws_ring_tail) & RING_MASK;
}

u32 WS_Dropped(void)
{
	return ws_drops;
}

void WS_GetStats(ws_stats *out)
{
	if (out)
		*out = ws_st;
}

void WS_SetIsoTap(ws_iso_tap tap)
{
	ws_tap = tap;
}

void WS_SetLayout(int layout)
{
	ws_layout = layout;
}

void WS_SetSwap(bool swap)
{
	ws_swap = swap;
}

bool WS_GetSwap(void)
{
	return ws_swap;
}

int WS_GetLayout(void)
{
	return ws_layout;
}

void WS_SetCancel(bool (*cancel)(void))
{
	ws_cancel = cancel;
}
