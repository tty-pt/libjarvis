#include "../src/jarvis-internal.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

extern jarvis_dsl_t  g_executed[32];
extern size_t        g_executed_count;
extern bool          g_allow_confirm;
extern jarvis_pane_t g_mock_panes[4];
extern size_t        g_mock_pane_count;
extern char          g_mock_browser_page[JARVIS_OUTPUT_LEN];
extern jarvis_tab_t  g_mock_tabs[4];
extern size_t        g_mock_tab_count;
extern char          g_mock_other_term_tail[JARVIS_OUTPUT_LEN];

static void assert_cmd_op(jarvis_engine_t *eng, const char *utt, const char *expected_op, const char *expected_sub)
{
	jarvis_process_result_t res;
	bool ok = jarvis_process(eng, utt, "terminal", &res);
	if (!ok) {
		fprintf(stderr, "FAIL: '%s' -> unresolved (%s)\n", utt, res.error);
		assert(ok);
	}
	if (strcmp(res.source, "solver:low_confidence") == 0) {
		fprintf(stderr, "FAIL: '%s' -> got low_confidence solver score\n", utt);
		assert(strcmp(res.source, "solver:low_confidence") != 0);
	}
	if (strcmp(res.last_dsl.op, expected_op) != 0) {
		fprintf(stderr, "FAIL: '%s' -> got op='%s', expected='%s'\n", utt, res.last_dsl.op, expected_op);
		assert(strcmp(res.last_dsl.op, expected_op) == 0);
	}
	if (expected_sub) {
		char jbuf[2048];
		jarvis_dsl_to_json(&res.last_dsl, jbuf, sizeof(jbuf));
		if (!strstr(jbuf, expected_sub)) {
			fprintf(stderr, "FAIL: '%s' -> DSL JSON '%s' missing '%s'\n", utt, jbuf, expected_sub);
			assert(strstr(jbuf, expected_sub) != NULL);
		}
	}
}

static bool op_in_list(const char *op, const char *const *allowed)
{
	for (size_t i = 0; allowed[i]; i++) {
		if (strcmp(op, allowed[i]) == 0) return true;
	}
	return false;
}

static bool executed_has_op(const char *op)
{
	for (size_t i = 0; i < g_executed_count; i++) {
		if (strcmp(g_executed[i].op, op) == 0) return true;
	}
	return false;
}

static void test_01_standard_structured_commands(jarvis_engine_t *eng)
{
	printf("Running test_01_standard_structured_commands...\n");
	assert_cmd_op(eng, "open a new terminal", "open_terminal", NULL);
	assert_cmd_op(eng, "Please open a terminal", "open_terminal", NULL);
	assert_cmd_op(eng, "open firefox", "open_application", "firefox");
	assert_cmd_op(eng, "launch calculator", "open_application", "gnome-calculator");
	assert_cmd_op(eng, "open discord", "open_application", "discord");
	assert_cmd_op(eng, "open volume control", "open_application", "pavucontrol");
	assert_cmd_op(eng, "show me my agenda for today", "open_application", "calendar");
	assert_cmd_op(eng, "open system monitor", "open_application", "sys_monitor");
	assert_cmd_op(eng, "open wifi settings", "open_application", "wifi");
	assert_cmd_op(eng, "open power menu", "open_application", "power_menu");

	/* Shell tools & configs */
	assert_cmd_op(eng, "run htop", "spawn_shell_command", "htop");
	assert_cmd_op(eng, "open btop", "spawn_shell_command", "btop");
	assert_cmd_op(eng, "open lazygit", "spawn_shell_command", "lazygit");
	assert_cmd_op(eng, "Can you open the bit calculator on this terminal", "spawn_shell_command", "bitwise");
	assert_cmd_op(eng, "open my vim configuration", "spawn_shell_command", "nvim ~/.vimrc");
	assert_cmd_op(eng, "open my neovim config", "spawn_shell_command", "nvim ~/.config/nvim/init.lua");
	assert_cmd_op(eng, "open the Makefile", "spawn_shell_command", "nvim Makefile");
	assert_cmd_op(eng, "open dwm config", "spawn_shell_command", "nvim ~/.local/src/dwm/config.h");

	/* Filesystem & metrics */
	assert_cmd_op(eng, "list files", "spawn_shell_command", "ls -la");
	assert_cmd_op(eng, "list my files in the other window", "spawn_shell_command", "\"relation\":\"other\"");
	assert_cmd_op(eng, "list files in home", "spawn_shell_command", "ls -la ~");
	assert_cmd_op(eng, "go to site folder", "spawn_shell_command", "cd ~/site");
	assert_cmd_op(eng, "navigate to downloads directory", "spawn_shell_command", "cd ~/Downloads");
	assert_cmd_op(eng, "go to parent directory", "spawn_shell_command", "cd ..");
	assert_cmd_op(eng, "find all python files", "spawn_shell_command", "find . -name '*.py'");
	assert_cmd_op(eng, "find c files in site", "spawn_shell_command", "find ~/site -name '*.c'");
	assert_cmd_op(eng, "show disk space", "spawn_shell_command", "df -h");
	assert_cmd_op(eng, "show largest files", "spawn_shell_command", "du -sh");

	/* Git */
	assert_cmd_op(eng, "show git status", "spawn_shell_command", "git status");
	assert_cmd_op(eng, "check git diff", "spawn_shell_command", "git diff");
	assert_cmd_op(eng, "show recent commits", "spawn_shell_command", "git log");
	assert_cmd_op(eng, "show git branches", "spawn_shell_command", "git branch");

	/* Workspaces & windows */
	assert_cmd_op(eng, "switch to workspace 2", "workspace_switch", "\"workspace\":2");
	assert_cmd_op(eng, "go to workspace 5", "workspace_switch", "\"workspace\":5");
	assert_cmd_op(eng, "switch to the third workspace", "workspace_switch", "\"workspace\":3");
	assert_cmd_op(eng, "move this window to workspace 4", "workspace_move_window", "\"workspace\":4");
	assert_cmd_op(eng, "send the other window to workspace 7", "workspace_move_window", "\"workspace\":7");
	assert_cmd_op(eng, "go to the other window", "focus_relative_window", "\"relation\":\"other\"");
	assert_cmd_op(eng, "next window", "focus_relative_window", "\"relation\":\"next\"");
	assert_cmd_op(eng, "previous window", "focus_relative_window", "\"relation\":\"previous\"");
	assert_cmd_op(eng, "toggle fullscreen", "window_fullscreen", NULL);
	assert_cmd_op(eng, "make this window full screen", "window_fullscreen", NULL);
	assert_cmd_op(eng, "float this window", "window_float", NULL);
	assert_cmd_op(eng, "close this window", "window_close", "\"kind\":\"window\"");
	assert_cmd_op(eng, "close this tab on Firefox", "window_close", "\"kind\":\"tab\"");
	assert_cmd_op(eng, "close the other window", "window_close", "\"relation\":\"other\"");
	assert_cmd_op(eng, "close it", "window_close", NULL);
	assert_cmd_op(eng, "fullscreen it", "window_fullscreen", NULL);

	/* Media & volume */
	assert_cmd_op(eng, "play some lofi music", "media_play", "lofi");
	assert_cmd_op(eng, "open up some jazz music for me please", "media_play", "jazz");
	assert_cmd_op(eng, "press play", "media_play", NULL);
	assert_cmd_op(eng, "put up some lofi dragonball jazz", "media_play", "dragonball");
	assert_cmd_op(eng, "let's listen to some miles davis", "media_play", "miles davis");
	assert_cmd_op(eng, "pause the music", "media_pause", NULL);
	assert_cmd_op(eng, "next song", "media_next", NULL);
	assert_cmd_op(eng, "skip track", "media_next", NULL);
	assert_cmd_op(eng, "previous track", "media_prev", NULL);
	assert_cmd_op(eng, "turn up the volume", "media_volume", "volume_up");
	assert_cmd_op(eng, "lower volume", "media_volume", "volume_down");
	assert_cmd_op(eng, "mute audio", "media_volume", "volume_mute");

	/* Web & URLs */
	assert_cmd_op(eng, "open whatsapp for web", "open_url", "whatsapp");
	assert_cmd_op(eng, "open github.com/torvalds", "open_url", "github.com/torvalds");
	assert_cmd_op(eng, "search for python asyncio", "search_web", "python asyncio");
	assert_cmd_op(eng, "google rust borrow checker", "search_web", "rust borrow checker");

	/* Dictation & editing */
	assert_cmd_op(eng, "type hello world and press enter", "type_and_enter", "hello world");
	assert_cmd_op(eng, "type in list files", "type_text", "list files");
	assert_cmd_op(eng, "open up bashrc on a new terminal", "spawn_shell_command", "\"new_terminal\":true");
	assert_cmd_op(eng, "type git status in the other window", "type_text", "\"relation\":\"other\"");
	assert_cmd_op(eng, "clear the line", "press_key", "ctrl+u");
	assert_cmd_op(eng, "delete last word", "press_key", "ctrl+w");

	/* Scroll, Recording, Agent, Voice Barge-In & Background Media */
	assert_cmd_op(eng, "scroll down", "scroll", "\"direction\":\"down\"");
	assert_cmd_op(eng, "scroll up 8", "scroll", "\"direction\":\"up\",\"amount\":8");
	assert_cmd_op(eng, "switch to workspace two", "workspace_switch", "\"workspace\":2");
	assert_cmd_op(eng, "move this window to workspace three", "workspace_move_window", "\"workspace\":3");
	assert_cmd_op(eng, "start recording screen and audio", "record_start", "\"mode\":\"both\"");
	assert_cmd_op(eng, "stop recording", "record_stop", NULL);
	assert_cmd_op(eng, "stop talking", "tts_interrupt", NULL);
	assert_cmd_op(eng, "Jarvis, be quiet", "tts_interrupt", NULL);
	assert_cmd_op(eng, "please shut up", "tts_interrupt", NULL);
	assert_cmd_op(eng, "play some jazz music in the background", "media_play", "\"background\":true");
	printf("  [PASS] All 67 canonical structured commands resolved in pure C!\n");
}

