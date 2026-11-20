#include "../include/ttypt/jarvis.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static void print_usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options] <utterance | subcommand>\n\n"
		"Subcommands:\n"
		"  query <utterance>                         Resolve and execute natural language command\n"
		"  start | on                                Start jarvisd daemon and enable wake-word listener\n"
		"  stop | off                                Disable wake-word listener\n"
		"  toggle                                    Toggle wake-word listener on/off\n"
		"  status                                    Print listener status ('on' or 'off')\n"
		"  lockdown | lock                           Disable listener and lock screen\n"
		"  menu                                      Open interactive voice & daemon settings menu\n"
		"  config [key] [value]                      Get or set ~/.jarvisrc configuration values\n"
		"  tts-on [voice]                            Enable neural TTS (optionally setting voice)\n"
		"  tts-off                                   Disable neural TTS (0 KB idle RAM)\n"
		"  tts-interrupt | quiet                     Interrupt active TTS speech immediately\n"
		"  speak <text>                              Speak text via TTS\n"
		"  dmenu | omni                              Open interactive command bar via jarvis_xy_prompt\n"
		"  warmup                                    Pre-warm ontology and inference engine\n"
		"  llm-mode [auto|cloud|embedding|off]       Get or set persistent inference mode\n"
		"  clear-learned                             Clear context, session, and embedding caches\n\n"
		"Options:\n"
		"  -p, --port <port>                         Override jarvisd TCP port (default from ~/.jarvisrc)\n"
		"  -c, --cmd <raw_cmd>                       Send raw control command to jarvisd socket\n"
		"  --wm <dwm|sway|hyprland>                  Window manager / environment (default from ~/.jarvisrc)\n"
		"  --inference <auto|llm|embedding|none>     Inference backend (default from ~/.jarvisrc)\n"
		"  --surface <any|terminal|browser|site|nd>  Active surface context (default: any)\n"
		"  --mod <path>                              Load additional libxylem (.so) adapter module\n"
		"  --dry-run                                 Compile utterance to IR/DSL without executing\n"
		"  --json                                    Emit JSON output\n"
		"  -q, --quiet                               Suppress desktop notifications / earcons\n",
		prog
	);
}

