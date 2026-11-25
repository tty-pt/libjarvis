#define _GNU_SOURCE
#include "../include/ttypt/jarvis.h"
#include "../include/ttypt/desktop-core.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void test_01_text_normalization_and_safety(void)
{
	printf("Running test_01_text_normalization_and_safety...\n");
	char buf[256];

	jarvis_clean_phrase("  Olá, Jarbas!   Ação rápida  ", buf, sizeof(buf));
	assert(strcmp(buf, "ola jarbas acao rapida") == 0);

	jarvis_clean_phrase("Let's switch to workspace 2!", buf, sizeof(buf));
	assert(strcmp(buf, "lets switch to workspace 2") == 0);

	assert(jarvis_clean_phrase(NULL, buf, sizeof(buf)) == 0);

	assert(jarvis_is_non_speech_hallucination("vvvvvvvvvvvvvvvv"));
	assert(jarvis_is_non_speech_hallucination("v v v v v"));
	assert(jarvis_is_non_speech_hallucination("..."));
	assert(jarvis_is_non_speech_hallucination("Jajajaja"));
	assert(jarvis_is_non_speech_hallucination("Jajajajajajajaja"));
	assert(jarvis_is_non_speech_hallucination("hahahahahahaha"));
	assert(jarvis_is_non_speech_hallucination("abcabcabcabc"));
	assert(jarvis_is_non_speech_hallucination("abcdabcdabcd"));
	assert(jarvis_is_non_speech_hallucination("のののののののの"));
	assert(jarvis_is_non_speech_hallucination("あいうあいうあいうあいう"));
	assert(!jarvis_is_non_speech_hallucination("open a terminal"));
	assert(!jarvis_is_non_speech_hallucination("abre a calculadora"));
	assert(!jarvis_is_non_speech_hallucination("banana banana"));

	assert(jarvis_is_destructive_command("rm -rf /tmp/build"));
	assert(jarvis_is_destructive_command("git reset --hard HEAD~1"));
	assert(jarvis_is_destructive_command("mkfs.ext4 /dev/sda1"));
	assert(jarvis_is_destructive_command("kill -9 1234"));
	assert(!jarvis_is_destructive_command("ls -la /tmp"));
	assert(!jarvis_is_destructive_command("git status"));
	printf("  [PASS] Text normalization, hallucination, and safety checks!\n");
}

static void test_02_ontology_api(void)
{
	printf("Running test_02_ontology_api...\n");
	static const char *sample_json =
		"{"
		"  \"lang\": \"en\","
		"  \"lang_name\": \"English\","
		"  \"wake_word\": \"jarvis\","
		"  \"wake_variants\": [\"jarvis\", \"jarvas\"],"
		"  \"whisper_wake_prompt\": \"Jarvis\","
		"  \"whisper_cmd_prompt\": \"open terminal\","
		"  \"polite_prefixes\": [\"hey\", \"please\"],"
		"  \"tts_interrupt_phrases\": [\"stop talking\", \"{wake} be quiet\"],"
		"  \"op_abort_phrases\": [\"cancel\", \"abort\"],"
		"  \"lockdown_phrases\": [\"lockdown\", \"go to sleep\"],"
		"  \"screen_lock_keywords\": [\"lock screen\", \"slock\"],"
		"  \"tier1_triggers\": [\"explain\", \"summarize\"],"
		"  \"hallucination_phrases\": [\"thank you for watching\"],"
		"  \"stopwords\": [\"the\", \"to\", \"and\", \"for\"],"
		"  \"conjunctions\": [\"then\", \"after\"],"
		"  \"ui_strings\": {"
		"    \"active_title\": \"{Wake} Ready\","
		"    \"active_body\": \"Say {wake} anytime\""
		"  },"
		"  \"reply_templates\": {"
		"    \"ack\": \"Done.\""
		"  }"
		"}";

	jarvis_ontology_t *ont = jarvis_ontology_create_from_json(sample_json);
	assert(ont != NULL);
	assert(strcmp(jarvis_ontology_get_lang(ont), "en") == 0);
	assert(strcmp(jarvis_ontology_get_lang_name(ont), "English") == 0);
	assert(strcmp(jarvis_ontology_get_wake_word(ont), "jarvis") == 0);
	assert(strcmp(jarvis_ontology_get_wake_prompt(ont), "Jarvis") == 0);
	assert(strcmp(jarvis_ontology_get_cmd_prompt(ont), "open terminal") == 0);
	assert(strcmp(jarvis_ontology_get_reply_template(ont, "ack"), "Done.") == 0);

	char out[128];
	jarvis_ontology_format_ui(ont, "active_title", "jarbas", "Fallback", out, sizeof(out));
	assert(strcmp(out, "Jarbas Ready") == 0);
	jarvis_ontology_format_ui(ont, "active_body", "jarbas", "Fallback", out, sizeof(out));
	assert(strcmp(out, "Say jarbas anytime") == 0);
	jarvis_ontology_format_ui(ont, "missing_key", "jarbas", "Hi {Wake}", out, sizeof(out));
	assert(strcmp(out, "Hi Jarbas") == 0);

	assert(jarvis_ontology_is_tts_interrupt(ont, "stop talking"));
	assert(jarvis_ontology_is_tts_interrupt_ex(ont, "jarbas be quiet", "jarbas"));
	assert(jarvis_ontology_is_op_abort(ont, "cancel", "jarvis"));
	assert(jarvis_ontology_is_screen_lock_phrase(ont, "please lock screen"));
	assert(jarvis_ontology_matches_tier1_trigger(ont, "summarize this file"));
	assert(jarvis_is_non_speech_hallucination("thank you for watching"));
	jarvis_ontology_destroy(ont);

	jarvis_ontology_t *file_ont = jarvis_ontology_create("../data/ontology.json");
	assert(file_ont != NULL);
	assert(jarvis_ontology_get_wake_word(file_ont) != NULL);
	jarvis_ontology_destroy(file_ont);
	printf("  [PASS] Ontology JSON load, formatting, interrupts, and triggers!\n");
}

