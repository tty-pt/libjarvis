# Changelog

All notable changes to `libjarvis` (`@tty-pt/jarvis`) are documented in this file.

## [1.2.0] - 2026-11-23

### Added
- **Zero-Config `auto` Defaults & One-Line Language Switching (`src/jarvisd.c`, `src/jarvis-tts.c`, `src/jarvis-ontology.c`, `src/jarvis.c`, `data/ontology.json`)**:
  - `lang = auto` resolves system locale (`LC_ALL`/`LC_MESSAGES`/`LANG`, defaulting to `en`), and switching `lang = pt-pt` (or `en-us`, `es`, `fr`, `de`, `it`) automatically configures Whisper STT language, `ontology.json` wake-word/phrases/UI strings, and neural TTS voice without per-feature overrides.
  - Added `tts_voice = local` (`JARVIS_VOICE=local`) to resolve local Piper ONNX (`piper:pt_PT-tugao-medium`, `piper:gb`, `piper:us`) or Kokoro fallback (`pm_tuga`, `bm_daniel`) with zero network calls.
  - Added `pw_target = auto` (`jarvis_aec_source` auto-detection with `data/99-jarvis-aec.conf` PipeWire drop-in), `wm = auto`, and `whisper_model = auto` discovery.
  - Added `jarvis restart`, `jarvis doctor`, `jarvis setup`, and `jarvis locales` CLI subcommands.
- **Persistent Edge-TTS Worker & 32-Slot LRU PCM Cache (`src/jarvis-tts.c`, `scripts/edge-tts-pcm`)**:
  - Replaced per-clause Python subprocess spawning with a persistent `edge-tts-pcm --serve` framing worker and 32-slot in-memory PCM LRU cache (`<5ms` repeat-phrase synthesis) with bounded timeout fallback to local ONNX.
- **Standalone Desktop & Window Manager Modules (`mods/ren-jarvis/`, `mods/desktop/`, `mods/x11/`, `mods/firefox/`, `mods/sway/`, `mods/hyprland/`)**:
  - Consolidated desktop automation into `ren-jarvis.so`, `ren-desktop.so`, `ren-x11.so`, `ren-firefox.so`, `ren-sway.so`, and `ren-hyprland.so` under `mods/`, with `browser` `open_url` load-wait/read and `window_list` / screen geometry reporting.

### Changed
- **End-to-End Voice Latency & `ren`-Only Dispatch (`src/jarvisd.c`, `src/jarvis.c`)**:
  - Completed the `ren`-only architecture over `/tmp/ren-jarvis-$USER.sock` with thin `bin/jarvis` CLI and removed legacy duplicate `libjarvis` agent/solver stacks.
  - Added dynamic `whisper_full_params.audio_ctx` sizing (rounded up from captured speech frames instead of fixed `1500` frames) for ~2–3x faster short-command Whisper decoding.
  - Lowered default `cmd_silence_ms` from `900ms` to `750ms` (with `spec_early_f` floor adjusted from `28` to `25` frames = `500ms`) for snappier VAD endpointing without clipping pauses.
  - Replaced `popen()` / `system()` shell invocations in `jarvisd` wake-time speaker ducking/restoration (`wpctl`) and PipeWire source discovery (`pactl`) with direct zero-shell `fork()`/`execvp()` helpers, and replaced hardcoded user/home fallbacks across `jarvisd.c`, `jarvis.c`, `libjarvis.c`, and `jarvis-tts.c` with thread-safe `getpwuid_r()` lookups.
  - Restored clean `SIGCHLD` child reaping and fixed follow-up barge-in cancellation across turns.

## [1.1.0] - 2026-11-20

### Added
- **Sway & Hyprland Window Manager Modules (`mods/sway/mod_sway.c`, `mods/hyprland/mod_hyprland.c`)**:
  - Dedicated `libxylem` dynamic window manager plugins for Sway (`swaymsg`) and Hyprland (`hyprctl`) supporting workspace switching, window movement, fullscreen/floating toggles, focus navigation, and layout inspection.
- **Browser Accessibility, Terminal Screen Inspection & Continuous Multi-Turn Interaction (`libjarvis.c`, `jarvis.c`, `jarvis-cli-adapter.c`)**:
  - Browser element/link accessibility targeting (`browser.click`, hint/text matching) and live terminal pane screen inspection (`terminal.inspect`).
  - Continuous multi-turn dialogue state (`jarvis_should_continue_listen`) for follow-up questions and clarifications without repeating the wake word.
  - Spoken cardinal (`"one"`..`"nine"`) and ordinal (`"first"`..`"ninth"`) workspace resolution in Tier-0 Intent IR compilation (`"switch to workspace two"`, `"move this window to workspace three"`).

