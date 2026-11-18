/* psprecomp -- a window to play in.
 *
 * Shows the PSP display, scaled, and turns the keyboard and the first game
 * controller into sceCtrl input. It runs on a host thread of its own and
 * reads the game's front buffer on a 60 Hz timer, so it never waits on the
 * game and the game never waits on it.
 *
 *   Keyboard            Controller (XInput)     PSP
 *   arrows              d-pad                   d-pad
 *   (none)              left stick              analog stick
 *   Z                   A                       Cross
 *   X                   B                       Circle
 *   A                   X                       Square
 *   S                   Y                       Triangle
 *   Q / W               LB, LT / RB, RT         L / R
 *   Enter               Start                   Start
 *   Backspace           Back                    Select
 *   Alt+Enter                                   borderless full screen <-> window
 *   F11                                         full screen at the chosen rate <-> window
 *   F9                                          dump GE/HLE census
 *   F12                                         capture a PPM (EMOTIONALSENSE_SHOT_DIR)
 *
 * SDL2 on Linux, Win32 + GDI + XInput on Windows. */
#ifndef PSPRECOMP_WINDOW_H
#define PSPRECOMP_WINDOW_H

#include <stdint.h>

/* The window's icon, RGBA8888 (w*h*4 bytes, copied). Call before
 * psp_window_start; without it the system's default icon is used. */
void psp_window_set_icon(const unsigned char *rgba, int w, int h);

/* Open the window, `scale` times the PSP's 480x272, or with `scale` 0 as
 * large as fits the screen. Returns 0 if it opened. */
int psp_window_start(const char *title, int scale);

/* Full screen: Alt+Enter always toggles a borderless window over the desktop
 * (SDL_WINDOW_FULLSCREEN_DESKTOP); F11 does the same unless the user chose a
 * rate (EMOTIONALSENSE_REFRESH=auto|60|120), then it is exclusive at 60/120 Hz.
 * Call before psp_window_start. `mode`: 1 start in full screen, 0 in a window,
 * -1 (default) the remembered mode, else full screen under gamescope / on a
 * Steam Deck. The file (one word) holds the last mode; NULL/"" = not kept. */
void psp_window_set_start_mode(int mode);
void psp_window_set_state_file(const char *path);

/* Nonzero once the user has closed the window. */
int psp_window_closed(void);

/* Close it from the host (the game's own exit). */
void psp_window_request_close(void);

/* Escape: how many times it was pressed since the last call (and cleared). The
 * game never sees it as a button; a host feature may use it as "back". */
int psp_window_take_escape(void);

/* A host overlay drawn over the presented frame (menus of the host, not of the
 * game). `fn` runs on the present thread for every presented frame with the
 * frame's RGBA8 pixels (0xAABBGGRR) in a scratch copy, `w`x`h` (480x272 times
 * the internal scale); it only has to draw when it has something to show.
 * `active` says whether it has: when it returns 0 the frame is shown as is. */
typedef int (*psp_overlay_fn)(uint32_t *px, int w, int h);
void psp_window_set_overlay(psp_overlay_fn fn);

/* Drawable height in pixels, or 0 when there is no window. */
int psp_window_height(void);

#endif
