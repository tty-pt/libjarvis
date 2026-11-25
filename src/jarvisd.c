/* See LICENSE file for copyright and license details.
 * jarvisd: Unified real-time streaming capture + libwhisper + neural TTS C daemon
 * with persistent circular float32 PCM ring buffer, dedicated non-blocking
 * background decode worker thread with ggml abort_callback, live status-bar
 * and OSD partial transcription, and 0ms speculative silence endpointing.
 */
#include <ttypt/stele.h>
#include "jarvis-thread.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <getopt.h>
#include <process.h>
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#define getpid      _getpid
#else
#include <fcntl.h>
#include <strings.h>
#include <unistd.h>
#endif

#include <ttypt/jarvis.h>
#include <ttypt/desktop-core.h>
#include "jarvis-audio.h"
#include "jarvis-platform.h"
#include "jarvis-ren-ipc.h"
#include "jarvis-tts.h"
#include "jarvis-dsp.h"
#include "jarvis-vad.h"
#include "jarvis-kws.h"
#include "jarvis-stt-backend.h"
#include "jarvis-stt.h"
#include "jarvis-stream.h"
#include "jarvis-dispatch.h"
#include "jarvis-ipc-srv.h"

static int is_tts_audio_hot(void);

static volatile sig_atomic_t g_jarvis_active = 0;
static volatile sig_atomic_t g_in_followup   = 0;
static volatile sig_atomic_t g_in_post_wake  = 0;
static volatile sig_atomic_t g_tts_speaking  = 0;
static volatile sig_atomic_t g_ptt_active    = 0;
static volatile uint64_t     g_wake_stt_calls = 0;
static jarvis_kws_t         *g_kws           = NULL;
static jarvis_ontology_t    *g_daemon_ont    = NULL;
static pthread_mutex_t       g_jarvis_mtx    = PTHREAD_MUTEX_INITIALIZER;
static jarvis_hw_profile_t   g_hw;

static struct {
	pthread_mutex_t mtx;
	int             active;
	int             was_stopped;
	int             act_rc;
	int             started_media;
	int             conv_exited;
	char            cmd_text[4096];
	pthread_t       tid;
} g_op_turn = {
	.mtx         = PTHREAD_MUTEX_INITIALIZER,
	.active      = 0,
	.was_stopped = 0
};

#define SAMPLE_RATE      JARVIS_SAMPLE_RATE
#define FRAME_MS         JARVIS_FRAME_MS
#define FRAME_SAMPLES    JARVIS_FRAME_SAMPLES
#define FRAME_BYTES      JARVIS_FRAME_BYTES
#define RING_FRAMES      JARVIS_RING_FRAMES
#define RING_SAMPLES     JARVIS_RING_SAMPLES
#define MAX_UTTER_FRAMES JARVIS_MAX_UTTER_FRAMES
#define WARM_HOLD_MS     JARVIS_WARM_HOLD_MS

