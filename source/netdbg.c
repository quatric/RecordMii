/* TCP debug console.  See netdbg.h. */

#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <network.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>

#include "escape.h"
#include "netdbg.h"
#include "wiispeak.h"

/* ---- log ------------------------------------------------------------------ */

#define LOG_SIZE (64 * 1024)

static char log_buf[LOG_SIZE];
static volatile u32 log_total; /* bytes ever written; the ring holds the tail */
static mutex_t log_lock = LWP_MUTEX_NULL;
static volatile bool log_quiet;

static devoptab_t out_tab;
static const devoptab_t *out_orig;

static void log_append(const char *p, size_t len)
{
	size_t i;

	if (log_lock == LWP_MUTEX_NULL)
		return;

	LWP_MutexLock(log_lock);
	for (i = 0; i < len; i++) {
		/* The console's cursor escapes are noise in a text log. */
		if (p[i] == '\x1b') {
			while (i < len && !((p[i] >= 'A' && p[i] <= 'Z') ||
			                    (p[i] >= 'a' && p[i] <= 'z')))
				i++;
			continue;
		}
		log_buf[log_total % LOG_SIZE] = p[i];
		log_total++;
	}
	LWP_MutexUnlock(log_lock);
}

static ssize_t out_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
	if (!log_quiet)
		log_append(ptr, len);
	if (out_orig && out_orig->write_r)
		return out_orig->write_r(r, fd, ptr, len);
	return (ssize_t)len;
}

void NDBG_HookStdout(void)
{
	if (out_orig)
		return;

	LWP_MutexInit(&log_lock, false);

	out_orig = devoptab_list[STD_OUT];
	out_tab = *out_orig;
	out_tab.write_r = out_write;
	devoptab_list[STD_OUT] = &out_tab;
	devoptab_list[STD_ERR] = &out_tab;
}

void NDBG_Quiet(bool quiet)
{
	/* Flush first, so what was printed before the switch is logged. */
	fflush(stdout);
	log_quiet = quiet;
}

/* ---- main-loop mailbox ------------------------------------------------------ */

#define REPLY_SIZE 2048

enum { MB_IDLE, MB_PENDING, MB_DONE };

static char mb_line[256];
static char mb_reply[REPLY_SIZE];
static volatile bool mb_ok;
static volatile int mb_state = MB_IDLE;

void NDBG_Poll(ndbg_handler handler)
{
	if (mb_state != MB_PENDING)
		return;

	mb_reply[0] = '\0';
	mb_ok = handler(mb_line, mb_reply, sizeof(mb_reply));
	mb_state = MB_DONE;
}

/* Hands a line to the main loop and waits for its reply.  Gives up after a
 * few seconds, which is itself a useful answer: the main loop is stuck. */
static bool mb_call(const char *line, char *reply, size_t size)
{
	int waited;

	if (mb_state != MB_IDLE) {
		snprintf(reply, size, "main loop still busy with an earlier command");
		return false;
	}

	snprintf(mb_line, sizeof(mb_line), "%s", line);
	mb_state = MB_PENDING;

	for (waited = 0; mb_state != MB_DONE && waited < 5000; waited += 5)
		usleep(5 * 1000);

	if (mb_state != MB_DONE) {
		/* Leave it pending: it will run if the loop ever comes back, and
		 * the next call reports it as busy. */
		snprintf(reply, size, "main loop did not answer within 5 s");
		return false;
	}

	snprintf(reply, size, "%s", mb_reply);
	mb_state = MB_IDLE;
	return mb_ok;
}

/* ---- captures ------------------------------------------------------------- */

/* One raw IN transfer exactly as IOS returned it. */
typedef struct {
	s32 ret;
	u16 sizes[WS_ISO_PACKETS];
	u8 data[WS_ISO_PACKETS * WS_ISO_PKTSIZE];
} iso_rec;

static iso_rec *volatile cap_iso;
static volatile u32 cap_iso_n, cap_iso_max;

