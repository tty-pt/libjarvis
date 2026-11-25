#ifndef JARVIS_DSP_H
#define JARVIS_DSP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JARVIS_DSP_NFFT  256
#define JARVIS_DSP_HOP   128
#define JARVIS_DSP_BINS  (JARVIS_DSP_NFFT / 2 + 1)

/* Causal, zero-allocation adaptive audio frontend:
 * - Universal 110 Hz high-pass (DC + fan/table rumble removal for all mics)
 * - Auto-detected dual-mic magnitude coherence telemetry (1.8-7.0 kHz)
 *   relative to the room's own tracked ambient coherence spectrum, invariant
 *   to head angle and asymmetric AEC phase.
 * - Lightweight GCC-PHAT inter-channel delay alignment for clean unity-gain
 *   stereo-to-mono summation without destructive broadband/spectral gating
 *   on the ASR PCM stream.
 * - Automatically bypasses spatial scoring on mono or mono-summed hardware.
 */
typedef struct {
	int   sample_rate;
	int   channels;
	float win[JARVIS_DSP_NFFT];

	/* 256-sample sliding STFT analysis windows & overlap-add synthesis */
	float in_l[JARVIS_DSP_NFFT];
	float in_r[JARVIS_DSP_NFFT];
	float ola[JARVIS_DSP_NFFT];
	int   in_fill;

	/* Small output FIFO to bridge 128-sample STFT hops to 320-sample (20ms) frames */
	float out_fifo[JARVIS_DSP_NFFT * 4];
	int   out_count;

	/* Per-bin causal auto/cross spectral densities & ambient coherence baseline */
	float pll[JARVIS_DSP_BINS];
	float prr[JARVIS_DSP_BINS];
	float plr_re[JARVIS_DSP_BINS];
	float plr_im[JARVIS_DSP_BINS];
	float amb_coh[JARVIS_DSP_BINS];
	int   amb_init;

	/* Auto-detected stereo spatial telemetry & smooth TDOA estimate (samples) */
	float side_ratio_ema;
	int   stereo_active;
	float frame_gain_sm;
	int   hold_hops;
	float last_near_score;
	float tau_est;
} jarvis_dsp_t;

void  jarvis_dsp_init(jarvis_dsp_t *dsp, int sample_rate, int channels);
void  jarvis_dsp_reset(jarvis_dsp_t *dsp);

/* Process one frame of `n_frames` mono or interleaved-stereo int16 PCM samples.
 * Writes `n_frames` cleaned float32 mono samples in [-1, 1] to `out_mono`
 * (clean 110 Hz HPF + delay-aligned stereo-to-mono sum; never gated).
 * If `out_near_score` is non-NULL, stores the frame's off-axis-safe spatial
 * near-field score in [0.0, 1.0] (always 1.0 when mono or mono-summed hardware).
 */
void  jarvis_dsp_process_frame(jarvis_dsp_t *dsp, const int16_t *in_interleaved,
                               size_t n_frames, float *out_mono, float *out_near_score);

int   jarvis_dsp_is_stereo_active(const jarvis_dsp_t *dsp);

#ifdef __cplusplus
}
#endif

#endif /* JARVIS_DSP_H */
