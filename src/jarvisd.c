/* See LICENSE file for copyright and license details.
 * dwm-whisperd: Unified real-time streaming PipeWire + libwhisper C daemon
 * with persistent circular float32 PCM ring buffer, dedicated non-blocking
 * background decode worker thread with ggml abort_callback, live status-bar
 * and OSD partial transcription, and 0ms speculative silence endpointing.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <ttypt/jarvis.h>
#include "whisper.h"

static volatile sig_atomic_t g_jarvis_active = 0;
static jarvis_engine_t      *g_jarvis_eng    = NULL;
static pthread_mutex_t       g_jarvis_mtx    = PTHREAD_MUTEX_INITIALIZER;

#define SAMPLE_RATE      16000
#define FRAME_MS         20
#define FRAME_SAMPLES    ((SAMPLE_RATE * FRAME_MS) / 1000) /* 320 samples */
#define FRAME_BYTES      (FRAME_SAMPLES * (int)sizeof(int16_t))
#define RING_FRAMES      2000 /* 40 seconds circular float32 PCM buffer */
#define RING_SAMPLES     (RING_FRAMES * FRAME_SAMPLES)
#define MAX_UTTER_FRAMES 1500 /* 30 seconds max utterance */
#define WARM_HOLD_MS     60000L

static volatile sig_atomic_t running          = 1;
static volatile sig_atomic_t g_stop_req       = 0;
static volatile sig_atomic_t g_cancel_req     = 0;
static volatile sig_atomic_t cli_stop_req     = 0;
static volatile sig_atomic_t cli_cancel_req   = 0;

static int  g_preempt_cli = -1;
static char g_preempt_buf[2048];

/* Shared PipeWire capture ring buffer */
static struct {
	pthread_mutex_t mtx;
	pthread_cond_t  cond;
	float           pcm[RING_SAMPLES];
	float           db[RING_FRAMES];
	uint8_t         is_speech[RING_FRAMES];
	int64_t         total_frames;
	int64_t         last_cmd_end_f;
	double          noise_floor_db;
	double          ambient_std_db;
	int             env_frames;
	int             active_users;
	long            hold_until_ms;
	int             in_speech_hint;
	int             pw_open;
} g_audio = {
	.mtx            = PTHREAD_MUTEX_INITIALIZER,
	.cond           = PTHREAD_COND_INITIALIZER,
	.total_frames   = 0,
	.noise_floor_db = -42.0,
	.ambient_std_db = 1.5,
	.env_frames     = 0,
	.active_users   = 0,
	.hold_until_ms  = 0,
	.in_speech_hint = 0,
	.pw_open        = 0
};

static const jarvis_ontology_t *
get_daemon_ont(void)
{
	return jarvis_engine_get_ontology(g_jarvis_eng);
}

static const char *
get_wake_initial_prompt(void)
{
	const char *p = jarvis_ontology_get_wake_prompt(get_daemon_ont());
	return (p && *p) ? p : NULL;
}

static const char *
build_wake_initial_prompt(const char *wake_word, char *buf, size_t buf_sz)
{
	const char *ont_p = get_wake_initial_prompt();
	if (ont_p && *ont_p)
		return ont_p;
	if (!wake_word || !*wake_word || !buf || buf_sz < 4)
		return NULL;
	snprintf(buf, buf_sz, "%s.", wake_word);
	if (buf[0] >= 'a' && buf[0] <= 'z')
		buf[0] = (char)(buf[0] - 'a' + 'A');
	return buf;
}

static const char *
get_cmd_initial_prompt(void)
{
	const char *p = jarvis_ontology_get_cmd_prompt(get_daemon_ont());
	return (p && *p) ? p : NULL;
}

/* Non-blocking background Whisper decode worker for ctx_main */
static struct {
	pthread_mutex_t       mtx;
	pthread_cond_t        cond;
	struct whisper_context *ctx;
	int                   n_threads;
	float                *pcm;
	int                   n_samples;
	char                  lang[32];
	const char           *prompt;
	int                   max_tok;
	int                   is_partial;
	int64_t               job_voiced_f;
	int64_t               job_end_f;
	int                   has_job;
	int                   busy;
	volatile sig_atomic_t abort_flag;
	int                   has_result;
	char                  result_text[4096];
	int64_t               result_voiced_f;
	int64_t               result_end_f;
	int                   result_is_partial;
	int                   result_all_silent;
	int64_t               done_at_frame;
} g_dec = {
	.mtx               = PTHREAD_MUTEX_INITIALIZER,
	.cond              = PTHREAD_COND_INITIALIZER,
	.ctx               = NULL,
	.n_threads         = 4,
	.pcm               = NULL,
	.n_samples         = 0,
	.prompt            = NULL,
	.job_end_f         = 0,
	.has_job           = 0,
	.busy              = 0,
	.abort_flag        = 0,
	.has_result        = 0,
	.result_voiced_f   = -1,
	.result_end_f      = -1,
	.result_all_silent = 0,
	.done_at_frame     = 0
};

static void
on_sig(int sig)
{
	(void)sig;
	running = 0;
	g_dec.abort_flag = 1;
	pthread_cond_broadcast(&g_audio.cond);
	pthread_cond_broadcast(&g_dec.cond);
}

static void
on_cli_sigint(int sig)
{
	(void)sig;
	cli_stop_req = 1;
}

static void
on_cli_sigusr1(int sig)
{
	(void)sig;
	cli_cancel_req = 1;
}

static void
quiet_log(enum ggml_log_level level, const char *text, void *user_data)
{
	(void)level;
	(void)text;
	(void)user_data;
}

static bool
whisper_abort_cb(void *user_data)
{
	(void)user_data;
	return (!running || g_cancel_req || g_dec.abort_flag) ? true : false;
}

static long
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

static void
write_all(int fd, const char *buf, size_t len)
{
	while (len > 0) {
		ssize_t w = write(fd, buf, len);
		if (w <= 0)
			break;
		buf += w;
		len -= (size_t)w;
	}
}

static const char *
get_username(void)
{
	const char *u = getenv("USER");
	return (u && *u) ? u : "quirinpa";
}

static void
notify_bar(void)
{
	char path[256];
	FILE *fp;
	int pid = 0;

	if (!jarvis_rc_get("status_notify_pid_file", NULL, path, sizeof(path)) || !path[0])
		snprintf(path, sizeof(path), "/tmp/dwm-bar-%s.pid", get_username());
	fp = fopen(path, "r");
	if (!fp)
		return;
	if (fscanf(fp, "%d", &pid) == 1 && pid > 1)
		kill((pid_t)pid, SIGUSR1);
	fclose(fp);
}

static void
notify_osd_async(const char *title, const char *body, int timeout_ms)
{
	if (!title || !*title)
		return;
	if (fork() == 0) {
		char tbuf[32];
		int fd = open("/dev/null", O_WRONLY);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
		snprintf(tbuf, sizeof(tbuf), "%d", timeout_ms > 0 ? timeout_ms : 4000);
		execlp("notify-send", "notify-send",
		       "-t", tbuf,
		       "-h", "string:x-dunst-stack-tag:voice",
		       title, (body && *body) ? body : "", (char *)NULL);
		_exit(0);
	}
}

static void
write_state_file(const char *path, const char *prefix, const char *detail, const char *osd_title)
{
	char tmp[512];
	FILE *fp;

	if (!path || !*path || strcmp(path, "-") == 0)
		return;
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fp = fopen(tmp, "wb");
	if (!fp)
		return;
	if (detail && *detail)
		fprintf(fp, "%s:%s\n", prefix, detail);
	else
		fprintf(fp, "%s\n", prefix);
	fclose(fp);
	rename(tmp, path);
	notify_bar();

	if (osd_title && *osd_title && detail && *detail)
		notify_osd_async(osd_title, detail, 6000);
}

static void
play_chime_async(const char *chime_wav)
{
	if (!chime_wav || !*chime_wav || strcmp(chime_wav, "-") == 0)
		return;
	if (access(chime_wav, R_OK) != 0)
		return;
	if (fork() == 0) {
		int fd = open("/dev/null", O_WRONLY);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
		execlp("pw-play", "pw-play", "--volume=0.75", chime_wav, (char *)NULL);
		_exit(0);
	}
}

/* Dynamically resolve "kbd" language or normalize ISO tags ("pt-pt" -> "pt") for Whisper. */
static void
resolve_lang(const char *req_lang, char *out, size_t out_sz)
{
	const char *def_lang = jarvis_ontology_get_lang(get_daemon_ont());
	if (!def_lang || !*def_lang) def_lang = "pt";

	if (!req_lang || !*req_lang || strcmp(req_lang, "kbd") == 0) {
		char rc_lang[32] = {0};
		if (jarvis_rc_get("lang", NULL, rc_lang, sizeof(rc_lang)) &&
		    rc_lang[0] && strcmp(rc_lang, "kbd") != 0 && strcmp(rc_lang, "auto") != 0) {
			req_lang = rc_lang;
		}
	}

	if (!req_lang || !*req_lang || strcmp(req_lang, "kbd") == 0) {
		char path[256], buf[32] = {0};
		snprintf(path, sizeof(path), "/tmp/dwm-kbd-%s.state", get_username());
		FILE *fp = fopen(path, "r");
		if (fp) {
			if (fgets(buf, sizeof(buf), fp)) {
				char code[8] = {0};
				size_t ci = 0;
				for (size_t i = 0; buf[i] && ci < 2; i++) {
					if (isalpha((unsigned char)buf[i]))
						code[ci++] = (char)tolower((unsigned char)buf[i]);
				}
				code[ci] = '\0';
				if (strcmp(code, "us") == 0 || strcmp(code, "gb") == 0 || strcmp(code, "uk") == 0) {
					snprintf(out, out_sz, "en");
					fclose(fp);
					return;
				}
				if (strcmp(code, "br") == 0) {
					snprintf(out, out_sz, "pt");
					fclose(fp);
					return;
				}
				if (ci == 2) {
					snprintf(out, out_sz, "%s", code);
					fclose(fp);
					return;
				}
			}
			fclose(fp);
		}
		snprintf(out, out_sz, "%s", def_lang);
		return;
	}
	if (strcasecmp(req_lang, "auto") == 0) {
		snprintf(out, out_sz, "auto");
		return;
	}
	/* Normalize regional tags such as "pt-pt", "pt_PT", "en-gb" to 2-letter Whisper code */
	if (strlen(req_lang) >= 2 && (req_lang[2] == '-' || req_lang[2] == '_')) {
		char base[4] = {
			(char)tolower((unsigned char)req_lang[0]),
			(char)tolower((unsigned char)req_lang[1]),
			'\0', '\0'
		};
		snprintf(out, out_sz, "%s", base);
		return;
	}
	snprintf(out, out_sz, "%s", req_lang);
}

/* 2nd-order Butterworth speech-band filter (130 Hz high-pass + 3400 Hz low-pass
 * at 16 kHz) + Zero-Crossing Rate (ZCR) gate for VAD energy estimation.
 * Strips laptop cooling fan rumble (<120 Hz), desk thumps, and broadband air
 * turbulence hiss (>3500 Hz) from VAD decisions without altering the raw PCM
 * stored for Whisper.
 */
typedef struct {
	double hp_x1, hp_x2, hp_y1, hp_y2;
	double lp_x1, lp_x2, lp_y1, lp_y2;
} vad_biquad_t;

static double
calc_frame_db(const int16_t *samples, size_t n, vad_biquad_t *flt)
{
	/* 2nd-order Butterworth HPF fc=130 Hz @ fs=16000 Hz */
	static const double hp_b0 =  0.9645815294, hp_b1 = -1.9291630588, hp_b2 =  0.9645815294;
	static const double hp_a1 = -1.9279122460, hp_a2 =  0.9304138716;
	/* 2nd-order Butterworth LPF fc=3400 Hz @ fs=16000 Hz */
	static const double lp_b0 =  0.2266816615, lp_b1 =  0.4533633230, lp_b2 =  0.2266816615;
	static const double lp_a1 = -0.2764683938, lp_a2 =  0.1831950398;

	double sum = 0.0;
	int zc = 0;
	double prev_y = 0.0;
	size_t i;

	if (!n)
		return -100.0;
	for (i = 0; i < n; i++) {
		double x = (double)samples[i] / 32768.0;
		double y = x;
		if (flt) {
			double h = hp_b0 * x + hp_b1 * flt->hp_x1 + hp_b2 * flt->hp_x2
			         - hp_a1 * flt->hp_y1 - hp_a2 * flt->hp_y2;
			flt->hp_x2 = flt->hp_x1;
			flt->hp_x1 = x;
			flt->hp_y2 = flt->hp_y1;
			flt->hp_y1 = h;

			y = lp_b0 * h + lp_b1 * flt->lp_x1 + lp_b2 * flt->lp_x2
			  - lp_a1 * flt->lp_y1 - lp_a2 * flt->lp_y2;
			flt->lp_x2 = flt->lp_x1;
			flt->lp_x1 = h;
			flt->lp_y2 = flt->lp_y1;
			flt->lp_y1 = y;
		}
		if (i > 0 && ((y >= 0.0 && prev_y < 0.0) || (y < 0.0 && prev_y >= 0.0)))
			zc++;
		prev_y = y;
		sum += y * y;
	}
	double rms = sqrt(sum / (double)n);
	if (rms < 1e-5)
		return -100.0;
	double db = 20.0 * log10(rms);

	/* 130Hz-3400Hz Butterworth bandpass already strips fan rumble and air hiss;
	 * only penalize extreme sub-40Hz DC drift (<0.005) or ultrasonic whine (>0.65)
	 * so male low-formant vowels/nasals ("new", "terminal", "window") and
	 * sibilants ("workspace", "files") never suffer a mid-word dB drop.
	 */
	double zcr = (double)zc / (double)n;
	if (zcr < 0.005 || zcr > 0.65)
		db -= 3.0;
	return db;
}

/* Post-wake speaker auto-ducking: temporarily attenuate active playback while
 * capturing the spoken command after "Jarvis" wakes, then restore afterward.
 */
static double g_saved_sink_vol = -1.0;

static void
duck_speaker_for_command(void)
{
	if (g_saved_sink_vol > 0.0)
		return;
	FILE *fp = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
	if (!fp)
		return;
	char buf[128] = {0};
	if (fgets(buf, sizeof(buf), fp)) {
		if (!strstr(buf, "[MUTED]")) {
			double v = 0.0;
			if (sscanf(buf, "Volume: %lf", &v) == 1 && v > 0.22) {
				g_saved_sink_vol = v;
				double ducked = v * 0.35;
				if (ducked < 0.12)
					ducked = 0.12;
				char cmd[128];
				snprintf(cmd, sizeof(cmd),
				         "wpctl set-volume @DEFAULT_AUDIO_SINK@ %.2f >/dev/null 2>&1 &",
				         ducked);
				int rc = system(cmd);
				(void)rc;
			}
		}
	}
	pclose(fp);
}

static bool
is_volume_adjustment_command(const char *spoken_cmd)
{
	if (!spoken_cmd || !*spoken_cmd)
		return false;
	const jarvis_ontology_t *ont = get_daemon_ont();
	if (!ont)
		return false;
	jarvis_ir_t ir;
	if (jarvis_solve_clause(ont, spoken_cmd, "any", NULL, &ir) &&
	    strcmp(ir.cap, "media.volume") == 0) {
		return true;
	}
	return false;
}

static void
restore_speaker_after_command(const char *spoken_cmd)
{
	if (g_saved_sink_vol <= 0.0)
		return;
	double restore_v = g_saved_sink_vol;
	g_saved_sink_vol = -1.0;
	/* Do not overwrite volume if the user's command itself adjusts volume/mute */
	if (is_volume_adjustment_command(spoken_cmd))
		return;
	char cmd[128];
	snprintf(cmd, sizeof(cmd),
	         "wpctl set-volume @DEFAULT_AUDIO_SINK@ %.2f >/dev/null 2>&1 &",
	         restore_v);
	int rc = system(cmd);
	(void)rc;
}

static int
spawn_pw_record(pid_t *out_pid)
{
	char target[256] = {0};
	char target_arg[280] = {0};
	jarvis_rc_get("pw_target", NULL, target, sizeof(target));
	if (target[0])
		snprintf(target_arg, sizeof(target_arg), "--target=%s", target);

	int pfd[2];
	if (pipe(pfd) < 0)
		return -1;
	fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
	fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
	pid_t pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		close(pfd[0]);
		dup2(pfd[1], STDOUT_FILENO);
		close(pfd[1]);
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		if (target_arg[0]) {
			execlp("pw-record", "pw-record", "--raw", "--rate=16000",
			       "--channels=1", "--format=s16", "--latency=20ms",
			       target_arg, "--properties=media.role=Communication",
			       "-", (char *)NULL);
		} else {
			execlp("pw-record", "pw-record", "--raw", "--rate=16000",
			       "--channels=1", "--format=s16", "--latency=20ms",
			       "--properties=media.role=Communication", "-", (char *)NULL);
		}
		_exit(127);
	}
	close(pfd[1]);
	*out_pid = pid;
	return pfd[0];
}

static void
close_pw_record(int *fd, pid_t *pid)
{
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
	if (*pid > 0) {
		kill(*pid, SIGTERM);
		*pid = -1;
	}
}

/* Compute the true local ambient noise average (trimmed mean) and spread (std dev)
 * from a sliding window of up to win_len frames ending at end_f in g_audio.db.
 * Trims bottom 10% (extreme momentary dropouts) and top 25% (transient clicks
 * or speech onset) so it measures the actual average noise level at that moment
 * rather than tracking troughs. Caller must hold g_audio.mtx.
 */
