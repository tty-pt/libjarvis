/* See LICENSE file for copyright and license details.
 * jarvisd - Main entry point, wiring all modules together.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <ttypt/jarvis.h>
#include <whisper.h>

/* Forward declarations for rc functions */
extern bool jarvis_rc_get(const char *key, const char *fallback, char *out, size_t out_sz);
extern int jarvis_rc_get_int(const char *key, int fallback);

#include "audio/audio.h"
#include "vad/vad.h"
#include "whisper/whisper_mod.h"
#include "server/server.h"
#include "tts/tts.h"
#include "voice/voice.h"
#include "jarvisd_internal.h"

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t g_stop_req = 0;
static volatile sig_atomic_t g_cancel_req = 0;
static volatile sig_atomic_t g_jarvis_active = 0;
static volatile sig_atomic_t g_preempt_cli = -1;
static char g_preempt_buf[2048];

static jarvis_engine_t *g_jarvis_eng = NULL;

static void on_sig(int sig)
{
    (void)sig;
    running = 0;
    g_cancel_req = 1;
}

static void on_cli_sigint(int sig)
{
    (void)sig;
    g_stop_req = 1;
}

static void on_cli_sigusr1(int sig)
{
    (void)sig;
    g_cancel_req = 1;
}

static bool send_control_msg(int port, const char *msg);

