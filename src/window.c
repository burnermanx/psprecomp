/* psprecomp -- a window to play in. See include/psprecomp/window.h. */

#include "psprecomp/window.h"
#include "psprecomp/log.h"
#include "psprecomp/hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Start mode: an explicit choice (0 window, 1 full screen) wins; under gamescope
 * full screen otherwise, whatever was remembered (the remembered mode only
 * counts outside a gamescope session); else the remembered word ("fullscreen" /
 * "window"), else a window. Pure, so it is tested without a window. */
int psp_window_resolve_start_mode(int explicit_mode, int gamescope, const char *remembered) {
    if (explicit_mode >= 0) return explicit_mode ? 1 : 0;
    if (gamescope) return 1;
    if (remembered && remembered[0]) return remembered[0] == 'f';
    return 0;
}

#if defined(_WIN32) && !defined(PSPRECOMP_SDL2)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>

#define W 480
#define H 272

/* PSP button bits (SceCtrlData.Buttons). */
enum {
    B_SELECT = 0x0001, B_START = 0x0008,
    B_UP = 0x0010, B_RIGHT = 0x0020, B_DOWN = 0x0040, B_LEFT = 0x0080,
    B_L = 0x0100, B_R = 0x0200,
    B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000, B_CROSS = 0x4000, B_SQUARE = 0x8000
};

static volatile LONG g_closed;
static int g_scale;
static const char *g_title = "psprecomp";
static uint32_t g_px[W * H];
static unsigned char *g_icon;
static int g_icon_w, g_icon_h;

void psp_window_set_icon(const unsigned char *rgba, int w, int h) {
    free(g_icon);
    g_icon = (unsigned char *)malloc((size_t)w * (size_t)h * 4);
    if (!g_icon) return;
    memcpy(g_icon, rgba, (size_t)w * (size_t)h * 4);
    g_icon_w = w; g_icon_h = h;
}

/* HICON from the RGBA pixels: a 32-bit BGRA colour bitmap plus an empty mask. */
static HICON make_icon(void) {
    if (!g_icon) return NULL;
    const int w = g_icon_w, hh = g_icon_h;
    BITMAPV5HEADER bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bV5Size = sizeof bi; bi.bV5Width = w; bi.bV5Height = -hh;
    bi.bV5Planes = 1; bi.bV5BitCount = 32; bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color || !bits) return NULL;
    uint32_t *d = (uint32_t *)bits;
    for (int i = 0; i < w * hh; i++) {
        const unsigned char *p = g_icon + (size_t)i * 4;
        d[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    HBITMAP mask = CreateBitmap(w, hh, 1, 1, NULL);
    ICONINFO ii;
    ZeroMemory(&ii, sizeof ii);
    ii.fIcon = TRUE; ii.hbmMask = mask; ii.hbmColor = color;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return ic;
}

static uint32_t keyboard(void) {
    static const struct { int vk; uint32_t bit; } K[] = {
        { VK_UP, B_UP }, { VK_RIGHT, B_RIGHT }, { VK_DOWN, B_DOWN }, { VK_LEFT, B_LEFT },
        { 'Z', B_CROSS }, { 'X', B_CIRCLE }, { 'A', B_SQUARE }, { 'S', B_TRIANGLE },
        { 'Q', B_L }, { 'W', B_R }, { VK_RETURN, B_START }, { VK_BACK, B_SELECT },
    };
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++)
        if (GetAsyncKeyState(K[i].vk) & 0x8000) b |= K[i].bit;
    return b;
}

/* E / R: the stick pushed left / right (both = centre). */
static void keyboard_stick(uint8_t *ax) {
    const int l = (GetAsyncKeyState('E') & 0x8000) != 0, r = (GetAsyncKeyState('R') & 0x8000) != 0;
    if (l != r) *ax = r ? 255 : 0;
}

static volatile int g_xinput_seen;
int psp_window_pad_kind(void) { return g_xinput_seen ? 1 : 0; }   /* XInput: Xbox buttons */

/* The first XInput controller, if one is connected. */
static uint32_t pad(uint8_t *ax, uint8_t *ay, uint32_t *trig, int *moved) {
    static const struct { WORD x; uint32_t bit; } P[] = {
        { XINPUT_GAMEPAD_DPAD_UP, B_UP }, { XINPUT_GAMEPAD_DPAD_RIGHT, B_RIGHT },
        { XINPUT_GAMEPAD_DPAD_DOWN, B_DOWN }, { XINPUT_GAMEPAD_DPAD_LEFT, B_LEFT },
        { XINPUT_GAMEPAD_A, B_CROSS }, { XINPUT_GAMEPAD_B, B_CIRCLE },
        { XINPUT_GAMEPAD_X, B_SQUARE }, { XINPUT_GAMEPAD_Y, B_TRIANGLE },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, B_L }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, B_R },
        { XINPUT_GAMEPAD_START, B_START }, { XINPUT_GAMEPAD_BACK, B_SELECT },
    };
    XINPUT_STATE s;
    if (XInputGetState(0, &s) != ERROR_SUCCESS) return 0;
    g_xinput_seen = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
        if (s.Gamepad.wButtons & P[i].x) b |= P[i].bit;
    if (s.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) *trig |= PSP_TRIG_LEFT;
    if (s.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) *trig |= PSP_TRIG_RIGHT;
    /* The PSP stick is 0..255 with 128 at rest and y growing downwards. */
    const int lx = s.Gamepad.sThumbLX, ly = s.Gamepad.sThumbLY;
    if (lx * lx + ly * ly > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE * XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) {
        *ax = (uint8_t)((lx + 32768) >> 8);
        *ay = (uint8_t)((32767 - ly) >> 8);
        *moved = 1;
    }
    return b;
}