static void iso_tap(s32 ret, const u16 *sizes, const u8 *buf)
{
	iso_rec *r;

	if (cap_iso_n >= cap_iso_max)
		return;

	r = &cap_iso[cap_iso_n];
	r->ret = ret;
	memcpy(r->sizes, sizes, sizeof(r->sizes));
	memcpy(r->data, buf, sizeof(r->data));
	cap_iso_n++;
}

static s16 *volatile cap_pcm;
static volatile u32 cap_pcm_n, cap_pcm_max, cap_pcm_rate;

void NDBG_FeedPcm(const s16 *samples, u32 count, u32 rate)
{
	u32 room;

	if (!cap_pcm || cap_pcm_n >= cap_pcm_max)
		return;

	room = cap_pcm_max - cap_pcm_n;
	if (count > room)
		count = room;

	memcpy(cap_pcm + cap_pcm_n, samples, count * sizeof(s16));
	cap_pcm_rate = rate;
	cap_pcm_n += count;
}

/* ---- socket helpers --------------------------------------------------------- */

static char srv_addr[24];
static volatile s32 srv_sock = -1;
static lwp_t srv_thread = LWP_THREAD_NULL;
static u8 srv_stack[32 * 1024] ATTRIBUTE_ALIGN(32);

static bool send_all(s32 fd, const void *data, u32 len)
{
	const u8 *p = data;

	while (len > 0) {
		s32 n = net_send(fd, p, len > 16384 ? 16384 : len, 0);
		if (n <= 0)
			return false;
		p += n;
		len -= (u32)n;
	}
	return true;
}

static bool sendf(s32 fd, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (n < 0)
		return false;
	if (n >= (int)sizeof(buf))
		n = sizeof(buf) - 1;
	return send_all(fd, buf, (u32)n);
}

/* Reads one line, without its terminator.  Returns false on disconnect. */
static bool recv_line(s32 fd, char *line, size_t size)
{
	size_t len = 0;

	while (1) {
		char c;
		s32 n = net_recv(fd, &c, 1, 0);

		if (n <= 0)
			return false;
		if (c == '\n')
			break;
		if (c != '\r' && len + 1 < size)
			line[len++] = c;
	}

	line[len] = '\0';
	return true;
}

/* ---- commands --------------------------------------------------------------- */

static const char HELP[] =
	"Handled here (work even if the main loop is stuck):\n"
	"  help | ping\n"
	"  log [all]            log since the last `log`, or the whole ring\n"
	"  stats                driver counters and the last IN transfer\n"
	"  cap <ms>             raw IN transfers: BIN <bytes> <recsize>\n"
	"  reg <reg> <a1> <a2> [r]  raw command (hex); r reads the reply\n"
	"  layout auto|packed|slots   how samples are cut out of a transfer\n"
	"  swap 0|1             byte-swap samples\n"
	"  pcm <ms>             what gets recorded: BIN <bytes> <rate>\n"
	"  ls <dir> | get <path>\n"
	"  kill                 back to the loader now, no cleanup\n"
	"  deadman <ms>         leave on its own after <ms> without a command (0 off)\n"
	"Handed to the main loop:\n"
	"  status\n"
	"  gain 0-3 | freq 8|11|16 | sp 0|1 | mute 0|1\n"
	"  tone <hz> <ms> [vol 0-255] | tone off\n"
	"  rec start|stop | play [path] | stop\n"
	"  exit                 clean exit; forced after 5 s if it stalls\n";

