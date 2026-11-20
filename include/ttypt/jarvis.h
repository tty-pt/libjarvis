#ifndef TTYPT_JARVIS_H
#define TTYPT_JARVIS_H

/**
 * @file jarvis.h
 * @brief Public API and libxylem (XY) hook declarations for libjarvis.
 *
 * Callers include <ttypt/jarvis.h>.
 * XY adapter modules that implement hooks include <ttypt/jarvis-types.h>
 * and <ttypt/xy-mod.h> (two-header rule).
 */

#include <ttypt/jarvis-types.h>
#include <ttypt/xy.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Xylem (XY) Hook Declarations — Environment Adapters & Voice Subsystem
 * ------------------------------------------------------------------------- */

/**
 * @brief Populate live environment entities into @p world.
 *
 * Multiple loaded XY modules (e.g. mod_dwm, mod_site, mod_nd, mod_posix)
 * can each implement this hook; libxylem runs all registered listeners so
 * every loaded domain contributes its live windows, tabs, apps, songs, or mobs.
 */
XY_DECL(int, jarvis_xy_observe, jarvis_world_t *, world);

/**
 * @brief Execute a compiled DSL operation @p dsl and populate @p result.
 *
 * Adapter modules inspect `dsl->op`. If the module handles the operation,
 * it sets `result->handled = true`, `result->success = ...`, and returns 0.
 */
XY_DECL(int, jarvis_xy_execute, const jarvis_dsl_t *, dsl, jarvis_adapter_result_t *, result);

/**
 * @brief Prompt the user to confirm a potentially destructive command.
 *
 * Returns 1 if confirmed, 0 if denied.
 */
XY_DECL(int, jarvis_xy_confirm, const char *, description);

/**
 * @brief Stream or speak a TTS clause / HUD notification.
 *
 * @param session_id  Streaming session identifier (or "" for one-shot).
 * @param clause_text Text clause to synthesize/speak.
 * @param is_final    1 if this is the final clause of the utterance, 0 otherwise.
 */
XY_DECL(int, jarvis_xy_speak_clause, const char *, session_id, const char *, clause_text, int, is_final);

/**
 * @brief Interrupt any active TTS playback immediately ("barge-in").
 */
XY_DECL(int, jarvis_xy_tts_interrupt, int, dummy);

/**
 * @brief Notify environment of a voice listener state transition (for status bar / OSD).
 */
XY_DECL(int, jarvis_xy_voice_status, int, state, const char *, partial_text);

/**
 * @brief Interactive omni-bar / command palette prompt hook (e.g. dmenu, web modal, CLI stdin).
 *
 * @param prompt_label     Label to show in the prompt UI (e.g. "Jarvis:").
 * @param candidates_lines Newline-delimited candidate strings.
 * @param out_selection    Output buffer to receive the user's selected/typed utterance.
 * @param out_sz           Capacity of @p out_selection.
 * @return 1 if an utterance was selected, 0 if cancelled or unhandled.
 */
XY_DECL(int, jarvis_xy_prompt, const char *, prompt_label, const char *, candidates_lines, char *, out_selection, size_t, out_sz);

/**
 * @brief Environment feedback notification & earcon hook (e.g. Dunst toast + audio earcon).
 *
 * @param title       Notification title.
 * @param body        Notification body text.
 * @param play_earcon 1 to play completion earcon chime, 0 otherwise.
 */
XY_DECL(int, jarvis_xy_notify, const char *, title, const char *, body, int, play_earcon);

/* ---------------------------------------------------------------------------
 * Core Ontology & Text Normalization API
 * ------------------------------------------------------------------------- */

typedef struct jarvis_ontology jarvis_ontology_t;
typedef struct jarvis_engine   jarvis_engine_t;

/**
 * @brief Clean phrase: lowercase alphanumeric + single-space normalized.
 *
 * Writes normalized string into @p out (up to @p out_sz bytes) and returns
 * length written.
 */
size_t jarvis_clean_phrase(const char *text, char *out, size_t out_sz);

/**
 * @brief Detect non-speech ASR hallucination (e.g. "vvvvvvvv", single-char stutter).
 */
bool jarvis_is_non_speech_hallucination(const char *text);

/**
 * @brief Detect whether a shell command matches destructive patterns
 *        (e.g. `rm -rf`, `git reset --hard`, `mkfs`, `dd if=`, `kill -9`).
 */
bool jarvis_is_destructive_command(const char *cmd);

