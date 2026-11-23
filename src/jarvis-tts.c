/**
 * @file jarvis-tts.c
 * @brief In-process Neural TTS subsystem (Kokoro-82M + Piper ONNX) for jarvisd.
 *
 * Features:
 * - Zero RAM at startup and when silent (0 KB allocated until speech is requested with tts_enabled=1)
 * - Lazy dlopen() of system libonnxruntime.so.1.23 and libespeak-ng.so.1
 * - Selective single-voice extraction (510 KB) from voices-v1.0.bin (instead of 28 MB all-voices load)
 * - DisableCpuMemArena + automatic 60-second idle unload + malloc_trim(0) back to 0 KB RAM
 * - Pipelined raw PCM streaming into pw-cat with instant <1ms SIGKILL barge-in preemption
 */

#define _GNU_SOURCE
#include "jarvis-tts.h"
#include "jarvis-audio.h"
#include "jarvis-platform.h"

#include <ttypt/stele.h>
#include "jarvis-thread.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef __GLIBC__
#include <malloc.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif
#else
#include <strings.h>
#include <unistd.h>
#endif

static int tts_fd_close(int fd)
{
#ifdef _WIN32
	return _close(fd);
#else
	return close(fd);
#endif
}

static ssize_t tts_fd_read(int fd, void *buf, size_t n)
{
#ifdef _WIN32
	return (ssize_t)_read(fd, buf, (unsigned int)n);
#else
	return read(fd, buf, n);
#endif
}

static ssize_t tts_fd_write(int fd, const void *buf, size_t n)
{
#ifdef _WIN32
	return (ssize_t)_write(fd, buf, (unsigned int)n);
#else
	return write(fd, buf, n);
#endif
}

/* ---------------------------------------------------------------------------
 * Minimal ABI Definitions for libespeak-ng.so.1 & libonnxruntime.so.1.23
 * ------------------------------------------------------------------------- */

typedef int (*espeak_Init_fn)(int output, int buflength, const char *path, int options);
typedef int (*espeak_SetVoice_fn)(const char *name);
typedef const char *(*espeak_TextToPhonemesTerm_fn)(const void **textptr, int textmode, int phonememode, int *terminator);
typedef int (*espeak_Terminate_fn)(void);

typedef struct {
	const void *(*GetApi)(uint32_t version);
	const char *(*GetVersionString)(void);
} OrtApiBase_t;

typedef const OrtApiBase_t *(*OrtGetApiBase_fn)(void);

typedef void *(*ort_CreateEnv_fn)(int log_level, const char *logid, void **out);
typedef void *(*ort_CreateSessionOptions_fn)(void **out);
typedef void *(*ort_DisableCpuMemArena_fn)(void *options);
typedef void *(*ort_SetGraphOptLevel_fn)(void *options, int level);
typedef void *(*ort_SetIntraThreads_fn)(void *options, int num_threads);
typedef void *(*ort_SetInterThreads_fn)(void *options, int num_threads);
typedef void *(*ort_CreateSession_fn)(const void *env, const char *model_path, const void *options, void **out);
typedef void *(*ort_Run_fn)(void *sess, const void *run_options,
                            const char *const *input_names, const void *const *inputs, size_t input_len,
                            const char *const *output_names, size_t output_names_len, void **outputs);
typedef void *(*ort_CreateCpuMemoryInfo_fn)(int alloc_type, int mem_type, void **out);
typedef void *(*ort_CreateTensorWithData_fn)(const void *info, void *p_data, size_t p_data_len,
                                             const int64_t *shape, size_t shape_len, int type, void **out);
typedef void *(*ort_GetTensorMutableData_fn)(void *value, void **out);
typedef void *(*ort_GetTensorTypeAndShape_fn)(const void *value, void **out);
typedef void *(*ort_GetTensorShapeElementCount_fn)(const void *info, size_t *out);
typedef void (*ort_Release_fn)(void *ptr);

/* Stable ORT 1.x C API vtable indices (verified against libonnxruntime.so.1.23) */
#define ORT_API_VERSION                    17
#define ORT_IDX_CreateEnv                   3
#define ORT_IDX_CreateSession               7
#define ORT_IDX_Run                         9
#define ORT_IDX_CreateSessionOptions       10
#define ORT_IDX_DisableCpuMemArena         19
#define ORT_IDX_SetGraphOptimizationLevel  23
#define ORT_IDX_SetIntraOpNumThreads       24
#define ORT_IDX_SetInterOpNumThreads       25
#define ORT_IDX_CreateTensorWithData       49
#define ORT_IDX_GetTensorMutableData       51
#define ORT_IDX_GetTensorShapeElementCount 64
#define ORT_IDX_GetTensorTypeAndShape      65
#define ORT_IDX_CreateCpuMemoryInfo        69
#define ORT_IDX_ReleaseEnv                 92
#define ORT_IDX_ReleaseStatus              93
#define ORT_IDX_ReleaseMemoryInfo          94
#define ORT_IDX_ReleaseSession             95
#define ORT_IDX_ReleaseValue               96
#define ORT_IDX_ReleaseTensorTypeAndShape  99
#define ORT_IDX_ReleaseSessionOptions     100

#define ONNX_TENSOR_FLOAT 1
#define ONNX_TENSOR_INT64 7

#define KOKORO_STYLE_ROWS 510
#define KOKORO_STYLE_DIM  256
#define KOKORO_SAMPLE_RATE 24000
#define PIPER_SAMPLE_RATE  22050
#define IDLE_UNLOAD_SECONDS 600

/* ---------------------------------------------------------------------------
 * Kokoro-82M & Piper Phoneme Vocabulary Lookup Tables
 * ------------------------------------------------------------------------- */

typedef struct {
	uint32_t cp;
	int16_t  id;
} cp_id_t;

/* Kokoro-82M IPA codepoint -> token ID table (from kokoro config.json) */
static const cp_id_t KOKORO_VOCAB[] = {
	{ ';', 1 }, { ':', 2 }, { ',', 3 }, { '.', 4 }, { '!', 5 }, { '?', 6 },
	{ 0x2014, 9 }, { 0x2026, 10 }, { '"', 11 }, { '(', 12 }, { ')', 13 },
	{ 0x201C, 14 }, { 0x201D, 15 }, { ' ', 16 }, { 0x0303, 17 },
	{ 0x02A3, 18 }, { 0x02A5, 19 }, { 0x02A6, 20 }, { 0x02A8, 21 },
	{ 0x1D5D, 22 }, { 0xAB67, 23 }, { 'A', 24 }, { 'I', 25 }, { 'O', 31 },
	{ 'Q', 33 }, { 'S', 35 }, { 'T', 36 }, { 'W', 39 }, { 'Y', 41 },
	{ 0x1D4A, 42 }, { 'a', 43 }, { 'b', 44 }, { 'c', 45 }, { 'd', 46 },
	{ 'e', 47 }, { 'f', 48 }, { 'h', 50 }, { 'i', 51 }, { 'j', 52 },
	{ 'k', 53 }, { 'l', 54 }, { 'm', 55 }, { 'n', 56 }, { 'o', 57 },
	{ 'p', 58 }, { 'q', 59 }, { 'r', 60 }, { 's', 61 }, { 't', 62 },
	{ 'u', 63 }, { 'v', 64 }, { 'w', 65 }, { 'x', 66 }, { 'y', 67 },
	{ 'z', 68 }, { 0x0251, 69 }, { 0x0250, 70 }, { 0x0252, 71 },
	{ 0x00E6, 72 }, { 0x03B2, 75 }, { 0x0254, 76 }, { 0x0255, 77 },
	{ 0x00E7, 78 }, { 0x0256, 80 }, { 0x00F0, 81 }, { 0x02A4, 82 },
	{ 0x0259, 83 }, { 0x025A, 85 }, { 0x025B, 86 }, { 0x025C, 87 },
	{ 0x025F, 90 }, { 0x0261, 92 }, { 'g', 92 }, { 0x0265, 99 },
	{ 0x0268, 101 }, { 0x026A, 102 }, { 0x029D, 103 }, { 0x026F, 110 },
	{ 0x0270, 111 }, { 0x014B, 112 }, { 0x0273, 113 }, { 0x0272, 114 },
	{ 0x0274, 115 }, { 0x00F8, 116 }, { 0x0278, 118 }, { 0x03B8, 119 },
	{ 0x0153, 120 }, { 0x0279, 123 }, { 0x027E, 125 }, { 0x027B, 126 },
	{ 0x0281, 128 }, { 0x027D, 129 }, { 0x0282, 130 }, { 0x0283, 131 },
	{ 0x0288, 132 }, { 0x02A7, 133 }, { 0x028A, 135 }, { 0x028B, 136 },
	{ 0x028C, 138 }, { 0x0263, 139 }, { 0x0264, 140 }, { 0x03C7, 142 },
	{ 0x028E, 143 }, { 0x0292, 147 }, { 0x0294, 148 }, { 0x02C8, 156 },
	{ 0x02CC, 157 }, { 0x02D0, 158 }, { 0x02B0, 162 }, { 0x02B2, 164 },
	{ 0x2193, 169 }, { 0x2192, 171 }, { 0x2197, 172 }, { 0x2198, 173 },
	{ 0x1D7B, 177 }, { 0, -1 }
};

/* Piper IPA codepoint -> token ID table */
static const cp_id_t PIPER_VOCAB[] = {
	{ '_', 0 }, { '^', 1 }, { '$', 2 }, { ' ', 3 }, { '!', 4 }, { '\'', 5 },
	{ '(', 6 }, { ')', 7 }, { ',', 8 }, { '-', 9 }, { '.', 10 }, { ':', 11 },
	{ ';', 12 }, { '?', 13 }, { 'a', 14 }, { 'b', 15 }, { 'c', 16 }, { 'd', 17 },
	{ 'e', 18 }, { 'f', 19 }, { 'h', 20 }, { 'i', 21 }, { 'j', 22 }, { 'k', 23 },
	{ 'l', 24 }, { 'm', 25 }, { 'n', 26 }, { 'o', 27 }, { 'p', 28 }, { 'q', 29 },
	{ 'r', 30 }, { 's', 31 }, { 't', 32 }, { 'u', 33 }, { 'v', 34 }, { 'w', 35 },
	{ 'x', 36 }, { 'y', 37 }, { 'z', 38 }, { 0x00E6, 39 }, { 0x00E7, 40 },
	{ 0x00F0, 41 }, { 0x00F8, 42 }, { 0x0127, 43 }, { 0x014B, 44 },
	{ 0x0153, 45 }, { 0x01C0, 46 }, { 0x01C1, 47 }, { 0x01C2, 48 },
	{ 0x01C3, 49 }, { 0x0250, 50 }, { 0x0251, 51 }, { 0x0252, 52 },
	{ 0x0253, 53 }, { 0x0254, 54 }, { 0x0255, 55 }, { 0x0256, 56 },
	{ 0x0257, 57 }, { 0x0258, 58 }, { 0x0259, 59 }, { 0x025A, 60 },
	{ 0x025B, 61 }, { 0x025C, 62 }, { 0x025E, 63 }, { 0x025F, 64 },
	{ 0x0260, 65 }, { 0x0261, 66 }, { 'g', 66 }, { 0x0262, 67 },
	{ 0x0263, 68 }, { 0x0264, 69 }, { 0x0265, 70 }, { 0x0266, 71 },
	{ 0x0267, 72 }, { 0x0268, 73 }, { 0x026A, 74 }, { 0x026B, 75 },
	{ 0x026C, 76 }, { 0x026D, 77 }, { 0x026E, 78 }, { 0x026F, 79 },
	{ 0x0270, 80 }, { 0x0271, 81 }, { 0x0272, 82 }, { 0x0273, 83 },
	{ 0x0274, 84 }, { 0x0275, 85 }, { 0x0276, 86 }, { 0x0278, 87 },
	{ 0x0279, 88 }, { 0x027A, 89 }, { 0x027B, 90 }, { 0x027D, 91 },
	{ 0x027E, 92 }, { 0x0280, 93 }, { 0x0281, 94 }, { 0x0282, 95 },
	{ 0x0283, 96 }, { 0x0284, 97 }, { 0x0288, 98 }, { 0x0289, 99 },
	{ 0x028A, 100 }, { 0x028B, 101 }, { 0x028C, 102 }, { 0x028D, 103 },
	{ 0x028E, 104 }, { 0x028F, 105 }, { 0x0290, 106 }, { 0x0291, 107 },
	{ 0x0292, 108 }, { 0x0294, 109 }, { 0x0295, 110 }, { 0x0298, 111 },
	{ 0x0299, 112 }, { 0x029B, 113 }, { 0x029C, 114 }, { 0x029D, 115 },
	{ 0x029F, 116 }, { 0x02A1, 117 }, { 0x02A2, 118 }, { 0x02B2, 119 },
	{ 0x02C8, 120 }, { 0x02CC, 121 }, { 0x02D0, 122 }, { 0x02D1, 123 },
	{ 0x02DE, 124 }, { 0x03B2, 125 }, { 0x03B8, 126 }, { 0x03C7, 127 },
	{ 0x1D7B, 128 }, { 0x2C71, 129 }, { '0', 130 }, { '1', 131 },
	{ '2', 132 }, { '3', 133 }, { '4', 134 }, { '5', 135 }, { '6', 136 },
	{ '7', 137 }, { '8', 138 }, { '9', 139 }, { 0x0327, 140 }, { 0x0303, 141 },
	{ 0x032A, 142 }, { 0x032F, 143 }, { 0x0329, 144 }, { 0x02B0, 145 },
	{ 0x02E4, 146 }, { 0x03B5, 147 }, { 0x2193, 148 }, { '#', 149 },
	{ '"', 150 }, { 0x2191, 151 }, { 0x033A, 152 }, { 0x033B, 153 },
	{ 0, -1 }
};

/* ---------------------------------------------------------------------------
 * Lazy Engine State & Streaming Queue
 * ------------------------------------------------------------------------- */

