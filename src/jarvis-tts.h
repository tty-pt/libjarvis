#ifndef JARVIS_TTS_H
#define JARVIS_TTS_H

#include <stdbool.h>
#include <stddef.h>

int jarvis_tts_speak_clause(const char *session_id, const char *clause_text, int is_final);
int jarvis_tts_speak_clause_lang(const char *session_id, const char *clause_text, const char *lang_hint, int is_final);
int jarvis_tts_precache_clause(const char *clause_text, const char *lang_hint);
bool jarvis_tts_is_enabled(void);
int jarvis_tts_interrupt(int dummy);
void jarvis_tts_resolve_voice(char *out, size_t out_sz);

#endif /* JARVIS_TTS_H */