static void test_03_streaming_tts_and_rc(void)
{
	printf("Running test_03_streaming_tts_and_rc...\n");
	char ans[512];
	assert(jarvis_extract_streaming_answer_prefix(
		"{\"op\":\"answer_question\",\"args\":{\"answer\":\"Water boils at 100\\u00b0C.\\nNext",
		ans, sizeof(ans)));
	assert(strstr(ans, "Water boils at 100") != NULL);

	assert(jarvis_extract_streaming_answer_prefix(
		"{\"reply\": \"Tudo pronto!\"}", ans, sizeof(ans)));
	assert(strcmp(ans, "Tudo pronto!") == 0);

	char clause[256], rem[512];
	assert(jarvis_pop_ready_tts_clause(
		"Linux is a family of open-source operating systems based on the kernel.",
		true, clause, sizeof(clause), rem, sizeof(rem)));
	assert(strlen(clause) > 0 && strlen(rem) > 0);

	char clean[256];
	jarvis_tts_clean_text("Check **this** at https://example.com/a/b `now`", clean, sizeof(clean));
	assert(strstr(clean, "**") == NULL);
	assert(strstr(clean, "https://") == NULL);
	assert(strstr(clean, "example.com") != NULL);

	char segs[8][256];
	size_t nseg = jarvis_tts_split_segments("First sentence here. Second sentence follows!", segs, 8);
	assert(nseg >= 1);

	/* RC get/set/int with isolated temp file */
	char tmp_rc[] = "/tmp/test_jarvisrc_XXXXXX";
	int fd = mkstemp(tmp_rc);
	assert(fd >= 0);
	dprintf(fd, "# comment preserved\nwake_word = Jarbas\nmodel_path = ~/models/x\ntts_enabled = yes\n");
	close(fd);

	setenv("JARVISRC", tmp_rc, 1);
	char val[512];
	assert(jarvis_rc_get("wake_word", "fallback", val, sizeof(val)));
	assert(strcmp(val, "Jarbas") == 0);
	assert(jarvis_rc_get_int("tts_enabled", 0) == 1);
	assert(jarvis_rc_get("model_path", "", val, sizeof(val)));
	const char *home = getenv("HOME");
	if (home && *home) {
		assert(strncmp(val, home, strlen(home)) == 0);
	}
	assert(jarvis_rc_set("port", "9182"));
	assert(jarvis_rc_get_int("port", 8179) == 9182);
	assert(jarvis_rc_get("wake_word", "", val, sizeof(val)));
	assert(strcmp(val, "Jarbas") == 0);
	unsetenv("JARVISRC");
	unlink(tmp_rc);
	printf("  [PASS] Streaming TTS extraction/clause popping and RC helpers!\n");
}