#define MAX_TTS_QUEUE 32
#define TTS_CACHE_SLOTS 32
#define TTS_CACHE_MAX_TEXT 96
#define TTS_CACHE_MAX_BYTES (24000 * 2 * 4)
#define EDGE_TTS_TIMEOUT_MS 8000

typedef struct {
	char text[512];
	char voice[64];
} tts_segment_t;

typedef struct {
	char     voice[64];
	char     text[TTS_CACHE_MAX_TEXT];
	int16_t *pcm;
	size_t   n_samples;
	int      sample_rate;
	uint64_t last_used;
} tts_cache_entry_t;

static struct {
	pthread_mutex_t mtx;
	pthread_cond_t  cond;
	pthread_cond_t  done_cond;

	/* Queue */
	tts_segment_t   queue[MAX_TTS_QUEUE];
	int             q_head;
	int             q_tail;
	int             q_count;
	bool            worker_running;
	bool            busy_speaking;
	uint64_t        cancel_gen;
	jarvis_audio_play_t *player;
	jarvis_audio_play_t *draining_player;

	/* Persistent edge-tts-pcm --serve coprocess */
	stl_proc_t      edge_proc;
	bool            has_edge;
	bool            edge_busy;

	/* Short-phrase PCM LRU cache */
	tts_cache_entry_t cache[TTS_CACHE_SLOTS];
	uint64_t          cache_tick;

	/* Lazy dlopen handles & symbols (NULL when unloaded) */
	void           *espeak_dl;
	espeak_Init_fn               espeak_Init;
	espeak_SetVoice_fn           espeak_SetVoice;
	espeak_TextToPhonemesTerm_fn espeak_TextToPhonemesTerm;
	espeak_Terminate_fn          espeak_Terminate;

	void           *ort_dl;
	const void    **ort_vtbl;
	void           *ort_env;
	void           *ort_mem_info;
	void           *ort_sess;

	bool            is_piper;
	bool            is_kokoro_pt;
	char            loaded_model[512];
	char            loaded_voice[64];
	float          *kokoro_style; /* 510 * 256 floats = 510 KB, NULL when unloaded */
} g_tts = {
	.mtx = PTHREAD_MUTEX_INITIALIZER,
	.cond = PTHREAD_COND_INITIALIZER,
	.done_cond = PTHREAD_COND_INITIALIZER,
	.cancel_gen = 1,
	.player = NULL,
	.draining_player = NULL,
	.edge_proc = { .stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1 },
	.has_edge = false
};

/* ---------------------------------------------------------------------------
 * Config & Helpers
 * ------------------------------------------------------------------------- */

static void trim_trailing(char *s)
{
	if (!s) return;
	size_t l = strlen(s);
	while (l > 0 && isspace((unsigned char)s[l - 1])) s[--l] = '\0';
}

static const char *tts_home_dir(char *buf, size_t cap)
{
	const char *home = getenv("HOME");
	if (home && *home) return home;
	stl_home_dir(buf, cap);
	return buf[0] ? buf : NULL;
}

static bool rc_lookup(const char *key, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return false;
	out[0] = '\0';
	const char *env_rc = getenv("JARVISRC");
	char hbuf[512] = {0};
	const char *home = tts_home_dir(hbuf, sizeof(hbuf));
	char rc_path[1024];
	if (env_rc && *env_rc) {
		snprintf(rc_path, sizeof(rc_path), "%s", env_rc);
	} else if (home && *home) {
		snprintf(rc_path, sizeof(rc_path), "%s/.jarvisrc", home);
		if (!jarvis_plat_is_readable(rc_path)) {
			snprintf(rc_path, sizeof(rc_path), "%s/.config/jarvis/jarvisrc", home);
			if (!jarvis_plat_is_readable(rc_path)) {
				char cdir[512];
				if (stl_config_dir("jarvis", cdir, sizeof(cdir)) && cdir[0]) {
					snprintf(rc_path, sizeof(rc_path), "%s/jarvisrc", cdir);
				}
			}
		}
	} else {
		return false;
	}
	FILE *fp = fopen(rc_path, "r");
	if (!fp) return false;
	bool found = false;
	char line[512];
	while (fgets(line, sizeof(line), fp)) {
		char *p = line;
		while (*p && isspace((unsigned char)*p)) p++;
		if (!*p || *p == '#' || *p == ';') continue;
		char *eq = strchr(p, '=');
		if (!eq) continue;
		*eq = '\0';
		size_t kl = strlen(p);
		while (kl > 0 && isspace((unsigned char)p[kl - 1])) p[--kl] = '\0';
		if (strcasecmp(p, key) != 0) continue;
		char *v = eq + 1;
		while (*v && isspace((unsigned char)*v)) v++;
		trim_trailing(v);
		size_t vl = strlen(v);
		if (vl >= 2 && ((v[0] == '"' && v[vl - 1] == '"') || (v[0] == '\'' && v[vl - 1] == '\''))) {
			v[vl - 1] = '\0';
			v++;
		}
		if (v[0]) {
			snprintf(out, out_sz, "%s", v);
			found = true;
		}
	}
	fclose(fp);
	return found;
}

static bool is_tts_enabled(void)
{
	const char *no_tts = getenv("JARVIS_NO_TTS");
	if (no_tts && strcmp(no_tts, "1") == 0) return false;
	const char *env_tts = getenv("JARVIS_TTS");
	if (env_tts) return strcmp(env_tts, "0") != 0;

	char buf[32];
	if (rc_lookup("tts_enabled", buf, sizeof(buf))) {
		return (strcmp(buf, "1") == 0 || strcasecmp(buf, "true") == 0 ||
		        strcasecmp(buf, "on") == 0 || strcasecmp(buf, "yes") == 0);
	}
	return true;
}

static bool normalize_lang_tts(const char *raw, char *out, size_t out_sz)
{
	if (!raw || !*raw || !out || out_sz == 0) return false;
	char buf[32];
	size_t w = 0;
	while (*raw && isspace((unsigned char)*raw)) raw++;
	for (size_t i = 0; raw[i] && w + 1 < sizeof(buf); i++) {
		char c = raw[i];
		if (c == '.' || c == '@') break;
		if (c == '_') c = '-';
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		buf[w++] = c;
	}
	while (w > 0 && isspace((unsigned char)buf[w - 1])) w--;
	buf[w] = '\0';
	if (!buf[0] || strcmp(buf, "c") == 0 || strcmp(buf, "posix") == 0)
		return false;
	snprintf(out, out_sz, "%s", buf);
	return true;
}

static void resolve_default_lang(char *out, size_t out_sz)
{
	char tag[32] = {0};
	const char *env_tl = getenv("JARVIS_TTS_LANG");
	if (env_tl && normalize_lang_tts(env_tl, tag, sizeof(tag)) &&
	    strcmp(tag, "auto") != 0 && strcmp(tag, "kbd") != 0) {
		snprintf(out, out_sz, "%s", tag);
		return;
	}
	char rc_val[32] = {0};
	if (rc_lookup("tts_lang", rc_val, sizeof(rc_val)) &&
	    normalize_lang_tts(rc_val, tag, sizeof(tag)) &&
	    strcmp(tag, "auto") != 0 && strcmp(tag, "kbd") != 0) {
		snprintf(out, out_sz, "%s", tag);
		return;
	}
	const char *env_l = getenv("JARVIS_LANG");
	if (env_l && normalize_lang_tts(env_l, tag, sizeof(tag)) &&
	    strcmp(tag, "auto") != 0 && strcmp(tag, "kbd") != 0) {
		snprintf(out, out_sz, "%s", tag);
		return;
	}
	if (rc_lookup("lang", rc_val, sizeof(rc_val)) &&
	    normalize_lang_tts(rc_val, tag, sizeof(tag)) &&
	    strcmp(tag, "auto") != 0 && strcmp(tag, "kbd") != 0) {
		snprintf(out, out_sz, "%s", tag);
		return;
	}
	static const char *const lc_vars[] = { "LC_ALL", "LC_MESSAGES", "LANG", NULL };
	for (size_t i = 0; lc_vars[i]; i++) {
		const char *lv = getenv(lc_vars[i]);
		if (lv && normalize_lang_tts(lv, tag, sizeof(tag))) {
			snprintf(out, out_sz, "%s", tag);
			return;
		}
	}
	snprintf(out, out_sz, "en");
}

static void resolve_local_voice(const char *def_lang, char *out_voice, size_t out_sz)
{
	char hbuf[512] = {0};
	const char *home = tts_home_dir(hbuf, sizeof(hbuf));
	bool is_pt = (def_lang && strncmp(def_lang, "pt", 2) == 0);
	if (is_pt) {
		if (home && *home) {
			char p[512];
			snprintf(p, sizeof(p), "%s/.local/share/piper/pt_PT-tugao-medium.onnx", home);
			if (jarvis_plat_is_readable(p)) {
				snprintf(out_voice, out_sz, "piper:pt_PT-tugao-medium");
				return;
			}
			snprintf(p, sizeof(p), "%s/.local/share/piper/pt_PT-miro.onnx", home);
			if (jarvis_plat_is_readable(p)) {
				snprintf(out_voice, out_sz, "piper:pt_PT-miro");
				return;
			}
		}
		char dp[512];
		if (jarvis_plat_data_file("piper", "pt_PT-tugao-medium.onnx", dp, sizeof(dp)) == 0 && jarvis_plat_is_readable(dp)) {
			snprintf(out_voice, out_sz, "piper:pt_PT-tugao-medium");
			return;
		}
		if (jarvis_plat_data_file("piper", "pt_PT-miro.onnx", dp, sizeof(dp)) == 0 && jarvis_plat_is_readable(dp)) {
			snprintf(out_voice, out_sz, "piper:pt_PT-miro");
			return;
		}
		snprintf(out_voice, out_sz, "pm_tuga");
		return;
	}
	if (home && *home) {
		char p[512];
		if (def_lang && strcmp(def_lang, "en-us") == 0) {
			snprintf(p, sizeof(p), "%s/.local/share/piper/en_US-lessac-high.onnx", home);
			if (jarvis_plat_is_readable(p)) {
				snprintf(out_voice, out_sz, "piper:us");
				return;
			}
		}
		snprintf(p, sizeof(p), "%s/.local/share/piper/en_GB-northern_english_male-medium.onnx", home);
		if (jarvis_plat_is_readable(p)) {
			snprintf(out_voice, out_sz, "piper:gb");
			return;
		}
	}
	{
		char dp[512];
		if (def_lang && strcmp(def_lang, "en-us") == 0 &&
		    jarvis_plat_data_file("piper", "en_US-lessac-high.onnx", dp, sizeof(dp)) == 0 && jarvis_plat_is_readable(dp)) {
			snprintf(out_voice, out_sz, "piper:us");
			return;
		}
		if (jarvis_plat_data_file("piper", "en_GB-northern_english_male-medium.onnx", dp, sizeof(dp)) == 0 && jarvis_plat_is_readable(dp)) {
			snprintf(out_voice, out_sz, "piper:gb");
			return;
		}
	}
	snprintf(out_voice, out_sz, "bm_daniel");
}

static void get_tts_voice(char *out, size_t out_sz)
{
	char def_lang[32];
	resolve_default_lang(def_lang, sizeof(def_lang));

	const char *req = getenv("JARVIS_VOICE");
	char rc_v[64] = {0};
	if ((!req || !*req) && rc_lookup("tts_voice", rc_v, sizeof(rc_v)) && rc_v[0])
		req = rc_v;

	if (req && *req && strcasecmp(req, "auto") != 0) {
		if (strcasecmp(req, "local") == 0) {
			resolve_local_voice(def_lang, out, out_sz);
			return;
		}
		snprintf(out, out_sz, "%s", req);
		return;
	}

	if (strcmp(def_lang, "pt-br") == 0) {
		snprintf(out, out_sz, "edge:pt-BR-FranciscaNeural");
	} else if (strncmp(def_lang, "pt", 2) == 0) {
		snprintf(out, out_sz, "edge:pt-PT-RaquelNeural");
	} else if (strcmp(def_lang, "en-us") == 0) {
		snprintf(out, out_sz, "edge:en-US-AndrewNeural");
	} else if (strncmp(def_lang, "en", 2) == 0) {
		snprintf(out, out_sz, "edge:en-GB-RyanNeural");
	} else if (strncmp(def_lang, "es", 2) == 0) {
		snprintf(out, out_sz, "edge:es-ES-AlvaroNeural");
	} else if (strncmp(def_lang, "fr", 2) == 0) {
		snprintf(out, out_sz, "edge:fr-FR-HenriNeural");
	} else if (strncmp(def_lang, "de", 2) == 0) {
		snprintf(out, out_sz, "edge:de-DE-ConradNeural");
	} else if (strncmp(def_lang, "it", 2) == 0) {
		snprintf(out, out_sz, "edge:it-IT-DiegoNeural");
	} else {
		snprintf(out, out_sz, "edge:en-GB-RyanNeural");
	}
}

void jarvis_tts_resolve_voice(char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	get_tts_voice(out, out_sz);
}

static void set_mic_mute(bool muted)
{
	(void)muted;
}

