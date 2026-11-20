#include "../include/ttypt/jarvis.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

static void print_usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options] <utterance | subcommand>\n\n"
		"Subcommands:\n"
		"  query <utterance>                         Resolve and execute natural language command\n"
		"  dmenu | omni                              Open interactive command bar via jarvis_xy_prompt\n"
		"  warmup                                    Pre-warm ontology and inference engine\n"
		"  llm-mode [auto|cloud|embedding|off]       Get or set persistent inference mode\n"
		"  clear-learned                             Clear context, session, and embedding caches\n\n"
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

static void get_mode_file_path(char *out, size_t out_sz)
{
	const char *home = getenv("HOME");
	if (!home || !*home) home = "/tmp";
	snprintf(out, out_sz, "%s/.config/dwm/llm-mode", home);
}

static void read_saved_llm_mode(char *out, size_t out_sz)
{
	snprintf(out, out_sz, "auto");
	char path[512];
	get_mode_file_path(path, sizeof(path));
	FILE *fp = fopen(path, "r");
	if (!fp) {
		const char *home = getenv("HOME");
		if (home && *home) {
			snprintf(path, sizeof(path), "%s/.config/jarvis/llm-mode", home);
			fp = fopen(path, "r");
		}
	}
	if (!fp) return;
	char buf[64];
	if (fgets(buf, sizeof(buf), fp)) {
		size_t l = strlen(buf);
		while (l > 0 && isspace((unsigned char)buf[l - 1])) buf[--l] = '\0';
		if (l > 0) snprintf(out, out_sz, "%s", buf);
	}
	fclose(fp);
}

static int write_saved_llm_mode(const char *mode, bool quiet)
{
	static const char *const valid[] = {"auto", "cloud", "fast", "smart", "llm", "embedding", "off", "none", NULL};
	bool ok = false;
	for (size_t i = 0; valid[i]; i++) {
		if (strcmp(mode, valid[i]) == 0) {
			ok = true;
			break;
		}
	}
	if (!ok) {
		fprintf(stderr, "Usage: jarvis llm-mode [auto|cloud|fast|smart|embedding|off|none]\n");
		return 1;
	}
	const char *home = getenv("HOME");
	if (home && *home) {
#ifndef _WIN32
		char dir1[512], dir2[512];
		snprintf(dir1, sizeof(dir1), "%s/.config/dwm", home);
		snprintf(dir2, sizeof(dir2), "%s/.config/jarvis", home);
		mkdir(dir1, 0755);
		mkdir(dir2, 0755);
#endif
		char p1[512], p2[512];
		snprintf(p1, sizeof(p1), "%s/.config/dwm/llm-mode", home);
		snprintf(p2, sizeof(p2), "%s/.config/jarvis/llm-mode", home);
		FILE *f1 = fopen(p1, "w");
		if (f1) { fprintf(f1, "%s\n", mode); fclose(f1); }
		FILE *f2 = fopen(p2, "w");
		if (f2) { fprintf(f2, "%s\n", mode); fclose(f2); }
	}
	if (!quiet) {
		char body[128];
		snprintf(body, sizeof(body), "Mode set to: %s", mode);
		jarvis_xy_notify("  Jarvis LLM Mode", body, 0);
	}
	printf("LLM mode set to %s\n", mode);
	return 0;
}

static jarvis_infer_mode_t parse_infer_mode(const char *im)
{
	if (!im || !*im) return JARVIS_INFER_AUTO;
	if (strcmp(im, "none") == 0 || strcmp(im, "off") == 0) return JARVIS_INFER_NONE;
	if (strcmp(im, "embedding") == 0) return JARVIS_INFER_EMBEDDING;
	if (strcmp(im, "llm") == 0 || strcmp(im, "cloud") == 0 ||
	    strcmp(im, "fast") == 0 || strcmp(im, "smart") == 0) return JARVIS_INFER_LLM;
	return JARVIS_INFER_AUTO;
}

static int load_xy_mod(const char *path)
{
	if (!path || !*path) return -1;
	char clean[512];
	snprintf(clean, sizeof(clean), "%s", path);
	size_t len = strlen(clean);
	if (len > 3 && strcmp(clean + len - 3, ".so") == 0) {
		clean[len - 3] = '\0';
	}
	return xy_load(clean);
}

