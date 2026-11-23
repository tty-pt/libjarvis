#ifndef TTYPT_JARVIS_H
#define TTYPT_JARVIS_H

/**
 * @file jarvis.h
 * @brief Public API for libjarvis — Multilingual Ontology/UI, Voice VAD,
 *        Wake-Word Matching, Streaming TTS Segmentation, and Config RC helpers.
 */

#include <ttypt/jarvis-types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Core Ontology & Text Normalization API
 * ------------------------------------------------------------------------- */

typedef struct jarvis_ontology jarvis_ontology_t;
typedef struct jarvis_vad      jarvis_vad_t;

/**
 * @brief Clean phrase: UTF-8 diacritic fold + lowercase alphanumeric + single-space normalized.
 *
 * Writes normalized string into @p out (up to @p out_sz bytes) and returns
 * length written.
 */
size_t jarvis_clean_phrase(const char *text, char *out, size_t out_sz);

/**
 * @brief Detect non-speech ASR hallucination (e.g. "vvvvvvvv", single-char stutter, ontology phrases).
 */
bool jarvis_is_non_speech_hallucination(const char *text);

/**
 * @brief Detect whether a shell command matches destructive patterns
 *        (e.g. `rm -rf`, `git reset --hard`, `mkfs`, `dd if=`, `kill -9`).
 */
bool jarvis_is_destructive_command(const char *cmd);

/**
 * @brief Create and load an ontology from @p json_path.
 *
 * If @p json_path is NULL or empty, checks `$JARVIS_ONTOLOGY_JSON`,
 * `~/.config/jarvis/ontology.json`, and local fallback paths.
 */
jarvis_ontology_t *jarvis_ontology_create(const char *json_path);

/**
 * @brief Create and load an ontology from @p json_path, applying @p lang locale overrides if present.
 */
jarvis_ontology_t *jarvis_ontology_create_for_lang(const char *json_path, const char *lang);

/**
 * @brief Load ontology directly from an in-memory JSON string.
 */
jarvis_ontology_t *jarvis_ontology_create_from_json(const char *json_str);

/**
 * @brief Load ontology from an in-memory JSON string, applying @p lang locale overrides if present.
 */
jarvis_ontology_t *jarvis_ontology_create_from_json_for_lang(const char *json_str, const char *lang);

/**
 * @brief Destroy an ontology instance.
 */
void jarvis_ontology_destroy(jarvis_ontology_t *ont);

bool jarvis_ontology_is_tts_interrupt(const jarvis_ontology_t *ont, const char *cleaned_text);
bool jarvis_ontology_is_tts_interrupt_ex(
	const jarvis_ontology_t *ont,
	const char              *cleaned_text,
	const char              *custom_wake_word
);
bool jarvis_ontology_is_op_abort(
	const jarvis_ontology_t *ont,
	const char              *phrase,
	const char              *custom_wake_word
);
bool jarvis_ontology_is_screen_lock_phrase(const jarvis_ontology_t *ont, const char *phrase);
bool jarvis_ontology_matches_tier1_trigger(const jarvis_ontology_t *ont, const char *text);
const char *jarvis_ontology_get_wake_word(const jarvis_ontology_t *ont);
const char *jarvis_ontology_get_lang(const jarvis_ontology_t *ont);
const char *jarvis_ontology_get_lang_name(const jarvis_ontology_t *ont);
const char *jarvis_ontology_get_wake_prompt(const jarvis_ontology_t *ont);
const char *jarvis_ontology_get_cmd_prompt(const jarvis_ontology_t *ont);
const char *jarvis_ontology_get_ui_string(const jarvis_ontology_t *ont, const char *key, const char *fallback);
void jarvis_ontology_format_ui(
	const jarvis_ontology_t *ont,
	const char              *key,
	const char              *custom_wake_word,
	const char              *fallback,
	char                    *out,
	size_t                   out_sz
);
const char *jarvis_ontology_get_reply_template(const jarvis_ontology_t *ont, const char *key);

/* ---------------------------------------------------------------------------
 * Streaming TTS Clause Extraction & RC Config
 * ------------------------------------------------------------------------- */

/**
 * @brief Incremental streaming answer prefix extractor for low-latency TTS.
 */
bool jarvis_extract_streaming_answer_prefix(
	const char *raw_json_fragment,
	char       *out_answer,
	size_t      out_sz
);

/**
 * @brief Pop the next ready spoken clause from @p unspoken buffer for immediate TTS.
 *
 * For the very first clause (@p is_first_clause == true), splits around 18-50 chars
 * on a natural preposition/conjunction or punctuation boundary for ~1.0s TTFA.
 */
bool jarvis_pop_ready_tts_clause(
	const char *unspoken,
	bool        is_first_clause,
	char       *out_clause,
	size_t      clause_sz,
	char       *out_remainder,
	size_t      rem_sz
);

/**
 * @brief Read a string setting from `~/.jarvisrc` (or `~/.config/jarvis/jarvisrc`).
 *
 * Expands leading `~/` to `$HOME/`. If @p key is not found, writes @p fallback
 * into @p out and returns false.
 */
bool jarvis_rc_get(const char *key, const char *fallback, char *out, size_t out_sz);

/**
 * @brief Read an integer setting from `~/.jarvisrc`.
 */