static void
calc_ambient_window_locked(int64_t end_f, int win_len, double *out_avg, double *out_std)
{
	float buf[128];
	int n = 0;

	if (end_f > g_audio.total_frames)
		end_f = g_audio.total_frames;
	int64_t avail_min = (g_audio.total_frames > RING_FRAMES)
	                  ? (g_audio.total_frames - RING_FRAMES) : 0;
	if (win_len > 128)
		win_len = 128;

	/* Scan backwards up to win_len + 40 frames (2.4s) to collect non-speech frames.
	 * If more than half of the recent window is voiced/active (e.g. music, video,
	 * or continuous background sound), fall back to the 15%..75% trimmed mean over
	 * the recent win_len frames so noise_floor_db adapts to the playing audio instead
	 * of staying pinned at -60 dB from 4.8 seconds ago.
	 */
	int64_t scan_min = end_f - (win_len + 40);
	if (scan_min < avail_min)
		scan_min = avail_min;
	for (int64_t f = end_f - 1; f >= scan_min && n < win_len; f--) {
		int slot = (int)(f % RING_FRAMES);
		if (g_audio.is_speech[slot])
			continue;
		float v = g_audio.db[slot];
		if (v > -95.0f)
			buf[n++] = v;
	}

	int fallback_all = 0;
	if (n < (win_len / 2)) {
		n = 0;
		fallback_all = 1;
		int64_t start_f = end_f - win_len;
		if (start_f < avail_min)
			start_f = avail_min;
		for (int64_t f = start_f; f < end_f && n < 128; f++) {
			float v = g_audio.db[f % RING_FRAMES];
			if (v > -95.0f)
				buf[n++] = v;
		}
	}

	if (n == 0) {
		*out_avg = g_audio.noise_floor_db;
		*out_std = g_audio.ambient_std_db;
		return;
	}
	for (int i = 1; i < n; i++) {
		float key = buf[i];
		int j = i - 1;
		while (j >= 0 && buf[j] > key) {
			buf[j + 1] = buf[j];
			j--;
		}
		buf[j + 1] = key;
	}
	/* Symmetric 10% trim on non-speech frames (or 15%..75% trim on fallback) so
	 * out_avg reflects the true running background baseline without locking onto
	 * extreme quiet troughs when continuous external audio is present.
	 */
	int lo = fallback_all ? ((n * 15) / 100) : ((n >= 10) ? (n / 10) : 0);
	int hi = fallback_all ? ((n * 75) / 100) : ((n >= 10) ? (n - (n / 10)) : n);
	if (hi <= lo) {
		lo = 0;
		hi = n;
	}
	double sum = 0.0;
	int cnt = hi - lo;
	for (int i = lo; i < hi; i++)
		sum += buf[i];
	double avg = sum / (double)cnt;

	double var_sum = 0.0;
	for (int i = lo; i < hi; i++) {
		double d = buf[i] - avg;
		var_sum += d * d;
	}
	double std_dev = (cnt > 1) ? sqrt(var_sum / (double)cnt) : 1.2;
	if (std_dev < 1.0)
		std_dev = 1.0;
	if (std_dev > 3.5)
		std_dev = 3.5;
	if (avg < -60.0)
		avg = -60.0;
	if (avg > -24.0)
		avg = -24.0;

	*out_avg = avg;
	*out_std = std_dev;
}

static double
calc_vad_start_db(double amb_avg, double amb_std)
{
	double margin = 5.2;
	if (2.3 * amb_std > margin)
		margin = 2.3 * amb_std;
	double start_db = amb_avg + margin;
	if (start_db < -44.0)
		start_db = -44.0;
	if (start_db > -20.0)
		start_db = -20.0;
	return start_db;
}

static double
calc_vad_keep_db(double utter_amb_avg, double utter_amb_std,
                 double utter_start_db, double peak_speech_db)
{
	double keep_margin = 3.2;
	if (1.6 * utter_amb_std > keep_margin)
		keep_margin = 1.6 * utter_amb_std;
	double keep_db = utter_amb_avg + keep_margin;
	if (utter_start_db - 3.8 > keep_db)
		keep_db = utter_start_db - 3.8;
	if (peak_speech_db - 14.0 > keep_db)
		keep_db = peak_speech_db - 14.0;
	if (keep_db > utter_start_db - 2.0)
		keep_db = utter_start_db - 2.0;
	return keep_db;
}

static void *
audio_thread_fn(void *arg)
{
	int pw_fd = -1;
	pid_t pw_pid = -1;
	int16_t raw[FRAME_SAMPLES];
	vad_biquad_t vad_flt;
	memset(&vad_flt, 0, sizeof(vad_flt));

	(void)arg;
	while (running) {
		pthread_mutex_lock(&g_audio.mtx);
		int need = (g_audio.active_users > 0) || (now_ms() < g_audio.hold_until_ms);
		if (!need) {
			if (pw_fd >= 0) {
				g_audio.pw_open = 0;
				pthread_mutex_unlock(&g_audio.mtx);
				close_pw_record(&pw_fd, &pw_pid);
				pthread_mutex_lock(&g_audio.mtx);
			}
			while (running && g_audio.active_users <= 0 && now_ms() >= g_audio.hold_until_ms)
				pthread_cond_wait(&g_audio.cond, &g_audio.mtx);
			pthread_mutex_unlock(&g_audio.mtx);
			continue;
		}
		pthread_mutex_unlock(&g_audio.mtx);

		if (pw_fd < 0) {
			pw_fd = spawn_pw_record(&pw_pid);
			if (pw_fd < 0) {
				struct timespec ts = {0, 50000000L};
				nanosleep(&ts, NULL);
				continue;
			}
			pthread_mutex_lock(&g_audio.mtx);
			g_audio.pw_open = 1;
			pthread_cond_broadcast(&g_audio.cond);
			pthread_mutex_unlock(&g_audio.mtx);
		}

		size_t got = 0;
		while (got < (size_t)FRAME_BYTES) {
			ssize_t r = read(pw_fd, (char *)raw + got, (size_t)FRAME_BYTES - got);
			if (r <= 0) {
				if (r < 0 && errno == EINTR && running)
					continue;
				break;
			}
			got += (size_t)r;
		}
		if (got < (size_t)FRAME_BYTES) {
			close_pw_record(&pw_fd, &pw_pid);
			pthread_mutex_lock(&g_audio.mtx);
			g_audio.pw_open = 0;
			pthread_mutex_unlock(&g_audio.mtx);
			struct timespec ts = {0, 20000000L};
			nanosleep(&ts, NULL);
			continue;
		}

		double db = calc_frame_db(raw, FRAME_SAMPLES, &vad_flt);

		pthread_mutex_lock(&g_audio.mtx);
		int slot = (int)(g_audio.total_frames % RING_FRAMES);
		float *dst = g_audio.pcm + (size_t)slot * FRAME_SAMPLES;
		for (int i = 0; i < FRAME_SAMPLES; i++)
			dst[i] = (float)raw[i] / 32768.0f;
		g_audio.db[slot] = (float)db;
		g_audio.is_speech[slot] = (g_audio.env_frames >= 10 &&
		                           db > g_audio.noise_floor_db + 8.5) ? 1 : 0;
		g_audio.total_frames++;

		/* Sliding-window ambient average tracker:
		 * Measure the trimmed-mean noise level and spread over the last 80 frames (1.6s).
		 * Because is_speech[slot] only marks genuine +8.5 dB peaks above the running
		 * floor (rather than freezing whenever in_speech_hint is set), continuous
		 * room audio (phone, TV, music) smoothly updates the noise floor so VAD never
		 * gets stuck open.
		 */
		double win_avg = g_audio.noise_floor_db;
		double win_std = g_audio.ambient_std_db;
		calc_ambient_window_locked(g_audio.total_frames, 80, &win_avg, &win_std);
		if (!g_audio.in_speech_hint || g_audio.env_frames < 15) {
			g_audio.noise_floor_db = win_avg;
			g_audio.ambient_std_db = win_std;
			if (g_audio.env_frames < 15)
				g_audio.env_frames++;
		} else {
			g_audio.noise_floor_db = 0.92 * g_audio.noise_floor_db + 0.08 * win_avg;
			g_audio.ambient_std_db = 0.92 * g_audio.ambient_std_db + 0.08 * win_std;
		}

		pthread_cond_broadcast(&g_audio.cond);
		pthread_mutex_unlock(&g_audio.mtx);
	}

	close_pw_record(&pw_fd, &pw_pid);
	return NULL;
}

static void
audio_acquire(void)
{
	pthread_mutex_lock(&g_audio.mtx);
	g_audio.active_users++;
	int64_t f0 = g_audio.total_frames;
	pthread_cond_broadcast(&g_audio.cond);

	long deadline = now_ms() + 350L;
	while (running && (!g_audio.pw_open || g_audio.total_frames == f0 || g_audio.env_frames < 6)) {
		long rem = deadline - now_ms();
		if (rem <= 0)
			break;
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += (rem > 25 ? 25 : rem) * 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec += 1;
			ts.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&g_audio.cond, &g_audio.mtx, &ts);
	}
	pthread_mutex_unlock(&g_audio.mtx);
}

static void
audio_release(long hold_ms)
{
	pthread_mutex_lock(&g_audio.mtx);
	if (g_audio.active_users > 0)
		g_audio.active_users--;
	g_audio.in_speech_hint = 0;
	long until = now_ms() + hold_ms;
	if (until > g_audio.hold_until_ms)
		g_audio.hold_until_ms = until;
	pthread_cond_broadcast(&g_audio.cond);
	pthread_mutex_unlock(&g_audio.mtx);
}

static int
ring_extract_pcm(int64_t start_f, int64_t end_f, float *out_pcm, int max_samples)
{
	int n_samples = 0;

	pthread_mutex_lock(&g_audio.mtx);
	if (end_f > g_audio.total_frames)
		end_f = g_audio.total_frames;
	if (end_f - start_f > RING_FRAMES - 4)
		start_f = end_f - (RING_FRAMES - 4);
	if (start_f < 0)
		start_f = 0;

	for (int64_t f = start_f; f < end_f; f++) {
		if (n_samples + FRAME_SAMPLES > max_samples)
			break;
		int slot = (int)(f % RING_FRAMES);
		memcpy(out_pcm + n_samples, g_audio.pcm + (size_t)slot * FRAME_SAMPLES,
		       (size_t)FRAME_SAMPLES * sizeof(float));
		n_samples += FRAME_SAMPLES;
	}
	pthread_mutex_unlock(&g_audio.mtx);
	return n_samples;
}

static int
load_wav_f32(const char *path, float **out_pcm, int *out_samples)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return -1;

	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return -1;
	}
	long fsize = ftell(fp);
	if (fsize <= 44 || fsize > 64 * 1024 * 1024) {
		fclose(fp);
		return -1;
	}
	if (fseek(fp, 44, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}

	size_t pcm_bytes = (size_t)(fsize - 44);
	int n_samples = (int)(pcm_bytes / sizeof(int16_t));
	int16_t *raw = malloc((size_t)n_samples * sizeof(int16_t));
	float *f32 = malloc((size_t)n_samples * sizeof(float));
	if (!raw || !f32) {
		free(raw);
		free(f32);
		fclose(fp);
		return -1;
	}

	size_t n_read = fread(raw, sizeof(int16_t), (size_t)n_samples, fp);
	fclose(fp);
	n_samples = (int)n_read;

	for (int i = 0; i < n_samples; i++)
		f32[i] = (float)raw[i] / 32768.0f;

	free(raw);
	*out_pcm = f32;
	*out_samples = n_samples;
	return 0;
}

static int
compute_audio_ctx(int n_samples, int is_wake, int is_partial)
{
	int frames = (n_samples / 320) + (is_wake ? 48 : (is_partial ? 64 : 80));
	int aligned = ((frames + 31) / 32) * 32;
	int min_ctx = is_wake ? 384 : (is_partial ? 640 : 768);
	if (aligned < min_ctx)
		aligned = min_ctx;
	if (aligned > 1500)
		aligned = 1500;
	return aligned;
}

/*
 * Classical Digital Signal Processing (DSP) Front-End:
 * 1. First-order 80 Hz IIR high-pass filter (fs = 16000 Hz, alpha = 0.9695)
 *    removes DC offset and low-frequency rumble/plosive energy.
 * 2. Active-speech RMS & peak estimation with Automatic Gain Control (AGC)
 *    normalizes quiet speech toward target peak 0.707 (-3.0 dBFS) and target
 *    active RMS 0.12 (-18.4 dBFS), capped at +18 dB (8.0x) gain so pure
 *    ambient noise floor is never over-amplified.
 */
static float *
normalize_pcm_agc(const float *pcm, int n_samples)
{
	if (!pcm || n_samples <= 0)
		return NULL;
	float *buf = malloc((size_t)n_samples * sizeof(float));
	if (!buf)
		return NULL;

	const float alpha = 0.9695f;
	float prev_x = pcm[0];
	float prev_y = 0.0f;
	float peak = 0.0f;
	double sum_sq = 0.0;

	for (int i = 0; i < n_samples; i++) {
		float x = pcm[i];
		float y = alpha * (prev_y + x - prev_x);
		prev_x = x;
		prev_y = y;
		buf[i] = y;
		float ay = fabsf(y);
		if (ay > peak)
			peak = ay;
		sum_sq += (double)y * (double)y;
	}

	/* Measure active-speech RMS on 20ms frames (320 samples) whose energy is at
	 * least -22 dB relative to peak (or > 0.015), so leading/trailing silence frames
	 * never underestimate speech RMS and cause over-gain clipping of consonants.
	 */
	double total_rms = sqrt(sum_sq / (double)n_samples);
	double active_sq = 0.0;
	int active_n = 0;
	float frame_thresh = peak * 0.08f;
	if (frame_thresh < 0.008f)
		frame_thresh = 0.008f;
	for (int base = 0; base + 320 <= n_samples; base += 320) {
		double f_sq = 0.0;
		for (int k = 0; k < 320; k++) {
			float v = buf[base + k];
			f_sq += (double)v * (double)v;
		}
		double f_rms = sqrt(f_sq / 320.0);
		if (f_rms >= (double)frame_thresh) {
			active_sq += f_sq;
			active_n += 320;
		}
	}
	double rms = (active_n >= 1600) ? sqrt(active_sq / (double)active_n) : total_rms;

	/* Only apply boost if signal contains genuine acoustic energy above silence floor */
	if (peak > 0.010f && rms > 0.0020) {
		float gain_peak = 0.78f / peak;
		float gain_rms = (float)(0.11 / rms);
		float gain = (gain_rms < gain_peak) ? gain_rms : gain_peak;
		if (gain > 5.0f)
			gain = 5.0f;
		if (gain < 0.85f)
			gain = 0.85f;
		if (fabsf(gain - 1.0f) > 0.05f) {
			for (int i = 0; i < n_samples; i++) {
				float v = buf[i] * gain;
				if (v > 0.98f)
					v = 0.98f;
				else if (v < -0.98f)
					v = -0.98f;
				buf[i] = v;
			}
		}
	}

	return buf;
}

static void
trim_inplace(char *s)
{
	for (char *c = s; *c; c++) {
		if (*c == '\r' || *c == '\n' || *c == '\t')
			*c = ' ';
	}
	char *p = s;
	while (*p && isspace((unsigned char)*p))
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	size_t len = strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1]))
		s[--len] = '\0';
}

/* Collapse immediate repeated word sequences (1..8 words) in-place so a noisy
 * clip never outputs "Go to workspace 1 Go to workspace 1".
 */
static void
collapse_repetitions(char *s, size_t s_sz)
{
	char copy[4096];
	char *words[256];
	int n_words = 0;

	if (!s || !*s || s_sz == 0)
		return;
	snprintf(copy, sizeof(copy), "%s", s);

	char *save = NULL;
	for (char *tok = strtok_r(copy, " ", &save); tok && n_words < 256;
	     tok = strtok_r(NULL, " ", &save)) {
		words[n_words++] = tok;
	}
	if (n_words < 2)
		return;

	int keep[256];
	for (int i = 0; i < n_words; i++)
		keep[i] = 1;

	for (int span = 8; span >= 1; span--) {
		for (int i = 0; i + 2 * span <= n_words; ) {
			if (!keep[i]) {
				i++;
				continue;
			}
			int match = 1;
			for (int k = 0; k < span; k++) {
				const char *a = words[i + k];
				const char *b = words[i + span + k];
				size_t la = strlen(a), lb = strlen(b);
				while (la > 0 && ispunct((unsigned char)a[la - 1]))
					la--;
				while (lb > 0 && ispunct((unsigned char)b[lb - 1]))
					lb--;
				if (la == 0 || la != lb || strncasecmp(a, b, la) != 0) {
					match = 0;
					break;
				}
			}
			if (match && (span >= 2 || strlen(words[i]) >= 4)) {
				for (int k = 0; k < span; k++)
					keep[i + span + k] = 0;
				/* Shift compactly and re-check for 3+ repetitions */
				int dst = 0;
				for (int j = 0; j < n_words; j++) {
					if (keep[j]) {
						words[dst] = words[j];
						keep[dst] = 1;
						dst++;
					}
				}
				n_words = dst;
			} else {
				i++;
			}
		}
	}

	/* Strip trailing truncated prefix repetition (e.g. "open up miles davis open up miles"
	 * or "Open T-MUX in this terminal. Open T").
	 */
	for (int k = 6; k >= 1; k--) {
		if (n_words >= 2 * k + 1) {
			int tail_match = 1;
			for (int j = 0; j < k; j++) {
				const char *a = words[j];
				const char *b = words[n_words - k + j];
				size_t la = strlen(a), lb = strlen(b);
				while (la > 0 && ispunct((unsigned char)a[la - 1]))
					la--;
				while (lb > 0 && ispunct((unsigned char)b[lb - 1]))
					lb--;
				if (la == 0 || lb == 0) {
					tail_match = 0;
					break;
				}
				if (j == k - 1 && k >= 2 && lb < la) {
					if (strncasecmp(a, b, lb) != 0) {
						tail_match = 0;
						break;
					}
				} else if (la != lb || strncasecmp(a, b, la) != 0) {
					tail_match = 0;
					break;
				}
			}
			if (tail_match) {
				/* For k == 1, only strip if preceded by sentence punctuation */
				if (k == 1) {
					const char *prev_w = words[n_words - 2];
					size_t pl = strlen(prev_w);
					if (pl == 0 || (prev_w[pl - 1] != '.' && prev_w[pl - 1] != '!' && prev_w[pl - 1] != '?'))
						continue;
				}
				n_words -= k;
				break;
			}
		}
	}

	size_t off = 0;
	s[0] = '\0';
	for (int i = 0; i < n_words; i++) {
		int n = snprintf(s + off, s_sz - off, "%s%s", (i > 0) ? " " : "", words[i]);
		if (n < 0 || (size_t)n >= s_sz - off) {
			s[s_sz - 1] = '\0';
			break;
		}
		off += (size_t)n;
	}
}

