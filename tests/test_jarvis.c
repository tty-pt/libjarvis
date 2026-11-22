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
	assert(!jarvis_is_non_speech_hallucination("open a terminal"));

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
	printf("  [PASS] desktop-core.h escaping, safe app resolution, and zombie-free spawn!\n");
}

int main(void)
{
	test_01_text_normalization_and_safety();
	test_02_ontology_api();
	test_03_streaming_tts_and_rc();
	test_04_vad_wake_and_earcon();
	test_05_desktop_core();
	printf("\nALL libjarvis unit tests passed (5/5 suites)!\n");
	return 0;
}
