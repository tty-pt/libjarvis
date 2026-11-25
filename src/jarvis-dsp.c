#include "jarvis-dsp.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Iterative in-place radix-2 complex FFT (N = JARVIS_DSP_NFFT = 256).
 * inverse == 0: forward FFT; inverse != 0: inverse FFT (scaled by 1/N).
 */
static void
fft256(float *re, float *im, int inverse)
{
	const int n = JARVIS_DSP_NFFT;

	/* Bit-reversal permutation */
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			float tr = re[i]; re[i] = re[j]; re[j] = tr;
			float ti = im[i]; im[i] = im[j]; im[j] = ti;
		}
	}

	/* Cooley-Tukey butterfly stages */
	const float sign = inverse ? 1.0f : -1.0f;
	for (int len = 2; len <= n; len <<= 1) {
		int half = len >> 1;
		float ang = sign * (float)(2.0 * M_PI / (double)len);
		float wlen_r = cosf(ang);
		float wlen_i = sinf(ang);
		for (int i = 0; i < n; i += len) {
			float wr = 1.0f;
			float wi = 0.0f;
			for (int j = 0; j < half; j++) {
				int u = i + j;
				int v = u + half;
				float vr = re[v] * wr - im[v] * wi;
				float vi = re[v] * wi + im[v] * wr;
				re[v] = re[u] - vr;
				im[v] = im[u] - vi;
				re[u] += vr;
				im[u] += vi;
				float nwr = wr * wlen_r - wi * wlen_i;
				wi = wr * wlen_i + wi * wlen_r;
				wr = nwr;
			}
		}
	}

	if (inverse) {
		const float inv_n = 1.0f / (float)n;
		for (int i = 0; i < n; i++) {
			re[i] *= inv_n;
			im[i] *= inv_n;
		}
	}
}

void
jarvis_dsp_reset(jarvis_dsp_t *dsp)
{
	if (!dsp)
		return;
	int sr = dsp->sample_rate > 0 ? dsp->sample_rate : 16000;
	int ch = (dsp->channels == 2) ? 2 : 1;

	memset(dsp, 0, sizeof(*dsp));
	dsp->sample_rate = sr;
	dsp->channels = ch;

	/* Periodic sqrt-Hann window: w[i]^2 + w[i + N/2]^2 == 1.0 exactly (exact COLA) */
	for (int i = 0; i < JARVIS_DSP_NFFT; i++) {
		double h = 0.5 * (1.0 - cos(2.0 * M_PI * (double)i / (double)JARVIS_DSP_NFFT));
		dsp->win[i] = (float)sqrt(h);
	}
	for (int k = 0; k < JARVIS_DSP_BINS; k++) {
		dsp->pll[k] = 1e-8f;
		dsp->prr[k] = 1e-8f;
	}
	dsp->in_fill = JARVIS_DSP_HOP; /* 1-hop zero lead-in for smooth start */
	dsp->frame_gain_sm = 0.15f;
	dsp->last_near_score = 1.0f;
}

void
jarvis_dsp_init(jarvis_dsp_t *dsp, int sample_rate, int channels)
{
	if (!dsp)
		return;
	dsp->sample_rate = sample_rate > 0 ? sample_rate : 16000;
	dsp->channels = (channels == 2) ? 2 : 1;
	jarvis_dsp_reset(dsp);
}

int
jarvis_dsp_is_stereo_active(const jarvis_dsp_t *dsp)
{
	return (dsp && dsp->channels == 2 && dsp->stereo_active) ? 1 : 0;
}

/* Process one 256-sample STFT block (hop = 128 samples = 8ms @ 16 kHz)
 * and append 128 cleaned mono samples to dsp->out_fifo.
 */