static void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT r;
    GetClientRect(h, &r);
    const int cw = r.right, ch = r.bottom;
    /* Keep the PSP's aspect, black bars around it. */
    int w = cw, hh = cw * H / W;
    if (hh > ch) { hh = ch; w = ch * W / H; }
    const int x0 = (cw - w) / 2, y0 = (ch - hh) / 2;
    PatBlt(dc, 0, 0, cw, y0, BLACKNESS);
    PatBlt(dc, 0, y0 + hh, cw, ch - y0 - hh, BLACKNESS);
    PatBlt(dc, 0, 0, x0, ch, BLACKNESS);
    PatBlt(dc, x0 + w, 0, cw - x0 - w, ch, BLACKNESS);

    if (psp_display_read(g_px) == 0) {
        for (int i = 0; i < W * H; i++) {      /* 0xAABBGGRR to the DIB's BGRA */
            const uint32_t c = g_px[i];
            g_px[i] = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
        }
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;            /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);   /* nearest: crisp pixels */
        StretchDIBits(dc, x0, y0, w, hh, 0, 0, W, H, g_px, &bi, DIB_RGB_COLORS, SRCCOPY);
    } else {
        PatBlt(dc, x0, y0, w, hh, BLACKNESS);
    }
    EndPaint(h, &ps);
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER: {
        uint8_t ax = 128, ay = 128;
        uint32_t b, trig = 0;
        int moved = 0;
        b = pad(&ax, &ay, &trig, &moved);
        if (GetForegroundWindow() == h) {                   /* keys only when focused */
            b |= keyboard();
            if (!moved) keyboard_stick(&ax);
        }
        psp_ctrl_set_ex(b, ax, ay, trig, moved);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_PAINT:      paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY:    PostQuitMessage(0); return 0;
    default:            return DefWindowProcA(h, msg, wp, lp);
    }
}

static DWORD WINAPI window_main(LPVOID arg) {
    HANDLE ready = (HANDLE)arg;
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "psprecomp_window";
    RegisterClassA(&wc);

    /* Per-monitor pixels, not Windows' blurry bitmap stretch of a DPI-unaware
     * window. With no scale asked for, take the largest whole multiple that
     * fits most of the screen's height. */
    SetProcessDPIAware();
    if (g_scale <= 0) {
        RECT wa;
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        g_scale = (int)((wa.bottom - wa.top) * 7 / 10 / H);
        if (g_scale < 1) g_scale = 1;
    }
    RECT r = { 0, 0, W * g_scale, H * g_scale };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND h = CreateWindowA(wc.lpszClassName, g_title, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                           NULL, NULL, wc.hInstance, NULL);
    if (!h) { InterlockedExchange(&g_closed, 1); SetEvent(ready); return 1; }
    {
        HICON ic = make_icon();
        if (ic) {
            SendMessageA(h, WM_SETICON, ICON_BIG, (LPARAM)ic);
            SendMessageA(h, WM_SETICON, ICON_SMALL, (LPARAM)ic);
        }
    }
    SetTimer(h, 1, 16, NULL);
    SetEvent(ready);

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    InterlockedExchange(&g_closed, 1);
    return 0;
}

int psp_window_start(const char *title, int scale) {
    if (title) g_title = title;
    g_scale = scale;
    HANDLE ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, window_main, ready, 0, NULL);
    if (!t) { CloseHandle(ready); return -1; }
    WaitForSingleObject(ready, INFINITE);       /* the window exists, or failed */
    CloseHandle(ready);
    CloseHandle(t);
    return g_closed ? -1 : 0;
}

int psp_window_closed(void) { return g_closed != 0; }
void psp_window_request_close(void) { InterlockedExchange(&g_closed, 1); }
int psp_window_take_escape(void) { return 0; }
void psp_window_set_overlay(psp_overlay_fn fn) { (void)fn; }
int psp_window_overlay_apply(uint32_t *px, int w, int h) { (void)px; (void)w; (void)h; return 0; }
void psp_window_set_state_file(const char *path) { (void)path; }
void psp_window_set_start_mode(int mode) { (void)mode; }

#elif defined(PSPRECOMP_SDL2)

/* SDL2 backend (Linux and anything else SDL runs on). Same shape as the Win32
 * one: a host thread of its own that owns the window, polls input into
 * sceCtrl and presents the game's front buffer at ~60 Hz, never waiting on
 * the game. */