static void test_04_vad_wake_and_earcon(void)
{
	printf("Running test_04_vad_wake_and_earcon...\n");
	float quiet_frame[JARVIS_FRAME_SAMPLES] = {0};
	float loud_frame[JARVIS_FRAME_SAMPLES];
	for (size_t i = 0; i < JARVIS_FRAME_SAMPLES; i++) {
		loud_frame[i] = 0.4f * sinf((float)i * 0.2f);
	}

	assert(jarvis_pcm_rms_db(quiet_frame, JARVIS_FRAME_SAMPLES) <= -90.0f);
	assert(jarvis_pcm_rms_db(loud_frame, JARVIS_FRAME_SAMPLES) > -20.0f);

	int16_t s16[8] = { 16384, -16384, 16384, -16384, 16384, -16384, 16384, -16384 };
	float f32[8] = {0};
	assert(jarvis_pcm_s16_to_f32_db(s16, f32, 8) > -10.0f);
	assert(fabsf(f32[0] - 0.5f) < 0.001f);

	jarvis_vad_t *vad = jarvis_vad_create(-50.0, -35.0);
	assert(vad != NULL);
	for (int i = 0; i < 5; i++) {
		assert(!jarvis_vad_push_frame(vad, quiet_frame, JARVIS_FRAME_SAMPLES));
	}
	assert(jarvis_vad_push_frame(vad, loud_frame, JARVIS_FRAME_SAMPLES));

	float pcm_out[JARVIS_FRAME_SAMPLES * 2];
	size_t got = jarvis_vad_get_samples(vad, 4, 6, pcm_out, JARVIS_FRAME_SAMPLES * 2);
	assert(got == JARVIS_FRAME_SAMPLES * 2);
	assert(fabsf(pcm_out[JARVIS_FRAME_SAMPLES + 1] - loud_frame[1]) < 0.0001f);
	jarvis_vad_destroy(vad);

	char cmd[256];
	assert(jarvis_voice_extract_wake_command("Hey Jarvis, open firefox", "jarvis", cmd, sizeof(cmd)));
	assert(strcmp(cmd, "open firefox") == 0);
	assert(jarvis_voice_match_wake(NULL, "jarvis", "jarvis", false, cmd, sizeof(cmd)) == 1);
	assert(jarvis_voice_match_wake(NULL, "please jarvis, check disk usage", "jarvis", false, cmd, sizeof(cmd)) == 2);
	assert(strcmp(cmd, "check disk usage") == 0);
	assert(jarvis_voice_match_wake(NULL, "what time is it", "jarvis", false, cmd, sizeof(cmd)) == 0);

	assert(jarvis_voice_is_lockdown_phrase("lockdown mode"));
	assert(jarvis_voice_is_lockdown_phrase("jarvis lockdown"));
	assert(jarvis_voice_is_lockdown_phrase_ex(NULL, "go to sleep", "jarbas"));
	assert(!jarvis_voice_is_lockdown_phrase("open a terminal"));

	char tmp_wav[128];
	snprintf(tmp_wav, sizeof(tmp_wav), "/tmp/test_earcon_%d.wav", (int)getpid());
	assert(jarvis_voice_write_earcon_wav(tmp_wav, false));
	struct stat st;
	assert(stat(tmp_wav, &st) == 0 && st.st_size > 44);
	FILE *wf = fopen(tmp_wav, "rb");
	assert(wf != NULL);
	char hdr[12] = {0};
	assert(fread(hdr, 1, 12, wf) == 12);
	fclose(wf);
	assert(memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WAVE", 4) == 0);
	unlink(tmp_wav);
	printf("  [PASS] VAD ring buffer, wake-word matcher, lockdown, and earcon WAV!\n");
}

static void test_05_desktop_core(void)
{
	printf("Running test_05_desktop_core...\n");
	char buf[512];

	desktop_core_url_encode("hello world+test", buf, sizeof(buf));
	assert(strcmp(buf, "hello+world%2Btest") == 0);

	desktop_core_json_escape("a\"b\\c\nd", buf, sizeof(buf));
	assert(strcmp(buf, "a\\\"b\\\\c\\nd") == 0);

	/* Known app resolution must accept real executables and reject arbitrary prose/shell injection */
	assert(desktop_core_resolve_known_app("sh", buf, sizeof(buf)));
	assert(!desktop_core_resolve_known_app("my dev server", buf, sizeof(buf)));
	assert(!desktop_core_resolve_known_app("firefox; rm -rf /", buf, sizeof(buf)));
	assert(!desktop_core_resolve_known_app("", buf, sizeof(buf)));

	/* Double-fork detached spawn returns success on valid binary, -1 on missing, and leaves zero zombies */
	char *const ok_argv[] = { "true", NULL };
	assert(desktop_core_spawn_detached_argv(ok_argv) > 0);

	char *const bad_argv[] = { "/nonexistent/jarvis_missing_bin_12345", NULL };
	assert(desktop_core_spawn_detached_argv(bad_argv) < 0);

	int wst = 0;
	pid_t reaped = waitpid(-1, &wst, WNOHANG);
	assert(reaped == -1 && errno == ECHILD);

	char health[256] = {0};
	desktop_core_populate_system_health(health, sizeof(health));
	assert(strlen(health) > 10);

	/* Process-tree parsers & live POSIX child PID traversal */
	unsigned long pids[4] = {0};
	assert(desktop_core_parse_pid_lines("101\n202\n 303  404\t\n", pids, 4) == 4);
	assert(pids[0] == 101 && pids[1] == 202 && pids[2] == 303 && pids[3] == 404);
	assert(desktop_core_parse_pid_lines("0 abc 12x 55 66 77", pids, 2) == 2);
	assert(pids[0] == 55 && pids[1] == 66);
	assert(desktop_core_parse_pid_lines("", pids, 4) == 0);
	assert(desktop_core_parse_pid_lines(NULL, pids, 4) == 0);

	char comm[64];
	desktop_core_extract_comm_basename("/usr/bin/ssh\n", comm, sizeof(comm));
	assert(strcmp(comm, "ssh") == 0);
	desktop_core_extract_comm_basename("  /bin/zsh  ", comm, sizeof(comm));
	assert(strcmp(comm, "zsh") == 0);
	desktop_core_extract_comm_basename("-zsh\r\n", comm, sizeof(comm));
	assert(strcmp(comm, "-zsh") == 0);
	desktop_core_extract_comm_basename("/usr/bin/", comm, sizeof(comm));
	assert(strcmp(comm, "") == 0);
	desktop_core_extract_comm_basename(NULL, comm, sizeof(comm));
	assert(strcmp(comm, "") == 0);

	int sync_pipe[2];
	assert(pipe(sync_pipe) == 0);
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		close(sync_pipe[1]);
		char b;
		ssize_t r = read(sync_pipe[0], &b, 1);
		(void)r;
		close(sync_pipe[0]);
		_exit(0);
	}
	close(sync_pipe[0]);
	unsigned long ch[16] = {0};
	size_t n_ch = desktop_core_get_child_pids((unsigned long)getpid(), ch, 16);
	bool found_child = false;
	for (size_t i = 0; i < n_ch; i++) {
		if (ch[i] == (unsigned long)child) found_child = true;
	}
	assert(found_child);
	assert(desktop_core_pid_is_descendant((unsigned long)getpid(), (unsigned long)child));
	assert(!desktop_core_pid_is_descendant((unsigned long)child, (unsigned long)getpid()));
	close(sync_pipe[1]);
	while (waitpid(child, &wst, 0) < 0 && errno == EINTR) {}

	/* UTF-8 safe truncation & copy helpers */
	char u8[8];
	desktop_core_utf8_copy(u8, 4, "a\xC3\xA3" "b"); /* "aãb": 'a' + 2-byte 'ã', cap 4 bytes -> "aã" */
	assert(strcmp(u8, "a\xC3\xA3") == 0);
	desktop_core_utf8_copy(u8, 3, "a\xC3\xA3" "b"); /* cap 3 bytes splits 'ã' -> "a" */
	assert(strcmp(u8, "a") == 0);
	desktop_core_utf8_copy(u8, 4, "\xE2\x82\xAC" "x"); /* 3-byte Euro '€', cap 4 bytes -> "€" */
	assert(strcmp(u8, "\xE2\x82\xAC") == 0);
	desktop_core_utf8_copy(u8, 3, "\xE2\x82\xAC" "x"); /* cap 3 bytes splits '€' -> "" */
	assert(strcmp(u8, "") == 0);
	desktop_core_utf8_copy(u8, 5, "\xF0\x9F\x98\x80" "!"); /* 4-byte emoji '😀', cap 5 -> "😀" */
	assert(strcmp(u8, "\xF0\x9F\x98\x80") == 0);
	desktop_core_utf8_copy(u8, 4, "\xF0\x9F\x98\x80" "!"); /* cap 4 splits '😀' -> "" */
	assert(strcmp(u8, "") == 0);
	desktop_core_utf8_copy(u8, sizeof(u8), NULL);
	assert(strcmp(u8, "") == 0);

	printf("  [PASS] desktop-core.h escaping, safe app resolution, process parsers, UTF-8 truncation, and zombie-free spawn!\n");
}

