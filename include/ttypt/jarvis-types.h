#ifndef TTYPT_JARVIS_TYPES_H
#define TTYPT_JARVIS_TYPES_H

/**
 * @file jarvis-types.h
 * @brief Shared types for libjarvis — Universal Compositional Semantic Solver,
 *        Cross-Platform Intent IR, Voice Pipeline, and Xylem Environment Adapters.
 *
 * Follows the libxylem two-header rule: implementers of XY hooks include
 * <ttypt/jarvis-types.h> and <ttypt/xy.h>; callers include <ttypt/jarvis.h>.
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

#define JARVIS_MAX_CLIENTS     32
#define JARVIS_MAX_TABS        16
#define JARVIS_MAX_APPS        64
#define JARVIS_MAX_PANES       8
#define JARVIS_MAX_STEPS       8
#define JARVIS_MAX_MEMORIES    64
#define JARVIS_MAX_TURNS       16

#define JARVIS_SAMPLE_RATE     16000
#define JARVIS_FRAME_MS        20
#define JARVIS_FRAME_SAMPLES   ((JARVIS_SAMPLE_RATE * JARVIS_FRAME_MS) / 1000)

/* Legacy aliases */
#define JARVIS_MAX_STR         JARVIS_STR_LEN
#define JARVIS_MAX_TEXT        JARVIS_TEXT_LEN

/**
 * @brief Active user surface context.
 */
typedef enum {
	JARVIS_SURFACE_ANY      = 0,
	JARVIS_SURFACE_TERMINAL = 1,
	JARVIS_SURFACE_BROWSER  = 2,
	JARVIS_SURFACE_SITE     = 3,
	JARVIS_SURFACE_ND       = 4
} jarvis_surface_t;

/**
 * @brief Inference mode policy.
 */
typedef enum {
	JARVIS_INFER_AUTO      = 0,
	JARVIS_INFER_LLM       = 1,
	JARVIS_INFER_EMBEDDING = 2,
	JARVIS_INFER_NONE      = 3
} jarvis_infer_mode_t;

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
 * @brief Live window / client descriptor observed from environment.
 */
typedef struct {
	char id[JARVIS_ID_LEN];
	char wm_class[JARVIS_NAME_LEN];
	char title[JARVIS_STR_LEN];
	int  tag;
	bool visible;
	bool focused;
} jarvis_window_t;

/**
 * @brief Live browser tab descriptor.
 */
typedef struct {
	int  index;
	char title[JARVIS_STR_LEN];
	char url[JARVIS_CLAUSE_LEN];
} jarvis_tab_t;

/**
 * @brief Live terminal / tmux pane descriptor.
 */
typedef struct {
	char pane[JARVIS_ID_LEN];
	char command[JARVIS_NAME_LEN];
	char cwd[JARVIS_STR_LEN];
	bool active;
	char tail[JARVIS_TEXT_LEN];
} jarvis_pane_t;

/**
 * @brief Custom domain entity contributed by an XY module (e.g. desktop app,
 *        music site song/poem/gig, or NeverDark mob/item/exit).
 */
typedef struct {
	char type[JARVIS_ID_LEN];
	char id[JARVIS_STR_LEN];
	char phrase[JARVIS_STR_LEN];
} jarvis_live_entity_t;

/**
 * @brief Conversational context across consecutive utterances.
 */
typedef struct {
	double last_ts;
	char   last_op[JARVIS_ID_LEN];
	char   last_command[JARVIS_CLAUSE_LEN];
	char   last_app[JARVIS_NAME_LEN];
	char   last_query[JARVIS_STR_LEN];
	char   last_location[JARVIS_STR_LEN];
	char   last_window_id[JARVIS_ID_LEN];
	char   last_window_class[JARVIS_NAME_LEN];
	int    last_workspace;
	int    prev_workspace;
	char   last_url[JARVIS_CLAUSE_LEN];
	char   last_file[JARVIS_STR_LEN];
	int    last_line;
	bool   conv_active;
	double conv_last_ts;
} jarvis_context_t;

/**
 * @brief Live world observation snapshot populated by XY adapter modules.
 */
typedef struct {
	char                 wm[JARVIS_SHORT_LEN];
	jarvis_window_t      focused;
	jarvis_window_t      other;
	char                 focused_cwd[JARVIS_STR_LEN];
	char                 focused_process[JARVIS_NAME_LEN];
	char                 system_health[JARVIS_STR_LEN];
	char                 browser_page[JARVIS_OUTPUT_LEN];

	jarvis_window_t      clients[JARVIS_MAX_CLIENTS];
	size_t               client_count;

	jarvis_tab_t         tabs[JARVIS_MAX_TABS];
	size_t               tab_count;

	jarvis_pane_t        panes[JARVIS_MAX_PANES];
	size_t               pane_count;

	jarvis_live_entity_t custom_entities[JARVIS_MAX_APPS];
	size_t               custom_entity_count;

	bool                 audio_playing;
	jarvis_context_t     context;
} jarvis_world_t;

