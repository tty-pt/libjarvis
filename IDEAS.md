# libjarvis — Architectural Ideas & Full Execution Plan (`IDEAS.md`)

> Living design document and execution plan for making `libjarvis` strictly language-agnostic in C and unifying its ontology, lexical/semantic recall, and workflow memory with `libmm` across native and WebAssembly targets.

---

## 1. Core Architectural Premise

### 1.1 What "`libmm` Compiles to WASM Too" Tells Us
Because `libmm` is a first-class `tty.pt` C library alongside `libcorm`, `libxylem`, `libstele`, `liblemma`, `libsepal`, `libjoint`, and `libislet` — targeting both native (`.so` / `.a`) and WebAssembly (`wasm32`, `@tty-pt/mm`) — three architectural invariants follow:

1. **`libmm` must call `libcorm` and the `cormr_*` Recall Kernel in-process, not `fork() + execvp("corm")`**:
   - Currently, `external/libmm/src/mm_ctx.c` falls back to `fork()` + `execvp("corm", ...)` when `.cormd.sock` is absent.
   - `fork()`/`execvp()` cannot exist in WebAssembly and adds 10–20 ms of process-spawn latency per call on native desktops when `cormd` is not running.
   - `libcorm` already exposes its associative store (`corm_open`, `corm_put`, `corm_get`, `corm_htok_pack`, `corm_htok_unpack`) and Recall Kernel (`<ttypt/cormr.h>`: `cormr_axis_register`, `cormr_query_run`, `cormr_set_*`, `cormr_rank_*`) in pure portable C99.
   - By driving `libcorm` + `liblemma` + `libsepal` + `libjoint` + `libislet` in-process in C (using `cormd` Unix domain socket transport only as an optional native multi-process concurrency lease), **`libmm` becomes both WASM-native and sub-millisecond in-process**.
2. **`libjarvis` can link and use `libmm` everywhere (Native + WASM)**:
   - We do not need `libjarvis` to maintain parallel JSON-array implementations (`memories.json`, `embeddings.json`, fixed 64-entry `JARVIS_MAX_MEMORIES`).
   - `libjarvis` becomes a direct consumer of `libmm` (`MM_TIER_GLOBAL`, `ctx_subsystem = "jarvis"`) across native and WASM builds.
3. **`liblemma` + `libsepal` inside `libmm` provide the language-agnostic recall substrate**:
   - `liblemma` provides UTF-8 case and diacritic folding (`música` $\equiv$ `musica`, `próximo` $\equiv$ `proximo`, `pão` $\equiv$ `pao` for token folding), and `libsepal` provides dense vector ANN search across languages.

---

## 2. Making `libjarvis` 100% Language-Agnostic in C

### 2.1 The Golden Rule
**Zero natural-language literals in `.c` files.**
C code in `libjarvis` (`jarvis-solver.c`, `jarvis-ontology.c`, `libjarvis.c`) must only know:
- **Abstract capability IDs** (`workspace.switch`, `window.control`, `media.play`, `ui.scroll`, `input.dictate`, etc.)
- **Abstract verb concepts** (`OPEN_LAUNCH`, `FOCUS_NAV`, `WORKSPACE_SWITCH`, `WINDOW_CONTROL`, `SCROLL_NAV`, `HELP_ASK`, etc.)
- **Abstract entity & slot types** (`ui.workspace`, `ui.window_action`, `ui.scroll_direction`, `num.digit`, `mod.flag`, `fs.location`, `ctx.anaphora`, etc.)
- **Structural scoring & compilation rules** (`jarvis_ir_t` $\rightarrow$ `jarvis_dsl_t`)

All human language vocabulary — whether English (`en`), Portuguese (`pt`), or any other language — must live exclusively in data (`ontology.json` / `JARVIS_DEFAULT_ONTOLOGY_JSON` and `libmm`).

### 2.2 Complete Audit of Hard-Coded English in `libjarvis` & Data-Driven Replacements