static int
is_hallucination(const char *s)
{
	return jarvis_is_non_speech_hallucination(s) ? 1 : 0;
}

static void
run_whisper_slice(struct whisper_context *ctx, int n_threads, const float *pcm, int n_samples,
                  const char *lang, const char *prompt, int max_tok, int is_wake, int is_partial,
                  int use_abort_cb, char *out_text, size_t out_sz, int *out_all_silent)
{
	char eff_lang[32];

	out_text[0] = '\0';
	if (out_all_silent)
		*out_all_silent = 0;
	if (n_samples < 1600)
		return;

	resolve_lang(lang, eff_lang, sizeof(eff_lang));

	if (is_partial) {
		int cap = (n_samples / 2200) + 6;
		if (cap < max_tok)
			max_tok = cap;
	}

	bool is_final_cmd = (!is_wake && !is_partial);

	struct whisper_full_params wparams = whisper_full_default_params(
		(is_wake || is_partial) ? WHISPER_SAMPLING_GREEDY : WHISPER_SAMPLING_BEAM_SEARCH);
	wparams.n_threads        = n_threads;
	wparams.single_segment   = is_final_cmd ? false : true;
	wparams.no_context       = true;
	wparams.no_timestamps    = is_final_cmd ? false : true;
	wparams.suppress_blank   = true;
	wparams.suppress_nst     = true;
	wparams.temperature      = 0.0f;
	wparams.temperature_inc  = (is_wake || is_partial) ? 0.0f : 0.2f;
	if (is_wake || is_partial) {
		wparams.greedy.best_of = 1;
	} else {
		wparams.beam_search.beam_size = 3;
	}
	wparams.max_tokens       = max_tok;
	wparams.audio_ctx        = compute_audio_ctx(n_samples, is_wake, is_partial);

	if (use_abort_cb) {
		wparams.abort_callback = whisper_abort_cb;
		wparams.abort_callback_user_data = NULL;
	}

	if (!whisper_is_multilingual(ctx)) {
		wparams.language = "en";
		wparams.detect_language = false;
	} else if (!eff_lang[0] || strcmp(eff_lang, "auto") == 0) {
		wparams.language = "auto";
		wparams.detect_language = false;
	} else {
		wparams.language = eff_lang;
	}

	if (prompt && *prompt && strcmp(prompt, "-") != 0)
		wparams.initial_prompt = prompt;

	float *agc_pcm = normalize_pcm_agc(pcm, n_samples);
	const float *eff_pcm = agc_pcm ? agc_pcm : pcm;

	if (whisper_full(ctx, wparams, eff_pcm, n_samples) == 0) {
		int n_seg = whisper_full_n_segments(ctx);
		int valid_seg = 0;
		float nsp_limit = is_wake ? 0.68f : 0.58f;
		whisper_token eot_id = whisper_token_eot(ctx);
		char prev_seg[1024] = {0};
		for (int i = 0; i < n_seg; i++) {
			float nsp = whisper_full_get_segment_no_speech_prob(ctx, i);
			if (nsp > nsp_limit)
				continue;

			int n_tok = whisper_full_n_tokens(ctx, i);
			float sum_p = 0.0f;
			int word_toks = 0;
			int end_tok = n_tok;

			/* Inspect token probabilities and prune low-confidence trailing
			 * noise tokens (e.g. "on online", "you") caused by ambient/fan noise
			 * at the tail of an utterance.
			 */
			for (int j = 0; j < n_tok; j++) {
				whisper_token_data td = whisper_full_get_token_data(ctx, i, j);
				const char *tt = whisper_full_get_token_text(ctx, i, j);
				if (td.id >= eot_id || !tt || !*tt)
					continue;
				if (tt[0] == '[' && tt[strlen(tt) - 1] == ']')
					continue;
				sum_p += td.p;
				word_toks++;
			}
			if (word_toks > 0 && (sum_p / (float)word_toks) < (is_wake ? 0.20f : 0.18f))
				continue;

			if (!is_wake && word_toks >= 2) {
				while (end_tok > 1) {
					whisper_token_data td = whisper_full_get_token_data(ctx, i, end_tok - 1);
					const char *tt = whisper_full_get_token_text(ctx, i, end_tok - 1);
					if (td.id >= eot_id || !tt || !*tt) {
						end_tok--;
						continue;
					}
					int has_alnum = 0;
					for (const char *c = tt; *c; c++) {
						unsigned char uc = (unsigned char)*c;
						if (isalnum(uc) || (uc >= 0xC2 && uc <= 0xDF) || uc >= 0xE3) {
							has_alnum = 1;
							break;
						}
					}
					if (!has_alnum) {
						end_tok--;
						continue;
					}
					/* Only prune an extreme <3% probability noise hallucination token,
					 * never genuine final words which often have p in 0.08..0.28.
					 */
					if (td.p < 0.03f) {
						end_tok--;
						continue;
					}
					break;
				}
			}

			char seg_clean[1024] = {0};
			if (!is_wake && end_tok < n_tok && end_tok > 0) {
				for (int j = 0; j < end_tok; j++) {
					whisper_token_data td = whisper_full_get_token_data(ctx, i, j);
					if (td.id >= eot_id)
						continue;
					const char *tt = whisper_full_get_token_text(ctx, i, j);
					if (tt && *tt)
						strncat(seg_clean, tt, sizeof(seg_clean) - strlen(seg_clean) - 1);
				}
			} else {
				const char *seg = whisper_full_get_segment_text(ctx, i);
				if (!seg)
					continue;
				snprintf(seg_clean, sizeof(seg_clean), "%s", seg);
			}
			trim_inplace(seg_clean);
			if (is_hallucination(seg_clean))
				continue;
			if (prev_seg[0] != '\0' && strcasecmp(prev_seg, seg_clean) == 0)
				continue;
			snprintf(prev_seg, sizeof(prev_seg), "%s", seg_clean);
			valid_seg++;
			if (out_text[0] != '\0')
				strncat(out_text, " ", out_sz - strlen(out_text) - 1);
			strncat(out_text, seg_clean, out_sz - strlen(out_text) - 1);
		}
		if (valid_seg == 0) {
			if (out_all_silent)
				*out_all_silent = 1;
			/* Instant noise-floor recalibration when Whisper confirms the captured
			 * slice contained no speech (e.g. sudden laptop cooling fan spin-up).
			 */
			pthread_mutex_lock(&g_audio.mtx);
			int64_t tf = g_audio.total_frames;
			int n_avg = (tf >= 16) ? 16 : (int)tf;
			if (n_avg > 0) {
				double sum_db = 0.0;
				for (int k = 1; k <= n_avg; k++)
					sum_db += g_audio.db[(tf - k) % RING_FRAMES];
				double avg_db = sum_db / (double)n_avg;
				if (avg_db > g_audio.noise_floor_db && avg_db < -19.5) {
					g_audio.noise_floor_db = 0.45 * g_audio.noise_floor_db + 0.55 * avg_db;
					if (g_audio.noise_floor_db > -21.0)
						g_audio.noise_floor_db = -21.0;
				}
			}
			pthread_mutex_unlock(&g_audio.mtx);
		}
	}
	free(agc_pcm);
	trim_inplace(out_text);
	collapse_repetitions(out_text, out_sz);
}

/* Dedicated background worker thread for ctx_main so the 20ms PipeWire VAD loop
 * never blocks during speculative or final Whisper passes.
 */
static void *
decode_worker_fn(void *arg)
{
	(void)arg;
	while (running) {
		pthread_mutex_lock(&g_dec.mtx);
		while (running && !g_dec.has_job)
			pthread_cond_wait(&g_dec.cond, &g_dec.mtx);
		if (!running) {
			pthread_mutex_unlock(&g_dec.mtx);
			break;
		}

		g_dec.has_job = 0;
		g_dec.busy = 1;
		g_dec.abort_flag = 0;
		int n_samples = g_dec.n_samples;
		char lang[32];
		snprintf(lang, sizeof(lang), "%s", g_dec.lang);
		const char *prompt = g_dec.prompt;
		int max_tok = g_dec.max_tok;
		int is_partial = g_dec.is_partial;
		int64_t job_vf = g_dec.job_voiced_f;
		int64_t job_ef = g_dec.job_end_f;
		pthread_mutex_unlock(&g_dec.mtx);

		char decoded[4096] = {0};
		int all_silent = 0;
		run_whisper_slice(g_dec.ctx, g_dec.n_threads, g_dec.pcm, n_samples,
		                  lang, prompt, max_tok, 0, is_partial, 1,
		                  decoded, sizeof(decoded), &all_silent);

		pthread_mutex_lock(&g_audio.mtx);
		int64_t cur_audio_f = g_audio.total_frames;
		pthread_mutex_unlock(&g_audio.mtx);

		pthread_mutex_lock(&g_dec.mtx);
		g_dec.busy = 0;
		g_dec.done_at_frame = cur_audio_f;
		if (!g_dec.abort_flag && !g_cancel_req) {
			snprintf(g_dec.result_text, sizeof(g_dec.result_text), "%s", decoded);
			g_dec.result_voiced_f = job_vf;
			g_dec.result_end_f = job_ef;
			g_dec.result_is_partial = is_partial;
			g_dec.result_all_silent = all_silent;
			g_dec.has_result = 1;
		}
		pthread_cond_broadcast(&g_dec.cond);
		pthread_mutex_unlock(&g_dec.mtx);
	}
	return NULL;
}

static void
decode_reset_session(void)
{
	pthread_mutex_lock(&g_dec.mtx);
	g_dec.has_job = 0;
	g_dec.abort_flag = 1;
	while (g_dec.busy)
		pthread_cond_wait(&g_dec.cond, &g_dec.mtx);
	g_dec.abort_flag = 0;
	g_dec.has_result = 0;
	g_dec.result_text[0] = '\0';
	g_dec.result_voiced_f = -1;
	g_dec.result_end_f = -1;
	g_dec.result_all_silent = 0;
	g_dec.done_at_frame = 0;
	pthread_mutex_unlock(&g_dec.mtx);
}

static int
decode_try_submit(int64_t start_f, int64_t end_f, int64_t voiced_f,
                  const char *lang, const char *prompt, int max_tok, int is_partial)
{
	pthread_mutex_lock(&g_dec.mtx);
	if (g_dec.busy || g_dec.has_job) {
		pthread_mutex_unlock(&g_dec.mtx);
		return 0;
	}
	int n_samp = ring_extract_pcm(start_f, end_f, g_dec.pcm,
	                              MAX_UTTER_FRAMES * FRAME_SAMPLES);
	if (n_samp < 2400) {
		pthread_mutex_unlock(&g_dec.mtx);
		return 0;
	}
	g_dec.n_samples = n_samp;
	snprintf(g_dec.lang, sizeof(g_dec.lang), "%s", lang ? lang : "kbd");
	g_dec.prompt = prompt;
	g_dec.max_tok = max_tok;
	g_dec.is_partial = is_partial;
	g_dec.job_voiced_f = voiced_f;
	g_dec.job_end_f = end_f;
	g_dec.abort_flag = 0;
	g_dec.has_job = 1;
	pthread_cond_broadcast(&g_dec.cond);
	pthread_mutex_unlock(&g_dec.mtx);
	return 1;
}

static void
decode_force_submit_final(int64_t start_f, int64_t end_f, int64_t voiced_f,
                          const char *lang, const char *prompt, int max_tok)
{
	pthread_mutex_lock(&g_dec.mtx);
	if (g_dec.busy) {
		if (!g_dec.is_partial && g_dec.job_voiced_f >= voiced_f - 1 &&
		    g_dec.job_end_f >= voiced_f + 20) {
			pthread_mutex_unlock(&g_dec.mtx);
			return;
		}
		g_dec.abort_flag = 1;
		while (g_dec.busy)
			pthread_cond_wait(&g_dec.cond, &g_dec.mtx);
	} else if (!g_dec.has_job && g_dec.has_result &&
	           !g_dec.result_is_partial && g_dec.result_voiced_f >= voiced_f - 1 &&
	           g_dec.result_end_f >= voiced_f + 20) {
		pthread_mutex_unlock(&g_dec.mtx);
		return;
	}
	g_dec.has_job = 0;
	g_dec.has_result = 0;
	int n_samp = ring_extract_pcm(start_f, end_f, g_dec.pcm,
	                              MAX_UTTER_FRAMES * FRAME_SAMPLES);
	if (n_samp >= 2400) {
		g_dec.n_samples = n_samp;
		snprintf(g_dec.lang, sizeof(g_dec.lang), "%s", lang ? lang : "kbd");
		g_dec.prompt = prompt;
		g_dec.max_tok = max_tok;
		g_dec.is_partial = 0;
		g_dec.job_voiced_f = voiced_f;
		g_dec.job_end_f = end_f;
		g_dec.abort_flag = 0;
		g_dec.has_job = 1;
		pthread_cond_broadcast(&g_dec.cond);
	}
	pthread_mutex_unlock(&g_dec.mtx);
}

static int
match_wake_prefix(const char *text, const char *wake_word, char *out_cmd, size_t out_sz)
{
	return jarvis_voice_match_wake(get_daemon_ont(), text, wake_word, false, out_cmd, out_sz);
}

static int
match_wake_anywhere(const char *text, const char *wake_word, char *out_cmd, size_t out_sz)
{
	return jarvis_voice_match_wake(get_daemon_ont(), text, wake_word, true, out_cmd, out_sz);
}

static int
contains_wake_word(const char *text, const char *wake_word)
{
	return match_wake_anywhere(text, wake_word, NULL, 0) > 0;
}

static char g_last_tts_cache[2048] = {0};
static long g_last_tts_ms = 0;

static void
clear_tts_speaking_cache(void)
{
	g_last_tts_cache[0] = '\0';
	g_last_tts_ms = 0;
}

static void
interrupt_tts_and_clear_cache(void)
{
	clear_tts_speaking_cache();
	jarvis_xy_tts_interrupt(0);
}

static int
is_tts_file_active(void)
{
	char path[256];
	snprintf(path, sizeof(path), "/tmp/jarvis-tts-active-%s.state", get_username());
	return (access(path, R_OK) == 0) ? 1 : 0;
}

static int
is_tts_currently_speaking(char *out_text, size_t out_sz)
{
	if (out_text && out_sz > 0)
		out_text[0] = '\0';
	char path[256];
	snprintf(path, sizeof(path), "/tmp/jarvis-tts-active-%s.state", get_username());
	FILE *fp = fopen(path, "r");
	if (fp) {
		char buf[2048] = {0};
		size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
		fclose(fp);
		buf[n] = '\0';
		for (size_t i = 0; i < n; i++) {
			if (buf[i] == '\n' || buf[i] == '\r')
				buf[i] = ' ';
		}
		trim_inplace(buf);
		if (buf[0]) {
			snprintf(g_last_tts_cache, sizeof(g_last_tts_cache), "%s", buf);
			g_last_tts_ms = now_ms();
		}
		if (out_text && out_sz > 0)
			snprintf(out_text, out_sz, "%.*s", (int)(out_sz - 1), buf);
		return 1;
	}
	/* 2000ms post-TTS acoustic grace window so room reverb / PipeWire drain tail
	 * plus endpointing + Whisper decode latency after pw-cat exits is still
	 * recognized as TTS self-echo (cleared immediately on explicit barge-in).
	 */
	if (g_last_tts_cache[0] != '\0' && (now_ms() - g_last_tts_ms) <= 2000L) {
		if (out_text && out_sz > 0)
			snprintf(out_text, out_sz, "%.*s", (int)(out_sz - 1), g_last_tts_cache);
		return 1;
	}
	return 0;
}

static int
is_bargein_stop_phrase(const char *text)
{
	if (!text || !*text)
		return 0;
	char norm[256];
	jarvis_clean_phrase(text, norm, sizeof(norm));
	return jarvis_ontology_is_tts_interrupt(get_daemon_ont(), norm) ? 1 : 0;
}

/*
 * Exact Acoustic Self-Echo Verification (`is_tts_self_echo`):
 * Compares the normalized words decoded by Whisper during TTS playback against
 * the active spoken TTS transcript (`/tmp/jarvis-tts-active-$USER.state`).
 * If >= 60% of the heard words (>= 3 chars) appear in the currently playing TTS
 * text, it is speaker bleed / room echo rather than user barge-in.
 */
