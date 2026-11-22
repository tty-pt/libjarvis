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
#include <sys/wait.h>
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
		"  notify <text>                             Announce background completion via OSD + non-preemptive TTS\n"
		"  dmenu | omni                              Open interactive command bar via jarvis_xy_prompt\n"
		"  warmup                                    Pre-warm ontology and inference engine\n"
		"  clear-learned                             Clear context, session, and embedding caches\n\n"
		"Options:\n"
		"  -p, --port <port>                         Override jarvisd TCP port (default from ~/.jarvisrc)\n"
		"  -c, --cmd <raw_cmd>                       Send raw control command to jarvisd socket\n"
		"  --wm <dwm|sway|hyprland>                  Window manager / environment (default from ~/.jarvisrc)\n"
		"  --surface <any|terminal|browser|site|nd>  Active surface context (default: any)\n"
		"  --mod <path>                              Load additional libxylem (.so) adapter module\n"
		"  --dry-run                                 Compile utterance to IR/DSL without executing\n"
		"  --json                                    Emit JSON output\n"
		"  -q, --quiet                               Suppress desktop notifications / earcons\n",
		prog
	);
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

static jarvis_ontology_t *get_cli_ont(void)
{
	static int loaded = 0;
	static jarvis_ontology_t *s_ont = NULL;
	if (!loaded) {
		loaded = 1;
		const char *home = getenv("HOME");
		char def_ont[512] = {0};
		if (home && *home)
			snprintf(def_ont, sizeof(def_ont), "%s/.config/jarvis/ontology.json", home);
		char ont_path[512] = {0};
		jarvis_rc_get("ontology_path", def_ont, ont_path, sizeof(ont_path));
		if (ont_path[0])
			s_ont = jarvis_ontology_create(ont_path);
	}
	return s_ont;
}

static void get_cli_wake_word(const jarvis_ontology_t *ont, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	const char *ont_wake = ont ? jarvis_ontology_get_wake_word(ont) : NULL;
	if (!ont_wake || !*ont_wake) ont_wake = "assistant";
	jarvis_rc_get("wake_word", ont_wake, out, out_sz);
	if (!out[0]) snprintf(out, out_sz, "%s", ont_wake);
}

static void get_cli_default_lang(const jarvis_ontology_t *ont, char *out, size_t out_sz)
{
	if (!out || out_sz == 0) return;
	const char *ont_lang = ont ? jarvis_ontology_get_lang(ont) : NULL;
	if (!ont_lang || !*ont_lang) ont_lang = "en";
	jarvis_rc_get("lang", ont_lang, out, out_sz);
	if (!out[0]) snprintf(out, out_sz, "%s", ont_lang);
}

static void get_cli_default_voice(const jarvis_ontology_t *ont, char *out, size_t out_sz)
{
	char lang[32];
	get_cli_default_lang(ont, lang, sizeof(lang));
	const char *def_voice = (strncmp(lang, "pt", 2) == 0) ? "pm_tuga" : "bm_daniel";
	jarvis_rc_get("tts_voice", def_voice, out, out_sz);
	if (!out[0]) snprintf(out, out_sz, "%s", def_voice);
}

static void do_jarvis_start(bool quiet)
{
	(void)system("wpctl set-mute @DEFAULT_AUDIO_SOURCE@ 0 2>/dev/null || true");
	ensure_daemon_running(true);
	notify_status_bar();
	if (!quiet) {
		const jarvis_ontology_t *ont = get_cli_ont();
		char wake[64], title[128], body[256];
		get_cli_wake_word(ont, wake, sizeof(wake));
		jarvis_ontology_format_ui(ont, "active_title", wake, "  {Wake} Active", title, sizeof(title));
		jarvis_ontology_format_ui(ont, "active_body", wake, "Say “{wake}” anytime, or “{wake}, lockdown” to stop.", body, sizeof(body));
		jarvis_xy_notify(title, body, 0);
	}
}

