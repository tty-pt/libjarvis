#define _GNU_SOURCE
#include "../src/jarvis-tts.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long monotonic_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

static size_t count_file_lines(const char *path)
{
	FILE *fp = fopen(path, "r");
	if (!fp) return 0;
	size_t lines = 0;
	int ch;
	while ((ch = fgetc(fp)) != EOF) {
		if (ch == '\n') lines++;
	}
	fclose(fp);
	return lines;
}

static void write_exec_script(const char *path, const char *body)
{
	FILE *fp = fopen(path, "w");
	assert(fp != NULL);
	fputs(body, fp);
	fclose(fp);
	assert(chmod(path, 0755) == 0);
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);

	printf("Running test_tts (persistent edge-tts-pcm worker, LRU PCM cache, timeout fallback, interrupt)...\n");

	char tmpdir[] = "/tmp/test_jarvis_tts_XXXXXX";
	assert(mkdtemp(tmpdir) != NULL);

	char rc_path[512], pw_path[512], edge_path[512], starts_log[512], reqs_log[512], pw_log[512];
	snprintf(rc_path, sizeof(rc_path), "%s/jarvisrc", tmpdir);
	snprintf(pw_path, sizeof(pw_path), "%s/pw-cat", tmpdir);
	snprintf(edge_path, sizeof(edge_path), "%s/edge-tts-pcm", tmpdir);
	snprintf(starts_log, sizeof(starts_log), "%s/edge.starts", tmpdir);
	snprintf(reqs_log, sizeof(reqs_log), "%s/edge.reqs", tmpdir);
	snprintf(pw_log, sizeof(pw_log), "%s/pw.log", tmpdir);

	/* Isolated RC with edge PT voice enabled */
	{
		FILE *fp = fopen(rc_path, "w");
		assert(fp != NULL);
		fprintf(fp, "tts_enabled = 1\ntts_voice = edge:pt-PT-RaquelNeural\n");
		fclose(fp);
	}

	/* Stub pw-cat that consumes raw PCM quickly and logs invocations */
	{
		char pw_body[2048];
		snprintf(pw_body, sizeof(pw_body),
		         "#!/bin/sh\n"
		         "echo \"$*\" >> '%s'\n"
		         "cat >/dev/null\n",
		         pw_log);
		write_exec_script(pw_path, pw_body);
	}

	/* Mock edge-tts-pcm supporting --serve:
	 * - logs worker launch to starts_log
	 * - logs each request to reqs_log
	 * - sleeps 0.5s cleanly if text contains 'Hang' so timeout path triggers deterministically
	 * - otherwise emits 4-byte LE len (4800 bytes = 100ms @ 24kHz) + non-zero s16le PCM
	 */
	{
		char edge_body[4096];
		snprintf(edge_body, sizeof(edge_body),
		         "#!/usr/bin/env python3\n"
		         "import sys, struct, time\n"
		         "if len(sys.argv) < 2 or sys.argv[1] != '--serve':\n"
		         "    sys.exit(2)\n"
		         "with open('%s', 'a') as sf:\n"
		         "    sf.write('start\\n')\n"
		         "payload = b'\\x10\\x00' * 2400\n"
		         "hdr = struct.pack('<I', len(payload))\n"
		         "for line in sys.stdin:\n"
		         "    line = line.rstrip('\\r\\n')\n"
		         "    if not line: continue\n"
		         "    with open('%s', 'a') as rf:\n"
		         "        rf.write(line + '\\n')\n"
		         "    if 'Hang' in line:\n"
		         "        time.sleep(0.5)\n"
		         "        sys.stdout.buffer.write(struct.pack('<I', 0))\n"
		         "        sys.stdout.buffer.flush()\n"
		         "        continue\n"
		         "    sys.stdout.buffer.write(hdr + payload)\n"
		         "    sys.stdout.buffer.flush()\n",
		         starts_log, reqs_log);
		write_exec_script(edge_path, edge_body);
	}

	const char *old_path = getenv("PATH");
	char new_path[4096];
	snprintf(new_path, sizeof(new_path), "%s:%s", tmpdir, old_path ? old_path : "/usr/bin:/bin");

	setenv("PATH", new_path, 1);
	setenv("JARVISRC", rc_path, 1);
	setenv("JARVIS_EDGE_TTS_BIN", edge_path, 1);
	setenv("JARVIS_EDGE_TTS_TIMEOUT_MS", "200", 1);
	setenv("JARVIS_TTS", "1", 1);
	setenv("JARVIS_TTS_SYNC", "1", 1);
	unsetenv("JARVIS_NO_TTS");

	/* 1. First two distinct phrases must reuse ONE persistent --serve worker process */
	assert(jarvis_tts_speak_clause("s1", "Certo.", 1) == 0);
	assert(jarvis_tts_speak_clause("s1", "Segunda frase.", 1) == 0);
	assert(count_file_lines(starts_log) == 1);
	assert(count_file_lines(reqs_log) == 2);
	assert(count_file_lines(pw_log) >= 1);

	/* 2. Repeat "Certo." -> must hit 32-slot LRU PCM cache (zero new edge requests!) */
	long t0 = monotonic_ms();
	assert(jarvis_tts_speak_clause("s1", "Certo.", 1) == 0);
	long cache_ms = monotonic_ms() - t0;
	assert(count_file_lines(reqs_log) == 2);
	printf("  [PASS] Persistent --serve worker reused (starts=%zu, reqs=%zu) & LRU cache hit in %ldms\n",
	       count_file_lines(starts_log), count_file_lines(reqs_log), cache_ms);

	/* 3. Edge timeout (>200ms) aborts cleanly and falls back without caching failure under edge: key */
	t0 = monotonic_ms();
	assert(jarvis_tts_speak_clause("s1", "Hang timeout test.", 1) == 0);
	long fb_ms = monotonic_ms() - t0;
	assert(count_file_lines(reqs_log) == 3);
	/* Retry same phrase while mock still hangs: since fallback wasn't cached under edge:, it hits edge again */
	assert(jarvis_tts_speak_clause("s1", "Hang timeout test.", 1) == 0);
	assert(count_file_lines(reqs_log) == 4);
	printf("  [PASS] Timeout/fallback path bounded (%ldms) and uncached under edge: key\n", fb_ms);

	/* 4. Immediate recovery on next healthy call, busy barge-in interrupt, & clean shutdown */
	assert(jarvis_tts_speak_clause("s1", "Recuperado.", 1) == 0);
	assert(count_file_lines(reqs_log) == 5);

	/* Start slow hanging Edge synthesis asynchronously and barge-in cancel immediately */
	setenv("JARVIS_EDGE_TTS_TIMEOUT_MS", "2000", 1);
	t0 = monotonic_ms();
	assert(jarvis_tts_speak_clause("s1", "Hang bargein.", 0) == 0);
	usleep(40000);
	assert(jarvis_tts_interrupt(0) == 0);
	long barge_ms = monotonic_ms() - t0;
	assert(barge_ms < 400);

	/* Shutdown worker cleanly via JARVIS_NO_TTS=1 and confirm zero leftover child processes */
	setenv("JARVIS_NO_TTS", "1", 1);
	assert(jarvis_tts_speak_clause("s1", "", 0) == 0);
	int wst = 0;
	int reaped_zombies = 0;
	bool no_children = false;
	for (int i = 0; i < 50; i++) {
		pid_t w = waitpid(-1, &wst, WNOHANG);
		if (w < 0 && errno == ECHILD) {
			no_children = true;
			break;
		}
		if (w > 0) reaped_zombies++;
		usleep(20000);
	}
	assert(no_children);
	assert(reaped_zombies == 0);
	unsetenv("JARVIS_NO_TTS");
	printf("  [PASS] Busy barge-in interrupt (%ldms) & zero-child/zero-zombie cleanup verified!\n", barge_ms);

	/* Cleanup */
	unsetenv("JARVISRC");
	unsetenv("JARVIS_EDGE_TTS_BIN");
	unsetenv("JARVIS_EDGE_TTS_TIMEOUT_MS");
	unsetenv("JARVIS_TTS");
	unsetenv("JARVIS_TTS_SYNC");
	if (old_path) setenv("PATH", old_path, 1);

	unlink(rc_path);
	unlink(pw_path);
	unlink(edge_path);
	unlink(starts_log);
	unlink(reqs_log);
	unlink(pw_log);
	rmdir(tmpdir);
	return 0;
}
