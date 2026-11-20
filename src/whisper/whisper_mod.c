/* See LICENSE file for copyright and license details.
 * Whisper decode worker module for jarvisd.
 */

#include <ctype.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <whisper.h>

#include "whisper_mod.h"
#include "../audio/audio.h"

static volatile sig_atomic_t *g_running = NULL;
static volatile sig_atomic_t *g_cancel_req = NULL;
static volatile sig_atomic_t *g_stop_req = NULL;

void whisper_set_global_signals(volatile sig_atomic_t *running,
                                volatile sig_atomic_t *cancel_req,
                                volatile sig_atomic_t *stop_req)
{
    g_running = running;
    g_cancel_req = cancel_req;
    g_stop_req = stop_req;
}

static bool whisper_abort_cb(void *user_data)
{
    (void)user_data;
    return (g_running && (!*g_running || *g_cancel_req)) ? true : false;
}

int whisper_decoder_init(whisper_decoder_t *dec, struct whisper_context *ctx_main,
                         int n_threads, int max_utter_frames)
{
    if (!dec || !ctx_main)
        return -1;

    memset(dec, 0, sizeof(*dec));
    dec->ctx = ctx_main;
    dec->n_threads = n_threads;
    dec->pcm = malloc((size_t)max_utter_frames * AUDIO_FRAME_SAMPLES * sizeof(float));
    if (!dec->pcm)
        return -1;

    dec->mtx = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    dec->cond = (pthread_cond_t)PTHREAD_COND_INITIALIZER;

    return 0;
}

void whisper_decoder_fini(whisper_decoder_t *dec)
{
    if (!dec)
        return;

    pthread_mutex_lock(&dec->mtx);
    dec->has_job = 0;
    dec->abort_flag = 1;
    while (dec->busy)
        pthread_cond_wait(&dec->cond, &dec->mtx);
    dec->abort_flag = 0;
    pthread_mutex_unlock(&dec->mtx);

    pthread_mutex_destroy(&dec->mtx);
    pthread_cond_destroy(&dec->cond);
    free(dec->pcm);
}

void whisper_decoder_reset(whisper_decoder_t *dec)
{
    if (!dec)
        return;

    pthread_mutex_lock(&dec->mtx);
    dec->has_job = 0;
    dec->abort_flag = 1;
    while (dec->busy)
        pthread_cond_wait(&dec->cond, &dec->mtx);
    dec->abort_flag = 0;
    dec->has_result = 0;
    dec->result_text[0] = '\0';
    dec->result_voiced_f = -1;
    dec->result_all_silent = 0;
    dec->done_at_frame = 0;
    pthread_mutex_unlock(&dec->mtx);
}

static void resolve_lang(const char *req_lang, char *out, size_t out_sz)
{
    if (!req_lang || !*req_lang || strcmp(req_lang, "kbd") == 0) {
        out[0] = '\0';
        return;
    }
    if (strcasecmp(req_lang, "auto") == 0) {
        snprintf(out, out_sz, "auto");
        return;
    }
    if (strlen(req_lang) >= 2 && (req_lang[2] == '-' || req_lang[2] == '_')) {
        char base[4] = {
            (char)tolower((unsigned char)req_lang[0]),
            (char)tolower((unsigned char)req_lang[1]),
            '\0', '\0'
        };
        snprintf(out, out_sz, "%s", base);
        return;
    }
    snprintf(out, out_sz, "%s", req_lang);
}

static int compute_audio_ctx(int n_samples, int is_wake, int is_partial)
{
    int frames = (n_samples / 320) + (is_wake ? 32 : (is_partial ? 48 : 64));
    int aligned = ((frames + 31) / 32) * 32;
    int min_ctx = is_wake ? 256 : (is_partial ? 512 : 768);
    if (aligned < min_ctx)
        aligned = min_ctx;
    if (aligned > 1500)
        aligned = 1500;
    return aligned;
}

