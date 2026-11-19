#include "../include/ttypt/jarvis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options] <utterance | subcommand>\n\n"
		"Options:\n"
		"  --wm <dwm|sway|hyprland>                  Window manager / environment (default: dwm)\n"
		"  --inference <auto|llm|embedding|none>     Inference backend (default: auto)\n"
		"  --surface <any|terminal|browser|site|nd>  Active surface context (default: any)\n"
		"  --mod <path>                              Load additional libxylem (.so) adapter module\n"
		"  --dry-run                                 Compile utterance to IR/DSL without executing\n"
		"  --json                                    Emit JSON output\n"
		"  --quiet                                   Suppress desktop notifications / earcons\n",
		prog
	);
}

int main(int argc, char **argv)
{
	jarvis_config_t cfg;
	jarvis_config_init(&cfg);
	const char *surface = "any";
	bool as_json = false;
	char utterance[512] = {0};
	size_t u_pos = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			print_usage(argv[0]);
			return 0;
		}
		if (strcmp(argv[i], "--wm") == 0 && i + 1 < argc) {
			cfg.wm = argv[++i];
			continue;
		}
		if (strcmp(argv[i], "--inference") == 0 && i + 1 < argc) {
			const char *im = argv[++i];
			if (strcmp(im, "none") == 0 || strcmp(im, "off") == 0) cfg.infer_mode = JARVIS_INFER_NONE;
			else if (strcmp(im, "embedding") == 0) cfg.infer_mode = JARVIS_INFER_EMBEDDING;
			else if (strcmp(im, "llm") == 0) cfg.infer_mode = JARVIS_INFER_LLM;
			else cfg.infer_mode = JARVIS_INFER_AUTO;
			continue;
		}
		if (strcmp(argv[i], "--surface") == 0 && i + 1 < argc) {
			surface = argv[++i];
			continue;
		}
		if (strcmp(argv[i], "--mod") == 0 && i + 1 < argc) {
			xy_load(argv[++i]);
			continue;
		}
		if (strcmp(argv[i], "--dry-run") == 0) {
			cfg.dry_run = true;
			continue;
		}
		if (strcmp(argv[i], "--json") == 0) {
			as_json = true;
			continue;
		}
		if (strcmp(argv[i], "--quiet") == 0) {
			continue;
		}
		if (strcmp(argv[i], "query") == 0 || strcmp(argv[i], "exec") == 0) {
			continue;
		}
		if (strcmp(argv[i], "dry-run") == 0 && u_pos == 0) {
			cfg.dry_run = true;
			continue;
		}
		u_pos += (size_t)snprintf(utterance + u_pos,
		                          u_pos < sizeof(utterance) ? sizeof(utterance) - u_pos : 0,
		                          "%s%s", u_pos ? " " : "", argv[i]);
	}

	if (!utterance[0]) {
		print_usage(argv[0]);
		return 1;
	}

	jarvis_engine_t *eng = jarvis_engine_create(&cfg);
	if (!eng) {
		fprintf(stderr, "Error: failed to initialize jarvis engine\n");
		return 1;
	}

	jarvis_process_result_t res;
	bool ok = jarvis_process(eng, utterance, surface, &res);

	if (as_json) {
		char jbuf[8192];
		jarvis_process_result_to_json(&res, jbuf, sizeof(jbuf));
		printf("%s\n", jbuf);
	} else if (!ok) {
		printf("Error: %s\n", res.error[0] ? res.error : "unresolved");
	} else {
		printf("[%s] %s: %s\n",
		       res.combined_result.success ? "OK" : "FAIL",
		       res.source,
		       res.combined_result.output[0] ? res.combined_result.output : res.combined_result.error);
	}

	jarvis_engine_destroy(eng);
	return (ok && res.combined_result.success) ? 0 : 1;
}
