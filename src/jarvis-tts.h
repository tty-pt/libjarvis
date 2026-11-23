#ifndef JARVIS_TTS_H
#define JARVIS_TTS_H

#include <stddef.h>

int jarvis_tts_speak_clause(const char *session_id, const char *clause_text, int is_final);
int jarvis_tts_interrupt(int dummy);
void jarvis_tts_resolve_voice(char *out, size_t out_sz);

#endif /* JARVIS_TTS_H */
