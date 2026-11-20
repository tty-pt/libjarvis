# libjarvis (`@tty-pt/jarvis`)

Universal Compositional Semantic Solver, Cross-Platform Intent Intermediate Representation (Intent IR), Voice Pipeline, Multi-Turn ReAct Agent Orchestrator, and `libxylem` Environment Adapters in pure C99 and WebAssembly.

## Features

- **Tier 0 Deterministic Compositional Semantic Solver (~0.1 ms)**: Maps natural language utterances across desktop (`dwm`, `sway`, `hyprland`), terminal (`tmux`, POSIX shell), browser (`qutebrowser`, `firefox`), web applications (`site`), and game worlds (`NeverDark`) into platform-neutral **Intent IR** and executable **Action DSL** operations—including spoken cardinal/ordinal workspace numbers (`"switch to workspace two"`), composite scroll directions (`"scroll up 8"`), and casual conversational queries (`"let's listen to some miles davis"`).
- **Dynamic Window Manager & Environment Modules**: Ships pluggable `libxylem` window manager modules (`mod_sway.so`, `mod_hyprland.so`) alongside `dwm` / X11 / Wayland adapters, browser accessibility hint/element selection, and live terminal screen inspection.
- **Multi-Clause Chaining & Spatial Anaphora**: Splits compound commands (`"switch to workspace 2 and open a terminal there"`) while protecting `"and press enter"` and dictation spans, propagating deictic pronouns (`"it"`, `"that"`, `"there"`) across clauses and turns.
- **Multi-Turn ReAct Agent, Continuous Dialogue & Persistent Workflow Memory**: Inspects live terminal panes, system metrics, and clipboard contents via `libxylem` hooks (`jarvis_xy_observe`, `jarvis_xy_execute`), supports continuous multi-turn voice/text follow-ups (`jarvis_Engine_continue_listen`), defaults to low-latency remote `axil-auren` inference (`qwen3.5-4b` with deduplicated fallback model chain), expands user-taught workflows (`jarvis_memory_remember`), and gates destructive commands (`rm -rf`, `git reset --hard`, `mkfs`, `dd`, `kill -9`) behind `jarvis_xy_confirm`.
- **Low-Latency Voice Pipeline**: 16 kHz circular ring buffer, adaptive noise-floor VAD, wake-word extractor, pure-C WAV earcon chime generator, and ~1.0s TTFA incremental streaming TTS clause segmenter.
- **Universal Native C, Node.js N-API & WebAssembly**: Zero third-party JSON dependencies (vendored `jsmn.h`), two-header `libxylem` design (`<ttypt/jarvis.h>` and `<ttypt/jarvis-types.h>`), and colocated `@tty-pt/jarvis` package running identically on Node-API and `@tty-pt/stele` WebAssembly.

## Building & Testing (C)

```sh
make
make test
sudo make install
```

## Building & Testing (`@tty-pt/jarvis` Node.js + WebAssembly)

```sh
make wasm
make npm-test
```

## License

BSD-2-Clause
