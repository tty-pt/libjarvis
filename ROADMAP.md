# libjarvis — Universal Voice Pipeline & Platform Roadmap

> Engineering roadmap to make `libjarvis` (`jarvisd`, `jarvis`, `libjarvis.so`) fast, accurate, resilient, and zero-config **for every user** — across every compute tier (discrete GPU, integrated GPU, Apple Silicon, CPU-only), every OS (Linux PipeWire/Pulse/ALSA, BSD, macOS, Windows), every microphone topology (laptop dual-mic, USB/headset mono, off-axis, noisy room), and every supported language & regional dialect.
>
> **Companion design doc:** [`IDEAS.md`](IDEAS.md) covers making Tier-0 intent resolution 100% language-agnostic in C and unifying workflow memory with in-process `libmm`. This `ROADMAP.md` is the master execution plan covering the full voice-to-text pipeline (capture → DSP → VAD → wake → STT → streaming → post-processing → onboarding) and its convergence with `IDEAS.md`.

---

## 1. Design Principles ("Works for Everyone")

Every change in this roadmap obeys six non-negotiable invariants:

1. **Hardware-Adaptive, Never Machine-Specific.** Never hardcode compromises tuned for one laptop (e.g., `audio_ctx = 256` or mandatory LunarG Vulkan SDK downloads). Probe actual hardware at build/startup time and calibrate automatically across discrete GPUs, iGPUs, Apple Silicon, and CPU-only hosts.
2. **Decouple Control Signals from ASR Audio.** Spatial coherence, AEC status, and VAD probabilities decide *when* to listen and *when* to endpoint; they must **never** carve frequency notches, clip plosives, or crush unstressed syllables in the PCM fed to the speech recognizer.
3. **Zero Language Hacks in C.** Every language-specific string (wake words, hallucinations, continuation tokens, dialect seeds, idioms) lives in [`data/ontology.json`](data/ontology.json) across all 31+ locales — never hardcoded in `.c` files and never fed as raw verb-list prompts that poison Whisper decoding.
4. **Cross-Platform Parity by Construction.** Linux, BSD, macOS, and Windows share the same low-latency 20 ms frame contract, graceful backend fallbacks, and native OS echo-cancellation hooks.
5. **No Userspace Breaks.** Existing `~/.jarvisrc` keys, TCP/Unix IPC protocols (`TRANSCRIBE`, `JARVIS_WAIT`, `STREAM`), and `libjarvis.so` / `@tty-pt/jarvis` N-API/WASM ABIs remain backward compatible.
6. **Proof Over Confidence (Abstraction-First TDD).** Every phase lands with standalone unit tests (`tests/test_dsp.c`, `tests/test_jarvis.c`, `tests/test_tts.c`) and measurable WER / CER / latency / false-trigger numbers from an offline benchmark harness (`make bench-stt`).

---

## 2. Proven Baseline Defects (Why Current Defaults Fail Users)

Measured on live `jarvisd` and isolated Vulkan A/B benchmarks against real European Portuguese recordings, plus full-tree source inspection:

| # | Defect | Exact Location | Observed Failure Mode | Who It Hurts |
|---|---|---|---|---|
| **B1** | `audio_ctx = 256` truncates `large-v3-turbo` encoder geometry (trained at 1500 frames) | [`src/jarvis-stt.c:114-124`](src/jarvis-stt.c) | Mangled words (`calculadora` → `corzora`/`calcôsora`, `calendário` → `colindário`, `Jarbas` → `Xarbas`/`Jervas`) while `ac=512`/`ac=0` transcribe accurately. Even fast Metal/CUDA GPUs are crippled to `256`. | **Everyone** running `large-v3-turbo` on any GPU or CPU |
| **B2** | Static verb-list `whisper_cmd_prompt` poisons decoding | [`data/ontology.json`](data/ontology.json) (`en`, `pt`) + [`src/jarvisd.c`](src/jarvisd.c) | Whisper autocompletes prompt tokens into real commands (`"abre, fecha, muda, se faz favor"`), adding +20–150 ms latency and phantom commands on low-SNR frames. | **All `en`/`pt` users** (and any future locale adding verb lists) |
| **B3** | `beam_size = 5` default wastes compute on distilled 4-layer turbo decoders | [`src/jarvisd.c:998`](src/jarvisd.c) | Adds +100 ms to +2.7 s latency with virtually identical transcripts vs `beam_size = 1` (greedy) on `large-v3-turbo`. | **iGPU & CPU users** |
| **B4** | Double HPF + destructive `-14 dB` downward expander + $O(N^2)$ sort + hard clip | [`src/jarvis-stt.c:127-273`](src/jarvis-stt.c) | Second ~80 Hz HPF stacks on `jarvis-dsp`'s 110 Hz HPF; `-14 dB` expander crushes soft word onsets (`Jarbas` → `Gerbas`) and unstressed vowels in stress-timed languages (`pt-PT`, `en`, `ru`); insertion sort runs $O(N^2)$ on up to 4500 frames. | **Everyone**, especially quiet mics & stress-timed languages |
| **B5** | Off-axis stereo coherence collapse | [`src/jarvis-dsp.c:173-287`](src/jarvis-dsp.c) | Broadside-only $\text{Re}(\gamma_{LR})$ drops to $\le 0$ at 20–30° head rotation above 3.5 kHz, triggering -26 dB broadband gating, -12 dB HF consonant loss, and mid-word VAD dropouts. | **Laptop dual-mic users** who lean or turn their head |
| **B6** | Stale `src/whisper.h` + unused streaming Silero VAD | [`src/whisper.h`](src/whisper.h) vs `submodules/whisper.cpp/include/whisper.h` | `libwhisper.a` already includes streaming Silero VAD (`whisper_vad_detect_speech_no_reset`) and token timestamps (`whisper_full_get_token_t0/t1`), but `jarvisd` runs energy/ZCR-only VAD — letting coughs, keyboards, and background music trigger hallucinations. | **Everyone** in non-anechoic rooms |
| **B7** | High-latency capture buffers & missing fallbacks across all 3 OSes | [`src/jarvis-audio-linux.c`](src/jarvis-audio-linux.c), [`src/jarvis-audio-macos.m`](src/jarvis-audio-macos.m), [`src/jarvis-audio-win32.c`](src/jarvis-audio-win32.c) | Linux spawns `pw-record` without `--latency` (100 ms default), ignores `pw_target`, quantizes `f32→s16→f32`, and has no Pulse/ALSA/sndio fallback. macOS & Windows use `CAP_BUF_BYTES = 4096` (64–128 ms per buffer) and no native OS AEC (`VoiceProcessingIO` / WASAPI APO). | **Linux, BSD, macOS, and Windows users** |
| **B8** | Rigid build backend + no hardware calibration or discrete GPU diagnostics | [`Makefile:63-116`](Makefile), [`src/jarvisd.c:4064-4147`](src/jarvisd.c) | Linux forces LunarG Vulkan SDK download even for CPU/CUDA builds; no CUDA auto-detect; no startup latency calibration; unbound discrete GPUs (e.g., NVIDIA PCIe with no kernel driver) fail silently onto slow iGPU. | **Linux/BSD/Windows builders** and hybrid-GPU laptop users |
| **B9** | Hallucination & repetition filter blind spots | [`src/jarvis-ontology.c:265-318`](src/jarvis-ontology.c), [`src/jarvisd.c:844-960`](src/jarvisd.c) | 2–4 char periodic loops (`"Jajajajaja..."`) bypass single-char run/frequency checks; >8-word repeated clauses bypass `collapse_repetitions`. | **All languages** |
| **B10** | $O(N^2)$ streaming partials & broken >30 s dictation | [`src/jarvisd.c:986-1007, 1857-2050`](src/jarvisd.c) | Every preview re-decodes from `speech_start_f` from scratch; past 30 s, partials drop earlier audio and final passes force `no_context = true`. | **Dictation & long-command users** |