static void write_tts_active_state(const char *text, bool active)
{
	static char s_accum[2048] = {0};
	char path[512], tmp[528];
	if (jarvis_plat_state_path("jarvis-tts-active", ".state", path, sizeof(path)) != 0)
		return;
	if (!active) {
		s_accum[0] = '\0';
		remove(path);
		return;
	}
	if (text && *text) {
		size_t cur = strlen(s_accum);
		size_t add = strlen(text);
		if (cur == 0) {
			snprintf(s_accum, sizeof(s_accum), "%s", text);
		} else if (cur + 1 + add < sizeof(s_accum)) {
			snprintf(s_accum + cur, sizeof(s_accum) - cur, " %.*s", (int)(sizeof(s_accum) - cur - 2), text);
		} else {
			/* Keep most recent tail so self-echo matching spans across clause transitions */
			size_t keep_budget = (add + 2 < sizeof(s_accum)) ? (sizeof(s_accum) - 1 - add - 1) : 0;
			if (keep_budget > 256 && cur > keep_budget) {
				size_t shift = cur - keep_budget;
				memmove(s_accum, s_accum + shift, keep_budget);
				s_accum[keep_budget] = '\0';
				snprintf(s_accum + keep_budget, sizeof(s_accum) - keep_budget, " %s", text);
			} else {
				snprintf(s_accum, sizeof(s_accum), "%.*s", (int)(sizeof(s_accum) - 1), text);
			}
		}
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *fp = fopen(tmp, "wb");
	if (fp) {
		fprintf(fp, "%s\n", s_accum[0] ? s_accum : "1");
		fclose(fp);
#ifdef _WIN32
		remove(path);
#endif
		rename(tmp, path);
	}
}

/* Notify jarvisd of TTS state changes (speaking/idle) via TCP control socket */

static int resolve_daemon_port(void)
{
	const char *env_p = getenv("JARVIS_PORT");
	if (env_p && *env_p) {
		int p = atoi(env_p);
		if (p > 0 && p <= 65535) return p;
	}
	char pbuf[32];
	if (rc_lookup("port", pbuf, sizeof(pbuf)) && pbuf[0]) {
		int p = atoi(pbuf);
		if (p > 0 && p <= 65535) return p;
	}
	return 8179;
}

static void notify_tts_state(const char *state)
{
	stl_sock_init();
	int port = resolve_daemon_port();
	stl_socket_t fd = stl_sock_create(AF_INET, SOCK_STREAM, 0);
	if (fd == STL_INVALID_SOCKET) return;

	(void)stl_sock_set_timeout(fd, 200, 200);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (stl_sock_connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		stl_sock_close(fd);
		return;
	}

	char msg[128];
	snprintf(msg, sizeof(msg), "TTS_STATE|%s\n", state);
	jarvis_plat_write_sock_all(fd, msg, strlen(msg));
	stl_sock_close(fd);
}

static void clean_speech_text(const char *src, char *dst, size_t dst_sz)
{
	if (!dst || dst_sz == 0) return;
	dst[0] = '\0';
	if (!src) return;
	size_t w = 0;
	bool last_space = true;
	for (const char *p = src; *p && w + 1 < dst_sz; ) {
		if (strncmp(p, "http://", 7) == 0 || strncmp(p, "https://", 8) == 0) {
			const char *host = (strncmp(p, "https://", 8) == 0) ? (p + 8) : (p + 7);
			if (strncmp(host, "www.", 4) == 0) host += 4;
			while (*host && !isspace((unsigned char)*host) && *host != '/' && *host != ':' && w + 1 < dst_sz) {
				dst[w++] = *host++;
			}
			while (*p && !isspace((unsigned char)*p)) p++;
			last_space = false;
			continue;
		}
		if (strchr("`*_#[]<>", *p)) {
			p++;
			continue;
		}
		if (isspace((unsigned char)*p)) {
			if (!last_space) {
				dst[w++] = ' ';
				last_space = true;
			}
		} else {
			dst[w++] = *p;
			last_space = false;
		}
		p++;
	}
	if (w > 0 && dst[w - 1] == ' ') w--;
	dst[w] = '\0';
	if (w > 1000) {
		dst[1000] = '\0';
		char *dot = strrchr(dst, '.');
		if (dot && (dot - dst) > 200) *(dot + 1) = '\0';
	}
}

/* ---------------------------------------------------------------------------
 * Single-Voice Loader from Uncompressed ZIP (.npz) voices-v1.0.bin (510 KB)
 * ------------------------------------------------------------------------- */

static float *load_kokoro_voice_npy(const char *zip_path, const char *voice_name)
{
	FILE *fp = fopen(zip_path, "rb");
	if (!fp) return NULL;

	char target_entry[96];
	snprintf(target_entry, sizeof(target_entry), "%s.npy", voice_name);
	size_t target_len = strlen(target_entry);

	uint8_t hdr[30];
	while (fread(hdr, 1, 30, fp) == 30) {
		if (hdr[0] != 'P' || hdr[1] != 'K' || hdr[2] != 3 || hdr[3] != 4) break;
		uint16_t method = (uint16_t)hdr[8] | ((uint16_t)hdr[9] << 8);
		uint32_t comp_sz32 = (uint32_t)hdr[18] | ((uint32_t)hdr[19] << 8) |
		                     ((uint32_t)hdr[20] << 16) | ((uint32_t)hdr[21] << 24);
		uint32_t uncomp_sz32 = (uint32_t)hdr[22] | ((uint32_t)hdr[23] << 8) |
		                       ((uint32_t)hdr[24] << 16) | ((uint32_t)hdr[25] << 24);
		uint16_t fname_len = (uint16_t)hdr[26] | ((uint16_t)hdr[27] << 8);
		uint16_t extra_len = (uint16_t)hdr[28] | ((uint16_t)hdr[29] << 8);

		char fname[128] = {0};
		if (fname_len >= sizeof(fname)) {
			fseek(fp, (long)fname_len, SEEK_CUR);
		} else {
			if (fread(fname, 1, fname_len, fp) != fname_len) break;
			fname[fname_len] = '\0';
		}

		uint64_t real_comp_sz = comp_sz32;
		if (extra_len > 0) {
			uint8_t extra[512];
			if (extra_len <= sizeof(extra)) {
				if (fread(extra, 1, extra_len, fp) != extra_len) break;
				if (comp_sz32 == 0xFFFFFFFFU || uncomp_sz32 == 0xFFFFFFFFU) {
					size_t pos = 0;
					while (pos + 4 <= extra_len) {
						uint16_t etag = (uint16_t)extra[pos] | ((uint16_t)extra[pos + 1] << 8);
						uint16_t esz  = (uint16_t)extra[pos + 2] | ((uint16_t)extra[pos + 3] << 8);
						pos += 4;
						if (etag == 0x0001 && pos + esz <= extra_len) {
							size_t fpos = pos;
							if (uncomp_sz32 == 0xFFFFFFFFU && fpos + 8 <= pos + esz)
								fpos += 8;
							if (comp_sz32 == 0xFFFFFFFFU && fpos + 8 <= pos + esz) {
								uint64_t c64 = 0;
								for (int b = 0; b < 8; b++)
									c64 |= ((uint64_t)extra[fpos + b]) << (b * 8);
								real_comp_sz = c64;
							}
							break;
						}
						pos += esz;
					}
				}
			} else {
				fseek(fp, (long)extra_len, SEEK_CUR);
			}
		}

		long data_start = ftell(fp);
		if (method == 0 && fname_len == target_len && strcmp(fname, target_entry) == 0 &&
		    real_comp_sz >= (uint64_t)(128 + KOKORO_STYLE_ROWS * KOKORO_STYLE_DIM * 4)) {
			uint8_t npy_hdr[128];
			if (fread(npy_hdr, 1, 10, fp) == 10 && memcmp(npy_hdr, "\x93NUMPY", 6) == 0) {
				uint16_t hlen = (uint16_t)npy_hdr[8] | ((uint16_t)npy_hdr[9] << 8);
				fseek(fp, data_start + 10 + (long)hlen, SEEK_SET);
				size_t n_floats = KOKORO_STYLE_ROWS * KOKORO_STYLE_DIM;
				float *buf = (float *)malloc(n_floats * sizeof(float));
				if (buf) {
					if (fread(buf, sizeof(float), n_floats, fp) == n_floats) {
						fclose(fp);
						return buf;
					}
					free(buf);
				}
			}
			break;
		}
		if (fseek(fp, data_start + (long)real_comp_sz, SEEK_SET) != 0)
			break;
	}
	fclose(fp);
	return NULL;
}

/* ---------------------------------------------------------------------------
 * UTF-8 Decoder & IPA Phonemizer
 * ------------------------------------------------------------------------- */

static uint32_t next_utf8_cp(const char **p)
{
	const unsigned char *s = (const unsigned char *)*p;
	if (!*s) return 0;
	uint32_t cp = 0;
	if (s[0] < 0x80) {
		cp = s[0];
		*p += 1;
	} else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
		cp = ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
		*p += 2;
	} else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
		cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
		*p += 3;
	} else if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
		cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
		     ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
		*p += 4;
	} else {
		cp = s[0];
		*p += 1;
	}
	return cp;
}

static int16_t lookup_vocab_id(const cp_id_t *table, uint32_t cp)
{
	for (size_t i = 0; table[i].id >= 0; i++) {
		if (table[i].cp == cp) return table[i].id;
	}
	return -1;
}

static void normalize_espeak_voice(const char *in_voice, char *out_voice, size_t out_sz)
{
	if (!in_voice || !*in_voice) {
		char def_lang[32];
		resolve_default_lang(def_lang, sizeof(def_lang));
		in_voice = def_lang[0] ? def_lang : "en";
		if (in_voice == def_lang) {
			/* copy safely before recursion / fallthrough */
			char tmp[32];
			size_t i = 0;
			for (; def_lang[i] && i + 1 < sizeof(tmp); i++) {
				char c = (char)tolower((unsigned char)def_lang[i]);
				tmp[i] = (c == '_') ? '-' : c;
			}
			tmp[i] = '\0';
			if (strcmp(tmp, "pt-pt") == 0) snprintf(out_voice, out_sz, "pt");
			else if (strcmp(tmp, "fr-fr") == 0) snprintf(out_voice, out_sz, "fr");
			else snprintf(out_voice, out_sz, "%s", tmp);
			return;
		}
	}
	char tmp[32];
	size_t i = 0;
	for (; in_voice[i] && i + 1 < sizeof(tmp); i++) {
		char c = (char)tolower((unsigned char)in_voice[i]);
		tmp[i] = (c == '_') ? '-' : c;
	}
	tmp[i] = '\0';

	/* In espeak-ng, European Portuguese is "pt" (not "pt-pt") and French is "fr" (not "fr-fr"). */
	if (strcmp(tmp, "pt-pt") == 0) {
		snprintf(out_voice, out_sz, "pt");
	} else if (strcmp(tmp, "fr-fr") == 0) {
		snprintf(out_voice, out_sz, "fr");
	} else {
		snprintf(out_voice, out_sz, "%s", tmp);
	}
}

static bool select_espeak_voice(const char *espeak_voice)
{
	if (!g_tts.espeak_SetVoice) return false;
	char norm[32];
	normalize_espeak_voice(espeak_voice, norm, sizeof(norm));
	if (g_tts.espeak_SetVoice(norm) == 0) return true;

	/* Fallback 1: strip region suffix (e.g. "pt-pt" -> "pt", "es-es" -> "es") */
	char *dash = strchr(norm, '-');
	if (dash) {
		*dash = '\0';
		if (norm[0] && g_tts.espeak_SetVoice(norm) == 0) return true;
	}

	/* Fallback 2: neutral English ("en") if configured locale is missing */
	if (strcmp(norm, "en") != 0 && g_tts.espeak_SetVoice("en") == 0) return true;
	return false;
}

static void phonemize_to_ipa(const char *text, const char *espeak_voice, char *out_ipa, size_t out_sz)
{
	out_ipa[0] = '\0';
	if (!g_tts.espeak_TextToPhonemesTerm || !text || !*text) return;

	if (!select_espeak_voice(espeak_voice)) return;
	const void *ptr = (const void *)text;
	size_t pos = 0;

	while (ptr && *(const char *)ptr && pos + 8 < out_sz) {
		int term = 0;
		const char *ph = g_tts.espeak_TextToPhonemesTerm(&ptr, 1 /* UTF8 */, 0x02 /* IPA */, &term);
		if (ph && *ph) {
			if (pos > 0 && out_ipa[pos - 1] != ' ') {
				out_ipa[pos++] = ' ';
			}
			for (const char *c = ph; *c && pos + 4 < out_sz; c++) {
				/* Strip inline language switch flags like (en) */
				if (*c == '(') {
					while (*c && *c != ')') c++;
					if (!*c) break;
					continue;
				}
				out_ipa[pos++] = *c;
			}
			unsigned int punct_code = ((unsigned int)term >> 12) & 0x0FU;
			bool has_more = (ptr && *(const char *)ptr);
			char punct = 0;
			switch (punct_code) {
			case 0:
				/* Only append '.' if the input clause actually contained a period or another sub-clause follows */
				if (has_more) {
					punct = '.';
				} else {
					size_t tl = strlen(text);
					while (tl > 0 && isspace((unsigned char)text[tl - 1])) tl--;
					if (tl > 0 && text[tl - 1] == '.') punct = '.';
				}
				break;
			case 1: punct = ','; break;
			case 2: punct = '?'; break;
			case 3: punct = '!'; break;
			case 4: punct = ':'; break;
			case 5: punct = ';'; break;
			default: break;
			}
			if (punct && pos + 2 < out_sz) {
				out_ipa[pos++] = punct;
			}
		} else {
			break;
		}
	}
	out_ipa[pos] = '\0';
}

/* Clean up espeak-ng European Portuguese ("pt") IPA quirks before feeding Kokoro-82M:
 * 1. Replace English retroflex/approximant ɹ (U+0279, C9 B9) emitted in consonant clusters
 *    (cr, pr, tr, br, dr, fr, gr) with the Portuguese alveolar tap ɾ (U+027E, C9 BE).
 * 2. Strip espeak-ng's artificial epenthetic schwa ə (U+0259, C9 99) immediately after ɾ
 *    (e.g. ʒˈaɾəbɐʃ -> ʒˈaɾbɐʃ, kˌumpuɾətɐm -> kˌumpuɾtɐm, pˌɐɾətˈik -> pˌɐɾtˈik).
 * 3. Replace English-like diphthong nasal eɪŋ (65 C9 AA C5 8B) with Portuguese nasal ẽŋ (65 CC 83 C5 8B).
 * 4. Normalize word-final European Portuguese ʃ (U+0283, CA 83) before whitespace/punctuation/EOS
 *    to 's' so Kokoro's pm_alex voice does not over-palatalize final -s into English "-sh" ("Jarbash").
 */