int jarvis_rc_get_int(const char *key, int fallback);

/**
 * @brief Update or insert a `key = value` pair in `~/.jarvisrc`.
 */
bool jarvis_rc_set(const char *key, const char *value);

/**
 * @brief Remove @p key from `~/.jarvisrc` (if present).
 */
bool jarvis_rc_unset(const char *key);

/**
 * @brief Resolve effective language with zero-config fallbacks:
 *        `$JARVIS_LANG` -> `~/.jarvisrc` (`lang`) -> `$LC_ALL`/`$LC_MESSAGES`/`$LANG` -> ontology -> `"en"`.
 */
void jarvis_resolve_lang(const jarvis_ontology_t *ont, char *out, size_t out_sz);

/**
 * @brief Resolve effective wake word with zero-config fallbacks:
 *        `$JARVIS_WAKE_WORD` -> `~/.jarvisrc` (`wake_word`) -> ontology -> `"jarbas"`/`"jarvis"` per lang.
 */
void jarvis_resolve_wake_word(const jarvis_ontology_t *ont, char *out, size_t out_sz);

/**
 * @brief Resolve effective TTS voice with zero-config fallbacks:
 *        `$JARVIS_VOICE` -> `~/.jarvisrc` (`tts_voice` unless `"auto"`) -> language-mapped `edge:` neural voice.
 */
void jarvis_resolve_tts_voice(const jarvis_ontology_t *ont, char *out, size_t out_sz);

/**
 * @brief Resolve active window manager / compositor with zero-config fallbacks:
 *        `$JARVIS_WM` -> `~/.jarvisrc` (`wm` unless `"auto"`) -> Wayland/X11/dwm environment detection.
 */
void jarvis_resolve_wm(char *out, size_t out_sz);

/* ---------------------------------------------------------------------------
 * Voice Pipeline: Ring Buffer, Adaptive VAD, Wake-Word, Earcon & TTS Segmentation
 * ------------------------------------------------------------------------- */

/**
 * @brief Create a portable 16 kHz float32 circular ring buffer & adaptive noise-floor VAD.
 */
jarvis_vad_t *jarvis_vad_create(double initial_noise_floor_db, double threshold_db);

/**
 * @brief Destroy a VAD instance.
 */
void jarvis_vad_destroy(jarvis_vad_t *vad);

/**
 * @brief Compute RMS energy in dBFS for a frame of float32 PCM samples.
 */
float jarvis_pcm_rms_db(const float *samples, size_t count);

/**
 * @brief Compute RMS energy in dBFS for a frame of int16_t PCM samples and convert to float32.
 */
float jarvis_pcm_s16_to_f32_db(const int16_t *s16, float *out_f32, size_t count);

/**
 * @brief Push a 20ms frame (320 float32 samples at 16 kHz) into the circular VAD buffer.
 *
 * Updates the adaptive noise floor and returns true if the frame contains active speech.
 */
bool jarvis_vad_push_frame(jarvis_vad_t *vad, const float *frame_samples, size_t count);

/**
 * @brief Extract a contiguous slice of float32 PCM samples from the circular ring buffer.
 */
size_t jarvis_vad_get_samples(
	const jarvis_vad_t *vad,
	int64_t             start_frame,
	int64_t             end_frame,
	float              *out_pcm,
	size_t              max_samples
);

/**
 * @brief Check if a transcribed text begins with or contains the wake word (e.g. "jarvis")
 *        and extract the trailing command text (if any).
 */
bool jarvis_voice_extract_wake_command(
	const char *transcript,
	const char *wake_word,
	char       *out_command,
	size_t      out_sz
);

/**
 * @brief Match wake word (and ontology polite prefixes / wake_variants) at prefix or anywhere.
 *
 * @return 0 = no match, 1 = wake word only, 2 = wake word + trailing command in @p out_cmd.
 */
int jarvis_voice_match_wake(
	const jarvis_ontology_t *ont,
	const char              *text,
	const char              *custom_wake_word,
	bool                     allow_anywhere,
	char                    *out_cmd,
	size_t                   out_sz
);

/**
 * @brief Check if a spoken command is a voice lockdown / disable phrase
 *        ("lockdown", "turn off jarvis", "go to sleep", "stop listening").
 */
bool jarvis_voice_is_lockdown_phrase_ex(
	const jarvis_ontology_t *ont,
	const char              *phrase,
	const char              *custom_wake_word
);
bool jarvis_voice_is_lockdown_phrase(const char *phrase);

/**
 * @brief Synthesize a 16 kHz 16-bit mono WAV earcon chime in pure C (zero external dependencies).
 *
 * @param wav_path  Destination file path.
 * @param is_done   false = rising wake chime (D5 -> A5), true = falling done chime (E5 -> C5).
 */
bool jarvis_voice_write_earcon_wav(const char *wav_path, bool is_done);

/**
 * @brief Clean markdown/URLs from text for natural speech synthesis.
 */
void jarvis_tts_clean_text(const char *raw_text, char *out_clean, size_t out_sz);

/**
 * @brief Split text into low-latency TTS segments for streaming synthesis.
 */
size_t jarvis_tts_split_segments(
	const char *text,
	char        out_segments[][256],
	size_t      max_segments
);

#ifdef __cplusplus
}
#endif

#endif /* TTYPT_JARVIS_H */