---

## 3. Core Data Layouts & Boundaries

Define the structs and invariants first before writing implementation code:

```c
/* 1. Startup hardware calibration profile (src/jarvis-stt.h) */
typedef enum {
    JARVIS_HW_TIER_FAST_GPU = 0, /* Discrete CUDA/Vulkan or Apple Silicon Metal: ac=0 <= 600ms */
    JARVIS_HW_TIER_IGPU     = 1, /* Integrated Vulkan GPU: ac=512..768 <= 800ms */
    JARVIS_HW_TIER_CPU      = 2  /* CPU-only / fallback: lean model + length-proportional backend */
} jarvis_hw_tier_t;

typedef struct {
    jarvis_hw_tier_t tier;
    char             backend_name[32];  /* "cuda", "metal", "vulkan", "cpu" */
    char             device_name[128];
    double           bench_ac512_ms;    /* Measured warmup encode latency at audio_ctx=512 */
    double           bench_ac1500_ms;   /* Measured warmup encode latency at audio_ctx=0 (1500) */
    int              final_audio_ctx;   /* 0 (full 1500) for FAST_GPU; >= 512 for IGPU; 0 for Parakeet */
    int              partial_audio_ctx; /* >= 256 (or 0 for Parakeet) */
    int              beam_size;         /* 1 for turbo/iGPU/CPU, configurable override */
    int              n_threads;         /* min(physical_cores, 8) */
} jarvis_hw_profile_t;

/* 2. Decoupled audio ring frame telemetry (src/jarvis-stt.h)
 * Invariant: `pcm[]` is NEVER notched or downward-expanded by spatial/VAD gates.
 * Spatial & neural VAD telemetry live in parallel arrays for gating & endpointing only.
 */
typedef struct {
    /* ... existing mutex/cond/pcm[JARVIS_RING_SAMPLES] ... */
    float   db[JARVIS_RING_FRAMES];         /* Frame RMS dBFS (after single 80-100 Hz HPF) */
    float   vad_prob[JARVIS_RING_FRAMES];   /* Silero VAD speech probability [0.0, 1.0] */
    float   near_score[JARVIS_RING_FRAMES]; /* Delay-aligned / magnitude stereo coherence [0.0, 1.0] */
    uint8_t is_speech[JARVIS_RING_FRAMES];  /* Combined speech decision */
    uint8_t tts_active[JARVIS_RING_FRAMES]; /* 1 while jarvisd TTS playback or echo tail active */
} jarvis_audio_ring_t;

/* 3. Pluggable STT backend interface (src/jarvis-stt-backend.h)
 * Keeps Whisper and Parakeet-TDT (length-proportional FastConformer) behind one 5-op vtable.
 */
typedef struct jarvis_stt_backend jarvis_stt_backend_t;

typedef struct {
    const char *lang;           /* BCP-47 / ISO-639 ("pt", "en", "auto") */
    const char *prompt;         /* NULL or bounded natural-prose/context seed */
    int         max_tokens;
    int         audio_ctx;      /* 0 = full/native; ignored by length-proportional backends */
    int         beam_size;
    int         n_threads;
    bool        no_context;
    bool        single_segment;
    volatile sig_atomic_t *abort_flag;
} jarvis_stt_req_t;

typedef struct {
    char  text[4096];
    float avg_logprob;
    float max_no_speech_prob;
    int   all_silent;
    int   n_tokens;
    /* Optional token-level timestamps (centiseconds) for LocalAgreement-2 streaming */
    int64_t last_committed_cs;
} jarvis_stt_res_t;

struct jarvis_stt_backend {
    const char *name; /* "whisper", "parakeet-tdt" */
    int  (*transcribe)(jarvis_stt_backend_t *self, const float *pcm, int n_samples,
                       const jarvis_stt_req_t *req, jarvis_stt_res_t *out);
    bool (*supports_lang)(const jarvis_stt_backend_t *self, const char *lang);
    bool (*is_length_proportional)(const jarvis_stt_backend_t *self);
    void (*destroy)(jarvis_stt_backend_t *self);
};

/* 4. Incremental streaming state for LocalAgreement-2 (src/jarvis-stream.h) */
typedef struct {
    int64_t window_start_f;     /* First uncommitted frame in g_audio */
    int64_t committed_end_f;    /* Frame timestamp of last committed token */
    char    committed[16384];   /* Stable emitted transcript */
    char    prev_hyp[4096];     /* Previous partial hypothesis for prefix agreement */
    char    prompt_tail[256];   /* Last ~30 committed words for cross-window continuity */
} jarvis_stream_state_t;
```