static void test_06_zero_config_and_locales(void)
{
	printf("Running test_06_zero_config_and_locales...\n");

	/* Save relevant env vars */
	const char *keys[] = {
		"JARVISRC", "JARVIS_LANG", "JARVIS_WAKE_WORD", "JARVIS_VOICE",
		"JARVIS_TTS_LANG", "JARVIS_WM", "LC_ALL", "LC_MESSAGES", "LANG",
		"HYPRLAND_INSTANCE_SIGNATURE", "SWAYSOCK", "XDG_CURRENT_DESKTOP",
		"DESKTOP_SESSION", "DISPLAY"
	};
	char *saved[14];
	for (size_t i = 0; i < 14; i++) {
		const char *v = getenv(keys[i]);
		saved[i] = v ? strdup(v) : NULL;
		unsetenv(keys[i]);
	}

	/* 1. Locale overrides inside data/ontology.json */
	jarvis_ontology_t *ont_en = jarvis_ontology_create_for_lang("../data/ontology.json", "en");
	assert(ont_en != NULL);
	assert(strcmp(jarvis_ontology_get_lang(ont_en), "en") == 0);
	assert(strcmp(jarvis_ontology_get_wake_word(ont_en), "jarvis") == 0);
	assert(strcmp(jarvis_ontology_get_cmd_prompt(ont_en), "") == 0);
	assert(strcmp(jarvis_ontology_get_reply_template(ont_en, "default"), "Done.") == 0);
	assert(strcmp(jarvis_ontology_get_ui_string(ont_en, "menu_voice_auto", ""),
	              "auto                         (Automatic neural voice for active language)") == 0);

	jarvis_ontology_t *ont_pt = jarvis_ontology_create_for_lang("../data/ontology.json", "pt-PT");
	assert(ont_pt != NULL);
	assert(strcmp(jarvis_ontology_get_lang(ont_pt), "pt") == 0);
	assert(strcmp(jarvis_ontology_get_wake_word(ont_pt), "jarbas") == 0);
	assert(strcmp(jarvis_ontology_get_cmd_prompt(ont_pt), "") == 0);
	assert(strcmp(jarvis_ontology_get_reply_template(ont_pt, "default"), "Concluído.") == 0);
	assert(strcmp(jarvis_ontology_get_ui_string(ont_pt, "thinking", ""), "A pensar...") == 0);

	/* 2. Zero ~/.jarvisrc defaults driven solely by system locale */
	char missing_rc[128];
	snprintf(missing_rc, sizeof(missing_rc), "/tmp/test_missing_jarvisrc_%d", (int)getpid());
	unlink(missing_rc);
	setenv("JARVISRC", missing_rc, 1);

	char buf[128];
	setenv("LC_ALL", "pt_PT.UTF-8", 1);
	jarvis_resolve_lang(ont_en, buf, sizeof(buf));
	assert(strcmp(buf, "pt-pt") == 0);
	jarvis_resolve_wake_word(NULL, buf, sizeof(buf));
	assert(strcmp(buf, "jarbas") == 0);
	jarvis_resolve_tts_voice(NULL, buf, sizeof(buf));
	assert(strcmp(buf, "edge:pt-PT-RaquelNeural") == 0);

	setenv("LC_ALL", "en_US.UTF-8", 1);
	jarvis_resolve_lang(ont_en, buf, sizeof(buf));
	assert(strcmp(buf, "en-us") == 0);
	jarvis_resolve_wake_word(NULL, buf, sizeof(buf));
	assert(strcmp(buf, "jarvis") == 0);
	jarvis_resolve_tts_voice(NULL, buf, sizeof(buf));
	assert(strcmp(buf, "edge:en-US-AndrewNeural") == 0);

	/* 3. One-line lang switch in JARVISRC + auto TTS voice + jarvis_rc_unset */
	char tmp_rc[] = "/tmp/test_one_line_rc_XXXXXX";
	int fd = mkstemp(tmp_rc);
	assert(fd >= 0);
	dprintf(fd, "lang = pt\ntts_lang = en-us\ntts_voice = auto\n");
	close(fd);
	setenv("JARVISRC", tmp_rc, 1);

	assert(jarvis_rc_unset("tts_lang"));
	assert(!jarvis_rc_get("tts_lang", NULL, buf, sizeof(buf)));
	jarvis_resolve_lang(NULL, buf, sizeof(buf));
	assert(strcmp(buf, "pt") == 0);
	jarvis_resolve_wake_word(NULL, buf, sizeof(buf));
	assert(strcmp(buf, "jarbas") == 0);
	jarvis_resolve_tts_voice(NULL, buf, sizeof(buf));
	assert(strcmp(buf, "edge:pt-PT-RaquelNeural") == 0);

	assert(jarvis_rc_set("lang", "en"));
	jarvis_resolve_lang(NULL, buf, sizeof(buf));
	assert(strcmp(buf, "en") == 0);
	jarvis_resolve_wake_word(NULL, buf, sizeof(buf));
	assert(strcmp(buf, "jarvis") == 0);
	jarvis_resolve_tts_voice(NULL, buf, sizeof(buf));
	assert(strcmp(buf, "edge:en-GB-RyanNeural") == 0);
	unlink(tmp_rc);

	/* 4. Zero-config WM auto-detection */
	setenv("JARVISRC", missing_rc, 1);
	setenv("HYPRLAND_INSTANCE_SIGNATURE", "abc", 1);
	jarvis_resolve_wm(buf, sizeof(buf));
	assert(strcmp(buf, "hyprland") == 0);
	unsetenv("HYPRLAND_INSTANCE_SIGNATURE");
	setenv("SWAYSOCK", "/run/user/1000/sway.sock", 1);
	jarvis_resolve_wm(buf, sizeof(buf));
	assert(strcmp(buf, "sway") == 0);
	unsetenv("SWAYSOCK");
	setenv("XDG_CURRENT_DESKTOP", "dwm", 1);
	jarvis_resolve_wm(buf, sizeof(buf));
	assert(strcmp(buf, "dwm") == 0);

	jarvis_ontology_destroy(ont_en);
	jarvis_ontology_destroy(ont_pt);

	/* Restore env vars */
	for (size_t i = 0; i < 14; i++) {
		if (saved[i]) {
			setenv(keys[i], saved[i], 1);
			free(saved[i]);
		} else {
			unsetenv(keys[i]);
		}
	}
	printf("  [PASS] Locale overrides, zero-config resolution, and one-line switch!\n");
}