static float
process_hop(jarvis_dsp_t *dsp)
{
	float xl_re[JARVIS_DSP_NFFT], xl_im[JARVIS_DSP_NFFT];
	float xr_re[JARVIS_DSP_NFFT], xr_im[JARVIS_DSP_NFFT];
	float y_re[JARVIS_DSP_NFFT],  y_im[JARVIS_DSP_NFFT];

	const int is_ch2 = (dsp->channels == 2);
	const float df = (float)dsp->sample_rate / (float)JARVIS_DSP_NFFT;

	/* Bin index ranges computed dynamically from sample_rate */
	int b_hpf    = (int)ceilf(110.0f / df);
	int b_hf_lo  = (int)floorf(1800.0f / df);
	int b_hf_hi  = (int)ceilf(7000.0f / df);
	if (b_hpf < 1) b_hpf = 1;
	if (b_hf_lo < 1) b_hf_lo = 1;
	if (b_hf_hi >= JARVIS_DSP_BINS) b_hf_hi = JARVIS_DSP_BINS - 1;

	/* Windowed forward FFTs */
	for (int i = 0; i < JARVIS_DSP_NFFT; i++) {
		float w = dsp->win[i];
		xl_re[i] = dsp->in_l[i] * w;
		xl_im[i] = 0.0f;
	}
	fft256(xl_re, xl_im, 0);

	if (is_ch2) {
		for (int i = 0; i < JARVIS_DSP_NFFT; i++) {
			float w = dsp->win[i];
			xr_re[i] = dsp->in_r[i] * w;
			xr_im[i] = 0.0f;
		}
		fft256(xr_re, xr_im, 0);
	}

	float near_score = 1.0f;

	if (is_ch2) {
		/* Update causal cross/auto-spectral densities (tau ~ 36ms @ 8ms hop):
		 * long enough to keep diffuse-noise coherence variance low, short enough
		 * to lock onto speech syllables cleanly.
		 */
		const float alpha = 0.78f;
		const float one_m = 1.0f - alpha;
		float pm_hf = 0.0f;
		float ps_hf = 0.0f;
		float mag_coh[JARVIS_DSP_BINS];

		for (int k = 0; k < JARVIS_DSP_BINS; k++) {
			float lr = xl_re[k], li = xl_im[k];
			float rr = xr_re[k], ri = xr_im[k];

			float pl = lr * lr + li * li;
			float pr = rr * rr + ri * ri;
			/* XL * conj(XR) */
			float cr = lr * rr + li * ri;
			float ci = li * rr - lr * ri;

			dsp->pll[k]    = alpha * dsp->pll[k]    + one_m * pl;
			dsp->prr[k]    = alpha * dsp->prr[k]    + one_m * pr;
			dsp->plr_re[k] = alpha * dsp->plr_re[k] + one_m * cr;
			dsp->plr_im[k] = alpha * dsp->plr_im[k] + one_m * ci;

			float denom = sqrtf(dsp->pll[k] * dsp->prr[k] + 1e-18f);
			/* Magnitude coherence |P_LR| / sqrt(P_LL * P_RR):
			 * invariant to inter-channel time delay (off-axis head turns up to +-60 deg)
			 * and asymmetric WebRTC/OS AEC channel phase, while rejecting diffuse noise.
			 */
			float c_mag = sqrtf(dsp->plr_re[k] * dsp->plr_re[k] +
			                    dsp->plr_im[k] * dsp->plr_im[k]);
			float mc = c_mag / denom;
			if (mc < 0.0f) mc = 0.0f;
			if (mc > 1.0f) mc = 1.0f;
			mag_coh[k] = mc;

			if (k >= b_hf_lo && k <= b_hf_hi) {
				float mr = 0.5f * (lr + rr), mi = 0.5f * (li + ri);
				float sr = 0.5f * (lr - rr), si = 0.5f * (li - ri);
				pm_hf += mr * mr + mi * mi;
				ps_hf += sr * sr + si * si;
			}
		}

		/* Auto-detect true stereo capsules vs mono-summed hardware:
		 * If HF side energy exceeds 1% of total HF energy, capsules are independent.
		 */
		float tot_hf = pm_hf + ps_hf;
		if (tot_hf > 1e-10f) {
			float sr_inst = ps_hf / tot_hf;
			dsp->side_ratio_ema = 0.95f * dsp->side_ratio_ema + 0.05f * sr_inst;
			if (dsp->side_ratio_ema > 0.01f)
				dsp->stereo_active = 1;
		}

		/* Asymmetric ambient magnitude-coherence baseline tracker per bin:
		 * Tracks down toward diffuse/room coherence quickly (and absorbs the
		 * finite-sample bias of |Coh| on uncorrelated noise), rises only slowly
		 * when coherence is below 0.65 so near-field voice never corrupts it.
		 */
		if (dsp->amb_init < 12) {
			float w_new = 1.0f / (float)(dsp->amb_init + 1);
			float w_old = 1.0f - w_new;
			for (int k = 0; k < JARVIS_DSP_BINS; k++)
				dsp->amb_coh[k] = w_old * dsp->amb_coh[k] + w_new * mag_coh[k];
			dsp->amb_init++;
		} else {
			for (int k = 0; k < JARVIS_DSP_BINS; k++) {
				float mc = mag_coh[k];
				float ac = dsp->amb_coh[k];
				if (mc < ac)
					dsp->amb_coh[k] = 0.94f * ac + 0.06f * mc;
				else if (mc < 0.65f)
					dsp->amb_coh[k] = 0.992f * ac + 0.008f * mc;
			}
		}

		if (dsp->stereo_active) {
			/* Compute energy- & resolvability-weighted excess |Coh| in 1.8-7.0 kHz */
			float num = 0.0f;
			float den = 0.0f;
			for (int k = b_hf_lo; k <= b_hf_hi; k++) {
				float ac = dsp->amb_coh[k];
				if (ac < 0.0f)  ac = 0.0f;
				if (ac > 0.85f) ac = 0.85f;
				float headroom = 1.0f - ac;
				if (headroom < 0.15f) headroom = 0.15f;
				float exc = (mag_coh[k] - ac) / headroom;
				if (exc < 0.0f) exc = 0.0f;
				if (exc > 1.0f) exc = 1.0f;
				float mag = sqrtf(dsp->pll[k] * dsp->prr[k] + 1e-18f);
				float bw = mag * headroom;
				num += bw * exc;
				den += bw;
			}
			near_score = (den > 1e-12f) ? (num / den) : 0.0f;

			/* Lightweight GCC-PHAT TDOA search over [-3.0, +3.0] samples (+-187.5 us @ 16kHz)
			 * using the already-computed cross-spectrum (0.3-6.0 kHz). When near-field speech
			 * is present, phase-align R to L before summing so off-axis speech never suffers
			 * a high-frequency comb-filter null.
			 */
			int b_tdoa_lo = (int)floorf(300.0f / df);
			int b_tdoa_hi = (int)ceilf(6000.0f / df);
			if (b_tdoa_lo < b_hpf) b_tdoa_lo = b_hpf;
			if (b_tdoa_hi >= JARVIS_DSP_BINS) b_tdoa_hi = JARVIS_DSP_BINS - 1;

			if (near_score >= 0.42f) {
				float best_tau = dsp->tau_est;
				float best_val = -1e30f;
				const float base_w = (float)(2.0 * M_PI / (double)JARVIS_DSP_NFFT);
				for (int step = -6; step <= 6; step++) {
					float tau = 0.5f * (float)step;
					float acc = 0.0f;
					for (int k = b_tdoa_lo; k <= b_tdoa_hi; k++) {
						float cr = dsp->plr_re[k];
						float ci = dsp->plr_im[k];
						float cm = sqrtf(cr * cr + ci * ci + 1e-18f);
						float ang = base_w * (float)k * tau;
						float ca = cosf(ang);
						float sa = sinf(ang);
						/* Re(P_LR * exp(-j * w_k * tau)) / |P_LR| */
						acc += (cr * ca + ci * sa) / cm;
					}
					if (acc > best_val) {
						best_val = acc;
						best_tau = tau;
					}
				}
				dsp->tau_est = 0.70f * dsp->tau_est + 0.30f * best_tau;
			} else {
				/* Gently relax toward broadside during silence/diffuse background */
				dsp->tau_est *= 0.96f;
			}

			/* Clean, ungated ASR synthesis: 110 Hz HPF + phase-aligned stereo-to-mono sum.
			 * Spatial gating is strictly decoupled from the ASR waveform; callers use
			 * `out_near_score` alongside neural VAD for speech detection & barge-in.
			 */
			const float base_w = (float)(2.0 * M_PI / (double)JARVIS_DSP_NFFT);
			const float tau = dsp->tau_est;
			for (int k = 0; k < JARVIS_DSP_BINS; k++) {
				if (k < b_hpf) {
					y_re[k] = 0.0f;
					y_im[k] = 0.0f;
					continue;
				}
				float rr = xr_re[k], ri = xr_im[k];
				if (fabsf(tau) > 0.05f) {
					/* Align R to L: X_R(k) * exp(+j * w_k * tau) */
					float ang = base_w * (float)k * tau;
					float ca = cosf(ang);
					float sa = sinf(ang);
					float ar = rr * ca - ri * sa;
					float ai = rr * sa + ri * ca;
					rr = ar;
					ri = ai;
				}
				y_re[k] = 0.5f * (xl_re[k] + rr);
				y_im[k] = 0.5f * (xl_im[k] + ri);
			}
		} else {
			/* Stereo open, but mono-summed hardware -> clean Mid + 110 Hz HPF only */
			for (int k = 0; k < JARVIS_DSP_BINS; k++) {
				float g = (k < b_hpf) ? 0.0f : 1.0f;
				y_re[k] = 0.5f * (xl_re[k] + xr_re[k]) * g;
				y_im[k] = 0.5f * (xl_im[k] + xr_im[k]) * g;
			}
		}
	} else {
		/* Mono input -> universal 110 Hz HPF only (exact COLA passthrough >= 110 Hz) */
		for (int k = 0; k < JARVIS_DSP_BINS; k++) {
			float g = (k < b_hpf) ? 0.0f : 1.0f;
			y_re[k] = xl_re[k] * g;
			y_im[k] = xl_im[k] * g;
		}
	}

	/* Hermitian symmetry for real inverse FFT */
	y_im[0] = 0.0f;
	y_im[JARVIS_DSP_BINS - 1] = 0.0f;
	for (int k = 1; k < JARVIS_DSP_BINS - 1; k++) {
		y_re[JARVIS_DSP_NFFT - k] =  y_re[k];
		y_im[JARVIS_DSP_NFFT - k] = -y_im[k];
	}

	fft256(y_re, y_im, 1);

	/* Overlap-add synthesis (sqrt-Hann synthesis window) */
	for (int i = 0; i < JARVIS_DSP_NFFT; i++)
		dsp->ola[i] += y_re[i] * dsp->win[i];

	/* Emit first HOP (128) completed samples into out_fifo */
	int max_fifo = (int)(sizeof(dsp->out_fifo) / sizeof(dsp->out_fifo[0]));
	if (dsp->out_count + JARVIS_DSP_HOP <= max_fifo) {
		memcpy(dsp->out_fifo + dsp->out_count, dsp->ola,
		       (size_t)JARVIS_DSP_HOP * sizeof(float));
		dsp->out_count += JARVIS_DSP_HOP;
	}

	/* Shift OLA buffer left by HOP */
	memmove(dsp->ola, dsp->ola + JARVIS_DSP_HOP,
	        (size_t)JARVIS_DSP_HOP * sizeof(float));
	memset(dsp->ola + JARVIS_DSP_HOP, 0,
	       (size_t)JARVIS_DSP_HOP * sizeof(float));

	dsp->last_near_score = near_score;
	return near_score;
}