static void postprocess_kokoro_pt_ipa(char *ipa, size_t max_sz, bool is_native_pt_pt)
{
	if (!ipa || !ipa[0]) return;
	char out[1024];
	size_t oi = 0;
	const unsigned char *s = (const unsigned char *)ipa;
	while (*s && oi + 8 < sizeof(out) && oi + 8 < max_sz) {
		if (s[0] == 0xC9 && s[1] == 0xB9) {
			out[oi++] = (char)0xC9;
			out[oi++] = (char)0xBE;
			s += 2;
			continue;
		}
		if (s[0] == 0xC9 && s[1] == 0xBE && s[2] == 0xC9 && s[3] == 0x99) {
			out[oi++] = (char)0xC9;
			out[oi++] = (char)0xBE;
			s += 4;
			continue;
		}
		if (s[0] == 'e' && s[1] == 0xC9 && s[2] == 0xAA && s[3] == 0xC5 && s[4] == 0x8B) {
			out[oi++] = 'e';
			out[oi++] = (char)0xCC;
			out[oi++] = (char)0x83;
			out[oi++] = (char)0xC5;
			out[oi++] = (char)0x8B;
			s += 5;
			continue;
		}
		if (!is_native_pt_pt && s[0] == 0xCA && s[1] == 0x83) {
			unsigned char nxt = s[2];
			if (nxt == '\0' || nxt == ' ' || nxt == ',' || nxt == '.' ||
			    nxt == '!' || nxt == '?' || nxt == ';' || nxt == ':') {
				out[oi++] = 's';
				s += 2;
				continue;
			}
		}
		out[oi++] = (char)*s++;
	}
	out[oi] = '\0';
	snprintf(ipa, max_sz, "%s", out);
}

/* ---------------------------------------------------------------------------
 * Lazy Load & Unload of ONNX Runtime + Espeak-NG (0 KB RAM when unloaded)
 * ------------------------------------------------------------------------- */

static void ort_release_status(void *st)
{
	if (st && g_tts.ort_vtbl) {
		ort_Release_fn rel = (ort_Release_fn)g_tts.ort_vtbl[ORT_IDX_ReleaseStatus];
		if (rel) rel(st);
	}
}

static void tts_cache_clear_locked(void)
{
	for (size_t i = 0; i < TTS_CACHE_SLOTS; i++) {
		free(g_tts.cache[i].pcm);
		g_tts.cache[i].pcm = NULL;
		g_tts.cache[i].n_samples = 0;
		g_tts.cache[i].sample_rate = 0;
		g_tts.cache[i].voice[0] = '\0';
		g_tts.cache[i].text[0] = '\0';
		g_tts.cache[i].last_used = 0;
	}
}

static int16_t *tts_cache_lookup(const char *voice, const char *text, size_t *out_samples, int *out_rate)
{
	if (!voice || !text || !*text || strlen(text) >= TTS_CACHE_MAX_TEXT) return NULL;
	int16_t *copy = NULL;
	pthread_mutex_lock(&g_tts.mtx);
	for (size_t i = 0; i < TTS_CACHE_SLOTS; i++) {
		tts_cache_entry_t *e = &g_tts.cache[i];
		if (e->pcm && e->n_samples > 0 &&
		    strcmp(e->voice, voice) == 0 && strcmp(e->text, text) == 0) {
			size_t bytes = e->n_samples * sizeof(int16_t);
			copy = (int16_t *)malloc(bytes);
			if (copy) {
				memcpy(copy, e->pcm, bytes);
				*out_samples = e->n_samples;
				*out_rate = e->sample_rate;
				e->last_used = ++g_tts.cache_tick;
			}
			break;
		}
	}
	pthread_mutex_unlock(&g_tts.mtx);
	return copy;
}

static void tts_cache_store(const char *voice, const char *text, const int16_t *pcm, size_t n_samples, int sample_rate)
{
	if (!voice || !*voice || !text || !*text || !pcm || n_samples == 0) return;
	if (strlen(text) >= TTS_CACHE_MAX_TEXT) return;
	size_t bytes = n_samples * sizeof(int16_t);
	if (bytes > TTS_CACHE_MAX_BYTES) return;

	int16_t *dup = (int16_t *)malloc(bytes);
	if (!dup) return;
	memcpy(dup, pcm, bytes);

	pthread_mutex_lock(&g_tts.mtx);
	size_t victim = 0;
	uint64_t oldest = UINT64_MAX;
	for (size_t i = 0; i < TTS_CACHE_SLOTS; i++) {
		tts_cache_entry_t *e = &g_tts.cache[i];
		if (e->pcm && strcmp(e->voice, voice) == 0 && strcmp(e->text, text) == 0) {
			victim = i;
			break;
		}
		if (!e->pcm) {
			victim = i;
			break;
		}
		if (e->last_used < oldest) {
			oldest = e->last_used;
			victim = i;
		}
	}
	free(g_tts.cache[victim].pcm);
	g_tts.cache[victim].pcm = dup;
	g_tts.cache[victim].n_samples = n_samples;
	g_tts.cache[victim].sample_rate = sample_rate;
	g_tts.cache[victim].last_used = ++g_tts.cache_tick;
	snprintf(g_tts.cache[victim].voice, sizeof(g_tts.cache[victim].voice), "%s", voice);
	snprintf(g_tts.cache[victim].text, sizeof(g_tts.cache[victim].text), "%s", text);
	pthread_mutex_unlock(&g_tts.mtx);
}

static void close_edge_worker_locked(bool kill_immediate)
{
	stl_proc_t proc = g_tts.edge_proc;
	bool active = g_tts.has_edge;
	memset(&g_tts.edge_proc, 0, sizeof(g_tts.edge_proc));
	g_tts.edge_proc.stdin_fd = -1;
	g_tts.edge_proc.stdout_fd = -1;
	g_tts.edge_proc.stderr_fd = -1;
	g_tts.has_edge = false;
	g_tts.edge_busy = false;
	if (!active) return;

	if (proc.stdin_fd >= 0) { tts_fd_close(proc.stdin_fd); proc.stdin_fd = -1; }
	if (proc.stdout_fd >= 0) { tts_fd_close(proc.stdout_fd); proc.stdout_fd = -1; }
	stl_proc_kill(&proc, kill_immediate ? 1 : 0);
	pthread_mutex_unlock(&g_tts.mtx);
	int ec = 0;
	stl_proc_wait(&proc, &ec, -1);
	stl_proc_close(&proc);
	pthread_mutex_lock(&g_tts.mtx);
}

static bool resolve_edge_bin_and_python(char *bin_out, size_t bin_sz, char *py_out, size_t py_sz)
{
	const char *env_bin = getenv("JARVIS_EDGE_TTS_BIN");
	char hbuf[512] = {0};
	const char *home = tts_home_dir(hbuf, sizeof(hbuf));
	bin_out[0] = '\0';
	py_out[0] = '\0';

	if (env_bin && *env_bin && jarvis_plat_is_exec(env_bin)) {
		snprintf(bin_out, bin_sz, "%s", env_bin);
	} else if (home && *home) {
#ifdef _WIN32
		snprintf(bin_out, bin_sz, "%s\\.local\\bin\\edge-tts-pcm.exe", home);
		if (!jarvis_plat_is_exec(bin_out)) {
			snprintf(bin_out, bin_sz, "%s/.local/bin/edge-tts-pcm", home);
		}
#else
		snprintf(bin_out, bin_sz, "%s/.local/bin/edge-tts-pcm", home);
#endif
		if (!jarvis_plat_is_exec(bin_out)) {
			if (jarvis_plat_find_in_path("edge-tts-pcm", bin_out, bin_sz) != 0)
				snprintf(bin_out, bin_sz, "edge-tts-pcm");
		}
	} else {
		snprintf(bin_out, bin_sz, "edge-tts-pcm");
	}

	if (home && *home && !env_bin) {
		static const char *const venvs[] = {
#ifdef _WIN32
			"%s/.local/share/dwm-voice/venv/Scripts/python.exe",
			"%s/.venv/Scripts/python.exe",
#else
			"%s/.local/share/dwm-voice/venv/bin/python",
			"%s/.venv/bin/python",
#endif
			NULL
		};
		for (size_t i = 0; venvs[i]; i++) {
			snprintf(py_out, py_sz, venvs[i], home);
			if (jarvis_plat_is_exec(py_out)) {
				return true;
			}
		}
		py_out[0] = '\0';
	}
	return bin_out[0] != '\0';
}

static bool ensure_edge_worker_locked(void)
{
	if (g_tts.has_edge && g_tts.edge_proc.stdin_fd >= 0 && g_tts.edge_proc.stdout_fd >= 0) {
		int ec = 0;
		int wr = stl_proc_wait(&g_tts.edge_proc, &ec, 0);
		if (wr == 1) return true;
		close_edge_worker_locked(false);
	}

	char bin_path[512], py_path[512];
	if (!resolve_edge_bin_and_python(bin_path, sizeof(bin_path), py_path, sizeof(py_path)))
		return false;

	stl_proc_t proc;
	int spawned = -1;
	if (py_path[0]) {
		const char *argv_py[] = { py_path, bin_path, "--serve", NULL };
		spawned = stl_spawn(argv_py, STL_SPAWN_STDIN | STL_SPAWN_STDOUT | STL_SPAWN_NEW_GROUP, &proc);
	}
	if (spawned < 0) {
		const char *argv_bin[] = { bin_path, "--serve", NULL };
		spawned = stl_spawn(argv_bin, STL_SPAWN_STDIN | STL_SPAWN_STDOUT | STL_SPAWN_NEW_GROUP, &proc);
	}
	if (spawned < 0)
		return false;

	g_tts.edge_proc = proc;
	g_tts.has_edge = true;
	g_tts.edge_busy = false;
	return true;
}

static void tts_onnx_unload_unlocked(void)
{
	if (g_tts.kokoro_style) {
		free(g_tts.kokoro_style);
		g_tts.kokoro_style = NULL;
	}
	if (g_tts.ort_vtbl) {
		ort_Release_fn rel_sess = (ort_Release_fn)g_tts.ort_vtbl[ORT_IDX_ReleaseSession];
		ort_Release_fn rel_mi   = (ort_Release_fn)g_tts.ort_vtbl[ORT_IDX_ReleaseMemoryInfo];
		ort_Release_fn rel_env  = (ort_Release_fn)g_tts.ort_vtbl[ORT_IDX_ReleaseEnv];
		if (g_tts.ort_sess && rel_sess) rel_sess(g_tts.ort_sess);
		if (g_tts.ort_mem_info && rel_mi) rel_mi(g_tts.ort_mem_info);
		if (g_tts.ort_env && rel_env) rel_env(g_tts.ort_env);
	}
	g_tts.ort_sess = NULL;
	g_tts.ort_mem_info = NULL;
	g_tts.ort_env = NULL;
	g_tts.ort_vtbl = NULL;
	g_tts.loaded_model[0] = '\0';
	g_tts.loaded_voice[0] = '\0';

	if (g_tts.ort_dl) {
		stl_dlclose(g_tts.ort_dl);
		g_tts.ort_dl = NULL;
	}
	if (g_tts.espeak_dl) {
		if (g_tts.espeak_Terminate) g_tts.espeak_Terminate();
		stl_dlclose(g_tts.espeak_dl);
		g_tts.espeak_dl = NULL;
		g_tts.espeak_Init = NULL;
		g_tts.espeak_SetVoice = NULL;
		g_tts.espeak_TextToPhonemesTerm = NULL;
		g_tts.espeak_Terminate = NULL;
	}
#ifdef __GLIBC__
	malloc_trim(0);
#endif
}

static bool reap_draining_player_locked(bool block);
static void close_persistent_player_locked(bool wait_for_drain);

static void tts_engine_unload(void)
{
	tts_cache_clear_locked();
	close_persistent_player_locked(true);
	reap_draining_player_locked(true);
	close_edge_worker_locked(false);
	tts_onnx_unload_unlocked();
}

static void resolve_tts_data_path(const char *home, const char *subdir, const char *file,
                                  char *out, size_t cap)
{
	if (home && *home) {
		snprintf(out, cap, "%s/.local/share/%s/%s", home, subdir, file);
		if (jarvis_plat_is_readable(out))
			return;
	}
	if (jarvis_plat_data_file(subdir, file, out, cap) == 0 && jarvis_plat_is_readable(out))
		return;
	if (home && *home)
		snprintf(out, cap, "%s/.local/share/%s/%s", home, subdir, file);
	else
		out[0] = '\0';
}