static int
is_tts_self_echo(const char *heard_text, const char *active_tts_text)
{
	if (!heard_text || !*heard_text || !active_tts_text || !*active_tts_text)
		return 0;

	char h_clean[512], t_clean[2048], t_pad[2056];
	jarvis_clean_phrase(heard_text, h_clean, sizeof(h_clean));
	jarvis_clean_phrase(active_tts_text, t_clean, sizeof(t_clean));
	if (!h_clean[0] || !t_clean[0])
		return 0;

	/* Exact substring check first */
	if (strstr(t_clean, h_clean) != NULL)
		return 1;

	snprintf(t_pad, sizeof(t_pad), " %s ", t_clean);
	char *save = NULL;
	int total_w = 0, matched_w = 0;
	for (char *tok = strtok_r(h_clean, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
		size_t tl = strlen(tok);
		if (tl < 3) continue;
		total_w++;
		char probe[128];
		snprintf(probe, sizeof(probe), " %s ", tok);
		if (strstr(t_pad, probe) != NULL || strstr(t_clean, tok) != NULL) {
			matched_w++;
		}
	}
	if (total_w == 0)
		return 1;
	return (matched_w * 100 >= total_w * 60) ? 1 : 0;
}

static void
jarvis_set_active(int active)
{
	const char *u = get_username();
	char state_file[256], jpid_file[256], apid_file[256];
	char legacy_state[256], legacy_jpid[256], legacy_apid[256];
	snprintf(state_file, sizeof(state_file), "/tmp/jarvis-voice-%s.state", u);
	snprintf(jpid_file, sizeof(jpid_file), "/tmp/jarvis-%s.pid", u);
	snprintf(apid_file, sizeof(apid_file), "/tmp/jarvis-acting-%s.pid", u);
	snprintf(legacy_state, sizeof(legacy_state), "/tmp/dwm-voice-%s.state", u);
	snprintf(legacy_jpid, sizeof(legacy_jpid), "/tmp/dwm-jarvis-%s.pid", u);
	snprintf(legacy_apid, sizeof(legacy_apid), "/tmp/dwm-acting-%s.pid", u);
	if (active) {
		g_jarvis_active = 1;
		FILE *fp = fopen(jpid_file, "w");
		if (fp) {
			fprintf(fp, "%d\n", (int)getpid());
			fclose(fp);
		}
		write_state_file(state_file, "jarvis", NULL, NULL);
	} else {
		g_jarvis_active = 0;
		g_cancel_req = 1;
		g_dec.abort_flag = 1;
		interrupt_tts_and_clear_cache();
		unlink(jpid_file);
		unlink(apid_file);
		unlink(state_file);
		unlink(legacy_jpid);
		unlink(legacy_apid);
		unlink(legacy_state);
		notify_bar();
	}
}

static void
poll_control_socket(int srv, int cli, int is_jarvis_idle)
{
	if (cli >= 0) {
		struct pollfd cpfd;
		cpfd.fd = cli;
		cpfd.events = POLLIN | POLLHUP | POLLERR;
		cpfd.revents = 0;
		if (poll(&cpfd, 1, 0) > 0) {
			char peek;
			if ((cpfd.revents & (POLLHUP | POLLERR)) ||
			    ((cpfd.revents & POLLIN) && recv(cli, &peek, 1, MSG_PEEK | MSG_DONTWAIT) == 0)) {
				g_cancel_req = 1;
				g_dec.abort_flag = 1;
				return;
			}
		}
	}

	struct pollfd pfd;
	pfd.fd = srv;
	pfd.events = POLLIN;
	pfd.revents = 0;

	while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
		struct sockaddr_in caddr;
		socklen_t clen = sizeof(caddr);
		int cfd = accept(srv, (struct sockaddr *)&caddr, &clen);
		if (cfd < 0)
			break;
		fcntl(cfd, F_SETFD, FD_CLOEXEC);

		char cbuf[2048];
		ssize_t n = read(cfd, cbuf, sizeof(cbuf) - 1);
		if (n <= 0) {
			close(cfd);
			continue;
		}
		cbuf[n] = '\0';

		if (strncmp(cbuf, "GET /", 5) == 0 || strncmp(cbuf, "PING", 4) == 0) {
			const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
			write_all(cfd, ok, strlen(ok));
			close(cfd);
		} else if (strncmp(cbuf, "STATUS", 6) == 0) {
			char sbuf[256];
			pthread_mutex_lock(&g_audio.mtx);
			int64_t tf = g_audio.total_frames;
			double ldb = (tf > 0) ? g_audio.db[(tf - 1) % RING_FRAMES] : -100.0;
			snprintf(sbuf, sizeof(sbuf),
			         "STATUS|frames=%lld|nf_db=%.1f|std_db=%.1f|last_db=%.1f|pw_open=%d|in_speech=%d|jarvis=%d\n",
			         (long long)tf, g_audio.noise_floor_db, g_audio.ambient_std_db,
			         ldb, g_audio.pw_open, g_audio.in_speech_hint, (int)g_jarvis_active);
			pthread_mutex_unlock(&g_audio.mtx);
			write_all(cfd, sbuf, strlen(sbuf));
			close(cfd);
		} else if (strncmp(cbuf, "JARVIS_STATUS", 13) == 0) {
			const char *st = g_jarvis_active ? "ON\n" : "OFF\n";
			write_all(cfd, st, strlen(st));
			close(cfd);
		} else if (strncmp(cbuf, "JARVIS_ON", 9) == 0) {
			jarvis_set_active(1);
			write_all(cfd, "ON\n", 3);
			close(cfd);
		} else if (strncmp(cbuf, "JARVIS_OFF", 10) == 0) {
			jarvis_set_active(0);
			write_all(cfd, "OFF\n", 4);
			close(cfd);
		} else if (strncmp(cbuf, "JARVIS_TOGGLE", 13) == 0) {
			int next = !g_jarvis_active;
			jarvis_set_active(next);
			const char *st = next ? "ON\n" : "OFF\n";
			write_all(cfd, st, strlen(st));
			close(cfd);
		} else if (strncmp(cbuf, "TTS_INTERRUPT", 13) == 0) {
			interrupt_tts_and_clear_cache();
			write_all(cfd, "OK\n", 3);
			close(cfd);
		} else if (strncmp(cbuf, "TTS_SPEAK|", 10) == 0) {
			char *txt = cbuf + 10;
			char *nl = strpbrk(txt, "\r\n");
			if (nl) *nl = '\0';
			if (*txt) {
				interrupt_tts_and_clear_cache();
				jarvis_xy_speak_clause("oneshot", txt, 1);
			}
			write_all(cfd, "OK\n", 3);
			close(cfd);
		} else if (strncmp(cbuf, "TTS_NOTIFY|", 11) == 0) {
			char *payload = cbuf + 11;
			char *nl = strpbrk(payload, "\r\n");
			if (nl) *nl = '\0';
			char *sep = strchr(payload, '|');
			const char *title = "  Jarbas";
			const char *msg = payload;
			if (sep) {
				*sep = '\0';
				if (payload[0]) title = payload;
				msg = sep + 1;
			}
			if (*msg) {
				jarvis_xy_notify(title, msg, 1);
				if (!g_audio.in_speech_hint) {
					jarvis_xy_speak_clause("notify", msg, 0);
				}
			}
			write_all(cfd, "OK\n", 3);
			close(cfd);
		} else if (strncmp(cbuf, "STOP", 4) == 0) {
			g_stop_req = 1;
			write_all(cfd, "OK\n", 3);
			close(cfd);
		} else if (strncmp(cbuf, "CANCEL", 6) == 0) {
			g_cancel_req = 1;
			g_dec.abort_flag = 1;
			write_all(cfd, "OK\n", 3);
			close(cfd);
		} else if (is_jarvis_idle &&
		           (strncmp(cbuf, "STREAM|", 7) == 0 || strncmp(cbuf, "JARVIS_WAIT|", 12) == 0) &&
		           g_preempt_cli < 0) {
			g_preempt_cli = cfd;
			snprintf(g_preempt_buf, sizeof(g_preempt_buf), "%s", cbuf);
			g_cancel_req = 1;
			g_dec.abort_flag = 1;
			break;
		} else {
			close(cfd);
		}
	}
}

static void
wait_next_audio_frame(int64_t target_frame, int max_wait_ms)
{
	pthread_mutex_lock(&g_audio.mtx);
	if (g_audio.total_frames < target_frame && running && !g_stop_req && !g_cancel_req) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += (long)max_wait_ms * 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec += 1;
			ts.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&g_audio.cond, &g_audio.mtx, &ts);
	}
	pthread_mutex_unlock(&g_audio.mtx);
}

/* Unified Live Streaming Handler:
 *   STREAM|<mode:cmd|type|clip>|<lang>|<thresh_db>|<silence_ms>|<wait_ms>|<state_file>
 *
 *Features:
 *  1. Dynamic Peak-Relative Endpointing: tracks peak_speech_db during the utterance
 *     and sets eff_keep_db = max(eff_start_db - 3.5, peak_speech_db - 14.0).
 *     Even if room/fan noise is -28 dB, once you speak at -14 dB, dropping back to
 *     -28 dB immediately counts as silence and stops reliably!
 *  2. Transcript Stability Endpointing: if two consecutive streaming decodes return
 *     the exact same non-empty text (or Whisper reports no_speech on trailing audio)
 *     and your energy has dropped 8 dB below peak for >=360ms, auto-stops cleanly.
 */
static void
handle_stream(int srv, int cli, const char *mode, const char *lang,
              double thresh_db, int silence_ms, int wait_ms, const char *state_file)
{
	int is_cmd = (strcmp(mode, "cmd") == 0);
	int max_tok = is_cmd ? 64 : 112;
	const char *cmd_prompt = is_cmd ? get_cmd_initial_prompt() : NULL;
	const char *state_prefix = is_cmd ? "cmd" : "rec";
	const char *osd_title = is_cmd
	                      ? jarvis_ontology_get_ui_string(get_daemon_ont(), "command_streaming", "  Command (Streaming)...")
	                      : jarvis_ontology_get_ui_string(get_daemon_ont(), "dictation_streaming", "  Dictation (Streaming)...");
	int preroll_f = 8; /* 160 ms clean pre-roll */

	g_stop_req = 0;
	g_cancel_req = 0;
	decode_reset_session();
	audio_acquire();

	pthread_mutex_lock(&g_audio.mtx);
	int64_t session_start_f = g_audio.total_frames;
	int64_t ignore_until_f = is_cmd ? (session_start_f + 9) : session_start_f;
	int64_t min_preroll_f  = is_cmd ? ignore_until_f :
	                         ((session_start_f > preroll_f) ? (session_start_f - preroll_f) : 0);
	int64_t scan_f = session_start_f;
	int64_t speech_start_f = min_preroll_f;
	pthread_mutex_unlock(&g_audio.mtx);

	int in_speech = 0;
	int consec_voiced = 0;
	int voiced_frames = 0;
	int silence_frames = 0;
	int trailing_soft_frames = 0;
	double peak_speech_db = -100.0;
	double utter_amb_avg = -42.0;
	double utter_amb_std = 1.5;
	double utter_start_db = -35.0;
	int64_t last_voiced_f = scan_f;
	int64_t decoded_voiced_f = -1;
	char live_text[4096] = {0};
	long t_start = now_ms();
	(void)thresh_db;
	(void)trailing_soft_frames;

	while (running && !g_stop_req && !g_cancel_req) {
		poll_control_socket(srv, cli, 0);
		if (g_stop_req || g_cancel_req)
			break;

		wait_next_audio_frame(scan_f + 1, 20);

		/* Pick up any completed non-blocking background preview decode */
		pthread_mutex_lock(&g_dec.mtx);
		if (g_dec.has_result) {
			g_dec.has_result = 0;
			char rtext[4096];
			snprintf(rtext, sizeof(rtext), "%s", g_dec.result_text);
			int64_t rvf = g_dec.result_voiced_f;
			pthread_mutex_unlock(&g_dec.mtx);

			if (rtext[0] != '\0') {
				decoded_voiced_f = rvf;
				if (strcmp(live_text, rtext) != 0) {
					snprintf(live_text, sizeof(live_text), "%s", rtext);
					write_state_file(state_file, state_prefix, live_text, osd_title);
				}
			}
		} else {
			pthread_mutex_unlock(&g_dec.mtx);
		}

		pthread_mutex_lock(&g_audio.mtx);
		int64_t avail_f = g_audio.total_frames;
		if (avail_f - scan_f > RING_FRAMES - 4)
			scan_f = avail_f - (RING_FRAMES - 4);

		while (scan_f < avail_f) {
			float db = g_audio.db[scan_f % RING_FRAMES];
			if (scan_f < ignore_until_f) {
				scan_f++;
				continue;
			}
			if (!in_speech) {
				double amb_avg = g_audio.noise_floor_db;
				double amb_std = g_audio.ambient_std_db;
				calc_ambient_window_locked(scan_f - consec_voiced, 80, &amb_avg, &amb_std);
				double eff_start_db = calc_vad_start_db(amb_avg, amb_std);
				if (db >= eff_start_db) {
					consec_voiced++;
					if (consec_voiced >= 2) {
						in_speech = 1;
						g_audio.in_speech_hint = 1;
						voiced_frames = consec_voiced;
						silence_frames = 0;
						trailing_soft_frames = 0;
						peak_speech_db = db;
						utter_amb_avg = amb_avg;
						utter_amb_std = amb_std;
						utter_start_db = eff_start_db;
						speech_start_f = (scan_f > preroll_f) ? (scan_f - preroll_f) : 0;
						if (speech_start_f < min_preroll_f)
							speech_start_f = min_preroll_f;
						last_voiced_f = scan_f;
					}
				} else {
					consec_voiced = 0;
				}
			} else {
				if (db > peak_speech_db)
					peak_speech_db = db;

				double eff_keep_db = calc_vad_keep_db(utter_amb_avg, utter_amb_std,
				                                      utter_start_db, peak_speech_db);

				if (db >= eff_keep_db) {
					voiced_frames++;
					silence_frames = 0;
					last_voiced_f = scan_f;
				} else {
					silence_frames++;
				}
			}
			scan_f++;
		}
		pthread_mutex_unlock(&g_audio.mtx);

		if (!in_speech) {
			if (wait_ms > 0 && (now_ms() - t_start) >= wait_ms) {
				decode_reset_session();
				audio_release(WARM_HOLD_MS);
				write_all(cli, "TIMEOUT\n", 8);
				return;
			}
			continue;
		}

		/* Check auto-endpointing */
		if (silence_ms > 0 && (silence_frames * FRAME_MS) >= silence_ms)
			break;
		if ((scan_f - speech_start_f) >= MAX_UTTER_FRAMES - 10)
			break;

		/* Periodic live preview while speaking */
		if (silence_frames < 5 && (scan_f - speech_start_f) >= 16 &&
		    last_voiced_f > decoded_voiced_f + 10) {
			pthread_mutex_lock(&g_dec.mtx);
			int can_run = (!g_dec.busy && !g_dec.has_job &&
			               (scan_f - g_dec.done_at_frame) >= 15);
			pthread_mutex_unlock(&g_dec.mtx);
			if (can_run) {
				int64_t end_f = last_voiced_f + 6;
				if (end_f > scan_f)
					end_f = scan_f;
				decode_try_submit(speech_start_f, end_f, last_voiced_f, lang, cmd_prompt, max_tok, 1);
			}
		}
	}

	if (g_cancel_req) {
		decode_reset_session();
		audio_release(WARM_HOLD_MS);
		write_all(cli, "CANCELLED\n", 10);
		return;
	}

	if (!in_speech && g_stop_req) {
		pthread_mutex_lock(&g_audio.mtx);
		int64_t cur_f = g_audio.total_frames;
		pthread_mutex_unlock(&g_audio.mtx);
		speech_start_f = (cur_f > 25) ? (cur_f - 25) : 0;
		last_voiced_f = cur_f;
		voiced_frames = 10;
	}

	if (voiced_frames * FRAME_MS < 160 && !g_stop_req) {
		decode_reset_session();
		audio_release(WARM_HOLD_MS);
		write_all(cli, "EMPTY\n", 6);
		return;
	}

	/* Authoritative final pass covering last_voiced_f + 520ms tail (26 frames) */
	{
		int64_t end_f = last_voiced_f + 26;
		decode_force_submit_final(speech_start_f, end_f, last_voiced_f, lang, cmd_prompt, max_tok);
	}

	pthread_mutex_lock(&g_dec.mtx);
	while ((g_dec.busy || g_dec.has_job) && running && !g_cancel_req)
		pthread_cond_wait(&g_dec.cond, &g_dec.mtx);
	if (g_dec.has_result && g_dec.result_text[0] != '\0') {
		snprintf(live_text, sizeof(live_text), "%s", g_dec.result_text);
		g_dec.has_result = 0;
	}
	pthread_mutex_unlock(&g_dec.mtx);

	audio_release(WARM_HOLD_MS);

	if (live_text[0] == '\0') {
		write_all(cli, "EMPTY\n", 6);
	} else {
		char reply[4200];
		snprintf(reply, sizeof(reply), "OK|%s\n", live_text);
		write_all(cli, reply, strlen(reply));
	}
}

/* Always-On Streaming Jarvis Wake + Command handler (with optional follow-up window):
 *   JARVIS_WAIT|<lang>|<thresh_db>|<cmd_silence_ms>|<wake_word>|<state_file>|<chime_wav>[|<followup_ms>]
 */