/**
 * @brief Platform-neutral Intent Intermediate Representation (Intent IR).
 */
typedef struct {
	char   cap[JARVIS_ID_LEN];
	double score;
	char   surface[JARVIS_SHORT_LEN];
	bool   explicit_terminal;
	bool   target_other;
	bool   new_terminal;
	bool   submit_enter;
	bool   background;

	/* Canonical IR arguments */
	char   location[JARVIS_STR_LEN];
	char   collection[JARVIS_ID_LEN];
	char   metric[JARVIS_ID_LEN];
	char   pattern[JARVIS_NAME_LEN];
	char   aspect[JARVIS_ID_LEN];
	char   program[JARVIS_STR_LEN];
	char   unit[JARVIS_ID_LEN];
	char   kind[JARVIS_ID_LEN];
	char   identifier[JARVIS_STR_LEN];
	char   relation[JARVIS_SHORT_LEN];
	char   action[JARVIS_ID_LEN];
	char   direction[JARVIS_SHORT_LEN];
	char   text[JARVIS_CLAUSE_LEN];
	char   query[JARVIS_CLAUSE_LEN];
	char   url[JARVIS_CLAUSE_LEN];
	char   application[JARVIS_STR_LEN];
	char   record_target[JARVIS_SHORT_LEN];
	int    workspace;
	int    amount;
	bool   hidden;
} jarvis_ir_t;

/**
 * @brief Executable Action DSL operation emitted by Orchestrator or Inference.
 */
typedef struct {
	char op[JARVIS_ID_LEN];

	/* Target descriptor */
	struct {
		char kind[JARVIS_SHORT_LEN];
		char relation[JARVIS_SHORT_LEN];
		char identifier[JARVIS_STR_LEN];
		char title_contains[JARVIS_NAME_LEN];
		char class_contains[JARVIS_NAME_LEN];
		char url[JARVIS_CLAUSE_LEN];
		char command[JARVIS_CLAUSE_LEN];
		char query[JARVIS_CLAUSE_LEN];
		int  x;
		int  y;
		bool new_terminal;
	} target;

	/* Operation arguments */
	struct {
		char   text[JARVIS_CLAUSE_LEN];
		char   key_combo[JARVIS_NAME_LEN];
		int    button;
		char   direction[JARVIS_SHORT_LEN];
		int    amount;
		int    workspace;
		char   action[JARVIS_ID_LEN];
		char   mode[JARVIS_SHORT_LEN];
		double seconds;
		char   answer[JARVIS_TEXT_LEN];
		char   aspect[JARVIS_ID_LEN];
		char   key[JARVIS_NAME_LEN];
		char   value[JARVIS_CLAUSE_LEN];
		char   reply[JARVIS_CLAUSE_LEN];
		bool   speak;
		bool   background;
		bool   streamed_tts;
		bool   expect_reply;
	} args;

	/* Verification expectations */
	struct {
		bool has_audio_playing;
		bool audio_playing;
		bool has_window_focused;
		bool window_focused;
		char title_contains[JARVIS_NAME_LEN];
	} verify;

	/* Provenance metadata */
	struct {
		char   source_ir_cap[JARVIS_ID_LEN];
		double source_ir_score;
		char   inference_backend[JARVIS_ID_LEN];
		int    latency_ms;
	} meta;
} jarvis_dsl_t;

/**
 * @brief Result of executing a single DSL operation via an adapter.
 */
typedef struct {
	bool handled;
	bool success;
	char output[JARVIS_OUTPUT_LEN];
	char error[JARVIS_CLAUSE_LEN];
} jarvis_adapter_result_t;

/**
 * @brief Single step trace in a multi-clause or multi-turn execution.
 */
typedef struct {
	char                    clause[JARVIS_CLAUSE_LEN];
	char                    resolved_clause[JARVIS_CLAUSE_LEN];
	bool                    has_ir;
	jarvis_ir_t             ir;
	bool                    has_dsl;
	jarvis_dsl_t            dsl;
	jarvis_adapter_result_t result;
	char                    source[JARVIS_ID_LEN];
	double                  observe_ms;
	double                  solver_ms;
	double                  execute_ms;
	double                  total_ms;
} jarvis_step_t;

/**
 * @brief Full processing result for an utterance (single or compound).
 */
typedef struct {
	bool                    ok;
	char                    utterance[JARVIS_CLAUSE_LEN];
	char                    error[JARVIS_CLAUSE_LEN];
	jarvis_step_t           steps[JARVIS_MAX_STEPS];
	size_t                  step_count;
	jarvis_dsl_t            last_dsl;
	jarvis_adapter_result_t combined_result;
	char                    source[JARVIS_NAME_LEN];
} jarvis_process_result_t;

/**
 * @brief Streaming TTS clause callback invoked as LLM tokens arrive.
 */
typedef void (*jarvis_tts_clause_cb_t)(const char *clause_text, bool is_final, void *userdata);

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