void
jarvis_dsp_process_frame(jarvis_dsp_t *dsp, const int16_t *in_interleaved,
                         size_t n_frames, float *out_mono, float *out_near_score)
{
	if (!dsp || !in_interleaved || !out_mono || n_frames == 0)
		return;

	const float inv_i16 = 1.0f / 32768.0f;
	const int is_ch2 = (dsp->channels == 2);
	float max_score = -1.0f;

	for (size_t i = 0; i < n_frames; i++) {
		if (is_ch2) {
			dsp->in_l[dsp->in_fill] = (float)in_interleaved[i * 2] * inv_i16;
			dsp->in_r[dsp->in_fill] = (float)in_interleaved[i * 2 + 1] * inv_i16;
		} else {
			dsp->in_l[dsp->in_fill] = (float)in_interleaved[i] * inv_i16;
		}
		dsp->in_fill++;

		if (dsp->in_fill == JARVIS_DSP_NFFT) {
			float sc = process_hop(dsp);
			if (sc > max_score)
				max_score = sc;
			memmove(dsp->in_l, dsp->in_l + JARVIS_DSP_HOP,
			        (size_t)JARVIS_DSP_HOP * sizeof(float));
			if (is_ch2) {
				memmove(dsp->in_r, dsp->in_r + JARVIS_DSP_HOP,
				        (size_t)JARVIS_DSP_HOP * sizeof(float));
			}
			dsp->in_fill = JARVIS_DSP_HOP;
		}
	}

	/* Bulk drain out_fifo into out_mono (zero-pad leading startup latency on frame 0 only) */
	size_t take = (size_t)dsp->out_count;
	if (take > n_frames)
		take = n_frames;
	size_t pad = n_frames - take;
	if (pad > 0)
		memset(out_mono, 0, pad * sizeof(float));
	if (take > 0) {
		memcpy(out_mono + pad, dsp->out_fifo, take * sizeof(float));
		dsp->out_count -= (int)take;
		if (dsp->out_count > 0) {
			memmove(dsp->out_fifo, dsp->out_fifo + take,
			        (size_t)dsp->out_count * sizeof(float));
		}
	}

	/* Clamp output to [-1.0, 1.0] */
	for (size_t i = 0; i < n_frames; i++) {
		if (out_mono[i] > 1.0f) out_mono[i] = 1.0f;
		else if (out_mono[i] < -1.0f) out_mono[i] = -1.0f;
	}

	if (out_near_score) {
		float frame_sc = (max_score >= 0.0f) ? max_score : dsp->last_near_score;
		*out_near_score = dsp->stereo_active ? frame_sc : 1.0f;
	}
}
