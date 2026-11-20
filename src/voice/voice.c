/* See LICENSE file for copyright and license details.
 * Voice pipeline module for jarvisd.
 */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "voice.h"
#include "../audio/audio.h"
#include "../whisper/whisper_mod.h"
#include "../tts/tts.h"
#include <ttypt/jarvis.h>

#define MAX_UTTER_FRAMES 1500
#define WARM_HOLD_MS 60000L
#define FRAME_MS 20
#define FRAME_SAMPLES 320

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

static void write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t w = write(fd, buf, len);
        if (w <= 0)
            break;
        buf += w;
        len -= (size_t)w;
    }
}

static const char *get_username(void)
{
    const char *u = getenv("USER");
    return (u && *u) ? u : "quirinpa";
}

static void notify_bar(void)
{
    char path[256];
    FILE *fp;
    int pid = 0;

    snprintf(path, sizeof(path), "/tmp/dwm-bar-%s.pid", get_username());
    fp = fopen(path, "r");
    if (!fp)
        return;
    if (fscanf(fp, "%d", &pid) == 1 && pid > 1)
        kill((pid_t)pid, SIGUSR1);
    fclose(fp);
}

static void notify_osd_async(const char *title, const char *body, int timeout_ms)
{
    if (!title || !*title)
        return;
    if (fork() == 0) {
        char tbuf[32];
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        snprintf(tbuf, sizeof(tbuf), "%d", timeout_ms > 0 ? timeout_ms : 4000);
        execlp("notify-send", "notify-send",
               "-t", tbuf,
               "-h", "string:x-dunst-stack-tag:voice",
               title, (body && *body) ? body : "", (char *)NULL);
        _exit(0);
    }
}

static void write_state_file(const char *path, const char *prefix, const char *detail, const char *osd_title)
{
    char tmp[512];
    FILE *fp;

    if (!path || !*path || strcmp(path, "-") == 0)
        return;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fp = fopen(tmp, "wb");
    if (!fp)
        return;
    if (detail && *detail)
        fprintf(fp, "%s:%s\n", prefix, detail);
    else
        fprintf(fp, "%s\n", prefix);
    fclose(fp);
    rename(tmp, path);
    notify_bar();

    if (osd_title && *osd_title && detail && *detail)
        notify_osd_async(osd_title, detail, 6000);
}

static void play_chime_async(const char *chime_wav)
{
    if (!chime_wav || !*chime_wav || strcmp(chime_wav, "-") == 0)
        return;
    if (access(chime_wav, R_OK) != 0)
        return;
    if (fork() == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        execlp("pw-play", "pw-play", "--volume=0.75", chime_wav, (char *)NULL);
        _exit(0);
    }
}

static int is_tts_currently_speaking(char *out_text, size_t out_sz)
{
    static char s_last_tts[1024] = {0};
    static long s_last_tts_ms = 0;

    if (out_text && out_sz > 0)
        out_text[0] = '\0';
    char path[256];
    snprintf(path, sizeof(path), "/tmp/jarvis-tts-active-%s.state", get_username());
    FILE *fp = fopen(path, "r");
    if (fp) {
        char buf[1024] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        buf[n] = '\0';
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || buf[i] == '\r')
                buf[i] = ' ';
        }
        // trim_inplace would be needed
        if (buf[0]) {
            snprintf(s_last_tts, sizeof(s_last_tts), "%s", buf);
            s_last_tts_ms = now_ms();
        }
        if (out_text && out_sz > 0)
            snprintf(out_text, out_sz, "%.*s", (int)(out_sz - 1), buf);
        return 1;
    }
    if (s_last_tts[0] != '\0' && (now_ms() - s_last_tts_ms) <= 450L) {
        if (out_text && out_sz > 0)
            snprintf(out_text, out_sz, "%.*s", (int)(out_sz - 1), s_last_tts);
        return 1;
    }
    return 0;
}

int voice_is_speaker_playing(void)
{
    FILE *fp = popen("pactl list sink-inputs short 2>/dev/null", "r");
    if (!fp)
        return 0;
    char line[256];
    int has_line = (fgets(line, sizeof(line), fp) != NULL && line[0] != '\0');
    pclose(fp);
    return has_line;
}

void voice_read_runtime_cfg(char *lang, size_t lang_sz,
                            int *out_followup_ms,
                            char *llm_mode, size_t mode_sz)
{
    const char *def_lang = "pt";
    if (!jarvis_rc_get("lang", def_lang, lang, lang_sz))
        snprintf(lang, lang_sz, "%s", def_lang);
    *out_followup_ms = jarvis_rc_get_int("followup_ms", 0);
    if (*out_followup_ms < 0 || *out_followup_ms > 3600000)
        *out_followup_ms = 0;
    jarvis_rc_get("llm_mode", "auto", llm_mode, mode_sz);
}

int voice_handle_wait(const voice_config_t *cfg, char *out_cmd, size_t out_sz)
{
    (void)cfg; (void)out_cmd; (void)out_sz;
    return 2;
}

void voice_handle_stream(int srv_fd, int cli_fd, const char *mode, const char *lang,
                         double thresh_db, int silence_ms, int wait_ms,
                         const char *state_file, void *audio_ctx,
                         struct whisper_decoder *decoder)
{
    (void)srv_fd; (void)cli_fd; (void)mode; (void)lang;
    (void)thresh_db; (void)silence_ms; (void)wait_ms;
    (void)state_file; (void)audio_ctx; (void)decoder;
}

void voice_run_turn(int srv_fd, struct whisper_context *ctx_wake, int n_threads,
                    int *io_next_followup, void *audio_ctx,
                    struct jarvis_engine *engine, struct jarvis_ontology *ontology,
                    int *running, volatile sig_atomic_t *stop_req,
                    volatile sig_atomic_t *cancel_req)
{
    (void)srv_fd; (void)ctx_wake; (void)n_threads; (void)io_next_followup;
    (void)audio_ctx; (void)engine; (void)ontology; (void)running;
    (void)stop_req; (void)cancel_req;
}