/* See LICENSE file for copyright and license details.
 * Voice Activity Detection module for jarvisd.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "vad.h"

struct vad_biquad {
    double hp_x1, hp_x2, hp_y1, hp_y2;
    double lp_x1, lp_x2, lp_y1, lp_y2;
};

struct vad_ctx {
    double noise_floor_db;
    double ambient_std_db;
    double threshold_db;
    int env_frames;
    int in_speech_hint;
};

vad_ctx_t *vad_create(double initial_noise_floor_db, double threshold_db)
{
    vad_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->noise_floor_db = initial_noise_floor_db;
    ctx->ambient_std_db = 1.5;
    ctx->threshold_db = threshold_db;
    ctx->env_frames = 0;
    ctx->in_speech_hint = 0;
    return ctx;
}

void vad_destroy(vad_ctx_t *ctx)
{
    free(ctx);
}

float vad_calc_frame_db(const int16_t *samples, size_t n, vad_biquad_t *flt)
{
    static const double hp_b0 =  0.9645815294, hp_b1 = -1.9291630588, hp_b2 =  0.9645815294;
    static const double hp_a1 = -1.9279122460, hp_a2 =  0.9304138716;
    static const double lp_b0 =  0.2266816615, lp_b1 =  0.4533633230, lp_b2 =  0.2266816615;
    static const double lp_a1 = -0.2764683938, lp_a2 =  0.1831950398;

    double sum = 0.0;
    int zc = 0;
    double prev_y = 0.0;
    size_t i;

    if (!n)
        return -100.0f;

    for (i = 0; i < n; i++) {
        double x = (double)samples[i] / 32768.0;
        double y = x;
        if (flt) {
            double h = hp_b0 * x + hp_b1 * flt->hp_x1 + hp_b2 * flt->hp_x2
                     - hp_a1 * flt->hp_y1 - hp_a2 * flt->hp_y2;
            flt->hp_x2 = flt->hp_x1;
            flt->hp_x1 = x;
            flt->hp_y2 = flt->hp_y1;
            flt->hp_y1 = h;

            y = lp_b0 * h + lp_b1 * flt->lp_x1 + lp_b2 * flt->lp_x2
              - lp_a1 * flt->lp_y1 - lp_a2 * flt->lp_y2;
            flt->lp_x2 = flt->lp_x1;
            flt->lp_x1 = h;
            flt->lp_y2 = flt->lp_y1;
            flt->lp_y1 = y;
        }
        if (i > 0 && ((y >= 0.0 && prev_y < 0.0) || (y < 0.0 && prev_y >= 0.0)))
            zc++;
        prev_y = y;
        sum += y * y;
    }

    double rms = sqrt(sum / (double)n);
    if (rms < 1e-5)
        return -100.0f;

    double db = 20.0 * log10(rms);

    double zcr = (double)zc / (double)n;
    if (zcr < 0.005 || zcr > 0.65)
        db -= 3.0;

    return (float)db;
}

void vad_update_ambient(vad_ctx_t *ctx, const float *db_ring, int64_t total_frames,
                        int ring_frames, const uint8_t *is_speech)
{
    if (!ctx)
        return;

    float buf[128];
    int n = 0;

    int64_t avail_min = (total_frames > ring_frames)
                      ? (total_frames - ring_frames) : 0;

    int64_t scan_min = total_frames - 240;
    if (scan_min < avail_min)
        scan_min = avail_min;

    for (int64_t f = total_frames - 1; f >= scan_min && n < 80; f--) {
        int slot = (int)(f % ring_frames);
        if (is_speech[slot])
            continue;
        float v = db_ring[slot];
        if (v > -95.0f)
            buf[n++] = v;
    }

    int fallback_all = 0;
    if (n < 24) {
        n = 0;
        fallback_all = 1;
        int64_t start_f = total_frames - 80;
        if (start_f < avail_min)
            start_f = avail_min;
        for (int64_t f = start_f; f < total_frames && n < 128; f++) {
            float v = db_ring[f % ring_frames];
            if (v > -95.0f)
                buf[n++] = v;
        }
    }

    if (n == 0)
        return;

    for (int i = 1; i < n; i++) {
        float key = buf[i];
        int j = i - 1;
        while (j >= 0 && buf[j] > key) {
            buf[j + 1] = buf[j];
            j--;
        }
        buf[j + 1] = key;
    }

    int lo = fallback_all ? ((n * 15) / 100) : ((n >= 10) ? (n / 10) : 0);
    int hi = fallback_all ? ((n * 75) / 100) : ((n >= 10) ? (n - (n / 10)) : n);
    if (hi <= lo) {
        lo = 0;
        hi = n;
    }

    double sum = 0.0;
    int cnt = hi - lo;
    for (int i = lo; i < hi; i++)
        sum += buf[i];
    double avg = sum / (double)cnt;

    double var_sum = 0.0;
    for (int i = lo; i < hi; i++) {
        double d = buf[i] - avg;
        var_sum += d * d;
    }
    double std_dev = (cnt > 1) ? sqrt(var_sum / (double)cnt) : 1.2;
    if (std_dev < 1.0)
        std_dev = 1.0;
    if (std_dev > 3.5)
        std_dev = 3.5;
    if (avg < -60.0)
        avg = -60.0;
    if (avg > -24.0)
        avg = -24.0;

    if (!ctx->in_speech_hint || ctx->env_frames < 15) {
        ctx->noise_floor_db = avg;
        ctx->ambient_std_db = std_dev;
        if (ctx->env_frames < 15)
            ctx->env_frames++;
    } else {
        ctx->noise_floor_db = 0.92 * ctx->noise_floor_db + 0.08 * avg;
        ctx->ambient_std_db = 0.92 * ctx->ambient_std_db + 0.08 * std_dev;
    }
}

double vad_calc_start_db(double amb_avg, double amb_std)
{
    double margin = 5.2;
    if (2.3 * amb_std > margin)
        margin = 2.3 * amb_std;
    double start_db = amb_avg + margin;
    if (start_db < -44.0)
        start_db = -44.0;
    if (start_db > -20.0)
        start_db = -20.0;
    return start_db;
}

double vad_calc_keep_db(double utter_amb_avg, double utter_amb_std,
                        double utter_start_db, double peak_speech_db)
{
    double keep_margin = 3.2;
    if (1.6 * utter_amb_std > keep_margin)
        keep_margin = 1.6 * utter_amb_std;
    double keep_db = utter_amb_avg + keep_margin;
    if (utter_start_db - 3.8 > keep_db)
        keep_db = utter_start_db - 3.8;
    if (peak_speech_db - 14.0 > keep_db)
        keep_db = peak_speech_db - 14.0;
    if (keep_db > utter_start_db - 2.0)
        keep_db = utter_start_db - 2.0;
    return keep_db;
}

bool vad_push_frame(vad_ctx_t *ctx, const float *frame_samples, size_t count, float db)
{
    if (!ctx)
        return false;

    int is_speech = (ctx->env_frames >= 10 && db > ctx->noise_floor_db + 8.5) ? 1 : 0;
    return is_speech;
}