#ifndef JARVIS_VOICE_H
#define JARVIS_VOICE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct whisper_context;
struct whisper_decoder;
struct jarvis_engine;
struct jarvis_ontology;

typedef struct {
    int port;
    const char *lang;
    double thresh_db;
    int cmd_silence_ms;
    const char *wake_word;
    const char *state_file;
    const char *chime_wav;
    int followup_ms;
    int srv_fd;
    int cli_fd;
    struct whisper_context *ctx_wake;
    struct whisper_decoder *decoder;
    void *audio_ctx;
    struct jarvis_engine *engine;
    struct jarvis_ontology *ontology;
    int *running;
    volatile sig_atomic_t *stop_req;
    volatile sig_atomic_t *cancel_req;
} voice_config_t;

/* Main voice handler - runs until command received or timeout */
int voice_handle_wait(const voice_config_t *cfg, char *out_cmd, size_t out_sz);

/* Streaming mode handler */
void voice_handle_stream(int srv_fd, int cli_fd, const char *mode, const char *lang,
                         double thresh_db, int silence_ms, int wait_ms,
                         const char *state_file, void *audio_ctx,
                         struct whisper_decoder *decoder);

/* In-process Jarvis turn (process command, execute, speak) */
void voice_run_turn(int srv_fd, struct whisper_context *ctx_wake, int n_threads,
                    int *io_next_followup, void *audio_ctx,
                    struct jarvis_engine *engine, struct jarvis_ontology *ontology,
                    int *running, volatile sig_atomic_t *stop_req,
                    volatile sig_atomic_t *cancel_req);

/* Check if speaker is playing */
int voice_is_speaker_playing(void);

/* Read runtime config */
void voice_read_runtime_cfg(char *lang, size_t lang_sz, int *out_followup_ms,
                            char *llm_mode, size_t mode_sz);

#ifdef __cplusplus
}
#endif

#endif /* JARVIS_VOICE_H */