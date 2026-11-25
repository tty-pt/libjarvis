#define _GNU_SOURCE
#include "jarvis-ipc-srv.h"
#include "jarvis-platform.h"
#include "../include/ttypt/jarvis.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <ctype.h>

static bool
find_parakeet_model(char *out, size_t out_sz)
{
	if (!out || out_sz == 0)
		return false;
	out[0] = '\0';

	if (jarvis_rc_get("parakeet_model", NULL, out, out_sz) &&
	    out[0] && jarvis_plat_is_readable(out))
		return true;
	out[0] = '\0';

	static const char *const pk_cands[] = {
		"ggml-parakeet-tdt-0.6b-v3-q8_0.bin",
		"ggml-parakeet-tdt-0.6b-v3-q5_0.bin",
		"ggml-parakeet-tdt-0.6b-v3.bin",
		"parakeet-tdt-0.6b-v3-q8_0.bin",
		"parakeet-tdt-0.6b-v3-q5_0.bin",
		"parakeet-tdt-0.6b-v3.bin",
		NULL
	};
	static const char *const dirs[] = { "jarvis/models", "whisper", NULL };

	for (size_t d = 0; dirs[d]; d++) {
		for (size_t i = 0; pk_cands[i]; i++) {
			char cand[512];
			if (jarvis_plat_data_file(dirs[d], pk_cands[i], cand, sizeof(cand)) == 0 &&
			    jarvis_plat_is_readable(cand)) {
				snprintf(out, out_sz, "%s", cand);
				return true;
			}
		}
	}
	return false;
}

static bool
find_per_locale_whisper_model(const char *raw_lang, const char *eff_lang, char *out, size_t out_sz)
{
	const char *tags[2] = { raw_lang, eff_lang };
	for (int t = 0; t < 2; t++) {
		const char *tag = tags[t];
		if (!tag || !*tag || strcasecmp(tag, "auto") == 0)
			continue;
		char key[64];
		size_t k = 0;
		snprintf(key, sizeof(key), "whisper_model.");
		k = strlen(key);
		for (size_t i = 0; tag[i] && k + 1 < sizeof(key); i++) {
			char c = (char)tolower((unsigned char)tag[i]);
			if (c == '_') c = '-';
			key[k++] = c;
		}
		key[k] = '\0';
		if (jarvis_rc_get(key, NULL, out, out_sz) && out[0] && jarvis_plat_is_readable(out))
			return true;
	}
	out[0] = '\0';
	return false;
}

bool
jarvis_is_english_only_whisper_model(const char *path)
{
	if (!path || !*path) return false;
	return strstr(path, ".en.") != NULL || strstr(path, ".en-") != NULL;
}

