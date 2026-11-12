/* psprecomp -- a window to play in. See include/psprecomp/window.h. */

#include "psprecomp/window.h"
#include "psprecomp/hle.h"

#ifdef _WIN32

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

/* The first XInput controller, if one is connected. */
static uint32_t pad(uint8_t *ax, uint8_t *ay) {
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
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
        if (s.Gamepad.wButtons & P[i].x) b |= P[i].bit;
    if (s.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= B_L;
    if (s.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= B_R;
    /* The PSP stick is 0..255 with 128 at rest and y growing downwards. */
    const int lx = s.Gamepad.sThumbLX, ly = s.Gamepad.sThumbLY;
    if (lx * lx + ly * ly > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE * XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) {
        *ax = (uint8_t)((lx + 32768) >> 8);
        *ay = (uint8_t)((32767 - ly) >> 8);
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
        uint32_t b = pad(&ax, &ay);
        if (GetForegroundWindow() == h) b |= keyboard();   /* keys only when focused */
        psp_ctrl_set(b, ax, ay);
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

#elif defined(PSPRECOMP_SDL2)

/* SDL2 backend (Linux and anything else SDL runs on). Same shape as the Win32
 * one: a host thread of its own that owns the window, polls input into
 * sceCtrl and presents the game's front buffer at ~60 Hz, never waiting on
 * the game. */

#include <SDL.h>
#include <stdatomic.h>
#include <stdio.h>

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
static uint32_t g_px[W * H];

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
    return b;
}

static uint32_t pad(SDL_GameController *gc, uint8_t *ax, uint8_t *ay) {
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
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 8000) b |= B_L;
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000) b |= B_R;
    /* The PSP stick is 0..255 with 128 at rest and y growing downwards --
     * the same direction as SDL's. */
    const int lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
    const int ly = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
    if (lx * lx + ly * ly > 7849 * 7849) {
        *ax = (uint8_t)((lx + 32768) >> 8);
        *ay = (uint8_t)((ly + 32768) >> 8);
    }
    return b;
}

static int window_main(void *arg) {
    (void)arg;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "psprecomp: SDL_Init: %s\n", SDL_GetError());
        atomic_store(&g_closed, 1); atomic_store(&g_ready, -1);
        return 1;
    }
    if (g_scale <= 0) {
        SDL_Rect usable;
        g_scale = 2;
        if (SDL_GetDisplayUsableBounds(0, &usable) == 0) {
            g_scale = usable.h * 7 / 10 / H;
            if (g_scale < 1) g_scale = 1;
        }
    }
    SDL_Window *win = SDL_CreateWindow(g_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       W * g_scale, H * g_scale,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    const char *vsync = getenv("EMOTIONALSENSE_VSYNC");
    Uint32 rflags = SDL_RENDERER_ACCELERATED;
    if (!(vsync && vsync[0] == '0')) rflags |= SDL_RENDERER_PRESENTVSYNC;
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, rflags) : NULL;
    if (win && !ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                                               SDL_TEXTUREACCESS_STREAMING, W, H) : NULL;
    if (!tex) {
        fprintf(stderr, "psprecomp: cannot open a window: %s\n", SDL_GetError());
        atomic_store(&g_closed, 1); atomic_store(&g_ready, -1);
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");          /* nearest: crisp pixels */
    SDL_RenderSetLogicalSize(ren, W, H);                       /* keep the aspect, black bars */
    atomic_store(&g_ready, 1);

    SDL_GameController *gc = NULL;
    uint32_t snap_seq = 0;
    int dirty = 1;                              /* first frame and every resize */
    while (!atomic_load(&g_closed)) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: atomic_store(&g_closed, 1); break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    ev.window.event == SDL_WINDOWEVENT_EXPOSED) dirty = 1;
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (!gc) gc = SDL_GameControllerOpen(ev.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (gc && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc)) == ev.cdevice.which) {
                    SDL_GameControllerClose(gc); gc = NULL;
                }
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_F11) {
                    Uint32 fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                    SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
                break;
            }
        }
        uint8_t ax = 128, ay = 128;
        uint32_t b = pad(gc, &ax, &ay);
        if (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) b |= keyboard();
        psp_ctrl_set(b, ax, ay);

        /* Present only when the guest produced a frame (or the window changed),
         * so the screen rate never paces the game. */
        if (psp_display_snapshot(g_px, &snap_seq)) dirty = 1;
        if (dirty) {
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            SDL_UpdateTexture(tex, NULL, g_px, W * 4);   /* 0xAABBGGRR == ABGR8888 */
            SDL_RenderCopy(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
            dirty = 0;
        }
        SDL_Delay(1);
    }
    if (gc) SDL_GameControllerClose(gc);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
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

#else

/* ponytail: no window without Win32 or SDL2 (configure with
 * -DPSPRECOMP_SDL2=ON). */
int psp_window_start(const char *title, int scale) { (void)title; (void)scale; return -1; }
int psp_window_closed(void) { return 0; }

#endif