float *whisper_normalize_pcm_agc(const float *pcm, int n_samples)
{
    if (!pcm || n_samples <= 0)
        return NULL;

    float *buf = malloc((size_t)n_samples * sizeof(float));
    if (!buf)
        return NULL;

    const float alpha = 0.9695f;
    float prev_x = pcm[0];
    float prev_y = 0.0f;
    float peak = 0.0f;
    double sum_sq = 0.0;

    for (int i = 0; i < n_samples; i++) {
        float x = pcm[i];
        float y = alpha * (prev_y + x - prev_x);
        prev_x = x;
        prev_y = y;
        buf[i] = y;
        float ay = fabsf(y);
        if (ay > peak)
            peak = ay;
        sum_sq += (double)y * (double)y;
    }

    double total_rms = sqrt(sum_sq / (double)n_samples);
    double active_sq = 0.0;
    int active_n = 0;
    float frame_thresh = peak * 0.08f;
    if (frame_thresh < 0.008f)
        frame_thresh = 0.008f;

    for (int base = 0; base + 320 <= n_samples; base += 320) {
        double f_sq = 0.0;
        for (int k = 0; k < 320; k++) {
            float v = buf[base + k];
            f_sq += (double)v * (double)v;
        }
        double f_rms = sqrt(f_sq / 320.0);
        if (f_rms >= (double)frame_thresh) {
            active_sq += f_sq;
            active_n += 320;
        }
    }
    double rms = (active_n >= 1600) ? sqrt(active_sq / (double)active_n) : total_rms;

    if (peak > 0.010f && rms > 0.0020) {
        float gain_peak = 0.78f / peak;
        float gain_rms = (float)(0.11 / rms);
        float gain = (gain_rms < gain_peak) ? gain_rms : gain_peak;
        if (gain > 5.0f)
            gain = 5.0f;
        if (gain < 0.85f)
            gain = 0.85f;
        if (fabsf(gain - 1.0f) > 0.05f) {
            for (int i = 0; i < n_samples; i++) {
                float v = buf[i] * gain;
                if (v > 0.98f)
                    v = 0.98f;
                else if (v < -0.98f)
                    v = -0.98f;
                buf[i] = v;
            }
        }
    }

    return buf;
}

static void trim_inplace(char *s)
{
    for (char *c = s; *c; c++) {
        if (*c == '\r' || *c == '\n' || *c == '\t')
            *c = ' ';
    }
    char *p = s;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
}

void whisper_collapse_repetitions(char *s, size_t s_sz)
{
    if (!s || !*s || s_sz == 0)
        return;

    char copy[4096];
    char *words[256];
    int n_words = 0;

    snprintf(copy, sizeof(copy), "%s", s);

    char *save = NULL;
    for (char *tok = strtok_r(copy, " ", &save); tok && n_words < 256;
         tok = strtok_r(NULL, " ", &save)) {
        words[n_words++] = tok;
    }
    if (n_words < 2)
        return;

    int keep[256];
    for (int i = 0; i < n_words; i++)
        keep[i] = 1;

    for (int span = 8; span >= 1; span--) {
        for (int i = 0; i + 2 * span <= n_words; ) {
            if (!keep[i]) {
                i++;
                continue;
            }
            int match = 1;
            for (int k = 0; k < span; k++) {
                const char *a = words[i + k];
                const char *b = words[i + span + k];
                size_t la = strlen(a), lb = strlen(b);
                while (la > 0 && ispunct((unsigned char)a[la - 1]))
                    la--;
                while (lb > 0 && ispunct((unsigned char)b[lb - 1]))
                    lb--;
                if (la == 0 || la != lb || strncasecmp(a, b, la) != 0) {
                    match = 0;
                    break;
                }
            }
            if (match && (span >= 2 || strlen(words[i]) >= 4)) {
                for (int k = 0; k < span; k++)
                    keep[i + span + k] = 0;
                int dst = 0;
                for (int j = 0; j < n_words; j++) {
                    if (keep[j]) {
                        words[dst] = words[j];
                        keep[dst] = 1;
                        dst++;
                    }
                }
                n_words = dst;
            } else {
                i++;
            }
        }
    }

    for (int k = 6; k >= 1; k--) {
        if (n_words >= 2 * k + 1) {
            int tail_match = 1;
            for (int j = 0; j < k; j++) {
                const char *a = words[j];
                const char *b = words[n_words - k + j];
                size_t la = strlen(a), lb = strlen(b);
                while (la > 0 && ispunct((unsigned char)a[la - 1]))
                    la--;
                while (lb > 0 && ispunct((unsigned char)b[lb - 1]))
                    lb--;
                if (la == 0 || lb == 0) {
                    tail_match = 0;
                    break;
                }
                if (j == k - 1 && k >= 2 && lb < la) {
                    if (strncasecmp(a, b, lb) != 0) {
                        tail_match = 0;
                        break;
                    }
                } else if (la != lb || strncasecmp(a, b, la) != 0) {
                    tail_match = 0;
                    break;
                }
            }
            if (tail_match) {
                if (k == 1) {
                    const char *prev_w = words[n_words - 2];
                    size_t pl = strlen(prev_w);
                    if (pl == 0 || (prev_w[pl - 1] != '.' && prev_w[pl - 1] != '!' && prev_w[pl - 1] != '?'))
                        continue;
                }
                n_words -= k;
                break;
            }
        }
    }

    size_t off = 0;
    s[0] = '\0';
    for (int i = 0; i < n_words; i++) {
        int n = snprintf(s + off, s_sz - off, "%s%s", (i > 0) ? " " : "", words[i]);
        if (n < 0 || (size_t)n >= s_sz - off) {
            s[s_sz - 1] = '\0';
            break;
        }
        off += (size_t)n;
    }
}