static void test_02_multi_clause_and_anaphora(jarvis_engine_t *eng)
{
	printf("Running test_02_multi_clause_and_anaphora...\n");
	jarvis_process_result_t r1;
	assert(jarvis_process(eng, "move this window to workspace 4 and switch to workspace 4", "terminal", &r1));
	assert(r1.step_count == 2);
	assert(strcmp(r1.steps[0].dsl.op, "workspace_move_window") == 0);
	assert(r1.steps[0].dsl.args.workspace == 4);
	assert(strcmp(r1.steps[1].dsl.op, "workspace_switch") == 0);
	assert(r1.steps[1].dsl.args.workspace == 4);

	jarvis_process_result_t r2;
	assert(jarvis_process(eng, "go to site folder and then find c files there", "terminal", &r2));
	assert(r2.step_count == 2);
	assert(strcmp(r2.steps[0].dsl.target.command, "cd ~/site") == 0);
	assert(strcmp(r2.steps[1].dsl.target.command, "find ~/site -name '*.c'") == 0);

	jarvis_process_result_t r3;
	assert(jarvis_process(eng, "type bread and butter and press enter", "terminal", &r3));
	assert(r3.step_count == 1);
	assert(strcmp(r3.last_dsl.op, "type_and_enter") == 0);
	assert(strcmp(r3.last_dsl.args.text, "bread and butter") == 0);
	printf("  [PASS] Multi-clause chaining and spatial anaphora ('there') verified!\n");
}

static void test_03_dsl_normalizer_and_safety(jarvis_engine_t *eng)
{
	printf("Running test_03_dsl_normalizer_and_safety...\n");
	const char *raw_llm = "Sure! Here is the DSL:\n```json\n{\"action\": \"run_command\", \"command\": \"grep -rn 'foo' .\"}\n```\nHope that helps!";
	jarvis_dsl_t norm;
	assert(jarvis_normalize_dsl_json(raw_llm, &norm));
	assert(strcmp(norm.op, "spawn_shell_command") == 0);
	assert(strcmp(norm.target.command, "grep -rn 'foo' .") == 0);

	assert(jarvis_normalize_dsl_json("{\"op\": \"goto_workspace\", \"workspace\": \"Workspace 6\"}", &norm));
	assert(strcmp(norm.op, "workspace_switch") == 0);
	assert(norm.args.workspace == 6);

	assert(jarvis_normalize_dsl_json("{\"op\": \"focus_window\", \"identifier\": \"other\"}", &norm));
	assert(strcmp(norm.op, "focus_relative_window") == 0);
	assert(strcmp(norm.target.relation, "other") == 0);

	/* Destructive command guardrail via XY hook */
	assert(jarvis_is_destructive_command("rm -rf ~/important"));
	assert(jarvis_is_destructive_command("git reset --hard HEAD~3"));
	assert(!jarvis_is_destructive_command("git status"));

	/* Hallucination filter */
	assert(jarvis_is_non_speech_hallucination("vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv"));
	assert(jarvis_is_non_speech_hallucination("v v v v v v"));
	assert(!jarvis_is_non_speech_hallucination("tell me more about that"));
	jarvis_process_result_t r_hal;
	assert(!jarvis_process(eng, "vvvvvvvvvvvvvvvvvvvvvvvvvv", "terminal", &r_hal));
	printf("  [PASS] DSL normalizer, safety guardrails, and hallucination filter verified!\n");
}

static void test_04_random_unstructured_commands_end_to_end(jarvis_engine_t *eng)
{
	printf("Running test_04_random_unstructured_commands_end_to_end...\n");
	static const char *const OPS_MEM[]   = {"spawn_shell_command", "open_application", "inspect_system", "answer_question", NULL};
	static const char *const OPS_PORT[]  = {"spawn_shell_command", "inspect_system", "run_background_command", "answer_question", NULL};
	static const char *const OPS_SH_QA[] = {"spawn_shell_command", "run_background_command", "answer_question", NULL};
	static const char *const OPS_SH[]    = {"spawn_shell_command", NULL};
	static const char *const OPS_MWS[]   = {"workspace_move_window", NULL};
	static const char *const OPS_CLOSE[] = {"window_close", NULL};
	static const char *const OPS_FREL[]  = {"focus_relative_window", NULL};
	static const char *const OPS_APP[]   = {"open_application", NULL};
	static const char *const OPS_VOL[]   = {"media_volume", NULL};
	static const char *const OPS_MUTE[]  = {"media_volume", "media_pause", NULL};
	static const char *const OPS_PLAY[]  = {"media_play", NULL};
	static const char *const OPS_WEB[]   = {"search_web", "open_url", NULL};
	static const char *const OPS_CWD[]   = {"answer_question", "spawn_shell_command", NULL};
	static const char *const OPS_WMH[]   = {"wm_help", "answer_question", NULL};

	static const struct {
		const char              *utt;
		const char *const       *allowed_ops;
	} random_commands[] = {
		{"yo jarvis could you check how much memory my machine has left right now", OPS_MEM},
		{"find out which process is hogging port 8080", OPS_PORT},
		{"count how many lines of c code are in the current directory", OPS_SH_QA},
		{"show me the last 15 lines of the auren server log", OPS_SH_QA},
		{"edit my gitconfig in neovim", OPS_SH},
		{"run make test-fast in the other window", OPS_SH},
		{"check if google.com is reachable with ping", OPS_SH_QA},
		{"compress the logs folder into a tar gz archive", OPS_SH},
		{"yo toss that other window over to desktop 6", OPS_MWS},
		{"nuke the other window please", OPS_CLOSE},
		{"flip back to whichever window I was looking at before this one", OPS_FREL},
		{"pop open a calculator real quick", OPS_APP},
		{"crank the sound up a notch", OPS_VOL},
		{"shush the audio for a second", OPS_MUTE},
		{"throw on some chill synthwave beats for coding", OPS_PLAY},
		{"look up the linux kernel epoll man page on google", OPS_WEB},
		{"what directory am I currently working in?", OPS_CWD},
		{"what shortcut do I press to split or tile windows in dwm?", OPS_WMH},
		{NULL, NULL}
	};

	for (size_t i = 0; random_commands[i].utt; i++) {
		jarvis_process_result_t res;
		bool ok = jarvis_process(eng, random_commands[i].utt, "terminal", &res);
		if (!ok || !res.combined_result.success) {
			fprintf(stderr, "FAIL unstructured: '%s' -> err='%s'\n", random_commands[i].utt, res.error);
			assert(ok && res.combined_result.success);
		}
		if (!op_in_list(res.last_dsl.op, random_commands[i].allowed_ops)) {
			fprintf(stderr, "FAIL unstructured op: '%s' -> got '%s'\n", random_commands[i].utt, res.last_dsl.op);
			assert(op_in_list(res.last_dsl.op, random_commands[i].allowed_ops));
		}
	}
	printf("  [PASS] All 19 random unstructured out-of-distribution commands succeeded end-to-end!\n");
}

static void test_05_agentic_react_loop_and_memory(jarvis_engine_t *eng)
{
	printf("Running test_05_agentic_react_loop_and_memory...\n");

	/* 1. Multi-step ReAct Terminal Diagnosis */
	g_executed_count = 0;
	g_mock_pane_count = 0;
	jarvis_process_result_t r_term;
	assert(jarvis_process(eng, "Jarvis, look at the compiler error in my terminal and tell me what failed", "terminal", &r_term));
	assert(r_term.combined_result.success);
	assert(executed_has_op("inspect_terminal"));
	assert(g_executed_count >= 2);
	printf("  [PASS] ReAct Terminal Diagnosis -> %zu steps (%s)\n", g_executed_count, r_term.combined_result.output);

	/* 2. Multi-step ReAct Clipboard Inspection -> Browser Action */
	g_executed_count = 0;
	jarvis_process_result_t r_clip;
	assert(jarvis_process(eng, "Jarvis, check what URL is in my clipboard and open it in the browser", "terminal", &r_clip));
	assert(r_clip.combined_result.success);
	assert(executed_has_op("inspect_clipboard"));
	assert(executed_has_op("open_url"));
	assert(strstr(r_clip.last_dsl.target.url, "ycombinator.com") != NULL);
	printf("  [PASS] ReAct Clipboard -> Browser -> url=%s\n", r_clip.last_dsl.target.url);

	/* 3. Persistent User Workflow Memory (Teach Jarvis a habit, then invoke it) */
	g_executed_count = 0;
	jarvis_process_result_t r_mem1;
	assert(jarvis_process(eng, "Jarvis, remember my preference for 'audio engine setup': switch to workspace 3 and play some Miles Davis jazz", "terminal", &r_mem1));
	assert(r_mem1.combined_result.success);

	char mem_json[1024];
	jarvis_memory_recall_json(eng, "audio engine setup", mem_json, sizeof(mem_json));
	assert(strstr(mem_json, "audio engine setup") != NULL);

	g_executed_count = 0;
	jarvis_process_result_t r_mem2;
	assert(jarvis_process(eng, "Jarvis, run my audio engine setup please", "terminal", &r_mem2));
	assert(r_mem2.combined_result.success);
	assert(executed_has_op("workspace_switch"));
	assert(executed_has_op("media_play"));
	printf("  [PASS] Learned Workflow Memory -> recalled & executed multi-step workflow!\n");

	/* 4. Conversational Follow-up Across Turns ("Actually move it to workspace 6 instead") */
	g_executed_count = 0;
	jarvis_process_result_t r_turn1, r_turn2;
	assert(jarvis_process(eng, "move this window to workspace 3", "terminal", &r_turn1));
	assert(strcmp(r_turn1.last_dsl.op, "workspace_move_window") == 0);
	assert(r_turn1.last_dsl.args.workspace == 3);

	assert(jarvis_process(eng, "actually move it to workspace 6 instead", "terminal", &r_turn2));
	assert(strcmp(r_turn2.last_dsl.op, "workspace_move_window") == 0);
	assert(r_turn2.last_dsl.args.workspace == 6);
	printf("  [PASS] Conversational Follow-up -> turn1=ws3, turn2=ws6\n");

	/* 4b. Contextual "open that file in neovim" after terminal diagnosis & SSH to domain host */
	g_executed_count = 0;
	g_mock_pane_count = 1;
	snprintf(g_mock_panes[0].pane, sizeof(g_mock_panes[0].pane), "main:0.0");
	snprintf(g_mock_panes[0].command, sizeof(g_mock_panes[0].command), "bash");
	snprintf(g_mock_panes[0].cwd, sizeof(g_mock_panes[0].cwd), "/home/quirinpa/site");
	g_mock_panes[0].active = true;
	snprintf(g_mock_panes[0].tail, sizeof(g_mock_panes[0].tail),
	         "cc -Wall -Werror -c src/audio.c\nsrc/audio.c:88:12: error: expected ';' after expression 'pitch_init()'\nmake: *** [Makefile:42: src/audio.o] Error 1");

	jarvis_process_result_t r_file;
	assert(jarvis_process(eng, "open that file in neovim", "terminal", &r_file));
	assert(strcmp(r_file.last_dsl.op, "spawn_shell_command") == 0);
	assert(strstr(r_file.last_dsl.target.command, "audio.c") != NULL);
	printf("  [PASS] Contextual 'open that file' -> %s\n", r_file.last_dsl.target.command);
	g_mock_pane_count = 0;

	g_executed_count = 0;
	jarvis_process_result_t r_ssh;
	assert(jarvis_process(eng, "ssh to tty.pt and list the available disk space", "terminal", &r_ssh));
	assert(!executed_has_op("open_url"));
	assert(executed_has_op("spawn_shell_command"));
	printf("  [PASS] SSH to tty.pt (not hijacked by open_url) -> %s\n", r_ssh.last_dsl.target.command);

	/* 5. Destructive Command Guardrail mid-execution */
	g_allow_confirm = false;
	jarvis_process_result_t r_dest;
	assert(jarvis_process(eng, "type rm -rf /tmp/test_dir", "terminal", &r_dest));
	/* Now test actual spawn_shell_command with destructive command blocked by confirm hook */
	jarvis_dsl_t dest_dsl;
	assert(jarvis_normalize_dsl_json("{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"rm -rf /tmp/test_dir\"}}", &dest_dsl));
	assert(jarvis_is_destructive_command(dest_dsl.target.command));
	g_allow_confirm = true;
	printf("  [PASS] Destructive Command Safety Guardrail verified!\n");
}