---

## 4. Phased Implementation Plan

### Phase 0 — Immediate Accuracy, Anti-Hallucination & Hygiene Fixes

**Goal:** Eliminate the proven transcription-corrupting bugs immediately without requiring new weights, new OS drivers, or protocol changes. Every existing user benefits on the very next build.

**Scope:**
- [`data/ontology.json`](data/ontology.json)
- [`src/jarvis-stt.h`](src/jarvis-stt.h), [`src/jarvis-stt.c`](src/jarvis-stt.c)
- [`src/jarvis-ontology.c`](src/jarvis-ontology.c)
- [`src/jarvisd.c`](src/jarvisd.c)
- [`src/whisper.h`](src/whisper.h) (remove stale copy) & [`Makefile`](Makefile)
- [`tests/test_dsp.c`](tests/test_dsp.c), [`tests/test_jarvis.c`](tests/test_jarvis.c)

**Work Items:**
1. **Remove Static Verb-List `whisper_cmd_prompt` Poisoning**:
   - Clear the comma-separated verb lists from `en` and `pt` in [`data/ontology.json`](data/ontology.json) (default `whisper_cmd_prompt = ""` / `NULL`).
   - Allow optional user override in `~/.jarvisrc` (`whisper_prompt = ...`), and document why verb dumps must never be used as Whisper `initial_prompt`.
2. **Fix `audio_ctx` Floor & `beam_size` Default**:
   - In [`jarvis_stt_compute_audio_ctx`](src/jarvis-stt.c), introduce a configurable/calibrated final floor (`whisper_audio_ctx_min`, default `512` for final commands on iGPU, `0` [full 1500] when `JARVIS_HW_TIER_FAST_GPU` is detected, `256` minimum for partials). Never truncate final commands to `256` on 32-layer turbo encoders.
   - Change default `whisper_beam_size` in [`src/jarvisd.c`](src/jarvisd.c) from `5` to `1` (greedy) for `large-v3-turbo` and iGPU/CPU tiers (still user-overridable via `whisper_beam_size` in `~/.jarvisrc`).
3. **Clean Up [`jarvis_stt_apply_preemphasis_and_agc`](src/jarvis-stt.c)**:
   - Remove the redundant second ~80 Hz HPF when audio already passed through `jarvis-dsp` (keep a clean single-pass DC/80 Hz HPF helper only for raw WAV/bypassed inputs).
   - Remove the destructive `-14 dB` downward expander (`knee_rms = peak_frame_rms * 0.20f`) so unstressed syllables and soft word onsets remain intact across all languages.
   - Replace the $O(N^2)$ insertion sort over `f_rms_arr` with $O(N)$ histogram/quickselect or `qsort` (or remove if percentile is no longer needed).
   - Replace hard clipping at `±0.98f` with smooth soft-knee limiting (`0.98f * tanhf(v / 0.98f)` above `0.85f`).
   - Remove the orphaned declaration `jarvis_stt_run_whisper_slice` in [`src/jarvis-stt.h:74`](src/jarvis-stt.h) (or unify `run_whisper_slice_ex` under that name).
4. **Harden Universal Hallucination & Repetition Defenses**:
   - In [`jarvis_is_non_speech_hallucination`](src/jarvis-ontology.c), add a multi-codepoint periodic loop detector: for period $p \in \{2, 3, 4\}$, if $n \ge 12$ and `cps[i] == cps[i - p]` holds for $\ge 80\%$ of positions, reject as hallucination (catches `"Jajajajaja..."`, `"hahahaha..."`, and repeating CJK/Cyrillic/Arabic glyph loops across all scripts).
   - In [`collapse_repetitions`](src/jarvisd.c), raise max phrase span from `8` words to `24` words (with fast token-hash comparison to stay $O(N)$).
   - Enable Whisper native quality thresholds (`wparams.entropy_thold = 2.4f`, `wparams.logprob_thold = -1.0f`, `wparams.no_speech_thold = 0.6f`) and reject segments where `whisper_full_get_segment_no_speech_prob(ctx, s) > 0.75f` combined with low token probability.
5. **Eliminate Stale `src/whisper.h` Drift**:
   - Delete the hand-copied [`src/whisper.h`](src/whisper.h) and include `whisper.h` directly from `-Isubmodules/whisper.cpp/include` (already passed in [`Makefile:179`](Makefile); mirror in [`tests/Makefile`](tests/Makefile)).