#include <SDL.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define W 480
#define H 272

enum {
    B_SELECT = 0x0001, B_START = 0x0008,
    B_UP = 0x0010, B_RIGHT = 0x0020, B_DOWN = 0x0040, B_LEFT = 0x0080,
    B_L = 0x0100, B_R = 0x0200,
    B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000, B_CROSS = 0x4000, B_SQUARE = 0x8000
};

static atomic_int g_closed;
static atomic_int g_ready;          /* 0 pending, 1 open, -1 failed */
static int g_scale;
static const char *g_title = "psprecomp";
/* Max internal resolution is 4x (see psp_render_scale). */
static uint32_t g_px[W * H * 16];
static unsigned char *g_icon;
static int g_icon_w, g_icon_h;

void psp_window_set_icon(const unsigned char *rgba, int w, int h) {
    free(g_icon);
    g_icon = (unsigned char *)malloc((size_t)w * (size_t)h * 4);
    if (!g_icon) return;
    memcpy(g_icon, rgba, (size_t)w * (size_t)h * 4);
    g_icon_w = w; g_icon_h = h;
}

/* Host keyboard and gamepad are a convenience when someone is playing, and a
 * source of non-determinism when a script is driving: a connected pad with a
 * drifting stick (or a key held down) injects buttons the script never asked
 * for, and the run walks through the menus on its own. Off by default when a
 * script or the dummy video driver is in use; EMOTIONALSENSE_HOST_INPUT=0/1
 * forces it either way. */
static int host_input_enabled(void) {
    static int on = -1;
    if (on >= 0) return on;
    const char *e = getenv("EMOTIONALSENSE_HOST_INPUT");
    if (e && *e) { on = e[0] != '0'; return on; }
    const char *drv = getenv("SDL_VIDEODRIVER");
    on = !(getenv("EMOTIONALSENSE_SCRIPT") || (drv && strcmp(drv, "dummy") == 0));
    return on;
}

static uint32_t keyboard(void) {
    static const struct { SDL_Scancode sc; uint32_t bit; } K[] = {
        { SDL_SCANCODE_UP, B_UP }, { SDL_SCANCODE_RIGHT, B_RIGHT },
        { SDL_SCANCODE_DOWN, B_DOWN }, { SDL_SCANCODE_LEFT, B_LEFT },
        { SDL_SCANCODE_Z, B_CROSS }, { SDL_SCANCODE_X, B_CIRCLE },
        { SDL_SCANCODE_A, B_SQUARE }, { SDL_SCANCODE_S, B_TRIANGLE },
        { SDL_SCANCODE_Q, B_L }, { SDL_SCANCODE_W, B_R },
        { SDL_SCANCODE_RETURN, B_START }, { SDL_SCANCODE_BACKSPACE, B_SELECT },
    };
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++)
        if (ks[K[i].sc]) b |= K[i].bit;
    /* Alt+Enter is the full screen toggle, not START. */
    if (SDL_GetModState() & KMOD_ALT) b &= ~(uint32_t)B_START;
    return b;
}

/* E / R: the stick pushed left / right (both = centre). */
static void keyboard_stick(uint8_t *ax) {
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    const int l = ks[SDL_SCANCODE_E] != 0, r = ks[SDL_SCANCODE_R] != 0;
    if (l != r) *ax = r ? 255 : 0;
}

/* What kind of controller is this, for a host that draws its button names (1 xbox,
 * 2 playstation, 3 switch, 4 steam deck). SDL knows the real type of a physical
 * controller, and of a Steam Input one when the Steam client describes it
 * (SteamVirtualGamepadInfo, SDL 2.30+/sdl2-compat); the Steam virtual pad
 * itself (28de:11ff) says "Xbox 360" and is a Steam Deck's when SteamDeck=1. */
static int pad_kind(SDL_GameController *gc) {
    const Uint16 vid = SDL_GameControllerGetVendor(gc), pid = SDL_GameControllerGetProduct(gc);
    int kind = 1;
    switch (SDL_GameControllerGetType(gc)) {
    case SDL_CONTROLLER_TYPE_PS3: case SDL_CONTROLLER_TYPE_PS4: case SDL_CONTROLLER_TYPE_PS5:
        kind = 2; break;
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 24, 0)
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
        kind = 3; break;
    default: break;
    }
    const char *sd = getenv("SteamDeck");
    const int deck_env = sd && *sd && strcmp(sd, "0") != 0;
    if (vid == 0x28de && pid == 0x1205) kind = 4;                         /* the Deck's own pad */
    else if (kind == 1 && deck_env && vid == 0x28de) kind = 4;           /* Steam's virtual pad on a Deck */
    return kind;
}

#define MAX_PADS 4
static atomic_int g_pad_kind;       /* kind of the controller that last pressed something, 0 none */
int psp_window_pad_kind(void) { return atomic_load(&g_pad_kind); }