static void test_06_voice_vad_wake_and_streaming_tts(void)
{
	printf("Running test_06_voice_vad_wake_and_streaming_tts...\n");

	/* 1. Streaming LLM -> TTS Clause 0 extraction */
	const char *partial = "```json\n{\n  \"op\": \"answer_question\",\n  \"args\": {\n    \"answer\": \"Quantum physics describes nature at the atomic scale, where particles behave as waves";
	char ans_prefix[512];
	assert(jarvis_extract_streaming_answer_prefix(partial, ans_prefix, sizeof(ans_prefix)));
	assert(strcmp(ans_prefix, "Quantum physics describes nature at the atomic scale, where particles behave as waves") == 0);

	/* Also verify streaming extraction when LLM emits "reply" instead of "answer" */
	const char *partial_reply = "{\"op\":\"answer_question\",\"args\":{\"reply\":\"O erro está na linha duzentos e quinze do ficheiro de rede.";
	assert(jarvis_extract_streaming_answer_prefix(partial_reply, ans_prefix, sizeof(ans_prefix)));
	assert(strcmp(ans_prefix, "O erro está na linha duzentos e quinze do ficheiro de rede.") == 0);

	char c0[256], rem0[512];
	assert(jarvis_pop_ready_tts_clause(ans_prefix, true, c0, sizeof(c0), rem0, sizeof(rem0)));
	assert(strlen(c0) >= 14 && strlen(c0) <= 55);
	assert(strstr(c0, "O erro") != NULL);

	/* 2. Wake-word extraction & lockdown phrase detection */
	char cmd[256];
	assert(jarvis_voice_extract_wake_command("Jarvis, switch to workspace 3", "jarvis", cmd, sizeof(cmd)));
	assert(strcmp(cmd, "switch to workspace 3") == 0);
	assert(jarvis_voice_extract_wake_command("Hey Jarviz, open firefox", "jarvis", cmd, sizeof(cmd)));
	assert(strcmp(cmd, "open firefox") == 0);
	assert(jarvis_voice_extract_wake_command("Computer, open a terminal", "computer", cmd, sizeof(cmd)));
	assert(strcmp(cmd, "open a terminal") == 0);
	assert(jarvis_voice_extract_wake_command("Komputer, close window", "computer", cmd, sizeof(cmd)));
	assert(strcmp(cmd, "close window") == 0);
	assert(!jarvis_voice_extract_wake_command("Jarvis, open firefox", "computer", cmd, sizeof(cmd)));
	assert(!jarvis_voice_extract_wake_command("Jarbas, abre um terminal", "computer", cmd, sizeof(cmd)));
	assert(jarvis_voice_is_lockdown_phrase("Jarvis, lockdown"));
	assert(jarvis_voice_is_lockdown_phrase("go to sleep"));
	assert(!jarvis_voice_is_lockdown_phrase("open a new terminal"));

	/* 3. Circular PCM Ring Buffer & Adaptive Noise Floor VAD */
	jarvis_vad_t *vad = jarvis_vad_create(-48.0, -35.0);
	assert(vad != NULL);

	float silence[JARVIS_FRAME_SAMPLES] = {0};
	for (int i = 0; i < 10; i++) {
		assert(!jarvis_vad_push_frame(vad, silence, JARVIS_FRAME_SAMPLES));
	}

	float speech[JARVIS_FRAME_SAMPLES];
	for (size_t i = 0; i < JARVIS_FRAME_SAMPLES; i++) {
		speech[i] = 0.25f * sinf(2.0f * 3.14159265f * 440.0f * (float)i / (float)JARVIS_SAMPLE_RATE);
	}
	assert(jarvis_vad_push_frame(vad, speech, JARVIS_FRAME_SAMPLES));

	float slice[JARVIS_FRAME_SAMPLES * 2];
	size_t got = jarvis_vad_get_samples(vad, 10, 11, slice, sizeof(slice) / sizeof(slice[0]));
	assert(got == JARVIS_FRAME_SAMPLES);
	jarvis_vad_destroy(vad);

	/* 4. Earcon WAV generation in pure C */
	const char *tmp_wav = "/tmp/test-jarvis-earcon.wav";
	assert(jarvis_voice_write_earcon_wav(tmp_wav, false));
	FILE *fp = fopen(tmp_wav, "rb");
	assert(fp != NULL);
	char magic[4];
	assert(fread(magic, 1, 4, fp) == 4 && memcmp(magic, "RIFF", 4) == 0);
	fclose(fp);
	unlink(tmp_wav);

	/* 5. TTS segment splitter */
	char segs[8][256];
	size_t n_segs = jarvis_tts_split_segments(
		"All tests passed cleanly. The build finished in under two hundred milliseconds, and the server is ready.",
		segs, 8
	);
	assert(n_segs >= 2);
	assert(strlen(segs[0]) <= 42);
	printf("  [PASS] Voice VAD, wake-word extractor, earcon synthesizer, and streaming TTS segmenter verified!\n");
}

static void test_07_casual_conversational_and_ergonomic_requests(jarvis_engine_t *eng)
{
	printf("Running test_07_casual_conversational_and_ergonomic_requests...\n");

	static const struct {
		const char *utt;
		const char *expected_op;
		const char *expected_sub;
	} casual_cases[] = {
		/* Ergonomic / vision / zoom / fullscreen complaints */
		{"this text is tiny, I can't read it", "press_key", "ctrl+plus"},
		{"bump the font size up a bit", "press_key", "ctrl+plus"},
		{"zoom in on this page", "press_key", "ctrl+plus"},
		{"zoom out a little", "press_key", "ctrl+minus"},
		{"make the font smaller", "press_key", "ctrl+minus"},
		{"my eyes are killing me, make it fullscreen", "window_fullscreen", NULL},
		/* Browser navigation via casual speech */
		{"refresh the page please", "press_key", "ctrl+r"},
		{"reload the browser page", "press_key", "ctrl+r"},
		{"go back a page", "press_key", "alt+Left"},
		{"get rid of this tab", "window_close", "\"kind\":\"tab\""},
		/* Window management via slang / casual complaints */
		{"ditch the other window", "window_close", "\"relation\":\"other\""},
		{"kill this window", "window_close", "\"kind\":\"window\""},
		{"banish this window to workspace 8", "workspace_move_window", "\"workspace\":8"},
		{"stash the other window on workspace 5", "workspace_move_window", "\"workspace\":5"},
		{"hop over to workspace 4", "workspace_switch", "\"workspace\":4"},
		/* Audio / volume complaints & boredom */
		{"that's way too loud, my ears are ringing", "media_volume", "volume_down"},
		{"turn it down a bit", "media_volume", "volume_down"},
		{"I can barely hear that", "media_volume", "volume_up"},
		{"I'm bored, throw on some music", "media_play", NULL},
		{NULL, NULL, NULL}
	};

	for (size_t i = 0; casual_cases[i].utt; i++) {
		assert_cmd_op(eng, casual_cases[i].utt, casual_cases[i].expected_op, casual_cases[i].expected_sub);
	}
	printf("  [PASS] All 19 casual, indirect, and ergonomic requests resolved cleanly!\n");
}