static void read_saved_llm_mode(char *out, size_t out_sz)
{
	jarvis_rc_get("llm_mode", "auto", out, out_sz);
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
	jarvis_rc_set("llm_mode", mode);
	if (!quiet) {
		char body[128];
		snprintf(body, sizeof(body), "Modo definido para: %s", mode);
		jarvis_xy_notify("  Modo LLM", body, 0);
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
	const char *home = getenv("HOME");
	if (path[0] != '/' && path[0] != '.' && home && *home) {
		snprintf(clean, sizeof(clean), "%s/.local/lib/%s", home, path);
	} else {
		snprintf(clean, sizeof(clean), "%s", path);
	}
	size_t len = strlen(clean);
	if (len > 3 && strcmp(clean + len - 3, ".so") == 0) {
		clean[len - 3] = '\0';
	}
	int rc = xy_load(clean);
	if (rc != XY_OK && path[0] != '/') {
		snprintf(clean, sizeof(clean), "%s", path);
		len = strlen(clean);
		if (len > 3 && strcmp(clean + len - 3, ".so") == 0) {
			clean[len - 3] = '\0';
		}
		rc = xy_load(clean);
	}
	return rc;
}

static void autoload_xylem_modules(const char *wm)
{
	const char *home = getenv("HOME");

	/* 1. Load modules from $JARVIS_MODS or ~/.jarvisrc mods */
	char mods_buf[1024] = {0};
	const char *env_mods = getenv("JARVIS_MODS");
	if (env_mods && *env_mods) {
		snprintf(mods_buf, sizeof(mods_buf), "%s", env_mods);
	} else {
		jarvis_rc_get("mods", NULL, mods_buf, sizeof(mods_buf));
	}
	if (mods_buf[0]) {
		char *saveptr = NULL;
		for (char *tok = strtok_r(mods_buf, ":, ", &saveptr); tok; tok = strtok_r(NULL, ":, ", &saveptr)) {
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

	/* 3. Auto-load mod_<wm> and mod_tts */
	if (wm && *wm) {
		char cand[128];
		snprintf(cand, sizeof(cand), "mod_%s", wm);
		load_xy_mod(cand);
	}
	load_xy_mod("mod_tts");
}

#ifndef _WIN32
static void notify_status_bar(void)
{
	const char *u = getenv("USER");
	if (!u || !*u) u = "quirinpa";
	char path[256];
	if (!jarvis_rc_get("status_notify_pid_file", NULL, path, sizeof(path)) || !path[0]) {
		snprintf(path, sizeof(path), "/tmp/dwm-bar-%s.pid", u);
	}
	FILE *fp = fopen(path, "r");
	if (!fp) return;
	int pid = 0;
	if (fscanf(fp, "%d", &pid) == 1 && pid > 1) {
		kill((pid_t)pid, SIGUSR1);
	}
	fclose(fp);
}

static int g_cli_port_override = 0;

static bool daemon_cmd(const char *cmd, char *out_resp, size_t resp_sz)
{
	if (out_resp && resp_sz > 0) out_resp[0] = '\0';
	int port = (g_cli_port_override > 0) ? g_cli_port_override : jarvis_rc_get_int("port", 8179);
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) return false;

	struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(s);
		return false;
	}
	char req[1024];
	int n = snprintf(req, sizeof(req), "%s\n", cmd);
	if (n > 0) {
		(void)write(s, req, (size_t)n);
	}
	if (out_resp && resp_sz > 0) {
		ssize_t rd = read(s, out_resp, resp_sz - 1);
		if (rd > 0) {
			out_resp[rd] = '\0';
			size_t l = strlen(out_resp);
			while (l > 0 && isspace((unsigned char)out_resp[l - 1])) out_resp[--l] = '\0';
		}
	}
	close(s);
	return true;
}

static bool is_daemon_listening(void)
{
	char resp[64];
	if (daemon_cmd("JARVIS_STATUS", resp, sizeof(resp)) && strcmp(resp, "ON") == 0) {
		return true;
	}
	return false;
}

static void ensure_daemon_running(bool activate_listener)
{
	char resp[64];
	if (daemon_cmd("PING", resp, sizeof(resp))) {
		if (activate_listener) {
			daemon_cmd("JARVIS_ON", resp, sizeof(resp));
		}
		return;
	}

	const char *home = getenv("HOME");
	char bin_path[512];
	if (home && *home) {
		snprintf(bin_path, sizeof(bin_path), "%s/.local/bin/jarvisd", home);
		if (access(bin_path, X_OK) != 0) {
			snprintf(bin_path, sizeof(bin_path), "%s/.local/bin/dwm-whisperd", home);
		}
	} else {
		snprintf(bin_path, sizeof(bin_path), "jarvisd");
	}

	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		signal(SIGHUP, SIG_IGN);
		if (fork() > 0) _exit(0);
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd > 2) close(fd);
		}
		if (activate_listener) {
			execlp("systemd-run", "systemd-run", "--user", "--unit=jarvisd", "--collect", "--quiet", bin_path, "-j", (char *)NULL);
			execlp(bin_path, "jarvisd", "-j", (char *)NULL);
		} else {
			execlp("systemd-run", "systemd-run", "--user", "--unit=jarvisd", "--collect", "--quiet", bin_path, (char *)NULL);
			execlp(bin_path, "jarvisd", (char *)NULL);
		}
		_exit(1);
	}

	for (int i = 0; i < 40; i++) {
		if (daemon_cmd("PING", resp, sizeof(resp))) {
			break;
		}
		usleep(50000);
	}
}

static void restart_daemon(bool was_active)
{
	(void)system("systemctl --user stop jarvisd.service 2>/dev/null; pkill -x jarvisd 2>/dev/null; pkill -x dwm-whisperd 2>/dev/null");
	usleep(120000);
	ensure_daemon_running(was_active);
	notify_status_bar();
}