static int is_hallucination(const char *s)
{
    (void)s;
    return 0;
}

int whisper_run_slice(struct whisper_context *ctx, int n_threads,
                      const float *pcm, int n_samples,
                      const char *lang, const char *prompt,
                      int max_tok, int is_wake, int is_partial,
                      int use_abort_cb,
                      char *out_text, size_t out_sz, int *out_all_silent)
{
    char eff_lang[32];

    out_text[0] = '\0';
    if (out_all_silent)
        *out_all_silent = 0;
    if (n_samples < 1600)
        return 0;

    resolve_lang(lang, eff_lang, sizeof(eff_lang));

    if (is_partial) {
        int cap = (n_samples / 2200) + 6;
        if (cap < max_tok)
            max_tok = cap;
    }

    struct whisper_full_params wparams = whisper_full_default_params(
        (is_wake || is_partial) ? WHISPER_SAMPLING_GREEDY : WHISPER_SAMPLING_BEAM_SEARCH);
    wparams.n_threads        = n_threads;
    wparams.single_segment   = true;
    wparams.no_context       = true;
    wparams.no_timestamps    = true;
    wparams.suppress_blank   = true;
    wparams.suppress_nst     = true;
    wparams.temperature      = 0.0f;
    wparams.temperature_inc  = (is_wake || is_partial) ? 0.0f : 0.2f;
    if (is_wake || is_partial) {
        wparams.greedy.best_of = 1;
    } else {
        wparams.beam_search.beam_size = 3;
    }
    wparams.max_tokens       = max_tok;
    wparams.audio_ctx        = compute_audio_ctx(n_samples, is_wake, is_partial);

    if (use_abort_cb) {
        wparams.abort_callback = whisper_abort_cb;
        wparams.abort_callback_user_data = NULL;
    }

    if (!whisper_is_multilingual(ctx)) {
        wparams.language = "en";
        wparams.detect_language = false;
    } else if (!eff_lang[0] || strcmp(eff_lang, "auto") == 0) {
        wparams.language = "auto";
        wparams.detect_language = false;
    } else {
        wparams.language = eff_lang;
    }

    if (prompt && *prompt && strcmp(prompt, "-") != 0)
        wparams.initial_prompt = prompt;

    float *agc_pcm = whisper_normalize_pcm_agc(pcm, n_samples);
    const float *eff_pcm = agc_pcm ? agc_pcm : pcm;

    if (whisper_full(ctx, wparams, eff_pcm, n_samples) == 0) {
        int n_seg = whisper_full_n_segments(ctx);
        int valid_seg = 0;
        float nsp_limit = is_wake ? 0.72f : 0.58f;
        whisper_token eot_id = whisper_token_eot(ctx);
        char prev_seg[1024] = {0};

        for (int i = 0; i < n_seg; i++) {
            float nsp = whisper_full_get_segment_no_speech_prob(ctx, i);
            if (nsp > nsp_limit)
                continue;

            int n_tok = whisper_full_n_tokens(ctx, i);
            float sum_p = 0.0f;
            int word_toks = 0;
            int end_tok = n_tok;

            for (int j = 0; j < n_tok; j++) {
                whisper_token_data td = whisper_full_get_token_data(ctx, i, j);
                const char *tt = whisper_full_get_token_text(ctx, i, j);
                if (td.id >= eot_id || !tt || !*tt)
                    continue;
                if (tt[0] == '[' && tt[strlen(tt) - 1] == ']')
                    continue;
                sum_p += td.p;
                word_toks++;
            }
            if (word_toks > 0 && (sum_p / (float)word_toks) < (is_wake ? 0.20f : 0.18f))
                continue;

            if (!is_wake && word_toks >= 2) {
                while (end_tok > 1) {
                    whisper_token_data td = whisper_full_get_token_data(ctx, i, end_tok - 1);
                    const char *tt = whisper_full_get_token_text(ctx, i, end_tok - 1);
                    if (td.id >= eot_id || !tt || !*tt) {
                        end_tok--;
                        continue;
                    }
                    int has_alnum = 0;
                    for (const char *c = tt; *c; c++) {
                        if (isalnum((unsigned char)*c)) {
                            has_alnum = 1;
                            break;
                        }
                    }
                    if (!has_alnum) {
                        end_tok--;
                        continue;
                    }
                    if (td.p < 0.03f) {
                        end_tok--;
                        continue;
                    }
                    break;
                }
            }

            char seg_clean[1024] = {0};
            if (!is_wake && end_tok < n_tok && end_tok > 0) {
                for (int j = 0; j < end_tok; j++) {
                    whisper_token_data td = whisper_full_get_token_data(ctx, i, j);
                    if (td.id >= eot_id)
                        continue;
                    const char *tt = whisper_full_get_token_text(ctx, i, j);
                    if (tt && *tt)
                        strncat(seg_clean, tt, sizeof(seg_clean) - strlen(seg_clean) - 1);
                }
            } else {
                const char *seg = whisper_full_get_segment_text(ctx, i);
                if (!seg)
                    continue;
                snprintf(seg_clean, sizeof(seg_clean), "%s", seg);
            }
            trim_inplace(seg_clean);
            if (is_hallucination(seg_clean))
                continue;
            if (prev_seg[0] != '\0' && strcasecmp(prev_seg, seg_clean) == 0)
                continue;
            snprintf(prev_seg, sizeof(prev_seg), "%s", seg_clean);
            valid_seg++;
            if (out_text[0] != '\0')
                strncat(out_text, " ", out_sz - strlen(out_text) - 1);
            strncat(out_text, seg_clean, out_sz - strlen(out_text) - 1);
        }
        if (valid_seg == 0) {
            if (out_all_silent)
                *out_all_silent = 1;
        }
    }
    free(agc_pcm);
    trim_inplace(out_text);
    whisper_collapse_repetitions(out_text, out_sz);
    return 0;
}

