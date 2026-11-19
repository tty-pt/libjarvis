# `@tty-pt/jarvis`

Universal Node.js (N-API) and Browser (WebAssembly) bindings for [`libjarvis`](https://github.com/tty-pt/libjarvis) — Compositional Semantic Solver, Cross-Platform Intent IR, Voice Pipeline, and Xylem Environment Adapters.

## Installation

```sh
npm install @tty-pt/jarvis @tty-pt/stele
```

## Usage

```js
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

// Stateless deterministic solving (runs on Native N-API or WebAssembly)
const dsl = solveClause("play lofi chill beats on youtube", "any");
console.log(dsl.op, dsl.target.query); // "media_play", "lofi chill beats"

// Multi-clause splitting
const clauses = splitClauses("switch to workspace 2 and open a terminal");
// ["switch to workspace 2", "open a terminal"]

// Stateful engine with workflow memory and multi-turn orchestration
const engine = new JarvisEngine({ inferMode: "auto", dryRun: true });
engine.remember("morning routine", "switch to workspace 2 and open a terminal");
const res = engine.process("run my morning routine");
console.log(res.steps.map((s) => s.dsl.op)); // ["workspace_switch", "open_terminal"]
engine.close();
```

## License

BSD-2-Clause
