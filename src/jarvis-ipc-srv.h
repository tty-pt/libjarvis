#ifndef JARVIS_IPC_SRV_H
#define JARVIS_IPC_SRV_H

#include <stdbool.h>
#include <stddef.h>
#include <ttypt/stele.h>
#include "jarvis-stt-backend.h"

bool jarvis_is_english_only_whisper_model(const char *path);
void jarvis_resolve_stt_models(jarvis_hw_tier_t tier,
                               char *main_out, size_t main_sz,
                               char *wake_out, size_t wake_sz);
void jarvis_resolve_default_whisper_models(char *main_out, size_t main_sz, char *wake_out, size_t wake_sz);

#endif /* JARVIS_IPC_SRV_H */