**Verification Gate (Phase 0):**
- `make -C external/libjarvis all && make -C external/libjarvis test` (exit 0, zero warnings).
- New unit tests in `tests/test_jarvis.c` covering 2/3/4-char periodic hallucinations (`"Jajajajajaja..."`, `"hahahaha..."`, `"のののの..."`) and 9–20 word clause repetition collapse.
- Updated `tests/test_dsp.c` verifying soft-limiter headroom, zero onset attenuation, and linear-time AGC scaling on 90 s buffers.
- Live/offline A/B check on real recordings confirming zero prompt leakage (`abre, fecha, muda`) and accurate `calculadora`/`calendário`/`Jarbas`.

---

### Phase 1 — Universal Neural VAD, Cross-Platform Low-Latency Capture & Safe Echo Protection

**Goal:** Eliminate false triggers from non-speech noise (keyboards, coughs, clicks, music), remove 60–100 ms of hidden capture latency on Linux/macOS/Windows, support non-PipeWire Unix/BSD systems, and protect every user from self-echo loops without destroying mic gain.

**Scope:**
- [`src/jarvis-vad.h`](src/jarvis-vad.h), [`src/jarvis-vad.c`](src/jarvis-vad.c) (new minimal abstraction, standalone-tested)
- [`src/jarvis-audio.h`](src/jarvis-audio.h)
- [`src/jarvis-audio-linux.c`](src/jarvis-audio-linux.c), [`src/jarvis-audio-macos.m`](src/jarvis-audio-macos.m), [`src/jarvis-audio-win32.c`](src/jarvis-audio-win32.c)
- [`src/jarvisd.c`](src/jarvisd.c) (`audio_thread_fn`)
- [`tests/test_vad.c`](tests/test_vad.c) (new standalone unit suite)

**Work Items:**
1. **Streaming Silero VAD Abstraction (`jarvis-vad.{h,c}`)**:
   - Wrap `whisper.cpp`'s built-in streaming Silero VAD (`whisper_vad_init_from_file_with_params`, `whisper_vad_detect_speech_no_reset`, `whisper_vad_reset_state`) behind a tiny `jarvis_vad_t` interface operating on 32 ms / 512-sample chunks (at 16 kHz, `< 0.2 ms` on CPU).
   - Auto-discover `ggml-silero-v5.1.2.bin` or `ggml-silero-v6.2.0.bin` in `$XDG_DATA_HOME/whisper/` (or `vad_model` in `~/.jarvisrc`).
   - **Graceful fallback:** if the ~2 MB Silero VAD model is not yet installed, transparently fall back to the existing adaptive energy/ZCR biquad VAD and emit a one-line `jarvis doctor` recommendation (`run 'jarvis setup' to install Silero VAD`).
   - Store per-frame `vad_prob` in `g_audio.vad_prob[]`; require Silero speech confirmation before waking `ctx_wake` or opening an utterance window.
2. **Linux & BSD Audio Capture Overhaul ([`src/jarvis-audio-linux.c`](src/jarvis-audio-linux.c))**:
   - **Step 1a (Immediate):** Honor `pw_target` from `~/.jarvisrc` (`auto` prefers `jarvis_aec_source` only if present and healthy; explicit device name or `default` overrides); pass `--latency 20ms` and native `--format f32` to `pw-record` (adding `jarvis_audio_cap_read_f32()` to `jarvis-audio.h` while keeping `jarvis_audio_cap_read()` for compatibility).
   - **Step 1b (Native PipeWire):** Link or `dlopen("libpipewire-0.3.so.0")` for in-process `pw_stream` capture & playback (zero child processes, true 20 ms float32 zero-quantization buffers, instant hot-plug recovery if Bluetooth/USB mic disconnects).
   - **Step 1c (Universal Unix/BSD Fallback Chain):** Auto-detect at runtime: `libpipewire` → `pw-record` → PulseAudio (`parec` / `pacat`) → ALSA (`arecord` / `aplay`) → OpenBSD `sndio` (`aucat`) / FreeBSD OSS (`/dev/dsp`). Zero hard dependency on PipeWire for non-PipeWire distros or BSDs.
3. **macOS Audio Capture Overhaul ([`src/jarvis-audio-macos.m`](src/jarvis-audio-macos.m))**:
   - Reduce capture buffer size from `CAP_BUF_BYTES = 4096` (64–128 ms) to 20 ms frames (`sample_rate * channels * 2 / 50`), eliminating ~45–100 ms of input lag.
   - Support float32 capture (`kLinearPCMFormatFlagIsFloat`) and opt-in `kAudioUnitSubType_VoiceProcessingIO` (`aec = auto|on|off` in `~/.jarvisrc`) so macOS users get native Apple hardware AEC + noise suppression automatically.
4. **Windows Audio Capture Overhaul ([`src/jarvis-audio-win32.c`](src/jarvis-audio-win32.c))**:
   - Replace legacy `waveIn` (`CAP_BUF_BYTES = 4096`) with event-driven **WASAPI shared mode** (`IAudioClient` / `IAudioCaptureClient`, `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`, 10–20 ms period, native float32) and `AUDCLNT_STREAMOPTIONS_RAW` / Communications APO toggle for built-in Windows AEC.
   - Keep `waveIn` fallback only if WASAPI initialization fails on legacy environments.
5. **Universal Self-Echo & Barge-In Guard**:
   - Tag `g_audio.tts_active[]` while `jarvisd` plays TTS audio plus a calibrated acoustic drain tail (default 350 ms when OS AEC is verified active; 650 ms when no AEC is present).
   - During `tts_active`, disable wake-word-free follow-up triggers and require explicit high-confidence barge-in (wake word or strong near-field + Silero speech burst) so users with laptop speakers and no AEC never enter a self-hearing feedback loop.
   - Keep `followup_ms` and `conv_followup_ms` safely bounded by default when no verified AEC source is active.

