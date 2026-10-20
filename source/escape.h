/* A way out that works whatever state the app is in.
 *
 * RESET on the console returns to the loader immediately, POWER (console or
 * Wii Remote) turns the Wii off, and once a clean exit has started a watchdog
 * forces the return if it has not finished in time.  The watchdog thread runs
 * at top priority and does the jump itself, so a main loop stuck in a driver
 * call cannot hold it up.
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

/* Asks the watchdog to leave right now; safe from anywhere. */
void ESC_Request(void);

#endif /* ESCAPE_H */
