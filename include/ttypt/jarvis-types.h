#ifndef TTYPT_JARVIS_TYPES_H
#define TTYPT_JARVIS_TYPES_H

/**
 * @file jarvis-types.h
 * @brief Shared types and voice/UI constants for libjarvis.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Named Buffer & Capacity Constants
 * ------------------------------------------------------------------------- */

#define JARVIS_SHORT_LEN       32
#define JARVIS_WORD_LEN        48
#define JARVIS_ID_LEN          64
#define JARVIS_NAME_LEN        128
#define JARVIS_STR_LEN         256
#define JARVIS_CLAUSE_LEN      512
#define JARVIS_TEXT_LEN        1024
#define JARVIS_OUTPUT_LEN      2048

#define JARVIS_SAMPLE_RATE     16000
#define JARVIS_FRAME_MS        20
#define JARVIS_FRAME_SAMPLES   ((JARVIS_SAMPLE_RATE * JARVIS_FRAME_MS) / 1000)

/* Legacy aliases */
#define JARVIS_MAX_STR         JARVIS_STR_LEN
#define JARVIS_MAX_TEXT        JARVIS_TEXT_LEN

/**
 * @brief Voice listener lifecycle state.
 */
typedef enum {
	JARVIS_VOICE_OFF       = 0,
	JARVIS_VOICE_STANDBY   = 1, /* Listening for wake word */
	JARVIS_VOICE_FOLLOWUP  = 2, /* Wake-free follow-up window */
	JARVIS_VOICE_HEARING   = 3, /* Speech detected, streaming STT */
	JARVIS_VOICE_ACTING    = 4, /* Executing command / reasoning */
	JARVIS_VOICE_SPEAKING  = 5  /* Neural TTS output active */
} jarvis_voice_state_t;

/**
 * @brief Voice VAD / endpointing configuration.
 */
typedef struct {
	double threshold_db;        /* Default -35.0 dB */
	int    cmd_silence_ms;      /* Default 420 ms */
	int    followup_ms;         /* Default 8000 ms (0 = strict wake-word, 3600000 = wake-free) */
	char   wake_word[JARVIS_ID_LEN];
	char   lang[JARVIS_SHORT_LEN];
	char   tts_voice[JARVIS_ID_LEN];
	bool   tts_enabled;
} jarvis_voice_config_t;

#ifdef __cplusplus
}
#endif

#endif /* TTYPT_JARVIS_TYPES_H */