static void do_jarvis_start(bool quiet)
{
	(void)system("wpctl set-mute @DEFAULT_AUDIO_SOURCE@ 0 2>/dev/null || true");
	ensure_daemon_running(true);
	notify_status_bar();
	if (!quiet) {
		char wake[64];
		jarvis_rc_get("wake_word", "jarbas", wake, sizeof(wake));
		char title[128], body[256];
		snprintf(title, sizeof(title), "  %c%s Ativo", toupper((unsigned char)wake[0]), wake + 1);
		snprintf(body, sizeof(body), "Diga “%s” a qualquer momento, ou “%s, bloquear” para desligar.", wake, wake);
		jarvis_xy_notify(title, body, 0);
	}
}

static void do_jarvis_stop(bool quiet)
{
	char resp[64];
	daemon_cmd("JARVIS_OFF", resp, sizeof(resp));
	notify_status_bar();
	if (!quiet) {
		char wake[64];
		jarvis_rc_get("wake_word", "jarbas", wake, sizeof(wake));
		char title[128];
		snprintf(title, sizeof(title), "  %c%s Desligado", toupper((unsigned char)wake[0]), wake + 1);
		jarvis_xy_notify(title, "Escuta de microfone desativada.", 0);
	}
}

static void do_jarvis_lockdown(bool quiet)
{
	do_jarvis_stop(true);
	if (!quiet) {
		jarvis_xy_notify("  Bloqueio", "Escuta desativada e a bloquear o ecrã...", 0);
	}
	char lock_cmd[256];
	jarvis_rc_get("lock_cmd", "slock", lock_cmd, sizeof(lock_cmd));
	if (lock_cmd[0] && fork() == 0) {
		setsid();
		usleep(150000);
		execl("/bin/sh", "sh", "-c", lock_cmd, (char *)NULL);
		_exit(0);
	}
}