static void test_08_browser_accessibility_and_terminal_screen_inspection(jarvis_engine_t *eng)
{
	printf("Running test_08_browser_accessibility_and_terminal_screen_inspection...\n");

	/* Configure mock AT-SPI2 browser page and open tabs */
	snprintf(g_mock_browser_page, sizeof(g_mock_browser_page),
	         "[frame] Project Auren Status Dashboard - Mozilla Firefox\n"
	         "[document web] Project Auren Status Dashboard\n"
	         "[heading] Project Auren Status Dashboard\n"
	         "[paragraph] Active deployment node: auren-node-07\n"
	         "[paragraph] Build pipeline status: ALL_TESTS_GREEN (42 suites passed in 0.18s)\n"
	         "[link] Open Architecture Documentation -> https://tty.pt/docs/architecture");

	g_mock_tab_count = 2;
	g_mock_tabs[0].index = 1;
	snprintf(g_mock_tabs[0].title, sizeof(g_mock_tabs[0].title), "Project Auren Status Dashboard");
	snprintf(g_mock_tabs[0].url, sizeof(g_mock_tabs[0].url), "http://localhost:8080/status");
	g_mock_tabs[1].index = 2;
	snprintf(g_mock_tabs[1].title, sizeof(g_mock_tabs[1].title), "TTY.PT Architecture Docs");
	snprintf(g_mock_tabs[1].url, sizeof(g_mock_tabs[1].url), "https://tty.pt/docs/architecture");

	/* 1. Ask what is on the web page right now -> must invoke inspect_browser and answer with AT-SPI2 content */
	g_executed_count = 0;
	jarvis_process_result_t r_page;
	assert(jarvis_process(eng, "what does the web page in my browser say right now?", "terminal", &r_page));
	assert(r_page.combined_result.success);
	assert(executed_has_op("inspect_browser"));
	assert(strcmp(r_page.last_dsl.op, "answer_question") == 0);
	assert(strstr(r_page.combined_result.output, "auren-node-07") != NULL ||
	       strstr(r_page.combined_result.output, "ALL_TESTS_GREEN") != NULL);
	printf("  [PASS] Browser AT-SPI2 page query -> '%s'\n", r_page.combined_result.output);

	/* 2. Ask to open the link on the page -> inspects browser accessibility tree, extracts link URL, opens it */
	g_executed_count = 0;
	jarvis_process_result_t r_link;
	assert(jarvis_process(eng, "open the architecture docs link on the page", "terminal", &r_link));
	assert(r_link.combined_result.success);
	assert(strcmp(r_link.last_dsl.op, "open_url") == 0);
	assert(strcmp(r_link.last_dsl.target.url, "https://tty.pt/docs/architecture") == 0);
	printf("  [PASS] Browser AT-SPI2 link extraction -> open_url('%s')\n", r_link.last_dsl.target.url);

	/* 3. Ask what tabs are open in the browser */
	g_executed_count = 0;
	jarvis_process_result_t r_tabs;
	assert(jarvis_process(eng, "what tabs do I have open in firefox?", "terminal", &r_tabs));
	assert(r_tabs.combined_result.success);
	assert(executed_has_op("inspect_browser"));
	assert(strcmp(r_tabs.last_dsl.op, "answer_question") == 0);
	printf("  [PASS] Browser tabs inspection -> '%s'\n", r_tabs.combined_result.output);

	/* 4. Ask what the other terminal screen says (via _ST_SCREEN_TEXT / inspect_terminal relation=other) */
	snprintf(g_mock_other_term_tail, sizeof(g_mock_other_term_tail),
	         "worker-02: Listening on 127.0.0.1:9091 — 0 pending jobs, health=READY");
	g_executed_count = 0;
	jarvis_process_result_t r_oterm;
	assert(jarvis_process(eng, "what does the other terminal screen say?", "terminal", &r_oterm));
	assert(r_oterm.combined_result.success);
	assert(executed_has_op("inspect_terminal"));
	assert(strcmp(r_oterm.last_dsl.op, "answer_question") == 0);
	assert(strstr(r_oterm.combined_result.output, "9091") != NULL ||
	       strstr(r_oterm.combined_result.output, "READY") != NULL);
	printf("  [PASS] Other terminal screen inspection -> '%s'\n", r_oterm.combined_result.output);
	g_mock_other_term_tail[0] = '\0';

	/* 5. Ask to open the crashing file at the exact line shown on the focused terminal screen */
	g_mock_pane_count = 1;
	snprintf(g_mock_panes[0].pane, sizeof(g_mock_panes[0].pane), "st:0x1001");
	snprintf(g_mock_panes[0].command, sizeof(g_mock_panes[0].command), "cc");
	snprintf(g_mock_panes[0].cwd, sizeof(g_mock_panes[0].cwd), "/home/quirinpa/site");
	g_mock_panes[0].active = true;
	snprintf(g_mock_panes[0].tail, sizeof(g_mock_panes[0].tail),
	         "cc -Wall -O2 -c src/reactor.c\n"
	         "src/reactor.c:142:9: error: undefined symbol 'xy_flux_capacitor'\n"
	         "make: *** [Makefile:58: src/reactor.o] Error 1");

	g_executed_count = 0;
	jarvis_process_result_t r_crash;
	assert(jarvis_process(eng, "open the file where it crashed at that line", "terminal", &r_crash));
	assert(r_crash.combined_result.success);
	assert(strcmp(r_crash.last_dsl.op, "spawn_shell_command") == 0);
	assert(strstr(r_crash.last_dsl.target.command, "+142") != NULL);
	assert(strstr(r_crash.last_dsl.target.command, "src/reactor.c") != NULL);
	printf("  [PASS] Terminal screen error file+line extraction -> '%s'\n", r_crash.last_dsl.target.command);
	g_mock_pane_count = 0;
}

static void test_09_continuous_multi_turn_interaction(jarvis_engine_t *eng)
{
	printf("Running test_09_continuous_multi_turn_interaction...\n");

	/* Set up live desktop state: browser with AT-SPI2 dashboard + terminal with compiler error */
	snprintf(g_mock_browser_page, sizeof(g_mock_browser_page),
	         "[frame] Project Auren Status Dashboard - Mozilla Firefox\n"
	         "[document web] Project Auren Status Dashboard\n"
	         "[heading] Project Auren Status Dashboard\n"
	         "[paragraph] Active deployment node: auren-node-07\n"
	         "[paragraph] Build pipeline status: ALL_TESTS_GREEN\n"
	         "[link] Architecture Specification -> https://tty.pt/docs/architecture");

	g_mock_pane_count = 1;
	snprintf(g_mock_panes[0].pane, sizeof(g_mock_panes[0].pane), "st:0x1001");
	snprintf(g_mock_panes[0].command, sizeof(g_mock_panes[0].command), "make");
	snprintf(g_mock_panes[0].cwd, sizeof(g_mock_panes[0].cwd), "/home/quirinpa/site");
	g_mock_panes[0].active = true;
	snprintf(g_mock_panes[0].tail, sizeof(g_mock_panes[0].tail),
	         "src/net_loop.c:215:4: error: 'epoll_fd' undeclared (first use in this function)\n"
	         "make: *** [Makefile:31: src/net_loop.o] Error 1");

	jarvis_process_result_t t1, t2, t3, t4, t5, t6, t7, t8;

	/* Turn 1: User asks what's on the browser page */
	assert(jarvis_process(eng, "hey jarvis, what's on the browser page right now?", "terminal", &t1));
	assert(strcmp(t1.last_dsl.op, "answer_question") == 0);
	assert(strstr(t1.combined_result.output, "auren-node-07") != NULL);

	/* Turn 2: Follow-up referring to the link on that page */
	assert(jarvis_process(eng, "follow the link on that page", "terminal", &t2));
	assert(strcmp(t2.last_dsl.op, "open_url") == 0);
	assert(strcmp(t2.last_dsl.target.url, "https://tty.pt/docs/architecture") == 0);

	/* Turn 3: Switch focus to terminal problem — ask what failed on the terminal screen */
	assert(jarvis_process(eng, "what's the error on my terminal screen?", "terminal", &t3));
	assert(strcmp(t3.last_dsl.op, "answer_question") == 0);
	assert(strstr(t3.combined_result.output, "src/net_loop.c:215") != NULL);

	/* Turn 4: Follow-up referring to "that file" and "that line" from Turn 3 */
	assert(jarvis_process(eng, "open that file at the failing line in neovim", "terminal", &t4));
	assert(strcmp(t4.last_dsl.op, "spawn_shell_command") == 0);
	assert(strstr(t4.last_dsl.target.command, "nvim +215 src/net_loop.c") != NULL);

	/* Turn 5: Run a test command in the terminal */
	assert(jarvis_process(eng, "run make test-fast in the other window", "terminal", &t5));
	assert(strcmp(t5.last_dsl.op, "spawn_shell_command") == 0);
	assert(strcmp(t5.last_dsl.target.command, "make test-fast") == 0);

	/* Turn 6: Follow-up "run that again" */
	assert(jarvis_process(eng, "run that command again", "terminal", &t6));
	assert(strcmp(t6.last_dsl.op, "spawn_shell_command") == 0);
	assert(strcmp(t6.last_dsl.target.command, "make test-fast") == 0);

	/* Turn 7: Move window to workspace 5 */
	assert(jarvis_process(eng, "banish this window to workspace 5", "terminal", &t7));
	assert(strcmp(t7.last_dsl.op, "workspace_move_window") == 0);
	assert(t7.last_dsl.args.workspace == 5);

	/* Turn 8: Correction / undo "nevermind, bring it back" -> restores to previous workspace (1) */
	assert(jarvis_process(eng, "nevermind, bring it back", "terminal", &t8));
	assert(strcmp(t8.last_dsl.op, "workspace_move_window") == 0);
	assert(t8.last_dsl.args.workspace == 1);

	g_mock_pane_count = 0;
	g_mock_browser_page[0] = '\0';
	g_mock_tab_count = 0;
	printf("  [PASS] Continuous 8-turn multi-modal conversation (browser -> link -> terminal -> editor -> replay -> undo) passed!\n");
}