static void cmd_log(s32 fd, bool all, u32 *cursor)
{
	u32 total, from, i;
	static char chunk[1024];
	u32 n = 0;

	LWP_MutexLock(log_lock);
	total = log_total;
	from = all ? 0 : *cursor;
	if (total - from > LOG_SIZE || from > total)
		from = total > LOG_SIZE ? total - LOG_SIZE : 0;
	LWP_MutexUnlock(log_lock);

	/* Copy in pieces so the lock isn't held across the network. */
	for (i = from; i < total; i++) {
		chunk[n++] = log_buf[i % LOG_SIZE];
		if (n == sizeof(chunk)) {
			send_all(fd, chunk, n);
			n = 0;
		}
	}
	if (n)
		send_all(fd, chunk, n);
	if (total > from && log_buf[(total - 1) % LOG_SIZE] != '\n')
		send_all(fd, "\n", 1);

	*cursor = total;
	sendf(fd, "OK %u bytes\n", (unsigned)(total - from));
}

static void cmd_stats(s32 fd)
{
	ws_stats st;
	int i;

	WS_GetStats(&st);
	sendf(fd, "in  calls %u errors %u samples %u last_ret %d\n",
	      (unsigned)st.in_calls, (unsigned)st.in_errors,
	      (unsigned)st.in_samples, (int)st.last_in_ret);
	sendf(fd, "out calls %u errors %u last_ret %d\n",
	      (unsigned)st.out_calls, (unsigned)st.out_errors,
	      (int)st.last_out_ret);
	sendf(fd, "last sizes");
	for (i = 0; i < WS_ISO_PACKETS; i++)
		sendf(fd, " %u", (unsigned)st.last_sizes[i]);
	sendf(fd, "\nlayout %d (used %d)  swap %d  buffered %u  dropped %u\n",
	      WS_GetLayout(), st.last_layout, (int)WS_GetSwap(),
	      (unsigned)WS_Available(), (unsigned)WS_Dropped());
	sendf(fd, "OK\n");
}

static void cmd_cap(s32 fd, u32 ms)
{
	u32 want, waited;

	if (ms == 0 || ms > 20000) {
		sendf(fd, "ERR ms must be 1..20000\n");
		return;
	}

	/* 125 transfers a second, plus slack. */
	want = ms * 125 / 1000 + 1;
	cap_iso = malloc(want * sizeof(iso_rec));
	if (!cap_iso) {
		sendf(fd, "ERR out of memory\n");
		return;
	}

	cap_iso_n = 0;
	cap_iso_max = want;
	WS_SetIsoTap(iso_tap);

	for (waited = 0; cap_iso_n < want && waited < ms + 3000; waited += 10)
		usleep(10 * 1000);

	WS_SetIsoTap(NULL);
	usleep(20 * 1000); /* let a tap call already under way finish */

	sendf(fd, "BIN %u %u\n", (unsigned)(cap_iso_n * sizeof(iso_rec)),
	      (unsigned)sizeof(iso_rec));
	send_all(fd, cap_iso, cap_iso_n * sizeof(iso_rec));
	sendf(fd, "OK %u transfers in %u ms\n", (unsigned)cap_iso_n,
	      (unsigned)waited);

	free(cap_iso);
	cap_iso = NULL;
	cap_iso_max = 0;
}

static void cmd_pcm(s32 fd, u32 ms)
{
	u32 want, waited;

	if (ms == 0 || ms > 30000) {
		sendf(fd, "ERR ms must be 1..30000\n");
		return;
	}

	/* Sized for the fastest rate; the reply says which one it was. */
	want = ms * 16;
	cap_pcm_n = 0;
	cap_pcm_rate = 0;
	cap_pcm_max = want;
	cap_pcm = malloc(want * sizeof(s16));
	if (!cap_pcm) {
		sendf(fd, "ERR out of memory\n");
		return;
	}

	/* Stop once `ms` worth has arrived at whatever rate the mic runs. */
	for (waited = 0; waited < ms + 3000; waited += 10) {
		if (cap_pcm_rate && cap_pcm_n >= ms * cap_pcm_rate / 1000)
			break;
		usleep(10 * 1000);
	}

	{
		s16 *buf = cap_pcm;
		u32 n = cap_pcm_n;

		cap_pcm = NULL;
		usleep(20 * 1000);

		sendf(fd, "BIN %u %u\n", (unsigned)(n * sizeof(s16)),
		      (unsigned)cap_pcm_rate);
		send_all(fd, buf, n * sizeof(s16));
		sendf(fd, "OK %u samples in %u ms\n", (unsigned)n,
		      (unsigned)waited);
		free(buf);
	}
}

