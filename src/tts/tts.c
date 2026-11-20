/* See LICENSE file for copyright and license details.
 * TTS module for jarvisd.
 */

#include <dlfcn.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tts.h"

typedef struct {
    void *ort_dl;
    void *espeak_dl;
    const void **ort_vtbl;
    void *ort_sess;
    void *ort_mem_info;
    void *ort_env;
    char loaded_model[512];
    char loaded_voice[512];
    float *kokoro_style;
} tts_state_t;

struct tts_ctx {
    tts_state_t state;
};

#define ORT_API_VERSION 18

typedef enum {
    ORT_IDX_CreateEnv = 0,
    ORT_IDX_CreateSession = 1,
    ORT_IDX_Run = 2,
    ORT_IDX_CreateSessionOptions = 3,
    ORT_IDX_DisableCpuMemArena = 4,
    ORT_IDX_SetGraphOptimizationLevel = 5,
    ORT_IDX_SetIntraOpNumThreads = 6,
    ORT_IDX_SetInterOpNumThreads = 7,
    ORT_IDX_CreateTensorWithData = 8,
    ORT_IDX_GetTensorMutableData = 9,
    ORT_IDX_GetTensorShapeElementCount = 10,
    ORT_IDX_GetTensorTypeAndShape = 11,
    ORT_IDX_CreateCpuMemoryInfo = 12,
    ORT_IDX_ReleaseEnv = 13,
    ORT_IDX_ReleaseStatus = 14,
    ORT_IDX_ReleaseMemoryInfo = 15,
    ORT_IDX_ReleaseSession = 16,
    ORT_IDX_ReleaseValue = 17,
    ORT_IDX_ReleaseTensorTypeAndShape = 18,
    ORT_IDX_ReleaseSessionOptions = 19,
} ort_idx_t;

typedef void *(*OrtGetApiBase_fn)(void);
typedef const void *(*OrtGetApi_fn)(int version);

typedef void *(*ort_CreateEnv_fn)(int, const char *, void **);
typedef void *(*ort_CreateSessionOptions_fn)(void **);
typedef void (*ort_DisableCpuMemArena_fn)(void *);
typedef void (*ort_SetGraphOptLevel_fn)(void *, int);
typedef void (*ort_SetIntraThreads_fn)(void *, int);
typedef void (*ort_SetInterThreads_fn)(void *, int);
typedef void *(*ort_CreateSession_fn)(void *, const char *, void *, void **);
typedef void *(*ort_CreateCpuMemoryInfo_fn)(int, int, void **);
typedef void *(*ort_CreateTensorWithData_fn)(void *, int, const int64_t *, int, void *, size_t, void **);
typedef void *(*ort_GetTensorMutableData_fn)(void *, void *);
typedef int64_t (*ort_GetTensorShapeElementCount_fn)(void *, void *);
typedef void *(*ort_GetTensorTypeAndShape_fn)(void *, void *, void **);
typedef void (*ort_Release_fn)(void *);
typedef void (*ort_ReleaseSessionOptions_fn)(void *);

typedef void (*espeak_Init_fn)(void);
typedef void (*espeak_SetVoice_fn)(const char *);
typedef void (*espeak_TextToPhonemesTerm_fn)(const char *, void *, int);
typedef void (*espeak_Terminate_fn)(void);

tts_ctx_t *tts_create(void)
{
    tts_ctx_t *ctx = calloc(1, sizeof(*ctx));
    return ctx;
}

void tts_destroy(tts_ctx_t *ctx)
{
    if (!ctx)
        return;

    tts_state_t *st = &ctx->state;

    if (st->ort_vtbl) {
        ort_Release_fn rel_sess = (ort_Release_fn)st->ort_vtbl[ORT_IDX_ReleaseSession];
        ort_Release_fn rel_mi   = (ort_Release_fn)st->ort_vtbl[ORT_IDX_ReleaseMemoryInfo];
        ort_Release_fn rel_env  = (ort_Release_fn)st->ort_vtbl[ORT_IDX_ReleaseEnv];
        if (st->ort_sess && rel_sess) rel_sess(st->ort_sess);
        if (st->ort_mem_info && rel_mi) rel_mi(st->ort_mem_info);
        if (st->ort_env && rel_env) rel_env(st->ort_env);
    }
    st->ort_sess = NULL;
    st->ort_mem_info = NULL;
    st->ort_env = NULL;
    st->ort_vtbl = NULL;
    st->loaded_model[0] = '\0';
    st->loaded_voice[0] = '\0';

    if (st->kokoro_style) {
        free(st->kokoro_style);
        st->kokoro_style = NULL;
    }

    if (st->ort_dl) {
        dlclose(st->ort_dl);
        st->ort_dl = NULL;
    }
    if (st->espeak_dl) {
        if (st->espeak_Terminate) st->espeak_Terminate();
        dlclose(st->espeak_dl);
        st->espeak_dl = NULL;
    }
    free(ctx);
}