static int
handle_jarvis_wait(int srv, int cli, struct whisper_context *ctx_wake, int n_threads,
                   const char *lang, double thresh_db, int cmd_silence_ms,
                   const char *wake_word, const char *state_file, const char *chime_wav,
                   int followup_ms, char *out_cmd, size_t out_sz)
{
	int preroll_f = 15; /* 300 ms pre-roll so soft initial consonant of "Jarvis" is never clipped */
	if (out_cmd && out_sz > 0)
		out_cmd[0] = '\0';
	float *wake_pcm = malloc((size_t)MAX_UTTER_FRAMES * FRAME_SAMPLES * sizeof(float));
	if (!wake_pcm) {
		if (cli >= 0)
			write_all(cli, "EMPTY\n", 6);
		return 2;
	}

	g_stop_req = 0;
	g_cancel_req = 0;
	decode_reset_session();
	audio_acquire();

	pthread_mutex_lock(&g_audio.mtx);
	int64_t scan_f = g_audio.total_frames;
	pthread_mutex_unlock(&g_audio.mtx);

	int in_speech = 0;
	int consec_voiced = 0;
	int voiced_frames = 0;
	int silence_frames = 0;
	int trailing_soft_frames = 0;
	double peak_speech_db = -100.0;
	double utter_amb_avg = -42.0;
	double utter_amb_std = 1.5;
	double utter_start_db = -35.0;
	int64_t speech_start_f = scan_f;
	int64_t last_voiced_f = scan_f;
	(void)thresh_db;

	int is_followup_window = (followup_ms > 0) ? 1 : 0;
	int wake_confirmed = is_followup_window;
	int waiting_post_wake = is_followup_window;
	int chime_played = is_followup_window;
	int non_jarvis_utter = 0;
	long post_wake_deadline_ms = is_followup_window ? (now_ms() + (long)followup_ms) : 0;
	/* Ignore first 12 frames (240ms) on follow-up entry so completion chime
	 * (/tmp/dwm-voice-done-*.wav, ~80ms + room reverb) and TTS tail never trigger
	 * false follow-up recording!
	 */
	int64_t chime_ignore_until_f = 0;
	int64_t wake_end_f = 0;
	int64_t cmd_start_f = 0;

	/* On follow-up entry, never rewind into the previous command's audio buffer
	 * (which caused trailing words of the previous utterance or TTS startup to be
	 * re-ingested). Start scanning 8 frames (160ms) forward.
	 */
	if (is_followup_window) {
		chime_ignore_until_f = scan_f + 8;
		wake_end_f = chime_ignore_until_f;
		cmd_start_f = chime_ignore_until_f;
	}
	int64_t decoded_wake_voiced_f = -1;
	int64_t decoded_cmd_voiced_f = -1;
	int64_t decoded_cmd_end_f = -1;
	int decoded_cmd_is_final = 0;
	int tts_was_active_in_utter = is_tts_currently_speaking(NULL, 0) ? 1 : 0;
	char live_cmd[4096] = {0};

	if (is_followup_window)
		write_state_file(state_file, "jarvis-cmd", NULL, NULL);

	if (cmd_silence_ms < 640)
		cmd_silence_ms = 640;

	while (running && !g_stop_req && !g_cancel_req) {
		poll_control_socket(srv, cli, !wake_confirmed || waiting_post_wake);
		if (g_stop_req || g_cancel_req)
			break;

		wait_next_audio_frame(scan_f + 1, 20);

		if (in_speech && is_tts_currently_speaking(NULL, 0))
			tts_was_active_in_utter = 1;

		/* Pick up any completed non-blocking preview or speculative final decode */
		if (wake_confirmed && !waiting_post_wake) {
			pthread_mutex_lock(&g_dec.mtx);
			if (g_dec.has_result) {
				g_dec.has_result = 0;
				char raw_res[4096];
				snprintf(raw_res, sizeof(raw_res), "%s", g_dec.result_text);
				int64_t res_vf = g_dec.result_voiced_f;
				int64_t res_ef = g_dec.result_end_f;
				int res_part = g_dec.result_is_partial;
				pthread_mutex_unlock(&g_dec.mtx);

				if (raw_res[0] != '\0') {
					char stripped[2048];
					int wm = match_wake_prefix(raw_res, wake_word,
					                           stripped, sizeof(stripped));
					if (wm == 0 && !chime_played)
						wm = match_wake_anywhere(raw_res, wake_word,
						                         stripped, sizeof(stripped));
					const char *candidate = NULL;
					char active_tts[2048] = {0};
					int tts_on = is_tts_currently_speaking(active_tts, sizeof(active_tts));
					if (!tts_on && tts_was_active_in_utter && g_last_tts_cache[0]) {
						snprintf(active_tts, sizeof(active_tts), "%s", g_last_tts_cache);
					}
					if (wm == 2 && stripped[0]) {
						candidate = stripped;
						if (tts_on)
							interrupt_tts_and_clear_cache();
					} else if (wm == 1) {
						if (tts_on)
							interrupt_tts_and_clear_cache();
					} else if (wm == 0) {
						if (tts_on || (tts_was_active_in_utter && active_tts[0])) {
							if (is_bargein_stop_phrase(raw_res)) {
								interrupt_tts_and_clear_cache();
								candidate = raw_res;
							} else if (!is_tts_self_echo(raw_res, active_tts)) {
								if (tts_on)
									interrupt_tts_and_clear_cache();
								candidate = raw_res;
							}
							/* While TTS is speaking (or overlapped this utterance), suppress
							 * acoustic self-echo, while allowing explicit stop phrases or any
							 * verified non-self-echo follow-up / barge-in speech.
							 */
						} else {
							candidate = raw_res;
						}
					}

					decoded_cmd_voiced_f = res_vf;
					decoded_cmd_end_f = res_ef;
					if (candidate && *candidate) {
						if (!res_part && res_vf >= last_voiced_f && res_ef >= last_voiced_f + 20)
							decoded_cmd_is_final = 1;
						if (strcmp(live_cmd, candidate) != 0) {
							snprintf(live_cmd, sizeof(live_cmd), "%s", candidate);
							write_state_file(state_file, "cmd", live_cmd,
							                 jarvis_ontology_get_ui_string(get_daemon_ont(), "command_live", "  Jarvis Command (Live)..."));
						}
					}
				}
			} else {
				pthread_mutex_unlock(&g_dec.mtx);
			}
		}

		int tts_now_hint = is_tts_currently_speaking(NULL, 0) ? 1 : 0;
		pthread_mutex_lock(&g_audio.mtx);
		int64_t avail_f = g_audio.total_frames;
		if (avail_f - scan_f > RING_FRAMES - 4)
			scan_f = avail_f - (RING_FRAMES - 4);

		while (scan_f < avail_f) {
			float db = g_audio.db[scan_f % RING_FRAMES];

			if (scan_f < chime_ignore_until_f) {
				scan_f++;
				continue;
			}

			if (wake_confirmed && waiting_post_wake) {
				double amb_avg = g_audio.noise_floor_db;
				double amb_std = g_audio.ambient_std_db;
				calc_ambient_window_locked(scan_f - consec_voiced, 80, &amb_avg, &amb_std);
				/* While RED (post-wake or follow-up), use a slightly more sensitive
				 * onset threshold (-1.0 dB) and trigger on 2 consecutive frames (40ms)
				 * with a 10-frame (200ms) pre-roll so normal or soft clarifications
				 * are captured immediately from the first consonant.
				 */
				double eff_start_db = calc_vad_start_db(amb_avg, amb_std) - 1.0;
				if (db >= eff_start_db) {
					consec_voiced++;
					if (consec_voiced >= 2) {
						waiting_post_wake = 0;
						in_speech = 1;
						g_audio.in_speech_hint = 1;
						tts_was_active_in_utter = tts_now_hint;
						voiced_frames = consec_voiced;
						silence_frames = 0;
						trailing_soft_frames = 0;
						peak_speech_db = db;
						utter_amb_avg = amb_avg;
						utter_amb_std = amb_std;
						utter_start_db = eff_start_db;
						cmd_start_f = (scan_f > 10) ? (scan_f - 10) : 0;
						if (cmd_start_f < wake_end_f)
							cmd_start_f = wake_end_f;
						last_voiced_f = scan_f;
						decoded_cmd_voiced_f = -1;
						decoded_cmd_is_final = 0;
					}
				} else {
					consec_voiced = 0;
				}
			} else if (!in_speech) {
				double amb_avg = g_audio.noise_floor_db;
				double amb_std = g_audio.ambient_std_db;
				calc_ambient_window_locked(scan_f - consec_voiced, 80, &amb_avg, &amb_std);
				double eff_start_db = calc_vad_start_db(amb_avg, amb_std);
				if (db >= eff_start_db) {
					consec_voiced++;
					if (consec_voiced >= 2) {
						in_speech = 1;
						g_audio.in_speech_hint = 1;
						tts_was_active_in_utter = tts_now_hint;
						voiced_frames = consec_voiced;
						silence_frames = 0;
						trailing_soft_frames = 0;
						peak_speech_db = db;
						utter_amb_avg = amb_avg;
						utter_amb_std = amb_std;
						utter_start_db = eff_start_db;
						non_jarvis_utter = 0;
						speech_start_f = (scan_f > preroll_f) ? (scan_f - preroll_f) : 0;
						if (speech_start_f < chime_ignore_until_f)
							speech_start_f = chime_ignore_until_f;
						cmd_start_f = speech_start_f;
						last_voiced_f = scan_f;
						decoded_wake_voiced_f = -1;
						decoded_cmd_is_final = 0;
					}
				} else {
					consec_voiced = 0;
				}
			} else {
				/* Energy-Step Re-Anchoring:
				 * If continuous background audio (e.g. a phone playing video) already
				 * triggered in_speech=1 (!wake_confirmed) and then the user begins
				 * speaking "Jarvis" significantly louder (+5.5 dB above the recent
				 * 20-frame average), re-anchor speech_start_f right to the user's
				 * voice onset so the wake check decodes their clean utterance!
				 */
				if (!wake_confirmed && (scan_f - speech_start_f) > 25 && scan_f >= 24) {
					double recent_sum = 0.0;
					for (int64_t rf = scan_f - 22; rf < scan_f - 2; rf++)
						recent_sum += g_audio.db[rf % RING_FRAMES];
					double recent_avg = recent_sum / 20.0;
					if (db >= recent_avg + 5.5 &&
					    g_audio.db[(scan_f - 1) % RING_FRAMES] >= recent_avg + 4.5) {
						speech_start_f = (scan_f > preroll_f) ? (scan_f - preroll_f) : 0;
						cmd_start_f = speech_start_f;
						voiced_frames = 2;
						silence_frames = 0;
						trailing_soft_frames = 0;
						peak_speech_db = db;
						utter_amb_avg = recent_avg;
						utter_start_db = recent_avg + 4.5;
						decoded_wake_voiced_f = -1;
					}
				}

				if (db > peak_speech_db)
					peak_speech_db = db;

				/* Dynamic Pre-Speech Ambient Snapshot + Peak-Relative Keep Threshold:
				 * Uses the frozen pre-speech ambient average (utter_amb_avg) and spread
				 * (utter_amb_std) measured right before speech started, combined with
				 * peak-relative drop detection (peak_speech_db - 13.5 dB).
				 */
				double eff_keep_db = calc_vad_keep_db(utter_amb_avg, utter_amb_std,
				                                      utter_start_db, peak_speech_db);

				if (db >= eff_keep_db) {
					voiced_frames++;
					silence_frames = 0;
					last_voiced_f = scan_f;
					decoded_cmd_is_final = 0;
					if (db >= peak_speech_db - 16.0) {
						trailing_soft_frames = 0;
					} else {
						trailing_soft_frames++;
					}
				} else {
					silence_frames++;
					trailing_soft_frames++;
				}
			}
			scan_f++;
		}
		pthread_mutex_unlock(&g_audio.mtx);

		if (wake_confirmed && waiting_post_wake) {
			if (is_followup_window && is_tts_file_active()) {
				long ext_dl = now_ms() + (long)followup_ms;
				if (ext_dl > post_wake_deadline_ms)
					post_wake_deadline_ms = ext_dl;
			}
			if (now_ms() >= post_wake_deadline_ms) {
				if (is_followup_window) {
					is_followup_window = 0;
					wake_confirmed = 0;
					waiting_post_wake = 0;
					chime_played = 0;
					write_state_file(state_file, "jarvis", NULL, NULL);
					continue;
				}
				restore_speaker_after_command(NULL);
				decode_reset_session();
				audio_release(WARM_HOLD_MS);
				free(wake_pcm);
				write_state_file(state_file, "jarvis", NULL, NULL);
				if (cli >= 0)
					write_all(cli, "TIMEOUT\n", 8);
				return 2;
			}
			continue;
		}

		if (!in_speech)
			continue;

		if (!wake_confirmed) {
			int64_t utter_f = scan_f - speech_start_f;

			if (non_jarvis_utter) {
				if (silence_frames >= 10 || utter_f > 200) {
					in_speech = 0;
					consec_voiced = 0;
					voiced_frames = 0;
					silence_frames = 0;
					non_jarvis_utter = 0;
					pthread_mutex_lock(&g_audio.mtx);
					g_audio.in_speech_hint = 0;
					pthread_mutex_unlock(&g_audio.mtx);
				}
				continue;
			}

			/* 1. Pause wake check: user spoke "Jarvis" (or "Jarvis <cmd>") and paused >= 160ms (8 frames)
			 *    after at least 10 voiced frames (200ms) or >= 240ms silence after shorter speech.
			 * 2. Continuous wake check: user has spoken continuously for >= 18 voiced frames (360ms)
			 *    without pausing (and repeats every 18 new voiced frames), so the indicator turns RED
			 *    immediately during a one-shot "<wake> <command>" utterance!
			 */
			char active_tts_now[2048] = {0};
			int tts_speaking_now = is_tts_currently_speaking(active_tts_now, sizeof(active_tts_now));
			int cont_step_f = tts_speaking_now ? 16 : 18;
			int pause_wake_check = (((silence_frames >= 8 && voiced_frames >= 10) ||
			                         (silence_frames >= 12 && voiced_frames >= 5)) &&
			                        last_voiced_f > decoded_wake_voiced_f &&
			                        (!tts_speaking_now || peak_speech_db >= utter_start_db + 8.0));
			int cont_wake_check  = (voiced_frames >= cont_step_f && silence_frames < 8 &&
			                        (!tts_speaking_now || peak_speech_db >= utter_start_db + 1.5) &&
			                        (decoded_wake_voiced_f < 0 ||
			                         (last_voiced_f - decoded_wake_voiced_f) >= cont_step_f));

			if (pause_wake_check || cont_wake_check) {
				int64_t end_f = cont_wake_check ? (last_voiced_f + 4) : (last_voiced_f + 6);
				if (end_f > scan_f)
					end_f = scan_f;
				/* Decode at most the most recent 75 frames (1.5s) up to end_f so
				 * continuous background audio never locks the wake slice onto stale
				 * audio from the start of the segment.
				 */
				int64_t wake_slice_start = speech_start_f;
				if (end_f - wake_slice_start > 75)
					wake_slice_start = end_f - 75;
				int64_t snap_voiced_f = last_voiced_f;
				int n_samp = ring_extract_pcm(wake_slice_start, end_f, wake_pcm,
				                              MAX_UTTER_FRAMES * FRAME_SAMPLES);
				if (n_samp >= 2400) {
					char heard[1024];
					char inline_cmd[1024];
					char wake_prompt_buf[128];
					const char *wake_prompt = build_wake_initial_prompt(wake_word, wake_prompt_buf, sizeof(wake_prompt_buf));
					int w_tok = cont_wake_check ? 16 : 28;
					int wake_all_silent = 0;
					run_whisper_slice(ctx_wake, n_threads, wake_pcm, n_samp,
					                  lang, wake_prompt, w_tok, 1, 0, 0,
					                  heard, sizeof(heard), &wake_all_silent);
					decoded_wake_voiced_f = snap_voiced_f;

					int wmatch = match_wake_prefix(heard, wake_word,
					                               inline_cmd, sizeof(inline_cmd));
					if (wmatch == 0) {
						wmatch = match_wake_anywhere(heard, wake_word,
						                             inline_cmd, sizeof(inline_cmd));
					}

					/* Check if the user kept speaking into the ring buffer while
					 * Whisper was decoding the wake slice! If so, never chop off
					 * those frames or play a chime over their ongoing voice.
					 */
					int spoke_during_decode = 0;
					pthread_mutex_lock(&g_audio.mtx);
					int64_t cur_tf = g_audio.total_frames;
					double chk_keep_db = calc_vad_keep_db(utter_amb_avg, utter_amb_std,
					                                      utter_start_db, peak_speech_db);
					double speech_cont_db = (peak_speech_db - 9.0 > chk_keep_db) ? (peak_speech_db - 9.0) : chk_keep_db;
					int v_during = 0;
					for (int64_t f = end_f; f < cur_tf; f++) {
						if (g_audio.db[f % RING_FRAMES] >= speech_cont_db)
							v_during++;
					}
					if (v_during >= 3)
						spoke_during_decode = 1;
					pthread_mutex_unlock(&g_audio.mtx);

					if (wmatch == 1 && !cont_wake_check && !spoke_during_decode) {
						/* User spoke ONLY wake word and genuinely paused: interrupt any TTS, warmup TTS engine, play chime, duck speaker, and wait for command */
						interrupt_tts_and_clear_cache();
						tts_was_active_in_utter = 0;
						jarvis_xy_speak_clause("warmup", "", 0);
						wake_confirmed = 1;
						chime_played = 1;
						play_chime_async(chime_wav);
						duck_speaker_for_command();
						waiting_post_wake = 1;
						in_speech = 0;
						consec_voiced = 0;
						voiced_frames = 0;
						silence_frames = 0;
						pthread_mutex_lock(&g_audio.mtx);
						g_audio.in_speech_hint = 0;
						chime_ignore_until_f = g_audio.total_frames + 5;
						wake_end_f = g_audio.total_frames + 2;
						pthread_mutex_unlock(&g_audio.mtx);
						post_wake_deadline_ms = now_ms() + 6000L;
						write_state_file(state_file, "jarvis-cmd", NULL, NULL);
						notify_osd_async(jarvis_ontology_get_ui_string(get_daemon_ont(), "listening_title", "  Jarvis Listening..."),
						                 jarvis_ontology_get_ui_string(get_daemon_ont(), "listening_body", "Speak your command now."), 4500);
						continue;
					} else if (wmatch >= 1) {
						/* User is speaking a continuous one-shot "<wake> <command>" utterance.
						 * Interrupt any active TTS immediately ("barge-in") and pre-warm TTS!
						 */
						interrupt_tts_and_clear_cache();
						jarvis_xy_speak_clause("warmup", "", 0);
						duck_speaker_for_command();
						wake_confirmed = 1;
						waiting_post_wake = 0;
						chime_played = 0;
						speech_start_f = wake_slice_start;
						cmd_start_f = wake_slice_start;
						pthread_mutex_lock(&g_audio.mtx);
						calc_ambient_window_locked(wake_slice_start, 80,
						                           &utter_amb_avg, &utter_amb_std);
						utter_start_db = calc_vad_start_db(utter_amb_avg, utter_amb_std);
						pthread_mutex_unlock(&g_audio.mtx);
						if (wmatch == 2 && inline_cmd[0] != '\0') {
							snprintf(live_cmd, sizeof(live_cmd), "%s", inline_cmd);
							decoded_cmd_voiced_f = snap_voiced_f;
							write_state_file(state_file, "cmd", live_cmd,
							                 jarvis_ontology_get_ui_string(get_daemon_ont(), "command_live", "  Jarvis Command (Live)..."));
						} else {
							live_cmd[0] = '\0';
							write_state_file(state_file, "jarvis-cmd", NULL, NULL);
						}
					} else if (tts_speaking_now && is_bargein_stop_phrase(heard)) {
						interrupt_tts_and_clear_cache();
						in_speech = 0;
						consec_voiced = 0;
						voiced_frames = 0;
						silence_frames = 0;
						decoded_wake_voiced_f = -1;
						pthread_mutex_lock(&g_audio.mtx);
						g_audio.in_speech_hint = 0;
						pthread_mutex_unlock(&g_audio.mtx);
						continue;
					} else if (tts_speaking_now && is_tts_self_echo(heard, active_tts_now)) {
						/* Acoustic self-echo of Jarvis's own currently playing TTS voice:
						 * reset VAD and adapt ambient baseline so speaker bleed is ignored.
						 */
						in_speech = 0;
						consec_voiced = 0;
						voiced_frames = 0;
						silence_frames = 0;
						decoded_wake_voiced_f = -1;
						pthread_mutex_lock(&g_audio.mtx);
						g_audio.in_speech_hint = 0;
						calc_ambient_window_locked(g_audio.total_frames, 80,
						                           &g_audio.noise_floor_db,
						                           &g_audio.ambient_std_db);
						pthread_mutex_unlock(&g_audio.mtx);
						continue;
					} else if (wake_all_silent || heard[0] == '\0') {
						/* Whisper decoded no speech tokens from this segment:
						 * reset VAD immediately and adapt ambient baseline so steady
						 * or fluctuating non-speech room noise never traps in_speech=1.
						 */
						in_speech = 0;
						consec_voiced = 0;
						voiced_frames = 0;
						silence_frames = 0;
						decoded_wake_voiced_f = -1;
						pthread_mutex_lock(&g_audio.mtx);
						g_audio.in_speech_hint = 0;
						calc_ambient_window_locked(g_audio.total_frames, 80,
						                           &g_audio.noise_floor_db,
						                           &g_audio.ambient_std_db);
						pthread_mutex_unlock(&g_audio.mtx);
						continue;
					} else if (pause_wake_check && !spoke_during_decode &&
					           (voiced_frames >= 20 || silence_frames >= 12)) {
						/* Finished a non-Jarvis phrase and paused: reset cleanly */
						in_speech = 0;
						consec_voiced = 0;
						voiced_frames = 0;
						silence_frames = 0;
						decoded_wake_voiced_f = -1;
						pthread_mutex_lock(&g_audio.mtx);
						g_audio.in_speech_hint = 0;
						pthread_mutex_unlock(&g_audio.mtx);
						continue;
					} else if ((snap_voiced_f - speech_start_f) >= 75) {
						/* Over 1.5s of continuous voiced audio (e.g. phone/TV or user chat)
						 * without wake word: unconditionally slide the window forward with a
						 * 15-frame (300ms) overlap even if sound continued during decode,
						 * and refresh the ambient baseline!
						 */
						pthread_mutex_lock(&g_audio.mtx);
						calc_ambient_window_locked(snap_voiced_f, 80,
						                           &utter_amb_avg, &utter_amb_std);
						utter_start_db = calc_vad_start_db(utter_amb_avg, utter_amb_std);
						pthread_mutex_unlock(&g_audio.mtx);
						speech_start_f = (snap_voiced_f > 15) ? (snap_voiced_f - 15) : snap_voiced_f;
						cmd_start_f = speech_start_f;
						voiced_frames = (int)(last_voiced_f - speech_start_f);
						if (voiced_frames < 0)
							voiced_frames = 0;
						peak_speech_db = utter_start_db + 2.0;
					}
				}
			}

			if (!wake_confirmed && silence_frames >= 14) {
				in_speech = 0;
				consec_voiced = 0;
				voiced_frames = 0;
				silence_frames = 0;
				decoded_wake_voiced_f = -1;
				pthread_mutex_lock(&g_audio.mtx);
				g_audio.in_speech_hint = 0;
				pthread_mutex_unlock(&g_audio.mtx);
				continue;
			}
		}

		if (wake_confirmed && !waiting_post_wake) {
			int64_t cmd_len_f = scan_f - cmd_start_f;
			char eff_wake_prompt_buf[128];
			const char *eff_prompt = chime_played
				? get_cmd_initial_prompt()
				: build_wake_initial_prompt(wake_word, eff_wake_prompt_buf, sizeof(eff_wake_prompt_buf));

			if (silence_frames < 8 && cmd_len_f >= 16 &&
			    last_voiced_f > decoded_cmd_voiced_f + 12) {
				pthread_mutex_lock(&g_dec.mtx);
				int can_run = (!g_dec.busy && !g_dec.has_job &&
				               (scan_f - g_dec.done_at_frame) >= 15);
				pthread_mutex_unlock(&g_dec.mtx);
				if (can_run) {
					int64_t end_f = last_voiced_f + 8;
					if (end_f > scan_f)
						end_f = scan_f;
					decode_try_submit(cmd_start_f, end_f, last_voiced_f,
					                  lang, eff_prompt, 96, 1);
				}
			} else if ((silence_frames >= 20 || trailing_soft_frames >= 40) &&
			           voiced_frames >= 8 &&
			           (!decoded_cmd_is_final || last_voiced_f > decoded_cmd_voiced_f)) {
				/* Speculative full decode starts after 400ms (20 frames) of silence
				 * with a full 20-frame tail so softer final words are included in full.
				 */
				int64_t end_f = last_voiced_f + 20;
				if (end_f > scan_f)
					end_f = scan_f;
				decode_try_submit(cmd_start_f, end_f, last_voiced_f,
				                  lang, eff_prompt, 96, 0);
			}

			/* Endpoint when:
			 * 1. Full silence reaches cmd_silence_ms (640ms = 32 frames), OR
			 * 2. Speculative final decode is already ready and silence >= 24 frames (480ms), OR
			 * 3. Energy has stayed >= 16.0 dB below speech peak for >= 60 frames (1.2s)
			 *    so steady background noise after speech never hangs Jarvis open, OR
			 * 4. Hard cap at 600 frames (12.0s).
			 */
			if ((silence_frames * FRAME_MS) >= cmd_silence_ms ||
			    (decoded_cmd_is_final && live_cmd[0] != '\0' && silence_frames >= 24) ||
			    (voiced_frames >= 8 && trailing_soft_frames >= 60) ||
			    cmd_len_f >= 600) {
				if (voiced_frames < 8 && live_cmd[0] == '\0') {
					if (now_ms() < post_wake_deadline_ms) {
						waiting_post_wake = 1;
						in_speech = 0;
						consec_voiced = 0;
						voiced_frames = 0;
						silence_frames = 0;
						pthread_mutex_lock(&g_audio.mtx);
						g_audio.in_speech_hint = 0;
						pthread_mutex_unlock(&g_audio.mtx);
						continue;
					}
					break;
				}

				char final_raw[4096] = {0};
				if (!(decoded_cmd_is_final && live_cmd[0] != '\0' &&
				      decoded_cmd_voiced_f >= last_voiced_f - 1 &&
				      decoded_cmd_end_f >= last_voiced_f + 20)) {
					int64_t end_f = last_voiced_f + 26;
					decode_force_submit_final(cmd_start_f, end_f, last_voiced_f,
					                          lang, eff_prompt, 96);

					pthread_mutex_lock(&g_dec.mtx);
					while ((g_dec.busy || g_dec.has_job) && running && !g_cancel_req)
						pthread_cond_wait(&g_dec.cond, &g_dec.mtx);
					if (g_dec.has_result) {
						snprintf(final_raw, sizeof(final_raw), "%s", g_dec.result_text);
						g_dec.has_result = 0;
					}
					pthread_mutex_unlock(&g_dec.mtx);
				}

				/* Check if the user resumed speaking to clarify while the final decode
				 * was running! If so, keep listening instead of cutting them off.
				 */
				int resumed_speaking = 0;
				pthread_mutex_lock(&g_audio.mtx);
				int64_t cur_tf = g_audio.total_frames;
				double chk_keep_db = calc_vad_keep_db(utter_amb_avg, utter_amb_std,
				                                      utter_start_db, peak_speech_db);
				int v_after = 0;
				double resume_db = (peak_speech_db - 9.5 > chk_keep_db) ? (peak_speech_db - 9.5) : chk_keep_db;
				for (int64_t f = scan_f; f < cur_tf; f++) {
					if (g_audio.db[f % RING_FRAMES] >= resume_db)
						v_after++;
				}
				if (v_after >= 3 && cmd_len_f < 600)
					resumed_speaking = 1;
				pthread_mutex_unlock(&g_audio.mtx);

				if (final_raw[0] != '\0') {
					char stripped[2048];
					char active_tts[2048] = {0};
					int tts_on = is_tts_currently_speaking(active_tts, sizeof(active_tts));
					if (!tts_on && tts_was_active_in_utter && g_last_tts_cache[0]) {
						snprintf(active_tts, sizeof(active_tts), "%s", g_last_tts_cache);
					}
					int wm = match_wake_prefix(final_raw, wake_word,
					                           stripped, sizeof(stripped));
					if (wm == 0 && !chime_played)
						wm = match_wake_anywhere(final_raw, wake_word,
						                         stripped, sizeof(stripped));
					if (wm == 2 && stripped[0]) {
						if (tts_on)
							interrupt_tts_and_clear_cache();
						snprintf(live_cmd, sizeof(live_cmd), "%s", stripped);
					} else if (wm == 1 || (!chime_played && live_cmd[0] == '\0' && voiced_frames <= 32 &&
					                       contains_wake_word(final_raw, wake_word))) {
						/* User spoke ONLY wake word and paused: interrupt TTS, play chime, duck speaker, and wait for command */
						interrupt_tts_and_clear_cache();
						tts_was_active_in_utter = 0;
						if (!chime_played)
							play_chime_async(chime_wav);
						duck_speaker_for_command();
						chime_played = 1;
						waiting_post_wake = 1;
						in_speech = 0;
						consec_voiced = 0;
						voiced_frames = 0;
						silence_frames = 0;
						live_cmd[0] = '\0';
						decoded_cmd_is_final = 0;
						decoded_cmd_end_f = -1;
						pthread_mutex_lock(&g_audio.mtx);
						g_audio.in_speech_hint = 0;
						chime_ignore_until_f = g_audio.total_frames + 5;
						wake_end_f = g_audio.total_frames + 2;
						pthread_mutex_unlock(&g_audio.mtx);
						post_wake_deadline_ms = now_ms() + 6000L;
						write_state_file(state_file, "jarvis-cmd", NULL, NULL);
						notify_osd_async(jarvis_ontology_get_ui_string(get_daemon_ont(), "listening_title", "  Jarvis Listening..."),
						                 jarvis_ontology_get_ui_string(get_daemon_ont(), "listening_body", "Speak your command now."), 4500);
						continue;
					} else if (wm == 0) {
						if (tts_on || (tts_was_active_in_utter && active_tts[0])) {
							if (is_bargein_stop_phrase(final_raw) ||
							    (is_followup_window && !is_tts_self_echo(final_raw, active_tts))) {
								if (tts_on)
									interrupt_tts_and_clear_cache();
								snprintf(live_cmd, sizeof(live_cmd), "%s", final_raw);
							} else {
								live_cmd[0] = '\0';
							}
						} else {
							snprintf(live_cmd, sizeof(live_cmd), "%s", final_raw);
						}
					} else {
						live_cmd[0] = '\0';
					}
				}
				if (resumed_speaking) {
					silence_frames = 0;
					decoded_cmd_is_final = 0;
					if (live_cmd[0] != '\0') {
						write_state_file(state_file, "cmd", live_cmd,
						                 jarvis_ontology_get_ui_string(get_daemon_ont(), "command_live", "  Jarvis Command (Live)..."));
					}
					continue;
				}
				if (live_cmd[0] == '\0' && now_ms() < post_wake_deadline_ms) {
					waiting_post_wake = 1;
					in_speech = 0;
					consec_voiced = 0;
					voiced_frames = 0;
					silence_frames = 0;
					decoded_cmd_is_final = 0;
					tts_was_active_in_utter = is_tts_currently_speaking(NULL, 0) ? 1 : 0;
					pthread_mutex_lock(&g_audio.mtx);
					g_audio.in_speech_hint = 0;
					pthread_mutex_unlock(&g_audio.mtx);
					continue;
				}
				break;
			}
		}
	}

	if (g_cancel_req) {
		restore_speaker_after_command(NULL);
		decode_reset_session();
		audio_release(g_preempt_cli >= 0 ? WARM_HOLD_MS : 2000L);
		free(wake_pcm);
		if (cli >= 0)
			write_all(cli, "CANCELLED\n", 10);
		return 130;
	}

	pthread_mutex_lock(&g_audio.mtx);
	g_audio.last_cmd_end_f = g_audio.total_frames;
	pthread_mutex_unlock(&g_audio.mtx);

	restore_speaker_after_command(live_cmd);
	audio_release(WARM_HOLD_MS);
	free(wake_pcm);

	if (live_cmd[0] != '\0') {
		if (out_cmd && out_sz > 0)
			snprintf(out_cmd, out_sz, "%s", live_cmd);
		if (cli >= 0) {
			char reply[4200];
			snprintf(reply, sizeof(reply), "CMD|%s\n", live_cmd);
			write_all(cli, reply, strlen(reply));
		}
		notify_osd_async(jarvis_ontology_get_ui_string(get_daemon_ont(), "command_title", "\xef\x95\x84  Jarvis Command"),
		                 live_cmd, 6000);
		return 0;
	} else {
		write_state_file(state_file, "jarvis", NULL, NULL);
		if (cli >= 0)
			write_all(cli, "EMPTY\n", 6);
		return 2;
	}
}

