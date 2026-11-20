# @tty-pt/jarvis

[![npm version](https://img.shields.io/badge/npm-v1.1.0-CB3837?logo=npm)](#)
[![License: BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](https://github.com/tty-pt/libjarvis/blob/main/LICENSE)
[![TypeScript Ready](https://img.shields.io/badge/TypeScript-Ready-3178C6?logo=typescript)](#)
[![Node.js Native](https://img.shields.io/badge/Node.js-N--API-339933?logo=node.js)](#)
[![WebAssembly](https://img.shields.io/badge/WebAssembly-Reactor-654FF0?logo=webassembly)](#)

> Universal Node.js (N-API) and Browser (WebAssembly) bindings for **`libjarvis`**, the Compositional Semantic Solver, Cross-Platform Intent IR, Multi-Turn ReAct Agent Orchestrator, Voice Pipeline, and `libxylem` Environment Adapters. Canonical C headers: `<ttypt/jarvis.h>` and `<ttypt/jarvis-types.h>`.

📖 **Full C & Architecture Documentation**: See the main [**libjarvis GitHub README**](https://github.com/tty-pt/libjarvis#readme) for the four-stage compositional pipeline, `XY_DECL` / `XY_IMPL` environment adapter contracts, and voice VAD / streaming TTS pipeline.

---

## 1. Quickstart & Installation

```bash
npm install @tty-pt/jarvis @tty-pt/stele
```

**Native OS Packages (`tty.pt`)**: Prebuilt native C packages (`libjarvis`) for Linux (`apt`, `apk`, `pacman`, `dnf`), macOS (`brew`), Windows (`winget`, MSYS2), and OpenBSD (`pkg_add`) are available via the [**tty.pt package repositories**](https://github.com/tty-pt/ci/blob/main/docs/install.md).

```javascript
import {
  solveClause,
  splitClauses,
  normalizeDslJson,
  extractStreamingAnswer,
  popTtsClause,
  isDestructiveCommand,
  extractWakeCommand,
  JarvisEngine,
} from "@tty-pt/jarvis";

// 1. Stateless deterministic solving (~0.1 ms on Native N-API or WebAssembly)
const dsl = solveClause("play lofi chill beats on youtube", "any");
console.log(dsl.op, dsl.target.query); // "media_play", "lofi chill beats"

// 2. Multi-clause splitting (protects "and press enter" and quoted spans)
const clauses = splitClauses("switch to workspace two and open a terminal");
// ["switch to workspace two", "open a terminal"]

// 3. Stateful engine with workflow memory and multi-turn orchestration
const engine = new JarvisEngine({ inferMode: "auto", dryRun: true });
engine.remember("morning routine", "switch to workspace 2 and open a terminal");
const res = engine.process("run my morning routine");
console.log(res.steps.map((s) => s.dsl.op)); // ["workspace_switch", "open_terminal"]
engine.close();
```

---

## 2. JavaScript / TypeScript API Reference

### `JarvisEngine`
Stateful orchestrator managing ontology, conversational anaphora context, and persistent workflow memories.
* `constructor(options?: { wm?: string, inferMode?: "auto" | "llm" | "embedding" | "none", dryRun?: boolean })`: Creates a new engine instance backed by Native N-API or `jarvis.wasm`.
* `.process(utterance: string, activeSurface?: string): JarvisProcessResult`: Solves and executes a single or multi-clause utterance, returning per-step Intent IR, Action DSL, and adapter outputs.
* `.remember(key: string, value: string): boolean`: Saves a persistent user-taught workflow or preference.
* `.recall(query?: string): Record<string, string>`: Recalls stored workflow memories matching `query`.
* `.close(): void`: Releases the underlying native/WASM engine instance.

### Standalone Solver, Safety & Voice Utilities
* `solveClause(clause: string, activeSurface?: string): JarvisDsl | null`: Deterministically solves a single clause into a canonical Action DSL object.
* `splitClauses(utterance: string): string[]`: Splits a compound command into atomic clauses.
* `normalizeDslJson(rawJson: string): JarvisDsl | null`: Parses and heals LLM JSON output (resolving operation aliases and misplaced fields) into canonical Action DSL.
* `extractStreamingAnswer(rawJsonFragment: string): string | null`: Extracts the incremental `"answer"` prefix from a partially streamed LLM JSON payload.
* `popTtsClause(unspoken: string, isFirstClause?: boolean): { clause: string, remainder: string } | null`: Splits streaming text into low-latency (~1.0s TTFA) spoken clauses.
* `isDestructiveCommand(cmd: string): boolean`: Detects dangerous shell commands (`rm -rf`, `git reset --hard`, `mkfs`, `dd`, `kill -9`).
* `isHallucination(text: string): boolean`: Filters non-speech ASR stutter hallucinations.
* `extractWakeCommand(transcript: string, wakeWord?: string): string | null`: Extracts the command following a wake word.
* `writeEarconWav(wavPath: string, isDone?: boolean): boolean`: Synthesizes a 16 kHz mono WAV earcon chime.

---

## 3. C Development Headers & Native Integration

```makefile
CFLAGS += -I./node_modules/@tty-pt/jarvis/include
LDLIBS += -ljarvis -lxylem -lstele -lcorm -lm
```

---

## License

BSD-2-Clause — Copyright (c) 2026, Paulo André Azevedo Quirino <quirinpa@gmail.com>. See [LICENSE](https://github.com/tty-pt/libjarvis/blob/main/LICENSE).