static uint32_t pad(SDL_GameController *gc, uint8_t *ax, uint8_t *ay, uint32_t *trig, int *moved) {
    static const struct { SDL_GameControllerButton btn; uint32_t bit; } P[] = {
        { SDL_CONTROLLER_BUTTON_DPAD_UP, B_UP }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, B_RIGHT },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN, B_DOWN }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, B_LEFT },
        { SDL_CONTROLLER_BUTTON_A, B_CROSS }, { SDL_CONTROLLER_BUTTON_B, B_CIRCLE },
        { SDL_CONTROLLER_BUTTON_X, B_SQUARE }, { SDL_CONTROLLER_BUTTON_Y, B_TRIANGLE },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, B_L }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, B_R },
        { SDL_CONTROLLER_BUTTON_START, B_START }, { SDL_CONTROLLER_BUTTON_BACK, B_SELECT },
    };
    if (!gc) return 0;
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
        if (SDL_GameControllerGetButton(gc, P[i].btn)) b |= P[i].bit;
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 8000) *trig |= PSP_TRIG_LEFT;
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000) *trig |= PSP_TRIG_RIGHT;
    /* The PSP stick is 0..255 with 128 at rest and y growing downwards --
     * the same direction as SDL's. */
    const int lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
    const int ly = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
    if (lx * lx + ly * ly > 7849 * 7849) {
        *ax = (uint8_t)((lx + 32768) >> 8);
        *ay = (uint8_t)((ly + 32768) >> 8);
        *moved = 1;
    }
    return b;
}

/* Whole pixels only when the window is an exact multiple of the frame, which
 * is the case at 1x and at 2x on a 1440p screen. At 4x the frame is 1088 tall
 * and a 1080p window cannot hold it whole, so let SDL scale by aspect instead
 * of cropping the bottom rows. */
static atomic_int g_win_h;          /* drawable height, for --internal-scale auto */
static atomic_int g_desk_h;         /* desktop (output) height */

static void fit_scale(SDL_Renderer *ren, int w, int h) {
    int dw = 0, dh = 0;
    SDL_GetRendererOutputSize(ren, &dw, &dh);
    atomic_store(&g_win_h, dh);
    SDL_RenderSetIntegerScale(ren, (dw > 0 && dh > 0 && dw % w == 0 && dh % h == 0)
                                   ? SDL_TRUE : SDL_FALSE);
}

static SDL_Window *g_win;
static FILE *g_trace;
static atomic_int g_present_state;  /* 0 starting, 1 running, -1 failed */
static atomic_int g_dirty = 1;      /* redraw the last frame (resize, expose) */
static atomic_int g_refit;          /* the window size changed: refit the scale */
static atomic_int g_esc;            /* Escape presses not yet taken by the host */
static psp_overlay_fn g_overlay;
static uint32_t *g_px_ov;           /* scratch copy the overlay draws on */

/* Owns the SDL renderer: copies the guest's newest frame to the screen. It
 * blocks in SDL_RenderPresent (vsync) without holding up anything else. */