static int
is_speaker_playing_c(void)
{
	FILE *fp = popen("pactl list sink-inputs 2>/dev/null", "r");
	if (!fp)
		return 0;
	char line[512];
	int in_block = 0;
	int corked_no = 0;
	int is_jarvis_tts = 0;
	int external_playing = 0;

	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, "Sink Input #", 12) == 0) {
			if (in_block && corked_no && !is_jarvis_tts) {
				external_playing = 1;
				break;
			}
			in_block = 1;
			corked_no = 0;
			is_jarvis_tts = 0;
			continue;
		}
		if (!in_block)
			continue;
		if (strstr(line, "Corked: no") != NULL)
			corked_no = 1;
		if (strstr(line, "jarvis-tts") != NULL)
			is_jarvis_tts = 1;
	}
	if (!external_playing && in_block && corked_no && !is_jarvis_tts)
		external_playing = 1;
	pclose(fp);
	return external_playing;
}

static void
read_jarvis_runtime_cfg(char *lang, size_t lang_sz,
                        int *out_followup_ms,
                        int *out_conv_followup_ms)
{
	const char *ont_lang = jarvis_ontology_get_lang(get_daemon_ont());
	const char *def_lang = (ont_lang && *ont_lang) ? ont_lang : "pt";

	jarvis_rc_get("lang", def_lang, lang, lang_sz);
	*out_followup_ms = jarvis_rc_get_int("followup_ms", 0);
	if (*out_followup_ms < 0 || *out_followup_ms > 3600000)
		*out_followup_ms = 0;
	*out_conv_followup_ms = jarvis_rc_get_int("conv_followup_ms", 9000);
	if (*out_conv_followup_ms < 0 || *out_conv_followup_ms > 3600000)
		*out_conv_followup_ms = 9000;
}

static void
load_xy_mod_name(const char *home, const char *mod)
{
	if (!mod || !*mod)
		return;
	char clean[512];
	if (mod[0] == '/' || mod[0] == '.') {
		snprintf(clean, sizeof(clean), "%s", mod);
	} else {
		snprintf(clean, sizeof(clean), "%s/.local/lib/%s", home, mod);
	}
	size_t l = strlen(clean);
	if (l > 3 && strcmp(clean + l - 3, ".so") == 0)
		clean[l - 3] = '\0';
	if (xy_load(clean) != XY_OK && mod[0] != '/') {
		snprintf(clean, sizeof(clean), "%s", mod);
		l = strlen(clean);
		if (l > 3 && strcmp(clean + l - 3, ".so") == 0)
			clean[l - 3] = '\0';
		xy_load(clean);
	}
}