/**
 * @brief Create and load a SemanticOntology from @p json_path.
 *
 * If @p json_path is NULL or empty, checks `$JARVIS_ONTOLOGY_JSON`,
 * `~/.config/jarvis/ontology.json`, and falls back to the built-in
 * embedded canonical ontology (works in WASM and zero-config environments).
 */
jarvis_ontology_t *jarvis_ontology_create(const char *json_path);

/**
 * @brief Load ontology directly from an in-memory JSON string.
 */
jarvis_ontology_t *jarvis_ontology_create_from_json(const char *json_str);

/**
 * @brief Destroy a SemanticOntology instance.
 */
void jarvis_ontology_destroy(jarvis_ontology_t *ont);

bool jarvis_ontology_is_tts_interrupt(const jarvis_ontology_t *ont, const char *cleaned_text);
const char *jarvis_ontology_get_wake_word(const jarvis_ontology_t *ont);
const char *jarvis_ontology_get_lang(const jarvis_ontology_t *ont);
const char *jarvis_ontology_get_lang_name(const jarvis_ontology_t *ont);
const char *jarvis_ontology_get_wake_prompt(const jarvis_ontology_t *ont);
const char *jarvis_ontology_get_cmd_prompt(const jarvis_ontology_t *ont);
const char *jarvis_ontology_get_ui_string(const jarvis_ontology_t *ont, const char *key, const char *fallback);
const char *jarvis_ontology_get_reply_template(const jarvis_ontology_t *ont, const char *key);

/**
 * @brief Vocabulary-driven ASR post-processor.
 *
 * Truncates trailing structural prefix-echoes, folds split compound tokens
 * ("low fee" -> "lofi"), and snaps phonetic leading verb typos.
 */
void jarvis_ontology_normalize_asr(
	const jarvis_ontology_t *ont,
	const jarvis_world_t    *world,
	const char              *raw_text,
	char                    *out,
	size_t                   out_sz
);

/* ---------------------------------------------------------------------------
 * Compositional Semantic Solver & IR -> DSL Compiler API
 * ------------------------------------------------------------------------- */

/**
 * @brief Deterministically solve a single clause into a platform-neutral Intent IR.
 *
 * @return true if a capability matched with confidence >= 0.80, false otherwise.
 */
bool jarvis_solve_clause(
	const jarvis_ontology_t *ont,
	const char              *raw_clause,
	const char              *active_surface,
	const jarvis_world_t    *world,
	jarvis_ir_t             *out_ir
);

/**
 * @brief Compile a platform-neutral Intent IR into an executable Action DSL struct.
 *
 * @return true if compilation succeeded, false if IR capability is unknown.
 */
bool jarvis_compile_ir_to_dsl(
	const jarvis_ir_t      *ir,
	const char             *active_surface,
	const jarvis_context_t *ctx,
	const jarvis_world_t   *world,
	jarvis_dsl_t           *out_dsl
);

bool jarvis_compile_ir_to_dsl_ex(
	const jarvis_ontology_t *ont,
	const jarvis_ir_t       *ir,
	const char              *active_surface,
	const jarvis_context_t  *ctx,
	const jarvis_world_t    *world,
	jarvis_dsl_t            *out_dsl
);

/* ---------------------------------------------------------------------------
 * DSL JSON Normalization, Healing & Streaming TTS Clause Extraction
 * ------------------------------------------------------------------------- */

/**
 * @brief Extract outermost JSON object/array payload from raw LLM text
 *        (strips markdown ```json fences and surrounding prose).
 */
bool jarvis_extract_json_payload(const char *raw_text, char *out_json, size_t out_sz);

/**
 * @brief Parse and heal an arbitrary LLM JSON object into canonical `jarvis_dsl_t`.
 *
 * Resolves operation aliases (`run_command` -> `spawn_shell_command`,
 * `goto_workspace` -> `workspace_switch`, etc.), lifts misplaced top-level
 * fields into `target` or `args`, and coerces workspace numbers.
 */
bool jarvis_normalize_dsl_json(const char *json_str, jarvis_dsl_t *out_dsl);

/**
 * @brief Serialize a `jarvis_dsl_t` to compact JSON.
 */
size_t jarvis_dsl_to_json(const jarvis_dsl_t *dsl, char *out_json, size_t out_sz);

/**
 * @brief Serialize a `jarvis_ir_t` to compact JSON.
 */
size_t jarvis_ir_to_json(const jarvis_ir_t *ir, char *out_json, size_t out_sz);

/**
 * @brief Serialize a `jarvis_process_result_t` to JSON.
 */
size_t jarvis_process_result_to_json(const jarvis_process_result_t *res, char *out_json, size_t out_sz);

