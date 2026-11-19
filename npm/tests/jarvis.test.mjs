import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import {
  solveClause,
  normalizeDslJson,
  splitClauses,
  extractStreamingAnswer,
  popTtsClause,
  isDestructiveCommand,
  isHallucination,
  extractWakeCommand,
  writeEarconWav,
  JarvisEngine,
} from "../dist/index.js";

test("solveClause solves structured commands identically in Native N-API and WebAssembly", () => {
  const cases = [
    ["open a terminal", "any", "open_terminal"],
    ["launch firefox", "any", "open_application"],
    ["switch to workspace 4", "any", "workspace_switch"],
    ["play lofi chill beats on youtube", "any", "media_play"],
    ["list files in /tmp", "terminal", "spawn_shell_command"],
    ["show disk usage", "terminal", "spawn_shell_command"],
    ["type hello world and press enter", "terminal", "type_and_enter"],
  ];

  for (const [utterance, surface, expectedOp] of cases) {
    const nat = solveClause(utterance, surface, { wasm: false });
    const wsm = solveClause(utterance, surface, { wasm: true });
    assert.ok(nat, `Native solve failed for: ${utterance}`);
    assert.ok(wsm, `WASM solve failed for: ${utterance}`);
    assert.equal(nat.op, expectedOp);
    assert.equal(wsm.op, expectedOp);
  }
});

test("normalizeDslJson heals LLM operation aliases in Native and WASM", () => {
  const raw = { op: "goto_workspace", workspace: "5" };
  const nat = normalizeDslJson(raw, { wasm: false });
  const wsm = normalizeDslJson(raw, { wasm: true });
  assert.equal(nat.op, "workspace_switch");
  assert.equal(nat.args.workspace, 5);
  assert.equal(wsm.op, "workspace_switch");
  assert.equal(wsm.args.workspace, 5);
});

test("splitClauses splits multi-clause utterances while protecting 'and press enter'", () => {
  const u1 = "switch to workspace 2 and open a terminal";
  assert.deepEqual(splitClauses(u1, { wasm: false }), ["switch to workspace 2", "open a terminal"]);
  assert.deepEqual(splitClauses(u1, { wasm: true }), ["switch to workspace 2", "open a terminal"]);

  const u2 = "type hello world and press enter";
  assert.deepEqual(splitClauses(u2, { wasm: false }), ["type hello world and press enter"]);
  assert.deepEqual(splitClauses(u2, { wasm: true }), ["type hello world and press enter"]);
});

test("extractStreamingAnswer and popTtsClause work across Native and WASM", () => {
  const frag = '```json\n{"op": "answer_question", "args": {"answer": "Water boils at 100 degrees Celsius at standard pressure';
  assert.ok(extractStreamingAnswer(frag, { wasm: false }).startsWith("Water boils at 100 degrees Celsius"));
  assert.ok(extractStreamingAnswer(frag, { wasm: true }).startsWith("Water boils at 100 degrees Celsius"));

  const text = "Linux is a family of open-source Unix-like operating systems based on the Linux kernel.";
  const popNat = popTtsClause(text, true, { wasm: false });
  const popWsm = popTtsClause(text, true, { wasm: true });
  assert.equal(popNat.ready, true);
  assert.equal(popWsm.ready, true);
  assert.ok(popNat.clause.length >= 15 && popNat.clause.length <= 48);
  assert.equal(popNat.clause, popWsm.clause);
});

test("safety and voice utilities (isDestructiveCommand, isHallucination, extractWakeCommand, writeEarconWav)", () => {
  assert.equal(isDestructiveCommand("rm -rf /tmp/build", { wasm: false }), true);
  assert.equal(isDestructiveCommand("rm -rf /tmp/build", { wasm: true }), true);
  assert.equal(isDestructiveCommand("ls -la", { wasm: false }), false);
  assert.equal(isDestructiveCommand("ls -la", { wasm: true }), false);

  assert.equal(isHallucination("vvvvvvvv", { wasm: false }), true);
  assert.equal(isHallucination("vvvvvvvv", { wasm: true }), true);
  assert.equal(isHallucination("open a terminal", { wasm: false }), false);

  const wakeNat = extractWakeCommand("Hey Jarvis, switch to workspace 3", "jarvis", { wasm: false });
  const wakeWsm = extractWakeCommand("Hey Jarvis, switch to workspace 3", "jarvis", { wasm: true });
  assert.equal(wakeNat.detected, true);
  assert.equal(wakeNat.command, "switch to workspace 3");
  assert.deepEqual(wakeNat, wakeWsm);

  const tmpWav = path.join(os.tmpdir(), `jarvis_test_${process.pid}.wav`);
  assert.equal(writeEarconWav(tmpWav, false), true);
  const stat = fs.statSync(tmpWav);
  assert.ok(stat.size > 44);
  fs.unlinkSync(tmpWav);
});

test("JarvisEngine stateful process, remember, and recall", () => {
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "jarvis-node-"));
  const eng = new JarvisEngine({
    contextPath: path.join(tmpDir, "ctx.json"),
    memoryPath: path.join(tmpDir, "mem.json"),
    sessionPath: path.join(tmpDir, "sess.json"),
    inferMode: "none",
    dryRun: true,
  });

  assert.equal(eng.remember("morning routine", "switch to workspace 2 and open a terminal"), true);
  const mems = eng.recall("morning");
  assert.equal(mems["morning routine"], "switch to workspace 2 and open a terminal");

  const res = eng.process("run my morning routine", "any");
  assert.equal(res.ok, true);
  assert.equal(res.steps.length, 2);
  assert.equal(res.steps[0].dsl.op, "workspace_switch");
  assert.equal(res.steps[1].dsl.op, "open_terminal");

  eng.close();
  fs.rmSync(tmpDir, { recursive: true, force: true });
});
