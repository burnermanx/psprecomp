/* psprecomp — diagnostic output.
 *
 * Nothing the runtime or the game host prints for diagnosis reaches the terminal by default;
 * only fatal errors (psp_fatal) do. Logging is switched on with EMOTIONALSENSE_LOG=1
 * (or PSPRECOMP_LOG=1), the game's --verbose, or any EMOTIONALSENSE_TRACE_* /
 * PSPRECOMP_TRACE_* variable, all of which imply it.
 *
 * With logging off the lines are still handed to the line hook (the game's crash report keeps
 * its "last log lines" ring from it); with logging on they go to stderr (stdout for psp_out)
 * and the crash tee of those descriptors fills the ring, as before. Output of other libraries
 * (Vulkan, Mesa, the Gamescope layer) is theirs: it is never touched. */
#ifndef PSPRECOMP_LOG_H
#define PSPRECOMP_LOG_H

#include <stddef.h>
#include <stdio.h>

#ifdef __GNUC__
#  define PSP_LOG_FMT(a, b) __attribute__((format(printf, a, b)))
#else
#  define PSP_LOG_FMT(a, b)
#endif

#ifdef __cplusplus
extern "C" {
#endif

int  psp_log_enabled(void);
void psp_log_enable(int on);
void psp_log_set_line_hook(void (*fn)(const char *line, size_t n));

void psp_log(const char *fmt, ...) PSP_LOG_FMT(1, 2);    /* diagnostics (what went to stderr) */
void psp_out(const char *fmt, ...) PSP_LOG_FMT(1, 2);    /* diagnostics (what went to stdout) */
void psp_fatal(const char *fmt, ...) PSP_LOG_FMT(1, 2);  /* always printed, to stderr */
void psp_log_putc(int c);
void psp_log_puts(const char *s);
FILE *psp_log_stream(void);   /* stderr when logging is on, else a stream that discards: for dump functions */

/* The report: lines that are kept in memory whether or not logging is on (and also go through
 * psp_log), for a host that writes them to a file the user can send (the Vulkan start-up
 * steps). Bounded: the oldest lines are kept, later ones dropped once the buffer is full. */
void psp_report(const char *fmt, ...) PSP_LOG_FMT(1, 2);
const char *psp_report_text(void);
void psp_report_clear(void);

#ifdef __cplusplus
}
#endif
#endif