static void test_07_multilang_wake_ack(void)
{
	printf("Running test_07_multilang_wake_ack...\n");

	const char *keys[] = {
		"JARVISRC", "JARVIS_LANG", "JARVIS_WAKE_ACK", "JARVIS_VOICE",
		"LC_ALL", "LC_MESSAGES", "LANG"
	};
	char *saved[7];
	for (size_t i = 0; i < 7; i++) {
		const char *v = getenv(keys[i]);
		saved[i] = v ? strdup(v) : NULL;
		unsetenv(keys[i]);
	}

	jarvis_ontology_t *ont = jarvis_ontology_create_for_lang("../data/ontology.json", "en");
	assert(ont != NULL);

	/* 1. Per-language lookup from ontology.json without hardcoding in C */
	assert(strcmp(jarvis_ontology_get_wake_ack_for_lang(ont, "en"), "Yes?") == 0);
	assert(strcmp(jarvis_ontology_get_wake_ack_for_lang(ont, "en-US"), "Yes?") == 0);
	assert(strcmp(jarvis_ontology_get_wake_ack_for_lang(ont, "pt"), "Sim?") == 0);
	assert(strcmp(jarvis_ontology_get_wake_ack_for_lang(ont, "pt-PT"), "Sim?") == 0);
	assert(strcmp(jarvis_ontology_get_wake_ack_for_lang(ont, "pt_BR"), "Sim?") == 0);
	assert(strcmp(jarvis_ontology_get_wake_ack_for_lang(ont, "es"), "¿Sí?") == 0);
	assert(strcmp(jarvis_ontology_get_wake_ack_for_lang(ont, "fr"), "Oui ?") == 0);
	assert(strcmp(jarvis_ontology_get_wake_ack_for_lang(ont, "de"), "Ja?") == 0);
	assert(strcmp(jarvis_ontology_get_wake_ack_for_lang(ont, "it"), "Sì?") == 0);
	assert(strcmp(jarvis_ontology_get_wake_ack_for_lang(ont, "ja"), "はい？") == 0);
	assert(strcmp(jarvis_ontology_get_wake_ack_for_lang(ont, "zh-CN"), "嗯？") == 0);
	/* Unknown language falls back cleanly to active ontology wake_ack ("Yes?") */
	assert(strcmp(jarvis_ontology_get_wake_ack_for_lang(ont, "zz"), "Yes?") == 0);

	/* 2. jarvis_resolve_wake_ack honors lang_hint, active config, env override, and opt-out */
	char tmp_rc[] = "/tmp/test_wake_ack_rc_XXXXXX";
	int fd = mkstemp(tmp_rc);
	assert(fd >= 0);
	dprintf(fd, "lang = pt-pt\n");
	close(fd);
	setenv("JARVISRC", tmp_rc, 1);

	char buf[128];
	assert(jarvis_resolve_wake_ack(ont, NULL, buf, sizeof(buf)));
	assert(strcmp(buf, "Sim?") == 0);
	/* Explicit Whisper auto-detected language hint overrides default */
	assert(jarvis_resolve_wake_ack(ont, "fr", buf, sizeof(buf)));
	assert(strcmp(buf, "Oui ?") == 0);
	assert(jarvis_resolve_wake_ack(ont, "es", buf, sizeof(buf)));
	assert(strcmp(buf, "¿Sí?") == 0);

	/* RC custom override, opt-out, and auto fallthrough */
	assert(jarvis_rc_set("wake_ack", "Pois não?"));
	assert(jarvis_resolve_wake_ack(ont, "en", buf, sizeof(buf)));
	assert(strcmp(buf, "Pois não?") == 0);

	assert(jarvis_rc_set("wake_ack", "off"));
	assert(!jarvis_resolve_wake_ack(ont, "pt", buf, sizeof(buf)));
	assert(buf[0] == '\0');

	assert(jarvis_rc_set("wake_ack", "auto"));
	assert(jarvis_resolve_wake_ack(ont, "de", buf, sizeof(buf)));
	assert(strcmp(buf, "Ja?") == 0);

	/* Environment variable override takes precedence over RC */
	setenv("JARVIS_WAKE_ACK", "Listening?", 1);
	assert(jarvis_resolve_wake_ack(ont, "pt", buf, sizeof(buf)));
	assert(strcmp(buf, "Listening?") == 0);

	setenv("JARVIS_WAKE_ACK", "none", 1);
	assert(!jarvis_resolve_wake_ack(ont, "pt", buf, sizeof(buf)));
	assert(buf[0] == '\0');
	unsetenv("JARVIS_WAKE_ACK");

	/* Multi-language auto TTS voice mapping */
	assert(jarvis_rc_set("lang", "es"));
	jarvis_resolve_tts_voice(NULL, buf, sizeof(buf));
	assert(strcmp(buf, "edge:es-ES-AlvaroNeural") == 0);
	assert(jarvis_rc_set("lang", "fr"));
	jarvis_resolve_tts_voice(NULL, buf, sizeof(buf));
	assert(strcmp(buf, "edge:fr-FR-HenriNeural") == 0);
	assert(jarvis_rc_set("lang", "ja"));
	jarvis_resolve_tts_voice(NULL, buf, sizeof(buf));
	assert(strcmp(buf, "edge:ja-JP-KeitaNeural") == 0);

	unlink(tmp_rc);
	jarvis_ontology_destroy(ont);

	for (size_t i = 0; i < 7; i++) {
		if (saved[i]) {
			setenv(keys[i], saved[i], 1);
			free(saved[i]);
		} else {
			unsetenv(keys[i]);
		}
	}
	printf("  [PASS] Multi-language wake acks, overrides, opt-out, and TTS voices!\n");
}

