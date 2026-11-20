/* See LICENSE file for copyright and license details.
 * Audio capture & ring buffer module for jarvisd.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "audio.h"
#include "vad.h"

struct audio_ctx {
    pthread_t thread;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    float pcm[AUDIO_RING_SAMPLES];
    float db[AUDIO_RING_FRAMES];
    uint8_t is_speech[AUDIO_RING_FRAMES];
    int64_t total_frames;
    int64_t last_cmd_end_f;
    double noise_floor_db;
    double ambient_std_db;
    int env_frames;
    int active_users;
    long hold_until_ms;
    int in_speech_hint;
    int pw_open;
    vad_ctx_t *vad;
    int pw_fd;
    pid_t pw_pid;
    volatile sig_atomic_t *running;
    vad_biquad_t vad_flt;
};

static int spawn_pw_record(int *out_fd, pid_t *out_pid)
{
    int pfd[2];
    if (pipe(pfd) < 0)
        return -1;
    fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("pw-record", "pw-record", "--raw", "--rate=16000",
               "--channels=1", "--format=s16", "--latency=20ms", "-", (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    *out_fd = pfd[0];
    *out_pid = pid;
    return 0;
}

static void close_pw_record(int *fd, pid_t *pid)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
    if (*pid > 0) {
        kill(*pid, SIGTERM);
        *pid = -1;
    }
}

audio_ctx_t *audio_create(void)
{
    audio_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->mtx = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    ctx->cond = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
    ctx->noise_floor_db = -42.0;
    ctx->ambient_std_db = 1.5;
    ctx->hold_until_ms = 0;

    ctx->vad = vad_create(ctx->noise_floor_db, -35.0);
    if (!ctx->vad) {
        free(ctx);
        return NULL;
    }

    return ctx;
}

void audio_destroy(audio_ctx_t *ctx)
{
    if (!ctx)
        return;

    if (ctx->running) {
        *ctx->running = 0;
        pthread_cond_broadcast(&ctx->cond);
        pthread_join(ctx->thread, NULL);
    }

    close_pw_record(&ctx->pw_fd, &ctx->pw_pid);
    vad_destroy(ctx->vad);
    pthread_mutex_destroy(&ctx->mtx);
    pthread_cond_destroy(&ctx->cond);
    free(ctx);
}

int audio_acquire(audio_ctx_t *ctx)
{
    if (!ctx)
        return -1;

    pthread_mutex_lock(&ctx->mtx);
    ctx->active_users++;
    int64_t f0 = ctx->total_frames;
    pthread_cond_broadcast(&ctx->cond);

    long deadline = audio_now_ms() + 350L;
    while (*ctx->running && (!ctx->pw_open || ctx->total_frames == f0 || ctx->env_frames < 6)) {
        long rem = deadline - audio_now_ms();
        if (rem <= 0)
            break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (rem > 25 ? 25 : rem) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&ctx->cond, &ctx->mtx, &ts);
    }
    pthread_mutex_unlock(&ctx->mtx);
    return 0;
}

void audio_release(audio_ctx_t *ctx, long hold_ms)
{
    if (!ctx)
        return;

    pthread_mutex_lock(&ctx->mtx);
    if (ctx->active_users > 0)
        ctx->active_users--;
    ctx->in_speech_hint = 0;
    long until = audio_now_ms() + hold_ms;
    if (until > ctx->hold_until_ms)
        ctx->hold_until_ms = until;
    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->mtx);
}

int audio_get_state(audio_ctx_t *ctx, audio_state_t *out_state)
{
    if (!ctx || !out_state)
        return -1;

    pthread_mutex_lock(&ctx->mtx);
    out_state->pcm = ctx->pcm;
    out_state->db = ctx->db;
    out_state->is_speech = ctx->is_speech;
    out_state->total_frames = ctx->total_frames;
    out_state->last_cmd_end_f = ctx->last_cmd_end_f;
    out_state->noise_floor_db = ctx->noise_floor_db;
    out_state->ambient_std_db = ctx->ambient_std_db;
    out_state->env_frames = ctx->env_frames;
    out_state->active_users = ctx->active_users;
    out_state->hold_until_ms = ctx->hold_until_ms;
    out_state->in_speech_hint = ctx->in_speech_hint;
    out_state->pw_open = ctx->pw_open;
    pthread_mutex_unlock(&ctx->mtx);
    return 0;
}

int audio_extract_pcm(audio_ctx_t *ctx, int64_t start_f, int64_t end_f,
                      float *out_pcm, int max_samples)
{
    if (!ctx || !out_pcm)
        return -1;

    int n_samples = 0;
    pthread_mutex_lock(&ctx->mtx);
    if (end_f > ctx->total_frames)
        end_f = ctx->total_frames;
    if (end_f - start_f > AUDIO_RING_FRAMES - 4)
        start_f = end_f - (AUDIO_RING_FRAMES - 4);
    if (start_f < 0)
        start_f = 0;

    for (int64_t f = start_f; f < end_f; f++) {
        if (n_samples + AUDIO_FRAME_SAMPLES > max_samples)
            break;
        int slot = (int)(f % AUDIO_RING_FRAMES);
        memcpy(out_pcm + n_samples, ctx->pcm + (size_t)slot * AUDIO_FRAME_SAMPLES,
               (size_t)AUDIO_FRAME_SAMPLES * sizeof(float));
        n_samples += AUDIO_FRAME_SAMPLES;
    }
    pthread_mutex_unlock(&ctx->mtx);
    return n_samples;
}

long audio_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

void *audio_thread_fn(void *arg)
{
    audio_ctx_t *ctx = arg;
    int16_t raw[AUDIO_FRAME_SAMPLES];

    while (*ctx->running) {
        pthread_mutex_lock(&ctx->mtx);
        int need = (ctx->active_users > 0) || (audio_now_ms() < ctx->hold_until_ms);
        if (!need) {
            if (ctx->pw_fd >= 0) {
                ctx->pw_open = 0;
                pthread_mutex_unlock(&ctx->mtx);
                close_pw_record(&ctx->pw_fd, &ctx->pw_pid);
                pthread_mutex_lock(&ctx->mtx);
            }
            while (*ctx->running && ctx->active_users <= 0 && audio_now_ms() >= ctx->hold_until_ms)
                pthread_cond_wait(&ctx->cond, &ctx->mtx);
            pthread_mutex_unlock(&ctx->mtx);
            continue;
        }
        pthread_mutex_unlock(&ctx->mtx);

        if (ctx->pw_fd < 0) {
            if (spawn_pw_record(&ctx->pw_fd, &ctx->pw_pid) < 0) {
                struct timespec ts = {0, 50000000L};
                nanosleep(&ts, NULL);
                continue;
            }
            pthread_mutex_lock(&ctx->mtx);
            ctx->pw_open = 1;
            pthread_cond_broadcast(&ctx->cond);
            pthread_mutex_unlock(&ctx->mtx);
        }

        size_t got = 0;
        while (got < (size_t)(AUDIO_FRAME_SAMPLES * sizeof(int16_t))) {
            ssize_t r = read(ctx->pw_fd, (char *)raw + got,
                             (size_t)(AUDIO_FRAME_SAMPLES * sizeof(int16_t)) - got);
            if (r <= 0) {
                if (r < 0 && errno == EINTR && *ctx->running)
                    continue;
                break;
            }
            got += (size_t)r;
        }
        if (got < (size_t)(AUDIO_FRAME_SAMPLES * sizeof(int16_t))) {
            close_pw_record(&ctx->pw_fd, &ctx->pw_pid);
            pthread_mutex_lock(&ctx->mtx);
            ctx->pw_open = 0;
            pthread_mutex_unlock(&ctx->mtx);
            struct timespec ts = {0, 20000000L};
            nanosleep(&ts, NULL);
            continue;
        }

        float db = vad_calc_frame_db(raw, AUDIO_FRAME_SAMPLES, &ctx->vad_flt);

        pthread_mutex_lock(&ctx->mtx);
        int slot = (int)(ctx->total_frames % AUDIO_RING_FRAMES);
        float *dst = ctx->pcm + (size_t)slot * AUDIO_FRAME_SAMPLES;
        for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++)
            dst[i] = (float)raw[i] / 32768.0f;
        ctx->db[slot] = db;
        ctx->is_speech[slot] = (ctx->env_frames >= 10 &&
                                db > ctx->noise_floor_db + 8.5) ? 1 : 0;
        ctx->total_frames++;

        vad_update_ambient(ctx->vad, ctx->db, ctx->total_frames,
                           AUDIO_RING_FRAMES, ctx->is_speech);

        pthread_cond_broadcast(&ctx->cond);
        pthread_mutex_unlock(&ctx->mtx);
    }

    close_pw_record(&ctx->pw_fd, &ctx->pw_pid);
    return NULL;
}

pthread_cond_t *audio_get_cond(audio_ctx_t *ctx)
{
    if (!ctx)
        return NULL;
    return &ctx->cond;
}

void audio_set_running(audio_ctx_t *ctx, volatile sig_atomic_t *running)
{
    if (!ctx)
        return;
    ctx->running = running;
}