static bool tts_engine_ensure_loaded(const char *voice)
{
	if (voice && (strncmp(voice, "edge:", 5) == 0 || strcasecmp(voice, "raquel") == 0 || strcasecmp(voice, "duarte") == 0)) {
		pthread_mutex_lock(&g_tts.mtx);
		bool ok = ensure_edge_worker_locked();
		pthread_mutex_unlock(&g_tts.mtx);
		return ok;
	}
	char hbuf[512] = {0};
	const char *home = tts_home_dir(hbuf, sizeof(hbuf));

	bool want_piper = (strncmp(voice, "piper:", 6) == 0);
	bool want_kokoro_pt = false;
	char model_path[512];
	char voices_bin[512];
	resolve_tts_data_path(home, "kokoro", "voices-v1.0.bin", voices_bin, sizeof(voices_bin));

	if (want_piper) {
		const char *p_name = voice + 6;
		if (strcmp(p_name, "us") == 0) {
			resolve_tts_data_path(home, "piper", "en_US-lessac-high.onnx", model_path, sizeof(model_path));
		} else if (strcmp(p_name, "gb") == 0 || !*p_name) {
			resolve_tts_data_path(home, "piper", "en_GB-northern_english_male-medium.onnx", model_path, sizeof(model_path));
		} else if (strstr(p_name, ".onnx")) {
			resolve_tts_data_path(home, "piper", p_name, model_path, sizeof(model_path));
		} else {
			char fname[128];
			snprintf(fname, sizeof(fname), "%s.onnx", p_name);
			resolve_tts_data_path(home, "piper", fname, model_path, sizeof(model_path));
		}
	} else {
		char def_lang[32];
		resolve_default_lang(def_lang, sizeof(def_lang));
		bool is_pt_voice = (voice && *voice)
			? (tolower((unsigned char)voice[0]) == 'p')
			: (strncasecmp(def_lang, "pt", 2) == 0);
		resolve_tts_data_path(home, "kokoro", "kokoro-pt-pt.onnx", model_path, sizeof(model_path));
		if (is_pt_voice && jarvis_plat_is_readable(model_path) && jarvis_plat_is_readable(voices_bin)) {
			want_kokoro_pt = true;
		} else {
			resolve_tts_data_path(home, "kokoro", "kokoro-v1.0.int8.onnx", model_path, sizeof(model_path));
			if (!jarvis_plat_is_readable(model_path) || !jarvis_plat_is_readable(voices_bin)) {
				want_piper = true;
				resolve_tts_data_path(home, "piper", "en_GB-northern_english_male-medium.onnx", model_path, sizeof(model_path));
			}
		}
	}

	if (!jarvis_plat_is_readable(model_path)) return false;

	/* 1. Lazy dlopen espeak-ng */
	if (!g_tts.espeak_dl) {
		g_tts.espeak_dl = stl_dlopen(jarvis_plat_espeak_lib_name(), 0);
		if (!g_tts.espeak_dl) return false;
		g_tts.espeak_Init = (espeak_Init_fn)stl_dlsym(g_tts.espeak_dl, "espeak_Initialize");
		g_tts.espeak_SetVoice = (espeak_SetVoice_fn)stl_dlsym(g_tts.espeak_dl, "espeak_SetVoiceByName");
		g_tts.espeak_TextToPhonemesTerm = (espeak_TextToPhonemesTerm_fn)stl_dlsym(g_tts.espeak_dl, "espeak_TextToPhonemesWithTerminator");
		g_tts.espeak_Terminate = (espeak_Terminate_fn)stl_dlsym(g_tts.espeak_dl, "espeak_Terminate");
		if (!g_tts.espeak_Init || !g_tts.espeak_SetVoice || !g_tts.espeak_TextToPhonemesTerm) {
			tts_onnx_unload_unlocked();
			return false;
		}
		char def_lang[32];
		resolve_default_lang(def_lang, sizeof(def_lang));
		if (g_tts.espeak_Init(0x02 /* AUDIO_OUTPUT_SYNCHRONOUS */, 0, NULL, 0) <= 0 ||
		    !select_espeak_voice(def_lang)) {
			tts_onnx_unload_unlocked();
			return false;
		}
	}

	/* 2. Lazy dlopen onnxruntime (suppress stderr duplicate ONNX schema chatter) */
	if (!g_tts.ort_dl) {
		int saved_stderr = -1;
		jarvis_plat_suppress_stderr_begin(&saved_stderr);
		g_tts.ort_dl = stl_dlopen(jarvis_plat_ort_lib_name(0), 0);
		if (!g_tts.ort_dl) g_tts.ort_dl = stl_dlopen(jarvis_plat_ort_lib_name(1), 0);
		jarvis_plat_suppress_stderr_end(saved_stderr);
		if (!g_tts.ort_dl) {
			tts_onnx_unload_unlocked();
			return false;
		}
		OrtGetApiBase_fn get_base = (OrtGetApiBase_fn)stl_dlsym(g_tts.ort_dl, "OrtGetApiBase");
		if (!get_base || !get_base()) {
			tts_onnx_unload_unlocked();
			return false;
		}
		const OrtApiBase_t *base = get_base();
		g_tts.ort_vtbl = (const void **)base->GetApi(ORT_API_VERSION);
		if (!g_tts.ort_vtbl ||
		    !g_tts.ort_vtbl[ORT_IDX_CreateEnv] ||
		    !g_tts.ort_vtbl[ORT_IDX_CreateSession] ||
		    !g_tts.ort_vtbl[ORT_IDX_Run] ||
		    !g_tts.ort_vtbl[ORT_IDX_CreateSessionOptions] ||
		    !g_tts.ort_vtbl[ORT_IDX_DisableCpuMemArena] ||
		    !g_tts.ort_vtbl[ORT_IDX_SetGraphOptimizationLevel] ||
		    !g_tts.ort_vtbl[ORT_IDX_SetIntraOpNumThreads] ||
		    !g_tts.ort_vtbl[ORT_IDX_SetInterOpNumThreads] ||
		    !g_tts.ort_vtbl[ORT_IDX_CreateTensorWithData] ||
		    !g_tts.ort_vtbl[ORT_IDX_GetTensorMutableData] ||
		    !g_tts.ort_vtbl[ORT_IDX_GetTensorShapeElementCount] ||
		    !g_tts.ort_vtbl[ORT_IDX_GetTensorTypeAndShape] ||
		    !g_tts.ort_vtbl[ORT_IDX_CreateCpuMemoryInfo] ||
		    !g_tts.ort_vtbl[ORT_IDX_ReleaseEnv] ||
		    !g_tts.ort_vtbl[ORT_IDX_ReleaseStatus] ||
		    !g_tts.ort_vtbl[ORT_IDX_ReleaseMemoryInfo] ||
		    !g_tts.ort_vtbl[ORT_IDX_ReleaseSession] ||
		    !g_tts.ort_vtbl[ORT_IDX_ReleaseValue] ||
		    !g_tts.ort_vtbl[ORT_IDX_ReleaseTensorTypeAndShape] ||
		    !g_tts.ort_vtbl[ORT_IDX_ReleaseSessionOptions]) {
			tts_onnx_unload_unlocked();
			return false;
		}

		ort_CreateEnv_fn create_env = (ort_CreateEnv_fn)g_tts.ort_vtbl[ORT_IDX_CreateEnv];
		ort_CreateCpuMemoryInfo_fn create_mi = (ort_CreateCpuMemoryInfo_fn)g_tts.ort_vtbl[ORT_IDX_CreateCpuMemoryInfo];
		int saved_err2 = -1;
		jarvis_plat_suppress_stderr_begin(&saved_err2);
		void *st = create_env(3 /* ORT_LOGGING_LEVEL_ERROR */, "jarvis-tts", &g_tts.ort_env);
		jarvis_plat_suppress_stderr_end(saved_err2);
		if (st) { ort_release_status(st); tts_onnx_unload_unlocked(); return false; }
		st = create_mi(1 /* OrtArenaAllocator */, 0 /* OrtMemTypeDefault */, &g_tts.ort_mem_info);
		if (st) { ort_release_status(st); tts_onnx_unload_unlocked(); return false; }
	}

	/* 3. Load or switch ONNX model session if needed */
	if (!g_tts.ort_sess || strcmp(g_tts.loaded_model, model_path) != 0) {
		if (g_tts.ort_sess) {
			ort_Release_fn rel_sess = (ort_Release_fn)g_tts.ort_vtbl[ORT_IDX_ReleaseSession];
			rel_sess(g_tts.ort_sess);
			g_tts.ort_sess = NULL;
		}
		void *so = NULL;
		ort_CreateSessionOptions_fn create_so = (ort_CreateSessionOptions_fn)g_tts.ort_vtbl[ORT_IDX_CreateSessionOptions];
		ort_SetGraphOptLevel_fn set_opt = (ort_SetGraphOptLevel_fn)g_tts.ort_vtbl[ORT_IDX_SetGraphOptimizationLevel];
		ort_SetIntraThreads_fn set_intra = (ort_SetIntraThreads_fn)g_tts.ort_vtbl[ORT_IDX_SetIntraOpNumThreads];
		ort_SetInterThreads_fn set_inter = (ort_SetInterThreads_fn)g_tts.ort_vtbl[ORT_IDX_SetInterOpNumThreads];
		ort_CreateSession_fn create_sess = (ort_CreateSession_fn)g_tts.ort_vtbl[ORT_IDX_CreateSession];
		ort_Release_fn rel_so = (ort_Release_fn)g_tts.ort_vtbl[ORT_IDX_ReleaseSessionOptions];

		void *st = create_so(&so);
		if (st) { ort_release_status(st); return false; }
		ort_release_status(set_opt(so, 99 /* ORT_ENABLE_ALL */));
		ort_release_status(set_intra(so, 4));
		ort_release_status(set_inter(so, 1));

		st = create_sess(g_tts.ort_env, model_path, so, &g_tts.ort_sess);
		rel_so(so);
		if (st || !g_tts.ort_sess) {
			ort_release_status(st);
			return false;
		}
		snprintf(g_tts.loaded_model, sizeof(g_tts.loaded_model), "%s", model_path);
		g_tts.is_piper = want_piper;
		g_tts.is_kokoro_pt = want_kokoro_pt;
	}

	/* 4. Load single 510 KB Kokoro voice style matrix if using Kokoro */
	if (!g_tts.is_piper) {
		const char *vname = (voice && *voice) ? voice : (g_tts.is_kokoro_pt ? "pm_tuga" : "bm_daniel");
		if (g_tts.is_kokoro_pt && strcmp(vname, "pm_alex") == 0) {
			vname = "pm_tuga";
		}
		if (!g_tts.kokoro_style || strcmp(g_tts.loaded_voice, vname) != 0) {
			free(g_tts.kokoro_style);
			g_tts.kokoro_style = load_kokoro_voice_npy(voices_bin, vname);
			if (!g_tts.kokoro_style && g_tts.is_kokoro_pt && strcmp(vname, "pm_tuga") != 0) {
				vname = "pm_tuga";
				g_tts.kokoro_style = load_kokoro_voice_npy(voices_bin, vname);
			}
			if (!g_tts.kokoro_style && strcmp(vname, "bm_daniel") != 0) {
				vname = "bm_daniel";
				g_tts.kokoro_style = load_kokoro_voice_npy(voices_bin, vname);
			}
			if (!g_tts.kokoro_style) return false;
			snprintf(g_tts.loaded_voice, sizeof(g_tts.loaded_voice), "%s", vname);
		}
	}

	return true;
}

/* ---------------------------------------------------------------------------
 * Synthesis of One Clause Segment to int16_t PCM
 * ------------------------------------------------------------------------- */

static void resolve_espeak_lang(const char *voice, bool is_piper, char *out_lang, size_t out_sz)
{
	char raw_lang[32] = {0};
	/* 1. Explicit override via JARVIS_TTS_LANG or ~/.jarvisrc tts_lang */
	const char *env_l = getenv("JARVIS_TTS_LANG");
	if (env_l && *env_l) {
		normalize_espeak_voice(env_l, out_lang, out_sz);
		return;
	}
	if (rc_lookup("tts_lang", raw_lang, sizeof(raw_lang)) && raw_lang[0]) {
		normalize_espeak_voice(raw_lang, out_lang, out_sz);
		return;
	}

	/* 2. Derive from Piper voice model prefix or Kokoro voice language prefix */
	if (is_piper) {
		const char *p = (strncmp(voice, "piper:", 6) == 0) ? (voice + 6) : voice;
		if (strcmp(p, "us") == 0 || strncmp(p, "en_US", 5) == 0) {
			snprintf(out_lang, out_sz, "en-us");
			return;
		}
		if (strncmp(p, "pt_BR", 5) == 0) { snprintf(out_lang, out_sz, "pt-br"); return; }
		if (strncmp(p, "pt_", 3) == 0)   { snprintf(out_lang, out_sz, "pt"); return; }
		if (strncmp(p, "es_", 3) == 0)   { snprintf(out_lang, out_sz, "es"); return; }
		if (strncmp(p, "fr_", 3) == 0)   { snprintf(out_lang, out_sz, "fr"); return; }
		if (strncmp(p, "de_", 3) == 0)   { snprintf(out_lang, out_sz, "de"); return; }
		if (strncmp(p, "it_", 3) == 0)   { snprintf(out_lang, out_sz, "it"); return; }
		if (strncmp(p, "ru_", 3) == 0)   { snprintf(out_lang, out_sz, "ru"); return; }
		snprintf(out_lang, out_sz, "en-gb-x-rp");
		return;
	}

	if (!voice || !voice[0]) {
		char def_lang[32];
		resolve_default_lang(def_lang, sizeof(def_lang));
		normalize_espeak_voice(def_lang, out_lang, out_sz);
		return;
	}

	/* Kokoro-82M standard 1-character language code prefix:
	 * 'a' = American English, 'b' = British English, 'p' = European Portuguese (pt),
	 * 'e' = Spanish, 'f' = French (fr), 'i' = Italian, 'h' = Hindi, 'j' = Japanese, 'z' = Mandarin
	 */
	switch (tolower((unsigned char)voice[0])) {
	case 'a': snprintf(out_lang, out_sz, "en-us"); break;
	case 'b': snprintf(out_lang, out_sz, "en-gb-x-rp"); break;
	case 'p': snprintf(out_lang, out_sz, "pt"); break;
	case 'e': snprintf(out_lang, out_sz, "es"); break;
	case 'f': snprintf(out_lang, out_sz, "fr"); break;
	case 'i': snprintf(out_lang, out_sz, "it"); break;
	case 'h': snprintf(out_lang, out_sz, "hi"); break;
	case 'j': snprintf(out_lang, out_sz, "ja"); break;
	case 'z': snprintf(out_lang, out_sz, "cmn"); break;
	default: {
		char def_lang[32];
		resolve_default_lang(def_lang, sizeof(def_lang));
		normalize_espeak_voice(def_lang, out_lang, out_sz);
		break;
	}
	}
}

static bool is_edge_voice_name(const char *voice)
{
	return voice && (strncmp(voice, "edge:", 5) == 0 ||
	                 strcasecmp(voice, "raquel") == 0 ||
	                 strcasecmp(voice, "duarte") == 0);
}