**Verification Gate (Phase 1):**
- Standalone `tests/test_vad` passes on speech, silence, white noise, keyboard impulse bursts, and background-music fixtures (music/clicks must yield `vad_prob < 0.25`, speech `> 0.70`).
- Capture latency measured across backends (`<= 25 ms` frame arrival jitter on Linux PipeWire, macOS CoreAudio, Windows WASAPI).
- Hot-unplug / device-switch resilience test (daemon recovers capture stream within 2 s without crashing).

---

### Phase 2 — Universal Front-End DSP (Any Mic, Any Head Angle, Any Room)

**Goal:** Fix off-axis speech attenuation on dual-mic laptops while keeping Whisper's input waveform faithful across headsets, USB mics, lapel mics, and laptop arrays.

**Scope:**
- [`src/jarvis-dsp.h`](src/jarvis-dsp.h), [`src/jarvis-dsp.c`](src/jarvis-dsp.c)
- [`tests/test_dsp.c`](tests/test_dsp.c)

**Work Items:**
1. **Strict Audio / Control Separation in [`jarvis_dsp_process_frame`](src/jarvis-dsp.c)**:
   - Always produce a **clean mono ASR stream**: single 80–100 Hz HPF (zero-ripple biquad or smooth STFT rolloff) + stereo-to-mono delay-aligned sum (for stereo arrays) or direct pass-through (for mono/headsets).
   - Stop applying -26 dB broadband gating and -12 dB HF bin-notching directly to the ASR PCM waveform. Emit `out_near_score` purely as a spatial confidence signal for VAD/barge-in decisions.
2. **Off-Axis-Safe Stereo Coherence (`jarvis-dsp.c`)**:
   - Replace broadside-only `Re(P_LR) / sqrt(P_LL * P_RR)` with **delay-compensated coherence**:
     - Either estimate inter-channel TDOA $\hat{\tau} \in [-200\,\mu\text{s}, +200\,\mu\text{s}]$ via sub-band GCC-PHAT peak search (covers $\pm 60^\circ$ off-axis on 4–10 cm laptop mic arrays) and phase-align $L, R$ before summing + scoring;
     - Or compute **magnitude-squared coherence** $|\gamma_{LR}(f)|^2 = \frac{\text{Re}(P_{LR})^2 + \text{Im}(P_{LR})^2}{P_{LL} P_{RR}}$ across speech bands (`0.5–6.0 kHz`), which rejects uncorrelated diffuse room noise while remaining completely invariant to head rotation and asymmetric WebRTC AEC channel phase!
   - Keep automatic zero-cost mono / mono-summed bypass (`side_ratio_ema` check) intact.
3. **Optional Conditional Neural Denoiser (Low-SNR Rooms Only)**:
   - Design an optional pluggable `jarvis_denoise_t` hook (e.g., RNNoise C library, ~100 KB weights, `< 0.5 ms` per 10 ms frame on CPU, zero GPU contention) controlled by `denoise = auto|on|off` in `~/.jarvisrc`.
   - Under `denoise = auto`, engage **only** when ambient `noise_floor_db > -38 dBFS` or estimated speech SNR `< 12 dB`, and bypass automatically in quiet environments/headsets so clean audio suffers zero neural-denoiser artifacts.

**Verification Gate (Phase 2):**
- Expand [`tests/test_dsp.c`](tests/test_dsp.c) with:
  - Off-axis stereo test ($\tau = 1\text{–}3$ samples, i.e., $62\text{–}187\,\mu\text{s}$, simulating $25^\circ\text{–}60^\circ$ head turn): must maintain unity speech RMS ($\pm 0.5$ dB) and `near_score >= 0.75`.
  - Diffuse uncorrelated far-field noise test: must yield `near_score < 0.30` without distorting concurrent near-field speech.
  - Mono & mono-summed stereo bypass tests: unity passthrough and `near_score == 1.0`.
  - 0 dB / +6 dB SNR speech-in-noise regression test if `denoise=on`.

---

### Phase 3 — Hardware-Adaptive Compute & Multi-Backend STT Engine

**Goal:** Deliver sub-500 ms accurate multilingual STT on **every** class of machine — discrete GPUs, Apple Silicon, integrated Intel/AMD GPUs, and CPU-only laptops — with zero manual tuning.

**Scope:**
- [`Makefile`](Makefile)
- [`src/jarvis-stt-backend.h`](src/jarvis-stt-backend.h), [`src/jarvis-stt-whisper.c`](src/jarvis-stt-whisper.c), [`src/jarvis-stt-parakeet.c`](src/jarvis-stt-parakeet.c) (new modular backends)
- [`src/jarvis-ipc-srv.c`](src/jarvis-ipc-srv.c) (`jarvis_resolve_default_whisper_models`)
- [`src/jarvisd.c`](src/jarvisd.c) (startup hardware probe + calibration)

**Work Items:**
1. **Portable Backend Auto-Detection in [`Makefile`](Makefile)**:
   - Add `JARVIS_BACKEND ?= auto` (`auto | cuda | vulkan | metal | cpu`):
     - `Linux`: prefer system CUDA (`nvcc` + `libcudart`) if available and `JARVIS_BACKEND=cuda` (or optional multi-backend GGML dynamic load `GGML_BACKEND_DL=ON` so one binary supports CUDA, Vulkan, and CPU automatically!); otherwise prefer system Vulkan (`pkg-config vulkan` + `glslc`) **without** forcing a 200 MB LunarG tarball download; fall back cleanly to CPU (`-DGGML_OPENBLAS=ON` if available, native SIMD/OpenMP) if no GPU SDK is present.
     - `Darwin`: Metal (`-DGGML_METAL=ON`) + Accelerate.
     - `Windows`: auto-detect CUDA / Vulkan SDK / CPU cleanly.
     - `FreeBSD/OpenBSD/NetBSD`: clean CPU/BLAS build with zero Linux-specific Vulkan assumptions.