void
jarvis_resolve_stt_models(jarvis_hw_tier_t tier,
                          char *main_out, size_t main_sz,
                          char *wake_out, size_t wake_sz)
{
	char eff_lang[32] = {0};
	char raw_lang[32] = {0};
	char stt_backend[32] = {0};

	jarvis_resolve_lang(NULL, eff_lang, sizeof(eff_lang));
	jarvis_rc_get("lang", "", raw_lang, sizeof(raw_lang));
	jarvis_rc_get("stt_backend", "auto", stt_backend, sizeof(stt_backend));

	bool allow_en_only = (strncasecmp(eff_lang, "en", 2) == 0);
	bool want_parakeet = (strcasecmp(stt_backend, "parakeet") == 0);
	bool pin_whisper   = (strcasecmp(stt_backend, "whisper") == 0);
	bool pk_lang_ok    = jarvis_stt_parakeet_supports_lang(eff_lang);

	main_out[0] = '\0';
	bool have_main = false;

	/* 1. Explicit per-locale whisper_model.<lang> override (e.g. whisper_model.pt-pt) */
	if (!want_parakeet && find_per_locale_whisper_model(raw_lang, eff_lang, main_out, main_sz)) {
		have_main = true;
	}

	/* 2. Explicit whisper_model override (unless user explicitly requested stt_backend=parakeet) */
	if (!have_main && !want_parakeet) {
		have_main = jarvis_rc_get("whisper_model", NULL, main_out, main_sz) &&
		            main_out[0] && jarvis_plat_is_readable(main_out);
	}

	if (have_main && !allow_en_only && jarvis_is_english_only_whisper_model(main_out)) {
		fprintf(stderr,
		        "jarvisd: WARNING: configured whisper_model '%s' is English-only for lang='%s'; "
		        "falling back to multilingual model\n",
		        main_out, eff_lang);
		have_main = false;
		main_out[0] = '\0';
	}

	/* 3. Parakeet-TDT-0.6B-v3 auto-selection when installed & language is supported */
	if (!have_main && !pin_whisper && pk_lang_ok) {
		if (find_parakeet_model(main_out, main_sz))
			have_main = true;
	}

	/* 4. Tier-aware Whisper fallback ranking */
	if (!have_main) {
		static const char *const fast_gpu_cands[] = {
			"ggml-large-v3-turbo-q8_0.bin",
			"ggml-large-v3-turbo.bin",
			"ggml-large-v3-turbo-q5_0.bin",
			"ggml-large-v3-q8_0.bin",
			"ggml-large-v3-q5_0.bin",
			"ggml-large-v3.bin",
			"ggml-small-q8_0.bin",
			"ggml-small-q5_1.bin",
			"ggml-base-q8_0.bin",
			"ggml-base-q5_1.bin",
			"ggml-base.bin",
			NULL
		};
		static const char *const igpu_cands[] = {
			"ggml-large-v3-turbo-q5_0.bin",
			"ggml-large-v3-turbo-q8_0.bin",
			"ggml-large-v3-turbo.bin",
			"ggml-large-v3-q5_0.bin",
			"ggml-large-v3.bin",
			"ggml-small-q8_0.bin",
			"ggml-small-q5_1.bin",
			"ggml-base-q8_0.bin",
			"ggml-base-q5_1.bin",
			"ggml-base.bin",
			NULL
		};
		static const char *const cpu_cands[] = {
			"ggml-small-q8_0.bin",
			"ggml-small-q5_1.bin",
			"ggml-large-v3-turbo-q5_0.bin",
			"ggml-large-v3-turbo-q8_0.bin",
			"ggml-base-q8_0.bin",
			"ggml-base-q5_1.bin",
			"ggml-base.bin",
			NULL
		};
		static const char *const en_cands[] = {
			"ggml-base.en-q8_0.bin",
			"ggml-small.en-q5_1.bin",
			NULL
		};

		const char *const *cands = (tier == JARVIS_HW_FAST_GPU) ? fast_gpu_cands
		                         : (tier == JARVIS_HW_CPU)      ? cpu_cands
		                                                        : igpu_cands;

		main_out[0] = '\0';
		for (size_t i = 0; cands[i]; i++) {
			char cand[512];
			if (jarvis_plat_data_file("whisper", cands[i], cand, sizeof(cand)) == 0 &&
			    jarvis_plat_is_readable(cand)) {
				snprintf(main_out, main_sz, "%s", cand);
				break;
			}
		}
		if (!main_out[0] && allow_en_only) {
			for (size_t i = 0; en_cands[i]; i++) {
				char cand[512];
				if (jarvis_plat_data_file("whisper", en_cands[i], cand, sizeof(cand)) == 0 &&
				    jarvis_plat_is_readable(cand)) {
					snprintf(main_out, main_sz, "%s", cand);
					break;
				}
			}
		}
	}

	bool have_wake = jarvis_rc_get("whisper_wake_model", NULL, wake_out, wake_sz) &&
	                 wake_out[0] && jarvis_plat_is_readable(wake_out);
	if (have_wake && !allow_en_only && jarvis_is_english_only_whisper_model(wake_out)) {
		fprintf(stderr,
		        "jarvisd: WARNING: configured whisper_wake_model '%s' is English-only for lang='%s'; "
		        "falling back to multilingual model\n",
		        wake_out, eff_lang);
		have_wake = false;
		wake_out[0] = '\0';
	}

	if (!have_wake) {
		static const char *const multi_wake_cands[] = {
			"ggml-base-q8_0.bin",
			"ggml-base-q5_1.bin",
			"ggml-base.bin",
			"ggml-tiny-q8_0.bin",
			"ggml-tiny-q5_1.bin",
			"ggml-tiny.bin",
			NULL
		};
		static const char *const en_wake_cands[] = {
			"ggml-tiny.en-q8_0.bin",
			"ggml-tiny.en-q5_1.bin",
			"ggml-base.en-q8_0.bin",
			NULL
		};
		wake_out[0] = '\0';
		for (size_t i = 0; multi_wake_cands[i]; i++) {
			char cand[512];
			if (jarvis_plat_data_file("whisper", multi_wake_cands[i], cand, sizeof(cand)) == 0 &&
			    jarvis_plat_is_readable(cand)) {
				snprintf(wake_out, wake_sz, "%s", cand);
				break;
			}
		}
		if (!wake_out[0] && allow_en_only) {
			for (size_t i = 0; en_wake_cands[i]; i++) {
				char cand[512];
				if (jarvis_plat_data_file("whisper", en_wake_cands[i], cand, sizeof(cand)) == 0 &&
				    jarvis_plat_is_readable(cand)) {
					snprintf(wake_out, wake_sz, "%s", cand);
					break;
				}
			}
		}
	}
}

void
jarvis_resolve_default_whisper_models(char *main_out, size_t main_sz, char *wake_out, size_t wake_sz)
{
	jarvis_resolve_stt_models(JARVIS_HW_IGPU, main_out, main_sz, wake_out, wake_sz);
}