static const char *resolve_edge_voice(const char *voice, bool *out_is_pt)
{
	char def_lang[32];
	resolve_default_lang(def_lang, sizeof(def_lang));
	const char *ev = (strncasecmp(def_lang, "pt", 2) == 0)
		? "pt-PT-RaquelNeural"
		: "en-GB-RyanNeural";
	if (voice) {
		if (strncmp(voice, "edge:", 5) == 0 && voice[5]) {
			ev = voice + 5;
		} else if (strcasecmp(voice, "duarte") == 0) {
			ev = "pt-PT-DuarteNeural";
		} else if (strcasecmp(voice, "raquel") == 0) {
			ev = "pt-PT-RaquelNeural";
		}
	}
	if (out_is_pt)
		*out_is_pt = (strncasecmp(ev, "pt", 2) == 0) || (strncasecmp(def_lang, "pt", 2) == 0 && strncasecmp(ev, "en", 2) != 0);
	return ev;
}

static int resolve_edge_timeout_ms(void)
{
	const char *env = getenv("JARVIS_EDGE_TTS_TIMEOUT_MS");
	if (env && *env) {
		int v = atoi(env);
		if (v >= 50 && v <= 30000) return v;
	}
	return EDGE_TTS_TIMEOUT_MS;
}

static long monotonic_ms_now(void)
{
	return (long)stl_now_ms();
}

static bool write_all_edge(int fd, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	size_t off = 0;
	while (off < len) {
		ssize_t w = tts_fd_write(fd, p + off, len - off);
		if (w <= 0) {
#ifndef _WIN32
			if (w < 0 && errno == EINTR) continue;
#endif
			return false;
		}
		off += (size_t)w;
	}
	return true;
}

static int read_exact_edge_timeout(int fd, void *buf, size_t len, int timeout_ms, uint64_t my_gen)
{
	uint8_t *p = (uint8_t *)buf;
	size_t got = 0;
	long deadline = monotonic_ms_now() + (long)timeout_ms;

	while (got < len) {
		if (my_gen != 0) {
			pthread_mutex_lock(&g_tts.mtx);
			bool canceled = (g_tts.cancel_gen != my_gen);
			pthread_mutex_unlock(&g_tts.mtx);
			if (canceled) return -2;
		}

		long rem = deadline - monotonic_ms_now();
		if (rem <= 0) return -3;

		int slice = (rem > 50) ? 50 : (int)rem;
#ifdef _WIN32
		HANDLE h = (HANDLE)_get_osfhandle(fd);
		if (h == INVALID_HANDLE_VALUE) return -1;
		DWORD avail = 0;
		if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
			return -1;
		}
		if (avail == 0) {
			stl_sleep_ms(slice > 5 ? 5 : (unsigned)slice);
			continue;
		}
		size_t want = len - got;
		if (want > (size_t)avail) want = (size_t)avail;
		ssize_t r = tts_fd_read(fd, p + got, want);
		if (r <= 0) return -1;
		got += (size_t)r;
#else
		struct pollfd pfd;
		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		int pr = stl_poll(&pfd, 1, slice);
		if (pr < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		if (pr == 0) continue;

		if (pfd.revents & (POLLIN | POLLHUP)) {
			ssize_t r = tts_fd_read(fd, p + got, len - got);
			if (r < 0) {
				if (errno == EINTR || errno == EAGAIN) continue;
				return -1;
			}
			if (r == 0) return -1;
			got += (size_t)r;
		} else if (pfd.revents & (POLLERR | POLLNVAL)) {
			return -1;
		}
#endif
	}
	return 0;
}

static void resolve_edge_fallback_voice(bool is_pt, char *out_voice, size_t out_sz)
{
	resolve_local_voice(is_pt ? "pt" : "en", out_voice, out_sz);
}

static size_t trim_edge_pcm_silence(int16_t *pcm, size_t n_samples, const char *text)
{
	if (!pcm || n_samples < 480) return n_samples;
	const int16_t thresh = 160;
	size_t first = n_samples, last = 0;
	for (size_t i = 0; i < n_samples; i++) {
		int v = pcm[i] < 0 ? -(int)pcm[i] : (int)pcm[i];
		if (v > thresh) {
			first = i;
			break;
		}
	}
	if (first >= n_samples) return n_samples;
	for (size_t i = n_samples; i > first; i--) {
		int v = pcm[i - 1] < 0 ? -(int)pcm[i - 1] : (int)pcm[i - 1];
		if (v > thresh) {
			last = i;
			break;
		}
	}

	size_t lead_pad = (KOKORO_SAMPLE_RATE * 20) / 1000;
	size_t tl = text ? strlen(text) : 0;
	while (tl > 0 && isspace((unsigned char)text[tl - 1])) tl--;
	bool sent_end = (tl > 0 && strchr(".!?;:", text[tl - 1]) != NULL);
	size_t trail_pad = sent_end ? ((KOKORO_SAMPLE_RATE * 120) / 1000)
	                            : ((KOKORO_SAMPLE_RATE * 45) / 1000);

	size_t start = (first > lead_pad) ? (first - lead_pad) : 0;
	size_t end = (last + trail_pad < n_samples) ? (last + trail_pad) : n_samples;
	if (end <= start) return n_samples;

	size_t new_n = end - start;
	if (start > 0 && new_n > 0) {
		memmove(pcm, pcm + start, new_n * sizeof(int16_t));
	}
	size_t fade = (KOKORO_SAMPLE_RATE * 2) / 1000;
	if (start > 0 && new_n > fade) {
		for (size_t i = 0; i < fade; i++) {
			pcm[i] = (int16_t)((int32_t)pcm[i] * (int32_t)i / (int32_t)fade);
		}
	}
	if (end < n_samples && new_n > fade) {
		for (size_t i = 0; i < fade; i++) {
			size_t idx = new_n - 1 - i;
			pcm[idx] = (int16_t)((int32_t)pcm[idx] * (int32_t)i / (int32_t)fade);
		}
	}
	return new_n;
}

static int16_t *synthesize_edge_tts(const char *seg_text, const char *voice, size_t *out_samples, int *out_rate, uint64_t my_gen)
{
	*out_samples = 0;
	*out_rate = KOKORO_SAMPLE_RATE;
	if (!seg_text || !*seg_text) return NULL;

	const char *ev = resolve_edge_voice(voice, NULL);

	char safe_text[1024];
	size_t j = 0;
	for (size_t i = 0; seg_text[i] && j + 1 < sizeof(safe_text); i++) {
		char c = seg_text[i];
		safe_text[j++] = (c == '\t' || c == '\r' || c == '\n') ? ' ' : c;
	}
	safe_text[j] = '\0';
	trim_trailing(safe_text);
	const char *sp = safe_text;
	while (*sp && isspace((unsigned char)*sp)) sp++;
	if (!*sp) return NULL;

	char req[1200];
	int req_len = snprintf(req, sizeof(req), "%s\t%s\n", ev, sp);
	if (req_len <= 0 || (size_t)req_len >= sizeof(req)) return NULL;

	int timeout_ms = resolve_edge_timeout_ms();

	for (int attempt = 0; attempt < 2; attempt++) {
		pthread_mutex_lock(&g_tts.mtx);
		if (my_gen != 0 && g_tts.cancel_gen != my_gen) {
			pthread_mutex_unlock(&g_tts.mtx);
			return NULL;
		}
		if (!ensure_edge_worker_locked()) {
			pthread_mutex_unlock(&g_tts.mtx);
			return NULL;
		}
		int in_fd = g_tts.edge_proc.stdin_fd;
		int out_fd = g_tts.edge_proc.stdout_fd;
		bool wrote = write_all_edge(in_fd, req, (size_t)req_len);
		if (!wrote) {
			close_edge_worker_locked(true);
			pthread_mutex_unlock(&g_tts.mtx);
			continue;
		}
		g_tts.edge_busy = true;
		pthread_mutex_unlock(&g_tts.mtx);

		uint8_t hdr[4];
		int rc = read_exact_edge_timeout(out_fd, hdr, sizeof(hdr), timeout_ms, my_gen);
		if (rc != 0) {
			pthread_mutex_lock(&g_tts.mtx);
			g_tts.edge_busy = false;
			close_edge_worker_locked(true);
			bool canceled = (my_gen != 0 && g_tts.cancel_gen != my_gen);
			pthread_mutex_unlock(&g_tts.mtx);
			if (!canceled && rc == -3) {
				fprintf(stderr, "jarvis-tts: edge-tts timed out after %d ms (voice='%s')\n",
				        timeout_ms, ev);
			}
			if (canceled || rc == -2 || rc == -3) return NULL;
			continue;
		}

		uint32_t pcm_len = (uint32_t)hdr[0] |
		                   ((uint32_t)hdr[1] << 8) |
		                   ((uint32_t)hdr[2] << 16) |
		                   ((uint32_t)hdr[3] << 24);
		if (pcm_len == 0 || pcm_len > (32U * 1024U * 1024U) || (pcm_len % sizeof(int16_t)) != 0) {
			pthread_mutex_lock(&g_tts.mtx);
			g_tts.edge_busy = false;
			close_edge_worker_locked(pcm_len != 0);
			pthread_mutex_unlock(&g_tts.mtx);
			if (pcm_len == 0 && attempt == 0) {
				fprintf(stderr, "jarvis-tts: edge-tts returned empty PCM (attempt 1, voice='%s'); retrying\n", ev);
				continue;
			}
			fprintf(stderr, "jarvis-tts: edge-tts invalid/empty PCM (len=%u, voice='%s')\n", pcm_len, ev);
			return NULL;
		}

		uint8_t *pcm_buf = (uint8_t *)malloc(pcm_len);
		if (!pcm_buf) {
			pthread_mutex_lock(&g_tts.mtx);
			g_tts.edge_busy = false;
			close_edge_worker_locked(true);
			pthread_mutex_unlock(&g_tts.mtx);
			return NULL;
		}

		rc = read_exact_edge_timeout(out_fd, pcm_buf, pcm_len, timeout_ms, my_gen);
		pthread_mutex_lock(&g_tts.mtx);
		g_tts.edge_busy = false;
		if (rc != 0) {
			free(pcm_buf);
			close_edge_worker_locked(true);
			bool canceled = (my_gen != 0 && g_tts.cancel_gen != my_gen);
			pthread_mutex_unlock(&g_tts.mtx);
			if (canceled || rc == -2 || rc == -3) return NULL;
			continue;
		}
		pthread_mutex_unlock(&g_tts.mtx);

		int16_t *pcm16 = (int16_t *)pcm_buf;
		*out_samples = trim_edge_pcm_silence(pcm16, pcm_len / sizeof(int16_t), sp);
		*out_rate = KOKORO_SAMPLE_RATE;
		return pcm16;
	}
	return NULL;
}

