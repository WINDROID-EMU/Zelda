#ifndef ZELDA3_RA_BRIDGE_H
#define ZELDA3_RA_BRIDGE_H

#include <stdbool.h>

// Applies pending configuration/login changes. Must be called from the game
// thread (top of the main loop) because it creates/destroys the rc_client.
void RaProcessPending(void);

// Processes one emulated frame of achievements. Call after ZeldaRunFrame.
void RaFrame(void);

// Releases the rc_client. Call once before the game loop exits.
void RaShutdown(void);

// True while an Android dialog (achievements/login notifications) is covering
// the game. The main loop stops running frames while this is set.
bool RaIsDialogPaused(void);
void RaSetDialogPaused(bool paused);

#endif  // ZELDA3_RA_BRIDGE_H