static void quiet_log(enum ggml_log_level level, const char *text, void *user_data)
{
    (void)level; (void)text; (void)user_data;
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

static void init_jarvis_xylem_runtime(void)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/home/quirinpa";

    xy_init();

    static char wm_buf[64];
    jarvis_rc_get("wm", "dwm", wm_buf, sizeof(wm_buf));

    char mods_cfg[512];
    if (jarvis_rc_get("mods", NULL, mods_cfg, sizeof(mods_cfg)) && mods_cfg[0]) {
        char *save = NULL;
        for (char *tok = strtok_r(mods_cfg, ":, ", &save); tok; tok = strtok_r(NULL, ":, ", &save)) {
            if (*tok) {
                char clean[512];
                if (tok[0] == '/' || tok[0] == '.') {
                    snprintf(clean, sizeof(clean), "%s", tok);
                } else {
                    snprintf(clean, sizeof(clean), "%s/.local/lib/%s", home, tok);
                }
                size_t l = strlen(clean);
                if (l > 3 && strcmp(clean + l - 3, ".so") == 0)
                    clean[l - 3] = '\0';
                if (xy_load(clean) != XY_OK && tok[0] != '/') {
                    snprintf(clean, sizeof(clean), "%s", tok);
                    l = strlen(clean);
                    if (l > 3 && strcmp(clean + l - 3, ".so") == 0)
                        clean[l - 3] = '\0';
                    xy_load(clean);
                }
            }
        }
    } else {
        char wm_mod[128];
        snprintf(wm_mod, sizeof(wm_mod), "mod_%s", wm_buf[0] ? wm_buf : "dwm");
        char clean[512];
        snprintf(clean, sizeof(clean), "%s/.local/lib/%s", home, wm_mod);
        size_t l = strlen(clean);
        if (l > 3 && strcmp(clean + l - 3, ".so") == 0)
            clean[l - 3] = '\0';
        xy_load(clean);
        snprintf(clean, sizeof(clean), "%s/.local/lib/mod_tts", home);
        l = strlen(clean);
        if (l > 3 && strcmp(clean + l - 3, ".so") == 0)
            clean[l - 3] = '\0';
        xy_load(clean);
    }

    char cfg_path[512];
    snprintf(cfg_path, sizeof(cfg_path), "%s/.config/jarvis/mods.load", home);
    FILE *mfp = fopen(cfg_path, "r");
    if (mfp) {
        char mline[512];
        while (fgets(mline, sizeof(mline), mfp)) {
            char *p = mline;
            while (*p && isspace((unsigned char)*p)) p++;
            size_t len = strlen(p);
            while (len > 0 && isspace((unsigned char)p[len - 1])) p[--len] = '\0';
            if (p[0] && p[0] != '#') {
                char clean[512];
                if (p[0] == '/' || p[0] == '.') {
                    snprintf(clean, sizeof(clean), "%s", p);
                } else {
                    snprintf(clean, sizeof(clean), "%s/.local/lib/%s", home, p);
                }
                size_t l = strlen(clean);
                if (l > 3 && strcmp(clean + l - 3, ".so") == 0)
                    clean[l - 3] = '\0';
                xy_load(clean);
            }
        }
        fclose(mfp);
    }

    const char *u = get_username();
    char chime_wav[256], done_wav[256];
    snprintf(chime_wav, sizeof(chime_wav), "/tmp/jarvis-chime-%s.wav", u);
    snprintf(done_wav, sizeof(done_wav), "/tmp/jarvis-done-%s.wav", u);
    if (access(chime_wav, R_OK) != 0)
        tts_write_earcon(chime_wav, 0);
    if (access(done_wav, R_OK) != 0)
        tts_write_earcon(done_wav, 1);

    static char ont_path[512], int_path[512], ctx_path[512], sess_path[512], mem_path[512], emb_path[512], auren_url[256];
    char mode[64], lang[32];
    int fms = 0;
    voice_read_runtime_cfg(lang, sizeof(lang), &fms, mode, sizeof(mode));

    char def_ont[512], def_int[512], def_mem[512], def_emb[512];
    snprintf(def_ont, sizeof(def_ont), "%s/.config/jarvis/ontology.json", home);
    snprintf(def_int, sizeof(def_int), "%s/.config/jarvis/few_shots.json", home);
    snprintf(def_mem, sizeof(def_mem), "%s/.config/jarvis/memories.json", home);
    snprintf(def_emb, sizeof(def_emb), "%s/.cache/jarvis/embeddings.json", home);

    jarvis_rc_get("ontology_path", def_ont, ont_path, sizeof(ont_path));
    jarvis_rc_get("few_shot_path", def_int, int_path, sizeof(int_path));
    snprintf(ctx_path, sizeof(ctx_path), "/tmp/jarvis-%s.context.json", u);
    snprintf(sess_path, sizeof(sess_path), "/tmp/jarvis-%s.session.json", u);
    jarvis_rc_get("memory_path", def_mem, mem_path, sizeof(mem_path));
    jarvis_rc_get("embed_cache_path", def_emb, emb_path, sizeof(emb_path));
    jarvis_rc_get("auren_url", "http://127.0.0.1:4242", auren_url, sizeof(auren_url));

    jarvis_config_t jcfg;
    jarvis_config_init(&jcfg);
    jcfg.wm = wm_buf[0] ? wm_buf : "dwm";
    jcfg.ontology_path = ont_path;
    if (access(int_path, R_OK) == 0)
        jcfg.few_shot_path = int_path;
    jcfg.context_path = ctx_path;
    jcfg.session_path = sess_path;
    jcfg.memory_path = mem_path;
    jcfg.embed_cache_path = emb_path;
    jcfg.auren_url = auren_url;
    jcfg.llm_model = "fast";
    if (strcmp(mode, "off") == 0 || strcmp(mode, "none") == 0)
        jcfg.infer_mode = JARVIS_INFER_NONE;
    else if (strcmp(mode, "embedding") == 0)
        jcfg.infer_mode = JARVIS_INFER_EMBEDDING;
    else if (strcmp(mode, "smart") == 0) {
        jcfg.infer_mode = JARVIS_INFER_LLM;
        jcfg.llm_model = "smart";
    } else if (strcmp(mode, "kilo") == 0) {
        jcfg.infer_mode = JARVIS_INFER_LLM;
        jcfg.llm_model = "kilo-auto/free";
    } else if (strcmp(mode, "cloud") == 0 || strcmp(mode, "remote") == 0 ||
               strcmp(mode, "llm") == 0 || strcmp(mode, "fast") == 0)
        jcfg.infer_mode = JARVIS_INFER_LLM;
    else
        jcfg.infer_mode = JARVIS_INFER_AUTO;

    g_jarvis_eng = jarvis_engine_create(&jcfg);
}