static void test_08_continuation_tokens(void)
{
	printf("Running test_08_continuation_tokens...\n");

	jarvis_ontology_t *ont_en = jarvis_ontology_create_for_lang("../data/ontology.json", "en");
	assert(ont_en != NULL);
	assert(jarvis_ontology_ends_with_continuation(ont_en, "open the file to"));
	assert(jarvis_ontology_ends_with_continuation(ont_en, "search the logs with..."));
	assert(jarvis_ontology_ends_with_continuation(ont_en, "send email for,"));
	assert(jarvis_ontology_ends_with_continuation(ont_en, "play music and"));
	assert(!jarvis_ontology_ends_with_continuation(ont_en, "open a terminal."));
	assert(!jarvis_ontology_ends_with_continuation(ont_en, "close firefox"));
	assert(!jarvis_ontology_ends_with_continuation(ont_en, ""));
	jarvis_ontology_destroy(ont_en);

	jarvis_ontology_t *ont_pt = jarvis_ontology_create_for_lang("../data/ontology.json", "pt-PT");
	assert(ont_pt != NULL);
	assert(jarvis_ontology_ends_with_continuation(ont_pt, "abre o ficheiro para"));
	assert(jarvis_ontology_ends_with_continuation(ont_pt, "fala com..."));
	assert(jarvis_ontology_ends_with_continuation(ont_pt, "mostra a lista de"));
	assert(jarvis_ontology_ends_with_continuation(ont_pt, "acho que,"));
	assert(!jarvis_ontology_ends_with_continuation(ont_pt, "abre o terminal."));
	assert(!jarvis_ontology_ends_with_continuation(ont_pt, "fecha a janela"));
	jarvis_ontology_destroy(ont_pt);

	jarvis_ontology_t *ont_es = jarvis_ontology_create_for_lang("../data/ontology.json", "es");
	assert(ont_es != NULL);
	assert(jarvis_ontology_ends_with_continuation(ont_es, "abre el archivo para"));
	assert(jarvis_ontology_ends_with_continuation(ont_es, "busca con"));
	assert(!jarvis_ontology_ends_with_continuation(ont_es, "cierra el navegador"));
	jarvis_ontology_destroy(ont_es);

	jarvis_ontology_t *ont_fr = jarvis_ontology_create_for_lang("../data/ontology.json", "fr");
	assert(ont_fr != NULL);
	assert(jarvis_ontology_ends_with_continuation(ont_fr, "ouvre le fichier pour"));
	assert(jarvis_ontology_ends_with_continuation(ont_fr, "cherche avec"));
	assert(!jarvis_ontology_ends_with_continuation(ont_fr, "ferme la fenetre"));
	jarvis_ontology_destroy(ont_fr);

	jarvis_ontology_t *ont_de = jarvis_ontology_create_for_lang("../data/ontology.json", "de");
	assert(ont_de != NULL);
	assert(jarvis_ontology_ends_with_continuation(ont_de, "offne die datei mit"));
	assert(jarvis_ontology_ends_with_continuation(ont_de, "suche fur"));
	assert(!jarvis_ontology_ends_with_continuation(ont_de, "schliesse das fenster"));
	jarvis_ontology_destroy(ont_de);

	jarvis_ontology_t *ont_ja = jarvis_ontology_create_for_lang("../data/ontology.json", "ja");
	assert(ont_ja != NULL);
	assert(jarvis_ontology_ends_with_continuation(ont_ja, "ターミナルを"));
	assert(!jarvis_ontology_ends_with_continuation(ont_ja, "ターミナル開く"));
	jarvis_ontology_destroy(ont_ja);

	printf("  [PASS] Locale-driven continuation_tokens verified across en, pt-PT, es, fr, de, ja!\n");
}