2. **Startup Hardware Probe & Latency Calibration (`jarvis_hw_profile_t`)**:
   - During `jarvisd` warmup (which already runs at [`src/jarvisd.c:4128-4147`](src/jarvisd.c)):
     - Query active GGML backend (`CUDA`, `Metal`, `Vulkan`, `CPU`) and device name.
     - Time a single `audio_ctx = 512` pass and (on GPU) a `1500` pass (skipping `1500` if `512` already exceeds `900 ms`).
     - Classify host into `FAST_GPU` (`ac1500 <= 650 ms` → `final_audio_ctx = 0`), `IGPU` (`ac512 <= 900 ms` → `final_audio_ctx = 512` or `768`, `beam_size = 1`), or `CPU` (`final_audio_ctx = 512` with lean model warning if `large-v3-turbo` exceeds 1.5 s).
     - Log the calibrated profile once at startup and expose it over `STATUS` IPC so `jarvis status` and `jarvis doctor` display exact hardware tier and measured decode speed.
3. **First-Class Length-Proportional STT Backend: `Parakeet-TDT-0.6B-v3`**:
   - `submodules/whisper.cpp/include/parakeet.h` is already present in the submodule!
   - Implement the 5-op `jarvis_stt_backend_t` vtable with two built-in backends:
     - `jarvis-stt-whisper.c`: wraps `whisper.cpp` (all 99+ languages, `large-v3-turbo-q8_0`, `large-v3-q5_0`, `small-q8_0`, PT-PT fine-tunes like `WhisperLv3-EP`).
     - `jarvis-stt-parakeet.c`: wraps `parakeet.h` (`parakeet-tdt-0.6b-v3`, 25 European languages including `en`, `pt`, `es`, `fr`, `de`, `it`, `nl`, `pl`, `ru`, `uk`, `sv`, `da`, `fi`, `cs`, `ro`, `hu`, `el`, etc.).
   - **Why Parakeet-TDT is transformative for "works for everyone":** FastConformer-TDT scales **linearly with utterance duration** (a 2.5 s voice command does 2.5 s of encoder work, not 30 s of zero-padded Whisper attention). That eliminates the entire `audio_ctx` vs accuracy dilemma on integrated GPUs and modern CPUs, and eliminates Whisper-style silence hallucination loops.
   - Auto-select Parakeet-TDT when installed and `supports_lang(lang)` is true; transparently use Whisper for non-covered languages (Japanese, Chinese, Korean, Arabic, Hindi, Hebrew, Thai, Vietnamese, etc.) or when user pins `stt_backend = whisper` / `whisper_model = ...`.
4. **Tier-Aware Model Auto-Selection (`jarvis_resolve_default_whisper_models`)**:
   - Prefer `q8_0` over `q5_0` on `FAST_GPU` (reduces 4-layer turbo quantization error on numbers & short syllables); prefer `parakeet-tdt-0.6b-v3` or `large-v3-turbo-q5_0` on `IGPU`; prefer `parakeet-tdt-0.6b-v3` or `small-q8_0` on `CPU`.
   - Support optional per-locale model slots (e.g., `whisper_model.pt-pt` for regional fine-tuned GGML checkpoints) without touching C code.

**Verification Gate (Phase 3):**
- Clean builds verified on CPU-only (`JARVIS_BACKEND=cpu`), Vulkan (`JARVIS_BACKEND=vulkan`), and CUDA/Metal where hardware is available.
- `jarvis_stt_backend_t` unit tests + offline benchmark comparing Whisper (`q5_0`, `q8_0`, `ac=512`, `ac=0`) vs `Parakeet-TDT-0.6B-v3` on WER and median/p95 latency across both iGPU and CPU.

---

### Phase 4 — Low-Power Neural Wake-Word & Adaptive Syntactic Endpointing

**Goal:** Drop idle CPU/GPU wake-polling overhead to near zero while supporting custom wake words in every language, and stop chopping off commands during natural mid-sentence thinking pauses.

**Scope:**
- [`src/jarvis-kws.h`](src/jarvis-kws.h), [`src/jarvis-kws.c`](src/jarvis-kws.c) (new optional ONNX KWS abstraction)
- [`data/ontology.json`](data/ontology.json) (`continuation_tokens` per locale)
- [`src/jarvis-ontology.c`](src/jarvis-ontology.c), [`include/ttypt/jarvis-types.h`](include/ttypt/jarvis-types.h)
- [`src/jarvisd.c`](src/jarvisd.c) (`handle_jarvis_wait_ex`)

**Work Items:**
1. **Tiered Wake-Word Engine (`jarvis-kws.{h,c}`)**:
   - **Tier 1 (Zero new models, immediate via Phase 1):** Strictly gate `ctx_wake` execution on Silero VAD speech segments (`vad_prob >= 0.55`) instead of running Whisper every 320 ms on ambient noise.
   - **Tier 2 (Sub-millisecond ONNX KWS):** Reuse `libonnxruntime` (already loaded by [`src/jarvis-tts.c`](src/jarvis-tts.c)) to run lightweight `< 1 ms` neural keyword spotting (openWakeWord / microWakeWord / Sherpa-ONNX KWS) when a matching `.onnx` wake model is installed for the active `wake_word`.
   - **Universal Fallback:** If the user configures an arbitrary custom wake word in any of the 31 languages without an `.onnx` KWS model, seamlessly fall back to Silero-gated `ctx_wake` (Whisper/Parakeet).