static int16_t *synthesize_onnx_pcm(const char *seg_text, const char *voice, size_t *out_samples, int *out_rate)
{
	*out_samples = 0;
	*out_rate = KOKORO_SAMPLE_RATE;
	if (!seg_text || !*seg_text) return NULL;
	if (!tts_engine_ensure_loaded(voice)) return NULL;

	char espeak_lang[32];
	resolve_espeak_lang(voice, g_tts.is_piper, espeak_lang, sizeof(espeak_lang));

	char ipa[1024];
	phonemize_to_ipa(seg_text, espeak_lang, ipa, sizeof(ipa));
	if (!ipa[0]) return NULL;
	if (!g_tts.is_piper && strncmp(espeak_lang, "pt", 2) == 0) {
		postprocess_kokoro_pt_ipa(ipa, sizeof(ipa), g_tts.is_kokoro_pt);
	}

	ort_CreateTensorWithData_fn create_tensor = (ort_CreateTensorWithData_fn)g_tts.ort_vtbl[ORT_IDX_CreateTensorWithData];
	ort_Run_fn ort_run = (ort_Run_fn)g_tts.ort_vtbl[ORT_IDX_Run];
	ort_GetTensorTypeAndShape_fn get_ts = (ort_GetTensorTypeAndShape_fn)g_tts.ort_vtbl[ORT_IDX_GetTensorTypeAndShape];
	ort_GetTensorShapeElementCount_fn get_cnt = (ort_GetTensorShapeElementCount_fn)g_tts.ort_vtbl[ORT_IDX_GetTensorShapeElementCount];
	ort_GetTensorMutableData_fn get_data = (ort_GetTensorMutableData_fn)g_tts.ort_vtbl[ORT_IDX_GetTensorMutableData];
	ort_Release_fn rel_val = (ort_Release_fn)g_tts.ort_vtbl[ORT_IDX_ReleaseValue];
	ort_Release_fn rel_ts = (ort_Release_fn)g_tts.ort_vtbl[ORT_IDX_ReleaseTensorTypeAndShape];

	void *out_val = NULL;

	if (!g_tts.is_piper) {
		int64_t tokens[512];
		size_t n_inner = 0;
		tokens[0] = 0;
		const char *p = ipa;
		while (*p && n_inner < 508) {
			uint32_t cp = next_utf8_cp(&p);
			int16_t tid = lookup_vocab_id(KOKORO_VOCAB, cp);
			if (tid >= 0) {
				tokens[1 + n_inner++] = (int64_t)tid;
			}
		}
		if (n_inner == 0) return NULL;
		tokens[1 + n_inner] = 0;
		size_t seq_len = n_inner + 2;

		/* Clamp style_row to >= 64 so short initial segments never select the
		 * ultra-short creaky/groggy style vectors at rows 0..45 of voices-v1.0.bin.
		 */
		size_t eff_inner = (n_inner < 65) ? 65 : n_inner;
		size_t style_row = (eff_inner <= KOKORO_STYLE_ROWS) ? (eff_inner - 1) : (KOKORO_STYLE_ROWS - 1);
		float *style_vec = g_tts.kokoro_style + (style_row * KOKORO_STYLE_DIM);
		float speed_val = g_tts.is_kokoro_pt ? 1.02f : 1.06f;

		int64_t tok_shape[2] = { 1, (int64_t)seq_len };
		int64_t style_shape[2] = { 1, KOKORO_STYLE_DIM };
		int64_t speed_shape[1] = { 1 };

		void *v_tok = NULL, *v_style = NULL, *v_speed = NULL;
		ort_release_status(create_tensor(g_tts.ort_mem_info, tokens, seq_len * sizeof(int64_t), tok_shape, 2, ONNX_TENSOR_INT64, &v_tok));
		ort_release_status(create_tensor(g_tts.ort_mem_info, style_vec, KOKORO_STYLE_DIM * sizeof(float), style_shape, 2, ONNX_TENSOR_FLOAT, &v_style));
		ort_release_status(create_tensor(g_tts.ort_mem_info, &speed_val, sizeof(float), speed_shape, 1, ONNX_TENSOR_FLOAT, &v_speed));

		const char *in_names[3] = { g_tts.is_kokoro_pt ? "input_ids" : "tokens", "style", "speed" };
		const void *in_vals[3] = { v_tok, v_style, v_speed };
		const char *out_names[1] = { g_tts.is_kokoro_pt ? "waveform" : "audio" };

		void *st = ort_run(g_tts.ort_sess, NULL, in_names, in_vals, 3, out_names, 1, &out_val);
		if (v_tok) rel_val(v_tok);
		if (v_style) rel_val(v_style);
		if (v_speed) rel_val(v_speed);
		if (st) {
			ort_release_status(st);
			return NULL;
		}
		*out_rate = KOKORO_SAMPLE_RATE;
	} else {
		int64_t ids[1024];
		size_t n_ids = 0;
		ids[n_ids++] = 1; /* BOS ^ */
		ids[n_ids++] = 0; /* PAD _ */
		const char *p = ipa;
		while (*p && n_ids + 4 < 1024) {
			uint32_t cp = next_utf8_cp(&p);
			int16_t pid = lookup_vocab_id(PIPER_VOCAB, cp);
			if (pid >= 0) {
				ids[n_ids++] = (int64_t)pid;
				ids[n_ids++] = 0; /* PAD _ */
			}
		}
		if (n_ids <= 2) return NULL;
		ids[n_ids++] = 2; /* EOS $ */

		int64_t in_shape[2] = { 1, (int64_t)n_ids };
		int64_t len_val[1] = { (int64_t)n_ids };
		int64_t len_shape[1] = { 1 };
		float scales[3] = { 0.667f, 1.0f, 0.8f };
		int64_t scales_shape[1] = { 3 };

		void *v_in = NULL, *v_len = NULL, *v_sc = NULL;
		ort_release_status(create_tensor(g_tts.ort_mem_info, ids, n_ids * sizeof(int64_t), in_shape, 2, ONNX_TENSOR_INT64, &v_in));
		ort_release_status(create_tensor(g_tts.ort_mem_info, len_val, sizeof(int64_t), len_shape, 1, ONNX_TENSOR_INT64, &v_len));
		ort_release_status(create_tensor(g_tts.ort_mem_info, scales, sizeof(scales), scales_shape, 1, ONNX_TENSOR_FLOAT, &v_sc));

		const char *in_names[3] = { "input", "input_lengths", "scales" };
		const void *in_vals[3] = { v_in, v_len, v_sc };
		const char *out_names[1] = { "output" };

		void *st = ort_run(g_tts.ort_sess, NULL, in_names, in_vals, 3, out_names, 1, &out_val);
		if (v_in) rel_val(v_in);
		if (v_len) rel_val(v_len);
		if (v_sc) rel_val(v_sc);
		if (st) {
			ort_release_status(st);
			return NULL;
		}
		*out_rate = PIPER_SAMPLE_RATE;
	}

	if (!out_val) return NULL;

	void *ts_info = NULL;
	size_t n_floats = 0;
	float *f_pcm = NULL;
	ort_release_status(get_ts(out_val, &ts_info));
	if (ts_info) {
		ort_release_status(get_cnt(ts_info, &n_floats));
		rel_ts(ts_info);
	}
	ort_release_status(get_data(out_val, (void **)&f_pcm));

	int16_t *pcm16 = NULL;
	if (f_pcm && n_floats > 0) {
		/* Trim leading/trailing near-zero silence (> 0.004f) with 45ms lead-in cushion
		 * so PipeWire stream startup never clips initial consonants/vowels. */
		size_t start = 0, end = n_floats;
		while (start < end && fabsf(f_pcm[start]) < 0.004f) start++;
		while (end > start && fabsf(f_pcm[end - 1]) < 0.004f) end--;
		start = (start > 1080) ? (start - 1080) : 0;
		if (end + 1200 < n_floats) end += 1200;
		else end = n_floats;
		size_t len = (end > start) ? (end - start) : 0;
		if (len > 0) {
			pcm16 = (int16_t *)malloc(len * sizeof(int16_t));
			if (pcm16) {
				for (size_t i = 0; i < len; i++) {
					float v = f_pcm[start + i];
					if (v > 1.0f) v = 1.0f;
					else if (v < -1.0f) v = -1.0f;
					pcm16[i] = (int16_t)(v * 32767.0f);
				}
				*out_samples = len;
			}
		}
	}
	rel_val(out_val);
	return pcm16;
}

static int16_t *synthesize_espeak_cli_pcm(const char *seg_text, const char *lang_hint, size_t *out_samples, int *out_rate)
{
	*out_samples = 0;
	*out_rate = 22050;
	if (!seg_text || !*seg_text) return NULL;

	char es_voice[32];
	normalize_espeak_voice(lang_hint, es_voice, sizeof(es_voice));

	stl_proc_t proc;
	const char *argv_ng[] = { "espeak-ng", "-v", es_voice, "--stdout", seg_text, NULL };
	const char *argv_es[] = { "espeak", "-v", es_voice, "--stdout", seg_text, NULL };
	if (stl_spawn(argv_ng, STL_SPAWN_STDOUT | STL_SPAWN_NEW_GROUP, &proc) < 0) {
		if (stl_spawn(argv_es, STL_SPAWN_STDOUT | STL_SPAWN_NEW_GROUP, &proc) < 0)
			return NULL;
	}

	size_t cap = 65536, len = 0;
	uint8_t *wav = (uint8_t *)malloc(cap);
	if (wav && proc.stdout_fd >= 0) {
		for (;;) {
			if (len + 4096 > cap) {
				if (cap >= 4U * 1024U * 1024U) break;
				size_t ncap = cap * 2;
				uint8_t *nb = (uint8_t *)realloc(wav, ncap);
				if (!nb) break;
				wav = nb;
				cap = ncap;
			}
			ssize_t r = tts_fd_read(proc.stdout_fd, wav + len, cap - len);
#ifndef _WIN32
			if (r < 0 && errno == EINTR) continue;
#endif
			if (r <= 0) break;
			len += (size_t)r;
		}
	}
	if (proc.stdout_fd >= 0) {
		tts_fd_close(proc.stdout_fd);
		proc.stdout_fd = -1;
	}
	int st = 0;
	stl_proc_wait(&proc, &st, -1);
	stl_proc_close(&proc);

	int16_t *pcm16 = NULL;
	if (wav && len >= 44) {
		(void)jarvis_plat_read_wav_pcm16(wav, len, &pcm16, out_samples, out_rate);
	}
	free(wav);
	return pcm16;
}

static int16_t *synthesize_segment_pcm(const char *seg_text, const char *voice, size_t *out_samples, int *out_rate, uint64_t my_gen)
{
	*out_samples = 0;
	*out_rate = KOKORO_SAMPLE_RATE;
	if (!seg_text || !*seg_text) return NULL;

	int16_t *cached = tts_cache_lookup(voice, seg_text, out_samples, out_rate);
	if (cached) return cached;

	if (is_edge_voice_name(voice)) {
		bool is_pt = false;
		(void)resolve_edge_voice(voice, &is_pt);
		int16_t *pcm = synthesize_edge_tts(seg_text, voice, out_samples, out_rate, my_gen);
		if (pcm && *out_samples > 0) {
			tts_cache_store(voice, seg_text, pcm, *out_samples, *out_rate);
			return pcm;
		}
		if (my_gen != 0) {
			pthread_mutex_lock(&g_tts.mtx);
			bool canceled = (g_tts.cancel_gen != my_gen);
			pthread_mutex_unlock(&g_tts.mtx);
			if (canceled) return NULL;
		}
		/* Local neural ONNX fallback when Edge TTS fails or times out (never cached under edge: key) */
		char fb_voice[64];
		resolve_edge_fallback_voice(is_pt, fb_voice, sizeof(fb_voice));
		fprintf(stderr, "jarvis-tts: edge voice '%s' unavailable, using local fallback '%s'\n",
		        voice, fb_voice);
		pcm = synthesize_onnx_pcm(seg_text, fb_voice, out_samples, out_rate);
		if (pcm && *out_samples > 0) return pcm;

		/* Last-resort offline CLI / native OS TTS fallback */
		pcm = synthesize_espeak_cli_pcm(seg_text, is_pt ? "pt" : NULL, out_samples, out_rate);
		if (pcm && *out_samples > 0) return pcm;
		pcm = jarvis_plat_tts_fallback_pcm(seg_text, is_pt ? "pt" : NULL, out_samples, out_rate);
		if (pcm && *out_samples > 0) return pcm;
	} else {
		int16_t *pcm = synthesize_onnx_pcm(seg_text, voice, out_samples, out_rate);
		if (pcm && *out_samples > 0) {
			tts_cache_store(voice, seg_text, pcm, *out_samples, *out_rate);
			return pcm;
		}
		pcm = synthesize_espeak_cli_pcm(seg_text, NULL, out_samples, out_rate);
		if (pcm && *out_samples > 0) return pcm;
		pcm = jarvis_plat_tts_fallback_pcm(seg_text, NULL, out_samples, out_rate);
		if (pcm && *out_samples > 0) return pcm;
	}

	static bool s_warned_no_tts = false;
	if (!s_warned_no_tts) {
		s_warned_no_tts = true;
		fprintf(stderr,
		        "jarvis-tts: no TTS engine succeeded for voice='%s' "
		        "(check edge-tts-pcm, Piper/Kokoro models in ~/.local/share/, or espeak-ng)\n",
		        voice ? voice : "(null)");
	}
	return NULL;
}

/* ---------------------------------------------------------------------------
 * Persistent Playback Stream & Instant Barge-In Preemption
 * ------------------------------------------------------------------------- */

static bool reap_draining_player_locked(bool block)
{
	jarvis_audio_play_t *dp = g_tts.draining_player;
	if (!dp) return true;
	if (!block) {
		int done = jarvis_audio_play_poll_done(dp);
		if (done == 0) return false;
	}
	g_tts.draining_player = NULL;
	pthread_mutex_unlock(&g_tts.mtx);
	jarvis_audio_play_close(dp);
	pthread_mutex_lock(&g_tts.mtx);
	return true;
}

static void close_persistent_player_locked(bool wait_for_drain)
{
	if (!g_tts.player) return;

	if (!wait_for_drain) {
		if (g_tts.draining_player)
			reap_draining_player_locked(true);
		jarvis_audio_play_t *p = g_tts.player;
		g_tts.player = NULL;
		if (!p) return;
		jarvis_audio_play_finish(p);
		g_tts.draining_player = p;
		return;
	}
	if (g_tts.draining_player)
		reap_draining_player_locked(true);
	jarvis_audio_play_t *p = g_tts.player;
	g_tts.player = NULL;
	if (!p) return;
	pthread_mutex_unlock(&g_tts.mtx);
	jarvis_audio_play_finish(p);
	jarvis_audio_play_close(p);
	pthread_mutex_lock(&g_tts.mtx);
}

static jarvis_audio_play_t *ensure_persistent_player_locked(int sample_rate)
{
	if (g_tts.player) {
		if (jarvis_audio_play_rate(g_tts.player) == sample_rate &&
		    jarvis_audio_play_poll_done(g_tts.player) == 0) {
			return g_tts.player;
		}
		close_persistent_player_locked(true);
	}
	reap_draining_player_locked(true);
	g_tts.player = jarvis_audio_play_open(sample_rate, 1, "jarvis-tts");
	return g_tts.player;
}

/* ---------------------------------------------------------------------------
 * Background TTS Worker Thread (600s Idle Auto-Unload -> 0 KB RAM)
 * ------------------------------------------------------------------------- */