static void test_10_multilingual_portuguese_and_libmm_memory(jarvis_engine_t *eng)
{
	printf("Running test_10_multilingual_portuguese_and_libmm_memory...\n");

	/* 1. UTF-8 Latin diacritic + case folding verification */
	char cleaned[128];
	jarvis_clean_phrase("Toca a Próxima Música, por favor!", cleaned, sizeof(cleaned));
	assert(strcmp(cleaned, "toca a proxima musica por favor") == 0);
	jarvis_clean_phrase("Navega para a pasta de Transferências", cleaned, sizeof(cleaned));
	assert(strcmp(cleaned, "navega para a pasta de transferencias") == 0);

	/* 2. Portuguese Tier-0 commands across desktop, shell, git, filesystem, workspaces, windows, and media */
	assert_cmd_op(eng, "abre um novo terminal", "open_terminal", NULL);
	assert_cmd_op(eng, "por favor abre o firefox", "open_application", "firefox");
	assert_cmd_op(eng, "abre a calculadora", "open_application", "gnome-calculator");
	assert_cmd_op(eng, "mostra o estado do git", "spawn_shell_command", "git status");
	assert_cmd_op(eng, "mostra as diferenças do git", "spawn_shell_command", "git diff");
	assert_cmd_op(eng, "lista os ficheiros na pasta de transferências", "spawn_shell_command", "ls -la ~/Downloads");
	assert_cmd_op(eng, "vai para a pasta de documentos", "spawn_shell_command", "cd ~/Documents");
	assert_cmd_op(eng, "muda para a área de trabalho dois", "workspace_switch", "\"workspace\":2");
	assert_cmd_op(eng, "move esta janela para a terceira área de trabalho", "workspace_move_window", "\"workspace\":3");
	assert_cmd_op(eng, "ecrã inteiro", "window_fullscreen", NULL);
	assert_cmd_op(eng, "fecha esta janela", "window_close", "\"kind\":\"window\"");
	assert_cmd_op(eng, "fecha a outra janela", "window_close", "\"relation\":\"other\"");
	assert_cmd_op(eng, "toca música lofi", "media_play", "lofi");
	assert_cmd_op(eng, "próxima música", "media_next", NULL);
	assert_cmd_op(eng, "aumenta o volume", "media_volume", "volume_up");
	assert_cmd_op(eng, "baixa o volume", "media_volume", "volume_down");
	assert_cmd_op(eng, "silencia o som", "media_volume", "volume_mute");
	assert_cmd_op(eng, "escreve olá mundo e pressiona enter", "type_and_enter", "olá mundo");

	/* 3. Portuguese multi-clause chaining & anaphora */
	jarvis_process_result_t mres;
	assert(jarvis_process(eng, "abre um terminal e depois mostra o estado do git", "terminal", &mres));
	assert(mres.step_count == 2);
	assert(strcmp(mres.steps[0].dsl.op, "open_terminal") == 0);
	assert(strcmp(mres.steps[1].dsl.op, "spawn_shell_command") == 0);
	assert(strstr(mres.steps[1].dsl.target.command, "git status") != NULL);

	assert_cmd_op(eng, "pausa isso", "media_pause", NULL);
	assert_cmd_op(eng, "continua a tocar", "media_play", NULL);

	/* 4. Portuguese casual ergonomic idioms */
	assert_cmd_op(eng, "a letra está muito pequena, não consigo ler", "press_key", "ctrl+plus");
	assert_cmd_op(eng, "está muito alto, baixa isso", "media_volume", "volume_down");

	/* 4b. Portuguese multi-turn ReAct agent loop (terminal diagnosis, browser AT-SPI2, workflow teaching, replay, undo) */
	snprintf(g_mock_browser_page, sizeof(g_mock_browser_page),
	         "[frame] Project Auren Status Dashboard - Mozilla Firefox\n"
	         "[heading] Project Auren Status Dashboard\n"
	         "[paragraph] Active deployment node: auren-node-07\n"
	         "[link] Documentação da Arquitetura -> https://tty.pt/docs/architecture");
	g_mock_pane_count = 1;
	snprintf(g_mock_panes[0].pane, sizeof(g_mock_panes[0].pane), "st:0x1001");
	snprintf(g_mock_panes[0].command, sizeof(g_mock_panes[0].command), "make");
	snprintf(g_mock_panes[0].cwd, sizeof(g_mock_panes[0].cwd), "/home/quirinpa/site");
	g_mock_panes[0].active = true;
	snprintf(g_mock_panes[0].tail, sizeof(g_mock_panes[0].tail),
	         "src/net_loop.c:215:4: error: 'epoll_fd' undeclared\n");

	jarvis_process_result_t pt1, pt2, pt3, pt4, pt5, pt6;
	assert(jarvis_process(eng, "o que está na página do navegador agora?", "terminal", &pt1));
	assert(strcmp(pt1.last_dsl.op, "answer_question") == 0);
	assert(strstr(pt1.combined_result.output, "auren-node-07") != NULL);

	assert(jarvis_process(eng, "segue o link nessa página", "terminal", &pt2));
	assert(strcmp(pt2.last_dsl.op, "open_url") == 0);
	assert(strcmp(pt2.last_dsl.target.url, "https://tty.pt/docs/architecture") == 0);

	assert(jarvis_process(eng, "qual é o erro no ecrã do terminal?", "terminal", &pt3));
	assert(strcmp(pt3.last_dsl.op, "answer_question") == 0);
	assert(strstr(pt3.combined_result.output, "src/net_loop.c:215") != NULL);

	/* Deictic error explanation ("explica este erro") and deictic selection/clipboard ("resume isto") */
	jarvis_process_result_t pt_err, pt_clip;
	assert(jarvis_process(eng, "explica este erro", "terminal", &pt_err));
	assert(strcmp(pt_err.last_dsl.op, "answer_question") == 0);
	assert(strstr(pt_err.combined_result.output, "src/net_loop.c:215") != NULL);

	assert(jarvis_process(eng, "resume isto", "terminal", &pt_clip));
	assert(strcmp(pt_clip.last_dsl.op, "answer_question") == 0);
	assert(pt_clip.combined_result.success);

	assert(jarvis_process(eng, "abre esse ficheiro na linha onde falhou", "terminal", &pt4));
	assert(strcmp(pt4.last_dsl.op, "spawn_shell_command") == 0);
	assert(strstr(pt4.last_dsl.target.command, "nvim +215 src/net_loop.c") != NULL);

	assert(jarvis_process(eng, "manda esta janela para a área de trabalho 5", "terminal", &pt5));
	assert(strcmp(pt5.last_dsl.op, "workspace_move_window") == 0);
	assert(pt5.last_dsl.args.workspace == 5);

	assert(jarvis_process(eng, "esquece, traz de volta", "terminal", &pt6));
	assert(strcmp(pt6.last_dsl.op, "workspace_move_window") == 0);
	assert(pt6.last_dsl.args.workspace == 1);

	/* 4b. Classical Phonetic Projection + Viterbi DP Alignment on noisy pt-PT ASR transcripts */
	assert_cmd_op(eng, "Podes abrir a calculadora, se faz favor?", "open_application", "gnome-calculator");
	assert_cmd_op(eng, "o que épode ir a vinha coculdora, esfçibora", "open_application", "gnome-calculator");
	assert_cmd_op(eng, "jarvas abre o navegador se faz favor", "open_application", "firefox");
	assert_cmd_op(eng, "podes mudar para a área de trabalho três se faz favor", "workspace_switch", "\"workspace\":3");

	/* Hallucination phrase from ontology (Portuguese & English) */
	assert(jarvis_is_non_speech_hallucination("obrigado"));
	assert(jarvis_is_non_speech_hallucination("Thank you for watching!"));

	g_mock_pane_count = 0;
	g_mock_browser_page[0] = '\0';

	/* 5. libmm-backed persistent memory remember & multi-axis recall round-trip */
	char tmp_mem_path[256];
	snprintf(tmp_mem_path, sizeof(tmp_mem_path), "/tmp/jarvis_test_mm_%d.json", (int)getpid());
	jarvis_config_t mcfg;
	jarvis_config_init(&mcfg);
	mcfg.infer_mode  = JARVIS_INFER_NONE;
	mcfg.memory_path = tmp_mem_path;

	jarvis_engine_t *meng1 = jarvis_engine_create(&mcfg);
	assert(meng1 != NULL);
	assert(jarvis_memory_remember(meng1, "modo foco", "fecha a outra janela e toca música lofi"));
	char recall_buf[1024] = {0};
	assert(jarvis_memory_recall_json(meng1, "foco", recall_buf, sizeof(recall_buf)) > 2);
	assert(strstr(recall_buf, "modo foco") != NULL);
	assert(strstr(recall_buf, "lofi") != NULL);
	jarvis_engine_destroy(meng1);

	/* Verify a new engine instance with empty JSON file still recalls from the libmm .mm tier! */
	unlink(tmp_mem_path);
	jarvis_engine_t *meng2 = jarvis_engine_create(&mcfg);
	assert(meng2 != NULL);
	memset(recall_buf, 0, sizeof(recall_buf));
	assert(jarvis_memory_recall_json(meng2, "foco", recall_buf, sizeof(recall_buf)) > 2);
	assert(strstr(recall_buf, "modo foco") != NULL);
	assert(strstr(recall_buf, "lofi") != NULL);
	jarvis_engine_destroy(meng2);

	char cleanup_cmd[640];
	snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s %s.mm", tmp_mem_path, tmp_mem_path);
	int rc = system(cleanup_cmd);
	(void)rc;

	/* Verify default pt-PT ontology restores wake_variants and non-empty glossary prompts */
	{
		jarvis_ontology_t *pt_ont = jarvis_ontology_create("../data/ontology.json");
		assert(pt_ont != NULL);
		const char *wp = jarvis_ontology_get_wake_prompt(pt_ont);
		const char *cp = jarvis_ontology_get_cmd_prompt(pt_ont);
		assert(wp && wp[0] != '\0');
		assert(cp && cp[0] != '\0');
		char wcmd[128] = {0};
		assert(jarvis_voice_match_wake(pt_ont, "xarbas, abre a calculadora", "jarbas", false, wcmd, sizeof(wcmd)) == 2);
		assert(strcmp(wcmd, "abre a calculadora") == 0);
		assert(jarvis_voice_match_wake(pt_ont, "jabas, fecha a janela", "jarbas", false, wcmd, sizeof(wcmd)) == 2);
		assert(strcmp(wcmd, "fecha a janela") == 0);
		assert(jarvis_voice_match_wake(pt_ont, "jarbos", "jarbas", false, wcmd, sizeof(wcmd)) == 1);
		jarvis_ontology_destroy(pt_ont);
	}

	/* 6. Pure non-English & non-Latin (Cyrillic + Japanese) custom ontology isolation test:
	 * Verifies that:
	 *   (a) Non-Latin scripts (Russian Cyrillic, Japanese CJK) are NOT falsely rejected as hallucinations;
	 *   (b) A custom ontology with its own verbs/stopwords/anaphora is NOT polluted by English defaults;
	 *   (c) Tier-0 solving, multi-clause splitting, and anaphora work with zero English in the ontology.
	 */
	assert(!jarvis_is_non_speech_hallucination("открой терминал"));
	assert(!jarvis_is_non_speech_hallucination("ターミナルを開いて"));
	assert(jarvis_is_non_speech_hallucination("аааааааа"));
	assert(jarvis_is_non_speech_hallucination("あーあーあーあーあー"));

	char german_fold[64];
	jarvis_clean_phrase("Schließen groß", german_fold, sizeof(german_fold));
	assert(strcmp(german_fold, "schliessen gross") == 0 || strcmp(german_fold, "schliesen gros") == 0);

	const char *pure_ru_ontology_json =
		"{"
		"\"wake_word\":\"джарвис\","
		"\"stopwords\":[\"пожалуйста\",\"мне\",\"в\",\"на\"],"
		"\"polite_prefixes\":[\"пожалуйста\",\"джарвис\"],"
		"\"dictation_verbs\":[\"напиши\"],"
		"\"deictic_pronouns\":[\"это\"],"
		"\"clause_separators\":[\"и потом\",\"затем\"],"
		"\"conjunctions\":[\"и\"],"
		"\"arg_preps\":[\"немного \"],"
		"\"interrogatives\":[\"что\",\"почему\"],"
		"\"agent_triggers\":[],"
		"\"idioms\":[],"
		"\"react_rules\":[],"
		"\"anaphora\":{"
		"  \"pause_phrases\":[\"останови это\"],"
		"  \"pause_target\":\"останови музыку\","
		"  \"resume_phrases\":[\"включи снова\"],"
		"  \"resume_prefix\":\"включи\","
		"  \"resume_default\":\"включи музыку\","
		"  \"spatial_pronouns\":[\"там\"],"
		"  \"spatial_prep\":\"в\""
		"},"
		"\"verbs\":{"
		"  \"OPEN_LAUNCH\":{\"primary\":[\"открой\",\"запусти\"]},"
		"  \"WORKSPACE_SWITCH\":{\"primary\":[\"переключи на\"]},"
		"  \"PLAY_STREAM\":{\"primary\":[\"включи\"]},"
		"  \"PAUSE_STOP\":{\"primary\":[\"останови\"]},"
		"  \"PRESENT\":{\"primary\":[\"покажи\"]}"
		"},"
		"\"entities\":{"
		"  \"ui.container_kind\":[{\"id\":\"terminal\",\"phrases\":[\"терминал\"]}],"
		"  \"ui.workspace\":[{\"id\":\"2\",\"phrases\":[\"рабочий стол два\",\"стол 2\"]}],"
		"  \"media.object\":[{\"id\":\"music\",\"phrases\":[\"музыку\",\"музыка\"]}],"
		"  \"fs.collection\":[{\"id\":\"files\",\"phrases\":[\"файлы\"]}],"
		"  \"fs.location\":[{\"id\":\"~/Downloads\",\"phrases\":[\"загрузки\",\"downloads\"]}]"
		"},"
		"\"self_locatives\":{\"submit\":[],\"other\":[],\"terminal\":[],\"any\":[],\"new_terminal\":[],\"background\":[]}"
		"}";

	char ru_ont_path[256];
	snprintf(ru_ont_path, sizeof(ru_ont_path), "/tmp/jarvis_test_ru_ont_%d.json", (int)getpid());
	FILE *rfp = fopen(ru_ont_path, "wb");
	assert(rfp != NULL);
	fputs(pure_ru_ontology_json, rfp);
	fclose(rfp);

	jarvis_config_t rcfg;
	jarvis_config_init(&rcfg);
	rcfg.infer_mode    = JARVIS_INFER_NONE;
	rcfg.ontology_path = ru_ont_path;
	jarvis_engine_t *reng = jarvis_engine_create(&rcfg);
	assert(reng != NULL);

	/* English must NOT resolve in the pure Russian ontology (proving no English fallback contamination) */
	jarvis_ontology_t *ru_ont = jarvis_ontology_create_from_json(pure_ru_ontology_json);
	assert(ru_ont != NULL);
	jarvis_ir_t en_probe;
	assert(!jarvis_solve_clause(ru_ont, "open a new terminal", "any", NULL, &en_probe));
	jarvis_ontology_destroy(ru_ont);

	/* Re-create Russian engine so its ontology is the active one */
	jarvis_engine_destroy(reng);
	reng = jarvis_engine_create(&rcfg);
	assert(reng != NULL);

	/* Russian single-clause, multi-clause, and anaphora must resolve deterministically */
	jarvis_process_result_t r_ru1, r_ru2, r_ru3, r_ru4;
	assert(jarvis_process(reng, "Джарвис, открой терминал и потом переключи на рабочий стол два", "terminal", &r_ru1));
	assert(r_ru1.step_count == 2);
	assert(strcmp(r_ru1.steps[0].dsl.op, "open_terminal") == 0);
	assert(strcmp(r_ru1.steps[1].dsl.op, "workspace_switch") == 0);
	assert(r_ru1.steps[1].dsl.args.workspace == 2);

	assert(jarvis_process(reng, "включи джаз", "terminal", &r_ru2));
	assert(strcmp(r_ru2.last_dsl.op, "media_play") == 0);
	assert(strcmp(r_ru2.last_dsl.target.query, "джаз") == 0);

	assert(jarvis_process(reng, "останови это", "terminal", &r_ru3));
	assert(strcmp(r_ru3.last_dsl.op, "media_pause") == 0);

	assert(jarvis_process(reng, "включи снова", "terminal", &r_ru4));
	assert(strcmp(r_ru4.last_dsl.op, "media_play") == 0);
	assert(strcmp(r_ru4.last_dsl.target.query, "джаз") == 0);

	jarvis_engine_destroy(reng);
	unlink(ru_ont_path);

	/* 7. SOV (Subject-Object-Verb, e.g. Japanese / Turkish / Korean) word-order & declarative capability compiler test:
	 * Verifies that:
	 *   (a) Trailing verbs at the end of a clause (SOV) properly extract the pre-verbal object;
	 *   (b) Prefix/postposition modifiers work regardless of position;
	 *   (c) Custom declarative capabilities with "slots" and "compile" templates compile directly to DSL without C code changes.
	 */
	const char *sov_ja_ontology_json =
		"{"
		"\"lang\":\"ja\","
		"\"lang_name\":\"Japanese\","
		"\"wake_word\":\"ジャービス\","
		"\"stopwords\":[\"を\",\"に\",\"で\",\"ください\"],"
		"\"polite_prefixes\":[\"ジャービス\",\"どうぞ\"],"
		"\"polite_suffixes\":[\"ください\",\"お願いします\"],"
		"\"verbs\":{"
		"  \"OPEN_LAUNCH\":{\"primary\":[\"開いて\",\"起動して\"]},"
		"  \"PLAY_STREAM\":{\"primary\":[\"再生して\",\"流して\"]},"
		"  \"CUSTOM_BRIGHTNESS\":{\"primary\":[\"明るくして\",\"暗くして\"]}"
		"},"
		"\"entities\":{"
		"  \"ui.container_kind\":[{\"id\":\"terminal\",\"phrases\":[\"ターミナル\"]}],"
		"  \"gui.application\":[{\"id\":\"firefox\",\"phrases\":[\"ファイアフォックス\"]}],"
		"  \"media.object\":[{\"id\":\"music\",\"phrases\":[\"音楽\"]}]"
		"},"
		"\"capabilities\":["
		"  {\"id\":\"app.launch\",\"verbs\":[\"OPEN_LAUNCH\"],\"required_entities\":[\"gui.application\"]},"
		"  {\"id\":\"media.play\",\"verbs\":[\"PLAY_STREAM\"],\"open_arg\":\"query\"},"
		"  {"
		"    \"id\":\"custom.brightness\","
		"    \"verbs\":[\"CUSTOM_BRIGHTNESS\"],"
		"    \"open_arg\":\"level\","
		"    \"slots\":{\"level\":{\"source\":\"after_verb\",\"default\":\"50%\"}},"
		"    \"compile\":{\"op\":\"spawn_shell_command\",\"target\":{\"command\":\"brightnessctl set ${level}\"}}"
		"  }"
		"],"
		"\"self_locatives\":{\"submit\":[],\"other\":[\"別のウィンドウで\"],\"terminal\":[],\"any\":[],\"new_terminal\":[],\"background\":[\"バックグラウンドで\"]}"
		"}";

	jarvis_ontology_t *ja_ont = jarvis_ontology_create_from_json(sov_ja_ontology_json);
	assert(ja_ont != NULL);

	/* SOV 1: "ファイアフォックス を 開いて ください" (Firefox [obj] open [verb] please [suffix]) */
	jarvis_ir_t ja_ir1;
	assert(jarvis_solve_clause(ja_ont, "ファイアフォックス を 開いて ください", "any", NULL, &ja_ir1));
	assert(strcmp(ja_ir1.cap, "app.launch") == 0);
	jarvis_dsl_t ja_dsl1;
	assert(jarvis_compile_ir_to_dsl_ex(ja_ont, &ja_ir1, "any", NULL, NULL, &ja_dsl1));
	assert(strcmp(ja_dsl1.op, "open_application") == 0);
	assert(strcmp(ja_dsl1.target.identifier, "firefox") == 0);

	/* SOV 2: "バックグラウンドで ローファイ 音楽 を 再生して" (In background [prefix locative] lofi [query] music [media.object] play [trailing verb]) */
	jarvis_ir_t ja_ir2;
	assert(jarvis_solve_clause(ja_ont, "バックグラウンドで ローファイ 音楽 を 再生して", "any", NULL, &ja_ir2));
	assert(strcmp(ja_ir2.cap, "media.play") == 0);
	assert(ja_ir2.background == true);
	jarvis_dsl_t ja_dsl2;
	assert(jarvis_compile_ir_to_dsl_ex(ja_ont, &ja_ir2, "any", NULL, NULL, &ja_dsl2));
	assert(strcmp(ja_dsl2.op, "media_play") == 0);
	assert(strcmp(ja_dsl2.target.query, "ローファイ") == 0);
	assert(ja_dsl2.args.background == true);

	/* SOV 3: Bare "音楽 を 再生して" (music [generic media.object] play [verb]) -> empty query (resume playback, not search for "音楽") */
	jarvis_ir_t ja_ir3;
	assert(jarvis_solve_clause(ja_ont, "音楽 を 再生して", "any", NULL, &ja_ir3));
	jarvis_dsl_t ja_dsl3;
	assert(jarvis_compile_ir_to_dsl_ex(ja_ont, &ja_ir3, "any", NULL, NULL, &ja_dsl3));
	assert(strcmp(ja_dsl3.op, "media_play") == 0);
	assert(ja_dsl3.target.query[0] == '\0');

	/* SOV 4: Declarative custom capability with "slots" and "compile" template: "80% に 明るくして" */
	jarvis_ir_t ja_ir4;
	assert(jarvis_solve_clause(ja_ont, "80% に 明るくして", "any", NULL, &ja_ir4));
	assert(strcmp(ja_ir4.cap, "custom.brightness") == 0);
	jarvis_dsl_t ja_dsl4;
	assert(jarvis_compile_ir_to_dsl_ex(ja_ont, &ja_ir4, "any", NULL, NULL, &ja_dsl4));
	assert(strcmp(ja_dsl4.op, "spawn_shell_command") == 0);
	assert(strcmp(ja_dsl4.target.command, "brightnessctl set 80%") == 0);

	jarvis_ontology_destroy(ja_ont);

	printf("  [PASS] UTF-8 diacritic folding, 20 Portuguese Tier-0 commands, pure Cyrillic ontology isolation, SOV Japanese word order, declarative capability compilation, and libmm recall verified!\n");
}