static void do_jarvis_stop(bool quiet)
{
	char resp[64];
	daemon_cmd("JARVIS_OFF", resp, sizeof(resp));
	notify_status_bar();
	if (!quiet) {
		const jarvis_ontology_t *ont = get_cli_ont();
		char wake[64], title[128], body[256];
		get_cli_wake_word(ont, wake, sizeof(wake));
		jarvis_ontology_format_ui(ont, "disabled_title", wake, "  {Wake} Disabled", title, sizeof(title));
		jarvis_ontology_format_ui(ont, "mic_disabled_body", wake, "Microphone listening disabled.", body, sizeof(body));
		jarvis_xy_notify(title, body, 0);
	}
}

static void do_jarvis_lockdown(bool quiet)
{
	do_jarvis_stop(true);
	if (!quiet) {
		const jarvis_ontology_t *ont = get_cli_ont();
		char wake[64], title[128], body[256];
		get_cli_wake_word(ont, wake, sizeof(wake));
		jarvis_ontology_format_ui(ont, "lockdown_short_title", wake, "  Lockdown", title, sizeof(title));
		jarvis_ontology_format_ui(ont, "lockdown_short_body", wake, "Listening disabled and locking screen...", body, sizeof(body));
		jarvis_xy_notify(title, body, 0);
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
	const jarvis_ontology_t *ont = get_cli_ont();
	bool running = is_daemon_listening();
	int tts_on = jarvis_rc_get_int("tts_enabled", 1);
	char voice[64], lang[32], model_path[512], wake[64];
	get_cli_default_voice(ont, voice, sizeof(voice));
	get_cli_default_lang(ont, lang, sizeof(lang));
	jarvis_rc_get("whisper_model", "ggml-base-q8_0.bin", model_path, sizeof(model_path));
	get_cli_wake_word(ont, wake, sizeof(wake));
	int fms = jarvis_rc_get_int("followup_ms", 8000);

	const char *model_base = strrchr(model_path, '/');
	model_base = model_base ? (model_base + 1) : model_path;

	char st_on[64], st_off[64], fw_always[96], fw_conv[64], fw_strict[64];
	char lbl_wake[64], lbl_voice[64];
	char it_toggle[128], it_tts[128], it_fw[128], it_lock[128], it_lang[128];
	char it_model[128], it_dmenu[128], it_clear[128], it_ren[128], it_rc[128];
	jarvis_ontology_format_ui(ont, "menu_state_on", wake, "ON", st_on, sizeof(st_on));
	jarvis_ontology_format_ui(ont, "menu_state_off", wake, "OFF", st_off, sizeof(st_off));
	jarvis_ontology_format_ui(ont, "menu_followup_always", wake, "Always-On", fw_always, sizeof(fw_always));
	jarvis_ontology_format_ui(ont, "menu_followup_conv", wake, "Conversational", fw_conv, sizeof(fw_conv));
	jarvis_ontology_format_ui(ont, "menu_followup_strict", wake, "Strict", fw_strict, sizeof(fw_strict));
	jarvis_ontology_format_ui(ont, "menu_wake_label", wake, "wake", lbl_wake, sizeof(lbl_wake));
	jarvis_ontology_format_ui(ont, "menu_voice_label", wake, "voice", lbl_voice, sizeof(lbl_voice));
	jarvis_ontology_format_ui(ont, "menu_item_toggle", wake, "1. Toggle Listening", it_toggle, sizeof(it_toggle));
	jarvis_ontology_format_ui(ont, "menu_item_tts", wake, "2. Neural TTS Voice", it_tts, sizeof(it_tts));
	jarvis_ontology_format_ui(ont, "menu_item_followup", wake, "3. Follow-up Window", it_fw, sizeof(it_fw));
	jarvis_ontology_format_ui(ont, "menu_item_lock", wake, "4. Lockdown (Disable Listening + Lock Screen)", it_lock, sizeof(it_lock));
	jarvis_ontology_format_ui(ont, "menu_item_lang", wake, "5. Whisper / TTS Language", it_lang, sizeof(it_lang));
	jarvis_ontology_format_ui(ont, "menu_item_model", wake, "6. Whisper Model", it_model, sizeof(it_model));
	jarvis_ontology_format_ui(ont, "menu_item_dmenu", wake, "7. Semantic Command Bar", it_dmenu, sizeof(it_dmenu));
	jarvis_ontology_format_ui(ont, "menu_item_clear", wake, "8. Clear Learned Shortcuts & Cache", it_clear, sizeof(it_clear));
	jarvis_ontology_format_ui(ont, "menu_item_ren", wake, "9. Open ren Terminal (dwm-jarvis)", it_ren, sizeof(it_ren));
	jarvis_ontology_format_ui(ont, "menu_item_rc", wake, "10. Edit ~/.jarvisrc", it_rc, sizeof(it_rc));

	char items[2048];
	snprintf(items, sizeof(items),
		"%s (%s — %s: “%s”)\n"
		"%s: %s (%s: %s)\n"
		"%s: %s (%d ms)\n"
		"%s\n"
		"%s: %s\n"
		"%s: %s\n"
		"%s\n"
		"%s\n"
		"%s\n"
		"%s",
		it_toggle, running ? st_on : st_off, lbl_wake, wake,
		it_tts, tts_on ? st_on : st_off, lbl_voice, voice,
		it_fw, fms >= 3600000 ? fw_always : (fms > 0 ? fw_conv : fw_strict), fms,
		it_lock,
		it_lang, lang,
		it_model, model_base,
		it_dmenu,
		it_clear,
		it_ren,
		it_rc);

	char mprompt[128], choice[512] = {0};
	jarvis_ontology_format_ui(ont, "menu_prompt", wake, " {Wake}:", mprompt, sizeof(mprompt));
	if (jarvis_xy_prompt(mprompt, items, choice, sizeof(choice)) <= 0 || !choice[0]) {
		return;
	}

	if (strncmp(choice, "1.", 2) == 0) {
		if (running) do_jarvis_stop(false);
		else do_jarvis_start(false);
	} else if (strncmp(choice, "2.", 2) == 0) {
		char v_off[128], vprompt[128], vitems[1536];
		jarvis_ontology_format_ui(ont, "menu_voice_off", wake, "off                          (Disable Neural TTS Voice)", v_off, sizeof(v_off));
		jarvis_ontology_format_ui(ont, "voice_prompt", wake, "🗣 TTS Voice:", vprompt, sizeof(vprompt));
		snprintf(vitems, sizeof(vitems),
			"pm_tuga                      (Português de Portugal — Male, Tuga)\n"
			"pm_alex                      (Português — Male, Alex)\n"
			"pm_santa                     (Português — Male, Santa)\n"
			"pf_dora                      (Português — Female, Dora)\n"
			"bm_daniel                    (English — British Male, Daniel)\n"
			"bm_george                    (English — British Male, George)\n"
			"bm_lewis                     (English — British Male, Lewis)\n"
			"bf_emma                      (English — British Female, Emma)\n"
			"am_michael                   (English — American Male, Michael)\n"
			"af_heart                     (English — American Female, Heart)\n"
			"%s", v_off);
		char vchoice[256] = {0};
		if (jarvis_xy_prompt(vprompt, vitems, vchoice, sizeof(vchoice)) > 0 && vchoice[0]) {
			char vid[64] = {0};
			sscanf(vchoice, "%63s", vid);
			if (strcmp(vid, "off") == 0) {
				jarvis_rc_set("tts_enabled", "0");
				char resp[64], ttitle[128], tbody[256];
				daemon_cmd("TTS_INTERRUPT", resp, sizeof(resp));
				jarvis_ontology_format_ui(ont, "tts_off_title", wake, "🗣  Neural Voice Disabled", ttitle, sizeof(ttitle));
				jarvis_ontology_format_ui(ont, "tts_off_body", wake, "Silent mode active.", tbody, sizeof(tbody));
				jarvis_xy_notify(ttitle, tbody, 0);
			} else if (vid[0]) {
				jarvis_rc_set("tts_enabled", "1");
				jarvis_rc_set("tts_voice", vid);
				const char *tlang = (vid[0] == 'p') ? "pt-pt" : ((vid[0] == 'a') ? "en-us" : "en-gb-x-rp");
				jarvis_rc_set("tts_lang", tlang);
				char ttitle[128], greet[128], cmd_sp[192], resp[64];
				jarvis_ontology_format_ui(ont, "tts_on_title", wake, "🗣  Neural Voice Active", ttitle, sizeof(ttitle));
				jarvis_xy_notify(ttitle, vid, 0);
				jarvis_ontology_format_ui(ont, "tts_greeting", wake, "Ready.", greet, sizeof(greet));
				snprintf(cmd_sp, sizeof(cmd_sp), "TTS_SPEAK|%s", greet);
				daemon_cmd(cmd_sp, resp, sizeof(resp));
			}
		}
	} else if (strncmp(choice, "3.", 2) == 0) {
		char f8[160], f15[160], fc[160], fs[160], fprompt[128], fitems[768];
		jarvis_ontology_format_ui(ont, "menu_followup_8s", wake, "8000     (8s Follow-up Window — no wake word needed)", f8, sizeof(f8));
		jarvis_ontology_format_ui(ont, "menu_followup_15s", wake, "15000    (15s Extended Follow-up Window)", f15, sizeof(f15));
		jarvis_ontology_format_ui(ont, "menu_followup_cont", wake, "3600000  (Continuous Mode — speak directly without wake word)", fc, sizeof(fc));
		jarvis_ontology_format_ui(ont, "menu_followup_strict_opt", wake, "0        (Strict Mode — always require wake word)", fs, sizeof(fs));
		jarvis_ontology_format_ui(ont, "followup_prompt", wake, "🎙 Follow-up:", fprompt, sizeof(fprompt));
		snprintf(fitems, sizeof(fitems), "%s\n%s\n%s\n%s", f8, f15, fc, fs);
		char fchoice[256] = {0};
		if (jarvis_xy_prompt(fprompt, fitems, fchoice, sizeof(fchoice)) > 0 && fchoice[0]) {
			char fval[32] = {0};
			sscanf(fchoice, "%31s", fval);
			if (fval[0]) {
				char ftitle[128];
				jarvis_rc_set("followup_ms", fval);
				jarvis_ontology_format_ui(ont, "followup_title", wake, "🎙  Follow-up Window", ftitle, sizeof(ftitle));
				jarvis_xy_notify(ftitle, fval, 0);
			}
		}
	} else if (strncmp(choice, "4.", 2) == 0) {
		do_jarvis_lockdown(false);
	} else if (strncmp(choice, "5.", 2) == 0) {
		char l_kbd[128], l_auto[128], lprompt[128], lang_items[512];
		jarvis_ontology_format_ui(ont, "menu_lang_kbd", wake, "kbd   (Follow keyboard layout automatically)", l_kbd, sizeof(l_kbd));
		jarvis_ontology_format_ui(ont, "menu_lang_auto", wake, "auto  (Whisper auto-detect)", l_auto, sizeof(l_auto));
		jarvis_ontology_format_ui(ont, "lang_prompt", wake, "🌐 Language:", lprompt, sizeof(lprompt));
		snprintf(lang_items, sizeof(lang_items),
			"en    (English)\n"
			"pt-pt (Português Europeu)\n"
			"es    (Español)\n"
			"fr    (Français)\n"
			"de    (Deutsch)\n"
			"%s\n%s", l_kbd, l_auto);
		char gchoice[256] = {0};
		if (jarvis_xy_prompt(lprompt, lang_items, gchoice, sizeof(gchoice)) > 0 && gchoice[0]) {
			char gval[32] = {0};
			sscanf(gchoice, "%31s", gval);
			if (gval[0]) {
				char ltitle[128];
				jarvis_rc_set("lang", gval);
				if (strncmp(gval, "pt", 2) == 0) jarvis_rc_set("tts_lang", "pt-pt");
				else if (strncmp(gval, "en", 2) == 0) jarvis_rc_set("tts_lang", "en-gb-x-rp");
				jarvis_ontology_format_ui(ont, "lang_updated_title", wake, "🌐  Language Updated", ltitle, sizeof(ltitle));
				jarvis_xy_notify(ltitle, gval, 0);
			}
		}
	} else if (strncmp(choice, "6.", 2) == 0) {
		char m1[128], m2[128], m3[128], m4[128], wprompt[128], mitems[640];
		jarvis_ontology_format_ui(ont, "menu_model_base_q8", wake, "ggml-base-q8_0.bin           (78 MB  - Fast Multilingual Q8)", m1, sizeof(m1));
		jarvis_ontology_format_ui(ont, "menu_model_small_q8", wake, "ggml-small-q8_0.bin          (252 MB - High-Accuracy Multilingual Q8)", m2, sizeof(m2));
		jarvis_ontology_format_ui(ont, "menu_model_small_q5", wake, "ggml-small-q5_1.bin          (182 MB - Multilingual Q5)", m3, sizeof(m3));
		jarvis_ontology_format_ui(ont, "menu_model_en_q8", wake, "ggml-base.en-q8_0.bin        (78 MB  - English-Only Fast Q8)", m4, sizeof(m4));
		jarvis_ontology_format_ui(ont, "whisper_prompt", wake, "🧠 Whisper Model:", wprompt, sizeof(wprompt));
		snprintf(mitems, sizeof(mitems), "%s\n%s\n%s\n%s", m1, m2, m3, m4);
		char mchoice[256] = {0};
		if (jarvis_xy_prompt(wprompt, mitems, mchoice, sizeof(mchoice)) > 0 && mchoice[0]) {
			char mfile[128] = {0};
			sscanf(mchoice, "%127s", mfile);
			if (mfile[0]) {
				const char *home = getenv("HOME");
				char full_m[512], wtitle[128];
				snprintf(full_m, sizeof(full_m), "%s/.local/share/whisper/%s", home ? home : "", mfile);
				jarvis_rc_set("whisper_model", full_m);
				restart_daemon(running);
				jarvis_ontology_format_ui(ont, "whisper_updated_title", wake, "🧠  Whisper Model Active", wtitle, sizeof(wtitle));
				jarvis_xy_notify(wtitle, mfile, 0);
			}
		}
	} else if (strncmp(choice, "7.", 2) == 0) {
		(void)system("jarvis dmenu &");
	} else if (strncmp(choice, "8.", 2) == 0) {
		(void)system("jarvis clear-learned");
	} else if (strncmp(choice, "9.", 2) == 0) {
		(void)system("dwm-jarvis show &");
	} else if (strncmp(choice, "10.", 3) == 0) {
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
	if (home && *home) {
		char def_sess[512];
		snprintf(def_sess, sizeof(def_sess), "%s/.cache/jarvis/session.json", home);
		jarvis_rc_get("session_path", def_sess, sess_path, sizeof(sess_path));
	} else {
		snprintf(sess_path, sizeof(sess_path), "/tmp/jarvis-%s.session.json", user);
	}
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
			    strcmp(argv[i], "notify") == 0 ||
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

	/* Auto-load configured or environment-matched Xylem adapter modules */
	jarvis_load_mods(cfg.wm, true);

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
			const jarvis_ontology_t *ont = get_cli_ont();
			jarvis_rc_set("tts_enabled", "1");
			if (subarg && *subarg) {
				jarvis_rc_set("tts_voice", subarg);
			}
			char v[64], wake[64], ttitle[128], greet[128], cmd_sp[192], resp[64];
			get_cli_default_voice(ont, v, sizeof(v));
			get_cli_wake_word(ont, wake, sizeof(wake));
			jarvis_ontology_format_ui(ont, "tts_on_title", wake, "🗣  Neural Voice Active", ttitle, sizeof(ttitle));
			if (!quiet) jarvis_xy_notify(ttitle, v, 0);
			jarvis_ontology_format_ui(ont, "tts_greeting", wake, "Ready.", greet, sizeof(greet));
			snprintf(cmd_sp, sizeof(cmd_sp), "TTS_SPEAK|%s", greet);
			daemon_cmd(cmd_sp, resp, sizeof(resp));
			return 0;
		}
		if (strcmp(subcmd, "tts-off") == 0) {
			const jarvis_ontology_t *ont = get_cli_ont();
			jarvis_rc_set("tts_enabled", "0");
			char resp[64], wake[64], ttitle[128], tbody[256];
			get_cli_wake_word(ont, wake, sizeof(wake));
			daemon_cmd("TTS_INTERRUPT", resp, sizeof(resp));
			jarvis_ontology_format_ui(ont, "tts_off_title", wake, "🗣  Neural Voice Disabled", ttitle, sizeof(ttitle));
			jarvis_ontology_format_ui(ont, "tts_off_body", wake, "Silent mode active.", tbody, sizeof(tbody));
			if (!quiet) jarvis_xy_notify(ttitle, tbody, 0);
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
		if (strcmp(subcmd, "notify") == 0) {
			if (utterance[0]) {
				const jarvis_ontology_t *ont = get_cli_ont();
				char wake[64], ntitle[128];
				get_cli_wake_word(ont, wake, sizeof(wake));
				jarvis_ontology_format_ui(ont, "notify_title", wake, "  {Wake}", ntitle, sizeof(ntitle));
#ifndef _WIN32
				char cmd[1024], resp[64];
				snprintf(cmd, sizeof(cmd), "TTS_NOTIFY|%s|%s", ntitle, utterance);
				if (!daemon_cmd(cmd, resp, sizeof(resp)))
#endif
				{
					if (!quiet)
						jarvis_xy_notify(ntitle, utterance, 1);
					jarvis_xy_speak_clause("notify", utterance, 2);
				}
			}
			return 0;
		}
		if (strcmp(subcmd, "clear-learned") == 0) {
#ifndef _WIN32
			if (ctx_path[0]) unlink(ctx_path);
			if (sess_path[0]) unlink(sess_path);
			if (emb_path[0]) unlink(emb_path);
#endif
			if (!quiet) {
				const jarvis_ontology_t *ont = get_cli_ont();
				char wake[64], ctitle[128], cbody[256];
				get_cli_wake_word(ont, wake, sizeof(wake));
				jarvis_ontology_format_ui(ont, "cache_cleared_title", wake, "  Cache Cleared", ctitle, sizeof(ctitle));
				jarvis_ontology_format_ui(ont, "cache_cleared_body", wake, "Context and embedding caches cleared.", cbody, sizeof(cbody));
				jarvis_xy_notify(ctitle, cbody, 0);
			}
			printf("Cleared caches\n");
			return 0;
		}
		if (strcmp(subcmd, "warmup") == 0) {
			jarvis_engine_t *w_eng = jarvis_engine_create(&cfg);
			jarvis_engine_destroy(w_eng);
			return 0;
		} else if (strcmp(subcmd, "dmenu") == 0 || strcmp(subcmd, "omni") == 0) {
			const jarvis_ontology_t *ont = get_cli_ont();
			char wake[64], dprompt[128];
			get_cli_wake_word(ont, wake, sizeof(wake));
			jarvis_ontology_format_ui(ont, "dmenu_prompt", wake, " {Wake}:", dprompt, sizeof(dprompt));
			char candidates[4096];
			build_prompt_candidates(fs_path, candidates, sizeof(candidates));
			char selection[512] = {0};
			if (jarvis_xy_prompt(dprompt, candidates, selection, sizeof(selection)) <= 0 || !selection[0]) {
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

#ifndef _WIN32
	if (!cfg.dry_run && !as_json) {
		pid_t pid = fork();
		if (pid == 0) {
			execlp("dwm-jarvis", "dwm-jarvis", "send", utterance, (char *)NULL);
			_exit(127);
		} else if (pid > 0) {
			int st = 0;
			if (waitpid(pid, &st, 0) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0) {
				return 0;
			}
		}
	}
#endif

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
			const jarvis_ontology_t *ont = jarvis_engine_get_ontology(eng);
			char wake[64], utitle[128], body[560];
			get_cli_wake_word(ont, wake, sizeof(wake));
			jarvis_ontology_format_ui(ont, "unrecognized_title", wake, "  {Wake} — Unrecognized", utitle, sizeof(utitle));
			snprintf(body, sizeof(body), "“%s”", utterance);
			jarvis_xy_notify(utitle, body, 0);
		}
		printf("Error: %s\n", res.error[0] ? res.error : "unresolved");
	} else {
		const char *out_msg = res.combined_result.output[0] ? res.combined_result.output : res.combined_result.error;
		if (!quiet && strcmp(res.last_dsl.op, "answer_question") != 0) {
			const jarvis_ontology_t *ont = jarvis_engine_get_ontology(eng);
			char wake[64], ntitle[128], title[256], body[1024];
			get_cli_wake_word(ont, wake, sizeof(wake));
			jarvis_ontology_format_ui(ont, "notify_title", wake, "  {Wake}", ntitle, sizeof(ntitle));
			snprintf(title, sizeof(title), "%s → %s (%s)",
			         ntitle,
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
