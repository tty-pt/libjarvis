#define _GNU_SOURCE
#include "jarvis-ipc-srv.h"
#include "jarvis-platform.h"
#include "../include/ttypt/jarvis.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool
jarvis_is_english_only_whisper_model(const char *path)
{
	if (!path || !*path) return false;
	return strstr(path, ".en.") != NULL || strstr(path, ".en-") != NULL;
}

void
jarvis_resolve_default_whisper_models(char *main_out, size_t main_sz, char *wake_out, size_t wake_sz)
{
	char eff_lang[32] = {0};
	jarvis_resolve_lang(NULL, eff_lang, sizeof(eff_lang));
	bool allow_en_only = (strncasecmp(eff_lang, "en", 2) == 0);

	bool have_main = jarvis_rc_get("whisper_model", NULL, main_out, main_sz) &&
	                 main_out[0] && jarvis_plat_is_readable(main_out);
	if (have_main && !allow_en_only && jarvis_is_english_only_whisper_model(main_out)) {
		fprintf(stderr,
		        "jarvisd: WARNING: configured whisper_model '%s' is English-only for lang='%s'; "
		        "falling back to multilingual model\n",
		        main_out, eff_lang);
		have_main = false;
		main_out[0] = '\0';
	}

	if (!have_main) {
		static const char *const multi_cands[] = {
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
		static const char *const en_cands[] = {
			"ggml-base.en-q8_0.bin",
			"ggml-small.en-q5_1.bin",
			NULL
		};
		main_out[0] = '\0';
		for (size_t i = 0; multi_cands[i]; i++) {
			char cand[512];
			if (jarvis_plat_data_file("whisper", multi_cands[i], cand, sizeof(cand)) == 0 &&
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