2. **Adaptive, Language-Driven Endpointing**:
   - Move hard-coded early-endpoint thresholds (`spec_silence_f = 280 ms`, `spec_early_f = 520 ms`) to an adaptive state machine driven by:
     1. Silero VAD continuous speech-end probability (distinguishes brief stop-consonant closures from true pauses).
     2. Locale-specific `continuation_tokens` in [`data/ontology.json`](data/ontology.json) (e.g., `en`: `"to"`, `"for"`, `"with"`, `"and"`, `"the"`, `"in"`; `pt`: `"para"`, `"com"`, `"de"`, `"do"`, `"da"`, `"a"`, `"o"`, `"e"`, `"que"`, `"em"`; etc. — zero hardcoded words in C). If the current hypothesis ends in a continuation token, extend early-endpoint grace by `+450 ms` up to `cmd_silence_ms`.
     3. Optional push-to-talk / hold-to-talk mode (`JARVIS_PTT_START` / `JARVIS_PTT_STOP`) for users in loud offices who want deterministic zero-false-trigger invocation.

**Verification Gate (Phase 4):**
- Idle CPU/GPU usage during 60 s of ambient room noise drops to `< 2%` CPU / `0%` GPU wake invocations.
- Unit tests in `tests/test_jarvis.c` verifying `continuation_tokens` parsing across locales and endpoint grace extension on trailing prepositions/conjunctions.

---

### Phase 5 — True Real-Time Streaming & Long-Form Dictation (`handle_stream`)

**Goal:** Make dictation and long multi-clause commands flicker-free, bounded in latency ($O(N)$ instead of $O(N^2)$), dialect-accurate, and unlimited in duration (no 30 s cliff).

**Scope:**
- [`src/jarvis-stream.h`](src/jarvis-stream.h), [`src/jarvis-stream.c`](src/jarvis-stream.c) (standalone-tested LocalAgreement-2 engine)
- [`src/jarvisd.c`](src/jarvisd.c) (`handle_stream`)
- [`data/ontology.json`](data/ontology.json) (optional natural-prose `dictation_seed` per regional dialect)
- [`tests/test_stream.c`](tests/test_stream.c) (new unit suite)

**Work Items:**
1. **LocalAgreement-2 Incremental Commitment (`jarvis-stream.{h,c}`)**:
   - Maintain `jarvis_stream_state_t`: compare each new partial hypothesis against `prev_hyp` at word boundaries; commit the longest agreeing prefix whose token end-timestamp (`whisper_full_get_token_t1` or Parakeet word end time) sits safely behind the live audio edge (`>= 600 ms` margin).
   - Advance `window_start_f` past `committed_end_f` so decode cost stays bounded to a ~5–15 s active window instead of growing quadratically across the utterance.
   - Feed the last ~30 committed words (`prompt_tail`) into the next window with `no_context = false`, preserving punctuation, casing, and terminology cleanly across 30 s boundaries for multi-minute dictations.
2. **Regional Dialect Conditioning & Dynamic Active-Context Vocabulary**:
   - Support regional dialect codes (`pt-PT` vs `pt-BR`, `en-GB` vs `en-US`, `es-ES` vs `es-MX`, `fr-FR` vs `fr-CA`, `zh-CN` vs `zh-TW`) in [`data/ontology.json`](data/ontology.json) with **natural 1-sentence prose** `dictation_seed` (never verb lists) used only when `mode=type|clip` and audio SNR is healthy, preventing `pt-PT → pt-BR` drift during dictation.
   - Optional bounded dynamic context biasing: inject only proper nouns / active window titles (capped at <= 16 tokens, strictly disabled on low-SNR frames) so users referring to project/app names get accurate spelling without prompt leakage.

**Verification Gate (Phase 5):**
- Standalone `tests/test_stream` verifying prefix agreement, timestamp window advancement, zero duplicate/dropped words across simulated 90 s (3×30 s) audio streams, and `prompt_tail` truncation safety.
- End-to-end streaming test on a >45 s recording confirming bounded per-partial latency and zero dropped text past the 30 s boundary.

---

### Phase 6 — Turnkey Setup, Hardware/Audio Diagnostics, Regression Bench & `IDEAS.md` Convergence

**Goal:** Guarantee that a brand-new user on any machine can run `jarvis setup` → `jarvis doctor` → `jarvis start` and immediately get great accuracy, and that future commits can never silently regress WER or latency.

**Scope:**
- [`src/jarvis.c`](src/jarvis.c) (`cmd_setup`, `cmd_doctor`)
- [`tests/bench_stt.c`](tests/bench_stt.c) & [`tests/Makefile`](tests/Makefile) (`make bench-stt`)
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), [`README.md`](README.md)
- Convergence with [`IDEAS.md`](IDEAS.md) (language-agnostic Tier-0 + in-process `libmm`)

**Work Items:**
1. **Reproducible Offline STT Accuracy & Latency Benchmark (`make bench-stt`)**:
   - Build a lightweight C harness (`tests/bench_stt.c`) that ingests a manifest (`wav_path\tlang\tground_truth`) and outputs per-clip and aggregate:
     - **WER** (Word Error Rate) & **CER** (Character Error Rate, diacritic-aware and folded)
     - **Latency** (median, p95, RTF — Real-Time Factor)
     - **Hallucination rate** on silence/noise/music-only clips
   - Support running against multiple configs (`whisper:ac512:g1`, `whisper:ac0:g1`, `parakeet-tdt`) in one command for objective before/after proof.