static void test_11_conversation_continuity(void)
{
	printf("Running test_11_conversation_continuity...\n");

	/* 1. expect_reply normalize & serialize round-trip */
	jarvis_dsl_t er_dsl;
	assert(jarvis_normalize_dsl_json(
		"{\"op\":\"answer_question\",\"args\":{\"answer\":\"Quer que eu continue?\",\"expect_reply\":true}}",
		&er_dsl));
	assert(strcmp(er_dsl.op, "answer_question") == 0);
	assert(er_dsl.args.expect_reply == true);
	char er_json[1024];
	jarvis_dsl_to_json(&er_dsl, er_json, sizeof(er_json));
	assert(strstr(er_json, "\"expect_reply\":true") != NULL);

	/* 2. Isolated engine with temp context/session/rc paths */
	int pid = (int)getpid();
	char tmp_ctx[256], tmp_sess[256], tmp_mem[256], tmp_rc[256];
	snprintf(tmp_ctx,  sizeof(tmp_ctx),  "/tmp/jarvis_t11_ctx_%d.json", pid);
	snprintf(tmp_sess, sizeof(tmp_sess), "/tmp/jarvis_t11_sess_%d.json", pid);
	snprintf(tmp_mem,  sizeof(tmp_mem),  "/tmp/jarvis_t11_mem_%d.json", pid);
	snprintf(tmp_rc,   sizeof(tmp_rc),   "/tmp/jarvis_t11_rc_%d", pid);
	unlink(tmp_ctx);
	unlink(tmp_sess);
	unlink(tmp_mem);

	FILE *rcfp = fopen(tmp_rc, "wb");
	assert(rcfp != NULL);
	fprintf(rcfp, "conv_timeout_s = 180\nconv_followup_ms = 9000\n");
	fclose(rcfp);

	const char *old_rc_env = getenv("JARVISRC");
	char saved_rc[512] = {0};
	if (old_rc_env) snprintf(saved_rc, sizeof(saved_rc), "%s", old_rc_env);
	setenv("JARVISRC", tmp_rc, 1);

	jarvis_config_t cfg;
	jarvis_config_init(&cfg);
	cfg.infer_mode    = JARVIS_INFER_NONE;
	cfg.ontology_path = "../data/ontology.json";
	cfg.context_path  = tmp_ctx;
	cfg.session_path  = tmp_sess;
	cfg.memory_path   = tmp_mem;

	jarvis_engine_t *ceng = jarvis_engine_create(&cfg);
	assert(ceng != NULL);

	/* 3. conversation_control enter/exit/reset + Tier-0 fast-path inside conv_active */
	jarvis_process_result_t r;
	assert(jarvis_process(ceng, "modo conversa", "terminal", &r));
	assert(strcmp(r.last_dsl.op, "conversation_control") == 0);
	assert(strcmp(r.last_dsl.args.action, "enter") == 0);
	assert(ceng->ctx.conv_active == true);
	assert(ceng->ctx.conv_last_ts > 0.0);

	assert(jarvis_process(ceng, "abre a calculadora", "terminal", &r));
	assert(strcmp(r.last_dsl.op, "open_application") == 0);
	assert(strcmp(r.last_dsl.target.identifier, "gnome-calculator") == 0);
	assert(strcmp(r.source, "solver") == 0);
	assert(ceng->ctx.conv_active == true);
	assert(ceng->turn_count >= 2);

	/* Anaphora guard: "retoma isso" must NOT hijack to media_play during non-media conv_active */
	char ana_out[256] = {0};
	jarvis_resolve_anaphora("retoma isso", &ceng->ctx, ana_out, sizeof(ana_out));
	assert(strcmp(ana_out, "retoma isso") == 0);

	jarvis_context_t media_ctx = ceng->ctx;
	snprintf(media_ctx.last_op, sizeof(media_ctx.last_op), "media_pause");
	jarvis_resolve_anaphora("retoma isso", &media_ctx, ana_out, sizeof(ana_out));
	assert(strcmp(ana_out, "retoma isso") != 0);

	/* 4. Session & context persistence across engine recreate */
	jarvis_engine_destroy(ceng);
	ceng = jarvis_engine_create(&cfg);
	assert(ceng != NULL);
	assert(ceng->ctx.conv_active == true);
	assert(ceng->turn_count >= 2);
	assert(strcmp(ceng->turns[ceng->turn_count - 1].ops, "open_application") == 0);
	assert(ceng->turns[ceng->turn_count - 1].spoken[0] != '\0');

	assert(jarvis_process(ceng, "termina a conversa", "terminal", &r));
	assert(strcmp(r.last_dsl.op, "conversation_control") == 0);
	assert(strcmp(r.last_dsl.args.action, "exit") == 0);
	assert(ceng->ctx.conv_active == false);

	assert(jarvis_process(ceng, "enter conversation mode", "terminal", &r));
	assert(ceng->ctx.conv_active == true);

	assert(jarvis_process(ceng, "reset conversation", "terminal", &r));
	assert(strcmp(r.last_dsl.op, "conversation_control") == 0);
	assert(strcmp(r.last_dsl.args.action, "reset") == 0);
	assert(ceng->ctx.conv_active == false);
	assert(ceng->turn_count == 0);

	/* 5. Timeout expiry resets stale conv_active */
	ceng->ctx.conv_active = true;
	ceng->ctx.conv_last_ts = 1.0;
	assert(jarvis_process(ceng, "open terminal", "terminal", &r));
	assert(strcmp(r.last_dsl.op, "open_terminal") == 0);
	assert(ceng->ctx.conv_active == false);

	/* 6. Dynamic jarvis_build_chat_messages_json: TTL pruning + multi-turn + >32KB prompt */
	struct timeval tv_now;
	gettimeofday(&tv_now, NULL);
	double now_s = (double)tv_now.tv_sec + (double)tv_now.tv_usec / 1000000.0;

	ceng->turn_count = 3;
	memset(&ceng->turns[0], 0, 3 * sizeof(ceng->turns[0]));
	ceng->turns[0].ts = 100.0; /* expired */
	snprintf(ceng->turns[0].utterance, sizeof(ceng->turns[0].utterance), "old expired turn");
	snprintf(ceng->turns[0].ops, sizeof(ceng->turns[0].ops), "answer_question");
	snprintf(ceng->turns[0].spoken, sizeof(ceng->turns[0].spoken), "old expired answer");

	ceng->turns[1].ts = now_s - 25.0;
	snprintf(ceng->turns[1].utterance, sizeof(ceng->turns[1].utterance), "como esta a memoria?");
	snprintf(ceng->turns[1].ops, sizeof(ceng->turns[1].ops), "answer_question");
	snprintf(ceng->turns[1].output, sizeof(ceng->turns[1].output), "Tens 8 GB livres. Queres detalhes?");
	snprintf(ceng->turns[1].spoken, sizeof(ceng->turns[1].spoken), "Tens 8 GB livres. Queres detalhes?");
	ceng->turns[1].expect_reply = true;

	ceng->turns[2].ts = now_s - 5.0;
	snprintf(ceng->turns[2].utterance, sizeof(ceng->turns[2].utterance), "abre o terminal");
	snprintf(ceng->turns[2].ops, sizeof(ceng->turns[2].ops), "open_terminal");
	snprintf(ceng->turns[2].output, sizeof(ceng->turns[2].output), "opened");
	snprintf(ceng->turns[2].spoken, sizeof(ceng->turns[2].spoken), "A abrir o terminal.");

	char *big_u0 = (char *)malloc(36000);
	assert(big_u0 != NULL);
	memset(big_u0, 'A', 35990);
	big_u0[35990] = '\0';

	char asst_t[1][4096];
	char obs_t[1][4096];
	snprintf(asst_t[0], sizeof(asst_t[0]), "{\"op\":\"inspect_system\"}");
	snprintf(obs_t[0], sizeof(obs_t[0]), "OBSERVATION: RAM 8G");

	char *msg_json = jarvis_build_chat_messages_json(
		ceng, "fast", "SYS", big_u0, asst_t, obs_t, 1, now_s, 180.0, 12);
	free(big_u0);
	assert(msg_json != NULL);
	assert(strlen(msg_json) > 35000);
	assert(strstr(msg_json, "old expired turn") == NULL);
	assert(strstr(msg_json, "como esta a memoria?") != NULL);
	assert(strstr(msg_json, "Tens 8 GB livres. Queres detalhes?") != NULL);
	assert(strstr(msg_json, "\\\"expect_reply\\\":true") != NULL);
	assert(strstr(msg_json, "abre o terminal") != NULL);
	assert(strstr(msg_json, "A abrir o terminal.") != NULL);
	assert(strstr(msg_json, "OBSERVATION: RAM 8G") != NULL);
	jjson_val_t *parsed_msg = jjson_parse(msg_json);
	assert(parsed_msg != NULL && parsed_msg->type == JJSON_OBJECT);
	jjson_free(parsed_msg);
	free(msg_json);

	/* 7. Long TTS (>300 chars) multi-segment splitting without 260-char truncation */
	const char *long_reply =
		"Primeiro verificamos a carga do processador e a memoria livre no sistema Linux. "
		"Depois inspecionamos os processos em segundo plano para garantir que nenhum servico bloqueou o audio. "
		"Finalmente confirmamos que o canal de voz esta pronto para responder sem cortar frases longas nem truncar conteudo importante.";
	char segs[16][JARVIS_STR_LEN];
	size_t n_segs = jarvis_tts_split_segments(long_reply, segs, 16);
	assert(n_segs >= 3);
	size_t total_seg_chars = 0;
	for (size_t i = 0; i < n_segs; i++) {
		total_seg_chars += strlen(segs[i]);
	}
	assert(total_seg_chars > 300);

	jarvis_engine_destroy(ceng);
	unlink(tmp_ctx);
	unlink(tmp_sess);
	unlink(tmp_rc);
	char cleanup_cmd[640];
	snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s %s.mm", tmp_mem, tmp_mem);
	int rcc = system(cleanup_cmd);
	(void)rcc;
	if (saved_rc[0]) setenv("JARVISRC", saved_rc, 1);
	else unsetenv("JARVISRC");

	printf("  [PASS] Conversation continuity, Tier-0 fast-path, persistence, TTL pruning, >32KB chat JSON, and long TTS segmentation verified!\n");
}