static int present_main(void *arg) {
    (void)arg;
    SDL_Window *win = g_win;
    const char *vsync = getenv("EMOTIONALSENSE_VSYNC");
    Uint32 rflags = SDL_RENDERER_ACCELERATED;
    if (!(vsync && vsync[0] == '0')) rflags |= SDL_RENDERER_PRESENTVSYNC;
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, rflags);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    int tex_w = W, tex_h = H;
    psp_display_size(&tex_w, &tex_h);
    SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                                               SDL_TEXTUREACCESS_STREAMING, tex_w, tex_h) : NULL;
    if (!tex) {
        psp_log("psprecomp: cannot open a window: %s\n", SDL_GetError());
        atomic_store(&g_present_state, -1);
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");          /* nearest: crisp pixels */
    SDL_RenderSetLogicalSize(ren, tex_w, tex_h);               /* keep the aspect, black bars */
    fit_scale(ren, tex_w, tex_h);
    atomic_store(&g_present_state, 1);

    uint32_t snap_seq = 0;
    FILE *tp = g_trace;
    while (!atomic_load(&g_closed)) {
        if (atomic_exchange(&g_refit, 0)) fit_scale(ren, tex_w, tex_h);
        /* Present only when the guest produced a frame (or the window changed),
         * so the screen rate never paces the game. */
        uint32_t prev_seq = snap_seq;
        int fresh = psp_display_snapshot(g_px, &snap_seq);
        int dirty = atomic_exchange(&g_dirty, 0) || fresh;
        if (dirty) {
            int fw, fh;
            psp_display_size(&fw, &fh);
            if (fw != tex_w || fh != tex_h) {
                /* The frame size only changes when --internal-scale does. */
                SDL_Texture *nt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                                                    SDL_TEXTUREACCESS_STREAMING, fw, fh);
                if (nt) {
                    SDL_DestroyTexture(tex);
                    tex = nt;
                    tex_w = fw; tex_h = fh;
                    SDL_RenderSetLogicalSize(ren, fw, fh);
                    fit_scale(ren, fw, fh);
                }
            }
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            const uint32_t *src = g_px;
            if (g_overlay) {
                /* Draw on a copy: a redraw without a new frame would otherwise
                 * blend the overlay over its own previous result. */
                if (!g_px_ov) g_px_ov = (uint32_t *)malloc(sizeof g_px);
                if (g_px_ov) {
                    memcpy(g_px_ov, g_px, (size_t)tex_w * (size_t)tex_h * 4);
                    if (g_overlay(g_px_ov, tex_w, tex_h)) {
                        src = g_px_ov;
                        /* EMOTIONALSENSE_OVERLAY_SHOT=<file.ppm>: keep the first
                         * overlaid frame (a test aid: the compositor is not
                         * capturable from a script). */
                        static int shot_done; const char *sp;
                        /* EMOTIONALSENSE_OVERLAY_SHOT_EVERY=N: instead of only the first, one overlaid frame
                         * every N vblanks, as <file>.<vblank>.ppm */
                        static unsigned long long last_slot = ~0ull;
                        const char *ev = getenv("EMOTIONALSENSE_OVERLAY_SHOT_EVERY");
                        const unsigned long long evn = ev ? strtoull(ev, NULL, 10) : 0;
                        const unsigned long long vbn = psp_display_vblanks();
                        char shotname[1100];
                        int take = 0;
                        sp = getenv("EMOTIONALSENSE_OVERLAY_SHOT");
                        if (sp && *sp) {
                            if (evn) {
                                if (vbn / evn != last_slot) {
                                    last_slot = vbn / evn;
                                    snprintf(shotname, sizeof shotname, "%s.%llu.ppm", sp, vbn);
                                    take = 1;
                                }
                            } else if (!shot_done) {
                                snprintf(shotname, sizeof shotname, "%s", sp);
                                take = 1;
                            }
                        }
                        if (take) {
                            FILE *f = fopen(shotname, "wb");
                            if (f) {
                                fprintf(f, "P6\n%d %d\n255\n", tex_w, tex_h);
                                for (int i = 0; i < tex_w * tex_h; i++) {
                                    uint32_t c = g_px_ov[i];
                                    unsigned char rgb[3] = { (unsigned char)c, (unsigned char)(c >> 8), (unsigned char)(c >> 16) };
                                    fwrite(rgb, 1, 3, f);
                                }
                                fclose(f);
                                shot_done = 1;
                            }
                        }
                    }
                }
            }
            SDL_UpdateTexture(tex, NULL, src, tex_w * 4);   /* 0xAABBGGRR == ABGR8888 */
            SDL_RenderCopy(ren, tex, NULL, NULL);
            uint64_t t_before = tp ? psp_host_mono_us() : 0;
            SDL_RenderPresent(ren);
            if (tp && fresh) {
                uint64_t t_after = psp_host_mono_us();
                fprintf(tp, "P %llu %llu %llu %u\n", (unsigned long long)t_before,
                        (unsigned long long)t_after,
                        (unsigned long long)psp_display_snap_us(),
                        (snap_seq - prev_seq) / 2 - 1);
                fflush(tp);
            }
        }
        SDL_Delay(1);
    }
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    return 0;
}

/* Exclusive full screen at a rate the guest divides evenly into: the guest runs
 * at 59.94 Hz, so a 60 Hz mode repeats a frame about once in 17 s and a 120 Hz
 * mode shows every frame for exactly two refreshes, while 75 or 144 Hz
 * repeat frames unevenly. EMOTIONALSENSE_REFRESH = auto (60, else 120, else the
 * desktop mode) | 60 | 120 | native (the desktop mode, no mode change).
 * Windowed, the desktop's rate is whatever it is. Only a user who sets one of
 * those (auto, 60, 120) gets the exclusive mode; with none, or "native", full
 * screen is a borderless window over the desktop. Returns 1 if a mode was set. */
static int pick_refresh_mode(SDL_Window *win, SDL_DisplayMode *out) {
    const char *e = getenv("EMOTIONALSENSE_REFRESH");
    int want[2] = { 60, 120 }, n = 2;
    /* No rate chosen (or "native"): no mode change, full screen is borderless. */
    if (!e || !*e || !strcmp(e, "native")) return 0;
    if (e && !strcmp(e, "60"))  { want[0] = 60;  n = 1; }
    if (e && !strcmp(e, "120")) { want[0] = 120; n = 1; }
    int di = SDL_GetWindowDisplayIndex(win);
    SDL_DisplayMode desk;
    if (di < 0 || SDL_GetDesktopDisplayMode(di, &desk) != 0) return 0;
    int modes = SDL_GetNumDisplayModes(di);
    for (int k = 0; k < n; k++) {
        int best = -1, bd = 1 << 30;
        for (int i = 0; i < modes; i++) {
            SDL_DisplayMode m;
            if (SDL_GetDisplayMode(di, i, &m) != 0 || m.w != desk.w || m.h != desk.h) continue;
            int d = m.refresh_rate > want[k] ? m.refresh_rate - want[k] : want[k] - m.refresh_rate;
            if (d <= 1 && d < bd) { best = i; bd = d; }
        }
        if (best >= 0) { SDL_GetDisplayMode(di, best, out); return 1; }
    }
    return 0;
}

