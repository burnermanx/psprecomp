/* psprecomp — diagnostic output. See include/psprecomp/log.h. */
#include "psprecomp/log.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
extern char **_environ;
#  define ENV_LIST _environ
#  define NULL_DEVICE "NUL"
#else
extern char **environ;
#  define ENV_LIST environ
#  define NULL_DEVICE "/dev/null"
#endif

static int g_on;
static int g_init;
static void (*g_hook)(const char *, size_t);
static char g_line[2048];
static size_t g_ll;
static FILE *g_null;

static int truthy(const char *v) { return v && *v && strcmp(v, "0") != 0; }

static void init(void) {
    g_init = 1;
    if (truthy(getenv("EMOTIONALSENSE_LOG")) || truthy(getenv("PSPRECOMP_LOG"))) { g_on = 1; return; }
    for (char **e = ENV_LIST; e && *e; e++) {
        const char *s = *e;
        if (!strncmp(s, "EMOTIONALSENSE_TRACE_", 21) || !strncmp(s, "PSPRECOMP_TRACE_", 16)) {
            const char *eq = strchr(s, '=');
            if (eq && truthy(eq + 1)) { g_on = 1; return; }
        }
    }
}

int psp_log_enabled(void) { if (!g_init) init(); return g_on; }
void psp_log_enable(int on) { g_init = 1; g_on = on; }
void psp_log_set_line_hook(void (*fn)(const char *, size_t)) { g_hook = fn; }

/* Logging off: complete lines go to the hook only. */
static void feed(const char *s, size_t n) {
    if (!g_hook) return;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') { g_hook(g_line, g_ll); g_ll = 0; }
        else if (g_ll < sizeof g_line - 1) g_line[g_ll++] = s[i];
    }
}

static void emit(FILE *f, const char *s, size_t n) {
    if (!g_init) init();
    if (g_on) fwrite(s, 1, n, f);
    else feed(s, n);
}

static void vemit(FILE *f, const char *fmt, va_list ap) {
    char buf[1024];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n >= 0 && (size_t)n < sizeof buf) emit(f, buf, (size_t)n);
    else if (n >= 0) {
        char *big = (char *)malloc((size_t)n + 1);
        if (big) { vsnprintf(big, (size_t)n + 1, fmt, ap2); emit(f, big, (size_t)n); free(big); }
    }
    va_end(ap2);
}

void psp_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vemit(stderr, fmt, ap); va_end(ap); }
void psp_out(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vemit(stdout, fmt, ap); va_end(ap); }

void psp_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

void psp_log_putc(int c) { char ch = (char)c; emit(stderr, &ch, 1); }
void psp_log_puts(const char *s) { emit(stderr, s, strlen(s)); }

FILE *psp_log_stream(void) {
    if (!g_init) init();
    if (g_on) return stderr;
    if (!g_null) g_null = fopen(NULL_DEVICE, "w");
    return g_null ? g_null : stderr;
}

#define REPORT_MAX 24576
static char g_rep[REPORT_MAX];
static size_t g_repn;

void psp_report(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    if (g_repn + (size_t)n < REPORT_MAX - 1) { memcpy(g_rep + g_repn, buf, (size_t)n); g_repn += (size_t)n; g_rep[g_repn] = 0; }
    emit(stderr, buf, (size_t)n);
}
const char *psp_report_text(void) { return g_rep; }
void psp_report_clear(void) { g_repn = 0; g_rep[0] = 0; }