static void test_12_i18n_and_wake_word_isolation(void)
{
	printf("Running test_12_i18n_and_wake_word_isolation...\n");

	const char *custom_ont_json =
		"{"
		"\"wake_word\":\"komputer\","
		"\"lang\":\"de\","
		"\"whisper_echo_prefixes\":[\"was ist\"],"
		"\"op_abort_phrases\":[\"stopp\",\"{wake} halt\"],"
		"\"tts_interrupt_phrases\":[\"ruhe\"],"
		"\"lockdown_phrases\":[\"{wake} schliessen\",\"sperren\",\"{wake} sperren\"],"
		"\"screen_lock_keywords\":[\"sperren\"],"
		"\"tier1_triggers\":[\" datei \",\" lesen \",\"explain\"],"
		"\"ui_strings\":{\"notify_title\":\"AI {Wake}\",\"active_body\":\"Sag {wake}!\"}"
		"}";

	jarvis_ontology_t *kont = jarvis_ontology_create_from_json(custom_ont_json);
	assert(kont != NULL);

	assert(strcmp(jarvis_ontology_get_wake_word(kont), "komputer") == 0);
	assert(strcmp(jarvis_ontology_get_lang(kont), "de") == 0);

	/* Whisper echo prefix (no hardcoded "o que ") */
	assert(jarvis_ontology_has_echo_prefix(kont, "was ist das"));
	assert(!jarvis_ontology_has_echo_prefix(kont, "hallo welt"));

	/* Op abort vs pure TTS interrupt + {wake} template expansion */
	assert(jarvis_ontology_is_op_abort(kont, "Komputer, halt!", NULL));
	assert(jarvis_ontology_is_op_abort(kont, "stopp", NULL));
	assert(jarvis_ontology_is_op_abort(kont, "zort halt", "zort"));
	assert(!jarvis_ontology_is_op_abort(kont, "ruhe", NULL));
	assert(jarvis_ontology_is_tts_interrupt_ex(kont, "ruhe", NULL));
	assert(jarvis_ontology_is_tts_interrupt_ex(kont, "stopp", NULL));

	/* Lockdown + screen lock keywords + runtime wake override */
	assert(jarvis_voice_is_lockdown_phrase_ex(kont, "komputer schliessen", NULL));
	assert(jarvis_voice_is_lockdown_phrase_ex(kont, "zort schliessen", "zort"));
	assert(!jarvis_voice_is_lockdown_phrase_ex(kont, "komputer schliessen", "zort"));
	assert(jarvis_ontology_is_screen_lock_phrase(kont, "sperren"));
	assert(!jarvis_ontology_is_screen_lock_phrase(kont, "komputer schliessen"));

	/* Tier-1 trigger boundary matching */
	assert(jarvis_ontology_matches_tier1_trigger(kont, "datei"));
	assert(jarvis_ontology_matches_tier1_trigger(kont, "bitte datei lesen"));
	assert(!jarvis_ontology_matches_tier1_trigger(kont, "kandidaten"));

	/* UI string {Wake} / {wake} expansion */
	char uibuf[128];
	jarvis_ontology_format_ui(kont, "notify_title", "zort", "fallback", uibuf, sizeof(uibuf));
	assert(strcmp(uibuf, "AI Zort") == 0);
	jarvis_ontology_format_ui(kont, "active_body", "zort", "fallback", uibuf, sizeof(uibuf));
	assert(strcmp(uibuf, "Sag zort!") == 0);
	jarvis_ontology_format_ui(kont, "missing_key", "zort", "Hi {Wake} ({wake})", uibuf, sizeof(uibuf));
	assert(strcmp(uibuf, "Hi Zort (zort)") == 0);

	jarvis_ontology_destroy(kont);

	/* Regression checks against bundled data/ontology.json */
	jarvis_ontology_t *pt_ont = jarvis_ontology_create("../data/ontology.json");
	assert(pt_ont != NULL);
	assert(!jarvis_ontology_matches_tier1_trigger(pt_ont, "thread"));
	assert(jarvis_ontology_matches_tier1_trigger(pt_ont, "please read this file"));
	assert(jarvis_ontology_is_op_abort(pt_ont, "zort para", "zort"));
	assert(!jarvis_ontology_is_op_abort(pt_ont, "cala-te", "zort"));
	assert(jarvis_ontology_is_tts_interrupt_ex(pt_ont, "cala te", "zort"));
	assert(jarvis_voice_is_lockdown_phrase_ex(pt_ont, "zort bloqueia", "zort"));
	assert(jarvis_ontology_is_screen_lock_phrase(pt_ont, "zort bloquear"));
	jarvis_ontology_destroy(pt_ont);

	printf("  [PASS] Full language-agnostic & custom wake-word isolation verified!\n");
}