void *whisper_decode_worker_fn(void *arg)
{
    whisper_decoder_t *dec = arg;

    while (g_running && *g_running) {
        pthread_mutex_lock(&dec->mtx);
        while (*g_running && !dec->has_job)
            pthread_cond_wait(&dec->cond, &dec->mtx);
        if (!*g_running) {
            pthread_mutex_unlock(&dec->mtx);
            break;
        }

        dec->has_job = 0;
        dec->busy = 1;
        dec->abort_flag = 0;
        int n_samples = dec->n_samples;
        char lang[32];
        snprintf(lang, sizeof(lang), "%s", dec->lang);
        const char *prompt = dec->prompt;
        int max_tok = dec->max_tok;
        int is_partial = dec->is_partial;
        int64_t job_vf = dec->job_voiced_f;
        pthread_mutex_unlock(&dec->mtx);

        char decoded[4096] = {0};
        int all_silent = 0;
        whisper_run_slice(dec->ctx, dec->n_threads, dec->pcm, n_samples,
                          lang, prompt, max_tok, 0, is_partial, 1,
                          decoded, sizeof(decoded), &all_silent);

        int64_t cur_audio_f = 0;  // Would need audio_ctx to get this
        (void)cur_audio_f;

        pthread_mutex_lock(&dec->mtx);
        dec->busy = 0;
        dec->done_at_frame = cur_audio_f;
        if (!dec->abort_flag && (!g_cancel_req || !*g_cancel_req)) {
            snprintf(dec->result_text, sizeof(dec->result_text), "%s", decoded);
            dec->result_voiced_f = job_vf;
            dec->result_is_partial = is_partial;
            dec->result_all_silent = all_silent;
            dec->has_result = 1;
        }
        pthread_cond_broadcast(&dec->cond);
        pthread_mutex_unlock(&dec->mtx);
    }
    return NULL;
}

