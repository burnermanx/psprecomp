/* Start-mode decision of the window (no window needed). */
#include "psprecomp/window.h"
#include <stdio.h>
#include <stdlib.h>
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "window_mode: line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
int main(void) {
    /* outside gamescope: explicit > remembered > window */
    REQUIRE(psp_window_resolve_start_mode(-1, 0, NULL) == 0);
    REQUIRE(psp_window_resolve_start_mode(-1, 0, "") == 0);
    REQUIRE(psp_window_resolve_start_mode(-1, 0, "fullscreen\n") == 1);
    REQUIRE(psp_window_resolve_start_mode(-1, 0, "window\n") == 0);
    REQUIRE(psp_window_resolve_start_mode(1, 0, "window\n") == 1);
    REQUIRE(psp_window_resolve_start_mode(0, 0, "fullscreen\n") == 0);
    /* gamescope: full screen even if the remembered mode is window */
    REQUIRE(psp_window_resolve_start_mode(-1, 1, NULL) == 1);
    REQUIRE(psp_window_resolve_start_mode(-1, 1, "window\n") == 1);
    REQUIRE(psp_window_resolve_start_mode(-1, 1, "fullscreen\n") == 1);
    /* gamescope: an explicit "window" still wins */
    REQUIRE(psp_window_resolve_start_mode(0, 1, "fullscreen\n") == 0);
    REQUIRE(psp_window_resolve_start_mode(0, 1, NULL) == 0);
    REQUIRE(psp_window_resolve_start_mode(1, 1, "window\n") == 1);
    puts("window_mode: ok");
    return 0;
}