### Changed
- **Tier-0 Compositional Solver Disambiguation & Ontology Hardening (`jarvis-solver.c`, `jarvis-ontology.c`)**:
  - Granted surface-agnostic context bonus (`+0.04`) for capabilities with `surface: "any"`, satisfied `required_any` constraints, or numeric `ui.scroll` line counts, and boosted `workspace.switch` disambiguation (`0.99`) so cross-surface commands clear the `0.90` Tier-0 confidence threshold without falling back to LLM round-trips.
  - Extracted scroll direction (`up`/`down`/`left`/`right`) directly from composite multi-word lead verb spans (e.g., `"scroll up 8"`).
  - Protected built-in ontology entity phrases (such as `"other"` -> `ui.relative_window`) from being shadowed by bare `$HOME` subdirectory names during live filesystem indexing, and normalized mid-word apostrophes (`"let's"` -> `"lets"`) before stopword stripping.
- **Remote LLM Default & Fallback Deduplication (`libjarvis.c`)**:
  - Defaulted remote `axil-auren` inference to the fast `qwen3.5-4b` model and deduplicated the fallback model chain.
  - Guarded native `<time.h>` calls in `libjarvis.c` so freestanding WebAssembly (`npm/dist/jarvis.wasm`) compiles cleanly under `clang --target=wasm32`.

## [1.0.0] - 2026-11-19

### Added
- **Universal Compositional Semantic Solver & Ontology (`jarvis-ontology.c`, `jarvis-solver.c`)**:
  - Zero-dependency pure C semantic ontology parser (`ontology.json` + built-in canonical fallback for WASM and zero-config environments).
  - Vocabulary-driven ASR post-processor (`jarvis_ontology_normalize_asr`) with trailing prefix-echo truncation, compound token folding (`"low fee"` -> `"lofi"`), and phonetic verb snapping.
  - Sub-millisecond compositional clause solver (`jarvis_solve_clause`) and cross-platform Intent IR -> Action DSL compiler (`jarvis_compile_ir_to_dsl`).
- **DSL JSON Normalization, Healing & Streaming TTS (`jarvis-json.c`)**:
  - Standalone `jsmn.h`-backed JSON AST parser and serializer across all native OS targets and WebAssembly.
  - LLM output normalizer (`jarvis_normalize_dsl_json`) resolving operation aliases (`run_command`, `goto_workspace`, `launch_app`, etc.) and misplaced fields.
  - Incremental streaming answer prefix extractor (`jarvis_extract_streaming_answer_prefix`) and ~1.0s TTFA clause splitter (`jarvis_pop_ready_tts_clause`).
- **Multi-Clause Chainer, Context, Memory, Safety Guardrails & ReAct Agent (`libjarvis.c`)**:
  - Compound utterance clause splitter (`jarvis_split_clauses`) protecting `"and press enter"`, `"screen and audio"`, and dictation spans.
  - Spatial and conversational anaphora resolution (`jarvis_resolve_anaphora`), persistent workflow memory (`jarvis_memory_remember`, `jarvis_memory_recall_json`), and multi-turn ReAct observation-reasoning-action loop backed by `axil-auren` remote inference with deterministic offline fallback.
  - Destructive shell command detection (`jarvis_is_destructive_command`) and `jarvis_xy_confirm` confirmation gate.
- **Voice Pipeline (`jarvis-voice.c`)**:
  - 16 kHz float32 circular ring buffer and adaptive noise-floor VAD (`jarvis_vad_create`, `jarvis_vad_push_frame`).
  - Wake-word command extractor (`jarvis_voice_extract_wake_command`), lockdown phrase detector, and pure-C WAV earcon chime synthesizer (`jarvis_voice_write_earcon_wav`).
- **Xylem Environment Adapters & Universal `@tty-pt/jarvis` NPM Package**:
  - `libxylem` hook contracts (`jarvis_xy_observe`, `jarvis_xy_execute`, `jarvis_xy_confirm`, `jarvis_xy_speak_clause`, `jarvis_xy_tts_interrupt`, `jarvis_xy_voice_status`) following the two-header rule (`<ttypt/jarvis.h>` and `<ttypt/jarvis-types.h>`).
  - Universal colocated Node-API native addon (`npm/native/jarvis_napi.c`) and WebAssembly reactor (`npm/dist/jarvis.wasm`) mounted on the `@tty-pt/stele` shared-memory substrate.