static void test_09_regional_dictation_seeds(void)
{
	printf("[TEST 09] Regional dialect dictation_seed & base-locale inheritance...\n");

	jarvis_ontology_t *ont_pt_br = jarvis_ontology_create_for_lang("../data/ontology.json", "pt-BR");
	assert(ont_pt_br != NULL);
	/* Must inherit base "pt" lang, wake_word & continuation_tokens while applying pt-BR seed */
	assert(strcmp(jarvis_ontology_get_lang(ont_pt_br), "pt") == 0);
	assert(strcmp(jarvis_ontology_get_wake_word(ont_pt_br), "jarbas") == 0);
	assert(jarvis_ontology_ends_with_continuation(ont_pt_br, "abre para"));

	const char *s_pt_pt = jarvis_ontology_get_dictation_seed_for_lang(ont_pt_br, "pt-PT");
	const char *s_pt_br = jarvis_ontology_get_dictation_seed_for_lang(ont_pt_br, "pt-BR");
	const char *s_pt_def = jarvis_ontology_get_dictation_seed_for_lang(ont_pt_br, NULL);
	assert(s_pt_pt && strstr(s_pt_pt, "registar") != NULL);
	assert(s_pt_br && strstr(s_pt_br, "registrar") != NULL);
	assert(s_pt_def && strcmp(s_pt_def, s_pt_br) == 0);

	const char *s_en_gb = jarvis_ontology_get_dictation_seed_for_lang(ont_pt_br, "en_GB.UTF-8");
	const char *s_en_us = jarvis_ontology_get_dictation_seed_for_lang(ont_pt_br, "en-US");
	assert(s_en_gb && strstr(s_en_gb, "colour") != NULL);
	assert(s_en_us && strstr(s_en_us, "color") != NULL);

	const char *s_es_es = jarvis_ontology_get_dictation_seed_for_lang(ont_pt_br, "es-ES");
	const char *s_es_mx = jarvis_ontology_get_dictation_seed_for_lang(ont_pt_br, "es-MX");
	assert(s_es_es && strstr(s_es_es, "ordenador") != NULL);
	assert(s_es_mx && strstr(s_es_mx, "computadora") != NULL);

	const char *s_zh_cn = jarvis_ontology_get_dictation_seed_for_lang(ont_pt_br, "zh-CN");
	const char *s_zh_tw = jarvis_ontology_get_dictation_seed_for_lang(ont_pt_br, "zh-TW");
	assert(s_zh_cn && s_zh_tw && strcmp(s_zh_cn, s_zh_tw) != 0);

	/* Never bias auto-detection or leak a wrong language seed */
	assert(jarvis_ontology_get_dictation_seed_for_lang(ont_pt_br, "auto") == NULL);
	assert(jarvis_ontology_get_dictation_seed_for_lang(ont_pt_br, "ja") == NULL);

	jarvis_ontology_destroy(ont_pt_br);
	printf("  [PASS] Regional dictation_seed and base-locale inheritance verified!\n");
}

int main(void)
{
	test_01_text_normalization_and_safety();
	test_02_ontology_api();
	test_03_streaming_tts_and_rc();
	test_04_vad_wake_and_earcon();
	test_05_desktop_core();
	test_06_zero_config_and_locales();
	test_07_multilang_wake_ack();
	test_08_continuation_tokens();
	test_09_regional_dictation_seeds();
	printf("\nALL libjarvis unit tests passed (9/9 suites)!\n");
	return 0;
}