static int is_fullscreen(SDL_Window *win) {
    return (SDL_GetWindowFlags(win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
}

/* Where the last mode is remembered (a one-word file), or NULL. */
static char g_state_path[1024];
static int g_start_mode = -1;       /* -1 remember/auto, 0 window, 1 full screen */

void psp_window_set_state_file(const char *path) {
    snprintf(g_state_path, sizeof g_state_path, "%s", path ? path : "");
}
void psp_window_set_start_mode(int mode) { g_start_mode = mode; }

static void remember_fullscreen(int on) {
    if (!g_state_path[0]) return;
    FILE *f = fopen(g_state_path, "w");
    if (f) { fputs(on ? "fullscreen\n" : "window\n", f); fclose(f); }
}

/* gamescope (Steam Deck game mode) draws one full-screen surface and a window
 * there is pointless: start full screen unless told otherwise. */
static int on_gamescope(void) {
    const char *d = getenv("XDG_CURRENT_DESKTOP"), *sd = getenv("SteamDeck");
    return getenv("GAMESCOPE_WAYLAND_DISPLAY") || (sd && *sd && strcmp(sd, "0")) ||
           (d && strstr(d, "gamescope"));
}

static int want_fullscreen_at_start(void) {
    char b[16] = "";
    if (g_state_path[0]) {
        FILE *f = fopen(g_state_path, "r");
        if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); }
    }
    return psp_window_resolve_start_mode(g_start_mode, on_gamescope(), b);
}

/* Borderless full screen over the desktop: no mode change, the swapchain (the
 * SDL renderer here) follows the size event, textures are not touched. */
static void set_borderless(SDL_Window *win, int on) {
    SDL_SetWindowFullscreen(win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    remember_fullscreen(on);
}

/* Alt+Enter: window <-> borderless, always. */
static void toggle_borderless(SDL_Window *win) {
    int on = !is_fullscreen(win);
    set_borderless(win, on);
    SDL_DisplayMode d;
    if (on && SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(win), &d) == 0)
        psp_log("window: borderless full screen %dx%d at %d Hz\n", d.w, d.h, d.refresh_rate);
    else if (!on) psp_log("window: windowed\n");
}

/* F11: window <-> full screen at the rate the user chose (exclusive 60/120 Hz
 * when EMOTIONALSENSE_REFRESH is auto/60/120, otherwise borderless). */
static void toggle_fullscreen(SDL_Window *win) {
    if (is_fullscreen(win)) {
        SDL_SetWindowFullscreen(win, 0);
        remember_fullscreen(0);
        psp_log("window: windowed\n");
        return;
    }
    SDL_DisplayMode m;
    if (pick_refresh_mode(win, &m) && SDL_SetWindowDisplayMode(win, &m) == 0 &&
        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN) == 0) {
        remember_fullscreen(1);
        psp_log("window: exclusive full screen %dx%d at %d Hz\n", m.w, m.h, m.refresh_rate);
        return;
    }
    set_borderless(win, 1);
    SDL_DisplayMode d;
    if (SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(win), &d) == 0)
        psp_log("window: borderless full screen %dx%d at %d Hz\n", d.w, d.h, d.refresh_rate);
}

