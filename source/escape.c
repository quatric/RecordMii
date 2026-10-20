/* A way out that works whatever state the app is in.  See escape.h. */

#include <unistd.h>

#include <gccore.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <wiiuse/wpad.h>

#include "escape.h"

/* The Homebrew Channel leaves its reload stub here, tagged "STUBHAXX". */
#define STUB_ADDR  0x80001800
#define STUB_MAGIC ((volatile u32 *)0x80001804)

static volatile bool want_exit;
static volatile bool want_power;
static volatile u64 deadline; /* 0 = not armed */

static lwp_t dog = LWP_THREAD_NULL;
static u8 dog_stack[8192] ATTRIBUTE_ALIGN(32);

void ESC_Now(void)
{
	u32 level;

	/* No cleanup on purpose: whatever is hung stays hung, and the stub
	 * reloads IOS, which resets USB, the network and everything else.
	 * This is what libogc's own reset handler does; its exit() instead
	 * runs SYS_ResetSystem(SYS_SHUTDOWN) first, which can block. */
	_CPU_ISR_Disable(level);
	(void)level;

	/* libogc accepts the tag at either offset, so do the same. */
	if ((STUB_MAGIC[0] == 0x53545542 && STUB_MAGIC[1] == 0x48415858) ||
	    (STUB_MAGIC[1] == 0x53545542 && STUB_MAGIC[2] == 0x48415858))
		((void (*)(void))STUB_ADDR)();

	_CPU_ISR_Restore(level);
	SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
	while (1)
		;
}

void ESC_Request(void)
{
	want_exit = true;
}

void ESC_Arm(u32 ms)
{
	deadline = gettime() + millisecs_to_ticks(ms);
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
		if (want_power)
			SYS_ResetSystem(SYS_POWEROFF, 0, 0);
		if (want_exit || (deadline && gettime() >= deadline))
			ESC_Now();
		usleep(50 * 1000);
	}

	return NULL;
}

void ESC_Init(void)
{
	if (dog != LWP_THREAD_NULL)
		return;

	SYS_SetResetCallback(on_reset);
	SYS_SetPowerCallback(on_power);

	/* Top priority, so a busy thread can't starve it. */
	LWP_CreateThread(&dog, dog_main, NULL, dog_stack, sizeof(dog_stack),
	                 LWP_PRIO_HIGHEST);
}

void ESC_WpadReady(void)
{
	WPAD_SetPowerButtonCallback(on_wpad_power);
}