int whisper_decoder_submit(whisper_decoder_t *dec,
                           int64_t start_f, int64_t end_f, int64_t voiced_f,
                           const char *lang, const char *prompt,
                           int max_tok, int is_partial,
                           int (*ring_extract)(int64_t, int64_t, float*, int))
{
    if (!dec || !ring_extract)
        return 0;

    pthread_mutex_lock(&dec->mtx);
    if (dec->busy || dec->has_job) {
        pthread_mutex_unlock(&dec->mtx);
        return 0;
    }
    int n_samp = ring_extract(start_f, end_f, dec->pcm,
                              1500 * AUDIO_FRAME_SAMPLES);
    if (n_samp < 2400) {
        pthread_mutex_unlock(&dec->mtx);
        return 0;
    }
    dec->n_samples = n_samp;
    snprintf(dec->lang, sizeof(dec->lang), "%s", lang ? lang : "kbd");
    dec->prompt = prompt;
    dec->max_tok = max_tok;
    dec->is_partial = is_partial;
    dec->job_voiced_f = voiced_f;
    dec->abort_flag = 0;
    dec->has_job = 1;
    pthread_cond_broadcast(&dec->cond);
    pthread_mutex_unlock(&dec->mtx);
    return 1;
}

void whisper_decoder_force_final(whisper_decoder_t *dec,
                                 int64_t start_f, int64_t end_f, int64_t voiced_f,
                                 const char *lang, const char *prompt, int max_tok,
                                 int (*ring_extract)(int64_t, int64_t, float*, int))
{
    if (!dec || !ring_extract)
        return;

    pthread_mutex_lock(&dec->mtx);
    if (dec->busy) {
        if (!dec->is_partial && dec->job_voiced_f >= voiced_f - 1) {
            pthread_mutex_unlock(&dec->mtx);
            return;
        }
        dec->abort_flag = 1;
        while (dec->busy)
            pthread_cond_wait(&dec->cond, &dec->mtx);
    } else if (!dec->has_job && dec->has_result &&
               !dec->result_is_partial && dec->result_voiced_f >= voiced_f - 1) {
        pthread_mutex_unlock(&dec->mtx);
        return;
    }
    dec->has_job = 0;
    dec->has_result = 0;
    int n_samp = ring_extract(start_f, end_f, dec->pcm,
                              1500 * AUDIO_FRAME_SAMPLES);
    if (n_samp >= 2400) {
        dec->n_samples = n_samp;
        snprintf(dec->lang, sizeof(dec->lang), "%s", lang ? lang : "kbd");
        dec->prompt = prompt;
        dec->max_tok = max_tok;
        dec->is_partial = 0;
        dec->job_voiced_f = voiced_f;
        dec->abort_flag = 0;
        dec->has_job = 1;
        pthread_cond_broadcast(&dec->cond);
    }
    pthread_mutex_unlock(&dec->mtx);
}