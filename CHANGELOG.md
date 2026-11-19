# Changelog

All notable changes to `libjarvis` (`@tty-pt/jarvis`) are documented in this file.

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