static void jarvis_set_active(int active)
{
    const char *u = get_username();
    char state_file[256], jpid_file[256], apid_file[256];
    char legacy_state[256], legacy_jpid[256], legacy_apid[256];
    snprintf(state_file, sizeof(state_file), "/tmp/jarvis-voice-%s.state", u);
    snprintf(jpid_file, sizeof(jpid_file), "/tmp/jarvis-%s.pid", u);
    snprintf(apid_file, sizeof(apid_file), "/tmp/jarvis-acting-%s.pid", u);
    snprintf(legacy_state, sizeof(legacy_state), "/tmp/dwm-voice-%s.state", u);
    snprintf(legacy_jpid, sizeof(legacy_jpid), "/tmp/dwm-jarvis-%s.pid", u);
    snprintf(legacy_apid, sizeof(legacy_apid), "/tmp/dwm-acting-%s.pid", u);
    if (active) {
        FILE *fp = fopen(jpid_file, "w");
        if (fp) {
            fprintf(fp, "%d\n", (int)getpid());
            fclose(fp);
        }
        write_state_file(state_file, "jarvis", NULL, NULL);
        g_jarvis_active = 1;
    } else {
        g_jarvis_active = 0;
        g_cancel_req = 1;
        jarvis_xy_tts_interrupt(0);
        unlink(jpid_file);
        unlink(apid_file);
        unlink(state_file);
        unlink(legacy_jpid);
        unlink(legacy_apid);
        unlink(legacy_state);
        notify_bar();
    }
}

static int run_client_mode(int port, const char *req)
{
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_cli_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_cli_sigusr1;
    sigaction(SIGUSR1, &sa, NULL);

    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        return 3;
    struct timeval tv_to = { .tv_sec = 4, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv_to, sizeof(tv_to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv_to, sizeof(tv_to));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return 3;
    }

    char send_buf[2048];
    snprintf(send_buf, sizeof(send_buf), "%s\n", req);
    write(s, send_buf, strlen(send_buf));

    int sent_stop = 0;
    char resp[4096];
    size_t total = 0;

    while (total < sizeof(resp) - 1) {
        if (g_cancel_req) {
            send_control_msg(port, "CANCEL\n");
            close(s);
            return 130;
        }
        if (g_stop_req && !sent_stop) {
            send_control_msg(port, "STOP\n");
            sent_stop = 1;
        }

        struct pollfd pfd;
        pfd.fd = s;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 40);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            continue;
        if (pfd.revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(s, resp + total, sizeof(resp) - 1 - total);
            if (n <= 0)
                break;
            total += (size_t)n;
            if (memchr(resp, '\n', total))
                break;
        }
    }
    close(s);
    resp[total] = '\0';

    char *p = resp;
    while (*p && isspace((unsigned char)*p)) p++;
    size_t l = strlen(p);
    while (l > 0 && isspace((unsigned char)p[l - 1])) p[--l] = '\0';

    if (strcmp(p, "CANCELLED") == 0)
        return 130;
    if (strcmp(p, "EMPTY") == 0 || strcmp(p, "TIMEOUT") == 0 || p[0] == '\0')
        return 2;
    if (strncmp(p, "OK|", 3) == 0) {
        printf("%s\n", p + 3);
        return 0;
    }
    if (strncmp(p, "CMD|", 4) == 0) {
        printf("%s\n", p + 4);
        return 0;
    }
    printf("%s\n", p);
    return 0;
}

static bool send_control_msg(int port, const char *msg)
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
        write(s, msg, strlen(msg));

    close(s);
    return true;
}

