/* A way out that works whatever state the app is in.  See escape.h. */

#include <stdio.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/ios.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <wiiuse/wpad.h>

#include "escape.h"

/* The Homebrew Channel leaves its reload stub here, tagged "STUBHAXX". */
#define STUB_ADDR  0x80001800
#define STUB_MAGIC ((volatile u32 *)0x80001804)

/* Processor interface interrupt cause and mask. */
#define PI_INTSR   ((volatile u32 *)0xCC003000)
#define PI_INTMR   ((volatile u32 *)0xCC003004)

/* How long the IOS reload may take before the rescue thread gives up on it
 * and jumps anyway.  A normal reload takes about a second. */
#define RELOAD_GRACE_MS 4000

static volatile bool want_exit;
static volatile bool want_power;
static volatile u64 deadline;       /* 0 = not armed */
static volatile u64 reload_started; /* 0 = no reload under way */
static volatile u64 deadman;        /* 0 = off */
static volatile u32 deadman_ms;
static void (*quiesce)(void);

static lwp_t dog = LWP_THREAD_NULL;
static lwp_t rescue = LWP_THREAD_NULL;
static u8 dog_stack[8192] ATTRIBUTE_ALIGN(32);
static u8 rescue_stack[4096] ATTRIBUTE_ALIGN(32);

static bool stub_found(void)
{
	/* libogc accepts the tag at either offset, so do the same. */
	return (STUB_MAGIC[0] == 0x53545542 && STUB_MAGIC[1] == 0x48415858) ||
	       (STUB_MAGIC[1] == 0x53545542 && STUB_MAGIC[2] == 0x48415858);
}

/* The jump itself.  Interrupts go off at the processor and at their source,
 * so nothing still armed can vector into this program's handlers once the
 * stub has loaded over it. */
static void __attribute__((noreturn)) jump_to_loader(void)
{
	u32 level;

	/* Breadcrumbs go to the TV: if this hangs, the screen says where. */
	printf("leave: jumping to the %s\n", stub_found() ? "loader stub" : "system menu");
	fflush(stdout);

	_CPU_ISR_Disable(level);
	(void)level;

	*PI_INTMR = 0;
	*PI_INTSR = 0xFFFFFFFF;

	if (stub_found())
		((void (*)(void))STUB_ADDR)();

	/* No loader to go back to.  This needs IPC, which may be wedged, but
	 * there is nothing better left. */
	_CPU_ISR_Restore(level);
	SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
	while (1)
		;
}

void ESC_Now(void)
{
	/* The Homebrew Channel's stub starts by closing every IOS descriptor
	 * and waits, with no timeout, for each close to be answered.  IOS
	 * holds a close back while requests on that descriptor are still
	 * outstanding -- the mic's isochronous transfers, a socket blocked in
	 * accept -- so jumping straight to the stub could hang right there.
	 * Reloading IOS first throws all of that away.  If the reload itself
	 * stalls, the rescue thread jumps anyway. */
	if (reload_started == 0) {
		s32 ret;

		printf("leave: reloading IOS%d\n", (int)IOS_GetVersion());
		if (quiesce)
			quiesce();
		reload_started = gettime();
		ret = IOS_ReloadIOS(IOS_GetVersion());
		printf("leave: reload %d\n", (int)ret);
	}

	jump_to_loader();
}

void ESC_SetQuiesce(void (*fn)(void))
{
	quiesce = fn;
}

void ESC_Request(void)
{
	want_exit = true;
}

void ESC_Arm(u32 ms)
{
	deadline = gettime() + millisecs_to_ticks(ms);
}

void ESC_SetDeadman(u32 ms)
{
	deadman_ms = ms;
	deadman = ms ? gettime() + millisecs_to_ticks(ms) : 0;
}

void ESC_Feed(void)
{
	if (deadman_ms)
		deadman = gettime() + millisecs_to_ticks(deadman_ms);
}

/* These run in interrupt context: just raise a flag for the watchdog. */
static void on_reset(u32 irq, void *ctx)
{
	(void)irq;
	(void)ctx;
	want_exit = true;
}

static void on_power(void)
{
	want_power = true;
}

static void on_wpad_power(s32 chan)
{
	(void)chan;
	want_power = true;
}

static void *dog_main(void *arg)
{
	(void)arg;

	while (1) {
		u64 now = gettime();

		if (want_power)
			SYS_ResetSystem(SYS_POWEROFF, 0, 0);
		if (deadman && now >= deadman) {
			printf("\nNo debug command for %u ms: leaving.\n",
			       (unsigned)deadman_ms);
			want_exit = true;
		}
		if (want_exit || (deadline && now >= deadline))
			ESC_Now();
		usleep(50 * 1000);
	}

	return NULL;
}

/* Watches the watchdog: ESC_Now's IOS reload blocks on IPC, which is the one
 * place the watchdog itself can get stuck. */
static void *rescue_main(void *arg)
{
	(void)arg;

	while (1) {
		u64 started = reload_started;

		if (started && ticks_to_millisecs(gettime() - started) > RELOAD_GRACE_MS) {
			printf("leave: reload stalled\n");
			jump_to_loader();
		}
		usleep(100 * 1000);
	}

	return NULL;
}

void ESC_Init(void)
{
	if (dog != LWP_THREAD_NULL)
		return;

	SYS_SetResetCallback(on_reset);
	SYS_SetPowerCallback(on_power);

	/* Top priority, so a busy thread can't starve them. */
	LWP_CreateThread(&dog, dog_main, NULL, dog_stack, sizeof(dog_stack),
	                 LWP_PRIO_HIGHEST);
	LWP_CreateThread(&rescue, rescue_main, NULL, rescue_stack,
	                 sizeof(rescue_stack), LWP_PRIO_HIGHEST);
}

void ESC_WpadReady(void)
{
	WPAD_SetPowerButtonCallback(on_wpad_power);
}
