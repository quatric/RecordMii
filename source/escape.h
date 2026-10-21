/* A way out that works whatever state the app is in.
 *
 * RESET on the console returns to the loader immediately, POWER (console or
 * Wii Remote) turns the Wii off, and once a clean exit has started a watchdog
 * forces the return if it has not finished in time.  The watchdog thread runs
 * at top priority and does the jump itself, so a main loop stuck in a driver
 * call cannot hold it up.
 *
 * Leaving reloads IOS before jumping to the loader, so nothing the app left
 * pending in IOS can hold the loader up, and a second thread jumps anyway if
 * that reload stalls.
 */

#ifndef ESCAPE_H
#define ESCAPE_H

#include <gctypes.h>

/* Installs the button callbacks and starts the watchdog.  Call first thing. */
void ESC_Init(void);

/* Hooks the Wii Remote's power button; call once WPAD is up. */
void ESC_WpadReady(void);

/* A clean exit is under way: force it if it takes longer than `ms`. */
void ESC_Arm(u32 ms);

/* Leaves for the loader right now, skipping all cleanup.  Safe from any
 * thread (not from an interrupt handler). */
void ESC_Now(void) __attribute__((noreturn));

/* Called by ESC_Now just before it reloads IOS: should stop the app's own
 * threads from starting new IOS requests.  Must not block. */
void ESC_SetQuiesce(void (*fn)(void));

/* Leaves on its own if ESC_Feed() has not been called for `ms` (0 = off), so
 * an unattended test run always ends up back in the loader. */
void ESC_SetDeadman(u32 ms);
void ESC_Feed(void);

/* Asks the watchdog to leave right now; safe from anywhere. */
void ESC_Request(void);

#endif /* ESCAPE_H */