static void resolve_default_whisper_models(char *main_out, size_t main_sz, char *wake_out, size_t wake_sz)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/home/quirinpa";

    if (!jarvis_rc_get("whisper_model", NULL, main_out, main_sz) || !main_out[0] || access(main_out, R_OK) != 0) {
        static const char *const main_cands[] = {
            "ggml-small-q8_0.bin",
            "ggml-small-q5_1.bin",
            "ggml-base-q8_0.bin",
            "ggml-base-q5_1.bin",
            "ggml-base.bin",
            "ggml-base.en-q8_0.bin",
            "ggml-small.en-q5_1.bin",
            NULL
        };
        main_out[0] = '\0';
        for (size_t i = 0; main_cands[i]; i++) {
            char cand[512];
            snprintf(cand, sizeof(cand), "%s/.local/share/whisper/%s", home, main_cands[i]);
            if (access(cand, R_OK) == 0) {
                snprintf(main_out, main_sz, "%s", cand);
                break;
            }
        }
    }

    if (!jarvis_rc_get("whisper_wake_model", NULL, wake_out, wake_sz) || !wake_out[0] || access(wake_out, R_OK) != 0) {
        static const char *const wake_cands[] = {
            "ggml-base-q8_0.bin",
            "ggml-base-q5_1.bin",
            "ggml-base.bin",
            NULL
        };
        wake_out[0] = '\0';
        for (size_t i = 0; wake_cands[i]; i++) {
            char cand[512];
            snprintf(cand, sizeof(cand), "%s/.local/share/whisper/%s", home, wake_cands[i]);
            if (access(cand, R_OK) == 0) {
                snprintf(wake_out, wake_sz, "%s", cand);
                break;
            }
        }
        if (!wake_out[0])
            snprintf(wake_out, wake_sz, "%s", main_out);
    }
}