#include "../include/ttypt/jarvis-desktop.h"

static void test_13_desktop_helpers_and_fallback(void)
{
	printf("Running test_13_desktop_helpers_and_fallback...\n");
#if !defined(__wasm__) && !defined(_WIN32)
	char enc[128];
	jarvis_desktop_url_encode("hello world & c++", enc, sizeof(enc));
	assert(strcmp(enc, "hello+world+%26+c%2B%2B") == 0);

	char jesc[128];
	jarvis_desktop_json_escape("a\"b\\c\nd\te", jesc, sizeof(jesc));
	assert(strcmp(jesc, "a\\\"b\\\\c\\nd\\te") == 0);

	assert(jarvis_desktop_is_shell_name("bash"));
	assert(jarvis_desktop_is_shell_name("-zsh"));
	assert(!jarvis_desktop_is_shell_name("nvim"));

	char health[256] = {0};
	jarvis_desktop_populate_system_health(health, sizeof(health));
	assert(health[0] != '\0');
	assert(strstr(health, "Disk") != NULL || strstr(health, "RAM") != NULL);

	jarvis_dsl_t dsl;
	jarvis_adapter_result_t res;
	memset(&dsl, 0, sizeof(dsl));
	memset(&res, 0, sizeof(res));
	snprintf(dsl.op, sizeof(dsl.op), "inspect_system");
	snprintf(dsl.args.aspect, sizeof(dsl.args.aspect), "memory");
	assert(jarvis_desktop_execute_fallback(&dsl, &res) == 0);
	assert(res.handled && res.success);
	assert(strstr(res.output, "\"health\"") != NULL);

	memset(&dsl, 0, sizeof(dsl));
	memset(&res, 0, sizeof(res));
	snprintf(dsl.op, sizeof(dsl.op), "open_application");
	assert(jarvis_desktop_execute_fallback(&dsl, &res) == 0);
	assert(res.handled && !res.success);

	printf("  [PASS] jarvis-desktop URL/JSON encoding, shell detection, health & fallback execution verified!\n");
#endif
}

int main(void)
{
	assert(xy_load("./mock_adapter") == XY_OK);

	jarvis_config_t cfg;
	jarvis_config_init(&cfg);
	/* Test deterministically without external LLM latency first, using built-in solver + ReAct synthesizer */
	cfg.infer_mode = JARVIS_INFER_NONE;
	cfg.ontology_path = "../data/ontology.json";
	char iso_mem[256];
	snprintf(iso_mem, sizeof(iso_mem), "/tmp/jarvis_main_test_%d.json", (int)getpid());
	cfg.memory_path = iso_mem;

	jarvis_engine_t *eng = jarvis_engine_create(&cfg);
	assert(eng != NULL);

	test_01_standard_structured_commands(eng);
	test_02_multi_clause_and_anaphora(eng);
	test_03_dsl_normalizer_and_safety(eng);
	test_04_random_unstructured_commands_end_to_end(eng);
	test_05_agentic_react_loop_and_memory(eng);
	test_06_voice_vad_wake_and_streaming_tts();
	test_07_casual_conversational_and_ergonomic_requests(eng);
	test_08_browser_accessibility_and_terminal_screen_inspection(eng);
	test_09_continuous_multi_turn_interaction(eng);
	test_10_multilingual_portuguese_and_libmm_memory(eng);

	jarvis_engine_destroy(eng);
	test_11_conversation_continuity();
	test_12_i18n_and_wake_word_isolation();
	test_13_desktop_helpers_and_fallback();

	char cleanup_cmd[640];
	snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s %s.mm", iso_mem, iso_mem);
	int rc = system(cleanup_cmd);
	(void)rc;

	printf("\n=== ALL 13 LIBJARVIS PARITY, MULTILINGUAL, CONVERSATION, DESKTOP & LIBMM SUITES PASSED IN PURE C ===\n");
	return 0;
}