static void autoload_xylem_modules(const char *wm)
{
	const char *home = getenv("HOME");

	/* 1. Load modules from $JARVIS_MODS (colon or comma separated) */
	const char *env_mods = getenv("JARVIS_MODS");
	if (env_mods && *env_mods) {
		char buf[1024];
		snprintf(buf, sizeof(buf), "%s", env_mods);
		char *saveptr = NULL;
		for (char *tok = strtok_r(buf, ":, ", &saveptr); tok; tok = strtok_r(NULL, ":, ", &saveptr)) {
			if (*tok) load_xy_mod(tok);
		}
	}

	/* 2. Load modules from ~/.config/jarvis/mods.load if present */
	if (home && *home) {
		char mpath[512];
		snprintf(mpath, sizeof(mpath), "%s/.config/jarvis/mods.load", home);
		FILE *fp = fopen(mpath, "r");
		if (fp) {
			char line[512];
			while (fgets(line, sizeof(line), fp)) {
				char *p = line;
				while (*p && isspace((unsigned char)*p)) p++;
				if (!*p || *p == '#') continue;
				size_t l = strlen(p);
				while (l > 0 && isspace((unsigned char)p[l - 1])) p[--l] = '\0';
				if (l > 0) load_xy_mod(p);
			}
			fclose(fp);
		}
	}

	/* 3. Auto-load mod_<wm> (e.g. ~/.local/lib/mod_dwm) and mod_tts */
	if (wm && *wm) {
		char cand[512];
		if (home && *home) {
			snprintf(cand, sizeof(cand), "%s/.local/lib/mod_%s", home, wm);
			if (load_xy_mod(cand) != XY_OK) {
				snprintf(cand, sizeof(cand), "mod_%s", wm);
				load_xy_mod(cand);
			}
		} else {
			snprintf(cand, sizeof(cand), "mod_%s", wm);
			load_xy_mod(cand);
		}
	}
	if (home && *home) {
		char tts_cand[512];
		snprintf(tts_cand, sizeof(tts_cand), "%s/.local/lib/mod_tts", home);
		load_xy_mod(tts_cand);
	}
}

static void build_prompt_candidates(const char *fs_path, char *out, size_t out_sz)
{
	out[0] = '\0';
	if (!fs_path || !*fs_path) return;
	FILE *fp = fopen(fs_path, "rb");
	if (!fp) return;
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz <= 0 || sz > 256 * 1024) {
		fclose(fp);
		return;
	}
	char *buf = (char *)malloc((size_t)sz + 1);
	if (!buf) {
		fclose(fp);
		return;
	}
	size_t rd = fread(buf, 1, (size_t)sz, fp);
	buf[rd] = '\0';
	fclose(fp);

	size_t pos = 0;
	size_t count = 0;
	const char *p = buf;
	while ((p = strstr(p, "\"utterance\"")) != NULL && count < 16) {
		p = strchr(p + 11, '"');
		if (!p) break;
		p++;
		const char *u_end = strchr(p, '"');
		if (!u_end) break;
		size_t u_len = (size_t)(u_end - p);
		const char *op_p = strstr(u_end, "\"op\"");
		if (!op_p) break;
		op_p = strchr(op_p + 4, '"');
		if (!op_p) break;
		op_p++;
		const char *op_end = strchr(op_p, '"');
		if (!op_end) break;
		size_t op_len = (size_t)(op_end - op_p);
		if (u_len > 0 && u_len < 120 && op_len > 0 && op_len < 64) {
			pos += (size_t)snprintf(out + pos, pos < out_sz ? out_sz - pos : 0,
			                        "%s%-38.*s ▸ %.*s",
			                        count ? "\n" : "",
			                        (int)u_len, p,
			                        (int)op_len, op_p);
			count++;
		}
		p = op_end + 1;
	}
	free(buf);
}