int main(int argc, char *argv[])
{
    const char *main_model_path = NULL;
    const char *wake_model_path = NULL;
    const char *client_req = NULL;
    int port = jarvis_rc_get_int("port", 8179);
    int n_threads = jarvis_rc_get_int("threads", 6);
    int start_jarvis = 0;
    int opt;

    while ((opt = getopt(argc, argv, "m:w:p:t:c:jh")) != -1) {
        switch (opt) {
        case 'm': main_model_path = optarg; break;
        case 'w': wake_model_path = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 't': n_threads = atoi(optarg); break;
        case 'c': client_req = optarg; break;
        case 'j': start_jarvis = 1; break;
        default:
            fprintf(stderr,
                "Usage:\n"
                "  Daemon: %s [-m <main-model.bin>] [-w <wake-model.bin>] [-p port] [-t threads] [-j]\n"
                "  Client: %s [-p port] -c \"<COMMAND|...>\"\n",
                argv[0], argv[0]);
            return 1;
        }
    }

    if (client_req)
        return run_client_mode(port, client_req);

    static char s_main_model[512], s_wake_model[512];
    resolve_default_whisper_models(s_main_model, sizeof(s_main_model), s_wake_model, sizeof(s_wake_model));
    if (!main_model_path || !*main_model_path)
        main_model_path = s_main_model;
    if (!wake_model_path || !*wake_model_path)
        wake_model_path = s_wake_model;

    if (!main_model_path || !*main_model_path) {
        fprintf(stderr, "jarvisd: no whisper model found in ~/.jarvisrc or ~/.local/share/whisper/\n");
        return 1;
    }

    char lock_path[256], pid_path[256];
    snprintf(lock_path, sizeof(lock_path), "/tmp/jarvisd-%s.lock", get_username());
    snprintf(pid_path, sizeof(pid_path), "/tmp/jarvisd-%s.pid", get_username());
    int lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (lock_fd < 0 || fcntl(lock_fd, F_SETLK, &fl) < 0) {
        if (start_jarvis)
            send_control_msg(port, "JARVIS_ON\n");
        if (lock_fd >= 0)
            close(lock_fd);
        return 0;
    }
    {
        FILE *pfp = fopen(pid_path, "w");
        if (pfp) {
            fprintf(pfp, "%d\n", (int)getpid());
            fclose(pfp);
        }
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    init_jarvis_xylem_runtime();
    if (start_jarvis)
        jarvis_set_active(1);

    ggml_backend_load_all();
    whisper_log_set(quiet_log, NULL);

    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = true;
    cparams.flash_attn = true;

    struct whisper_context *ctx_main = whisper_init_from_file_with_params(main_model_path, cparams);
    if (!ctx_main) {
        fprintf(stderr, "jarvisd: failed to load main model %s\n", main_model_path);
        return 1;
    }

    struct whisper_context *ctx_wake = NULL;
    if (wake_model_path && *wake_model_path && strcmp(wake_model_path, main_model_path) != 0 &&
        access(wake_model_path, R_OK) == 0) {
        ctx_wake = whisper_init_from_file_with_params(wake_model_path, cparams);
    }
    if (!ctx_wake)
        ctx_wake = ctx_main;

    audio_ctx_t *audio = audio_create();
    if (!audio) {
        fprintf(stderr, "jarvisd: failed to create audio context\n");
        return 1;
    }
    audio_set_running(audio, &running);

    whisper_decoder_t decoder;
    if (whisper_decoder_init(&decoder, ctx_main, n_threads, MAX_UTTER_FRAMES) != 0) {
        fprintf(stderr, "jarvisd: failed to init whisper decoder\n");
        return 1;
    }
    whisper_set_global_signals(&running, &g_cancel_req, &g_stop_req);

    tts_ctx_t *tts = tts_create();

    pthread_t audio_tid, decode_tid;
    pthread_create(&audio_tid, NULL, audio_thread_fn, audio);
    pthread_create(&decode_tid, NULL, whisper_decode_worker_fn, &decoder);

    server_config_t srv_cfg = {
        .port = port,
        .n_threads = n_threads,
        .ctx_main = ctx_main,
        .ctx_wake = ctx_wake,
        .decoder = (struct whisper_decoder *)&decoder,
        .audio_ctx = audio,
        .jarvis_engine = g_jarvis_eng,
        .running = (int *)&running,
        .stop_req = (volatile sig_atomic_t *)&g_stop_req,
        .cancel_req = (volatile sig_atomic_t *)&g_cancel_req,
        .preempt_cli = &g_preempt_cli,
        .preempt_buf = g_preempt_buf,
        .preempt_buf_sz = sizeof(g_preempt_buf),
    };

    server_ctx_t *server = server_create(&srv_cfg);
    if (!server) {
        fprintf(stderr, "jarvisd: failed to create server\n");
        return 1;
    }

    int next_followup = 0;
    int srv_fd = server_get_fd(server);

    while (running) {
        if (g_jarvis_active) {
            run_inprocess_jarvis_turn(srv_fd, ctx_wake, n_threads, &next_followup);
            continue;
        }

        next_followup = 0;
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int cli_fd = accept(srv_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (cli_fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        fcntl(cli_fd, F_SETFD, FD_CLOEXEC);
        char buf[2048];
        ssize_t n = read(cli_fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            close(cli_fd);
            continue;
        }
        buf[n] = '\0';

        server_handle_client(server, cli_fd, buf, n);
    }

    running = 0;
    if (g_jarvis_eng) {
        jarvis_engine_destroy(g_jarvis_eng);
        g_jarvis_eng = NULL;
    }
    g_cancel_req = 1;
    pthread_cond_t *audio_cond = audio_get_cond(audio);
    if (audio_cond)
        pthread_cond_broadcast(audio_cond);
    pthread_cond_broadcast(&decoder.cond);
    pthread_join(audio_tid, NULL);
    pthread_join(decode_tid, NULL);

    server_destroy(server);
    whisper_decoder_fini(&decoder);
    audio_destroy(audio);
    tts_destroy(tts);
    close(srv_fd);
    unlink(pid_path);
    close(lock_fd);
    whisper_free(ctx_main);
    if (ctx_wake != ctx_main)
        whisper_free(ctx_wake);
    return 0;
}