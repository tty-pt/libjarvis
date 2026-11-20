# libjarvis

[![npm](https://img.shields.io/badge/npm-@tty--pt/jarvis-CB3837?logo=npm)](npm/README.md)
[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](LICENSE)
[![Tier-0 Solver](https://img.shields.io/badge/solver-Tier--0%20~0.1ms-16A34A)](#)
[![ReAct Agent](https://img.shields.io/badge/agent-ReAct%20%2B%20Memory-blueviolet)](#)
[![Voice Pipeline](https://img.shields.io/badge/voice-VAD%20%2B%20Streaming%20TTS-4B8BBE)](#)

> Universal Compositional Semantic Solver, Cross-Platform Intent Intermediate Representation (Intent IR), Multi-Turn ReAct Agent Orchestrator, Low-Latency Voice Pipeline, and `libxylem` Environment Adapters in pure C99 and WebAssembly. Canonical headers are `<ttypt/jarvis.h>` and `<ttypt/jarvis-types.h>`.

---

## 1. Quickstart

### The `jarvis` CLI Tool (< 30 Seconds)
Solve and execute natural language commands across desktop window managers (`dwm`, `sway`, `hyprland`), terminals (`tmux`, POSIX shell), browsers (`qutebrowser`, `firefox`), web applications (`site`), and game worlds (`NeverDark`):

```bash
# 1. Deterministic Tier-0 command execution (~0.1 ms, zero LLM round-trip):
jarvis "switch to workspace two and open a terminal there"
jarvis "let's listen to some miles davis"
jarvis "scroll up 8"

# 2. Dry-run inspection (prints compiled Intent IR + Action DSL JSON without executing):
jarvis --dry-run "move this window to workspace three"

# 3. Teach a persistent reusable workflow and invoke it later:
jarvis "remember morning setup as switch to workspace 2 and open a terminal"
jarvis "run morning setup"

# 4. Multi-turn observation & reasoning across terminal panes and browser accessibility:
jarvis "what error is showing in the other terminal"
jarvis "open that file at the failing line"
```

### In-Memory C API
Deterministically solve a natural language utterance into Intent IR and Action DSL, or run the full stateful orchestrator in under 20 lines:

```c
#include <stdio.h>
#include <ttypt/jarvis.h>

int main(void) {
    jarvis_config_t cfg;
    jarvis_config_init(&cfg);
    cfg.dry_run = true;

    jarvis_engine_t *eng = jarvis_engine_create(&cfg);
    jarvis_process_result_t res;

    if (jarvis_process(eng, "switch to workspace two and open a terminal", "any", &res)) {
        for (size_t i = 0; i < res.step_count; i++) {
            printf("Step %zu: clause=\"%s\" -> op=%s (score=%.2f)\n",
                   i, res.steps[i].resolved_clause,
                   res.steps[i].dsl.op, res.steps[i].ir.score);
        }
    }

    jarvis_engine_destroy(eng);
    return 0;
}
```

Compile and link:
```sh
cc demo.c $(pkg-config --cflags --libs jarvis) -o demo
```

---

## 2. Architecture & Compositional Pipeline

`libjarvis` decouples natural language understanding from environment-specific execution through a four-stage pipeline and `libxylem` (`XY_DECL` / `XY_IMPL`) dynamic hooks:

```
Utterance ("switch to workspace two and open a terminal there")
  │
  ├─► 1. Clause Splitter & Anaphora Resolver (jarvis_split_clauses, jarvis_resolve_anaphora)
  │      Splits compound junctions while protecting "and press enter" & dictation spans;
  │      propagates deictic references ("it", "that", "there") across clauses and turns.
  │
  ├─► 2. Tier-0 Deterministic Compositional Solver (~0.1 ms, jarvis_solve_clause)
  │      Matches normalized ASR phrases against live ontology + environment entities
  │      (windows, tabs, directories, apps, spoken numbers) -> Platform-Neutral Intent IR.
  │
  ├─► 3. Tier-1 / Tier-2 ReAct Agent Fallback (axil-auren remote inference, qwen3.5-4b)
  │      Invoked only for out-of-distribution queries or multi-step diagnostics
  │      (e.g. inspecting live terminal tracebacks or browser AT-SPI2 accessibility trees).
  │
  └─► 4. Xylem Environment Adapters (jarvis_xy_observe, jarvis_xy_execute, jarvis_xy_confirm)
         Dispatches compiled Action DSL to loaded modules (dwm, mod_sway.so, mod_hyprland.so,
         browser, tmux, site, NeverDark) with destructive-command confirmation guardrails.
```

### The `libxylem` Two-Header Rule
To enforce strict encapsulation across dynamically loaded `.so` adapter modules:
1. **`<ttypt/jarvis-types.h>` (Shared Types Header)**: Defines `jarvis_world_t`, `jarvis_ir_t`, `jarvis_dsl_t`, `jarvis_adapter_result_t`, and `jarvis_context_t`. Adapter plugins (such as `mods/sway/mod_sway.c` and `mods/hyprland/mod_hyprland.c`) include `<ttypt/jarvis-types.h>` and `<ttypt/xy-mod.h>` to implement `XY_IMPL(int, jarvis_xy_execute, ...)`.
2. **`<ttypt/jarvis.h>` (Caller & Hook Declaration Header)**: Declares `XY_DECL` hooks and orchestrator functions for host applications.

---

## 3. Core API Reference

| Function / Hook | Signature | Description |
| :--- | :--- | :--- |
| `jarvis_engine_create` | `jarvis_engine_t *jarvis_engine_create(const jarvis_config_t *cfg)` | Initialize stateful orchestrator, ontology, context, and workflow memory |
| `jarvis_process` | `bool jarvis_process(eng, utterance, surface, &out_res)` | End-to-end utterance execution (Solver $\rightarrow$ ReAct $\rightarrow$ Xylem Adapter) |
| `jarvis_solve_clause` | `bool jarvis_solve_clause(ont, clause, surface, world, &out_ir)` | Deterministic ~0.1 ms compositional clause solver producing `jarvis_ir_t` |
| `jarvis_compile_ir_to_dsl` | `bool jarvis_compile_ir_to_dsl(ir, surface, ctx, world, &out_dsl)` | Compile platform-neutral Intent IR into executable `jarvis_dsl_t` |
| `jarvis_split_clauses` | `size_t jarvis_split_clauses(ont, utterance, out_clauses, max)` | Split multi-clause utterances while preserving `"and press enter"` and quotes |
| `jarvis_normalize_dsl_json` | `bool jarvis_normalize_dsl_json(json_str, &out_dsl)` | Heal LLM JSON operation aliases (`run_command`, `goto_workspace`) into canonical DSL |
| `jarvis_memory_remember` | `bool jarvis_memory_remember(eng, key, value)` | Persist a user-taught workflow or preference into long-term memory |
| `jarvis_vad_push_frame` | `bool jarvis_vad_push_frame(vad, frame_samples, count)` | Push 20 ms 16 kHz PCM frame into circular ring buffer and update adaptive VAD |
| `jarvis_pop_ready_tts_clause` | `bool jarvis_pop_ready_tts_clause(unspoken, first, clause, ...)` | Segment streaming LLM answers into ~1.0s TTFA spoken clauses |
| `jarvis_xy_observe` | `XY_DECL(int, jarvis_xy_observe, jarvis_world_t *, world)` | Xylem hook for environment adapters to populate live windows, tabs, and panes |
| `jarvis_xy_execute` | `XY_DECL(int, jarvis_xy_execute, const jarvis_dsl_t *, dsl, ...)` | Xylem hook for environment adapters (`dwm`, `sway`, `hyprland`, `site`) to run DSL ops |
| `jarvis_xy_confirm` | `XY_DECL(int, jarvis_xy_confirm, const char *, description)` | Xylem confirmation gate triggered before destructive shell commands |

---

## 4. Low-Latency Voice Pipeline & Window Manager Modules

- **Adaptive VAD & Wake-Word Pipeline (`jarvis-voice.c`)**:
  - 16 kHz `float32` circular ring buffer with pre-speech lookback (`jarvis_vad_get_samples`), adaptive noise-floor tracking (`jarvis_vad_push_frame`), wake-word command extraction (`jarvis_voice_extract_wake_command`), lockdown phrase detection, and zero-dependency WAV earcon chime synthesis (`jarvis_voice_write_earcon_wav`).
  - Incremental streaming TTS answer extractor (`jarvis_extract_streaming_answer_prefix`) and prosodic clause splitter (`jarvis_pop_ready_tts_clause`) delivering ~1.0s Time-To-First-Audio while keeping the microphone active for live barge-in.
- **Pluggable Window Manager Modules (`mods/sway/mod_sway.so`, `mods/hyprland/mod_hyprland.so`)**:
  - Built-in X11/`dwm` adapter plus dynamically loadable Wayland `libxylem` plugins for **Sway** (`swaymsg` IPC) and **Hyprland** (`hyprctl` IPC), supporting workspace switching, window movement, fullscreen/floating layout toggles, directional focus, and live client tree observation.

---

## 5. English & Portuguese Language Support (`en` / `pt`)

`libjarvis` and its companion voice pipeline (`dwm-whisperd`, `dwm-voice`, `mod_tts.so`) support both **English (`en`)** and **Portuguese (`pt`)** speech recognition, natural language execution, and neural voice synthesis:

### 1. Automatic Keyboard-Synchronized or Explicit Language Selection
Configure the active spoken language in `~/.config/dwm-voice/lang` (or via the `dwm-voice menu` $\rightarrow$ **Whisper Language** selector):
- **`kbd` (Default — Keyboard Layout Sync)**: Automatically follows your active keyboard layout (`Mod+Space` toggles between `US — English` and `PT — Português`), routing speech recognition to `"en"` or `"pt"` dynamically.
- **`en` (English)**: Forces English speech recognition and command processing:
  ```bash
  mkdir -p ~/.config/dwm-voice
  printf 'en\n' > ~/.config/dwm-voice/lang
  ```
- **`pt` (Portuguese — Português)**: Forces Portuguese speech recognition and multilingual command processing:
  ```bash
  mkdir -p ~/.config/dwm-voice
  printf 'pt\n' > ~/.config/dwm-voice/lang
  ```
- **`auto` (Multilingual Auto-Detect)**: Lets Whisper detect the spoken language per utterance (`printf 'auto\n' > ~/.config/dwm-voice/lang`).

### 2. English vs. Multilingual (Portuguese) Whisper Models
Place quantized GGML Whisper models in `~/.local/share/whisper/` and select the active model in `~/.config/dwm-voice/model`:
- **English-Only Ultra-Fast (Default for `en`)**:
  - `ggml-base.en-q8_0.bin` (78 MB) or `ggml-small.en-q5_1.bin` (182 MB)
  ```bash
  printf '%s/.local/share/whisper/ggml-base.en-q8_0.bin\n' "$HOME" > ~/.config/dwm-voice/model
  ```
- **Portuguese & Multilingual (`pt` + `en`)**:
  - Use a multilingual model (`ggml-base-q8_0.bin` or `ggml-small-q5_1.bin` without the `.en` suffix) so both Portuguese and English utterances transcribe cleanly:
  ```bash
  curl -L -o ~/.local/share/whisper/ggml-small-q5_1.bin \
    https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q5_1.bin
  printf '%s/.local/share/whisper/ggml-small-q5_1.bin\n' "$HOME" > ~/.config/dwm-voice/model
  ```

### 3. Neural TTS Voices & Multilingual Custom Ontology
- **Neural TTS (`~/.config/dwm-voice/tts_voice`)**: Supports Kokoro-82M (`bm_daniel`, `bm_george`, `am_michael`, `bf_emma`, `af_heart` in `~/.local/share/kokoro/`) and Piper ONNX (`piper:gb`, `piper:us`, or Portuguese Piper models in `~/.local/share/piper/`) via `libespeak-ng` IPA phonemization (`en-gb-x-rp`, `en-us`, `pt`).
- **Bilingual Compositional Ontology & ReAct Reasoning**:
  - Custom Portuguese and English verb/entity synonyms can be added side-by-side in `~/.config/jarvis/ontology.json` (or `$JARVIS_ONTOLOGY_JSON`), while `axil-auren` remote inference (`qwen3.5-4b`) natively understands and translates both Portuguese and English queries into canonical Action DSL (`jarvis "muda para o workspace dois e abre um terminal"` / `jarvis "switch to workspace two and open a terminal"`).

---

## 6. Installation & Build

### Prebuilt Packages
Prebuilt packages for Linux (APT / Alpine / Arch / Fedora-RHEL), macOS (Homebrew), Windows (winget / MSYS2), and OpenBSD are distributed via [tty.pt packages](https://github.com/tty-pt/ci/blob/main/docs/install.md). Use **libjarvis** as the package name.

### Build from Source
```sh
make                  # builds lib/libjarvis.so, bin/jarvis, mod_sway.so, and mod_hyprland.so
make test             # runs all 9 C parity, solver, ReAct, accessibility, and multi-turn suites
sudo make install     # installs headers, shared libraries, CLI binary, and pkg-config file
```

---

## 7. JavaScript & WebAssembly Bindings (`@tty-pt/jarvis`)

Official Node.js (N-API) and Browser (WebAssembly) bindings are colocated in [`npm/`](npm/README.md):
- **Universal Runtime**: Native Node-API addon (`jarvis_node.node`) with automatic fallback to the freestanding WebAssembly reactor (`dist/jarvis.wasm`) mounted on the `@tty-pt/stele` shared-memory substrate.
- **Full Solver & Engine Parity**: Exposes `solveClause`, `splitClauses`, `normalizeDslJson`, `extractStreamingAnswer`, `popTtsClause`, `isDestructiveCommand`, `extractWakeCommand`, `writeEarconWav`, and the stateful `JarvisEngine` class identically across C, Node.js, and browser WebAssembly.
- **C Development Headers**: Install via `npm install @tty-pt/jarvis` and consume canonical headers from `node_modules/@tty-pt/jarvis/include`.

```sh
make wasm             # compiles npm/dist/jarvis.wasm via clang --target=wasm32
make npm-test         # builds N-API + WASM targets and runs the Node.js parity test suite
```

👉 *For complete TypeScript/JavaScript usage and WebAssembly integration, see [npm/README.md](npm/README.md).*

## License

BSD 2-Clause License. Copyright (c) 2026, Paulo André Azevedo Quirino. See `LICENSE`.