| Location | Hard-Coded English in C Today | Language-Agnostic Data-Driven Replacement |
| :--- | :--- | :--- |
| **`libjarvis.c:361-446`** (`jarvis_split_clauses`) | Hard-codes `" and then "`, `" then "`, `" and "`, `" and press enter"`, `"please"`, `"also"`. Meanwhile `ont->clause_seps` is loaded at `jarvis-ontology.c:348` and **never read**! | Drive splitting exclusively from `ont->clause_seps`, `ont->conjunctions` (splitting only when RHS starts with `ont->polite_prefixes` + `ont->verb_heads`), and protect suffixes in `ont->loc_submit`. |
| **`libjarvis.c:206-264`** (`jarvis_resolve_anaphora`) | Hard-codes `"pause it"`, `"play it"`, `"resume that"`, and substring search for `"there"`. | Model deictics and anaphora rules in the ontology (`anaphora_rules` / `ctx.anaphora` entities) bound to context registers (`$last_query`, `$last_location`, `$last_window`, `$last_app`). |
| **`libjarvis.c:100-115`** (`is_prep_conj_at`) | Hard-codes English prepositions/conjunctions (`"at"`, `"in"`, `"because"`, `"while"`) for TTFA streaming TTS clause chunking. | Read TTS prosodic break tokens from ontology (or use universal punctuation + word-boundary splitting). |
| **`libjarvis.c:785-850`** (`requires_agent_reasoning`) | Hard-codes a 25-line `TRIGGERS[]` array (`" what "`, `" why "`, `" error "`, `" crash "`, `" undo "`, `" remember "`) that forces utterances straight to Tier-2 LLM before Tier-0 runs. | Eliminate `TRIGGERS[]` in C. Let Tier-0 evaluate first (or check ontology `agent_triggers` / `HELP_ASK` verb concept): if a clause matches `HELP_ASK` / `agent_triggers`, ends in `'?'` (and isn't a covered `fs.metric`/`git.inspect`), or scores $< 0.90$, escalate to Tier-1/2 ReAct agent. |
| **`jarvis-solver.c:512-542`** (`is_digit_1_to_9`, `parse_digit_1_to_9`) | Hard-codes `"one"`..`"nine"` and `"first"`..`"ninth"` in C arrays. | Represent cardinal/ordinal number words in the ontology under `entities["num.digit"]` (and `entities["ui.workspace"]`) with canonical `"id": "1"`..`"9"` (`"two"`, `"second"`, `"dois"`, `"segundo"` $\rightarrow$ `"2"`). C only parses raw ASCII digits `0..9` or reads the matched entity's canonical `id`. |
| **`jarvis-solver.c:354-360`** (`extract_constituents`) | Hard-codes `"new"`, `"hidden"`, `"all"`, `"silent"`, `"fullscreen"`, `"floating"`. | Represent modifiers as ontology entities of type `"mod.flag"` with canonical IDs (`"new"`, `"hidden"`, `"all"`, `"silent"`, `"fullscreen"`, `"floating"`). |
| **`jarvis-solver.c:686-706`** (`build_intent_ir`) | Fallback `strstr` for `"fullscreen"`, `"tile"`, `"float"`, `"close"` and scroll directions `"up"`, `"left"`, `"right"` inside `p->lead_verb_span`. | Covered by `ui.window_action` and `ui.scroll_direction` entities in the ontology. |
| **`jarvis-solver.c:720-815`** (`PREPS[]` in `input.dictate`, `agent.ask`, `web.search`, `media.play`) | Hard-codes English prepositions (`"for "`, `"about "`, `"on "`, `"listen to "`, `"some "`) stripped after the lead verb. | Move argument-introducer prepositions into `ont->arg_preps` in `ontology.json`. |
| **`jarvis-solver.c:858-887`** (`NEW_TERM_TAILS`, `BG_TAILS`) | Hard-codes `"in a new terminal"` and `"in the background"` right after `strip_locative_modifiers()`. | Add `"new_terminal"` and `"background"` categories to `self_locatives` in `ontology.json` and strip them inside `strip_locative_modifiers()`. |
| **`jarvis-solver.c:928-1075`** (Casual & Ergonomic Idiom Fast-Path) | ~150 lines of `strstr(padded_raw, " too loud ")`, `" zoom in "`, `" get rid of "`, `" turn on voice "`, etc. | Express these in `ontology.json` via a declarative `"idioms"` table (and verb/entity mappings) mapping phrase patterns to `{ "cap": "...", "action": "...", "text": "...", "kind": "...", "surface": "..." }`. |
| **`jarvis-ontology.c:634-676`** (`jarvis_ontology_build_live_index`) | Hard-codes `"%s folder"` and `"%s directory"` when indexing `$HOME` and XDG directories. | Use `"fs_templates": ["%s folder", "%s directory", "pasta %s", "diretorio %s"]` from `ontology.json`. |

---

## 3. Detailed 3-Phase Execution Plan

Following Rule 9 (Abstraction-First Test-Driven Development & Integration Sequencing), we implement and unit-test each abstraction standalone before cross-library integration.

### Phase 1: Make `libmm` In-Process (`libcorm` C API) & WASM-Ready (`external/libmm`)
1. **In-Process `libcorm` Store & Structured Candidate Recall in `external/libmm`**:
   - Add an in-process `libcorm` + `mm_fusion` execution path in `mm_ctx.c` so `mm_record`, `mm_get_envelope`, `mm_put_envelope`, `mm_forget`, `mm_list_refs`, and `mm_recall` work in-process when `.cormd.sock` / external `corm` CLI is unavailable (or in WebAssembly).
   - Expose `mm_recall_candidates(mm_ctx_t *ctx, const mm_recall_req_t *req, mm_candidate_t **out_candidates, size_t *out_count)` in `<ttypt/mm.h>` so C callers (`libjarvis`) receive ranked `mm_candidate_t` structs (`rrf_score`, `lexical_score`, `semantic_score`, `ranking_score`, and decoded `mm_envelope_t`) directly in memory.
2. **WebAssembly Target (`make wasm`) in `external/libmm`**:
   - Guard POSIX process/socket calls in `mm_ctx.c`, `mm_code.c`, `mm_session.c`, and `mm_mcp.c` with `#ifndef __wasm__` (and `<ttypt/stele.h>`).
   - Add `npm/wasm/mm_wasm.c` and `WASM_SRCS` in `external/libmm/Makefile` so `make wasm` compiles `npm/dist/mm.wasm`.
3. **Phase 1 Verification Gate**:
   - `make -C external/libmm && make -C external/libmm wasm && make -C external/libmm test && make -C external/libmm npm-test`.

### Phase 2: Make `libjarvis` Tier-0 100% Language-Agnostic & Data-Driven (`external/libjarvis`)
1. **UTF-8 Case & Diacritic Folding in `jarvis_clean_phrase()`**:
   - Fold UTF-8 Latin-1 / Latin-Extended diacritics (`á/à/â/ã/ä` $\rightarrow$ `a`, `é/è/ê/ë` $\rightarrow$ `e`, `í/ì/î/ï` $\rightarrow$ `i`, `ó/ò/ô/õ/ö` $\rightarrow$ `o`, `ú/ù/û/ü` $\rightarrow$ `u`, `ç` $\rightarrow$ `c`, `ñ` $\rightarrow$ `n`, plus uppercase equivalents) matching `liblemma`'s folding rules.
2. **Extend `jarvis_ontology_t` & `ontology.json` Schema**:
   - `clause_separators` + `conjunctions` used by `jarvis_split_clauses()`, protecting `self_locatives.submit`.
   - `self_locatives.new_terminal` and `self_locatives.background` handled in `strip_locative_modifiers()`.
   - `entities["num.digit"]` and `entities["mod.flag"]` replacing hard-coded number words and modifier strings in `jarvis-solver.c`.
   - `arg_prepositions` replacing hard-coded `PREPS[]` in `build_intent_ir()`.
   - `anaphora_rules` replacing hard-coded English in `jarvis_resolve_anaphora()`.
   - `idioms` table in `ontology.json` replacing the 150-line English `strstr()` fast-path in `jarvis-solver.c`, and `agent_triggers` / `interrogatives` replacing `TRIGGERS[]` / `INTERROGATIVES[]`.
   - `fs_templates` replacing hard-coded `"%s folder"` / `"%s directory"` in `jarvis_ontology_build_live_index()`.
3. **Bilingual Default Ontology (`en` + `pt`) in `jarvis-default-ontology.h`**:
   - Populate `jarvis-default-ontology.h` with both English and Portuguese verbs, entities, numbers (`um`..`nove`, `primeiro`..`nono`), stopwords, clause separators (`e depois`, `depois`, `a seguir`), conjunctions (`and`, `e`), locatives (` noutro terminal`, ` em pano de fundo`, ` e carrega enter`), anaphora (`lá`, `isso`), and idioms.
4. **Phase 2 Verification Gate**:
   - Audit `src/*.c` to confirm natural-language phrase arrays are gone from C code.
   - Add English and Portuguese Tier-0 unit tests in `tests/test_jarvis.c` and verify `make -C external/libjarvis test` and `make -C external/libjarvis wasm`.

### Phase 3: Wire `libmm` into `libjarvis` for Memory & Self-Learning
1. **Link `libjarvis` against `libmm` (`-lmm`)**:
   - Integrate `mm_ctx_t *mm` (`MM_TIER_GLOBAL`, `subsystem = "jarvis"`) into `jarvis_engine_t`.
   - Back `jarvis_memory_remember()` and `jarvis_memory_recall_json()` with `mm_record()` and `mm_recall_candidates()` while preserving backward compatibility with `cfg->memory_path` if explicitly set.
   - Check `libmm` recalled workflow memories (`type = "workflow"`, `status = MM_STATUS_SUPPORTED`) during utterance processing so taught workflows and confirmed self-learned commands resolve deterministically via `libmm`'s hybrid lexical + semantic RRF ranking.
2. **Epistemic Self-Learning (`MM_STATUS_OBSERVED` $\rightarrow$ `MM_STATUS_SUPPORTED` / `MM_STATUS_CONTRADICTED`)**:
   - Record verified Tier-2 single-step deterministic resolutions in `libmm` as `MM_STATUS_OBSERVED` (tagged with `ctx_module = active_surface`), promote on repeat/confirmation to `MM_STATUS_SUPPORTED`, and downgrade to `MM_STATUS_CONTRADICTED` on undo.
3. **Phase 3 Verification Gate**:
   - `make -C external/libmm test && make -C external/libmm npm-test`
   - `make -C external/libjarvis test && make -C external/libjarvis npm-test`
   - Install both `libmm` and `libjarvis` to `~/.local` and run `make test-fast` at the repository root.