static int window_main(void *arg) {
    (void)arg;
    /* Without this Windows reports a scaled size and blurs the nearest-neighbour
     * pixels; the hint is ignored off Windows. */
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS   /* SDL >= 2.24; older SDL2 (Ubuntu 22.04: 2.0.20) lacks it */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    /* Positional face buttons: the bottom one is always the same button, whatever is
     * printed on it (a Nintendo pad would otherwise swap A/B and X/Y in SDL). Set before the
     * init; a value the user put in the environment stays. */
    SDL_SetHintWithPriority("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0", SDL_HINT_DEFAULT);
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        psp_log("psprecomp: SDL_Init: %s\n", SDL_GetError());
        atomic_store(&g_closed, 1); atomic_store(&g_ready, -1);
        return 1;
    }
    if (g_scale <= 0) {
        /* A window smaller than the frame would downscale it by nearest and
         * lose pixels, so never go below the internal resolution. */
        SDL_Rect usable;
        int fw = W, fh = H;
        psp_display_size(&fw, &fh);
        g_scale = fw / W;
        if (SDL_GetDisplayUsableBounds(0, &usable) == 0) {
            int fit = usable.h * 7 / 10 / H;
            if (fit < 1) fit = 1;
            if (g_scale < fit) g_scale = fit;
            if (g_scale * H > usable.h) g_scale = usable.h / H;
            if (g_scale < 1) g_scale = 1;
        }
    }
    const int start_fs = want_fullscreen_at_start();
    SDL_Window *win = SDL_CreateWindow(g_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       W * g_scale, H * g_scale,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                       (start_fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (win && g_icon) {
        SDL_Surface *is = SDL_CreateRGBSurfaceWithFormatFrom(g_icon, g_icon_w, g_icon_h, 32, g_icon_w * 4,
                                                             SDL_PIXELFORMAT_RGBA32);
        if (is) { SDL_SetWindowIcon(win, is); SDL_FreeSurface(is); }
    }
    g_win = win;
    if (win) {
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(win), &dm) == 0) atomic_store(&g_desk_h, dm.h);
    }
    if (win && getenv("EMOTIONALSENSE_TRACE_MODES")) {
        int di = SDL_GetWindowDisplayIndex(win);
        SDL_DisplayMode d, p;
        SDL_GetDesktopDisplayMode(di, &d);
        psp_log("window: display %d, desktop %dx%d at %d Hz; modes:", di, d.w, d.h, d.refresh_rate);
        for (int i = 0; i < SDL_GetNumDisplayModes(di); i++)
            if (SDL_GetDisplayMode(di, i, &p) == 0 && p.w == d.w && p.h == d.h) psp_log(" %d", p.refresh_rate);
        psp_log("\n");
        if (pick_refresh_mode(win, &p)) psp_log("window: refresh option picks %d Hz\n", p.refresh_rate);
        else psp_log("window: refresh option keeps the desktop mode\n");
    }
    if (!win) {
        psp_log("psprecomp: cannot open a window: %s\n", SDL_GetError());
        atomic_store(&g_closed, 1); atomic_store(&g_ready, -1);
        return 1;
    }
    /* EMOTIONALSENSE_TRACE_PRESENT=<file>: one line per presented frame,
     * "P <before-present us> <after-present us> <snapshot copied us> <snapshots
     * skipped>", and "G <us> <gap us>" whenever the input loop did not poll
     * for more than 3 ms. Times are host monotonic microseconds. */
    {
        const char *f = getenv("EMOTIONALSENSE_TRACE_PRESENT");
        if (f && *f) g_trace = fopen(f, "w");
    }
    /* The renderer lives on a thread of its own: SDL_RenderPresent blocks until
     * the monitor's next refresh, and the thread that reads the buttons must
     * not wait for that (the guest would read buttons up to one refresh old). */
    SDL_Thread *pt = SDL_CreateThread(present_main, "psp_present", NULL);
    if (!pt) { atomic_store(&g_closed, 1); atomic_store(&g_ready, -1); return 1; }
    while (atomic_load(&g_present_state) == 0) SDL_Delay(1);
    if (atomic_load(&g_present_state) < 0) {
        atomic_store(&g_closed, 1); atomic_store(&g_ready, -1);
        SDL_WaitThread(pt, NULL);
        return 1;
    }
    atomic_store(&g_ready, 1);

    SDL_GameController *gcs[MAX_PADS] = { NULL };
    uint64_t last_poll_us = 0;
    while (!atomic_load(&g_closed)) {
        if (g_trace) {
            uint64_t n = psp_host_mono_us();
            if (last_poll_us && n - last_poll_us > 3000) fprintf(g_trace, "G %llu %llu\n",
                (unsigned long long)n, (unsigned long long)(n - last_poll_us));
            last_poll_us = n;
        }
        /* EMOTIONALSENSE_TEST_ALTENTER="ms,ms,...": synthesise Alt+Enter that many
         * ms after the loop starts (there is no way to type into a headless
         * test run). It goes through the same event branch as the keyboard. */
        {
            static const char *list; static Uint32 t0; static int init;
            if (!init) { init = 1; list = getenv("EMOTIONALSENSE_TEST_ALTENTER"); t0 = SDL_GetTicks(); }
            if (list && *list && SDL_GetTicks() - t0 >= (Uint32)strtoul(list, NULL, 10)) {
                SDL_Event k;
                SDL_zero(k);
                k.type = SDL_KEYDOWN; k.key.state = SDL_PRESSED;
                k.key.keysym.sym = SDLK_RETURN; k.key.keysym.scancode = SDL_SCANCODE_RETURN;
                k.key.keysym.mod = KMOD_LALT;
                SDL_PushEvent(&k);
                while (*list && *list != ',') list++;
                if (*list == ',') list++;
            }
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: atomic_store(&g_closed, 1); break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
                    if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        atomic_store(&g_refit, 1);
                        if (getenv("EMOTIONALSENSE_TRACE_MODES"))
                            psp_log("window: size %dx%d%s\n", ev.window.data1, ev.window.data2,
                                    is_fullscreen(win) ? " (full screen)" : " (window)");
                    }
                    atomic_store(&g_dirty, 1);
                }
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (host_input_enabled())
                    for (int i = 0; i < MAX_PADS; i++)
                        if (!gcs[i]) {
                            gcs[i] = SDL_GameControllerOpen(ev.cdevice.which);
                            if (gcs[i] && getenv("EMOTIONALSENSE_TRACE_PAD"))
                                psp_log("pad: opened '%s' type=%d vid=%04x pid=%04x kind=%d\n",
                                        SDL_GameControllerName(gcs[i]), (int)SDL_GameControllerGetType(gcs[i]),
                                        SDL_GameControllerGetVendor(gcs[i]), SDL_GameControllerGetProduct(gcs[i]),
                                        pad_kind(gcs[i]));
                            break;
                        }
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                for (int i = 0; i < MAX_PADS; i++)
                    if (gcs[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gcs[i])) == ev.cdevice.which) {
                        SDL_GameControllerClose(gcs[i]); gcs[i] = NULL;
                    }
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_ESCAPE) {
                    if (!ev.key.repeat) atomic_fetch_add(&g_esc, 1);
                } else if (ev.key.keysym.sym == SDLK_F11) {
                    toggle_fullscreen(win);
                } else if ((ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER) &&
                           (ev.key.keysym.mod & KMOD_ALT) && !ev.key.repeat) {
                    toggle_borderless(win);
                } else if (ev.key.keysym.sym == SDLK_F9) {
                    psp_ge_dump_stats(psp_log_stream());
                    psp_hle_dump_counts(psp_log_stream(), 40);
                } else if (ev.key.keysym.sym == SDLK_F12) {
                    const char *dir = getenv("EMOTIONALSENSE_SHOT_DIR");
                    char path[512];
                    snprintf(path, sizeof path, "%s/f12_%llu.ppm",
                             dir ? dir : psp_host_temp_dir(),
                             (unsigned long long)psp_display_vblanks());
                    psp_display_capture(path);
                    psp_log("window: captured %s\n", path);
                }
                break;
            }
        }
        uint8_t ax = 128, ay = 128;
        uint32_t bp = 0, bk = 0, trig = 0;
        int moved = 0;
        if (host_input_enabled()) {
            for (int i = 0; i < MAX_PADS; i++) {
                if (!gcs[i]) continue;
                uint32_t t = 0;
                const uint32_t b = pad(gcs[i], &ax, &ay, &t, &moved);
                bp |= b; trig |= t;
                if (b || t) atomic_store(&g_pad_kind, pad_kind(gcs[i]));   /* the last one to press */
            }
            if (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) {
                bk = keyboard();
                if (!moved) keyboard_stick(&ax);
            }
        }
        psp_ctrl_set_ex(bp | bk, ax, ay, trig, moved);
        static int in_trace = -1;
        if (in_trace < 0) in_trace = getenv("EMOTIONALSENSE_TRACE_INPUT") != NULL;
        if (in_trace) {
            static uint32_t l_b; static uint8_t l_ax = 128, l_ay = 128;
            if ((bp | bk) != l_b || ax != l_ax || ay != l_ay) {
                psp_log("input[%llu]: pad=%04X kbd=%04X ax=%u ay=%u\n",
                        (unsigned long long)psp_display_vblanks(), bp, bk, ax, ay);
                l_b = bp | bk; l_ax = ax; l_ay = ay;
            }
        }
        SDL_Delay(1);
    }
    atomic_store(&g_closed, 1);
    SDL_WaitThread(pt, NULL);
    for (int i = 0; i < MAX_PADS; i++) if (gcs[i]) SDL_GameControllerClose(gcs[i]);
    SDL_DestroyWindow(win);
    return 0;
}