static void
init_jarvis_xylem_runtime(void)
{
	const char *home = getenv("HOME");
	if (!home || !*home)
		home = "/home/quirinpa";

	xy_init();

	static char wm_buf[64];
	jarvis_rc_get("wm", "dwm", wm_buf, sizeof(wm_buf));

	char mods_cfg[512];
	if (jarvis_rc_get("mods", NULL, mods_cfg, sizeof(mods_cfg)) && mods_cfg[0]) {
		char *save = NULL;
		for (char *tok = strtok_r(mods_cfg, ":, ", &save); tok; tok = strtok_r(NULL, ":, ", &save)) {
			if (*tok)
				load_xy_mod_name(home, tok);
		}
	} else {
		char wm_mod[128];
		snprintf(wm_mod, sizeof(wm_mod), "mod_%s", wm_buf[0] ? wm_buf : "dwm");
		load_xy_mod_name(home, wm_mod);
		load_xy_mod_name(home, "mod_tts");
	}

	char cfg_path[512];
	snprintf(cfg_path, sizeof(cfg_path), "%s/.config/jarvis/mods.load", home);
	FILE *mfp = fopen(cfg_path, "r");
	if (mfp) {
		char mline[512];
		while (fgets(mline, sizeof(mline), mfp)) {
			trim_inplace(mline);
			if (mline[0] && mline[0] != '#')
				load_xy_mod_name(home, mline);
		}
		fclose(mfp);
	}

	const char *u = get_username();
	char chime_wav[256], done_wav[256];
	snprintf(chime_wav, sizeof(chime_wav), "/tmp/jarvis-chime-%s.wav", u);
	snprintf(done_wav, sizeof(done_wav), "/tmp/jarvis-done-%s.wav", u);
	if (access(chime_wav, R_OK) != 0)
		jarvis_voice_write_earcon_wav(chime_wav, 0);
	if (access(done_wav, R_OK) != 0)
		jarvis_voice_write_earcon_wav(done_wav, 1);

	static char ont_path[512], int_path[512], ctx_path[512], sess_path[512], mem_path[512], emb_path[512], auren_url[256];
	char lang[32];
	int fms = 0, conv_fms = 9000;
	read_jarvis_runtime_cfg(lang, sizeof(lang), &fms, &conv_fms);

	char def_ont[512], def_int[512], def_mem[512], def_emb[512], def_sess[512];
	snprintf(def_ont, sizeof(def_ont), "%s/.config/jarvis/ontology.json", home);
	snprintf(def_int, sizeof(def_int), "%s/.config/jarvis/few_shots.json", home);
	snprintf(def_mem, sizeof(def_mem), "%s/.config/jarvis/memories.json", home);
	snprintf(def_emb, sizeof(def_emb), "%s/.cache/jarvis/embeddings.json", home);
	snprintf(def_sess, sizeof(def_sess), "%s/.cache/jarvis/session.json", home);

	jarvis_rc_get("ontology_path", def_ont, ont_path, sizeof(ont_path));
	jarvis_rc_get("few_shot_path", def_int, int_path, sizeof(int_path));
	snprintf(ctx_path, sizeof(ctx_path), "/tmp/jarvis-%s.context.json", u);
	jarvis_rc_get("session_path", def_sess, sess_path, sizeof(sess_path));
	jarvis_rc_get("memory_path", def_mem, mem_path, sizeof(mem_path));
	jarvis_rc_get("embed_cache_path", def_emb, emb_path, sizeof(emb_path));
	jarvis_rc_get("auren_url", "http://127.0.0.1:4242", auren_url, sizeof(auren_url));

	jarvis_config_t jcfg;
	jarvis_config_init(&jcfg);
	jcfg.wm = wm_buf[0] ? wm_buf : "dwm";
	jcfg.ontology_path = ont_path;
	if (access(int_path, R_OK) == 0)
		jcfg.few_shot_path = int_path;
	jcfg.context_path = ctx_path;
	jcfg.session_path = sess_path;
	jcfg.memory_path = mem_path;
	jcfg.embed_cache_path = emb_path;
	jcfg.auren_url = auren_url;
	char mode[64];
	jarvis_rc_get("infer_mode", "auto", mode, sizeof(mode));
	jcfg.llm_model = "fast";
	if (strcmp(mode, "off") == 0 || strcmp(mode, "none") == 0)
		jcfg.infer_mode = JARVIS_INFER_NONE;
	else if (strcmp(mode, "embedding") == 0)
		jcfg.infer_mode = JARVIS_INFER_EMBEDDING;
	else if (strcmp(mode, "smart") == 0) {
		jcfg.infer_mode = JARVIS_INFER_LLM;
		jcfg.llm_model = "smart";
	} else if (strcmp(mode, "kilo") == 0) {
		jcfg.infer_mode = JARVIS_INFER_LLM;
		jcfg.llm_model = "kilo-auto/free";
	} else if (strcmp(mode, "cloud") == 0 || strcmp(mode, "remote") == 0 ||
	           strcmp(mode, "llm") == 0 || strcmp(mode, "fast") == 0)
		jcfg.infer_mode = JARVIS_INFER_LLM;
	else
		jcfg.infer_mode = JARVIS_INFER_AUTO;

	g_jarvis_eng = jarvis_engine_create(&jcfg);
}

static void
format_natural_voice_reply(const jarvis_process_result_t *res, char *out, size_t out_sz)
{
	jarvis_format_voice_reply(get_daemon_ont(), res, out, out_sz);
}

static bool
dispatch_to_ren_tmux(const char *utterance, int *out_started_media, int *out_conv_exited)
{
	const char *u = get_username();
	char sock_path[96];
	snprintf(sock_path, sizeof(sock_path), "/tmp/ren-jarvis-%.48s.sock", u);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return false;

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		/* Auto-start detached tmux ren session via dwm-jarvis ensure */
		int sys_rc = system("dwm-jarvis ensure >/dev/null 2>&1");
		(void)sys_rc;
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0)
			return false;
		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			close(fd);
			return false;
		}
	}

	char req[4096];
	int req_len = snprintf(req, sizeof(req), "%s\n", utterance);
	if (req_len <= 0) {
		close(fd);
		return false;
	}
	write_all(fd, req, (size_t)req_len);

	FILE *fp = fdopen(fd, "r");
	if (!fp) {
		close(fd);
		return false;
	}

	bool any_event = false;
	bool ok = true;
	char line[4096];
	while (fgets(line, sizeof(line), fp)) {
		size_t l = strlen(line);
		while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
			line[--l] = '\0';
		if (l == 0)
			continue;

		char *tab1 = strchr(line, '\t');
		if (!tab1)
			continue;
		*tab1 = '\0';
		const char *ev = line;
		char *arg1 = tab1 + 1;
		char *tab2 = strchr(arg1, '\t');
		const char *arg2 = "";
		if (tab2) {
			*tab2 = '\0';
			arg2 = tab2 + 1;
		}

		any_event = true;
		if (strcmp(ev, "TIER0") == 0) {
			if (strcmp(arg1, "media_play") == 0 && out_started_media)
				*out_started_media = 1;
			if (strcmp(arg1, "conversation_control") == 0 && out_conv_exited)
				*out_conv_exited = 1;
			char title[256], body[1024];
			snprintf(title, sizeof(title), "  Jarvis → %s (tier-0)", arg1[0] ? arg1 : "action");
			snprintf(body, sizeof(body), "“%.350s”\n▸ %.550s", utterance, arg2);
			jarvis_xy_notify(title, body, 1);
		} else if (strcmp(ev, "TOOL") == 0) {
			if (strcmp(arg1, "media") == 0 && strncasecmp(arg2, "play", 4) == 0 && out_started_media)
				*out_started_media = 1;
			char title[256], body[1024];
			snprintf(title, sizeof(title), "  ren → %s", arg1);
			snprintf(body, sizeof(body), "▸ %.550s", arg2);
			jarvis_xy_notify(title, body, 0);
		} else if (strcmp(ev, "CLAUSE") == 0) {
			if (arg1[0]) {
				int is_final = (strcmp(arg2, "1") == 0) ? 1 : 0;
				jarvis_xy_speak_clause("ren", arg1, is_final);
			}
		} else if (strcmp(ev, "DONE") == 0) {
			ok = (strcmp(arg1, "1") == 0);
			break;
		}
	}

	fclose(fp);
	return any_event && ok;
}

static void
run_inprocess_jarvis_turn(int srv, struct whisper_context *ctx_wake, int n_threads,
                          int *io_next_followup)
{
	const char *u = get_username();
	char state_file[256], chime_wav[256], done_wav[256], last_txt[256], acting_pid[256];
	snprintf(state_file, sizeof(state_file), "/tmp/jarvis-voice-%s.state", u);
	snprintf(chime_wav, sizeof(chime_wav), "/tmp/jarvis-chime-%s.wav", u);
	snprintf(done_wav, sizeof(done_wav), "/tmp/jarvis-done-%s.wav", u);
	snprintf(last_txt, sizeof(last_txt), "/tmp/jarvis-voice-%s.last.txt", u);
	snprintf(acting_pid, sizeof(acting_pid), "/tmp/jarvis-acting-%s.pid", u);

	char lang[32], wm_surface[64];
	int fcfg = 0, conv_followup_ms = 9000;
	read_jarvis_runtime_cfg(lang, sizeof(lang), &fcfg, &conv_followup_ms);
	jarvis_rc_get("wm", "dwm", wm_surface, sizeof(wm_surface));

	static char s_pending_frag[512] = {0};
	static long s_pending_ms = 0;

	int next_followup = *io_next_followup;
	if (fcfg == 3600000) {
		next_followup = is_speaker_playing_c() ? 0 : 3600000;
	}

	write_state_file(state_file, (next_followup > 0) ? "jarvis-cmd" : "jarvis", NULL, NULL);

	char wake_buf[64];
	const char *ont_wake = jarvis_ontology_get_wake_word(get_daemon_ont());
	if (!ont_wake || !*ont_wake)
		ont_wake = "jarvis";
	jarvis_rc_get("wake_word", ont_wake, wake_buf, sizeof(wake_buf));

	char cmd_text[4096] = {0};
	int rc = handle_jarvis_wait(srv, -1, ctx_wake, n_threads,
	                            lang, -35.0, 460, wake_buf,
	                            state_file, chime_wav, next_followup,
	                            cmd_text, sizeof(cmd_text));
	int was_followup = next_followup;
	*io_next_followup = 0;

	if (!g_jarvis_active) {
		unlink(state_file);
		notify_bar();
		return;
	}

	if (rc == 130)
		return;

	if (rc == 0 && cmd_text[0] != '\0') {
		FILE *lfp = fopen(last_txt, "w");
		if (lfp) {
			fprintf(lfp, "%s", cmd_text);
			fclose(lfp);
		}

		if (jarvis_voice_is_lockdown_phrase(cmd_text)) {
			jarvis_set_active(0);
			char clean_ld[256];
			jarvis_clean_phrase(cmd_text, clean_ld, sizeof(clean_ld));
			int want_screen_lock = (strstr(clean_ld, "lock") != NULL ||
			                        strstr(clean_ld, "bloque") != NULL);
			if (want_screen_lock) {
				jarvis_xy_notify(jarvis_ontology_get_ui_string(get_daemon_ont(), "lockdown_title", "  Jarbas Bloqueio"),
				                 jarvis_ontology_get_ui_string(get_daemon_ont(), "lockdown_body", "Escuta contínua desativada. A bloquear o ecrã."), 0);
				char lock_cmd[256];
				jarvis_rc_get("lock_cmd", "slock", lock_cmd, sizeof(lock_cmd));
				if (lock_cmd[0] && fork() == 0) {
					setsid();
					execl("/bin/sh", "sh", "-c", lock_cmd, (char *)NULL);
					_exit(0);
				}
			} else {
				jarvis_xy_notify("  Jarbas Desligado", "Escuta contínua desativada.", 0);
			}
			return;
		}

		FILE *afp = fopen(acting_pid, "w");
		if (afp) {
			fprintf(afp, "%d\n", (int)getpid());
			fclose(afp);
		}
		notify_bar();

		interrupt_tts_and_clear_cache();

		int act_rc = -1;
		int started_media = 0;
		int conv_exited = 0;
		pthread_mutex_lock(&g_jarvis_mtx);
		if (dispatch_to_ren_tmux(cmd_text, &started_media, &conv_exited)) {
			s_pending_frag[0] = '\0';
			act_rc = 0;
		} else if (g_jarvis_eng) {
			char eff_cmd[4096];
			snprintf(eff_cmd, sizeof(eff_cmd), "%s", cmd_text);
			if (s_pending_frag[0] != '\0' && (now_ms() - s_pending_ms) <= 12000L) {
				snprintf(eff_cmd, sizeof(eff_cmd), "%.500s %.3500s", s_pending_frag, cmd_text);
			}
			jarvis_process_result_t res;
			memset(&res, 0, sizeof(res));
			bool ok = jarvis_process(g_jarvis_eng, eff_cmd, wm_surface, &res);
			if (!ok && strcmp(eff_cmd, cmd_text) != 0) {
				memset(&res, 0, sizeof(res));
				snprintf(eff_cmd, sizeof(eff_cmd), "%s", cmd_text);
				ok = jarvis_process(g_jarvis_eng, eff_cmd, wm_surface, &res);
			}
			if (ok) {
				s_pending_frag[0] = '\0';
				act_rc = res.combined_result.success ? 0 : 1;
				if (strcmp(res.last_dsl.op, "media_play") == 0)
					started_media = 1;
				if (strcmp(res.last_dsl.op, "conversation_control") == 0 &&
				    (strcasecmp(res.last_dsl.args.action, "exit") == 0 ||
				     strcasecmp(res.last_dsl.args.action, "reset") == 0 ||
				     strcasecmp(res.last_dsl.args.action, "off") == 0 ||
				     strcasecmp(res.last_dsl.args.action, "stop") == 0 ||
				     strcasecmp(res.last_dsl.args.action, "clear") == 0))
					conv_exited = 1;
				const char *out_msg = res.combined_result.output[0]
				                    ? res.combined_result.output
				                    : res.combined_result.error;
				if (strcmp(res.last_dsl.op, "answer_question") != 0) {
					char title[256], body[1024];
					snprintf(title, sizeof(title), "  Jarvis → %s (%s)",
					         res.last_dsl.op[0] ? res.last_dsl.op : "action",
					         res.source[0] ? res.source : "solver");
					snprintf(body, sizeof(body), "“%.350s”\n▸ %.550s", eff_cmd, out_msg);
					jarvis_xy_notify(title, body, res.combined_result.success ? 1 : 0);
					if (!res.last_dsl.args.reply[0] &&
					    strcmp(res.last_dsl.op, "tts_interrupt") != 0 &&
					    strcmp(res.last_dsl.op, "media_play") != 0 &&
					    out_msg && out_msg[0]) {
						char spoken[512];
						format_natural_voice_reply(&res, spoken, sizeof(spoken));
						if (spoken[0])
							jarvis_xy_speak_clause("oneshot", spoken, 1);
					}
				}
			} else {
				/* If an un-waked capture during a follow-up window could not be
				 * resolved as a valid command, treat it as ambient room/phone chatter
				 * and return cleanly to standby without speaking "Pardon?" or polluting
				 * the next utterance with s_pending_frag.
				 */
				s_pending_frag[0] = '\0';
				act_rc = 1;
				if (was_followup <= 0) {
					char body[512];
					const char *unrec_lbl = jarvis_ontology_get_ui_string(get_daemon_ont(), "unrecognized_body", "unrecognized command");
					snprintf(body, sizeof(body), "“%.420s” (%s)", cmd_text, unrec_lbl);
					jarvis_xy_notify(jarvis_ontology_get_ui_string(get_daemon_ont(), "unrecognized_title", "  Jarvis — Unrecognized"), body, 0);
				}
			}
		}
		pthread_mutex_unlock(&g_jarvis_mtx);

		unlink(acting_pid);
		if (g_jarvis_active) {
			int eff_followup = (fcfg > 0) ? fcfg : conv_followup_ms;
			if ((act_rc == 0 || act_rc == -1) && !started_media && !conv_exited && !is_speaker_playing_c())
				*io_next_followup = eff_followup;
			write_state_file(state_file, (*io_next_followup > 0) ? "jarvis-cmd" : "jarvis", NULL, NULL);
		}
	} else if (rc == 2) {
		write_state_file(state_file, "jarvis", NULL, NULL);
		if (was_followup <= 0)
			jarvis_xy_notify(jarvis_ontology_get_ui_string(get_daemon_ont(), "standby_title", "  Jarvis Standby"),
			                 jarvis_ontology_get_ui_string(get_daemon_ont(), "standby_body", "No command spoken."), 0);
	}
}

static void
send_control_msg(int port, const char *msg)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return;
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0)
		write_all(s, msg, strlen(msg));
	close(s);
}

