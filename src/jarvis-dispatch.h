#ifndef JARVIS_DISPATCH_H
#define JARVIS_DISPATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ttypt/stele.h>
#include <ttypt/jarvis.h>

/* Forward declare dispatch interface */
typedef struct jarvis_dispatch jarvis_dispatch_t;

int jarvis_dispatch_send_ren_cmd(const char *cmd, char *out_reply, size_t out_sz, int (*is_active_fn)(void));
bool jarvis_dispatch_to_ren(const char *utterance, const jarvis_ontology_t *ont, const char *wake_buf,
                           int *out_started_media, int *out_conv_exited,
                           char *out_err, size_t err_sz, int (*is_stopped_fn)(void));

#endif /* JARVIS_DISPATCH_H */