static bool ort_validate_vtable(const void **vtbl)
{
    if (!vtbl) return false;
    for (int i = 0; i <= ORT_IDX_ReleaseSessionOptions; i++) {
        if (!vtbl[i]) return false;
    }
    return true;
}

bool tts_ensure_loaded(tts_ctx_t *ctx, const char *voice)
{
    if (!ctx) return false;

    tts_state_t *st = &ctx->state;

    if (st->ort_vtbl && st->loaded_voice[0] && strcmp(st->loaded_voice, voice) == 0)
        return true;

    const char *home = getenv("HOME");
    if (!home || !*home) return false;

    char model_path[512];
    snprintf(model_path, sizeof(model_path), "%s/.local/share/kokoro/kokoro-v1.0.onnx", home);

    if (!st->ort_dl) {
        st->ort_dl = dlopen("libonnxruntime.so", RTLD_LAZY | RTLD_LOCAL);
        if (!st->ort_dl) return false;
    }

    OrtGetApiBase_fn get_base = (OrtGetApiBase_fn)dlsym(st->ort_dl, "OrtGetApiBase");
    if (!get_base || !get_base()) return false;

    st->ort_vtbl = (const void **)get_base()->GetApi(ORT_API_VERSION);
    if (!ort_validate_vtable(st->ort_vtbl)) {
        tts_destroy(ctx);
        return false;
    }

    ort_CreateEnv_fn create_env = (ort_CreateEnv_fn)st->ort_vtbl[ORT_IDX_CreateEnv];
    ort_CreateCpuMemoryInfo_fn create_mi = (ort_CreateCpuMemoryInfo_fn)st->ort_vtbl[ORT_IDX_CreateCpuMemoryInfo];
    void *status = create_env(3, "mod_tts", &st->ort_env);
    if (status) { ((ort_Release_fn)st->ort_vtbl[ORT_IDX_ReleaseStatus])(status); return false; }
    status = create_mi(1, 0, &st->ort_mem_info);
    if (status) { ((ort_Release_fn)st->ort_vtbl[ORT_IDX_ReleaseStatus])(status); return false; }

    ort_CreateSessionOptions_fn create_so = (ort_CreateSessionOptions_fn)st->ort_vtbl[ORT_IDX_CreateSessionOptions];
    ort_DisableCpuMemArena_fn disable_arena = (ort_DisableCpuMemArena_fn)st->ort_vtbl[ORT_IDX_DisableCpuMemArena];
    ort_SetGraphOptLevel_fn set_opt = (ort_SetGraphOptLevel_fn)st->ort_vtbl[ORT_IDX_SetGraphOptimizationLevel];
    ort_SetIntraThreads_fn set_intra = (ort_SetIntraThreads_fn)st->ort_vtbl[ORT_IDX_SetIntraOpNumThreads];
    ort_SetInterThreads_fn set_inter = (ort_SetInterThreads_fn)st->ort_vtbl[ORT_IDX_SetInterOpNumThreads];
    ort_CreateSession_fn create_sess = (ort_CreateSession_fn)st->ort_vtbl[ORT_IDX_CreateSession];
    ort_ReleaseSessionOptions_fn rel_so = (ort_ReleaseSessionOptions_fn)st->ort_vtbl[ORT_IDX_ReleaseSessionOptions];

    void *so = NULL;
    status = create_so(&so);
    if (status) { ((ort_Release_fn)st->ort_vtbl[ORT_IDX_ReleaseStatus])(status); return false; }
    disable_arena(so);
    set_opt(so, 99);
    set_intra(so, 1);
    set_inter(so, 1);
    status = create_sess(st->ort_env, model_path, so, &st->ort_sess);
    if (status) { ((ort_Release_fn)st->ort_vtbl[ORT_IDX_ReleaseStatus])(status); rel_so(so); return false; }
    rel_so(so);

    snprintf(st->loaded_model, sizeof(st->loaded_model), "%s", model_path);
    snprintf(st->loaded_voice, sizeof(st->loaded_voice), "%s", voice ? voice : "default");

    return true;
}