static void cmd_reg(s32 fd, const char *args)
{
	unsigned reg = 0, a1 = 0, a2 = 0;
	char mode[4] = "";
	u16 o1 = 0, o2 = 0;
	s32 ret;
	int n;

	n = sscanf(args, "%x %x %x %3s", &reg, &a1, &a2, mode);
	if (n < 3) {
		sendf(fd, "ERR usage: reg <reg> <a1> <a2> [r]  (hex)\n");
		return;
	}

	if (mode[0] == 'r') {
		ret = WS_RawCommand((u8)reg, (u16)a1, (u16)a2, &o1, &o2);
		sendf(fd, "OK ret %d out1 %04x out2 %04x\n", (int)ret, o1, o2);
	} else {
		ret = WS_RawCommand((u8)reg, (u16)a1, (u16)a2, NULL, NULL);
		sendf(fd, "OK ret %d\n", (int)ret);
	}
}

static void cmd_ls(s32 fd, const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;

	if (!d) {
		sendf(fd, "ERR cannot open %s\n", path);
		return;
	}

	while ((e = readdir(d)) != NULL) {
		char full[300];
		struct stat st;

		snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
		if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
			sendf(fd, "%s/\n", e->d_name);
		else if (stat(full, &st) == 0)
			sendf(fd, "%s %ld\n", e->d_name, (long)st.st_size);
	}

	closedir(d);
	sendf(fd, "OK\n");
}

static void cmd_get(s32 fd, const char *path)
{
	static u8 buf[16384] ATTRIBUTE_ALIGN(32);
	FILE *fp = fopen(path, "rb");
	long size;

	if (!fp) {
		sendf(fd, "ERR cannot open %s\n", path);
		return;
	}

	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	sendf(fd, "BIN %ld\n", size);
	while (size > 0) {
		size_t n = fread(buf, 1, size > (long)sizeof(buf) ? sizeof(buf)
		                                                  : (size_t)size, fp);
		if (n == 0) {
			/* Keep the framing intact even if the read fails. */
			memset(buf, 0, sizeof(buf));
			n = size > (long)sizeof(buf) ? sizeof(buf) : (size_t)size;
		}
		send_all(fd, buf, n);
		size -= (long)n;
	}

	fclose(fp);
	sendf(fd, "OK\n");
}