static void do_jarvis_menu(void)
{
	bool running = is_daemon_listening();
	int tts_on = jarvis_rc_get_int("tts_enabled", 1);
	char voice[64], lang[32], model_path[512], llm_mode[64], wake[64];
	jarvis_rc_get("tts_voice", "pm_alex", voice, sizeof(voice));
	jarvis_rc_get("lang", "pt-pt", lang, sizeof(lang));
	jarvis_rc_get("whisper_model", "ggml-base-q8_0.bin", model_path, sizeof(model_path));
	jarvis_rc_get("llm_mode", "auto", llm_mode, sizeof(llm_mode));
	jarvis_rc_get("wake_word", "jarbas", wake, sizeof(wake));
	int fms = jarvis_rc_get_int("followup_ms", 8000);

	const char *model_base = strrchr(model_path, '/');
	model_base = model_base ? (model_base + 1) : model_path;

	char items[2048];
	snprintf(items, sizeof(items),
		"1. Alternar Escuta (%s — palavra: “%s”)\n"
		"2. Voz Neural TTS: %s (voz: %s)\n"
		"3. Janela de Seguimento: %s (%d ms)\n"
		"4. Bloquear (Desligar Escuta + Bloquear Ecrã)\n"
		"5. Modo de Inferência LLM: %s\n"
		"6. Idioma Whisper / TTS: %s\n"
		"7. Modelo Whisper: %s\n"
		"8. Barra de Comandos Semântica\n"
		"9. Limpar Atalhos e Cache Aprendidos\n"
		"10. Abrir Terminal Pi Agent\n"
		"11. Editar ~/.jarvisrc",
		running ? "ATIVO [Amarelo]" : "DESLIGADO [Prata]", wake,
		tts_on ? "ATIVO" : "DESLIGADO", voice,
		fms >= 3600000 ? "Modo Sem Palavra de Ativação" : (fms > 0 ? "Conversacional" : "Estrito"), fms,
		llm_mode, lang, model_base);

	char choice[512] = {0};
	if (jarvis_xy_prompt(" Jarbas:", items, choice, sizeof(choice)) <= 0 || !choice[0]) {
		return;
	}

	if (strncmp(choice, "1.", 2) == 0) {
		if (running) do_jarvis_stop(false);
		else do_jarvis_start(false);
	} else if (strncmp(choice, "2.", 2) == 0) {
		const char *vitems =
			"pm_alex                      (Português — Masculino, Alex)\n"
			"pm_santa                     (Português — Masculino, Santa)\n"
			"pf_dora                      (Português — Feminino, Dora)\n"
			"bm_daniel                    (English — British Male, Daniel)\n"
			"bm_george                    (English — British Male, George)\n"
			"bm_lewis                     (English — British Male, Lewis)\n"
			"bf_emma                      (English — British Female, Emma)\n"
			"am_michael                   (English — American Male, Michael)\n"
			"af_heart                     (English — American Female, Heart)\n"
			"off                          (Desativar Voz Neural TTS)";
		char vchoice[256] = {0};
		if (jarvis_xy_prompt("🗣 Voz TTS:", vitems, vchoice, sizeof(vchoice)) > 0 && vchoice[0]) {
			char vid[64] = {0};
			sscanf(vchoice, "%63s", vid);
			if (strcmp(vid, "off") == 0) {
				jarvis_rc_set("tts_enabled", "0");
				char resp[64];
				daemon_cmd("TTS_INTERRUPT", resp, sizeof(resp));
				jarvis_xy_notify("🗣  Voz Neural Desativada", "Modo silencioso ativo.", 0);
			} else if (vid[0]) {
				jarvis_rc_set("tts_enabled", "1");
				jarvis_rc_set("tts_voice", vid);
				const char *tlang = (vid[0] == 'p') ? "pt-pt" : ((vid[0] == 'a') ? "en-us" : "en-gb-x-rp");
				jarvis_rc_set("tts_lang", tlang);
				jarvis_xy_notify("🗣  Voz Neural Ativa", vid, 0);
				char resp[64];
				daemon_cmd("TTS_SPEAK|Às suas ordens.", resp, sizeof(resp));
			}
		}
	} else if (strncmp(choice, "3.", 2) == 0) {
		const char *fitems =
			"8000     (8s Janela de Seguimento — sem repetir a palavra de ativação)\n"
			"15000    (15s Janela de Seguimento Prolongada)\n"
			"3600000  (Modo Contínuo — falar diretamente sem palavra de ativação)\n"
			"0        (Modo Estrito — exigir sempre a palavra de ativação)";
		char fchoice[256] = {0};
		if (jarvis_xy_prompt("🎙 Seguimento:", fitems, fchoice, sizeof(fchoice)) > 0 && fchoice[0]) {
			char fval[32] = {0};
			sscanf(fchoice, "%31s", fval);
			if (fval[0]) {
				jarvis_rc_set("followup_ms", fval);
				jarvis_xy_notify("🎙  Janela de Seguimento", fval, 0);
			}
		}
	} else if (strncmp(choice, "4.", 2) == 0) {
		do_jarvis_lockdown(false);
	} else if (strncmp(choice, "5.", 2) == 0) {
		const char *litems =
			"auto       (Embeddings locais -> LLM remoto)\n"
			"cloud      (LLM remoto rápido/inteligente)\n"
			"embedding  (Apenas embeddings locais — offline)\n"
			"off        (Apenas solver determinístico Tier-0)";
		char lchoice[256] = {0};
		if (jarvis_xy_prompt(" Modo LLM:", litems, lchoice, sizeof(lchoice)) > 0 && lchoice[0]) {
			char lval[32] = {0};
			sscanf(lchoice, "%31s", lval);
			if (lval[0]) write_saved_llm_mode(lval, false);
		}
	} else if (strncmp(choice, "6.", 2) == 0) {
		const char *lang_items =
			"pt-pt (Português Europeu)\n"
			"en    (English)\n"
			"es    (Español)\n"
			"fr    (Français)\n"
			"de    (Deutsch)\n"
			"kbd   (Seguir layout do teclado automaticamente)\n"
			"auto  (Deteção automática Whisper)";
		char gchoice[256] = {0};
		if (jarvis_xy_prompt("🌐 Idioma:", lang_items, gchoice, sizeof(gchoice)) > 0 && gchoice[0]) {
			char gval[32] = {0};
			sscanf(gchoice, "%31s", gval);
			if (gval[0]) {
				jarvis_rc_set("lang", gval);
				if (strncmp(gval, "pt", 2) == 0) jarvis_rc_set("tts_lang", "pt-pt");
				else if (strncmp(gval, "en", 2) == 0) jarvis_rc_set("tts_lang", "en-gb-x-rp");
				jarvis_xy_notify("🌐  Idioma Atualizado", gval, 0);
			}
		}
	} else if (strncmp(choice, "7.", 2) == 0) {
		const char *mitems =
			"ggml-base-q8_0.bin           (78 MB  - Multilingue Rápido Q8)\n"
			"ggml-small-q8_0.bin          (252 MB - Multilingue Alta Precisão Q8)\n"
			"ggml-small-q5_1.bin          (182 MB - Multilingue Q5)\n"
			"ggml-base.en-q8_0.bin        (78 MB  - Inglês Apenas Ultra-Rápido)";
		char mchoice[256] = {0};
		if (jarvis_xy_prompt("🧠 Modelo Whisper:", mitems, mchoice, sizeof(mchoice)) > 0 && mchoice[0]) {
			char mfile[128] = {0};
			sscanf(mchoice, "%127s", mfile);
			if (mfile[0]) {
				const char *home = getenv("HOME");
				char full_m[512];
				snprintf(full_m, sizeof(full_m), "%s/.local/share/whisper/%s", home ? home : "", mfile);
				jarvis_rc_set("whisper_model", full_m);
				restart_daemon(running);
				jarvis_xy_notify("🧠  Modelo Whisper Ativo", mfile, 0);
			}
		}
	} else if (strncmp(choice, "8.", 2) == 0) {
		(void)system("jarvis dmenu &");
	} else if (strncmp(choice, "9.", 2) == 0) {
		(void)system("jarvis clear-learned");
	} else if (strncmp(choice, "10.", 3) == 0) {
		(void)system("dwm-jarvis-pi show &");
	} else if (strncmp(choice, "11.", 3) == 0) {
		(void)system("st -e ${EDITOR:-nvim} ~/.jarvisrc &");
	}
}
#endif

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
	const char *subarg2 = NULL;

	const char *home = getenv("HOME");
	const char *user = getenv("USER");
	if (!user || !*user) user = "user";

	char ctx_path[512] = {0}, sess_path[512] = {0}, mem_path[512] = {0}, fs_path[512] = {0}, emb_path[512] = {0};
	snprintf(ctx_path, sizeof(ctx_path), "/tmp/jarvis-%s.context.json", user);
	snprintf(sess_path, sizeof(sess_path), "/tmp/jarvis-%s.session.json", user);
	cfg.context_path = ctx_path;
	cfg.session_path = sess_path;
	if (home && *home) {
		char def_mem[512], def_fs[512], def_emb[512];
		snprintf(def_mem, sizeof(def_mem), "%s/.config/jarvis/memories.json", home);
		snprintf(def_fs, sizeof(def_fs), "%s/.config/jarvis/few_shots.json", home);
		snprintf(def_emb, sizeof(def_emb), "%s/.cache/jarvis/embeddings.json", home);
		jarvis_rc_get("memory_path", def_mem, mem_path, sizeof(mem_path));
		jarvis_rc_get("few_shot_path", def_fs, fs_path, sizeof(fs_path));
		jarvis_rc_get("embed_cache_path", def_emb, emb_path, sizeof(emb_path));
		cfg.memory_path = mem_path;
		cfg.few_shot_path = fs_path;
		cfg.embed_cache_path = emb_path;
	}

	bool end_of_opts = false;
	const char *raw_daemon_cmd = NULL;

	for (int i = 1; i < argc; i++) {
		if (!end_of_opts) {
			if (strcmp(argv[i], "--") == 0) {
				end_of_opts = true;
				continue;
			}
			if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
				print_usage(argv[0]);
				return 0;
			}
			if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc) {
				g_cli_port_override = atoi(argv[++i]);
				continue;
			}
			if ((strcmp(argv[i], "--cmd") == 0 || strcmp(argv[i], "-c") == 0) && i + 1 < argc) {
				raw_daemon_cmd = argv[++i];
				continue;
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
			if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0) {
				quiet = true;
				continue;
			}
			if (u_pos == 0 && !subcmd && argv[i][0] == '-' && argv[i][1] != '\0') {
				fprintf(stderr, "Unknown option: %s\n", argv[i]);
				print_usage(argv[0]);
				return 1;
			}
		}
		if (u_pos == 0 && !subcmd && !end_of_opts) {
			if (strcmp(argv[i], "dmenu") == 0 || strcmp(argv[i], "omni") == 0 ||
			    strcmp(argv[i], "warmup") == 0 || strcmp(argv[i], "clear-learned") == 0 ||
			    strcmp(argv[i], "tts-interrupt") == 0 || strcmp(argv[i], "interrupt") == 0 ||
			    strcmp(argv[i], "quiet") == 0 || strcmp(argv[i], "speak") == 0 ||
			    strcmp(argv[i], "start") == 0 || strcmp(argv[i], "on") == 0 ||
			    strcmp(argv[i], "jarvis-start") == 0 || strcmp(argv[i], "server-start") == 0 ||
			    strcmp(argv[i], "stop") == 0 || strcmp(argv[i], "off") == 0 ||
			    strcmp(argv[i], "jarvis-stop") == 0 ||
			    strcmp(argv[i], "toggle") == 0 || strcmp(argv[i], "jarvis-toggle") == 0 ||
			    strcmp(argv[i], "status") == 0 ||
			    strcmp(argv[i], "lockdown") == 0 || strcmp(argv[i], "lock") == 0 ||
			    strcmp(argv[i], "menu") == 0 || strcmp(argv[i], "tts-off") == 0) {
				subcmd = argv[i];
				continue;
			}
			if (strcmp(argv[i], "tts-on") == 0) {
				subcmd = "tts-on";
				if (i + 1 < argc) subarg = argv[++i];
				continue;
			}
			if (strcmp(argv[i], "llm-mode") == 0) {
				subcmd = "llm-mode";
				if (i + 1 < argc) subarg = argv[++i];
				continue;
			}
			if (strcmp(argv[i], "config") == 0) {
				subcmd = "config";
				if (i + 1 < argc) subarg = argv[++i];
				if (i + 1 < argc) subarg2 = argv[++i];
				continue;
			}
			if (strcmp(argv[i], "query") == 0 || strcmp(argv[i], "exec") == 0 || strcmp(argv[i], "parse") == 0) {
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

	if (raw_daemon_cmd) {
		char resp[4096];
		if (!daemon_cmd(raw_daemon_cmd, resp, sizeof(resp))) {
			if (!quiet) fprintf(stderr, "Failed to contact jarvisd\n");
			return 1;
		}
		if (resp[0]) printf("%s\n", resp);
		return 0;
	}

	if (!cli_infer_set) {
		char saved_mode[64];
		read_saved_llm_mode(saved_mode, sizeof(saved_mode));
		cfg.infer_mode = parse_infer_mode(saved_mode);
	}

	/* Auto-load configured or environment-matched Xylem adapter modules */
	autoload_xylem_modules(cfg.wm);

	if (subcmd) {
#ifndef _WIN32
		if (strcmp(subcmd, "start") == 0 || strcmp(subcmd, "on") == 0 || strcmp(subcmd, "jarvis-start") == 0) {
			do_jarvis_start(quiet);
			return 0;
		}
		if (strcmp(subcmd, "server-start") == 0) {
			ensure_daemon_running(false);
			return 0;
		}
		if (strcmp(subcmd, "stop") == 0 || strcmp(subcmd, "off") == 0 || strcmp(subcmd, "jarvis-stop") == 0) {
			do_jarvis_stop(quiet);
			return 0;
		}
		if (strcmp(subcmd, "toggle") == 0 || strcmp(subcmd, "jarvis-toggle") == 0) {
			if (is_daemon_listening()) do_jarvis_stop(quiet);
			else do_jarvis_start(quiet);
			return 0;
		}
		if (strcmp(subcmd, "status") == 0) {
			printf("%s\n", is_daemon_listening() ? "on" : "off");
			return 0;
		}
		if (strcmp(subcmd, "lockdown") == 0 || strcmp(subcmd, "lock") == 0) {
			do_jarvis_lockdown(quiet);
			return 0;
		}
		if (strcmp(subcmd, "menu") == 0) {
			do_jarvis_menu();
			return 0;
		}
		if (strcmp(subcmd, "tts-on") == 0) {
			jarvis_rc_set("tts_enabled", "1");
			if (subarg && *subarg) {
				jarvis_rc_set("tts_voice", subarg);
			}
			char v[64];
			jarvis_rc_get("tts_voice", "pm_alex", v, sizeof(v));
			if (!quiet) jarvis_xy_notify("🗣  Voz Neural Ativa", v, 0);
			char resp[64];
			daemon_cmd("TTS_SPEAK|Às suas ordens.", resp, sizeof(resp));
			return 0;
		}
		if (strcmp(subcmd, "tts-off") == 0) {
			jarvis_rc_set("tts_enabled", "0");
			char resp[64];
			daemon_cmd("TTS_INTERRUPT", resp, sizeof(resp));
			if (!quiet) jarvis_xy_notify("🗣  Voz Neural Desativada", "Modo silencioso ativo.", 0);
			return 0;
		}
#endif
		if (strcmp(subcmd, "config") == 0) {
			if (!subarg) {
				const char *h = getenv("HOME");
				char rc_p[512];
				snprintf(rc_p, sizeof(rc_p), "%s/.jarvisrc", h ? h : "");
				FILE *fp = fopen(rc_p, "r");
				if (fp) {
					char line[512];
					while (fgets(line, sizeof(line), fp)) fputs(line, stdout);
					fclose(fp);
				}
				return 0;
			}
			if (!subarg2) {
				char val[512];
				if (jarvis_rc_get(subarg, "", val, sizeof(val))) {
					printf("%s\n", val);
					return 0;
				}
				return 1;
			}
			return jarvis_rc_set(subarg, subarg2) ? 0 : 1;
		}
		if (strcmp(subcmd, "tts-interrupt") == 0 || strcmp(subcmd, "interrupt") == 0 || strcmp(subcmd, "quiet") == 0) {
#ifndef _WIN32
			char resp[64];
			if (!daemon_cmd("TTS_INTERRUPT", resp, sizeof(resp)))
#endif
				jarvis_xy_tts_interrupt(0);
			return 0;
		}
		if (strcmp(subcmd, "speak") == 0) {
			if (utterance[0]) {
#ifndef _WIN32
				char cmd[1024], resp[64];
				snprintf(cmd, sizeof(cmd), "TTS_SPEAK|%s", utterance);
				if (!daemon_cmd(cmd, resp, sizeof(resp)))
#endif
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
				jarvis_xy_notify("  Cache Limpa", "Caches de contexto e embeddings limpas.", 0);
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
			if (jarvis_xy_prompt(" Jarbas:", candidates, selection, sizeof(selection)) <= 0 || !selection[0]) {
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
			jarvis_xy_notify("  Jarbas — Não Reconhecido", body, 0);
		}
		printf("Error: %s\n", res.error[0] ? res.error : "unresolved");
	} else {
		const char *out_msg = res.combined_result.output[0] ? res.combined_result.output : res.combined_result.error;
		if (!quiet && strcmp(res.last_dsl.op, "answer_question") != 0) {
			char title[256], body[1024];
			snprintf(title, sizeof(title), "  Jarbas → %s (%s)",
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
