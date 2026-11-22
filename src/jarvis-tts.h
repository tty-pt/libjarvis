#ifndef JARVIS_TTS_H
#define JARVIS_TTS_H

int jarvis_tts_speak_clause(const char *session_id, const char *clause_text, int is_final);
int jarvis_tts_interrupt(int dummy);

#endif /* JARVIS_TTS_H */
