/* TCP debug console, so the Wii Speak can be poked at from a computer instead
 * of from the couch.  tools/wsdbg.py is the matching client.
 *
 * The protocol is line-based text on port NDBG_PORT.  Every reply ends with a
 * line starting "OK" or "ERR"; a binary payload is announced by a line
 * "BIN <bytes> [info...]" and the raw bytes follow it, before the final OK.
 */

#ifndef NETDBG_H
#define NETDBG_H

#include <gctypes.h>
#include <stddef.h>

#define NDBG_PORT 4300

/* Mirrors stdout/stderr into an in-memory log the client can read.  Call it
 * once the console is up, so everything printed afterwards is captured. */
void NDBG_HookStdout(void);

/* While quiet, writes still reach the screen but are kept out of the log --
 * for the status block the main loop redraws every frame. */
void NDBG_Quiet(bool quiet);

/* Starts the server thread.  It brings the network up itself if nothing has
 * yet, so it can be called right after the IOS is settled.  With a USB
 * Gecko in slot B the same console runs over that as well. */
void NDBG_Start(void);

/* Closes the listening socket and takes the network down, on the way out. */
void NDBG_Stop(void);

/* True when the console also runs over a USB Gecko in slot B. */
bool NDBG_GeckoActive(void);

/* The server's address once it is listening, else NULL. */
const char *NDBG_Address(void);

/* Commands the server hands to the main loop, so they run on the thread that
 * owns the app's state.  The handler writes a reply (without the trailing OK
 * line) and returns false to have it sent as an ERR. */
typedef bool (*ndbg_handler)(const char *line, char *reply, size_t size);

/* Runs a pending main-loop command, if any.  Call once a frame. */
void NDBG_Poll(ndbg_handler handler);

/* Hands the samples the app is about to record to a pending `pcm` capture. */
void NDBG_FeedPcm(const s16 *samples, u32 count, u32 rate);

#endif /* NETDBG_H */
