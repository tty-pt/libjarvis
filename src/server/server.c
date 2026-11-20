/* See LICENSE file for copyright and license details.
 * Control server module for jarvisd.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "server.h"
#include "../audio/audio.h"
#include "../whisper/whisper_mod.h"
#include <ttypt/jarvis.h>

struct server_ctx {
    int srv_fd;
    server_config_t cfg;
    pthread_t thread;
    volatile sig_atomic_t running;
};

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

server_ctx_t *server_create(const server_config_t *cfg)
{
    if (!cfg)
        return NULL;

    server_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->cfg = *cfg;
    ctx->running = 1;

    int srv = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv < 0) {
        free(ctx);
        return NULL;
    }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(srv, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)cfg->port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(srv, 16) < 0) {
        close(srv);
        free(ctx);
        return NULL;
    }

    ctx->srv_fd = srv;
    return ctx;
}

void server_destroy(server_ctx_t *ctx)
{
    if (!ctx)
        return;

    ctx->running = 0;
    if (ctx->srv_fd >= 0)
        close(ctx->srv_fd);
    free(ctx);
}

bool server_send_control(int port, const char *msg)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        write_all(s, msg, strlen(msg));

    close(s);
    return true;
}

static void handle_status(server_ctx_t *ctx, int cli_fd)
{
    audio_state_t state;
    audio_get_state(ctx->cfg.audio_ctx, &state);

    char sbuf[256];
    double ldb = (state.total_frames > 0) ? state.db[(state.total_frames - 1) % AUDIO_RING_FRAMES] : -100.0;
    snprintf(sbuf, sizeof(sbuf),
             "STATUS|frames=%lld|nf_db=%.1f|std_db=%.1f|last_db=%.1f|pw_open=%d|in_speech=%d|jarvis=%d\n",
             (long long)state.total_frames, state.noise_floor_db, state.ambient_std_db,
             ldb, state.pw_open, state.in_speech_hint, 0);
    write_all(cli_fd, sbuf, strlen(sbuf));
}

static void handle_jarvis_status(server_ctx_t *ctx, int cli_fd, int active)
{
    const char *st = active ? "ON\n" : "OFF\n";
    write_all(cli_fd, st, strlen(st));
}

static void handle_stream(server_ctx_t *ctx, int cli_fd, char *buf, size_t len)
{
    char *save = NULL;
    char *cmd = strtok_r(buf, "|", &save);
    if (!cmd || strcmp(cmd, "STREAM") != 0) {
        close(cli_fd);
        return;
    }

    char *mode       = strtok_r(NULL, "|", &save);
    char *lang       = strtok_r(NULL, "|", &save);
    char *thresh_str = strtok_r(NULL, "|", &save);
    char *sil_str    = strtok_r(NULL, "|", &save);
    char *wait_str   = strtok_r(NULL, "|", &save);
    char *state_file = strtok_r(NULL, "|", &save);

    if (mode && lang && thresh_str && sil_str && wait_str) {
        voice_handle_stream(ctx->srv_fd, cli_fd, mode, lang,
                           strtod(thresh_str, NULL), atoi(sil_str),
                           atoi(wait_str),
                           state_file ? state_file : "-",
                           ctx->cfg.audio_ctx,
                           ctx->cfg.decoder);
    } else {
        close(cli_fd);
    }
}

static void handle_jarvis_wait(server_ctx_t *ctx, int cli_fd, char *buf, size_t len)
{
    char *save = NULL;
    char *cmd = strtok_r(buf, "|", &save);
    if (!cmd || strcmp(cmd, "JARVIS_WAIT") != 0) {
        close(cli_fd);
        return;
    }

    char *lang         = strtok_r(NULL, "|", &save);
    char *thresh_str   = strtok_r(NULL, "|", &save);
    char *sil_str      = strtok_r(NULL, "|", &save);
    char *wake_word    = strtok_r(NULL, "|", &save);
    char *state_file   = strtok_r(NULL, "|", &save);
    char *chime_wav    = strtok_r(NULL, "|", &save);
    char *followup_str = strtok_r(NULL, "|", &save);

    if (lang && thresh_str && sil_str && wake_word) {
        char out_cmd[4096];
        voice_handle_wait((voice_config_t){
            .port = ctx->cfg.port,
            .lang = lang,
            .thresh_db = strtod(thresh_str, NULL),
            .cmd_silence_ms = atoi(sil_str),
            .wake_word = wake_word,
            .state_file = state_file ? state_file : "-",
            .chime_wav = chime_wav ? chime_wav : "-",
            .followup_ms = followup_str ? atoi(followup_str) : 0,
            .srv_fd = ctx->srv_fd,
            .cli_fd = cli_fd,
            .ctx_wake = ctx->cfg.ctx_wake,
            .decoder = ctx->cfg.decoder,
            .audio_ctx = ctx->cfg.audio_ctx,
            .engine = ctx->cfg.jarvis_engine,
            .ontology = NULL, // Would need to pass this
            .running = ctx->cfg.running,
            .stop_req = ctx->cfg.stop_req,
            .cancel_req = ctx->cfg.cancel_req,
        }, out_cmd, sizeof(out_cmd));
    }
    close(cli_fd);
}

static void handle_transcribe(server_ctx_t *ctx, int cli_fd, char *buf, size_t len)
{
    char *save = NULL;
    char *cmd = strtok_r(buf, "|", &save);
    if (!cmd || strcmp(cmd, "TRANSCRIBE") != 0) {
        close(cli_fd);
        return;
    }

    char *mode     = strtok_r(NULL, "|", &save);
    char *lang     = strtok_r(NULL, "|", &save);
    char *mt_str   = strtok_r(NULL, "|", &save);
    char *prompt   = strtok_r(NULL, "|", &save);
    char *wav_path = strtok_r(NULL, "|", &save);

    if (!mode || !lang || !mt_str || !prompt || !wav_path) {
        close(cli_fd);
        return;
    }

    int is_wake = (strcmp(mode, "wake") == 0);
    int is_cmd  = (strcmp(mode, "cmd") == 0);
    int max_tok = atoi(mt_str);
    if (max_tok <= 0)
        max_tok = is_wake ? 8 : (is_cmd ? 24 : 96);

    float *pcm = NULL;
    int n_samples = 0;
    // load_wav_f32 would be needed here
    (void)pcm; (void)n_samples;

    close(cli_fd);
}

void server_handle_client(server_ctx_t *ctx, int cli_fd, char *buf, size_t len)
{
    if (strncmp(buf, "GET /", 5) == 0 || strncmp(buf, "PING", 4) == 0) {
        const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
        write_all(cli_fd, ok, strlen(ok));
        close(cli_fd);
        return;
    }
    if (strncmp(buf, "STATUS", 6) == 0) {
        handle_status(ctx, cli_fd);
        close(cli_fd);
        return;
    }
    if (strncmp(buf, "JARVIS_STATUS", 13) == 0) {
        handle_jarvis_status(ctx, cli_fd, 0); // Would need jarvis_active
        close(cli_fd);
        return;
    }
    if (strncmp(buf, "JARVIS_ON", 9) == 0) {
        // jarvis_set_active(1);
        write_all(cli_fd, "ON\n", 3);
        close(cli_fd);
        return;
    }
    if (strncmp(buf, "JARVIS_OFF", 10) == 0) {
        // jarvis_set_active(0);
        write_all(cli_fd, "OFF\n", 4);
        close(cli_fd);
        return;
    }
    if (strncmp(buf, "JARVIS_TOGGLE", 13) == 0) {
        write_all(cli_fd, "OFF\n", 4);
        close(cli_fd);
        return;
    }
    if (strncmp(buf, "TTS_INTERRUPT", 13) == 0) {
        // jarvis_xy_tts_interrupt(0);
        write_all(cli_fd, "OK\n", 3);
        close(cli_fd);
        return;
    }
    if (strncmp(buf, "TTS_SPEAK|", 10) == 0) {
        char *txt = buf + 10;
        char *nl = strpbrk(txt, "\r\n");
        if (nl) *nl = '\0';
        if (*txt) {
            // jarvis_xy_tts_interrupt(0);
            // jarvis_xy_speak_clause("oneshot", txt, 1);
        }
        write_all(cli_fd, "OK\n", 3);
        close(cli_fd);
        return;
    }
    if (strncmp(buf, "STOP", 4) == 0) {
        if (ctx->cfg.stop_req) *ctx->cfg.stop_req = 1;
        write_all(cli_fd, "OK\n", 3);
        close(cli_fd);
        return;
    }
    if (strncmp(buf, "CANCEL", 6) == 0) {
        if (ctx->cfg.cancel_req) *ctx->cfg.cancel_req = 1;
        write_all(cli_fd, "OK\n", 3);
        close(cli_fd);
        return;
    }

    char *nl = strpbrk(buf, "\r\n");
    if (nl) *nl = '\0';

    if (strncmp(buf, "STREAM|", 7) == 0) {
        handle_stream(ctx, cli_fd, buf, len);
        return;
    }
    if (strncmp(buf, "JARVIS_WAIT|", 12) == 0) {
        handle_jarvis_wait(ctx, cli_fd, buf, len);
        return;
    }
    if (strncmp(buf, "TRANSCRIBE|", 11) == 0) {
        handle_transcribe(ctx, cli_fd, buf, len);
        return;
    }

    close(cli_fd);
}

int server_run(server_ctx_t *ctx)
{
    if (!ctx)
        return -1;

    while (ctx->running && ctx->cfg.running && *ctx->cfg.running) {
        int cli_fd = -1;
        char buf[2048];

        if (ctx->cfg.preempt_cli && *ctx->cfg.preempt_cli >= 0) {
            cli_fd = *ctx->cfg.preempt_cli;
            *ctx->cfg.preempt_cli = -1;
            snprintf(buf, sizeof(buf), "%s", ctx->cfg.preempt_buf);
            server_handle_client(ctx, cli_fd, buf, strlen(buf));
            continue;
        }

        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        cli_fd = accept(ctx->srv_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (cli_fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        fcntl(cli_fd, F_SETFD, FD_CLOEXEC);
        ssize_t n = read(cli_fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            close(cli_fd);
            continue;
        }
        buf[n] = '\0';

        server_handle_client(ctx, cli_fd, buf, n);
    }

    return 0;
}