int main(int argc, char **argv)
{
	jarvis_config_t cfg;
	jarvis_config_init(&cfg);
	const char *env_wm = getenv("JARVIS_WM");
	if (env_wm && *env_wm) cfg.wm = env_wm;

	const char *surface = "any";
	bool as_json = false;
	bool quiet = false;
	bool cli_infer_set = false;
	char utterance[512] = {0};
	size_t u_pos = 0;
	const char *subcmd = NULL;
	const char *subarg = NULL;

	const char *home = getenv("HOME");
	const char *user = getenv("USER");
	if (!user || !*user) user = "user";

	char ctx_path[512] = {0}, sess_path[512] = {0}, mem_path[512] = {0}, fs_path[512] = {0}, emb_path[512] = {0};
	snprintf(ctx_path, sizeof(ctx_path), "/tmp/jarvis-%s.context.json", user);
	snprintf(sess_path, sizeof(sess_path), "/tmp/jarvis-%s.session.json", user);
	cfg.context_path = ctx_path;
	cfg.session_path = sess_path;
	if (home && *home) {
		snprintf(mem_path, sizeof(mem_path), "%s/.config/jarvis/memories.json", home);
		snprintf(fs_path, sizeof(fs_path), "%s/.config/jarvis/few_shots.json", home);
		snprintf(emb_path, sizeof(emb_path), "%s/.cache/jarvis/embeddings.json", home);
		cfg.memory_path = mem_path;
		cfg.few_shot_path = fs_path;
		cfg.embed_cache_path = emb_path;
	}

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
			cfg.infer_mode = parse_infer_mode(argv[++i]);
			cli_infer_set = true;
			continue;
		}
		if (strcmp(argv[i], "--surface") == 0 && i + 1 < argc) {
			surface = argv[++i];
			continue;
		}
		if (strcmp(argv[i], "--mod") == 0 && i + 1 < argc) {
			load_xy_mod(argv[++i]);
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
			quiet = true;
			continue;
		}
		if (u_pos == 0 && !subcmd) {
			if (strcmp(argv[i], "dmenu") == 0 || strcmp(argv[i], "omni") == 0 ||
			    strcmp(argv[i], "warmup") == 0 || strcmp(argv[i], "clear-learned") == 0 ||
			    strcmp(argv[i], "tts-interrupt") == 0 || strcmp(argv[i], "speak") == 0) {
				subcmd = argv[i];
				continue;
			}
			if (strcmp(argv[i], "llm-mode") == 0) {
				subcmd = "llm-mode";
				if (i + 1 < argc) subarg = argv[++i];
				continue;
			}
			if (strcmp(argv[i], "query") == 0 || strcmp(argv[i], "exec") == 0) {
				continue;
			}
			if (strcmp(argv[i], "dry-run") == 0) {
				cfg.dry_run = true;
				continue;
			}
		}
		u_pos += (size_t)snprintf(utterance + u_pos,
		                          u_pos < sizeof(utterance) ? sizeof(utterance) - u_pos : 0,
		                          "%s%s", u_pos ? " " : "", argv[i]);
	}

	if (!cli_infer_set) {
		char saved_mode[64];
		read_saved_llm_mode(saved_mode, sizeof(saved_mode));
		cfg.infer_mode = parse_infer_mode(saved_mode);
	}

	/* Auto-load configured or environment-matched Xylem adapter modules */
	autoload_xylem_modules(cfg.wm);

	if (subcmd) {
		if (strcmp(subcmd, "tts-interrupt") == 0) {
			jarvis_xy_tts_interrupt(0);
			return 0;
		}
		if (strcmp(subcmd, "speak") == 0) {
			if (utterance[0]) {
				jarvis_xy_speak_clause("cli", utterance, 1);
			}
			return 0;
		}
		if (strcmp(subcmd, "llm-mode") == 0) {
			if (!subarg) {
				char saved[64];
				read_saved_llm_mode(saved, sizeof(saved));
				printf("%s\n", saved);
				return 0;
			}
			return write_saved_llm_mode(subarg, quiet);
		}
		if (strcmp(subcmd, "clear-learned") == 0) {
#ifndef _WIN32
			if (ctx_path[0]) unlink(ctx_path);
			if (sess_path[0]) unlink(sess_path);
			if (emb_path[0]) unlink(emb_path);
#endif
			if (!quiet) {
				jarvis_xy_notify("  Jarvis Learned Cache", "Cleared embedding & context caches", 0);
			}
			printf("Cleared Jarvis caches\n");
			return 0;
		}
		if (strcmp(subcmd, "warmup") == 0) {
			jarvis_engine_t *w_eng = jarvis_engine_create(&cfg);
			jarvis_engine_destroy(w_eng);
			return 0;
		} else if (strcmp(subcmd, "dmenu") == 0 || strcmp(subcmd, "omni") == 0) {
			char candidates[4096];
			build_prompt_candidates(fs_path, candidates, sizeof(candidates));
			char selection[512] = {0};
			if (jarvis_xy_prompt(" Jarvis:", candidates, selection, sizeof(selection)) <= 0 || !selection[0]) {
				return 0;
			}
			char *arrow = strstr(selection, "▸");
			if (arrow) {
				*arrow = '\0';
			}
			size_t slen = strlen(selection);
			while (slen > 0 && isspace((unsigned char)selection[slen - 1])) selection[--slen] = '\0';
			if (!selection[0]) return 0;
			snprintf(utterance, sizeof(utterance), "%s", selection);
		}
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

	if (cfg.dry_run) {
		char jbuf[8192];
		jarvis_process_result_to_json(&res, jbuf, sizeof(jbuf));
		if (as_json) {
			printf("%s\n", jbuf);
		} else if (!quiet) {
			printf("DRY_RUN: %s\n", jbuf);
		}
		jarvis_engine_destroy(eng);
		return ok ? 0 : 1;
	}

	if (as_json) {
		char jbuf[8192];
		jarvis_process_result_to_json(&res, jbuf, sizeof(jbuf));
		printf("%s\n", jbuf);
	} else if (!ok) {
		if (!quiet) {
			char body[560];
			snprintf(body, sizeof(body), "“%s”", utterance);
			jarvis_xy_notify("  Jarvis — Unresolved", body, 0);
		}
		printf("Error: %s\n", res.error[0] ? res.error : "unresolved");
	} else {
		const char *out_msg = res.combined_result.output[0] ? res.combined_result.output : res.combined_result.error;
		if (!quiet && strcmp(res.last_dsl.op, "answer_question") != 0) {
			char title[256], body[1024];
			snprintf(title, sizeof(title), "  Jarvis → %s (%s)",
			         res.last_dsl.op[0] ? res.last_dsl.op : "action",
			         res.source[0] ? res.source : "solver");
			snprintf(body, sizeof(body), "“%.300s”\n▸ %.680s", utterance, out_msg);
			jarvis_xy_notify(title, body, res.combined_result.success ? 1 : 0);
		}
		printf("[%s] %s: %s\n",
		       res.combined_result.success ? "OK" : "FAIL",
		       res.source,
		       out_msg);
	}

	jarvis_engine_destroy(eng);
	return (ok && res.combined_result.success) ? 0 : 1;
}