static void *tts_worker_fn(void *arg)
{
	(void)arg;
	bool mic_muted = false;
	int16_t *prefetched_pcm = NULL;
	size_t prefetched_samples = 0;
	int prefetched_rate = KOKORO_SAMPLE_RATE;
	uint64_t prefetched_gen = 0;

	pthread_mutex_lock(&g_tts.mtx);
	for (;;) {
		while (g_tts.q_count == 0 && !prefetched_pcm) {
			/* Finish write-end of active player without blocking so if another
			 * LLM clause arrives during drain we immediately synthesize it in
			 * parallel while audio finishes playing!
			 */
			if (g_tts.player) {
				close_persistent_player_locked(false);
			}
			if (g_tts.draining_player && !reap_draining_player_locked(false)) {
				struct timespec drain_ts;
				jarvis_clock_realtime(&drain_ts);
				drain_ts.tv_nsec += 25000000L;
				if (drain_ts.tv_nsec >= 1000000000L) {
					drain_ts.tv_sec += 1;
					drain_ts.tv_nsec -= 1000000000L;
				}
				pthread_cond_timedwait(&g_tts.cond, &g_tts.mtx, &drain_ts);
				continue;
			}
			bool was_active = g_tts.busy_speaking || mic_muted;
			g_tts.busy_speaking = false;
			if (was_active) {
				write_tts_active_state(NULL, false);
				mic_muted = false;
				pthread_cond_broadcast(&g_tts.done_cond);
				pthread_mutex_unlock(&g_tts.mtx);
				set_mic_mute(false);
				notify_tts_state("idle");
				pthread_mutex_lock(&g_tts.mtx);
				if (g_tts.q_count > 0 || prefetched_pcm) break;
			} else {
				pthread_cond_broadcast(&g_tts.done_cond);
			}

			/* If TTS was disabled, unload immediately without waiting */
			if (!is_tts_enabled()) {
				tts_engine_unload();
				g_tts.worker_running = false;
				pthread_mutex_unlock(&g_tts.mtx);
				return NULL;
			}

			struct timespec ts;
			jarvis_clock_realtime(&ts);
			ts.tv_sec += IDLE_UNLOAD_SECONDS;
			int rc = pthread_cond_timedwait(&g_tts.cond, &g_tts.mtx, &ts);
			if (rc == ETIMEDOUT && g_tts.q_count == 0) {
				/* Idle timeout: unload ONNX + Espeak + Voice style table -> 0 KB RAM */
				tts_engine_unload();
				g_tts.worker_running = false;
				pthread_mutex_unlock(&g_tts.mtx);
				return NULL;
			}
		}

		int16_t *pcm = NULL;
		size_t n_samples = 0;
		int sample_rate = KOKORO_SAMPLE_RATE;
		uint64_t my_gen = g_tts.cancel_gen;

		if (prefetched_pcm) {
			if (prefetched_gen == my_gen) {
				pcm = prefetched_pcm;
				n_samples = prefetched_samples;
				sample_rate = prefetched_rate;
			} else {
				free(prefetched_pcm);
			}
			prefetched_pcm = NULL;
			prefetched_samples = 0;
			g_tts.busy_speaking = true;
			pthread_mutex_unlock(&g_tts.mtx);
		} else {
			tts_segment_t seg = g_tts.queue[g_tts.q_head];
			g_tts.q_head = (g_tts.q_head + 1) % MAX_TTS_QUEUE;
			g_tts.q_count--;

			/* Speculative warmup packet ("__WARMUP__"): pre-load ONNX & eSpeak into RAM
			 * on wake-word detection without emitting audio or marking busy_speaking!
			 */
			if (strcmp(seg.text, "__WARMUP__") == 0) {
				pthread_mutex_unlock(&g_tts.mtx);
				(void)tts_engine_ensure_loaded(seg.voice);
				pthread_mutex_lock(&g_tts.mtx);
				continue;
			}

			g_tts.busy_speaking = true;
			pthread_mutex_unlock(&g_tts.mtx);

			pcm = synthesize_segment_pcm(seg.text, seg.voice, &n_samples, &sample_rate, my_gen);
		}

		if (pcm && n_samples > 0) {
			pthread_mutex_lock(&g_tts.mtx);
			bool cancelled = (g_tts.cancel_gen != my_gen);
			jarvis_audio_play_t *play = (!cancelled) ? ensure_persistent_player_locked(sample_rate) : NULL;
			pthread_mutex_unlock(&g_tts.mtx);

			if (!cancelled && play) {
				notify_tts_state("speaking");
				if (!mic_muted) {
					set_mic_mute(true);
					mic_muted = true;
				}
				size_t off_samp = 0;
				while (off_samp < n_samples) {
					pthread_mutex_lock(&g_tts.mtx);
					bool now_canc = (g_tts.cancel_gen != my_gen || g_tts.player != play);
					pthread_mutex_unlock(&g_tts.mtx);
					if (now_canc) break;

					size_t chunk_samp = n_samples - off_samp;
					if (chunk_samp > 16384) chunk_samp = 16384;
					if (jarvis_audio_play_write(play, pcm + off_samp, chunk_samp) != 0) break;
					off_samp += chunk_samp;
				}

				/* Immediately synthesize the next queued segment while PipeWire
				 * plays the 1 MB kernel pipe buffer in the background!
				 */
				pthread_mutex_lock(&g_tts.mtx);
				bool has_next = (g_tts.cancel_gen == my_gen && g_tts.q_count > 0);
				tts_segment_t next_seg;
				if (has_next) {
					next_seg = g_tts.queue[g_tts.q_head];
					g_tts.q_head = (g_tts.q_head + 1) % MAX_TTS_QUEUE;
					g_tts.q_count--;
				}
				pthread_mutex_unlock(&g_tts.mtx);

				if (has_next && strcmp(next_seg.text, "__WARMUP__") != 0) {
					prefetched_pcm = synthesize_segment_pcm(next_seg.text, next_seg.voice,
					                                        &prefetched_samples, &prefetched_rate, my_gen);
					prefetched_gen = my_gen;
				}
			}
		}
		free(pcm);

		pthread_mutex_lock(&g_tts.mtx);
	}
}

static void enqueue_segment_locked(const char *text, const char *voice)
{
	if (!text || !*text || g_tts.q_count >= MAX_TTS_QUEUE) return;
	tts_segment_t *slot = &g_tts.queue[g_tts.q_tail];
	snprintf(slot->text, sizeof(slot->text), "%s", text);
	snprintf(slot->voice, sizeof(slot->voice), "%s", voice);
	g_tts.q_tail = (g_tts.q_tail + 1) % MAX_TTS_QUEUE;
	g_tts.q_count++;
	if (strcmp(text, "__WARMUP__") != 0) {
		g_tts.busy_speaking = true;
	}

	if (!g_tts.worker_running) {
		pthread_t tid;
		if (pthread_create(&tid, NULL, tts_worker_fn, NULL) == 0) {
			pthread_detach(tid);
			g_tts.worker_running = true;
		}
	}
	pthread_cond_signal(&g_tts.cond);
}

static void split_and_enqueue_locked(const char *clean, const char *voice, bool is_first_clause)
{
	const char *p = clean;
	bool is_edge = (voice && (strncmp(voice, "edge:", 5) == 0 ||
	                          strcasecmp(voice, "raquel") == 0 ||
	                          strcasecmp(voice, "duarte") == 0));
	bool is_piper = (voice && strncmp(voice, "piper:", 6) == 0);
	bool is_neural = (is_edge || is_piper);

	if (is_neural) {
		/* Neural Voice Strategy (Edge-TTS Raquel/Duarte & Piper ONNX):
		 * Modern neural models generate natural cadence, pitch, and breath only across
		 * complete prosodic units. Never chop mid-sentence at arbitrary 38-char limits.
		 * Segment strictly on sentence terminators (. ! ? ; : or newline) up to 240 chars.
		 * If an opening phrase has an early sentence boundary (e.g. "Olá!", "Com certeza!"),
		 * pop it immediately for ultra-fast Time-To-First-Audio while synthesizing the remainder
		 * in parallel in the background!
		 */
		while (*p) {
			while (*p && isspace((unsigned char)*p)) p++;
			if (!*p) break;

			size_t rem = strlen(p);
			if (rem <= 240) {
				size_t sent_cut = 0;
				for (size_t i = 3; i < rem; i++) {
					if (strchr(".!?;:\n", p[i]) && (isspace((unsigned char)p[i + 1]) || p[i + 1] == '\0')) {
						sent_cut = i + 1;
						break;
					}
				}
				if (sent_cut > 0 && sent_cut < rem) {
					char seg[512];
					if (sent_cut >= sizeof(seg)) sent_cut = sizeof(seg) - 1;
					memcpy(seg, p, sent_cut);
					seg[sent_cut] = '\0';
					trim_trailing(seg);
					if (seg[0]) enqueue_segment_locked(seg, voice);
					p += sent_cut;
					continue;
				}
				enqueue_segment_locked(p, voice);
				break;
			}

			/* Longer than 240 chars: find best sentence boundary between 16 and 240 */
			size_t cut = 0;
			for (size_t i = 16; i < 240 && p[i]; i++) {
				if (strchr(".!?;:\n", p[i]) && (isspace((unsigned char)p[i + 1]) || p[i + 1] == '\0')) {
					cut = i + 1;
					break;
				}
			}
			/* Fallback to comma boundary between 50 and 200 */
			if (!cut) {
				for (size_t i = 50; i < 200 && p[i]; i++) {
					if (p[i] == ',' && (isspace((unsigned char)p[i + 1]) || p[i + 1] == '\0')) {
						cut = i + 1;
						break;
					}
				}
			}
			/* Fallback to word boundary between 80 and 200 */
			if (!cut) {
				for (size_t i = 200; i > 60; i--) {
					if (p[i] == ' ') {
						cut = i;
						break;
					}
				}
			}
			if (!cut) cut = (rem < 240) ? rem : 240;

			char seg[512];
			if (cut >= sizeof(seg)) cut = sizeof(seg) - 1;
			memcpy(seg, p, cut);
			seg[cut] = '\0';
			trim_trailing(seg);
			if (seg[0]) enqueue_segment_locked(seg, voice);
			p += cut;
		}
		return;
	}

	bool first_seg = is_first_clause;
	while (*p) {
		while (*p && isspace((unsigned char)*p)) p++;
		if (!*p) break;

		/* Micro-First-Chunk Strategy for CPU Kokoro-82M */
		size_t max_len = first_seg ? 38 : 72;
		size_t rem = strlen(p);
		if (rem <= max_len) {
			enqueue_segment_locked(p, voice);
			break;
		}

		size_t scan_lim = first_seg ? (max_len + 10) : (max_len + 16);
		size_t min_cut = first_seg ? 14 : 22;
		size_t cut = 0;
		for (size_t i = min_cut; i < scan_lim && p[i]; i++) {
			if (strchr(".!?;:", p[i]) && (p[i + 1] == ' ' || p[i + 1] == '\0')) {
				cut = i + 1;
				break;
			}
		}
		/* 2. Comma boundary */
		if (!cut) {
			size_t clim = (rem < scan_lim) ? rem : scan_lim;
			size_t min_comma = first_seg ? 14 : 20;
			for (size_t i = min_comma; i < clim && p[i]; i++) {
				if (p[i] == ',' && p[i + 1] == ' ') {
					cut = i + 1;
					break;
				}
			}
		}
		/* 3. Word boundary near max_len */
		if (!cut) {
			size_t min_word = first_seg ? 16 : 24;
			for (size_t i = max_len; i > min_word; i--) {
				if (p[i] == ' ') {
					cut = i;
					break;
				}
			}
		}
		if (!cut) cut = max_len;

		char seg[512];
		if (cut >= sizeof(seg)) cut = sizeof(seg) - 1;
		memcpy(seg, p, cut);
		seg[cut] = '\0';
		trim_trailing(seg);
		if (seg[0]) enqueue_segment_locked(seg, voice);
		p += cut;
		first_seg = false;
	}
}

/* ---------------------------------------------------------------------------
 * Public API: jarvis_tts_speak_clause & jarvis_tts_interrupt
 * ------------------------------------------------------------------------- */

int
jarvis_tts_interrupt(int dummy)
{
	(void)dummy;
	pthread_mutex_lock(&g_tts.mtx);
	g_tts.cancel_gen++;
	g_tts.q_head = 0;
	g_tts.q_tail = 0;
	g_tts.q_count = 0;
	g_tts.busy_speaking = false;
	if (g_tts.player) {
		jarvis_audio_play_abort(g_tts.player);
	}
	if (g_tts.draining_player) {
		jarvis_audio_play_abort(g_tts.draining_player);
	}
	if (g_tts.edge_busy && g_tts.has_edge) {
		stl_proc_kill(&g_tts.edge_proc, 1);
	}
	write_tts_active_state(NULL, false);
	pthread_cond_broadcast(&g_tts.cond);
	pthread_cond_broadcast(&g_tts.done_cond);
	pthread_mutex_unlock(&g_tts.mtx);
	notify_tts_state("idle");
	return 0;
}

int
jarvis_tts_speak_clause(const char *session_id, const char *clause_text, int is_final)
{
	if (!is_tts_enabled()) {
		/* Ensure any loaded TTS resources are released if user disabled TTS */
		jarvis_tts_interrupt(0);
		return 0;
	}

	char voice[64];
	get_tts_voice(voice, sizeof(voice));

	/* Speculative warmup on wake-word confirmation: pre-warm ONNX & eSpeak in the
	 * background worker thread before the user even finishes speaking their command!
	 */
	if (session_id && strcmp(session_id, "warmup") == 0) {
		pthread_mutex_lock(&g_tts.mtx);
		if (g_tts.q_count == 0 && !g_tts.busy_speaking) {
			enqueue_segment_locked("__WARMUP__", voice);
		}
		pthread_mutex_unlock(&g_tts.mtx);
		return 0;
	}

	char clean[1024];
	clean_speech_text(clause_text, clean, sizeof(clean));

	pthread_mutex_lock(&g_tts.mtx);
	if (clean[0]) {
		bool first_clause = (g_tts.q_count == 0 && !g_tts.busy_speaking);
		write_tts_active_state(clean, true);
		split_and_enqueue_locked(clean, voice, first_clause);
	}
	/* Only block synchronously if JARVIS_TTS_SYNC=1 is explicitly set (e.g. CLI test harness);
	 * in daemon mode, return immediately so dwm-whisperd immediately re-enters its listening
	 * loop while the background TTS worker synthesizes and speaks!
	 */
	const char *sync_env = getenv("JARVIS_TTS_SYNC");
	bool wait_sync = (is_final >= 2) || (is_final && sync_env && strcmp(sync_env, "1") == 0);
	if (wait_sync && g_tts.worker_running) {
		uint64_t wait_gen = g_tts.cancel_gen;
		struct timespec ts;
		jarvis_clock_realtime(&ts);
		ts.tv_sec += 45;
		while ((g_tts.q_count > 0 || g_tts.busy_speaking) && g_tts.cancel_gen == wait_gen) {
			if (pthread_cond_timedwait(&g_tts.done_cond, &g_tts.mtx, &ts) == ETIMEDOUT) {
				break;
			}
		}
	}
	pthread_mutex_unlock(&g_tts.mtx);
	return 0;
}
