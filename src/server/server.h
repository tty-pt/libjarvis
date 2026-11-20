#ifndef JARVIS_SERVER_H
#define JARVIS_SERVER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct whisper_context;
struct whisper_decoder;

typedef struct {
    int port;
    int n_threads;
    struct whisper_context *ctx_main;
    struct whisper_context *ctx_wake;
    struct whisper_decoder *decoder;
    void *audio_ctx;
    void *jarvis_engine;
    int *running;
    volatile sig_atomic_t *stop_req;
    volatile sig_atomic_t *cancel_req;
    volatile sig_atomic_t *preempt_cli;
    char *preempt_buf;
    size_t preempt_buf_sz;
} server_config_t;

typedef struct server_ctx server_ctx_t;

server_ctx_t *server_create(const server_config_t *cfg);
void server_destroy(server_ctx_t *ctx);

/* Main server loop - blocks until shutdown */
int server_run(server_ctx_t *ctx);

/* Handle a single client request (non-blocking) */
void server_handle_client(server_ctx_t *ctx, int cli_fd, char *buf, size_t len);

/* Send control message to daemon */
bool server_send_control(int port, const char *msg);

#ifdef __cplusplus
}
#endif

#endif /* JARVIS_SERVER_H */