int psp_window_start(const char *title, int scale) {
    if (title) g_title = title;
    g_scale = scale;
    atomic_store(&g_closed, 0);
    atomic_store(&g_ready, 0);
    SDL_Thread *t = SDL_CreateThread(window_main, "psp_window", NULL);
    if (!t) return -1;
    SDL_DetachThread(t);
    while (atomic_load(&g_ready) == 0) SDL_Delay(1);  /* the window exists, or failed */
    return atomic_load(&g_ready) == 1 ? 0 : -1;
}

int psp_window_closed(void) { return atomic_load(&g_closed) != 0; }

void psp_window_request_close(void) { atomic_store(&g_closed, 1); }
int psp_window_take_escape(void) { return atomic_exchange(&g_esc, 0); }
void psp_window_set_overlay(psp_overlay_fn fn) { g_overlay = fn; }
int psp_window_overlay_apply(uint32_t *px, int w, int h) { return g_overlay ? g_overlay(px, w, h) : 0; }

/* Height the picture is shown at, for --internal-scale auto. Under gamescope a
 * window is stretched by the compositor to its output, so the output's height
 * (the desktop mode) counts, not the window's. */
int psp_window_height(void) {
    int h = atomic_load(&g_win_h), d = atomic_load(&g_desk_h);
    return (on_gamescope() && d > h) ? d : h;
}

#else

/* ponytail: no window without Win32 or SDL2 (configure with
 * -DPSPRECOMP_SDL2=ON). */
void psp_window_set_state_file(const char *path) { (void)path; }
void psp_window_set_start_mode(int mode) { (void)mode; }
void psp_window_set_icon(const unsigned char *rgba, int w, int h) { (void)rgba; (void)w; (void)h; }
int psp_window_start(const char *title, int scale) { (void)title; (void)scale; return -1; }
int psp_window_closed(void) { return 0; }
void psp_window_request_close(void) {}
int psp_window_take_escape(void) { return 0; }
void psp_window_set_overlay(psp_overlay_fn fn) { (void)fn; }
int psp_window_overlay_apply(uint32_t *px, int w, int h) { (void)px; (void)w; (void)h; return 0; }

int psp_window_height(void) { return 0; }
int psp_window_pad_kind(void) { return 0; }

#endif
