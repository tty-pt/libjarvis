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
		float re_coh[JARVIS_DSP_BINS];

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
			float rc = dsp->plr_re[k] / denom;
			if (rc < -1.0f) rc = -1.0f;
			if (rc >  1.0f) rc =  1.0f;
			re_coh[k] = rc;

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

		/* Asymmetric ambient coherence baseline tracker per bin:
		 * Tracks down toward diffuse/room coherence quickly, rises only very slowly
		 * when coherence is already low (<0.45) so near-field voice never corrupts it.
		 */
		if (dsp->amb_init < 10) {
			float w_new = 1.0f / (float)(dsp->amb_init + 1);
			float w_old = 1.0f - w_new;
			for (int k = 0; k < JARVIS_DSP_BINS; k++)
				dsp->amb_coh[k] = w_old * dsp->amb_coh[k] + w_new * re_coh[k];
			dsp->amb_init++;
		} else {
			for (int k = 0; k < JARVIS_DSP_BINS; k++) {
				float rc = re_coh[k];
				float ac = dsp->amb_coh[k];
				if (rc < ac)
					dsp->amb_coh[k] = 0.96f * ac + 0.04f * rc;
				else if (rc < 0.45f)
					dsp->amb_coh[k] = 0.996f * ac + 0.004f * rc;
			}
		}

		if (dsp->stereo_active) {
			/* Compute energy- & resolvability-weighted excess coherence in 1.8-7.0 kHz */
			float num = 0.0f;
			float den = 0.0f;
			for (int k = b_hf_lo; k <= b_hf_hi; k++) {
				float ac = dsp->amb_coh[k];
				if (ac < -0.30f) ac = -0.30f;
				if (ac >  0.85f) ac =  0.85f;
				float headroom = 1.0f - ac;
				if (headroom < 0.15f) headroom = 0.15f;
				float exc = (re_coh[k] - ac) / headroom;
				if (exc < 0.0f) exc = 0.0f;
				if (exc > 1.0f) exc = 1.0f;
				float mag = sqrtf(dsp->pll[k] * dsp->prr[k] + 1e-18f);
				float bw = mag * headroom;
				num += bw * exc;
				den += bw;
			}
			near_score = (den > 1e-12f) ? (num / den) : 0.0f;

			/* 160ms hangover hold once near-field speech is confirmed (>=0.52),
			 * preserving consonants/plosives while preventing single-hop diffuse spikes
			 * from latching the gate open. */
			if (near_score >= 0.52f && dsp->last_near_score >= 0.46f)
				dsp->hold_hops = 20;
			else if (dsp->hold_hops > 0)
				dsp->hold_hops--;

			float target_fg;
			if (dsp->hold_hops > 0) {
				target_fg = 1.0f;
			} else {
				float t = (near_score - 0.28f) / 0.30f;
				if (t < 0.0f) t = 0.0f;
				if (t > 1.0f) t = 1.0f;
				target_fg = 0.05f + 0.95f * powf(t, 1.5f);
			}

			if (target_fg > dsp->frame_gain_sm)
				dsp->frame_gain_sm = 0.35f * dsp->frame_gain_sm + 0.65f * target_fg;
			else
				dsp->frame_gain_sm = 0.91f * dsp->frame_gain_sm + 0.09f * target_fg;

			/* Synthesize Mid spectrum with smooth broadband gate + gentle HF coherence mask */
			for (int k = 0; k < JARVIS_DSP_BINS; k++) {
				float g;
				if (k < b_hpf) {
					g = 0.0f;
				} else {
					float ac = dsp->amb_coh[k];
					if (ac < -0.30f) ac = -0.30f;
					if (ac >  0.85f) ac =  0.85f;
					float bh = 1.0f - ac;
					if (bh < 0.15f) bh = 0.15f;
					float be = (re_coh[k] - ac) / bh;
					if (be < 0.0f) be = 0.0f;
					if (be > 1.0f) be = 1.0f;
					float bin_g = 0.25f + 0.75f * powf(be, 0.8f);
					float fk = (float)k * df;
					float hi_blend = (fk - 800.0f) / 1100.0f;
					if (hi_blend < 0.0f) hi_blend = 0.0f;
					if (hi_blend > 1.0f) hi_blend = 1.0f;
					g = dsp->frame_gain_sm * ((1.0f - hi_blend) + hi_blend * bin_g);
				}
				float mr = 0.5f * (xl_re[k] + xr_re[k]);
				float mi = 0.5f * (xl_im[k] + xr_im[k]);
				y_re[k] = mr * g;
				y_im[k] = mi * g;
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