static void handle(s32 fd, char *line, u32 *log_cursor)
{
	char *args = strchr(line, ' ');
	char reply[REPLY_SIZE];

	if (args)
		*args++ = '\0';
	else
		args = line + strlen(line);

	if (!strcmp(line, "help")) {
		send_all(fd, HELP, sizeof(HELP) - 1);
		sendf(fd, "OK\n");
	} else if (!strcmp(line, "ping")) {
		sendf(fd, "OK pong\n");
	} else if (!strcmp(line, "log")) {
		cmd_log(fd, !strcmp(args, "all"), log_cursor);
	} else if (!strcmp(line, "stats")) {
		cmd_stats(fd);
	} else if (!strcmp(line, "cap")) {
		cmd_cap(fd, (u32)strtoul(args, NULL, 10));
	} else if (!strcmp(line, "pcm")) {
		cmd_pcm(fd, (u32)strtoul(args, NULL, 10));
	} else if (!strcmp(line, "reg")) {
		cmd_reg(fd, args);
	} else if (!strcmp(line, "layout")) {
		if (!strcmp(args, "packed"))
			WS_SetLayout(WS_LAYOUT_PACKED);
		else if (!strcmp(args, "slots"))
			WS_SetLayout(WS_LAYOUT_SLOTS);
		else
			WS_SetLayout(WS_LAYOUT_AUTO);
		sendf(fd, "OK layout %d\n", WS_GetLayout());
	} else if (!strcmp(line, "swap")) {
		WS_SetSwap(atoi(args) != 0);
		sendf(fd, "OK swap %d\n", (int)WS_GetSwap());
	} else if (!strcmp(line, "deadman")) {
		ESC_SetDeadman((u32)strtoul(args, NULL, 10));
		sendf(fd, "OK deadman %s ms\n", *args ? args : "0");
	} else if (!strcmp(line, "kill")) {
		sendf(fd, "OK leaving\n");
		usleep(100 * 1000);
		ESC_Request();
	} else if (!strcmp(line, "ls")) {
		cmd_ls(fd, *args ? args : "sd:/wiispeak");
	} else if (!strcmp(line, "get")) {
		cmd_get(fd, args);
	} else {
		/* Everything else belongs to the main loop. */
		if (*args)
			args[-1] = ' ';
		/* Leave even if the main loop never picks this up. */
		if (!strcmp(line, "exit"))
			ESC_Arm(8000);
		if (mb_call(line, reply, sizeof(reply))) {
			send_all(fd, reply, strlen(reply));
			sendf(fd, "OK\n");
		} else {
			sendf(fd, "ERR %s\n", reply);
		}
	}
}

/* ---- server ------------------------------------------------------------------ */

static void *srv_main(void *arg)
{
	struct sockaddr_in sa;
	s32 sock;
	u32 ip;
	int tries;

	(void)arg;

	/* The update check normally leaves the network up already. */
	ip = net_gethostip();
	for (tries = 0; ip == 0 && tries < 3; tries++) {
		char a[16] = "", m[16] = "", g[16] = "";
		if (if_config(a, m, g, true, 20) >= 0)
			ip = net_gethostip();
	}
	if (ip == 0) {
		printf("Debug: no network\n");
		return NULL;
	}

	sock = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (sock < 0) {
		printf("Debug: socket failed (%d)\n", (int)sock);
		return NULL;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(NDBG_PORT);
	sa.sin_addr.s_addr = INADDR_ANY;

	if (net_bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    net_listen(sock, 1) < 0) {
		printf("Debug: cannot listen on %d\n", NDBG_PORT);
		net_close(sock);
		return NULL;
	}

	snprintf(srv_addr, sizeof(srv_addr), "%u.%u.%u.%u:%d",
	         (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 0xFF),
	         (unsigned)((ip >> 8) & 0xFF), (unsigned)(ip & 0xFF), NDBG_PORT);
	printf("Debug: listening on %s\n", srv_addr);
	srv_sock = sock;

	while (1) {
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		u32 log_cursor = 0;
		char line[256];
		s32 fd;

		fd = net_accept(sock, (struct sockaddr *)&peer, &plen);
		if (srv_sock < 0)
			break;
		if (fd < 0) {
			usleep(100 * 1000);
			continue;
		}

		sendf(fd, "RecordMii debug %s\n", APP_VERSION);
		while (recv_line(fd, line, sizeof(line))) {
			if (line[0] == '\0')
				continue;
			ESC_Feed();
			handle(fd, line, &log_cursor);
		}

		net_close(fd);
	}

	return NULL;
}

void NDBG_Start(void)
{
	if (srv_thread != LWP_THREAD_NULL)
		return;

	/* Below the USB workers (80) so a capture never starves the mic. */
	LWP_CreateThread(&srv_thread, srv_main, NULL, srv_stack,
	                 sizeof(srv_stack), 50);
}

const char *NDBG_Address(void)
{
	return srv_addr[0] ? srv_addr : NULL;
}

void NDBG_Stop(void)
{
	s32 sock = srv_sock;

	if (sock >= 0) {
		srv_sock = -1;
		net_close(sock);
	}
	net_deinit();
}