static const jarvis_ontology_t *
get_daemon_ont(void)
{
	return g_daemon_ont;
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
get_cmd_initial_prompt(char *buf, size_t buf_sz)
{
	/* Optional user override in ~/.jarvisrc (e.g. domain jargon or natural
	 * dialect prose — never a comma-separated verb list).
	 */
	if (buf && buf_sz > 1 && jarvis_rc_get("whisper_prompt", "", buf, buf_sz) && buf[0]) {
		if (strcmp(buf, "-") == 0 || strcasecmp(buf, "none") == 0 || strcasecmp(buf, "off") == 0)
			return NULL;
		return buf;
	}
	const char *p = jarvis_ontology_get_cmd_prompt(get_daemon_ont());
	return (p && *p) ? p : NULL;
}

static volatile sig_atomic_t running          = 1;
static volatile sig_atomic_t g_stop_req       = 0;
static volatile sig_atomic_t g_cancel_req     = 0;
static volatile sig_atomic_t cli_stop_req     = 0;
static volatile sig_atomic_t cli_cancel_req   = 0;

static pthread_mutex_t g_state_file_mtx = PTHREAD_MUTEX_INITIALIZER;

static struct {
	pthread_mutex_t mtx;
	pthread_cond_t  cond;
	stl_socket_t    srv;
	stl_socket_t    pending_cli;
	char            pending_buf[2048];
	int             can_preempt_wait;
} g_ctrl = {
	.mtx              = PTHREAD_MUTEX_INITIALIZER,
	.cond             = PTHREAD_COND_INITIALIZER,
	.srv              = STL_INVALID_SOCKET,
	.pending_cli      = STL_INVALID_SOCKET,
	.pending_buf      = {0},
	.can_preempt_wait = 0
};

static void
set_can_preempt_wait(int v)
{
	pthread_mutex_lock(&g_ctrl.mtx);
	g_ctrl.can_preempt_wait = v;
	pthread_mutex_unlock(&g_ctrl.mtx);
}

static int
has_pending_client(void)
{
	pthread_mutex_lock(&g_ctrl.mtx);
	int has = (g_ctrl.pending_cli != STL_INVALID_SOCKET) ? 1 : 0;
	pthread_mutex_unlock(&g_ctrl.mtx);
	return has;
}

static void
on_sig(int sig)
{
	(void)sig;
	running = 0;
	g_dec.abort_flag = 1;
}

static void
on_cli_sigint(int sig)
{
	(void)sig;
	cli_stop_req = 1;
}

#ifdef SIGUSR1
static void
on_cli_sigusr1(int sig)
{
	(void)sig;
	cli_cancel_req = 1;
}
#endif

static bool
whisper_abort_cb(void *user_data)
{
	(void)user_data;
	return (!running || g_cancel_req || g_dec.abort_flag) ? true : false;
}

static long
now_ms(void)
{
	return (long)stl_now_ms();
}

#ifndef _WIN32
static const char *
get_username(void)
{
	static char cached[64];
	stl_username(cached, sizeof(cached));
	return cached[0] ? cached : "user";
}
#endif

static const char *
get_home_dir(void)
{
	static char cached[512];
	const char *home = getenv("HOME");
	if (home && *home)
		return home;
	stl_home_dir(cached, sizeof(cached));
	return cached[0] ? cached : ".";
}

static void
notify_bar(void)
{
#ifndef _WIN32
	char path[256];
	FILE *fp;
	int pid = 0;

	if (!jarvis_rc_get("status_notify_pid_file", NULL, path, sizeof(path)) || !path[0])
		snprintf(path, sizeof(path), "/tmp/dwm-bar-%s.pid", get_username());
	fp = fopen(path, "r");
	if (fp) {
		if (fscanf(fp, "%d", &pid) == 1 && pid > 1) {
#ifdef SIGUSR1
			kill((pid_t)pid, SIGUSR1);
#endif
		}
		fclose(fp);
		return;
	}
#endif
	jarvis_plat_notify_bar();
}

static void
notify_osd_async(const char *title, const char *body, int timeout_ms)
{
	if (!title || !*title)
		return;
	jarvis_plat_notify_osd(title, body ? body : "", timeout_ms > 0 ? timeout_ms : 4000);
}

static int
remove_state_file(const char *path)
{
	int removed;
	if (!path || !*path || strcmp(path, "-") == 0)
		return 0;
	pthread_mutex_lock(&g_state_file_mtx);
	removed = (remove(path) == 0) ? 1 : 0;
	pthread_mutex_unlock(&g_state_file_mtx);
	return removed;
}

static void
write_state_file(const char *path, const char *prefix, const char *detail, const char *osd_title)
{
	char tmp[512];
	FILE *fp;

	if (!path || !*path || strcmp(path, "-") == 0)
		return;
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	pthread_mutex_lock(&g_state_file_mtx);
	fp = fopen(tmp, "wb");
	if (!fp) {
		pthread_mutex_unlock(&g_state_file_mtx);
		return;
	}
	if (detail && *detail)
		fprintf(fp, "%s:%s\n", prefix, detail);
	else
		fprintf(fp, "%s\n", prefix);
	fclose(fp);
#ifdef _WIN32
	remove(path);
#endif
	rename(tmp, path);
	pthread_mutex_unlock(&g_state_file_mtx);
	notify_bar();

	if (osd_title && *osd_title && detail && *detail)
		notify_osd_async(osd_title, detail, 6000);
}

static void
get_active_wake_word(char *out, size_t out_sz)
{
	jarvis_resolve_wake_word(get_daemon_ont(), out, out_sz);
}

static void
update_idle_voice_state(const char *state_file)
{
	if (!state_file || !*state_file || strcmp(state_file, "-") == 0)
		return;
	if (!g_jarvis_active) {
		if (g_tts_speaking) {
			write_state_file(state_file, "tts-speaking", NULL, NULL);
		} else {
			remove_state_file(state_file);
			notify_bar();
		}
		return;
	}
	if (g_tts_speaking) {
		write_state_file(state_file, "tts-speaking", NULL, NULL);
		return;
	}
	pthread_mutex_lock(&g_op_turn.mtx);
	int op_active = g_op_turn.active;
	pthread_mutex_unlock(&g_op_turn.mtx);
	if (op_active) {
		write_state_file(state_file, "processing", NULL, NULL);
		return;
	}
	if (g_in_followup || g_in_post_wake) {
		write_state_file(state_file, "jarvis-cmd", NULL, NULL);
		return;
	}
	write_state_file(state_file, "jarvis", NULL, NULL);
}

static void
play_chime_async(const char *chime_wav)
{
	if (!chime_wav || !*chime_wav || strcmp(chime_wav, "-") == 0)
		return;
	jarvis_audio_play_chime_async(chime_wav);
}

static void
daemon_notify(const char *title, const char *body, int earcon)
{
	notify_osd_async(title, body, 4000);
	if (earcon) {
		char done_wav[256];
		if (jarvis_plat_state_path("jarvis-done", ".wav", done_wav, sizeof(done_wav)) == 0)
			play_chime_async(done_wav);
	}
}

/* Dynamically resolve "kbd" / locale language or normalize ISO tags ("pt-pt" -> "pt") for Whisper. */
static void
resolve_lang(const char *req_lang, char *out, size_t out_sz)
{
	char resolved[32] = {0};
	if (!req_lang || !*req_lang || strcmp(req_lang, "kbd") == 0) {
		jarvis_resolve_lang(get_daemon_ont(), resolved, sizeof(resolved));
		req_lang = resolved;
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

static void
precache_active_wake_ack(void)
{
	if (!jarvis_tts_is_enabled())
		return;
	char eff_lang[32] = {0};
	jarvis_resolve_lang(get_daemon_ont(), eff_lang, sizeof(eff_lang));
	char ack[128] = {0};
	if (jarvis_resolve_wake_ack(get_daemon_ont(), eff_lang, ack, sizeof(ack)) && ack[0])
		jarvis_tts_precache_clause(ack, eff_lang);
}

/* Speak the language-appropriate wake acknowledgment ("Yes?", "Sim?", etc.) when TTS
 * and wake_ack are enabled; otherwise fall back to the earcon chime WAV.
 * Returns 1 if spoken TTS wake ack was queued, 0 if chime fallback was used.
 */
static int
trigger_wake_ack_response(const char *req_lang, const char *detected_lang, const char *chime_wav)
{
	char eff_lang[32] = {0};
	if (req_lang && strcasecmp(req_lang, "auto") == 0 && detected_lang && *detected_lang) {
		snprintf(eff_lang, sizeof(eff_lang), "%s", detected_lang);
	} else if (req_lang && *req_lang && strcasecmp(req_lang, "auto") != 0 && strcasecmp(req_lang, "kbd") != 0) {
		snprintf(eff_lang, sizeof(eff_lang), "%s", req_lang);
	} else {
		jarvis_resolve_lang(get_daemon_ont(), eff_lang, sizeof(eff_lang));
	}

	char ack[128] = {0};
	if (jarvis_tts_is_enabled() &&
	    jarvis_resolve_wake_ack(get_daemon_ont(), eff_lang, ack, sizeof(ack)) &&
	    ack[0] != '\0') {
		jarvis_tts_speak_clause_lang("wake_ack", ack, eff_lang, 0);
		return 1;
	}

	jarvis_tts_speak_clause("warmup", "", 0);
	play_chime_async(chime_wav);
	return 0;
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
calc_frame_db(const float *samples, size_t n, vad_biquad_t *flt)
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
		double x = (double)samples[i];
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

static bool
is_volume_adjustment_command(const char *spoken_cmd)
{
	if (!spoken_cmd || !*spoken_cmd)
		return false;
	static const char *const kws[] = {
		"volume", "louder", "quieter", "mute", "unmute",
		"sobe o som", "baixa o som", "aumenta o som", "diminui o som",
		"aumentar o volume", "diminuir o volume", "aumentar o som", "diminuir o som",
		"mais alto", "mais baixo", "sem som", "calar", "silencia", NULL
	};
	for (int i = 0; kws[i]; i++) {
		if (desktop_core_str_contains_ci(spoken_cmd, kws[i]))
			return true;
	}
	return false;
}

static void
duck_speaker_for_command(void)
{
	jarvis_audio_duck();
}

static void
restore_speaker_after_command(const char *spoken_cmd)
{
	/* Do not overwrite volume if the user's command itself adjusts volume/mute */
	if (is_volume_adjustment_command(spoken_cmd))
		return;
	jarvis_audio_restore();
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
	if (avg > -8.0)
		avg = -8.0;

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
	if (start_db > -3.0)
		start_db = -3.0;
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

static inline int
frame_is_voiced_start(float db, float ns, float vp, int sp_on, uint8_t tts_hot,
                      double eff_start_db, float ns_min, float vp_min)
{
	if (tts_hot) {
		float tts_ns = (ns_min > 0.55f) ? ns_min : 0.55f;
		if (sp_on && ns < tts_ns)
			return 0;
		if (vp >= 0.0f) {
			float tts_vp = (vp_min > 0.65f) ? vp_min : 0.65f;
			return (vp >= tts_vp && db >= (eff_start_db - 1.0)) ? 1 : 0;
		}
		return (db >= (eff_start_db + 2.0)) ? 1 : 0;
	}
	if (sp_on && ns < ns_min)
		return 0;
	if (vp >= 0.0f)
		return (vp >= vp_min && db >= (eff_start_db - 2.0)) ? 1 : 0;
	return (db >= eff_start_db) ? 1 : 0;
}

static inline int
frame_is_voiced_keep(float db, float ns, float vp, int sp_on, double eff_keep_db)
{
	if (sp_on && ns < 0.22f)
		return 0;
	if (vp >= 0.0f)
		return (vp >= 0.28f || (db >= eff_keep_db && vp >= 0.12f)) ? 1 : 0;
	return (db >= eff_keep_db) ? 1 : 0;
}

static void *
audio_thread_fn(void *arg)
{
	jarvis_audio_cap_t *cap = NULL;
	int cap_ch = 1;
	int16_t raw[FRAME_SAMPLES * 2];
	float clean_pcm[FRAME_SAMPLES];
	vad_biquad_t vad_flt;
	jarvis_dsp_t dsp;
	jarvis_nvad_t nvad;
	char vad_model_path[512] = {0};

	memset(&vad_flt, 0, sizeof(vad_flt));
	jarvis_dsp_init(&dsp, SAMPLE_RATE, 1);
	if (jarvis_nvad_resolve_model(vad_model_path, sizeof(vad_model_path)) == 0)
		jarvis_nvad_init(&nvad, vad_model_path, 2);
	else
		jarvis_nvad_init(&nvad, NULL, 2);

	if (jarvis_nvad_is_available(&nvad)) {
		fprintf(stderr, "jarvisd: streaming Silero VAD enabled (%s)\n", nvad.model_path);
	} else {
		fprintf(stderr, "jarvisd: Silero VAD model not found; using adaptive energy VAD fallback\n");
	}

	pthread_mutex_lock(&g_audio.mtx);
	g_audio.vad_neural = jarvis_nvad_is_available(&nvad);
	pthread_mutex_unlock(&g_audio.mtx);

	(void)arg;
	while (running) {
		pthread_mutex_lock(&g_audio.mtx);
		int need = (g_audio.active_users > 0) || (now_ms() < g_audio.hold_until_ms);
		if (!need) {
			if (cap) {
				g_audio.pw_open = 0;
				g_audio.stereo_spatial = 0;
				g_audio.aec_active = 0;
				pthread_mutex_unlock(&g_audio.mtx);
				jarvis_audio_cap_close(cap);
				cap = NULL;
				pthread_mutex_lock(&g_audio.mtx);
			}
			while (running && g_audio.active_users <= 0 && now_ms() >= g_audio.hold_until_ms)
				pthread_cond_wait(&g_audio.cond, &g_audio.mtx);
			pthread_mutex_unlock(&g_audio.mtx);
			continue;
		}
		pthread_mutex_unlock(&g_audio.mtx);

		if (!cap) {
			/* Prefer 2-channel capture for adaptive off-axis-safe stereo coherence;
			 * automatically fall back to 1-channel on strictly mono devices, and
			 * auto-bypass spatial scoring if hardware is mono-summed (L == R).
			 */
			cap_ch = 2;
			cap = jarvis_audio_cap_open(SAMPLE_RATE, 2);
			if (!cap) {
				cap_ch = 1;
				cap = jarvis_audio_cap_open(SAMPLE_RATE, 1);
			}
			if (!cap) {
				stl_sleep_ms(50);
				continue;
			}
			memset(&vad_flt, 0, sizeof(vad_flt));
			jarvis_dsp_init(&dsp, SAMPLE_RATE, cap_ch);
			jarvis_nvad_reset(&nvad);
			int has_aec = jarvis_audio_has_aec_source();
			pthread_mutex_lock(&g_audio.mtx);
			g_audio.pw_open = 1;
			g_audio.stereo_spatial = 0;
			g_audio.aec_active = has_aec;
			g_audio.vad_neural = jarvis_nvad_is_available(&nvad);
			pthread_cond_broadcast(&g_audio.cond);
			pthread_mutex_unlock(&g_audio.mtx);
		}

		if (jarvis_audio_cap_read(cap, raw, (size_t)FRAME_SAMPLES * (size_t)cap_ch) < 0) {
			jarvis_audio_cap_close(cap);
			cap = NULL;
			pthread_mutex_lock(&g_audio.mtx);
			g_audio.pw_open = 0;
			g_audio.stereo_spatial = 0;
			g_audio.aec_active = 0;
			pthread_mutex_unlock(&g_audio.mtx);
			stl_sleep_ms(20);
			continue;
		}

		float near_score = 1.0f;
		jarvis_dsp_process_frame(&dsp, raw, FRAME_SAMPLES, clean_pcm, &near_score);
		int stereo_sp = jarvis_dsp_is_stereo_active(&dsp);
		double db = calc_frame_db(clean_pcm, FRAME_SAMPLES, &vad_flt);
		float vad_p = jarvis_nvad_process(&nvad, clean_pcm, FRAME_SAMPLES);
		uint8_t tts_hot = is_tts_audio_hot() ? 1 : 0;

		pthread_mutex_lock(&g_audio.mtx);
		int slot = (int)(g_audio.total_frames % RING_FRAMES);
		float *dst = g_audio.pcm + (size_t)slot * FRAME_SAMPLES;
		memcpy(dst, clean_pcm, (size_t)FRAME_SAMPLES * sizeof(float));
		g_audio.db[slot] = (float)db;
		g_audio.near_score[slot] = near_score;
		g_audio.vad_prob[slot] = vad_p;
		g_audio.tts_active[slot] = tts_hot;
		g_audio.stereo_spatial = stereo_sp;
		if (vad_p >= 0.0f) {
			g_audio.is_speech[slot] = (vad_p >= 0.45f &&
			                           (!stereo_sp || near_score >= 0.35f)) ? 1 : 0;
		} else {
			g_audio.is_speech[slot] = (g_audio.env_frames >= 10 &&
			                           db > g_audio.noise_floor_db + 8.5 &&
			                           (!stereo_sp || near_score >= 0.40f)) ? 1 : 0;
		}
		g_audio.total_frames++;

		/* Sliding-window ambient average tracker:
		 * Measure the trimmed-mean noise level and spread over the last 80 frames (1.6s).
		 * Because is_speech[slot] only marks genuine speech frames (via Silero VAD or
		 * +8.5 dB fallback peaks), continuous room audio smoothly updates the noise floor.
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

	jarvis_nvad_free(&nvad);
	jarvis_audio_cap_close(cap);
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
		jarvis_clock_realtime(&ts);
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
	return jarvis_stt_ring_extract_pcm(start_f, end_f, out_pcm, max_samples);
}

static int
load_wav_f32(const char *path, float **out_pcm, int *out_samples)
{
	return jarvis_stt_load_wav_f32(path, out_pcm, out_samples);
}

static int
compute_audio_ctx(int n_samples, int is_wake, int is_partial)
{
	char cfg_buf[32] = {0};
	int final_min_ctx = g_hw.final_audio_ctx;
	if (jarvis_rc_get("whisper_audio_ctx_min", NULL, cfg_buf, sizeof(cfg_buf)) && cfg_buf[0])
		final_min_ctx = atoi(cfg_buf);
	return jarvis_stt_compute_audio_ctx_ex(n_samples, is_wake, is_partial, final_min_ctx);
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

static void
run_whisper_slice_opts(jarvis_stt_ctx_t *ctx, int n_threads, const float *pcm, int n_samples,
                       const char *lang, const char *prompt, int max_tok, int is_wake, int is_partial,
                       int use_abort_cb, int use_context,
                       jarvis_stt_word_t *out_words, int max_words, int *out_n_words,
                       char *out_text, size_t out_sz, int *out_all_silent,
                       char *out_detected_lang, size_t out_lang_sz)
{
	char eff_lang[32];

	out_text[0] = '\0';
	if (out_all_silent)
		*out_all_silent = 0;
	if (out_n_words)
		*out_n_words = 0;
	if (out_detected_lang && out_lang_sz > 0)
		out_detected_lang[0] = '\0';
	if (!ctx || n_samples < 1600)
		return;

	resolve_lang(lang, eff_lang, sizeof(eff_lang));

	bool is_final_cmd = (!is_wake && !is_partial);
	int default_beam = (g_hw.beam_size >= 1) ? g_hw.beam_size : 1;
	int beam_sz = is_final_cmd ? jarvis_rc_get_int("whisper_beam_size", default_beam) : 1;

	jarvis_stt_req_t req;
	memset(&req, 0, sizeof(req));
	req.pcm         = pcm;
	req.n_samples   = n_samples;
	req.n_threads   = n_threads;
	req.lang        = eff_lang;
	req.prompt      = prompt;
	req.max_tok     = max_tok;
	req.audio_ctx   = compute_audio_ctx(n_samples, is_wake, is_partial);
	req.beam_size   = beam_sz;
	req.is_wake     = is_wake;
	req.is_partial  = is_partial;
	req.apply_agc   = 1;
	req.use_context = use_context;
	req.out_words   = out_words;
	req.max_words   = max_words;
	if (use_abort_cb) {
		req.abort_cb = whisper_abort_cb;
		req.abort_ud = NULL;
	}

	jarvis_stt_res_t res;
	if (jarvis_stt_transcribe(ctx, &req, &res) == 0) {
		if (out_n_words)
			*out_n_words = res.n_words;
		if (out_detected_lang && out_lang_sz > 0 && res.detected_lang[0])
			snprintf(out_detected_lang, out_lang_sz, "%.*s", (int)(out_lang_sz - 1), res.detected_lang);
		if (res.text[0])
			snprintf(out_text, out_sz, "%s", res.text);
		if (res.all_silent) {
			if (out_all_silent)
				*out_all_silent = 1;
			/* Instant noise-floor recalibration when STT confirms the captured
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
}

static void
run_whisper_slice_ex(jarvis_stt_ctx_t *ctx, int n_threads, const float *pcm, int n_samples,
                     const char *lang, const char *prompt, int max_tok, int is_wake, int is_partial,
                     int use_abort_cb, char *out_text, size_t out_sz, int *out_all_silent,
                     char *out_detected_lang, size_t out_lang_sz)
{
	run_whisper_slice_opts(ctx, n_threads, pcm, n_samples, lang, prompt, max_tok,
	                       is_wake, is_partial, use_abort_cb, 0, NULL, 0, NULL,
	                       out_text, out_sz, out_all_silent, out_detected_lang, out_lang_sz);
}

static void
run_whisper_slice(jarvis_stt_ctx_t *ctx, int n_threads, const float *pcm, int n_samples,
                  const char *lang, const char *prompt, int max_tok, int is_wake, int is_partial,
                  int use_abort_cb, char *out_text, size_t out_sz, int *out_all_silent)
{
	run_whisper_slice_ex(ctx, n_threads, pcm, n_samples, lang, prompt, max_tok,
	                     is_wake, is_partial, use_abort_cb, out_text, out_sz,
	                     out_all_silent, NULL, 0);
}

/* Dedicated background worker thread for ctx_main so the 20ms PipeWire VAD loop
 * never blocks during speculative or final Whisper passes.
 */
static void *
decode_worker_fn(void *arg)
{
	(void)arg;
	jarvis_stt_word_t *local_words = calloc(JARVIS_STT_MAX_WORDS, sizeof(*local_words));
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
		char prompt[256];
		snprintf(prompt, sizeof(prompt), "%s", g_dec.prompt);
		int max_tok = g_dec.max_tok;
		int is_partial = g_dec.is_partial;
		int use_context = g_dec.use_context;
		int want_ts = g_dec.want_timestamps;
		int64_t job_sf = g_dec.job_start_f;
		int64_t job_vf = g_dec.job_voiced_f;
		int64_t job_ef = g_dec.job_end_f;
		pthread_mutex_unlock(&g_dec.mtx);

		char decoded[4096] = {0};
		int all_silent = 0;
		int n_words = 0;
		run_whisper_slice_opts(g_dec.ctx, g_dec.n_threads, g_dec.pcm, n_samples,
		                       lang, prompt[0] ? prompt : NULL, max_tok, 0, is_partial, 1,
		                       use_context,
		                       (want_ts && local_words) ? local_words : NULL,
		                       (want_ts && local_words) ? JARVIS_STT_MAX_WORDS : 0,
		                       &n_words,
		                       decoded, sizeof(decoded), &all_silent, NULL, 0);

		pthread_mutex_lock(&g_audio.mtx);
		int64_t cur_audio_f = g_audio.total_frames;
		pthread_mutex_unlock(&g_audio.mtx);

		pthread_mutex_lock(&g_dec.mtx);
		g_dec.busy = 0;
		g_dec.done_at_frame = cur_audio_f;
		if (!g_dec.abort_flag && !g_cancel_req) {
			snprintf(g_dec.result_text, sizeof(g_dec.result_text), "%s", decoded);
			if (n_words > JARVIS_STT_MAX_WORDS)
				n_words = JARVIS_STT_MAX_WORDS;
			if (n_words > 0 && local_words) {
				memcpy(g_dec.result_words, local_words,
				       (size_t)n_words * sizeof(jarvis_stt_word_t));
			}
			g_dec.result_n_words = (n_words > 0 && local_words) ? n_words : 0;
			g_dec.result_start_f = job_sf;
			g_dec.result_voiced_f = job_vf;
			g_dec.result_end_f = job_ef;
			g_dec.result_is_partial = is_partial;
			g_dec.result_all_silent = all_silent;
			g_dec.has_result = 1;
		}
		pthread_cond_broadcast(&g_dec.cond);
		pthread_mutex_unlock(&g_dec.mtx);
	}
	free(local_words);
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
	g_dec.result_n_words = 0;
	g_dec.result_start_f = -1;
	g_dec.result_voiced_f = -1;
	g_dec.result_end_f = -1;
	g_dec.result_all_silent = 0;
	g_dec.done_at_frame = 0;
	pthread_mutex_unlock(&g_dec.mtx);
}

static int
decode_try_submit_ex(int64_t start_f, int64_t end_f, int64_t voiced_f,
                     const char *lang, const char *prompt, int max_tok, int is_partial,
                     int use_context, int want_timestamps)
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
	snprintf(g_dec.prompt, sizeof(g_dec.prompt), "%s", prompt ? prompt : "");
	g_dec.max_tok = max_tok;
	g_dec.is_partial = is_partial;
	g_dec.use_context = use_context;
	g_dec.want_timestamps = want_timestamps;
	g_dec.job_start_f = start_f;
	g_dec.job_voiced_f = voiced_f;
	g_dec.job_end_f = end_f;
	g_dec.abort_flag = 0;
	g_dec.has_job = 1;
	pthread_cond_broadcast(&g_dec.cond);
	pthread_mutex_unlock(&g_dec.mtx);
	return 1;
}

static int
decode_try_submit(int64_t start_f, int64_t end_f, int64_t voiced_f,
                  const char *lang, const char *prompt, int max_tok, int is_partial)
{
	return decode_try_submit_ex(start_f, end_f, voiced_f, lang, prompt, max_tok,
	                            is_partial, 0, 0);
}

static void
decode_try_abort_partial(void)
{
	pthread_mutex_lock(&g_dec.mtx);
	if (g_dec.busy && g_dec.is_partial) {
		g_dec.abort_flag = 1;
	}
	pthread_mutex_unlock(&g_dec.mtx);
}

static void
decode_force_submit_final_ex(int64_t start_f, int64_t end_f, int64_t voiced_f,
                             const char *lang, const char *prompt, int max_tok,
                             int use_context, int want_timestamps)
{
	pthread_mutex_lock(&g_dec.mtx);
	if (g_dec.busy) {
		if (!g_dec.is_partial && g_dec.job_start_f == start_f &&
		    g_dec.want_timestamps == want_timestamps &&
		    g_dec.job_voiced_f >= voiced_f - 1 &&
		    g_dec.job_end_f >= voiced_f + 12) {
			pthread_mutex_unlock(&g_dec.mtx);
			return;
		}
		g_dec.abort_flag = 1;
		while (g_dec.busy)
			pthread_cond_wait(&g_dec.cond, &g_dec.mtx);
	} else if (!g_dec.has_job && g_dec.has_result &&
	           !g_dec.result_is_partial && g_dec.result_start_f == start_f &&
	           (!want_timestamps || g_dec.result_n_words > 0) &&
	           g_dec.result_voiced_f >= voiced_f - 1 &&
	           g_dec.result_end_f >= voiced_f + 12) {
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
		snprintf(g_dec.prompt, sizeof(g_dec.prompt), "%s", prompt ? prompt : "");
		g_dec.max_tok = max_tok;
		g_dec.is_partial = 0;
		g_dec.use_context = use_context;
		g_dec.want_timestamps = want_timestamps;
		g_dec.job_start_f = start_f;
		g_dec.job_voiced_f = voiced_f;
		g_dec.job_end_f = end_f;
		g_dec.abort_flag = 0;
		g_dec.has_job = 1;
		pthread_cond_broadcast(&g_dec.cond);
	}
	pthread_mutex_unlock(&g_dec.mtx);
}

static void
decode_force_submit_final(int64_t start_f, int64_t end_f, int64_t voiced_f,
                          const char *lang, const char *prompt, int max_tok)
{
	decode_force_submit_final_ex(start_f, end_f, voiced_f, lang, prompt, max_tok, 0, 0);
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

#define TTS_DRAIN_COOLDOWN_AEC_MS  350L
#define TTS_DRAIN_COOLDOWN_MS      650L
#define WAKE_ACK_DRAIN_MS          160L
#define JARVIS_FOLLOWUP_PERSISTENT 3600000

static char g_last_tts_cache[2048] = {0};
static long g_last_tts_ms = 0;
static long g_tts_last_active_ms = 0;

static long
tts_drain_cooldown_ms(void)
{
	int aec = __atomic_load_n(&g_audio.aec_active, __ATOMIC_RELAXED);
	long def = aec ? TTS_DRAIN_COOLDOWN_AEC_MS : TTS_DRAIN_COOLDOWN_MS;
	int cfg = jarvis_rc_get_int("tts_drain_ms", (int)def);
	if (cfg < 100) cfg = 100;
	if (cfg > 2000) cfg = 2000;
	return (long)cfg;
}

static void
clear_tts_speaking_cache(void)
{
	g_last_tts_cache[0] = '\0';
	g_last_tts_ms = 0;
	g_tts_last_active_ms = 0;
}

static void
interrupt_tts_and_clear_cache(void)
{
	clear_tts_speaking_cache();
	g_tts_speaking = 0;
	jarvis_tts_interrupt(0);
}

static int
is_tts_file_active(void)
{
	char path[256];
	if (jarvis_plat_state_path("jarvis-tts-active", ".state", path, sizeof(path)) != 0)
		return 0;
	return jarvis_plat_is_readable(path);
}

static int
is_tts_audio_hot(void)
{
	long now = now_ms();
	if (g_tts_speaking || is_tts_file_active()) {
		g_tts_last_active_ms = now;
		return 1;
	}
	long cool = tts_drain_cooldown_ms();
	if (g_tts_last_active_ms > 0 && (now - g_tts_last_active_ms) < cool)
		return 1;
	return 0;
}

static int
is_tts_currently_speaking(char *out_text, size_t out_sz)
{
	if (out_text && out_sz > 0)
		out_text[0] = '\0';
	long now = now_ms();
	char path[256];
	FILE *fp = (jarvis_plat_state_path("jarvis-tts-active", ".state", path, sizeof(path)) == 0)
	         ? fopen(path, "r") : NULL;
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
		g_tts_last_active_ms = now;
		if (buf[0]) {
			snprintf(g_last_tts_cache, sizeof(g_last_tts_cache), "%s", buf);
			g_last_tts_ms = now;
		}
		if (out_text && out_sz > 0)
			snprintf(out_text, out_sz, "%.*s", (int)(out_sz - 1), buf);
		return 1;
	}
	if (g_tts_speaking) {
		g_tts_last_active_ms = now;
		if (out_text && out_sz > 0 && g_last_tts_cache[0])
			snprintf(out_text, out_sz, "%.*s", (int)(out_sz - 1), g_last_tts_cache);
		return 1;
	}
	/* Post-TTS acoustic drain cooldown so playback buffer + room reverb + capture
	 * pipe tail are still recognized as active TTS audio (shorter when AEC is active).
	 */
	long cool = tts_drain_cooldown_ms();
	if ((g_last_tts_cache[0] != '\0' && (now - g_last_tts_ms) < cool) ||
	    (g_tts_last_active_ms > 0 && (now - g_tts_last_active_ms) < cool)) {
		if (out_text && out_sz > 0 && g_last_tts_cache[0])
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
	char norm[256], wake_buf[64];
	jarvis_clean_phrase(text, norm, sizeof(norm));
	get_active_wake_word(wake_buf, sizeof(wake_buf));
	return jarvis_ontology_is_tts_interrupt_ex(get_daemon_ont(), norm, wake_buf) ? 1 : 0;
}

static int
is_op_abort_phrase(const char *text)
{
	if (!text || !*text)
		return 0;
	char wake_buf[64];
	get_active_wake_word(wake_buf, sizeof(wake_buf));
	return jarvis_ontology_is_op_abort(get_daemon_ont(), text, wake_buf) ? 1 : 0;
}

/*
 * Exact Acoustic Self-Echo Verification (`is_tts_self_echo`):
 * Compares the normalized words decoded by Whisper during TTS playback against
 * the active spoken TTS transcript.
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
	for (char *tok = dc_strtok_r(h_clean, " ", &save); tok; tok = dc_strtok_r(NULL, " ", &save)) {
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

static int
jarvis_apply_active(int active, int toggle, int defer_tts_interrupt)
{
	char state_file[256], jpid_file[256], apid_file[256];
	char legacy_state[256], legacy_jpid[256], legacy_apid[256];
	jarvis_plat_state_path("jarvis-voice", ".state", state_file, sizeof(state_file));
	jarvis_plat_state_path("jarvis", ".pid", jpid_file, sizeof(jpid_file));
	jarvis_plat_state_path("jarvis-acting", ".pid", apid_file, sizeof(apid_file));
	jarvis_plat_state_path("dwm-voice", ".state", legacy_state, sizeof(legacy_state));
	jarvis_plat_state_path("dwm-jarvis", ".pid", legacy_jpid, sizeof(legacy_jpid));
	jarvis_plat_state_path("dwm-acting", ".pid", legacy_apid, sizeof(legacy_apid));

	pthread_mutex_lock(&g_ctrl.mtx);
	int next = toggle ? !g_jarvis_active : (active ? 1 : 0);
	g_jarvis_active = next;
	if (!next) {
		g_in_followup = 0;
		g_in_post_wake = 0;
		g_cancel_req = 1;
		g_tts_speaking = 0;
		g_ctrl.can_preempt_wait = 0;
	}
	pthread_cond_broadcast(&g_ctrl.cond);
	pthread_mutex_unlock(&g_ctrl.mtx);

	if (next) {
		pthread_mutex_lock(&g_state_file_mtx);
		FILE *fp = fopen(jpid_file, "w");
		if (fp) {
			fprintf(fp, "%d\n", (int)getpid());
			fclose(fp);
		}
		pthread_mutex_unlock(&g_state_file_mtx);
		update_idle_voice_state(state_file);
		precache_active_wake_ack();
	} else {
		pthread_mutex_lock(&g_op_turn.mtx);
		if (g_op_turn.active)
			g_op_turn.was_stopped = 1;
		pthread_mutex_unlock(&g_op_turn.mtx);

		pthread_mutex_lock(&g_dec.mtx);
		g_dec.abort_flag = 1;
		pthread_cond_broadcast(&g_dec.cond);
		pthread_mutex_unlock(&g_dec.mtx);

		pthread_mutex_lock(&g_audio.mtx);
		pthread_cond_broadcast(&g_audio.cond);
		pthread_mutex_unlock(&g_audio.mtx);

		pthread_mutex_lock(&g_state_file_mtx);
		remove(jpid_file);
		remove(apid_file);
		remove(state_file);
		remove(legacy_jpid);
		remove(legacy_apid);
		remove(legacy_state);
		pthread_mutex_unlock(&g_state_file_mtx);
		notify_bar();
		if (!defer_tts_interrupt)
			interrupt_tts_and_clear_cache();
	}
	return next;
}

static void
jarvis_set_active(int active)
{
	(void)jarvis_apply_active(active, 0, 0);
}

static int
jarvis_toggle_active(int defer_tts_interrupt)
{
	return jarvis_apply_active(0, 1, defer_tts_interrupt);
}

static ssize_t
read_ctrl_line(stl_socket_t cfd, char *buf, size_t cap)
{
	size_t total = 0;
	while (total + 1 < cap) {
		ssize_t r = stl_sock_recv(cfd, buf + total, cap - 1 - total, 0);
		if (r <= 0)
			break;
		total += (size_t)r;
		if (memchr(buf, '\n', total) != NULL)
			break;
	}
	buf[total] = '\0';
	return (ssize_t)total;
}

/* Handle fast O(1) control commands directly on the dedicated control thread.
 * Returns 1 if cfd was handled and closed, 0 if cfd is a session command to queue.
 */
static int
handle_control_cmd(stl_socket_t cfd, char *cbuf)
{
	if (strncmp(cbuf, "GET /", 5) == 0 || strncmp(cbuf, "PING", 4) == 0) {
		const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
		jarvis_plat_write_sock_all(cfd, ok, strlen(ok));
		stl_sock_close(cfd);
		return 1;
	}
	if (strncmp(cbuf, "STATUS", 6) == 0) {
		char sbuf[512];
		pthread_mutex_lock(&g_audio.mtx);
		int64_t tf = g_audio.total_frames;
		double ldb = (tf > 0) ? g_audio.db[(tf - 1) % RING_FRAMES] : -100.0;
		float lns = (tf > 0) ? g_audio.near_score[(tf - 1) % RING_FRAMES] : 1.0f;
		float lvp = (tf > 0) ? g_audio.vad_prob[(tf - 1) % RING_FRAMES] : -1.0f;
		snprintf(sbuf, sizeof(sbuf),
		         "STATUS|frames=%lld|nf_db=%.1f|std_db=%.1f|last_db=%.1f|pw_open=%d|in_speech=%d|jarvis=%d|stereo=%d|near=%.2f|vad=%d|vad_p=%.2f|aec=%d|hw=%s:%s|stt=%s|ac512=%d|ac=%d|kws=%s|wake_n=%llu|ptt=%d\n",
		         (long long)tf, g_audio.noise_floor_db, g_audio.ambient_std_db,
		         ldb, g_audio.pw_open, g_audio.in_speech_hint, (int)g_jarvis_active,
		         g_audio.stereo_spatial, lns, g_audio.vad_neural, lvp, g_audio.aec_active,
		         jarvis_hw_tier_str(g_hw.tier), g_hw.backend,
		         jarvis_stt_backend_name(g_dec.ctx), g_hw.ac512_ms, g_hw.final_audio_ctx,
		         jarvis_kws_tier_str(g_kws), (unsigned long long)g_wake_stt_calls, (int)g_ptt_active);
		pthread_mutex_unlock(&g_audio.mtx);
		jarvis_plat_write_sock_all(cfd, sbuf, strlen(sbuf));
		stl_sock_close(cfd);
		return 1;
	}
	if (strncmp(cbuf, "JARVIS_PTT_START", 16) == 0) {
		g_ptt_active = 1;
		if (!g_jarvis_active)
			jarvis_set_active(1);
		interrupt_tts_and_clear_cache();
		jarvis_plat_write_sock_all(cfd, "OK\n", 3);
		stl_sock_close(cfd);
		return 1;
	}
	if (strncmp(cbuf, "JARVIS_PTT_STOP", 15) == 0) {
		g_ptt_active = 0;
		jarvis_plat_write_sock_all(cfd, "OK\n", 3);
		stl_sock_close(cfd);
		return 1;
	}
	if (strncmp(cbuf, "JARVIS_STATUS", 13) == 0) {
		pthread_mutex_lock(&g_ctrl.mtx);
		int active = (int)g_jarvis_active;
		pthread_mutex_unlock(&g_ctrl.mtx);
		const char *st = active ? "ON\n" : "OFF\n";
		jarvis_plat_write_sock_all(cfd, st, strlen(st));
		stl_sock_close(cfd);
		return 1;
	}
	if (strncmp(cbuf, "JARVIS_ON", 9) == 0) {
		jarvis_set_active(1);
		jarvis_plat_write_sock_all(cfd, "ON\n", 3);
		stl_sock_close(cfd);
		return 1;
	}
	if (strncmp(cbuf, "JARVIS_OFF", 10) == 0) {
		(void)jarvis_apply_active(0, 0, 1);
		jarvis_plat_write_sock_all(cfd, "OFF\n", 4);
		stl_sock_close(cfd);
		interrupt_tts_and_clear_cache();
		return 1;
	}
	if (strncmp(cbuf, "JARVIS_TOGGLE", 13) == 0) {
		int next = jarvis_toggle_active(1);
		const char *st = next ? "ON\n" : "OFF\n";
		jarvis_plat_write_sock_all(cfd, st, strlen(st));
		stl_sock_close(cfd);
		if (!next)
			interrupt_tts_and_clear_cache();
		return 1;
	}
	if (strncmp(cbuf, "TTS_STATE|", 10) == 0) {
		char *st = cbuf + 10;
		char *nl = strpbrk(st, "\r\n");
		if (nl) *nl = '\0';
		char sf[256];
		jarvis_plat_state_path("jarvis-voice", ".state", sf, sizeof(sf));
		if (strcmp(st, "speaking") == 0) {
			if (g_jarvis_active || is_tts_file_active()) {
				g_tts_speaking = 1;
				g_tts_last_active_ms = now_ms();
				update_idle_voice_state(sf);
			}
		} else if (strcmp(st, "idle") == 0) {
			if (g_tts_speaking)
				g_tts_last_active_ms = now_ms();
			g_tts_speaking = 0;
			update_idle_voice_state(sf);
		}
		jarvis_plat_write_sock_all(cfd, "OK\n", 3);
		stl_sock_close(cfd);
		return 1;
	}
	if (strncmp(cbuf, "TTS_INTERRUPT", 13) == 0) {
		interrupt_tts_and_clear_cache();
		jarvis_plat_write_sock_all(cfd, "OK\n", 3);
		stl_sock_close(cfd);
		return 1;
	}
	if (strncmp(cbuf, "TTS_SPEAK|", 10) == 0) {
		char *txt = cbuf + 10;
		char *nl = strpbrk(txt, "\r\n");
		if (nl) *nl = '\0';
		if (*txt) {
			interrupt_tts_and_clear_cache();
			jarvis_tts_speak_clause("oneshot", txt, 1);
		}
		jarvis_plat_write_sock_all(cfd, "OK\n", 3);
		stl_sock_close(cfd);
		return 1;
	}
	if (strncmp(cbuf, "TTS_NOTIFY|", 11) == 0) {
		char *payload = cbuf + 11;
		char *nl = strpbrk(payload, "\r\n");
		if (nl) *nl = '\0';
		char *sep = strchr(payload, '|');
		char def_title[128], wake_buf[64];
		get_active_wake_word(wake_buf, sizeof(wake_buf));
		jarvis_ontology_format_ui(get_daemon_ont(), "notify_title", wake_buf,
		                          "  {Wake}", def_title, sizeof(def_title));
		const char *title = def_title;
		const char *msg = payload;
		if (sep) {
			*sep = '\0';
			if (payload[0]) title = payload;
			msg = sep + 1;
		}
		if (*msg) {
			notify_osd_async(title, msg, 5500);
			if (!g_audio.in_speech_hint && !is_tts_currently_speaking(NULL, 0))
				jarvis_tts_speak_clause("notify", msg, 0);
		}
		jarvis_plat_write_sock_all(cfd, "OK\n", 3);
		stl_sock_close(cfd);
		return 1;
	}
	if (strncmp(cbuf, "STOP", 4) == 0) {
		g_stop_req = 1;
		pthread_mutex_lock(&g_audio.mtx);
		pthread_cond_broadcast(&g_audio.cond);
		pthread_mutex_unlock(&g_audio.mtx);
		jarvis_plat_write_sock_all(cfd, "OK\n", 3);
		stl_sock_close(cfd);
		return 1;
	}
	if (strncmp(cbuf, "CANCEL", 6) == 0) {
		g_cancel_req = 1;
		pthread_mutex_lock(&g_dec.mtx);
		g_dec.abort_flag = 1;
		pthread_cond_broadcast(&g_dec.cond);
		pthread_mutex_unlock(&g_dec.mtx);
		pthread_mutex_lock(&g_audio.mtx);
		pthread_cond_broadcast(&g_audio.cond);
		pthread_mutex_unlock(&g_audio.mtx);
		jarvis_plat_write_sock_all(cfd, "OK\n", 3);
		stl_sock_close(cfd);
		return 1;
	}
	return 0;
}

/* Dedicated control-plane socket acceptor thread.
 * Always serves PING, STATUS, JARVIS_TOGGLE, JARVIS_ON, JARVIS_OFF, TTS, STOP, and CANCEL
 * in <1ms regardless of model loading, Whisper inference, or active ren turns.
 */
static void *
control_thread_fn(void *arg)
{
	(void)arg;
	while (running) {
		struct pollfd pfd;
		pfd.fd = g_ctrl.srv;
		pfd.events = POLLIN;
		pfd.revents = 0;

		int pr = stl_poll(&pfd, 1, 200);
		if (pr <= 0 || !(pfd.revents & POLLIN))
			continue;

		struct sockaddr_in caddr;
		socklen_t clen = sizeof(caddr);
		stl_socket_t cfd = stl_sock_accept(g_ctrl.srv, (struct sockaddr *)&caddr, &clen);
		if (cfd == STL_INVALID_SOCKET)
			continue;

		stl_sock_set_timeout(cfd, 1000, 1000);
		char cbuf[2048];
		ssize_t n = read_ctrl_line(cfd, cbuf, sizeof(cbuf));
		if (n <= 0) {
			stl_sock_close(cfd);
			continue;
		}

		if (handle_control_cmd(cfd, cbuf))
			continue;

		if (strncmp(cbuf, "STREAM|", 7) == 0 ||
		    strncmp(cbuf, "JARVIS_WAIT|", 12) == 0 ||
		    strncmp(cbuf, "TRANSCRIBE|", 11) == 0) {
			int should_preempt = 0;
			int queued = 0;
			pthread_mutex_lock(&g_ctrl.mtx);
			if (g_ctrl.pending_cli == STL_INVALID_SOCKET) {
				g_ctrl.pending_cli = cfd;
				snprintf(g_ctrl.pending_buf, sizeof(g_ctrl.pending_buf), "%s", cbuf);
				should_preempt = g_ctrl.can_preempt_wait;
				queued = 1;
				pthread_cond_broadcast(&g_ctrl.cond);
			}
			pthread_mutex_unlock(&g_ctrl.mtx);

			if (!queued) {
				stl_sock_close(cfd);
			} else if (should_preempt) {
				g_cancel_req = 1;
				pthread_mutex_lock(&g_dec.mtx);
				g_dec.abort_flag = 1;
				pthread_cond_broadcast(&g_dec.cond);
				pthread_mutex_unlock(&g_dec.mtx);
				pthread_mutex_lock(&g_audio.mtx);
				pthread_cond_broadcast(&g_audio.cond);
				pthread_mutex_unlock(&g_audio.mtx);
			}
		} else {
			stl_sock_close(cfd);
		}
	}
	return NULL;
}

static void
poll_control_socket(stl_socket_t srv, stl_socket_t cli, int is_jarvis_idle)
{
	(void)srv;
	if (cli != STL_INVALID_SOCKET && jarvis_plat_sock_peer_closed(cli)) {
		g_cancel_req = 1;
		g_dec.abort_flag = 1;
		return;
	}
	if (cli == STL_INVALID_SOCKET && g_ctrl.can_preempt_wait != is_jarvis_idle) {
		set_can_preempt_wait(is_jarvis_idle);
	}
}

static void
wait_next_audio_frame(int64_t target_frame, int max_wait_ms)
{
	pthread_mutex_lock(&g_audio.mtx);
	if (g_audio.total_frames < target_frame && running && !g_stop_req && !g_cancel_req) {
		struct timespec ts;
		jarvis_clock_realtime(&ts);
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
 *   STREAM|<mode:cmd|type|clip>|<lang>|<thresh_db>|<silence_ms>|<wait_ms>|<state_file>[|<context>]
 *
 * Features:
 *  1. Dynamic Peak-Relative Endpointing: tracks peak_speech_db during the utterance
 *     and sets eff_keep_db = max(eff_start_db - 3.5, peak_speech_db - 14.0).
 *  2. LocalAgreement-2 Sliding-Window Streaming (jarvis-stream):
 *     Commits agreeing prefix words >= 600ms behind the live edge, slides window_start_f
 *     forward to keep per-partial decode cost O(1) (~5-12s active window), carries last ~30
 *     committed words as prompt_tail across 30s boundaries, and supports multi-minute dictation.
 *  3. Regional Dialect Conditioning & Bounded Active Context:
 *     In dictation modes (type/clip), seeds window 0 with natural 1-sentence prose from
 *     ontology.json (pt-PT vs pt-BR, en-GB vs en-US, etc.) and <= 16 sanitized context tokens
 *     strictly gated on healthy SNR (>= 10 dB) so low-SNR hallucination stays 0%.
 */
static void
handle_stream(stl_socket_t srv, stl_socket_t cli, const char *mode, const char *lang,
              double thresh_db, int silence_ms, int wait_ms, const char *state_file,
              const char *active_context)
{
	int is_cmd = (strcmp(mode, "cmd") == 0);
	int max_tok = is_cmd ? 64 : 112;
	char cmd_prompt_buf[256];
	const char *cmd_prompt = is_cmd ? get_cmd_initial_prompt(cmd_prompt_buf, sizeof(cmd_prompt_buf)) : NULL;
	const char *state_prefix = is_cmd ? "cmd" : "rec";
	const char *osd_title = is_cmd
	                      ? jarvis_ontology_get_ui_string(get_daemon_ont(), "command_streaming", "  Command (Streaming)...")
	                      : jarvis_ontology_get_ui_string(get_daemon_ont(), "dictation_streaming", "  Dictation (Streaming)...");
	int preroll_f = 15; /* 300 ms clean pre-roll so soft initial consonants are never clipped */

	/* Resolve regional dialect dictation seed (dictation modes only, never cmd/wake) */
	char raw_lang[32] = {0};
	if (lang && *lang && strcmp(lang, "kbd") != 0)
		snprintf(raw_lang, sizeof(raw_lang), "%s", lang);
	else
		jarvis_resolve_lang(get_daemon_ont(), raw_lang, sizeof(raw_lang));
	const char *dictation_seed = (!is_cmd)
	                           ? jarvis_ontology_get_dictation_seed_for_lang(get_daemon_ont(), raw_lang)
	                           : NULL;

	char rc_ctx_buf[256] = {0};
	const char *eff_context = NULL;
	if (!is_cmd) {
		if (active_context && *active_context && strcmp(active_context, "-") != 0)
			eff_context = active_context;
		else if (jarvis_rc_get("dictation_context", "", rc_ctx_buf, sizeof(rc_ctx_buf)) && rc_ctx_buf[0])
			eff_context = rc_ctx_buf;
	}

	jarvis_stream_t *stream = calloc(1, sizeof(*stream));
	jarvis_stt_word_t *scratch_words = calloc(JARVIS_STT_MAX_WORDS, sizeof(*scratch_words));
	if (!stream || !scratch_words) {
		free(stream);
		free(scratch_words);
		jarvis_plat_write_sock_all(cli, "EMPTY\n", 6);
		return;
	}

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

	jarvis_stream_init(stream, speech_start_f, FRAME_MS);

	int in_speech = 0;
	int consec_voiced = 0;
	int voiced_frames = 0;
	int silence_frames = 0;
	double peak_speech_db = -100.0;
	double utter_amb_avg = -42.0;
	double utter_amb_std = 1.5;
	double utter_start_db = -35.0;
	int64_t last_voiced_f = scan_f;
	int64_t decoded_voiced_f = -1;
	char live_preview[512] = {0};
	long t_start = now_ms();
	(void)thresh_db;

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
			int rn_words = g_dec.result_n_words;
			if (rn_words > JARVIS_STT_MAX_WORDS)
				rn_words = JARVIS_STT_MAX_WORDS;
			if (rn_words > 0) {
				memcpy(scratch_words, g_dec.result_words,
				       (size_t)rn_words * sizeof(jarvis_stt_word_t));
			}
			int64_t rsf = g_dec.result_start_f;
			int64_t ref = g_dec.result_end_f;
			int64_t rvf = g_dec.result_voiced_f;
			pthread_mutex_unlock(&g_dec.mtx);

			if (rtext[0] != '\0' && rsf == stream->window_start_f) {
				decoded_voiced_f = rvf;
				int win_dur_ms = (ref > rsf) ? (int)((ref - rsf) * FRAME_MS) : 0;
				if (rn_words == 0) {
					rn_words = jarvis_stream_words_from_text(rtext, win_dur_ms,
					                                         scratch_words,
					                                         JARVIS_STT_MAX_WORDS);
				}
				jarvis_stream_update_partial(stream, scratch_words, rn_words, win_dur_ms);

				char next_preview[512];
				jarvis_stream_format_preview(stream, next_preview, sizeof(next_preview));
				if (next_preview[0] != '\0' && strcmp(live_preview, next_preview) != 0) {
					snprintf(live_preview, sizeof(live_preview), "%s", next_preview);
					write_state_file(state_file, state_prefix, live_preview, osd_title);
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
			int sl = (int)(scan_f % RING_FRAMES);
			float db = g_audio.db[sl];
			float ns = g_audio.near_score[sl];
			float vp = g_audio.vad_prob[sl];
			uint8_t tts_hot = g_audio.tts_active[sl];
			int sp_on = g_audio.stereo_spatial;
			if (scan_f < ignore_until_f) {
				scan_f++;
				continue;
			}
			if (!in_speech) {
				double amb_avg = g_audio.noise_floor_db;
				double amb_std = g_audio.ambient_std_db;
				calc_ambient_window_locked(scan_f - consec_voiced, 80, &amb_avg, &amb_std);
				double eff_start_db = calc_vad_start_db(amb_avg, amb_std);
				if (frame_is_voiced_start(db, ns, vp, sp_on, tts_hot,
				                          eff_start_db, 0.45f, 0.45f)) {
					consec_voiced++;
					if (consec_voiced >= 2) {
						in_speech = 1;
						g_audio.in_speech_hint = 1;
						voiced_frames = consec_voiced;
						silence_frames = 0;
						peak_speech_db = db;
						utter_amb_avg = amb_avg;
						utter_amb_std = amb_std;
						utter_start_db = eff_start_db;
						speech_start_f = (scan_f > preroll_f) ? (scan_f - preroll_f) : 0;
						if (speech_start_f < min_preroll_f)
							speech_start_f = min_preroll_f;
						last_voiced_f = scan_f;
						jarvis_stream_init(stream, speech_start_f, FRAME_MS);
					}
				} else {
					consec_voiced = 0;
				}
			} else {
				if (db > peak_speech_db)
					peak_speech_db = db;

				double eff_keep_db = calc_vad_keep_db(utter_amb_avg, utter_amb_std,
				                                      utter_start_db, peak_speech_db);

				if (frame_is_voiced_keep(db, ns, vp, sp_on, eff_keep_db)) {
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
				free(stream);
				free(scratch_words);
				jarvis_plat_write_sock_all(cli, "TIMEOUT\n", 8);
				return;
			}
			continue;
		}

		/* Check auto-endpointing */
		if (silence_ms > 0 && (silence_frames * FRAME_MS) >= silence_ms)
			break;
		if (is_cmd && (scan_f - speech_start_f) >= MAX_UTTER_FRAMES - 10)
			break;
		if (!is_cmd && stream->committed_len >= JARVIS_STREAM_COMMITTED_CAP - 1024)
			break;

		/* Ring-buffer safety guard for unlimited-duration dictation */
		if (scan_f - stream->window_start_f > RING_FRAMES - 100)
			stream->window_start_f = scan_f - 400;

		/* Periodic live preview while speaking (sliding active window from stream->window_start_f) */
		if (silence_frames < 5 && (scan_f - stream->window_start_f) >= 16 &&
		    last_voiced_f > decoded_voiced_f + 10) {
			pthread_mutex_lock(&g_dec.mtx);
			int can_run = (!g_dec.busy && !g_dec.has_job &&
			               (scan_f - g_dec.done_at_frame) >= 15);
			pthread_mutex_unlock(&g_dec.mtx);
			if (can_run) {
				int64_t end_f = last_voiced_f + 6;
				if (end_f > scan_f)
					end_f = scan_f;
				char win_prompt[256] = {0};
				int use_ctx = 0;
				if (is_cmd) {
					if (stream->prompt_tail[0]) {
						snprintf(win_prompt, sizeof(win_prompt), "%s", stream->prompt_tail);
						use_ctx = 1;
					} else if (cmd_prompt && *cmd_prompt) {
						snprintf(win_prompt, sizeof(win_prompt), "%s", cmd_prompt);
					}
				} else {
					double snr_db = peak_speech_db - utter_amb_avg;
					jarvis_stream_build_prompt(stream, dictation_seed, eff_context,
					                           snr_db, voiced_frames,
					                           win_prompt, sizeof(win_prompt));
					use_ctx = (stream->prompt_tail[0] != '\0') ? 1 : 0;
				}
				decode_try_submit_ex(stream->window_start_f, end_f, last_voiced_f, lang,
				                     win_prompt[0] ? win_prompt : NULL, max_tok, 1,
				                     use_ctx, 1);
			}
		}
	}

	if (g_cancel_req) {
		decode_reset_session();
		audio_release(WARM_HOLD_MS);
		free(stream);
		free(scratch_words);
		jarvis_plat_write_sock_all(cli, "CANCELLED\n", 10);
		return;
	}

	if (!in_speech && g_stop_req) {
		pthread_mutex_lock(&g_audio.mtx);
		int64_t cur_f = g_audio.total_frames;
		pthread_mutex_unlock(&g_audio.mtx);
		speech_start_f = (cur_f > 25) ? (cur_f - 25) : 0;
		last_voiced_f = cur_f;
		voiced_frames = 10;
		jarvis_stream_init(stream, speech_start_f, FRAME_MS);
	}

	if (voiced_frames * FRAME_MS < 160 && !g_stop_req && stream->committed_len == 0) {
		decode_reset_session();
		audio_release(WARM_HOLD_MS);
		free(stream);
		free(scratch_words);
		jarvis_plat_write_sock_all(cli, "EMPTY\n", 6);
		return;
	}

	/* Authoritative final pass covering only the uncommitted tail [window_start_f .. last_voiced_f + 360ms] */
	{
		int64_t start_f = stream->window_start_f;
		int64_t end_f = last_voiced_f + 18;
		if (end_f > start_f + 6) {
			char fin_prompt[256] = {0};
			int use_ctx = 0;
			if (is_cmd) {
				if (stream->prompt_tail[0]) {
					snprintf(fin_prompt, sizeof(fin_prompt), "%s", stream->prompt_tail);
					use_ctx = 1;
				} else if (cmd_prompt && *cmd_prompt) {
					snprintf(fin_prompt, sizeof(fin_prompt), "%s", cmd_prompt);
				}
			} else {
				double snr_db = peak_speech_db - utter_amb_avg;
				jarvis_stream_build_prompt(stream, dictation_seed, eff_context,
				                           snr_db, voiced_frames,
				                           fin_prompt, sizeof(fin_prompt));
				use_ctx = (stream->prompt_tail[0] != '\0') ? 1 : 0;
			}
			decode_force_submit_final_ex(start_f, end_f, last_voiced_f, lang,
			                             fin_prompt[0] ? fin_prompt : NULL, 0,
			                             use_ctx, 1);

			pthread_mutex_lock(&g_dec.mtx);
			while ((g_dec.busy || g_dec.has_job) && running && !g_cancel_req)
				pthread_cond_wait(&g_dec.cond, &g_dec.mtx);
			char ftext[4096] = {0};
			int fn_words = 0;
			int64_t fsf = -1, fef = -1;
			if (g_dec.has_result) {
				snprintf(ftext, sizeof(ftext), "%s", g_dec.result_text);
				fn_words = g_dec.result_n_words;
				if (fn_words > JARVIS_STT_MAX_WORDS)
					fn_words = JARVIS_STT_MAX_WORDS;
				if (fn_words > 0) {
					memcpy(scratch_words, g_dec.result_words,
					       (size_t)fn_words * sizeof(jarvis_stt_word_t));
				}
				fsf = g_dec.result_start_f;
				fef = g_dec.result_end_f;
				g_dec.has_result = 0;
			}
			pthread_mutex_unlock(&g_dec.mtx);

			int fdur_ms = (fef > fsf) ? (int)((fef - fsf) * FRAME_MS) : (int)((end_f - start_f) * FRAME_MS);
			if (fn_words == 0 && ftext[0] != '\0') {
				fn_words = jarvis_stream_words_from_text(ftext, fdur_ms,
				                                         scratch_words,
				                                         JARVIS_STT_MAX_WORDS);
			}
			jarvis_stream_finalize(stream, fn_words > 0 ? scratch_words : NULL, fn_words, fdur_ms);
		} else {
			jarvis_stream_finalize(stream, NULL, 0, 0);
		}
	}

	audio_release(WARM_HOLD_MS);

	if (stream->committed_len == 0) {
		jarvis_plat_write_sock_all(cli, "EMPTY\n", 6);
	} else {
		size_t rsz = stream->committed_len + 8;
		char *reply = malloc(rsz);
		if (reply) {
			int n = snprintf(reply, rsz, "OK|%s\n", stream->committed);
			if (n > 0)
				jarvis_plat_write_sock_all(cli, reply, (size_t)n);
			free(reply);
		} else {
			jarvis_plat_write_sock_all(cli, "OK|", 3);
			jarvis_plat_write_sock_all(cli, stream->committed, stream->committed_len);
			jarvis_plat_write_sock_all(cli, "\n", 1);
		}
	}

	free(stream);
	free(scratch_words);
}

/* Always-On Streaming Jarvis Wake + Command handler (with optional follow-up window):
 *   JARVIS_WAIT|<lang>|<thresh_db>|<cmd_silence_ms>|<wake_word>|<state_file>|<chime_wav>[|<followup_ms>]
 */
static long
calc_post_wake_deadline(long timeout_ms)
{
	return (timeout_ms > 0) ? (now_ms() + timeout_ms) : 0;
}

static int
handle_jarvis_wait_ex(stl_socket_t srv, stl_socket_t cli, jarvis_stt_ctx_t *ctx_wake, int n_threads,
                      const char *lang, double thresh_db, int cmd_silence_ms,
                      const char *wake_word, const char *state_file, const char *chime_wav,
                      int followup_ms, long post_wake_timeout_ms, int operating_mode,
                      char *out_cmd, size_t out_sz)
{
	int preroll_f = 15; /* 300 ms pre-roll so soft initial consonant of "Jarvis" is never clipped */
	if (out_cmd && out_sz > 0)
		out_cmd[0] = '\0';
	float *wake_pcm = malloc((size_t)MAX_UTTER_FRAMES * FRAME_SAMPLES * sizeof(float));
	if (!wake_pcm) {
		if (cli != STL_INVALID_SOCKET)
			jarvis_plat_write_sock_all(cli, "EMPTY\n", 6);
		return 2;
	}

	pthread_mutex_lock(&g_ctrl.mtx);
	g_stop_req = 0;
	g_cancel_req = 0;
	int entry_abort = (cli == STL_INVALID_SOCKET &&
	                   (!g_jarvis_active || (!operating_mode && g_ctrl.pending_cli != STL_INVALID_SOCKET)));
	if (!entry_abort)
		g_ctrl.can_preempt_wait = (cli == STL_INVALID_SOCKET && !operating_mode) ? 1 : 0;
	pthread_mutex_unlock(&g_ctrl.mtx);
	if (entry_abort) {
		free(wake_pcm);
		return 130;
	}
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

	int is_followup_window = (!operating_mode && followup_ms > 0) ? 1 : 0;
	int followup_persistent = (is_followup_window && followup_ms >= JARVIS_FOLLOWUP_PERSISTENT) ? 1 : 0;
	int followup_armed = (is_followup_window && !is_tts_audio_hot()) ? 1 : 0;
	int steer_req_wake = operating_mode ? jarvis_rc_get_int("steer_require_wake", 0) : 1;
	int wake_confirmed = followup_armed;
	int waiting_post_wake = followup_armed;
	int waiting_wake_ack = 0;
	long wake_ack_start_ms = 0;
	long wake_ack_done_ms = 0;
	char wake_detected_lang[16] = {0};
	int wake_only_pending = 0;
	int64_t wake_only_voiced_f = -1;
	int chime_played = followup_armed;
	int non_jarvis_utter = 0;
	long post_wake_deadline_ms = (followup_armed && !followup_persistent)
	                           ? (now_ms() + (long)followup_ms) : 0;
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
	if (followup_armed) {
		chime_ignore_until_f = scan_f + 8;
		wake_end_f = chime_ignore_until_f;
		cmd_start_f = chime_ignore_until_f;
	}
	int64_t decoded_wake_voiced_f = -1;
	int decoded_wake_paused = 0;
	int64_t decoded_cmd_voiced_f = -1;
	int64_t decoded_cmd_end_f = -1;
	int decoded_cmd_is_final = 0;
	int live_cmd_from_main = 0;
	int tts_was_active_in_utter = is_tts_currently_speaking(NULL, 0) ? 1 : 0;
	char live_cmd[4096] = {0};

	if (followup_armed) {
		g_in_followup = 1;
		write_state_file(state_file, "jarvis-cmd", NULL, NULL);
	} else if (is_followup_window) {
		g_in_followup = 0;
		update_idle_voice_state(state_file);
	}

	int eff_silence_ms = operating_mode ? jarvis_rc_get_int("steer_silence_ms", 620) : cmd_silence_ms;
	int min_silence_ms = operating_mode ? 440 : 560;
	if (eff_silence_ms < min_silence_ms)
		eff_silence_ms = min_silence_ms;

	int max_utter_ms = jarvis_rc_get_int("max_utter_ms", 60000);
	if (max_utter_ms < 5000)
		max_utter_ms = 5000;
	if (max_utter_ms > MAX_UTTER_FRAMES * FRAME_MS)
		max_utter_ms = MAX_UTTER_FRAMES * FRAME_MS;
	int64_t max_utter_frames = (int64_t)(max_utter_ms / FRAME_MS);
	int spec_tail_f = operating_mode ? 12 : 15;
	int final_tail_f = operating_mode ? 12 : 18;
	int soft_limit_f = (eff_silence_ms / FRAME_MS) + (operating_mode ? 10 : 18);
	int ptt_session = 0;
	float last_frame_vad_prob = -1.0f;
	int last_frame_vad_neural = 0;
	if (g_kws && !wake_confirmed)
		jarvis_kws_reset(g_kws);

	while (running && !g_stop_req && !g_cancel_req && (cli != STL_INVALID_SOCKET || g_jarvis_active)) {
		if (operating_mode) {
			pthread_mutex_lock(&g_op_turn.mtx);
			int op_active = g_op_turn.active;
			pthread_mutex_unlock(&g_op_turn.mtx);
			if (!op_active && !wake_confirmed && (!in_speech || tts_was_active_in_utter)) {
				set_can_preempt_wait(0);
				decode_reset_session();
				audio_release(WARM_HOLD_MS);
				free(wake_pcm);
				return 3;
			}
		}

		poll_control_socket(srv, cli, (!operating_mode) && (!wake_confirmed || waiting_post_wake));
		if (g_stop_req || g_cancel_req || (cli == STL_INVALID_SOCKET && !g_jarvis_active))
			break;

		/* Push-to-Talk (JARVIS_PTT_START): immediately enter active command capture
		 * with zero wake-word overhead and hold open until JARVIS_PTT_STOP. */
		if (!wake_confirmed && g_ptt_active && !operating_mode) {
			interrupt_tts_and_clear_cache();
			is_followup_window = 0;
			followup_armed = 0;
			g_in_followup = 0;
			g_in_post_wake = 0;
			wake_confirmed = 1;
			waiting_post_wake = 0;
			chime_played = 1;
			ptt_session = 1;
			in_speech = 1;
			speech_start_f = (scan_f > preroll_f) ? (scan_f - preroll_f) : 0;
			cmd_start_f = speech_start_f;
			voiced_frames = 10;
			silence_frames = 0;
			trailing_soft_frames = 0;
			last_voiced_f = scan_f;
			decoded_cmd_voiced_f = -1;
			decoded_cmd_is_final = 0;
			live_cmd[0] = '\0';
			write_state_file(state_file, "jarvis-cmd", NULL, NULL);
			pthread_mutex_lock(&g_audio.mtx);
			g_audio.in_speech_hint = 1;
			pthread_mutex_unlock(&g_audio.mtx);
		}

		wait_next_audio_frame(scan_f + 1, 20);

		/* While Jarvis speaks its short wake acknowledgment ("Yes?" / "Sim?"),
		 * fast-forward the VAD cursor past all playback + short acoustic drain
		 * frames so the spoken acknowledgment never triggers post-wake VAD or
		 * enters Whisper, and defer external-media ducking until after playback.
		 */
		if (waiting_wake_ack) {
			long now = now_ms();
			post_wake_deadline_ms = calc_post_wake_deadline(post_wake_timeout_ms);
			pthread_mutex_lock(&g_audio.mtx);
			scan_f = g_audio.total_frames;
			wake_end_f = scan_f;
			chime_ignore_until_f = scan_f;
			pthread_mutex_unlock(&g_audio.mtx);
			consec_voiced = 0;

			if (!g_tts_speaking && !is_tts_file_active()) {
				if (wake_ack_done_ms == 0)
					wake_ack_done_ms = now;
			} else {
				wake_ack_done_ms = 0;
			}

			if ((wake_ack_done_ms > 0 && (now - wake_ack_done_ms) >= WAKE_ACK_DRAIN_MS) ||
			    (now - wake_ack_start_ms) >= 2500L) {
				waiting_wake_ack = 0;
				wake_ack_done_ms = 0;
				tts_was_active_in_utter = 0;
				clear_tts_speaking_cache();
				pthread_mutex_lock(&g_audio.mtx);
				scan_f = g_audio.total_frames;
				wake_end_f = scan_f;
				chime_ignore_until_f = scan_f;
				pthread_mutex_unlock(&g_audio.mtx);
				if (post_wake_timeout_ms > 0 && jarvis_audio_is_external_playing())
					duck_speaker_for_command();
				write_state_file(state_file, "jarvis-cmd", NULL, NULL);
			}
			continue;
		}

		/* Never run wake-word-free follow-up VAD while Jarvis's own TTS is playing
		 * or draining through PipeWire/room reverb. Arm the follow-up window cleanly
		 * once TTS finishes + TTS_DRAIN_COOLDOWN_MS, fast-forwarding past all TTS frames.
		 */
		if (is_followup_window) {
			int tts_hot = is_tts_audio_hot();
			if (!followup_armed && !wake_confirmed && !tts_hot) {
				followup_armed = 1;
				wake_confirmed = 1;
				waiting_post_wake = 1;
				chime_played = 1;
				in_speech = 0;
				consec_voiced = 0;
				voiced_frames = 0;
				silence_frames = 0;
				trailing_soft_frames = 0;
				decoded_wake_voiced_f = -1;
				decoded_wake_paused = 0;
				decoded_cmd_voiced_f = -1;
				decoded_cmd_end_f = -1;
				decoded_cmd_is_final = 0;
				live_cmd[0] = '\0';
				live_cmd_from_main = 0;
				tts_was_active_in_utter = 0;
				pthread_mutex_lock(&g_audio.mtx);
				g_audio.in_speech_hint = 0;
				chime_ignore_until_f = g_audio.total_frames;
				wake_end_f = chime_ignore_until_f;
				cmd_start_f = chime_ignore_until_f;
				scan_f = chime_ignore_until_f;
				pthread_mutex_unlock(&g_audio.mtx);
				post_wake_deadline_ms = followup_persistent ? 0 : (now_ms() + (long)followup_ms);
				g_in_followup = 1;
				write_state_file(state_file, "jarvis-cmd", NULL, NULL);
			} else if (followup_armed && waiting_post_wake && tts_hot) {
				followup_armed = 0;
				wake_confirmed = 0;
				waiting_post_wake = 0;
				chime_played = 0;
				consec_voiced = 0;
				g_in_followup = 0;
				update_idle_voice_state(state_file);
			}
		}

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
						if (tts_on || tts_was_active_in_utter) {
							if (operating_mode && (is_bargein_stop_phrase(raw_res) || is_op_abort_phrase(raw_res))) {
								interrupt_tts_and_clear_cache();
								candidate = raw_res;
							} else if (!operating_mode && is_bargein_stop_phrase(raw_res)) {
								interrupt_tts_and_clear_cache();
								tts_was_active_in_utter = 0;
								live_cmd[0] = '\0';
							}
							/* Never accept wake-word-free speech overlapping TTS:
							 * AEC-distorted speaker bleed transcribes as garbled
							 * words that defeat text-overlap echo checks.
							 */
						} else {
							candidate = raw_res;
						}
					}

					decoded_cmd_voiced_f = res_vf;
					decoded_cmd_end_f = res_ef;
					if (candidate && *candidate) {
						live_cmd_from_main = 1;
						if (!res_part && res_vf >= last_voiced_f && res_ef >= last_voiced_f + spec_tail_f)
							decoded_cmd_is_final = 1;
						if (strcmp(live_cmd, candidate) != 0) {
							snprintf(live_cmd, sizeof(live_cmd), "%s", candidate);
							char cmd_live_hdr[128];
							jarvis_ontology_format_ui(get_daemon_ont(), "command_live", wake_word,
							                          "  {Wake} Command (Live)...", cmd_live_hdr, sizeof(cmd_live_hdr));
							write_state_file(state_file, "cmd", live_cmd, cmd_live_hdr);
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
		int64_t kws_batch_start = scan_f;
		last_frame_vad_neural = g_audio.vad_neural;

		while (scan_f < avail_f) {
			int sl = (int)(scan_f % RING_FRAMES);
			float db = g_audio.db[sl];
			float ns = g_audio.near_score[sl];
			float vp = g_audio.vad_prob[sl];
			uint8_t tts_hot = g_audio.tts_active[sl];
			int sp_on = g_audio.stereo_spatial;
			last_frame_vad_prob = vp;

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
				 * with a 15-frame (300ms) pre-roll so normal or soft clarifications
				 * are captured immediately from the first consonant.
				 */
				double eff_start_db = calc_vad_start_db(amb_avg, amb_std) - 1.0;
				if (frame_is_voiced_start(db, ns, vp, sp_on, tts_hot,
				                          eff_start_db, 0.36f, 0.38f)) {
					consec_voiced++;
					if (consec_voiced >= 2) {
						waiting_post_wake = 0;
						g_in_post_wake = 0;
						in_speech = 1;
						g_audio.in_speech_hint = 1;
						tts_was_active_in_utter = (tts_now_hint || tts_hot) ? 1 : 0;
						voiced_frames = consec_voiced;
						silence_frames = 0;
						trailing_soft_frames = 0;
						peak_speech_db = db;
						utter_amb_avg = amb_avg;
						utter_amb_std = amb_std;
						utter_start_db = eff_start_db;
						cmd_start_f = (scan_f > preroll_f) ? (scan_f - preroll_f) : 0;
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
				if (frame_is_voiced_start(db, ns, vp, sp_on, tts_hot,
				                          eff_start_db, 0.45f, 0.45f)) {
					consec_voiced++;
					if (consec_voiced >= 2) {
						in_speech = 1;
						g_audio.in_speech_hint = 1;
						tts_was_active_in_utter = (tts_now_hint || tts_hot) ? 1 : 0;
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
						decoded_wake_paused = 0;
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
					    g_audio.db[(scan_f - 1) % RING_FRAMES] >= recent_avg + 4.5 &&
					    (!sp_on || ns >= 0.45f) &&
					    (vp < 0.0f || vp >= 0.45f)) {
						speech_start_f = (scan_f > preroll_f) ? (scan_f - preroll_f) : 0;
						cmd_start_f = speech_start_f;
						voiced_frames = 2;
						silence_frames = 0;
						trailing_soft_frames = 0;
						peak_speech_db = db;
						utter_amb_avg = recent_avg;
						utter_start_db = recent_avg + 4.5;
						decoded_wake_voiced_f = -1;
						decoded_wake_paused = 0;
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

				if (frame_is_voiced_keep(db, ns, vp, sp_on, eff_keep_db)) {
					voiced_frames++;
					silence_frames = 0;
					last_voiced_f = scan_f;
					decoded_wake_paused = 0;
					decoded_cmd_is_final = 0;
					if (db >= peak_speech_db - 16.0 || db >= utter_start_db) {
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

		/* Tier 2 sub-millisecond ONNX KWS (when a matching <wake>.onnx model is installed).
		 * Runs outside g_audio.mtx and gates on Silero VAD inside jarvis_kws_process_frame(). */
		if (!wake_confirmed && !tts_now_hint && !operating_mode &&
		    g_kws && jarvis_kws_tier(g_kws) == JARVIS_KWS_TIER_ONNX &&
		    scan_f > kws_batch_start) {
			int64_t kf_end = scan_f;
			int64_t kf_start = kws_batch_start;
			if (kf_end - kf_start > 8)
				kf_start = kf_end - 8;
			float kws_frames[8][FRAME_SAMPLES];
			float kws_vads[8];
			int kws_n = 0;
			int kws_vad_on = 0;
			pthread_mutex_lock(&g_audio.mtx);
			kws_vad_on = g_audio.vad_neural;
			for (int64_t f = kf_start; f < kf_end && kws_n < 8; f++) {
				int sl = (int)(f % RING_FRAMES);
				if (g_audio.tts_active[sl])
					continue;
				memcpy(kws_frames[kws_n], g_audio.pcm + (size_t)sl * FRAME_SAMPLES,
				       (size_t)FRAME_SAMPLES * sizeof(float));
				kws_vads[kws_n] = g_audio.vad_prob[sl];
				kws_n++;
			}
			pthread_mutex_unlock(&g_audio.mtx);
			int kws_hit = 0;
			for (int ki = 0; ki < kws_n; ki++) {
				if (jarvis_kws_process_frame(g_kws, kws_frames[ki], FRAME_SAMPLES,
				                             kws_vads[ki], kws_vad_on, NULL) > 0) {
					kws_hit = 1;
					break;
				}
			}
			if (kws_hit) {
				interrupt_tts_and_clear_cache();
				is_followup_window = 0;
				followup_armed = 0;
				tts_was_active_in_utter = 0;
				wake_confirmed = 1;
				wake_only_pending = 0;
				chime_played = 1;
				g_in_post_wake = 1;
				int spoke_ack = trigger_wake_ack_response(lang, NULL, chime_wav);
				waiting_post_wake = 1;
				in_speech = 0;
				consec_voiced = 0;
				voiced_frames = 0;
				silence_frames = 0;
				trailing_soft_frames = 0;
				wake_end_f = scan_f;
				chime_ignore_until_f = scan_f + (spoke_ack ? 18 : 12);
				post_wake_deadline_ms = now_ms() + (spoke_ack ? 14000 : 12000);
				write_state_file(state_file, "jarvis-cmd", NULL, NULL);
				pthread_mutex_lock(&g_audio.mtx);
				g_audio.in_speech_hint = 0;
				pthread_mutex_unlock(&g_audio.mtx);
				continue;
			}
		}

		if (wake_confirmed && waiting_post_wake) {
			if (is_followup_window && post_wake_deadline_ms > 0 &&
			    (is_tts_file_active() || is_tts_currently_speaking(NULL, 0))) {
				long ext_dl = now_ms() + (long)followup_ms;
				if (ext_dl > post_wake_deadline_ms)
					post_wake_deadline_ms = ext_dl;
			}
			if (post_wake_deadline_ms > 0 && now_ms() >= post_wake_deadline_ms) {
				if (is_followup_window) {
					is_followup_window = 0;
					followup_armed = 0;
					g_in_followup = 0;
					g_in_post_wake = 0;
					wake_confirmed = 0;
					waiting_post_wake = 0;
					chime_played = 0;
					update_idle_voice_state(state_file);
					continue;
				}
				set_can_preempt_wait(0);
				restore_speaker_after_command(NULL);
				decode_reset_session();
				audio_release(WARM_HOLD_MS);
				free(wake_pcm);
				g_in_followup = 0;
				g_in_post_wake = 0;
				update_idle_voice_state(state_file);
				if (cli != STL_INVALID_SOCKET)
					jarvis_plat_write_sock_all(cli, "TIMEOUT\n", 8);
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

			/* 1. Pause wake check: user spoke "Jarvis" (or "Jarvis <cmd>") and paused >= 140ms (7 frames)
			 *    after at least 10 voiced frames (200ms) or >= 240ms silence after shorter speech.
			 * 2. Continuous wake check: user has spoken continuously for >= 16 voiced frames (320ms)
			 *    without pausing (and repeats every 16 new voiced frames), so the indicator turns RED
			 *    immediately during a one-shot "<wake> <command>" utterance!
			 */
			char active_tts_now[2048] = {0};
			int tts_speaking_now = is_tts_currently_speaking(active_tts_now, sizeof(active_tts_now));
			if (!tts_speaking_now && tts_was_active_in_utter) {
				tts_speaking_now = 1;
				if (g_last_tts_cache[0])
					snprintf(active_tts_now, sizeof(active_tts_now), "%s", g_last_tts_cache);
			}
			int cont_step_f = tts_speaking_now ? 14 : 16;
			int pause_wake_check = (((silence_frames >= 7 && voiced_frames >= 10) ||
			                         (silence_frames >= 12 && voiced_frames >= 5)) &&
			                        !decoded_wake_paused &&
			                        (!tts_speaking_now || peak_speech_db >= utter_start_db + 8.0));
			int cont_wake_check  = (voiced_frames >= cont_step_f && silence_frames < 7 &&
			                        (!tts_speaking_now || peak_speech_db >= utter_start_db + 5.0) &&
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

				/* Tier 1 Silero VAD speech-segment gate: skip ctx_wake STT decode unless
				 * the candidate slice has genuine neural speech (peak >= 0.55, >= 3 frames).
				 * Drops idle GPU/CPU wake invocations on ambient noise & typing to 0. */
				float slice_vad[80];
				int n_slice_vad = 0;
				int slice_vad_neural = 0;
				pthread_mutex_lock(&g_audio.mtx);
				slice_vad_neural = g_audio.vad_neural;
				for (int64_t vf = wake_slice_start; vf < end_f && n_slice_vad < 80; vf++)
					slice_vad[n_slice_vad++] = g_audio.vad_prob[vf % RING_FRAMES];
				pthread_mutex_unlock(&g_audio.mtx);
				if (!jarvis_kws_vad_gate_slice(slice_vad, n_slice_vad, slice_vad_neural, 0.55f, 3)) {
					decoded_wake_voiced_f = snap_voiced_f;
					if (pause_wake_check)
						decoded_wake_paused = 1;
					continue;
				}

				int n_samp = ring_extract_pcm(wake_slice_start, end_f, wake_pcm,
				                              MAX_UTTER_FRAMES * FRAME_SAMPLES);
				if (n_samp >= 2400) {
					char heard[1024];
					char inline_cmd[1024];
					char wake_prompt_buf[128];
					const char *wake_prompt = build_wake_initial_prompt(wake_word, wake_prompt_buf, sizeof(wake_prompt_buf));
					int w_tok = cont_wake_check ? 16 : 28;
					int wake_all_silent = 0;
					g_wake_stt_calls++;
					run_whisper_slice_ex(ctx_wake, n_threads, wake_pcm, n_samp,
					                     lang, wake_prompt, w_tok, 1, 0, 1,
					                     heard, sizeof(heard), &wake_all_silent,
					                     wake_detected_lang, sizeof(wake_detected_lang));
					if (g_cancel_req || (cli == STL_INVALID_SOCKET && !g_jarvis_active))
						break;
					decoded_wake_voiced_f = snap_voiced_f;
					if (pause_wake_check)
						decoded_wake_paused = 1;

					int wmatch = match_wake_prefix(heard, wake_word,
					                               inline_cmd, sizeof(inline_cmd));
					if (wmatch == 0) {
						wmatch = match_wake_anywhere(heard, wake_word,
						                             inline_cmd, sizeof(inline_cmd));
					}
					if (tts_speaking_now && wmatch >= 1 && is_tts_self_echo(heard, active_tts_now)) {
						wmatch = 0;
						inline_cmd[0] = '\0';
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
						int sl = (int)(f % RING_FRAMES);
						if (g_audio.db[sl] >= speech_cont_db &&
						    (!g_audio.stereo_spatial || g_audio.near_score[sl] >= 0.30f) &&
						    (g_audio.vad_prob[sl] < 0.0f || g_audio.vad_prob[sl] >= 0.25f))
							v_during++;
					}
					if (v_during >= 3)
						spoke_during_decode = 1;
					pthread_mutex_unlock(&g_audio.mtx);

					if (wmatch == 1 && !cont_wake_check && !spoke_during_decode) {
						/* User spoke ONLY wake word and genuinely paused: interrupt any TTS,
						 * answer with language-appropriate wake ack ("Yes?"/"Sim?"/etc.) or
						 * fall back to chime, and wait cleanly for the command.
						 */
						interrupt_tts_and_clear_cache();
						is_followup_window = 0;
						followup_armed = 0;
						tts_was_active_in_utter = 0;
						wake_confirmed = 1;
						wake_only_pending = 0;
						chime_played = 1;
						g_in_post_wake = 1;
						int spoke_ack = trigger_wake_ack_response(lang, wake_detected_lang, chime_wav);
						waiting_post_wake = 1;
						in_speech = 0;
						consec_voiced = 0;
						voiced_frames = 0;
						silence_frames = 0;
						pthread_mutex_lock(&g_audio.mtx);
						g_audio.in_speech_hint = 0;
						if (spoke_ack) {
							waiting_wake_ack = 1;
							wake_ack_start_ms = now_ms();
							wake_ack_done_ms = 0;
							chime_ignore_until_f = g_audio.total_frames;
							wake_end_f = g_audio.total_frames;
						} else {
							chime_ignore_until_f = g_audio.total_frames + 5;
							wake_end_f = g_audio.total_frames + 2;
						}
						pthread_mutex_unlock(&g_audio.mtx);
						if (!spoke_ack && post_wake_timeout_ms > 0)
							duck_speaker_for_command();
						post_wake_deadline_ms = calc_post_wake_deadline(post_wake_timeout_ms);
						write_state_file(state_file, "jarvis-cmd", NULL, NULL);
						char ltitle[128], lbody[256];
						jarvis_ontology_format_ui(get_daemon_ont(), "listening_title", wake_word,
						                          "  {Wake} Listening...", ltitle, sizeof(ltitle));
						jarvis_ontology_format_ui(get_daemon_ont(), "listening_body", wake_word,
						                          "Speak your command now.", lbody, sizeof(lbody));
						notify_osd_async(ltitle, lbody, 4500);
						continue;
					} else if (wmatch >= 1) {
						/* User spoke wake word (and either kept speaking or cont_wake_check fired
						 * near the tail of the wake word). Interrupt any active TTS immediately.
						 * When wmatch == 1 (wake-only so far), mark wake_only_pending so a quick
						 * pause (~140ms) fires the cached wake ack immediately without waiting
						 * eff_silence_ms (750ms) + slow ctx_main beam-search!
						 */
						interrupt_tts_and_clear_cache();
						is_followup_window = 0;
						followup_armed = 0;
						wake_confirmed = 1;
						waiting_post_wake = 0;
						chime_played = 0;
						if (wmatch == 1) {
							wake_only_pending = 1;
							wake_only_voiced_f = snap_voiced_f;
							post_wake_deadline_ms = calc_post_wake_deadline(post_wake_timeout_ms);
						} else {
							wake_only_pending = 0;
							jarvis_tts_speak_clause("warmup", "", 0);
							if (jarvis_audio_is_external_playing())
								duck_speaker_for_command();
						}
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
							char cmd_live_hdr[128];
							jarvis_ontology_format_ui(get_daemon_ont(), "command_live", wake_word,
							                          "  {Wake} Command (Live)...", cmd_live_hdr, sizeof(cmd_live_hdr));
							write_state_file(state_file, "cmd", live_cmd, cmd_live_hdr);
						} else {
							live_cmd[0] = '\0';
							write_state_file(state_file, "jarvis-cmd", NULL, NULL);
						}
					} else if (operating_mode && !spoke_during_decode &&
					           (is_op_abort_phrase(heard) || is_bargein_stop_phrase(heard))) {
						interrupt_tts_and_clear_cache();
						if (is_op_abort_phrase(heard)) {
							snprintf(live_cmd, sizeof(live_cmd), "%s", heard);
							break;
						}
						in_speech = 0;
						consec_voiced = 0;
						voiced_frames = 0;
						silence_frames = 0;
						decoded_wake_voiced_f = -1;
						decoded_wake_paused = 0;
						pthread_mutex_lock(&g_audio.mtx);
						g_audio.in_speech_hint = 0;
						pthread_mutex_unlock(&g_audio.mtx);
						continue;
					} else if (tts_speaking_now && is_bargein_stop_phrase(heard)) {
						interrupt_tts_and_clear_cache();
						if (is_followup_window && !followup_persistent) {
							is_followup_window = 0;
							followup_armed = 0;
							g_in_followup = 0;
							update_idle_voice_state(state_file);
						}
						in_speech = 0;
						consec_voiced = 0;
						voiced_frames = 0;
						silence_frames = 0;
						decoded_wake_voiced_f = -1;
						decoded_wake_paused = 0;
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
						decoded_wake_paused = 0;
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
						decoded_wake_paused = 0;
						pthread_mutex_lock(&g_audio.mtx);
						g_audio.in_speech_hint = 0;
						calc_ambient_window_locked(g_audio.total_frames, 80,
						                           &g_audio.noise_floor_db,
						                           &g_audio.ambient_std_db);
						pthread_mutex_unlock(&g_audio.mtx);
						continue;
					} else if (operating_mode && !steer_req_wake &&
					           !tts_speaking_now && !tts_was_active_in_utter &&
					           voiced_frames >= 10 &&
					           !is_bargein_stop_phrase(heard) && !is_op_abort_phrase(heard)) {
						/* Wake-word-free steering mode (steer_require_wake=0): promote non-echo speech directly
						 * only when TTS is silent (during TTS, distorted speaker bleed must not steer ren).
						 */
						interrupt_tts_and_clear_cache();
						duck_speaker_for_command();
						wake_confirmed = 1;
						waiting_post_wake = 0;
						chime_played = 0;
						cmd_start_f = speech_start_f;
						decoded_cmd_voiced_f = snap_voiced_f;
						snprintf(live_cmd, sizeof(live_cmd), "%s", heard);
						char cmd_live_hdr[128];
						jarvis_ontology_format_ui(get_daemon_ont(), "command_live", wake_word,
						                          "  {Wake} Command (Live)...", cmd_live_hdr, sizeof(cmd_live_hdr));
						write_state_file(state_file, "cmd", live_cmd, cmd_live_hdr);
					} else if (pause_wake_check && !spoke_during_decode &&
					           (voiced_frames >= 20 || silence_frames >= 12)) {
						/* Finished a non-Jarvis phrase and paused: reset cleanly */
						in_speech = 0;
						consec_voiced = 0;
						voiced_frames = 0;
						silence_frames = 0;
						decoded_wake_voiced_f = -1;
						decoded_wake_paused = 0;
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
				decoded_wake_paused = 0;
				pthread_mutex_lock(&g_audio.mtx);
				g_audio.in_speech_hint = 0;
				pthread_mutex_unlock(&g_audio.mtx);
				continue;
			}
		}

		if (wake_confirmed && !waiting_post_wake) {
			/* Fast wake-only resolution: when cont_wake_check matched only the wake
			 * word near its tail, wait just 7 silence frames (140ms) and fire the
			 * cached wake acknowledgment immediately instead of stalling for
			 * eff_silence_ms (750ms) + slow ctx_main beam-search (~420ms).
			 */
			if (wake_only_pending && live_cmd[0] == '\0' && !chime_played) {
				if (silence_frames >= 7) {
					int fast_has_cmd = 0;
					char fast_inline[1024] = {0};
					if (last_voiced_f > wake_only_voiced_f + 10) {
						int64_t fast_end_f = last_voiced_f + 6;
						if (fast_end_f > scan_f)
							fast_end_f = scan_f;
						int n_fast = ring_extract_pcm(cmd_start_f, fast_end_f, wake_pcm,
						                              MAX_UTTER_FRAMES * FRAME_SAMPLES);
						if (n_fast >= 2400) {
							char fast_heard[1024] = {0};
							char fast_prompt_buf[128];
							const char *fast_prompt = build_wake_initial_prompt(wake_word,
							                                                    fast_prompt_buf,
							                                                    sizeof(fast_prompt_buf));
							int fast_silent = 0;
							run_whisper_slice_ex(ctx_wake, n_threads, wake_pcm, n_fast,
							                     lang, fast_prompt, 28, 1, 0, 1,
							                     fast_heard, sizeof(fast_heard), &fast_silent,
							                     wake_detected_lang, sizeof(wake_detected_lang));
							if (g_cancel_req || (cli == STL_INVALID_SOCKET && !g_jarvis_active))
								break;
							int wm_fast = match_wake_prefix(fast_heard, wake_word,
							                                fast_inline, sizeof(fast_inline));
							if (wm_fast == 0)
								wm_fast = match_wake_anywhere(fast_heard, wake_word,
								                              fast_inline, sizeof(fast_inline));
							if (wm_fast == 2 && fast_inline[0] != '\0')
								fast_has_cmd = 1;
						}
					}
					if (fast_has_cmd) {
						wake_only_pending = 0;
						jarvis_tts_speak_clause("warmup", "", 0);
						if (jarvis_audio_is_external_playing())
							duck_speaker_for_command();
						snprintf(live_cmd, sizeof(live_cmd), "%s", fast_inline);
						decoded_cmd_voiced_f = last_voiced_f;
						char cmd_live_hdr[128];
						jarvis_ontology_format_ui(get_daemon_ont(), "command_live", wake_word,
						                          "  {Wake} Command (Live)...", cmd_live_hdr, sizeof(cmd_live_hdr));
						write_state_file(state_file, "cmd", live_cmd, cmd_live_hdr);
					} else {
						interrupt_tts_and_clear_cache();
						is_followup_window = 0;
						followup_armed = 0;
						g_in_followup = 0;
						tts_was_active_in_utter = 0;
						wake_only_pending = 0;
						chime_played = 1;
						waiting_post_wake = 1;
						g_in_post_wake = 1;
						int spoke_ack = trigger_wake_ack_response(lang, wake_detected_lang, chime_wav);
						if (!spoke_ack && post_wake_timeout_ms > 0)
							duck_speaker_for_command();
						in_speech = 0;
						consec_voiced = 0;
						voiced_frames = 0;
						silence_frames = 0;
						trailing_soft_frames = 0;
						live_cmd[0] = '\0';
						live_cmd_from_main = 0;
						decoded_cmd_is_final = 0;
						decoded_cmd_end_f = -1;
						pthread_mutex_lock(&g_audio.mtx);
						g_audio.in_speech_hint = 0;
						if (spoke_ack) {
							waiting_wake_ack = 1;
							wake_ack_start_ms = now_ms();
							wake_ack_done_ms = 0;
							chime_ignore_until_f = g_audio.total_frames;
							wake_end_f = g_audio.total_frames;
						} else {
							chime_ignore_until_f = g_audio.total_frames + 5;
							wake_end_f = g_audio.total_frames + 2;
						}
						pthread_mutex_unlock(&g_audio.mtx);
						post_wake_deadline_ms = calc_post_wake_deadline(post_wake_timeout_ms);
						write_state_file(state_file, "jarvis-cmd", NULL, NULL);
						char ltitle[128], lbody[256];
						jarvis_ontology_format_ui(get_daemon_ont(), "listening_title", wake_word,
						                          "  {Wake} Listening...", ltitle, sizeof(ltitle));
						jarvis_ontology_format_ui(get_daemon_ont(), "listening_body", wake_word,
						                          "Speak your command now.", lbody, sizeof(lbody));
						notify_osd_async(ltitle, lbody, 4500);
						continue;
					}
				} else if (last_voiced_f - wake_only_voiced_f >= 22) {
					/* Continuous speech well beyond the wake word: promote to normal
					 * one-shot command streaming.
					 */
					wake_only_pending = 0;
					jarvis_tts_speak_clause("warmup", "", 0);
					if (jarvis_audio_is_external_playing())
						duck_speaker_for_command();
				} else {
					/* Still in wake-word tail: avoid premature ctx_main preview decode */
					continue;
				}
			}

			int64_t cmd_len_f = scan_f - cmd_start_f;
			char eff_prompt_buf[256];
			const char *eff_prompt = chime_played
				? get_cmd_initial_prompt(eff_prompt_buf, sizeof(eff_prompt_buf))
				: build_wake_initial_prompt(wake_word, eff_prompt_buf, sizeof(eff_prompt_buf));

			jarvis_endpoint_in_t ep_in;
			memset(&ep_in, 0, sizeof(ep_in));
			ep_in.eff_silence_ms    = eff_silence_ms;
			ep_in.frame_ms          = FRAME_MS;
			ep_in.operating_mode    = operating_mode;
			ep_in.vad_neural        = last_frame_vad_neural;
			ep_in.last_vad_prob     = last_frame_vad_prob;
			ep_in.ends_continuation = (live_cmd[0] != '\0' &&
			                           jarvis_ontology_ends_with_continuation(get_daemon_ont(), live_cmd)) ? 1 : 0;
			ep_in.ptt_held          = (ptt_session && g_ptt_active) ? 1 : 0;
			jarvis_endpoint_plan_t ep;
			jarvis_endpoint_compute(&ep_in, &ep);
			int ptt_released = (ptt_session && !g_ptt_active);
			int eff_soft_limit_f = soft_limit_f + (ep.grace_ms_applied / FRAME_MS);

			if (silence_frames < 8 && cmd_len_f >= 16 &&
			    last_voiced_f > decoded_cmd_voiced_f + 12) {
				pthread_mutex_lock(&g_dec.mtx);
				int can_run = (!g_dec.busy && !g_dec.has_job &&
				               (scan_f - g_dec.done_at_frame) >= 12);
				pthread_mutex_unlock(&g_dec.mtx);
				if (can_run) {
					int64_t end_f = last_voiced_f + 8;
					if (end_f > scan_f)
						end_f = scan_f;
					decode_try_submit(cmd_start_f, end_f, last_voiced_f,
					                  lang, eff_prompt, 256, 1);
				}
			} else if (!ep.suppress_endpoint &&
			           (silence_frames >= ep.spec_silence_f ||
			            trailing_soft_frames >= (ep.final_silence_ms / FRAME_MS)) &&
			           voiced_frames >= 8 &&
			           (!decoded_cmd_is_final || last_voiced_f > decoded_cmd_voiced_f)) {
				/* Preempt any stale partial decode so speculative final starts immediately */
				decode_try_abort_partial();
				/* Speculative full decode starts after ep.spec_silence_f frames of silence
				 * with a spec_tail_f tail so softer final words are included in full.
				 */
				int64_t end_f = last_voiced_f + spec_tail_f;
				if (end_f > scan_f)
					end_f = scan_f;
				decode_try_submit(cmd_start_f, end_f, last_voiced_f,
				                  lang, eff_prompt, 0, 0);
			}

			int spec_final_ready = (!operating_mode && !ep.suppress_endpoint &&
			                        decoded_cmd_is_final &&
			                        live_cmd[0] != '\0' &&
			                        decoded_cmd_voiced_f >= last_voiced_f - 1 &&
			                        decoded_cmd_end_f >= last_voiced_f + spec_tail_f &&
			                        silence_frames >= ep.spec_early_f);

			/* Endpoint when:
			 * 0. Push-to-Talk key released (JARVIS_PTT_STOP), OR
			 * 1. Speculative final decode completed on matching speech + adaptive ep.spec_early_f silence, OR
			 * 2. Full silence reaches adaptive ep.final_silence_ms (+450ms grace on continuation token), OR
			 * 3. Energy has stayed below utter_start_db and >= 16.0 dB below speech peak
			 *    for eff_soft_limit_f so steady noise never hangs, OR
			 * 4. Utterance reaches configurable max_utter_frames (default 60s, max 90s).
			 */
			if (ptt_released ||
			    (!ep.suppress_endpoint &&
			     (spec_final_ready ||
			      (silence_frames * FRAME_MS) >= ep.final_silence_ms ||
			      (voiced_frames >= 8 && trailing_soft_frames >= eff_soft_limit_f))) ||
			    cmd_len_f >= max_utter_frames) {
				int keep_post_wake = (chime_played && !operating_mode &&
				                      (post_wake_deadline_ms == 0 || now_ms() < post_wake_deadline_ms));
				if (voiced_frames < 8 && live_cmd[0] == '\0') {
					if (keep_post_wake) {
						waiting_post_wake = 1;
						g_in_post_wake = 1;
						in_speech = 0;
						consec_voiced = 0;
						voiced_frames = 0;
						silence_frames = 0;
						trailing_soft_frames = 0;
						restore_speaker_after_command(NULL);
						write_state_file(state_file, "jarvis-cmd", NULL, NULL);
						pthread_mutex_lock(&g_audio.mtx);
						g_audio.in_speech_hint = 0;
						pthread_mutex_unlock(&g_audio.mtx);
						continue;
					}
					break;
				}

				char final_raw[4096] = {0};
				int ran_final_main = 0;
				if (!(decoded_cmd_is_final && live_cmd[0] != '\0' &&
				      decoded_cmd_voiced_f >= last_voiced_f - 1 &&
				      decoded_cmd_end_f >= last_voiced_f + spec_tail_f)) {
					ran_final_main = 1;
					int64_t end_f = last_voiced_f + final_tail_f;
					decode_force_submit_final(cmd_start_f, end_f, last_voiced_f,
					                          lang, eff_prompt, 0);

					pthread_mutex_lock(&g_dec.mtx);
					while ((g_dec.busy || g_dec.has_job) && running && !g_cancel_req &&
					       (cli != STL_INVALID_SOCKET || g_jarvis_active))
						pthread_cond_wait(&g_dec.cond, &g_dec.mtx);
					if (g_dec.has_result) {
						snprintf(final_raw, sizeof(final_raw), "%s", g_dec.result_text);
						g_dec.has_result = 0;
					}
					pthread_mutex_unlock(&g_dec.mtx);
					if (g_cancel_req || (cli == STL_INVALID_SOCKET && !g_jarvis_active))
						break;
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
					int sl = (int)(f % RING_FRAMES);
					if (g_audio.db[sl] >= resume_db &&
					    (!g_audio.stereo_spatial || g_audio.near_score[sl] >= 0.30f) &&
					    (g_audio.vad_prob[sl] < 0.0f || g_audio.vad_prob[sl] >= 0.25f))
						v_after++;
				}
				if (v_after >= 3 && cmd_len_f < max_utter_frames)
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
						live_cmd_from_main = 1;
					} else if (wm == 1 || (!chime_played && live_cmd[0] == '\0' && voiced_frames <= 32 &&
					                       contains_wake_word(final_raw, wake_word))) {
						/* User spoke ONLY wake word and paused: interrupt TTS, answer with
						 * language-appropriate wake ack ("Yes?"/"Sim?"/etc.) or chime fallback,
						 * and wait cleanly for the command.
						 */
						interrupt_tts_and_clear_cache();
						is_followup_window = 0;
						followup_armed = 0;
						g_in_followup = 0;
						tts_was_active_in_utter = 0;
						wake_only_pending = 0;
						int spoke_ack = 0;
						if (wm == 1 || !chime_played) {
							if (jarvis_tts_is_enabled())
								restore_speaker_after_command(NULL);
							spoke_ack = trigger_wake_ack_response(lang, wake_detected_lang, chime_wav);
						}
						if (!spoke_ack && post_wake_timeout_ms > 0)
							duck_speaker_for_command();
						chime_played = 1;
						waiting_post_wake = 1;
						g_in_post_wake = 1;
						in_speech = 0;
						consec_voiced = 0;
						voiced_frames = 0;
						silence_frames = 0;
						trailing_soft_frames = 0;
						live_cmd[0] = '\0';
						live_cmd_from_main = 0;
						decoded_cmd_is_final = 0;
						decoded_cmd_end_f = -1;
						pthread_mutex_lock(&g_audio.mtx);
						g_audio.in_speech_hint = 0;
						if (spoke_ack) {
							waiting_wake_ack = 1;
							wake_ack_start_ms = now_ms();
							wake_ack_done_ms = 0;
							chime_ignore_until_f = g_audio.total_frames;
							wake_end_f = g_audio.total_frames;
						} else {
							chime_ignore_until_f = g_audio.total_frames + 5;
							wake_end_f = g_audio.total_frames + 2;
						}
						pthread_mutex_unlock(&g_audio.mtx);
						post_wake_deadline_ms = calc_post_wake_deadline(post_wake_timeout_ms);
						write_state_file(state_file, "jarvis-cmd", NULL, NULL);
						char ltitle[128], lbody[256];
						jarvis_ontology_format_ui(get_daemon_ont(), "listening_title", wake_word,
						                          "  {Wake} Listening...", ltitle, sizeof(ltitle));
						jarvis_ontology_format_ui(get_daemon_ont(), "listening_body", wake_word,
						                          "Speak your command now.", lbody, sizeof(lbody));
						notify_osd_async(ltitle, lbody, 4500);
						continue;
					} else if (wm == 0) {
						if (tts_on || tts_was_active_in_utter) {
							if (operating_mode && (is_bargein_stop_phrase(final_raw) || is_op_abort_phrase(final_raw))) {
								if (tts_on)
									interrupt_tts_and_clear_cache();
								snprintf(live_cmd, sizeof(live_cmd), "%s", final_raw);
								live_cmd_from_main = 1;
							} else if (!operating_mode && is_bargein_stop_phrase(final_raw)) {
								interrupt_tts_and_clear_cache();
								tts_was_active_in_utter = 0;
								live_cmd[0] = '\0';
								live_cmd_from_main = 0;
							} else {
								live_cmd[0] = '\0';
								live_cmd_from_main = 0;
							}
						} else if (!operating_mode && is_bargein_stop_phrase(final_raw)) {
							interrupt_tts_and_clear_cache();
							tts_was_active_in_utter = 0;
							live_cmd[0] = '\0';
							live_cmd_from_main = 0;
						} else {
							snprintf(live_cmd, sizeof(live_cmd), "%s", final_raw);
							live_cmd_from_main = 1;
						}
					} else {
						live_cmd[0] = '\0';
						live_cmd_from_main = 0;
					}
				} else if (ran_final_main && !live_cmd_from_main) {
					/* Final ctx_main pass rejected the utterance as empty/noise;
					 * do not fall back to the smaller ctx_wake preview text.
					 */
					live_cmd[0] = '\0';
				}
				if (resumed_speaking) {
					silence_frames = 0;
					trailing_soft_frames = 0;
					decoded_cmd_is_final = 0;
					if (live_cmd[0] != '\0') {
						char cmd_live_hdr[128];
						jarvis_ontology_format_ui(get_daemon_ont(), "command_live", wake_word,
						                          "  {Wake} Command (Live)...", cmd_live_hdr, sizeof(cmd_live_hdr));
						write_state_file(state_file, "cmd", live_cmd, cmd_live_hdr);
					}
					continue;
				}
				if (live_cmd[0] == '\0' && keep_post_wake) {
					waiting_post_wake = 1;
					g_in_post_wake = 1;
					in_speech = 0;
					consec_voiced = 0;
					voiced_frames = 0;
					silence_frames = 0;
					trailing_soft_frames = 0;
					decoded_cmd_is_final = 0;
					tts_was_active_in_utter = is_tts_currently_speaking(NULL, 0) ? 1 : 0;
					restore_speaker_after_command(NULL);
					write_state_file(state_file, "jarvis-cmd", NULL, NULL);
					pthread_mutex_lock(&g_audio.mtx);
					g_audio.in_speech_hint = 0;
					pthread_mutex_unlock(&g_audio.mtx);
					continue;
				}
				break;
			}
		}
	}

	g_in_post_wake = 0;
	set_can_preempt_wait(0);

	if (g_cancel_req || (cli == STL_INVALID_SOCKET && !g_jarvis_active)) {
		if (waiting_wake_ack)
			interrupt_tts_and_clear_cache();
		restore_speaker_after_command(NULL);
		decode_reset_session();
		audio_release(has_pending_client() ? WARM_HOLD_MS : 2000L);
		free(wake_pcm);
		if (cli != STL_INVALID_SOCKET)
			jarvis_plat_write_sock_all(cli, "CANCELLED\n", 10);
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
		if (cli != STL_INVALID_SOCKET) {
			char reply[4200];
			snprintf(reply, sizeof(reply), "CMD|%s\n", live_cmd);
			jarvis_plat_write_sock_all(cli, reply, strlen(reply));
		}
		if (!operating_mode) {
			char ctitle[128];
			jarvis_ontology_format_ui(get_daemon_ont(), "command_title", wake_word,
			                          "  {Wake} Command", ctitle, sizeof(ctitle));
			notify_osd_async(ctitle, live_cmd, 6000);
		}
		return 0;
	} else {
		if (!operating_mode)
			g_in_followup = 0;
		update_idle_voice_state(state_file);
		if (cli != STL_INVALID_SOCKET)
			jarvis_plat_write_sock_all(cli, "EMPTY\n", 6);
		return 2;
	}
}

static int
handle_jarvis_wait(stl_socket_t srv, stl_socket_t cli, jarvis_stt_ctx_t *ctx_wake, int n_threads,
                   const char *lang, double thresh_db, int cmd_silence_ms,
                   const char *wake_word, const char *state_file, const char *chime_wav,
                   int followup_ms, char *out_cmd, size_t out_sz)
{
	return handle_jarvis_wait_ex(srv, cli, ctx_wake, n_threads,
	                             lang, thresh_db, cmd_silence_ms,
	                             wake_word, state_file, chime_wav,
	                             followup_ms, 6000L, 0, out_cmd, out_sz);
}

static int
is_speaker_playing_c(void)
{
	return jarvis_audio_is_external_playing();
}

static void
read_jarvis_runtime_cfg(char *lang, size_t lang_sz,
                        int *out_followup_ms,
                        int *out_conv_followup_ms,
                        int *out_cmd_silence_ms)
{
	char rc_l[32] = {0};
	if (jarvis_rc_get("lang", NULL, rc_l, sizeof(rc_l)) &&
	    (strcasecmp(rc_l, "auto") == 0 || strcasecmp(rc_l, "kbd") == 0)) {
		snprintf(lang, lang_sz, "%s", rc_l);
	} else {
		jarvis_resolve_lang(get_daemon_ont(), lang, lang_sz);
	}

	char rc_f[32] = {0}, rc_cf[32] = {0};
	int has_followup = jarvis_rc_get("followup_ms", NULL, rc_f, sizeof(rc_f)) ? 1 : 0;
	int has_conv_followup = jarvis_rc_get("conv_followup_ms", NULL, rc_cf, sizeof(rc_cf)) ? 1 : 0;

	*out_followup_ms = has_followup ? atoi(rc_f) : 0;
	if (*out_followup_ms < 0 || *out_followup_ms > JARVIS_FOLLOWUP_PERSISTENT)
		*out_followup_ms = 0;

	int def_conv = (has_followup && *out_followup_ms == 0) ? 0 : JARVIS_FOLLOWUP_PERSISTENT;
	*out_conv_followup_ms = has_conv_followup ? atoi(rc_cf) : def_conv;
	if (*out_conv_followup_ms < 0 || *out_conv_followup_ms > JARVIS_FOLLOWUP_PERSISTENT)
		*out_conv_followup_ms = def_conv;
	if (out_cmd_silence_ms) {
		int cs = jarvis_rc_get_int("cmd_silence_ms", 750);
		if (cs < 560)
			cs = 560;
		if (cs > 10000)
			cs = 10000;
		*out_cmd_silence_ms = cs;
	}
}

static void
init_daemon_runtime(void)
{
	char chime_wav[256], done_wav[256];
	if (jarvis_plat_state_path("jarvis-chime", ".wav", chime_wav, sizeof(chime_wav)) == 0 &&
	    !jarvis_plat_is_readable(chime_wav))
		jarvis_voice_write_earcon_wav(chime_wav, 0);
	if (jarvis_plat_state_path("jarvis-done", ".wav", done_wav, sizeof(done_wav)) == 0 &&
	    !jarvis_plat_is_readable(done_wav))
		jarvis_voice_write_earcon_wav(done_wav, 1);

	/* Drop any stale ren session so config changes (e.g. lang/wake_word) take effect on restart */
	jarvis_ren_cleanup_stale();

	char ont_path[512] = {0};
	const char *req_ont = jarvis_rc_get("ontology_path", NULL, ont_path, sizeof(ont_path)) ? ont_path : NULL;
	g_daemon_ont = jarvis_ontology_create(req_ont);
}

static int
is_op_turn_active_cb(void)
{
	pthread_mutex_lock(&g_op_turn.mtx);
	int active = g_op_turn.active;
	pthread_mutex_unlock(&g_op_turn.mtx);
	return active;
}

static int
is_op_turn_stopped_cb(void)
{
	pthread_mutex_lock(&g_op_turn.mtx);
	int stopped = g_op_turn.was_stopped;
	pthread_mutex_unlock(&g_op_turn.mtx);
	return stopped;
}

static int
send_ren_control_cmd(const char *cmd, char *out_reply, size_t out_sz)
{
	return jarvis_dispatch_send_ren_cmd(cmd, out_reply, out_sz, is_op_turn_active_cb);
}

static bool
dispatch_to_ren_tmux(const char *utterance, int *out_started_media, int *out_conv_exited,
                     char *out_err, size_t err_sz)
{
	char wake_buf[64];
	get_active_wake_word(wake_buf, sizeof(wake_buf));
	return jarvis_dispatch_to_ren(utterance, get_daemon_ont(), wake_buf,
	                              out_started_media, out_conv_exited,
	                              out_err, err_sz, is_op_turn_stopped_cb);
}

typedef struct {
	char wm_surface[64];
	int  was_followup;
} op_worker_args_t;

static void *
op_turn_worker_fn(void *arg)
{
	op_worker_args_t *wargs = arg;

	char cmd_text[4096];
	pthread_mutex_lock(&g_op_turn.mtx);
	snprintf(cmd_text, sizeof(cmd_text), "%s", g_op_turn.cmd_text);
	pthread_mutex_unlock(&g_op_turn.mtx);

	int act_rc = -1;
	int started_media = 0;
	int conv_exited = 0;
	char err_msg[256] = {0};

	pthread_mutex_lock(&g_jarvis_mtx);
	if (dispatch_to_ren_tmux(cmd_text, &started_media, &conv_exited, err_msg, sizeof(err_msg))) {
		act_rc = 0;
	} else {
		pthread_mutex_lock(&g_op_turn.mtx);
		int stopped = g_op_turn.was_stopped;
		pthread_mutex_unlock(&g_op_turn.mtx);

		if (!stopped) {
			act_rc = 1;
			if (wargs->was_followup <= 0) {
				char body[640], wake_buf[64], ntitle[128], etitle[160];
				get_active_wake_word(wake_buf, sizeof(wake_buf));
				jarvis_ontology_format_ui(get_daemon_ont(), "notify_title", wake_buf,
				                          "  {Wake}", ntitle, sizeof(ntitle));
				snprintf(etitle, sizeof(etitle), "%s — Error", ntitle);
				snprintf(body, sizeof(body), "“%.340s”\n▸ %.180s",
				         cmd_text, err_msg[0] ? err_msg : "request failed");
				daemon_notify(etitle, body, 0);
			}
		}
	}
	pthread_mutex_unlock(&g_jarvis_mtx);

	pthread_mutex_lock(&g_op_turn.mtx);
	g_op_turn.act_rc = act_rc;
	g_op_turn.started_media = started_media;
	g_op_turn.conv_exited = conv_exited;
	g_op_turn.active = 0;
	pthread_mutex_unlock(&g_op_turn.mtx);

	pthread_mutex_lock(&g_audio.mtx);
	pthread_cond_broadcast(&g_audio.cond);
	pthread_mutex_unlock(&g_audio.mtx);
	return NULL;
}

static void
handle_lockdown_phrase(const char *cmd_text)
{
	jarvis_set_active(0);
	const jarvis_ontology_t *ont = get_daemon_ont();
	char wake_buf[64], title[128], body[256];
	get_active_wake_word(wake_buf, sizeof(wake_buf));
	int want_screen_lock = jarvis_ontology_is_screen_lock_phrase(ont, cmd_text) ? 1 : 0;
	if (want_screen_lock) {
		jarvis_ontology_format_ui(ont, "lockdown_title", wake_buf,
		                          "  {Wake} Lockdown", title, sizeof(title));
		jarvis_ontology_format_ui(ont, "lockdown_body", wake_buf,
		                          "Continuous listening disabled. Locking screen.", body, sizeof(body));
		daemon_notify(title, body, 0);
		char lock_cmd[256] = {0};
		jarvis_rc_get("lock_cmd", "", lock_cmd, sizeof(lock_cmd));
		jarvis_plat_lock_screen(lock_cmd[0] ? lock_cmd : NULL);
	} else {
		jarvis_ontology_format_ui(ont, "disabled_title", wake_buf,
		                          "  {Wake} Disabled", title, sizeof(title));
		jarvis_ontology_format_ui(ont, "disabled_body", wake_buf,
		                          "Continuous listening disabled.", body, sizeof(body));
		daemon_notify(title, body, 0);
	}
}

static void
run_inprocess_jarvis_turn(stl_socket_t srv, jarvis_stt_ctx_t *ctx_wake, int n_threads,
                          int *io_next_followup)
{
	char state_file[256], chime_wav[256], last_txt[256], acting_pid[256];
	jarvis_plat_state_path("jarvis-voice", ".state", state_file, sizeof(state_file));
	jarvis_plat_state_path("jarvis-chime", ".wav", chime_wav, sizeof(chime_wav));
	jarvis_plat_state_path("jarvis-voice", ".last.txt", last_txt, sizeof(last_txt));
	jarvis_plat_state_path("jarvis-acting", ".pid", acting_pid, sizeof(acting_pid));

	char lang[32], wm_surface[64];
	int fcfg = 0, conv_followup_ms = JARVIS_FOLLOWUP_PERSISTENT, cmd_silence_ms = 750;
	read_jarvis_runtime_cfg(lang, sizeof(lang), &fcfg, &conv_followup_ms, &cmd_silence_ms);
	jarvis_resolve_wm(wm_surface, sizeof(wm_surface));

	int eff_conv = (fcfg > 0) ? fcfg : conv_followup_ms;
	long post_wake_timeout_ms = (eff_conv >= JARVIS_FOLLOWUP_PERSISTENT) ? 0L : 6000L;

	int next_followup = *io_next_followup;
	if (fcfg == JARVIS_FOLLOWUP_PERSISTENT) {
		next_followup = is_speaker_playing_c() ? 0 : JARVIS_FOLLOWUP_PERSISTENT;
	}

	g_in_followup = (next_followup > 0 && !is_tts_audio_hot()) ? 1 : 0;
	update_idle_voice_state(state_file);

	char wake_buf[64];
	get_active_wake_word(wake_buf, sizeof(wake_buf));

	char cmd_text[4096] = {0};
	int rc = handle_jarvis_wait_ex(srv, STL_INVALID_SOCKET, ctx_wake, n_threads,
	                               lang, -35.0, cmd_silence_ms, wake_buf,
	                               state_file, chime_wav, next_followup,
	                               post_wake_timeout_ms, 0,
	                               cmd_text, sizeof(cmd_text));
	int was_followup = next_followup;
	*io_next_followup = 0;

	if (!g_jarvis_active) {
		if (remove_state_file(state_file))
			notify_bar();
		return;
	}

	if (rc == 130) {
		if (was_followup >= JARVIS_FOLLOWUP_PERSISTENT)
			*io_next_followup = was_followup;
		return;
	}

	if (rc == 0 && cmd_text[0] != '\0') {
		while (cmd_text[0] != '\0') {
			FILE *lfp = fopen(last_txt, "w");
			if (lfp) {
				fprintf(lfp, "%s", cmd_text);
				fclose(lfp);
			}

			if (jarvis_voice_is_lockdown_phrase_ex(get_daemon_ont(), cmd_text, wake_buf)) {
				handle_lockdown_phrase(cmd_text);
				return;
			}

			FILE *afp = fopen(acting_pid, "w");
			if (afp) {
				fprintf(afp, "%d\n", (int)getpid());
				fclose(afp);
			}
			write_state_file(state_file, "processing", NULL, NULL);

			interrupt_tts_and_clear_cache();

			op_worker_args_t wargs;
			snprintf(wargs.wm_surface, sizeof(wargs.wm_surface), "%s", wm_surface);
			wargs.was_followup = was_followup;

			pthread_mutex_lock(&g_op_turn.mtx);
			g_op_turn.active = 1;
			g_op_turn.was_stopped = 0;
			g_op_turn.act_rc = -1;
			g_op_turn.started_media = 0;
			g_op_turn.conv_exited = 0;
			snprintf(g_op_turn.cmd_text, sizeof(g_op_turn.cmd_text), "%s", cmd_text);
			pthread_mutex_unlock(&g_op_turn.mtx);

			audio_acquire();
			if (pthread_create(&g_op_turn.tid, NULL, op_turn_worker_fn, &wargs) != 0) {
				pthread_mutex_lock(&g_op_turn.mtx);
				g_op_turn.active = 0;
				pthread_mutex_unlock(&g_op_turn.mtx);
				audio_release(WARM_HOLD_MS);
				remove(acting_pid);
				notify_bar();
				return;
			}

			char pending_next_cmd[4096] = {0};
			while (1) {
				pthread_mutex_lock(&g_op_turn.mtx);
				int op_active = g_op_turn.active;
				pthread_mutex_unlock(&g_op_turn.mtx);
				if (!op_active)
					break;

				char steer_cmd[4096] = {0};
				int op_rc = handle_jarvis_wait_ex(srv, STL_INVALID_SOCKET, ctx_wake, n_threads,
				                                  lang, -35.0, cmd_silence_ms, wake_buf,
				                                  state_file, chime_wav, 0, 6000L, 1,
				                                  steer_cmd, sizeof(steer_cmd));

				if (!g_jarvis_active || op_rc == 130) {
					pthread_mutex_lock(&g_op_turn.mtx);
					g_op_turn.was_stopped = 1;
					int still_active = g_op_turn.active;
					pthread_mutex_unlock(&g_op_turn.mtx);
					if (still_active)
						send_ren_control_cmd("STOP", NULL, 0);
					break;
				}

				if (op_rc == 3)
					break;

				if (op_rc == 2) {
					update_idle_voice_state(state_file);
					continue;
				}

				if (op_rc == 0 && steer_cmd[0] != '\0') {
					if (jarvis_voice_is_lockdown_phrase_ex(get_daemon_ont(), steer_cmd, wake_buf)) {
						pthread_mutex_lock(&g_op_turn.mtx);
						g_op_turn.was_stopped = 1;
						pthread_mutex_unlock(&g_op_turn.mtx);
						send_ren_control_cmd("STOP", NULL, 0);
						handle_lockdown_phrase(steer_cmd);
						break;
					} else if (is_op_abort_phrase(steer_cmd)) {
						interrupt_tts_and_clear_cache();
						pthread_mutex_lock(&g_op_turn.mtx);
						g_op_turn.was_stopped = 1;
						pthread_mutex_unlock(&g_op_turn.mtx);
						send_ren_control_cmd("STOP", NULL, 0);
						char stitle[128];
						jarvis_ontology_format_ui(get_daemon_ont(), "stopped_title", wake_buf,
						                          "  {Wake} Stopped", stitle, sizeof(stitle));
						notify_osd_async(stitle, steer_cmd, 4000);
						update_idle_voice_state(state_file);
						continue;
					} else if (is_bargein_stop_phrase(steer_cmd)) {
						interrupt_tts_and_clear_cache();
						update_idle_voice_state(state_file);
						continue;
					} else {
						interrupt_tts_and_clear_cache();
						char ipc_steer[4096], reply[64] = {0};
						snprintf(ipc_steer, sizeof(ipc_steer), "STEER\t%.4080s", steer_cmd);
						int s_rc = send_ren_control_cmd(ipc_steer, reply, sizeof(reply));
						pthread_mutex_lock(&g_op_turn.mtx);
						int still_active = g_op_turn.active;
						pthread_mutex_unlock(&g_op_turn.mtx);
						if ((s_rc == 0 && strcmp(reply, "IDLE") == 0) || (s_rc < 0 && !still_active)) {
							snprintf(pending_next_cmd, sizeof(pending_next_cmd), "%s", steer_cmd);
							break;
						}
						char sttitle[128];
						jarvis_ontology_format_ui(get_daemon_ont(), "steering_title", wake_buf,
						                          "  {Wake} Steering", sttitle, sizeof(sttitle));
						notify_osd_async(sttitle, steer_cmd, 5000);
						update_idle_voice_state(state_file);
						continue;
					}
				}
			}

			pthread_join(g_op_turn.tid, NULL);
			audio_release(WARM_HOLD_MS);
			remove(acting_pid);
			notify_bar();

			pthread_mutex_lock(&g_op_turn.mtx);
			int act_rc = g_op_turn.act_rc;
			int started_media = g_op_turn.started_media;
			int conv_exited = g_op_turn.conv_exited;
			int was_stopped = g_op_turn.was_stopped;
			pthread_mutex_unlock(&g_op_turn.mtx);

			if (!g_jarvis_active) {
				remove_state_file(state_file);
				notify_bar();
				return;
			}

			if (pending_next_cmd[0] != '\0') {
				snprintf(cmd_text, sizeof(cmd_text), "%s", pending_next_cmd);
				was_followup = 0;
				continue;
			}

			int eff_followup = (fcfg > 0) ? fcfg : conv_followup_ms;
			if (!started_media && !conv_exited && eff_followup > 0 &&
			    (act_rc == 0 || act_rc == -1 || was_stopped))
				*io_next_followup = eff_followup;
			g_in_followup = (*io_next_followup > 0 && !is_tts_audio_hot()) ? 1 : 0;
			update_idle_voice_state(state_file);
			break;
		}
	} else if (rc == 2) {
		g_in_followup = 0;
		update_idle_voice_state(state_file);
		if (was_followup <= 0) {
			char sbtitle[128], sbbody[256];
			jarvis_ontology_format_ui(get_daemon_ont(), "standby_title", wake_buf,
			                          "  {Wake} Standby", sbtitle, sizeof(sbtitle));
			jarvis_ontology_format_ui(get_daemon_ont(), "standby_body", wake_buf,
			                          "No command spoken.", sbbody, sizeof(sbbody));
			daemon_notify(sbtitle, sbbody, 0);
		}
	}
}

static void
send_control_msg(int port, const char *msg)
{
	stl_sock_init();
	stl_socket_t s = stl_sock_create(AF_INET, SOCK_STREAM, 0);
	if (s == STL_INVALID_SOCKET)
		return;
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (stl_sock_connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0)
		jarvis_plat_write_sock_all(s, msg, strlen(msg));
	stl_sock_close(s);
}

static int
run_client_mode(int port, const char *req)
{
	stl_ignore_pipe();
	stl_stop_on_signal(on_cli_sigint);
#ifndef _WIN32
	signal(SIGHUP, SIG_IGN);
#ifdef SIGUSR1
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_cli_sigusr1;
		sigaction(SIGUSR1, &sa, NULL);
	}
#endif
#endif

	stl_sock_init();
	stl_socket_t s = stl_sock_create(AF_INET, SOCK_STREAM, 0);
	if (s == STL_INVALID_SOCKET)
		return 3;
	stl_sock_set_timeout(s, 4000, 4000);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (stl_sock_connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		stl_sock_close(s);
		return 3;
	}

	char send_buf[2048];
	snprintf(send_buf, sizeof(send_buf), "%s\n", req);
	jarvis_plat_write_sock_all(s, send_buf, strlen(send_buf));

	int sent_stop = 0;
	size_t resp_cap = JARVIS_STREAM_COMMITTED_CAP + 64;
	char *resp = (char *)malloc(resp_cap);
	if (!resp) {
		stl_sock_close(s);
		return 3;
	}
	size_t total = 0;

	while (total < resp_cap - 1) {
		if (cli_cancel_req) {
			send_control_msg(port, "CANCEL\n");
			stl_sock_close(s);
			free(resp);
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
		int pr = stl_poll(&pfd, 1, 40);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pr == 0)
			continue;
		if (pfd.revents & (POLLIN | POLLHUP)) {
			ssize_t n = stl_sock_recv(s, resp + total, resp_cap - 1 - total, 0);
			if (n <= 0)
				break;
			total += (size_t)n;
			if (memchr(resp, '\n', total))
				break;
		}
	}
	stl_sock_close(s);
	resp[total] = '\0';
	trim_inplace(resp);

	int rc = 0;
	if (strcmp(resp, "CANCELLED") == 0) {
		rc = 130;
	} else if (strcmp(resp, "EMPTY") == 0 || strcmp(resp, "TIMEOUT") == 0 || resp[0] == '\0') {
		rc = 2;
	} else if (strncmp(resp, "OK|", 3) == 0) {
		printf("%s\n", resp + 3);
	} else if (strncmp(resp, "CMD|", 4) == 0) {
		printf("%s\n", resp + 4);
	} else {
		printf("%s\n", resp);
	}
	free(resp);
	return rc;
}

static void
serve_client_request(stl_socket_t srv, stl_socket_t cli, char *buf,
                     jarvis_stt_ctx_t *ctx_main,
                     jarvis_stt_ctx_t *ctx_wake, int n_threads)
{
	if (handle_control_cmd(cli, buf))
		return;

	char *nl = strpbrk(buf, "\r\n");
	if (nl)
		*nl = '\0';

	char *save = NULL;
	char *cmd = dc_strtok_r(buf, "|", &save);
	if (!cmd) {
		stl_sock_close(cli);
		return;
	}

	if (strcmp(cmd, "STREAM") == 0) {
		char *mode       = dc_strtok_r(NULL, "|", &save);
		char *lang       = dc_strtok_r(NULL, "|", &save);
		char *thresh_str = dc_strtok_r(NULL, "|", &save);
		char *sil_str    = dc_strtok_r(NULL, "|", &save);
		char *wait_str   = dc_strtok_r(NULL, "|", &save);
		char *state_file = dc_strtok_r(NULL, "|", &save);
		char *ctx_str    = dc_strtok_r(NULL, "|", &save);

		if (mode && lang && thresh_str && sil_str && wait_str) {
			handle_stream(srv, cli, mode, lang,
			              strtod(thresh_str, NULL), atoi(sil_str),
			              atoi(wait_str), state_file ? state_file : "-",
			              ctx_str);
		}
		stl_sock_close(cli);
		return;
	}

	if (strcmp(cmd, "JARVIS_WAIT") == 0) {
		char *lang         = dc_strtok_r(NULL, "|", &save);
		char *thresh_str   = dc_strtok_r(NULL, "|", &save);
		char *sil_str      = dc_strtok_r(NULL, "|", &save);
		char *wake_word    = dc_strtok_r(NULL, "|", &save);
		char *state_file   = dc_strtok_r(NULL, "|", &save);
		char *chime_wav    = dc_strtok_r(NULL, "|", &save);
		char *followup_str = dc_strtok_r(NULL, "|", &save);

		if (lang && thresh_str && sil_str && wake_word) {
			handle_jarvis_wait(srv, cli, ctx_wake, n_threads,
			                   lang, strtod(thresh_str, NULL), atoi(sil_str),
			                   wake_word, state_file ? state_file : "-",
			                   chime_wav ? chime_wav : "-",
			                   followup_str ? atoi(followup_str) : 0, NULL, 0);
		}
		stl_sock_close(cli);
		return;
	}

	if (strcmp(cmd, "TRANSCRIBE") == 0) {
		char *mode     = dc_strtok_r(NULL, "|", &save);
		char *lang     = dc_strtok_r(NULL, "|", &save);
		char *mt_str   = dc_strtok_r(NULL, "|", &save);
		char *prompt   = dc_strtok_r(NULL, "|", &save);
		char *wav_path = dc_strtok_r(NULL, "|", &save);

		if (!mode || !lang || !mt_str || !prompt || !wav_path) {
			stl_sock_close(cli);
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
			stl_sock_close(cli);
			return;
		}
		jarvis_stt_apply_hpf_inplace(pcm, n_samples);

		decode_reset_session();
		jarvis_stt_ctx_t *ctx = is_wake ? ctx_wake : ctx_main;
		char out_text[4096];
		run_whisper_slice(ctx, n_threads, pcm, n_samples, lang, prompt,
		                  max_tok, is_wake, 0, 0, out_text, sizeof(out_text), NULL);
		strncat(out_text, "\n", sizeof(out_text) - strlen(out_text) - 1);
		jarvis_plat_write_sock_all(cli, out_text, strlen(out_text));
		stl_sock_close(cli);
		free(pcm);
		return;
	}

	stl_sock_close(cli);
}

static void
prepend_path_dir(const char *dir)
{
	if (!dir || !*dir) return;
	const char *cur_path = stl_getenv("PATH");
	if (!cur_path || !*cur_path) {
		stl_setenv("PATH", dir, 1);
		return;
	}
#ifdef _WIN32
	const char sep = ';';
#else
	const char sep = ':';
#endif
	size_t dlen = strlen(dir);
	const char *p = cur_path;
	while (p && *p) {
		const char *next = strchr(p, sep);
		size_t plen = next ? (size_t)(next - p) : strlen(p);
#ifdef _WIN32
		if (plen == dlen && _strnicmp(p, dir, dlen) == 0)
			return;
#else
		if (plen == dlen && strncmp(p, dir, dlen) == 0)
			return;
#endif
		p = next ? next + 1 : NULL;
	}
	char new_path[4096];
	snprintf(new_path, sizeof(new_path), "%s%c%s", dir, sep, cur_path);
	stl_setenv("PATH", new_path, 1);
}

int
main(int argc, char *argv[])
{
	const char *main_model_path = NULL;
	const char *wake_model_path = NULL;
	const char *client_req      = NULL;
	{
		const char *home = get_home_dir();
		if (home && *home) {
			char local_bin[512];
#ifdef _WIN32
			snprintf(local_bin, sizeof(local_bin), "%s\\.local\\bin", home);
#else
			snprintf(local_bin, sizeof(local_bin), "%s/.local/bin", home);
#endif
			prepend_path_dir(local_bin);
		}
		char exe_dir[512] = {0};
		if (stl_exe_dir(exe_dir, sizeof(exe_dir)) && exe_dir[0]) {
			prepend_path_dir(exe_dir);
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

	jarvis_hw_probe_devices(&g_hw);

	static char s_main_model[512], s_wake_model[512];
	jarvis_resolve_stt_models(g_hw.tier, s_main_model, sizeof(s_main_model),
	                          s_wake_model, sizeof(s_wake_model));
	if (!main_model_path || !*main_model_path)
		main_model_path = s_main_model;
	if (!wake_model_path || !*wake_model_path)
		wake_model_path = s_wake_model;

	if (!main_model_path || !*main_model_path) {
		fprintf(stderr, "jarvisd: no STT model found in ~/.jarvisrc or whisper/jarvis model directories\n");
		return 1;
	}

	/* Kernel-level singleton lock so duplicate jarvisd daemons can never start */
	jarvis_plat_lock_t *inst_lock = jarvis_plat_singleton_acquire("jarvisd");
	if (!inst_lock) {
		if (start_jarvis)
			send_control_msg(port, "JARVIS_ON\n");
		return 0;
	}

	stl_stop_on_signal(on_sig);
	stl_ignore_pipe();
#ifndef _WIN32
	signal(SIGHUP, SIG_IGN);
#endif

	init_daemon_runtime();

	stl_sock_init();
	stl_socket_t srv = stl_sock_create(AF_INET, SOCK_STREAM, 0);
	if (srv == STL_INVALID_SOCKET) {
		if (g_daemon_ont) {
			jarvis_ontology_destroy(g_daemon_ont);
			g_daemon_ont = NULL;
		}
		jarvis_plat_singleton_release(inst_lock);
		return 1;
	}
#ifndef _WIN32
	/* Prevent child capture/playback processes (pw-record, pw-cat) from inheriting the listen socket */
	fcntl(srv, F_SETFD, FD_CLOEXEC);
#endif

	int yes = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
#ifdef SO_REUSEPORT
	setsockopt(srv, SOL_SOCKET, SO_REUSEPORT, (const char *)&yes, sizeof(yes));
#endif

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (stl_sock_bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 || stl_sock_listen(srv, 16) < 0) {
		stl_sock_close(srv);
		if (g_daemon_ont) {
			jarvis_ontology_destroy(g_daemon_ont);
			g_daemon_ont = NULL;
		}
		jarvis_plat_singleton_release(inst_lock);
		return 1;
	}

	g_ctrl.srv = srv;
	if (start_jarvis)
		jarvis_set_active(1);

	pthread_t ctrl_tid;
	if (pthread_create(&ctrl_tid, NULL, control_thread_fn, NULL) != 0) {
		if (g_jarvis_active)
			jarvis_set_active(0);
		stl_sock_close(srv);
		if (g_daemon_ont) {
			jarvis_ontology_destroy(g_daemon_ont);
			g_daemon_ont = NULL;
		}
		jarvis_plat_singleton_release(inst_lock);
		return 1;
	}

	bool use_gpu = (jarvis_rc_get_int("whisper_gpu", 1) != 0);
	jarvis_stt_ctx_t *ctx_main = jarvis_stt_open_auto(main_model_path, use_gpu);
	if (!ctx_main) {
		fprintf(stderr, "jarvisd: failed to load main model %s\n", main_model_path);
		running = 0;
		if (g_jarvis_active)
			jarvis_set_active(0);
		pthread_join(ctrl_tid, NULL);
		pthread_mutex_lock(&g_ctrl.mtx);
		if (g_ctrl.pending_cli != STL_INVALID_SOCKET) {
			stl_sock_close(g_ctrl.pending_cli);
			g_ctrl.pending_cli = STL_INVALID_SOCKET;
		}
		pthread_mutex_unlock(&g_ctrl.mtx);
		stl_sock_close(srv);
		if (g_daemon_ont) {
			jarvis_ontology_destroy(g_daemon_ont);
			g_daemon_ont = NULL;
		}
		jarvis_plat_singleton_release(inst_lock);
		return 1;
	}

	jarvis_stt_ctx_t *ctx_wake = NULL;
	if (wake_model_path && *wake_model_path && strcmp(wake_model_path, main_model_path) != 0 &&
	    jarvis_plat_is_readable(wake_model_path)) {
		ctx_wake = jarvis_stt_open_auto(wake_model_path, use_gpu);
	}
	if (!ctx_wake)
		ctx_wake = ctx_main;

	g_dec.ctx = ctx_main;
	g_dec.n_threads = n_threads;
	g_dec.pcm = malloc((size_t)MAX_UTTER_FRAMES * FRAME_SAMPLES * sizeof(float));
	if (!g_dec.pcm) {
		running = 0;
		if (g_jarvis_active)
			jarvis_set_active(0);
		pthread_join(ctrl_tid, NULL);
		pthread_mutex_lock(&g_ctrl.mtx);
		if (g_ctrl.pending_cli != STL_INVALID_SOCKET) {
			stl_sock_close(g_ctrl.pending_cli);
			g_ctrl.pending_cli = STL_INVALID_SOCKET;
		}
		pthread_mutex_unlock(&g_ctrl.mtx);
		stl_sock_close(srv);
		if (g_daemon_ont) {
			jarvis_ontology_destroy(g_daemon_ont);
			g_daemon_ont = NULL;
		}
		jarvis_stt_close(ctx_main);
		if (ctx_wake != ctx_main)
			jarvis_stt_close(ctx_wake);
		jarvis_plat_singleton_release(inst_lock);
		return 1;
	}

	/* Warmup both contexts and calibrate hardware tier / default audio_ctx */
	if (running) {
		const char *ont_lang = jarvis_ontology_get_lang(get_daemon_ont());
		if (!ont_lang || !*ont_lang)
			ont_lang = "en";
		jarvis_hw_warmup_and_calibrate(&g_hw, ctx_main, ctx_wake, n_threads, ont_lang);
		fprintf(stderr,
		        "jarvisd: stt=%s hw=%s (%s: %s) ac512=%dms ac1500=%dms final_ctx=%d model=%s\n",
		        jarvis_stt_backend_name(ctx_main),
		        jarvis_hw_tier_str(g_hw.tier),
		        g_hw.backend,
		        g_hw.device,
		        g_hw.ac512_ms,
		        g_hw.ac1500_ms,
		        g_hw.final_audio_ctx,
		        main_model_path);
		if (!g_hw.has_gpu && g_hw.ac512_ms > 1500) {
			fprintf(stderr,
			        "jarvisd: NOTICE: CPU decode took %dms; consider installing "
			        "ggml-parakeet-tdt-0.6b-v3 or ggml-small-q8_0.bin for lower latency\n",
			        g_hw.ac512_ms);
		}

		char wake_init[64] = {0};
		jarvis_resolve_wake_word(get_daemon_ont(), wake_init, sizeof(wake_init));
		g_kws = jarvis_kws_open(wake_init, NULL, NULL);
		if (g_kws) {
			if (jarvis_kws_tier(g_kws) == JARVIS_KWS_TIER_ONNX) {
				fprintf(stderr, "jarvisd: kws=onnx wake=%s model=%s\n",
				        wake_init, jarvis_kws_model_path(g_kws));
			} else {
				fprintf(stderr, "jarvisd: kws=vad-gate wake=%s (universal Silero-gated fallback)\n",
				        wake_init);
			}
		}
	}

	pthread_t audio_tid, decode_tid;
	pthread_create(&audio_tid, NULL, audio_thread_fn, NULL);
	pthread_create(&decode_tid, NULL, decode_worker_fn, NULL);

	int next_followup = 0;

	while (running) {
		stl_socket_t cli = STL_INVALID_SOCKET;
		char buf[2048];

		pthread_mutex_lock(&g_ctrl.mtx);
		if (!g_jarvis_active)
			next_followup = 0;
		while (running && g_ctrl.pending_cli == STL_INVALID_SOCKET && !g_jarvis_active) {
			struct timespec ts;
			jarvis_clock_realtime(&ts);
			ts.tv_nsec += 200L * 1000000L;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec += 1;
				ts.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&g_ctrl.cond, &g_ctrl.mtx, &ts);
		}
		if (!running) {
			pthread_mutex_unlock(&g_ctrl.mtx);
			break;
		}
		if (g_ctrl.pending_cli != STL_INVALID_SOCKET) {
			cli = g_ctrl.pending_cli;
			g_ctrl.pending_cli = STL_INVALID_SOCKET;
			snprintf(buf, sizeof(buf), "%s", g_ctrl.pending_buf);
		}
		int active_now = (int)g_jarvis_active;
		pthread_mutex_unlock(&g_ctrl.mtx);

		if (cli != STL_INVALID_SOCKET) {
			serve_client_request(srv, cli, buf, ctx_main, ctx_wake, n_threads);
			continue;
		}

		if (active_now) {
			run_inprocess_jarvis_turn(srv, ctx_wake, n_threads, &next_followup);
			continue;
		}
	}

	running = 0;
	if (g_jarvis_active)
		jarvis_set_active(0);
	pthread_mutex_lock(&g_ctrl.mtx);
	pthread_cond_broadcast(&g_ctrl.cond);
	pthread_mutex_unlock(&g_ctrl.mtx);
	pthread_join(ctrl_tid, NULL);

	pthread_mutex_lock(&g_ctrl.mtx);
	if (g_ctrl.pending_cli != STL_INVALID_SOCKET) {
		stl_sock_close(g_ctrl.pending_cli);
		g_ctrl.pending_cli = STL_INVALID_SOCKET;
	}
	pthread_mutex_unlock(&g_ctrl.mtx);

	if (g_daemon_ont) {
		jarvis_ontology_destroy(g_daemon_ont);
		g_daemon_ont = NULL;
	}
	pthread_mutex_lock(&g_dec.mtx);
	g_dec.abort_flag = 1;
	pthread_cond_broadcast(&g_dec.cond);
	pthread_mutex_unlock(&g_dec.mtx);
	pthread_mutex_lock(&g_audio.mtx);
	pthread_cond_broadcast(&g_audio.cond);
	pthread_mutex_unlock(&g_audio.mtx);
	pthread_join(audio_tid, NULL);
	pthread_join(decode_tid, NULL);

	jarvis_kws_close(g_kws);
	g_kws = NULL;
	stl_sock_close(srv);
	jarvis_plat_singleton_release(inst_lock);
	free(g_dec.pcm);
	jarvis_stt_close(ctx_main);
	if (ctx_wake != ctx_main)
		jarvis_stt_close(ctx_wake);
	return 0;
}
