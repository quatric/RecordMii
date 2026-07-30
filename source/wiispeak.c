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
static u8 *ws_in_buf;   /* isochronous IN landing buffer */
static u8 *ws_out_buf;  /* isochronous OUT (silence) */
static u16 *ws_in_sizes;
static u16 *ws_out_sizes;

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

/* Submits one 10-byte command.  When out1/out2 are non-NULL it also reads the
 * reply back; a pure set passes NULL for both and skips the read. */
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

	if (out1 == NULL && out2 == NULL) {
		ret = WS_OK;
		goto out;
	}

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
	usleep(500 * 1000);
	(void)ws_wait_status(1, "status after upload");

	/* The DSP reboots after taking the firmware and NAKs control transfers
	 * until it is back -- which is why the first command times out.  We
	 * cannot read the ready flag (single-byte reads never land on this
	 * stack), so probe readiness by retrying a real command until the
	 * device answers it. */
	{
		int tries;

		for (tries = 0; tries < 30; tries++) {
			ret = ws_set(REG_SAMPLER_FREQ, WS_FREQ_16KHZ, 0);
			if (ret >= 0) {
				printf("  ready after %d tries\n", tries + 1);
				return WS_OK;
			}
			usleep(100 * 1000);
		}
		printf("  device never accepted a command (%d)\n", (int)ret);
	}

	return WS_OK;
}

/* ---- transfer threads ---------------------------------------------------- */

static u32 ws_iso_bytes(s32 ret, const u16 *sizes, u32 count, u32 limit)
{
	u32 total = 0;
	u32 i;

	/* IOS reports the transferred length either as the ioctl result or by
	 * rewriting the per-packet size table; accept whichever looks sane. */
	if (ret > 0 && (u32)ret <= limit)
		return (u32)ret;

	for (i = 0; i < count; i++)
		total += sizes[i];

	return total > limit ? limit : total;
}

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

static void *ws_in_worker(void *arg)
{
	(void)arg;

	while (ws_running) {
		s32 ret;
		u32 bytes, i;

		for (i = 0; i < ISO_IN_PACKETS; i++)
			ws_in_sizes[i] = ISO_IN_PKTSIZE;

		ret = USB_ReadIsoMsg(ws_fd, EP_AUDIO_IN, ISO_IN_PACKETS,
		                     ws_in_sizes, ws_in_buf);
		if (ret < 0) {
			if (!ws_running)
				break;
			usleep(1000);
			continue;
		}

		bytes = ws_iso_bytes(ret, ws_in_sizes, ISO_IN_PACKETS, ISO_IN_BYTES);
		if (bytes >= sizeof(s16)) {
			/* The mic sends big-endian 16-bit mono, which is already
			 * host order on PowerPC. */
			ws_push((const s16 *)ws_in_buf, bytes / sizeof(s16));
		}
	}

	return NULL;
}

/* The device expects the reference-audio stream to keep running while it is
 * sampling; a recorder has nothing to play back, so it feeds silence. */
static void *ws_out_worker(void *arg)
{
	(void)arg;

	while (ws_running) {
		u32 i;

		for (i = 0; i < ISO_OUT_PACKETS; i++)
			ws_out_sizes[i] = ISO_OUT_PKTSIZE;

		if (USB_WriteIsoMsg(ws_fd, EP_AUDIO_OUT, ISO_OUT_PACKETS,
		                    ws_out_sizes, ws_out_buf) < 0) {
			if (!ws_running)
				break;
			usleep(1000);
		}
	}

	return NULL;
}

/* ---- lifecycle ----------------------------------------------------------- */

static void ws_free_buffers(void)
{
	free(ws_cmd);       ws_cmd = NULL;
	free(ws_status);    ws_status = NULL;
	free(ws_chunk);     ws_chunk = NULL;
	free(ws_in_buf);    ws_in_buf = NULL;
	free(ws_out_buf);   ws_out_buf = NULL;
	free(ws_in_sizes);  ws_in_sizes = NULL;
	free(ws_out_sizes); ws_out_sizes = NULL;
	free(ws_in_stack);  ws_in_stack = NULL;
	free(ws_out_stack); ws_out_stack = NULL;
	free(ws_ring);      ws_ring = NULL;
}

static bool ws_alloc_buffers(void)
{
	ws_cmd       = memalign(32, 32);
	ws_status    = memalign(32, 32);
	ws_chunk     = memalign(32, FIRMWARE_CHUNK);
	ws_in_buf    = memalign(32, ISO_IN_BYTES);
	ws_out_buf   = memalign(32, ISO_OUT_BYTES);
	ws_in_sizes  = memalign(32, ISO_IN_PACKETS * sizeof(u16));
	ws_out_sizes = memalign(32, ISO_OUT_PACKETS * sizeof(u16));
	ws_in_stack  = memalign(32, STACKSIZE);
	ws_out_stack = memalign(32, STACKSIZE);
	ws_ring      = memalign(32, RING_SAMPLES * sizeof(s16));

	if (!ws_cmd || !ws_status || !ws_chunk || !ws_in_buf || !ws_out_buf ||
	    !ws_in_sizes || !ws_out_sizes || !ws_in_stack || !ws_out_stack ||
	    !ws_ring) {
		ws_free_buffers();
		return false;
	}

	memset(ws_out_buf, 0, ISO_OUT_BYTES);
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

	USB_WriteCtrlMsg(ws_fd, REQTYPE_VENDOR_OUT, REQ_RESET, 0, 0, 0, NULL);
	USB_CloseDevice(&ws_fd);
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

s32 WS_Start(void)
{
	s32 ret;

	if (!ws_opened)
		return WS_ESTATE;
	if (ws_running)
		return WS_OK;

	/* Write-only: don't gate startup on a control-IN that might block, and
	 * let the on-screen sample counter be the real test of whether the mic
	 * is streaming. */
	ret = ws_set(REG_SAMPLER_STATE, 1, 0);
	printf("  sampler on: %d\n", (int)ret);

	ws_running = true;

	if (LWP_CreateThread(&ws_in_thread, ws_in_worker, NULL, ws_in_stack,
	                     STACKSIZE, 80) < 0) {
		ws_running = false;
		ws_set(REG_SAMPLER_STATE, 0, 0);
		return WS_ENOMEM;
	}

	if (LWP_CreateThread(&ws_out_thread, ws_out_worker, NULL, ws_out_stack,
	                     STACKSIZE, 80) < 0) {
		/* The IN stream alone still records; just skip the reference feed. */
		ws_out_thread = LWP_THREAD_NULL;
	}

	return WS_OK;
}

s32 WS_Stop(void)
{
	if (!ws_running)
		return WS_OK;

	ws_running = false;

	/* Stopping the sampler unblocks any transfer still in flight. */
	ws_set(REG_SAMPLER_STATE, 0, 0);

	if (ws_in_thread != LWP_THREAD_NULL) {
		LWP_JoinThread(ws_in_thread, NULL);
		ws_in_thread = LWP_THREAD_NULL;
	}
	if (ws_out_thread != LWP_THREAD_NULL) {
		LWP_JoinThread(ws_out_thread, NULL);
		ws_out_thread = LWP_THREAD_NULL;
	}

	return WS_OK;
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
