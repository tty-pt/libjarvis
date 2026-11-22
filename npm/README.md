# @tty-pt/jarvis

[![License: BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](https://github.com/tty-pt/libjarvis/blob/main/LICENSE)
[![TypeScript Ready](https://img.shields.io/badge/TypeScript-Ready-3178C6?logo=typescript)](#)
[![Node.js Native](https://img.shields.io/badge/Node.js-N--API-339933?logo=node.js)](#)
[![WebAssembly](https://img.shields.io/badge/WebAssembly-Reactor-654FF0?logo=webassembly)](#)

> Universal Node.js (N-API) and Browser (WebAssembly) bindings for **`libjarvis`**, providing multilingual ontology matching, UTF-8 diacritic folding, ASR hallucination filtering, destructive-command checks, wake-word extraction, WAV earcon synthesis, and low-latency streaming TTS segmentation. Canonical headers: `<ttypt/jarvis.h>`, `<ttypt/jarvis-types.h>`, and `<ttypt/desktop-core.h>`.

---

## 1. Installation & Quickstart

```bash
npm install @tty-pt/jarvis @tty-pt/stele
```

```javascript
import {
  cleanPhrase,
  extractStreamingAnswer,
  popTtsClause,
  isDestructiveCommand,
  isHallucination,
  extractWakeCommand,
  writeEarconWav,
} from "@tty-pt/jarvis";

console.log(cleanPhrase("Olá, Jarbas!")); // "ola jarbas"

const wake = extractWakeCommand("Hey Jarvis, check the build log", "jarvis");
console.log(wake); // { detected: true, command: "check the build log" }

const tts = popTtsClause("First, open the terminal. Then inspect git status.", true);
console.log(tts.ready, tts.clause);
```

---

## 2. JavaScript / TypeScript API Reference

* `cleanPhrase(text: string, options?: { wasm?: boolean }): string`: Folds UTF-8 diacritics and normalizes whitespace/punctuation.
* `loadOntologyJson(jsonStr: string): boolean`: Loads a custom `ontology.json` into the WASM module.
* `extractStreamingAnswer(rawJsonFragment: string, options?: { wasm?: boolean }): string`: Extracts the incremental `"answer"` / `"reply"` prefix from a partially streamed JSON payload.
* `popTtsClause(unspoken: string, isFirstClause?: boolean, options?: { wasm?: boolean }): { ready: boolean, clause: string, remainder: string }`: Splits streaming text into low-latency spoken clauses.
* `isDestructiveCommand(cmd: string, options?: { wasm?: boolean }): boolean`: Detects dangerous shell commands (`rm -rf`, `git reset --hard`, `mkfs`, `dd`, `kill -9`).
* `isHallucination(text: string, options?: { wasm?: boolean }): boolean`: Filters non-speech ASR stutter hallucinations.
* `extractWakeCommand(transcript: string, wakeWord?: string, options?: { wasm?: boolean }): { detected: boolean, command: string }`: Extracts the command following a wake word.
* `writeEarconWav(wavPath: string, isDone?: boolean): boolean`: Synthesizes a 16 kHz mono WAV earcon chime (Native N-API).

---

## 3. C Development Headers & Native Integration

```makefile
CFLAGS += -I./node_modules/@tty-pt/jarvis/include
LDLIBS += -ljarvis -lm
```

---

## License

BSD-2-Clause — Copyright (c) 2026, Paulo André Azevedo Quirino <quirinpa@gmail.com>. See [LICENSE](https://github.com/tty-pt/libjarvis/blob/main/LICENSE).