static int
run_client_mode(int port, const char *req)
{
	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_cli_sigint;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = on_cli_sigusr1;
	sigaction(SIGUSR1, &sa, NULL);

	int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s < 0)
		return 3;
	struct timeval tv_to = { .tv_sec = 4, .tv_usec = 0 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv_to, sizeof(tv_to));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv_to, sizeof(tv_to));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(s);
		return 3;
	}

	char send_buf[2048];
	snprintf(send_buf, sizeof(send_buf), "%s\n", req);
	write_all(s, send_buf, strlen(send_buf));

	int sent_stop = 0;
	char resp[4096];
	size_t total = 0;

	while (total < sizeof(resp) - 1) {
		if (cli_cancel_req) {
			send_control_msg(port, "CANCEL\n");
			close(s);
			return 130;
		}
		if (cli_stop_req && !sent_stop) {
			send_control_msg(port, "STOP\n");
			sent_stop = 1;
		}

		struct pollfd pfd;
		pfd.fd = s;
		pfd.events = POLLIN;
		pfd.revents = 0;
		int pr = poll(&pfd, 1, 40);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pr == 0)
			continue;
		if (pfd.revents & (POLLIN | POLLHUP)) {
			ssize_t n = read(s, resp + total, sizeof(resp) - 1 - total);
			if (n <= 0)
				break;
			total += (size_t)n;
			if (memchr(resp, '\n', total))
				break;
		}
	}
	close(s);
	resp[total] = '\0';
	trim_inplace(resp);

	if (strcmp(resp, "CANCELLED") == 0)
		return 130;
	if (strcmp(resp, "EMPTY") == 0 || strcmp(resp, "TIMEOUT") == 0 || resp[0] == '\0')
		return 2;
	if (strncmp(resp, "OK|", 3) == 0) {
		printf("%s\n", resp + 3);
		return 0;
	}
	if (strncmp(resp, "CMD|", 4) == 0) {
		printf("%s\n", resp + 4);
		return 0;
	}
	printf("%s\n", resp);
	return 0;
}

static void
serve_client_request(int srv, int cli, char *buf,
                     struct whisper_context *ctx_main,
                     struct whisper_context *ctx_wake, int n_threads)
{
	if (strncmp(buf, "GET /", 5) == 0 || strncmp(buf, "PING", 4) == 0) {
		const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
		write_all(cli, ok, strlen(ok));
		close(cli);
		return;
	}
	if (strncmp(buf, "STATUS", 6) == 0) {
		char sbuf[256];
		pthread_mutex_lock(&g_audio.mtx);
		int64_t tf = g_audio.total_frames;
		double ldb = (tf > 0) ? g_audio.db[(tf - 1) % RING_FRAMES] : -100.0;
		snprintf(sbuf, sizeof(sbuf),
		         "STATUS|frames=%lld|nf_db=%.1f|std_db=%.1f|last_db=%.1f|pw_open=%d|in_speech=%d|jarvis=%d\n",
		         (long long)tf, g_audio.noise_floor_db, g_audio.ambient_std_db,
		         ldb, g_audio.pw_open, g_audio.in_speech_hint, (int)g_jarvis_active);
		pthread_mutex_unlock(&g_audio.mtx);
		write_all(cli, sbuf, strlen(sbuf));
		close(cli);
		return;
	}
	if (strncmp(buf, "JARVIS_STATUS", 13) == 0) {
		const char *st = g_jarvis_active ? "ON\n" : "OFF\n";
		write_all(cli, st, strlen(st));
		close(cli);
		return;
	}
	if (strncmp(buf, "JARVIS_ON", 9) == 0) {
		jarvis_set_active(1);
		write_all(cli, "ON\n", 3);
		close(cli);
		return;
	}
	if (strncmp(buf, "JARVIS_OFF", 10) == 0) {
		jarvis_set_active(0);
		write_all(cli, "OFF\n", 4);
		close(cli);
		return;
	}
	if (strncmp(buf, "JARVIS_TOGGLE", 13) == 0) {
		int next = !g_jarvis_active;
		jarvis_set_active(next);
		const char *st = next ? "ON\n" : "OFF\n";
		write_all(cli, st, strlen(st));
		close(cli);
		return;
	}
	if (strncmp(buf, "TTS_INTERRUPT", 13) == 0) {
		interrupt_tts_and_clear_cache();
		write_all(cli, "OK\n", 3);
		close(cli);
		return;
	}
	if (strncmp(buf, "TTS_SPEAK|", 10) == 0) {
		char *txt = buf + 10;
		char *nl = strpbrk(txt, "\r\n");
		if (nl)
			*nl = '\0';
		if (*txt) {
			interrupt_tts_and_clear_cache();
			jarvis_xy_speak_clause("oneshot", txt, 1);
		}
		write_all(cli, "OK\n", 3);
		close(cli);
		return;
	}
	if (strncmp(buf, "TTS_NOTIFY|", 11) == 0) {
		char *rest = buf + 11;
		char *nl = strpbrk(rest, "\r\n");
		if (nl)
			*nl = '\0';
		char *sep = strchr(rest, '|');
		const char *title = "Jarbas";
		const char *msg = rest;
		char title_buf[128] = {0};
		if (sep) {
			size_t tlen = (size_t)(sep - rest);
			if (tlen > 0 && tlen < sizeof(title_buf)) {
				memcpy(title_buf, rest, tlen);
				title_buf[tlen] = '\0';
				title = title_buf;
			}
			msg = sep + 1;
		}
		if (*msg) {
			notify_osd_async(title, msg, 5500);
			if (!is_tts_currently_speaking(NULL, 0))
				jarvis_xy_speak_clause("notify", msg, 2);
		}
		write_all(cli, "OK\n", 3);
		close(cli);
		return;
	}
	if (strncmp(buf, "STOP", 4) == 0) {
		g_stop_req = 1;
		pthread_cond_broadcast(&g_audio.cond);
		write_all(cli, "OK\n", 3);
		close(cli);
		return;
	}
	if (strncmp(buf, "CANCEL", 6) == 0) {
		g_cancel_req = 1;
		g_dec.abort_flag = 1;
		pthread_cond_broadcast(&g_audio.cond);
		write_all(cli, "OK\n", 3);
		close(cli);
		return;
	}

	char *nl = strpbrk(buf, "\r\n");
	if (nl)
		*nl = '\0';

	char *save = NULL;
	char *cmd = strtok_r(buf, "|", &save);
	if (!cmd) {
		close(cli);
		return;
	}

	if (strcmp(cmd, "STREAM") == 0) {
		char *mode       = strtok_r(NULL, "|", &save);
		char *lang       = strtok_r(NULL, "|", &save);
		char *thresh_str = strtok_r(NULL, "|", &save);
		char *sil_str    = strtok_r(NULL, "|", &save);
		char *wait_str   = strtok_r(NULL, "|", &save);
		char *state_file = strtok_r(NULL, "|", &save);

		if (mode && lang && thresh_str && sil_str && wait_str) {
			handle_stream(srv, cli, mode, lang,
			              strtod(thresh_str, NULL), atoi(sil_str),
			              atoi(wait_str), state_file ? state_file : "-");
		}
		close(cli);
		return;
	}

	if (strcmp(cmd, "JARVIS_WAIT") == 0) {
		char *lang         = strtok_r(NULL, "|", &save);
		char *thresh_str   = strtok_r(NULL, "|", &save);
		char *sil_str      = strtok_r(NULL, "|", &save);
		char *wake_word    = strtok_r(NULL, "|", &save);
		char *state_file   = strtok_r(NULL, "|", &save);
		char *chime_wav    = strtok_r(NULL, "|", &save);
		char *followup_str = strtok_r(NULL, "|", &save);

		if (lang && thresh_str && sil_str && wake_word) {
			handle_jarvis_wait(srv, cli, ctx_wake, n_threads,
			                   lang, strtod(thresh_str, NULL), atoi(sil_str),
			                   wake_word, state_file ? state_file : "-",
			                   chime_wav ? chime_wav : "-",
			                   followup_str ? atoi(followup_str) : 0, NULL, 0);
		}
		close(cli);
		return;
	}

	if (strcmp(cmd, "TRANSCRIBE") == 0) {
		char *mode     = strtok_r(NULL, "|", &save);
		char *lang     = strtok_r(NULL, "|", &save);
		char *mt_str   = strtok_r(NULL, "|", &save);
		char *prompt   = strtok_r(NULL, "|", &save);
		char *wav_path = strtok_r(NULL, "|", &save);

		if (!mode || !lang || !mt_str || !prompt || !wav_path) {
			close(cli);
			return;
		}

		int is_wake = (strcmp(mode, "wake") == 0);
		int is_cmd  = (strcmp(mode, "cmd") == 0);
		int max_tok = atoi(mt_str);
		if (max_tok <= 0)
			max_tok = is_wake ? 8 : (is_cmd ? 24 : 96);

		float *pcm = NULL;
		int n_samples = 0;
		if (load_wav_f32(wav_path, &pcm, &n_samples) != 0 || n_samples < 1600) {
			free(pcm);
			close(cli);
			return;
		}

		decode_reset_session();
		struct whisper_context *ctx = is_wake ? ctx_wake : ctx_main;
		char out_text[4096];
		run_whisper_slice(ctx, n_threads, pcm, n_samples, lang, prompt,
		                  max_tok, is_wake, 0, 0, out_text, sizeof(out_text), NULL);
		strncat(out_text, "\n", sizeof(out_text) - strlen(out_text) - 1);
		write_all(cli, out_text, strlen(out_text));
		close(cli);
		free(pcm);
		return;
	}

	close(cli);
}

static void
resolve_default_whisper_models(char *main_out, size_t main_sz, char *wake_out, size_t wake_sz)
{
	const char *home = getenv("HOME");
	if (!home || !*home)
		home = "/home/quirinpa";

	if (!jarvis_rc_get("whisper_model", NULL, main_out, main_sz) || !main_out[0] || access(main_out, R_OK) != 0) {
		static const char *const main_cands[] = {
			"ggml-small-q8_0.bin",
			"ggml-small-q5_1.bin",
			"ggml-base-q8_0.bin",
			"ggml-base-q5_1.bin",
			"ggml-base.bin",
			"ggml-base.en-q8_0.bin",
			"ggml-small.en-q5_1.bin",
			NULL
		};
		main_out[0] = '\0';
		for (size_t i = 0; main_cands[i]; i++) {
			char cand[512];
			snprintf(cand, sizeof(cand), "%s/.local/share/whisper/%s", home, main_cands[i]);
			if (access(cand, R_OK) == 0) {
				snprintf(main_out, main_sz, "%s", cand);
				break;
			}
		}
	}

	if (!jarvis_rc_get("whisper_wake_model", NULL, wake_out, wake_sz) || !wake_out[0] || access(wake_out, R_OK) != 0) {
		static const char *const wake_cands[] = {
			"ggml-base-q8_0.bin",
			"ggml-base-q5_1.bin",
			"ggml-base.bin",
			NULL
		};
		wake_out[0] = '\0';
		for (size_t i = 0; wake_cands[i]; i++) {
			char cand[512];
			snprintf(cand, sizeof(cand), "%s/.local/share/whisper/%s", home, wake_cands[i]);
			if (access(cand, R_OK) == 0) {
				snprintf(wake_out, wake_sz, "%s", cand);
				break;
			}
		}
		if (!wake_out[0])
			snprintf(wake_out, wake_sz, "%s", main_out);
	}
}

int
main(int argc, char *argv[])
{
	const char *main_model_path = NULL;
	const char *wake_model_path = NULL;
	const char *client_req      = NULL;
	{
		const char *home = getenv("HOME");
		const char *cur_path = getenv("PATH");
		if (home && *home) {
			char local_bin[512];
			snprintf(local_bin, sizeof(local_bin), "%s/.local/bin", home);
			if (!cur_path || !*cur_path) {
				setenv("PATH", local_bin, 1);
			} else if (!strstr(cur_path, local_bin)) {
				char new_path[4096];
				snprintf(new_path, sizeof(new_path), "%s:%s", local_bin, cur_path);
				setenv("PATH", new_path, 1);
			}
		}
	}
	int port = jarvis_rc_get_int("port", 8179);
	int n_threads = jarvis_rc_get_int("threads", 6);
	int start_jarvis = 0;
	int opt;

	while ((opt = getopt(argc, argv, "m:w:p:t:c:jh")) != -1) {
		switch (opt) {
		case 'm': main_model_path = optarg; break;
		case 'w': wake_model_path = optarg; break;
		case 'p': port = atoi(optarg); break;
		case 't': n_threads = atoi(optarg); break;
		case 'c': client_req = optarg; break;
		case 'j': start_jarvis = 1; break;
		default:
			fprintf(stderr,
				"Usage:\n"
				"  Daemon: %s [-m <main-model.bin>] [-w <wake-model.bin>] [-p port] [-t threads] [-j]\n"
				"  Client: %s [-p port] -c \"<COMMAND|...>\"\n",
				argv[0], argv[0]);
			return 1;
		}
	}

	if (client_req)
		return run_client_mode(port, client_req);

	static char s_main_model[512], s_wake_model[512];
	resolve_default_whisper_models(s_main_model, sizeof(s_main_model), s_wake_model, sizeof(s_wake_model));
	if (!main_model_path || !*main_model_path)
		main_model_path = s_main_model;
	if (!wake_model_path || !*wake_model_path)
		wake_model_path = s_wake_model;

	if (!main_model_path || !*main_model_path) {
		fprintf(stderr, "jarvisd: no whisper model found in ~/.jarvisrc or ~/.local/share/whisper/\n");
		return 1;
	}

	/* Kernel-level singleton lock so duplicate jarvisd daemons can never start */
	char lock_path[256], pid_path[256];
	snprintf(lock_path, sizeof(lock_path), "/tmp/jarvisd-%s.lock", get_username());
	snprintf(pid_path, sizeof(pid_path), "/tmp/jarvisd-%s.pid", get_username());
	int lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	struct flock fl;
	memset(&fl, 0, sizeof(fl));
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	if (lock_fd < 0 || fcntl(lock_fd, F_SETLK, &fl) < 0) {
		if (start_jarvis)
			send_control_msg(port, "JARVIS_ON\n");
		if (lock_fd >= 0)
			close(lock_fd);
		return 0;
	}
	{
		FILE *pfp = fopen(pid_path, "w");
		if (pfp) {
			fprintf(pfp, "%d\n", (int)getpid());
			fclose(pfp);
		}
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);

	init_jarvis_xylem_runtime();
	if (start_jarvis)
		jarvis_set_active(1);

	ggml_backend_load_all();
	whisper_log_set(quiet_log, NULL);

	struct whisper_context_params cparams = whisper_context_default_params();
	cparams.use_gpu = true;
	cparams.flash_attn = true;

	struct whisper_context *ctx_main = whisper_init_from_file_with_params(main_model_path, cparams);
	if (!ctx_main) {
		fprintf(stderr, "jarvisd: failed to load main model %s\n", main_model_path);
		return 1;
	}

	struct whisper_context *ctx_wake = NULL;
	if (wake_model_path && *wake_model_path && strcmp(wake_model_path, main_model_path) != 0 &&
	    access(wake_model_path, R_OK) == 0) {
		ctx_wake = whisper_init_from_file_with_params(wake_model_path, cparams);
	}
	if (!ctx_wake)
		ctx_wake = ctx_main;

	g_dec.ctx = ctx_main;
	g_dec.n_threads = n_threads;
	g_dec.pcm = malloc((size_t)MAX_UTTER_FRAMES * FRAME_SAMPLES * sizeof(float));
	if (!g_dec.pcm)
		return 1;

	int srv = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (srv < 0)
		return 1;

	int yes = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
	setsockopt(srv, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(srv, 16) < 0) {
		close(srv);
		return 1;
	}

	/* Warmup both contexts (128 frames) so threadpool and arena buffers are hot */
	{
		float warm_pcm[3200] = {0};
		const char *ont_lang = jarvis_ontology_get_lang(get_daemon_ont());
		if (!ont_lang || !*ont_lang)
			ont_lang = "en";
		struct whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
		wp.n_threads = n_threads;
		wp.single_segment = false;
		wp.no_context = true;
		wp.no_timestamps = false;
		wp.max_tokens = 1;
		wp.audio_ctx = 128;
		wp.temperature_inc = 0.0f;
		wp.language = whisper_is_multilingual(ctx_wake) ? ont_lang : "en";
		whisper_full(ctx_wake, wp, warm_pcm, 3200);
		if (ctx_main != ctx_wake) {
			wp.language = whisper_is_multilingual(ctx_main) ? ont_lang : "en";
			whisper_full(ctx_main, wp, warm_pcm, 3200);
		}
	}

	pthread_t audio_tid, decode_tid;
	pthread_create(&audio_tid, NULL, audio_thread_fn, NULL);
	pthread_create(&decode_tid, NULL, decode_worker_fn, NULL);

	int next_followup = 0;

	while (running) {
		int cli = -1;
		char buf[2048];

		if (g_preempt_cli >= 0) {
			cli = g_preempt_cli;
			g_preempt_cli = -1;
			snprintf(buf, sizeof(buf), "%s", g_preempt_buf);
			serve_client_request(srv, cli, buf, ctx_main, ctx_wake, n_threads);
			continue;
		}

		if (g_jarvis_active) {
			run_inprocess_jarvis_turn(srv, ctx_wake, n_threads, &next_followup);
			continue;
		}

		next_followup = 0;
		struct sockaddr_in cli_addr;
		socklen_t cli_len = sizeof(cli_addr);
		cli = accept(srv, (struct sockaddr *)&cli_addr, &cli_len);
		if (cli < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		fcntl(cli, F_SETFD, FD_CLOEXEC);
		ssize_t n = read(cli, buf, sizeof(buf) - 1);
		if (n <= 0) {
			close(cli);
			continue;
		}
		buf[n] = '\0';

		serve_client_request(srv, cli, buf, ctx_main, ctx_wake, n_threads);
	}

	running = 0;
	if (g_jarvis_active)
		jarvis_set_active(0);
	if (g_jarvis_eng) {
		jarvis_engine_destroy(g_jarvis_eng);
		g_jarvis_eng = NULL;
	}
	g_dec.abort_flag = 1;
	pthread_cond_broadcast(&g_audio.cond);
	pthread_cond_broadcast(&g_dec.cond);
	pthread_join(audio_tid, NULL);
	pthread_join(decode_tid, NULL);

	close(srv);
	unlink(pid_path);
	close(lock_fd);
	free(g_dec.pcm);
	whisper_free(ctx_main);
	if (ctx_wake != ctx_main)
		whisper_free(ctx_wake);
	return 0;
}