int tts_synthesize(tts_ctx_t *ctx, const char *text, float **out_pcm, int *out_samples)
{
    (void)ctx; (void)text; (void)out_pcm; (void)out_samples;
    return -1;
}

bool tts_write_earcon(const char *path, bool is_done)
{
    if (!path || !*path) return false;

    FILE *fp = fopen(path, "wb");
    if (!fp) return false;

    const int sample_rate = 16000;
    const int duration_ms = 120;
    const int n_samples = sample_rate * duration_ms / 1000;
    const float freq_start = is_done ? 659.25f : 587.33f;
    const float freq_end = is_done ? 523.25f : 880.00f;

    int16_t *buf = malloc(n_samples * sizeof(int16_t));
    if (!buf) { fclose(fp); return false; }

    for (int i = 0; i < n_samples; i++) {
        float t = (float)i / sample_rate;
        float freq = freq_start + (freq_end - freq_start) * t / (duration_ms / 1000.0f);
        float env = 1.0f - t / (duration_ms / 1000.0f);
        float sample = 0.3f * env * sinf(2.0f * 3.14159265f * freq * t);
        buf[i] = (int16_t)(sample * 32767.0f);
    }

    char wav_hdr[44];
    memset(wav_hdr, 0, 44);
    memcpy(wav_hdr, "RIFF", 4);
    *(uint32_t*)(wav_hdr+4) = 36 + n_samples * 2;
    memcpy(wav_hdr+8, "WAVE", 4);
    memcpy(wav_hdr+12, "fmt ", 4);
    *(uint32_t*)(wav_hdr+16) = 16;
    *(uint16_t*)(wav_hdr+20) = 1;
    *(uint16_t*)(wav_hdr+22) = 1;
    *(uint32_t*)(wav_hdr+24) = sample_rate;
    *(uint32_t*)(wav_hdr+28) = sample_rate * 2;
    *(uint16_t*)(wav_hdr+32) = 2;
    *(uint16_t*)(wav_hdr+34) = 16;
    memcpy(wav_hdr+36, "data", 4);
    *(uint32_t*)(wav_hdr+40) = n_samples * 2;

    fwrite(wav_hdr, 1, 44, fp);
    fwrite(buf, 2, n_samples, fp);
    fclose(fp);
    free(buf);
    return true;
}

void tts_clean_text(const char *raw, char *out, size_t out_sz)
{
    if (!raw || !out || out_sz == 0) return;

    size_t len = strlen(raw);
    size_t j = 0;
    for (size_t i = 0; i < len && j < out_sz - 1; i++) {
        unsigned char c = raw[i];
        if (c == '*' || c == '_' || c == '`' || c == '#' || c == '[' || c == ']' ||
            c == '(' || c == ')' || c == '{' || c == '}')
            continue;
        if (c == '\n' || c == '\r' || c == '\t') {
            if (j > 0 && out[j-1] != ' ')
                out[j++] = ' ';
            continue;
        }
        out[j++] = c;
    }
    out[j] = '\0';

    while (j > 0 && isspace((unsigned char)out[j-1]))
        out[--j] = '\0';
}

size_t tts_split_segments(const char *text, char out_segments[][256], size_t max_segments)
{
    if (!text || !*text || !out_segments || max_segments == 0) return 0;

    size_t count = 0;
    const char *p = text;
    size_t text_len = strlen(text);

    while (*p && count < max_segments) {
        size_t remaining = text_len - (p - text);
        size_t max_len = (count == 0) ? 36 : 95;
        if (remaining <= max_len) {
            snprintf(out_segments[count], 256, "%s", p);
            count++;
            break;
        }

        size_t cut = max_len;
        for (size_t i = max_len; i > max_len / 2; i--) {
            if (p[i] == ' ' || p[i] == ',' || p[i] == '.' || p[i] == ';' || p[i] == ':') {
                cut = i + 1;
                break;
            }
        }

        snprintf(out_segments[count], 256, "%.*s", (int)cut, p);
        out_segments[count][255] = '\0';
        count++;
        p += cut;
        while (*p == ' ') p++;
    }

    return count;
}