/**
 * @brief Incremental streaming answer prefix extractor for low-latency TTS.
 *
 * While an LLM is still streaming a JSON `answer_question` response, extracts
 * the decoded `"answer": "..."` prefix seen so far.
 */
bool jarvis_extract_streaming_answer_prefix(
	const char *raw_json_fragment,
	char       *out_answer,
	size_t      out_sz
);

/**
 * @brief Pop the next ready spoken clause from @p unspoken buffer for immediate TTS.
 *
 * For the very first clause (@p is_first_clause == true), splits around 18-38 chars
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

/* ---------------------------------------------------------------------------
 * Orchestrator, Multi-Clause Chainer, Context, Memory & ReAct Agent Loop
 * ------------------------------------------------------------------------- */

typedef struct {
	const char         *wm;              /* Default "dwm" */
	const char         *ontology_path;   /* Optional path to ontology.json */
	const char         *few_shot_path;   /* Optional path to few_shots.json */
	const char         *context_path;    /* Optional path to context.json */
	const char         *memory_path;     /* Optional path to memories.json */
	const char         *session_path;    /* Optional path to session.json */
	const char         *embed_cache_path;/* Optional path to embeddings.json */
	const char         *auren_url;       /* Default "http://127.0.0.1:4242" */
	const char         *llm_model;       /* Default "kilo-auto/free" */
	jarvis_infer_mode_t infer_mode;      /* Default JARVIS_INFER_AUTO */
	bool                dry_run;
} jarvis_config_t;

/**
 * @brief Initialize a default configuration struct.
 */
void jarvis_config_init(jarvis_config_t *cfg);

/**
 * @brief Create a Jarvis orchestrator engine instance.
 */
jarvis_engine_t *jarvis_engine_create(const jarvis_config_t *cfg);

/**
 * @brief Destroy a Jarvis orchestrator engine instance.
 */
void jarvis_engine_destroy(jarvis_engine_t *eng);

/**
 * @brief Get the active SemanticOntology instance owned by @p eng.
 */
const jarvis_ontology_t *jarvis_engine_get_ontology(const jarvis_engine_t *eng);

/**
 * @brief Split a compound utterance into atomic clauses while protecting
 *        `"and press enter"`, `"screen and audio"`, and dictation spans.
 *
 * @return Number of clauses written into @p out_clauses (up to @p max_clauses).
 */
size_t jarvis_split_clauses(
	const jarvis_ontology_t *ont,
	const char              *utterance,
	char                     out_clauses[][512],
	size_t                   max_clauses
);

/**
 * @brief Resolve conversational pronouns ("it", "that", "there") against @p ctx.
 */
void jarvis_resolve_anaphora(
	const char             *clause,
	const jarvis_context_t *ctx,
	char                   *out_clause,
	size_t                  out_sz
);

/**
 * @brief Persist a user preference or custom workflow in permanent memory.
 */
bool jarvis_memory_remember(jarvis_engine_t *eng, const char *key, const char *value);

/**
 * @brief Recall user memories matching @p query (or all memories if empty) as JSON.
 */
size_t jarvis_memory_recall_json(jarvis_engine_t *eng, const char *query, char *out_json, size_t out_sz);

/**
 * @brief Process a natural language utterance end-to-end (Solver -> Embedding -> ReAct Agent -> Xylem Adapter).
 */
bool jarvis_process(
	jarvis_engine_t         *eng,
	const char              *utterance,
	const char              *active_surface,
	jarvis_process_result_t *out_result
);

/* ---------------------------------------------------------------------------
 * Voice Pipeline: Ring Buffer, Adaptive VAD, Wake-Word, Earcon & TTS Segmentation
 * ------------------------------------------------------------------------- */

typedef struct jarvis_vad jarvis_vad_t;

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
 * @brief Check if a transcribed string is a known Whisper non-speech hallucination.
 */
bool jarvis_is_non_speech_hallucination(const char *text);

/**
 * @brief Format a natural spoken reply from ontology `reply_templates` and @p res.
 */
void jarvis_format_voice_reply(
	const jarvis_ontology_t       *ont,
	const jarvis_process_result_t *res,
	char                          *out,
	size_t                         out_sz
);

/**
 * @brief Check if a spoken command is a voice lockdown / disable phrase
 *        ("lockdown", "turn off jarvis", "go to sleep", "stop listening").
 */
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
 * @brief Split text into low-latency TTS segments (segment 0 <= 36 chars for fast TTFA,
 *        subsequent segments <= 95 chars).
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