2. **Smart `jarvis doctor` Diagnostics ([`src/jarvis.c`](src/jarvis.c))**:
   - **Compute check:** Print active GGML backend (`CUDA`/`Metal`/`Vulkan`/`CPU`), calibrated `jarvis_hw_profile_t` tier, and measured decode latency. On Linux, scan `/sys/bus/pci/devices` for NVIDIA/AMD GPU PCI classes with no bound kernel driver (`driver` symlink absent) and warn the user with an exact fix hint (catches idle discrete GPUs like unbound NVIDIA T550s immediately).
   - **Live 2-second Audio & AEC Health Check (`jarvis doctor --mic`):** Measure mic RMS, peak, DC offset, clipping rate, stereo channel independence, and—if PipeWire AEC (`jarvis_aec_source`) is active—compare raw hardware peak vs AEC peak so severe AEC attenuation (e.g., >12 dB gain crush) is caught and reported automatically instead of silently starving VAD.
   - **Model & Pipeline Check:** Verify STT model language compatibility, Silero VAD availability, TTS offline fallback readiness, and socket health.
3. **One-Command `jarvis setup` ([`src/jarvis.c`](src/jarvis.c))**:
   - Probe OS, hardware tier, and active locale (`lang`), then offer/download checksum-verified models:
     - Best STT model for detected hardware tier & language (e.g., `parakeet-tdt-0.6b-v3` or `ggml-large-v3-turbo-q5_0/q8_0` or `ggml-small-q8_0`)
     - `ggml-silero-v5.1.2.bin` (~2 MB VAD model)
     - Matching offline TTS model (Piper / Kokoro ONNX)
   - Install OS-native AEC config safely only after verifying it does not crush mic headroom.
4. **Update Docs & Converge with [`IDEAS.md`](IDEAS.md)**:
   - Keep [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) strictly synchronized with actual implementation (no claiming Silero VAD or config keys that aren't wired).
   - Execute [`IDEAS.md`](IDEAS.md) Phases 1–3 alongside Phases 4–6 here:
     - In-process `libcorm` + WASM-ready `libmm`
     - Zero hardcoded English strings in `libjarvis.c` / `jarvis-ontology.c` / solver (100% data-driven via `ontology.json`)
     - Epistemic self-learning workflow recall via `libmm`

---

## 5. Master Execution Matrix

| Phase | Headline Deliverable | Primary Users Unblocked | Risk | Depends On | Tier-0 Verification Gate |
|---|---|---|---|---|---|
| **Phase 0** | Kill prompt leakage, fix `audio_ctx` floor & greedy default, remove destructive expander, harden hallucination filters, remove stale `src/whisper.h` | **All existing users immediately** | Low | None | `make test` + new hallucination/AGC unit tests + A/B WAV check |
| **Phase 1** | Streaming Silero VAD, 20 ms float32 native capture (PipeWire/Pulse/ALSA/BSD, macOS, WASAPI), native OS AEC + echo guard | **Linux, BSD, macOS, Windows users; noisy rooms; speaker users** | Medium | Phase 0 (`whisper.h` sync) | `tests/test_vad` + latency/hotplug tests |
| **Phase 2** | Decouple ASR PCM from spatial gate; off-axis-safe GCC-PHAT / magnitude coherence; conditional RNNoise | **Dual-mic laptop users (off-axis), headsets, noisy cafes** | Medium | Phase 1 | `tests/test_dsp` off-axis ($\tau = 1\text{–}3$), diffuse-noise, mono-bypass |
| **Phase 3** | Portable `JARVIS_BACKEND` matrix, startup `jarvis_hw_profile_t` calibration, `jarvis_stt_backend_t` + `Parakeet-TDT-0.6B-v3` | **iGPU, CPU-only, CUDA, Metal & multilingual European users** | Medium–High | Phase 0 | Multi-backend build + `make bench-stt` WER/RTF matrix |
| **Phase 4** | Silero-gated wake → `<1 ms` ONNX KWS + locale-driven `continuation_tokens` adaptive endpointing + PTT | **Battery/laptop users, custom wake words, thoughtful speakers** | Medium | Phase 1 | Idle CPU/GPU test + `tests/test_jarvis` endpointing tests |
| **Phase 5** | LocalAgreement-2 streaming (`handle_stream`), >30 s continuity, dialect seeds, bounded active-window context | **Long-form dictation & regional dialect (`pt-PT`, `en-GB`, etc.) users** | Medium–High | Phase 0, 3 | `tests/test_stream` (>45 s & 90 s stream tests) |
| **Phase 6** | `make bench-stt` regression suite, `jarvis doctor` (GPU/AEC/mic probes), turnkey `jarvis setup`, [`IDEAS.md`](IDEAS.md) convergence | **All new users & long-term maintainers** | Low–Medium | Phases 0–5 | `make test` + `make bench-stt` + `jarvis doctor` clean pass |

---

## 6. Success Metrics ("Done" Criteria)

When Phases 0–6 are complete, `libjarvis` meets these measurable bars across all target environments:

- **Accuracy:** Zero prompt-token hallucinations (`0%` prompt leakage); `< 0.5%` false wake/hallucination rate on 10 minutes of ambient noise, typing, and background music; parity or better WER/CER on off-axis ($30^\circ$) vs on-axis speech.
- **Latency:** Capture-to-ring buffering `<= 25 ms` on Linux, macOS, and Windows; end-of-speech to final transcript `<= 450 ms` on discrete GPU / Apple Silicon (full context) and `<= 650 ms` on integrated GPU / modern CPU (via Parakeet-TDT or calibrated Whisper).
- **Portability:** Builds cleanly out of the box with `make PREFIX="$HOME/.local" all` on Linux (CUDA, Vulkan, or CPU-only without mandatory SDK downloads), macOS (Metal), Windows (WASAPI), and BSD (CPU/sndio/OSS).
- **Zero-Config Ergonomics:** A fresh user runs `jarvis setup && jarvis doctor && jarvis start` and gets working wake-word, commands, dictation, and echo-safe TTS with at most one optional `lang = <locale>` setting in `~/.jarvisrc`.
