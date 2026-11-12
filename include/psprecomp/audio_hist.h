/* psprecomp — audio content histogram.
 *
 * Diagnosis aid for partial/absent audio in a static recompile. While a guest
 * thread feeds a channel's mixer, we bucket every interleave's sample magnitudes
 * into a global distribution. Two questions the game's audio never tells the host
 * on its own become answerable from here:
 *
 *   1. Is anything actually reaching a channel at all? A channel that only ever
 *      sees zeroed buffers (stub decoders) has a flat histogram; a real BGM or
 *      preview track fills it.
 *   2. Is the sample rate / block pacing in sync? Compare the bucket spacing
 *      (frames) against the rate the game expects: 44100 Hz here.
 *
 * The histogram is deliberate dead weight in normal operation: the hot path
 * (render -> guest buffer) is measured by g_samples_nonzero in sascore.c, and
 * this is a separate, opt-in proof-of-content hook. Only histogram_interleaved
 * does work; it is called at most once per audio frame per output site, and it
 * returns 0 on the common path so callers simply fall through.
 */
#ifndef PSPRECOMP_AUDIO_HIST_H
#define PSPRECOMP_AUDIO_HIST_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bucket a stereo interleave (L/R samples of size `n`) into a distribution and
 * remember the range covered, for later inspection.
 *
 *   l      left  channel samples, int16/int32 values
 *   r      right channel samples, same values
 *   n      number of samples per channel
 *   out_addr guest-address of the just-written sample so we can also record which
 *          channel(s) produced it (0 == none, i.e. the stub path)
 *   tag    free-text tag carried on stderr for attribution ("sas:", "vaudio:", ...)
 *
 * Returns 1 if the interleave contained any non-zero sample (so a caller can
 * branch on real audio), 0 otherwise. This double duty means `histogram_interleaved`
 * is used as a boolean gate at the blocking output sites.
 *
 * Each call updates the window extremes g_hist_min / g_hist_max; call
 * `hist_dBFS` to read the peak of the latest window. */
int histogram_interleaved(
    const int32_t *l, const int32_t *r,
    int n, uint32_t out_addr, const char *tag);

/* Feed a contiguous guest interleave (L at [0..n-2], R at [1..n-1]) recorded by a
 * channel's blocking output into the window. `channel` is the guest channel this
 * frame came from, used only for attribution on stderr. bytes is samples * 4. */
int interleave_histogram(unsigned channel, const void *buf, size_t bytes, unsigned label_id);

/* Reset the window (bucket counts, extremes, counters) between traces/frames.
 * Call once at startup and, optionally, once per audio frame. */
void psprecomp_audio_hist_reset(void);

/* The quiet floor and peak of the latest window, as dBFS (0 dBFS == INT16_MAX).
 * These are globals (not functions) so a trace line can cheaply print them:
 * "hist L%d R%d dBFS". Handy on stderr so one line explains why a channel is
 * silent (flat, near -inf) or live (peaking toward 0). Each call updates them to
 * the just-run interleave's range; a site that never runs (stub silence, zeros)
 * leaves whatever sentinel reset left, so an isolated print tells you "no live
 * frame", not "quiet". */
extern int g_hist_min;
extern int g_hist_max;
double hist_dBFS(void);

/* Reset the running counters (blocks, peak extremes, histograms) between traces. */
void psprecomp_audio_reset(void);

/* Report the non-blocking audio transport over a run to stderr: how many guest
 * audio frames reached the SDL2 queue (g_audio_blocks) and how loud the loudest
 * frame got, referenced to a full-scale int16 (dBFS). Proves the guest's BGM
 * left the guest and reached the host audio queue, never stalling it. */
void psprecomp_audio_report(void);

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_AUDIO_HIST_H */
