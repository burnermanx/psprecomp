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
 *   F11                                         fullscreen
 *   F9                                          dump GE/HLE census
 *   F12                                         capture a PPM (EMOTIONALSENSE_SHOT_DIR)
 *
 * SDL2 on Linux, Win32 + GDI + XInput on Windows. */
#ifndef PSPRECOMP_WINDOW_H
#define PSPRECOMP_WINDOW_H

/* Open the window, `scale` times the PSP's 480x272, or with `scale` 0 as
 * large as fits the screen. Returns 0 if it opened. */
int psp_window_start(const char *title, int scale);

/* Nonzero once the user has closed the window. */
int psp_window_closed(void);

#endif
