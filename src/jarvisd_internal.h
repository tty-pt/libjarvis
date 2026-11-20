#ifndef JARVISD_INTERNAL_H
#define JARVISD_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Internal daemon state control - callable from server module */
void jarvis_set_active(int active);

/* Run one voice turn (wake word + command processing) */
void run_inprocess_jarvis_turn(int srv, struct whisper_context *ctx_wake, int n_threads,
                               int *io_next_followup);

#ifdef __cplusplus
}
#endif

#endif /* JARVISD_INTERNAL_H */