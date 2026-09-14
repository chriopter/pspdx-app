// hold.mjs <program.json>: the half of a stress run that a key file cannot
// do. PSPDX.KEYS presses a button for one frame; main.c's repeat() only gets
// going after a third of a second of a direction being *held*, and reaches
// twenty-five rows a second after two. So the holding is done here, through
// PPSSPP's own WebSocket debugger, which takes a duration in frames.
//
// The program is a JSON array of
//   { "op": "hold",   "button": "down", "frames": 300 }
//   { "op": "tap",    "button": "cross", "ms": 50 }
//   { "op": "analog", "x": -0.8, "y": 0.3, "ms": 250 }
//   { "op": "wait",   "ms": 400 }
//
// A tap is the other thing a key file cannot do: PSPDX.KEYS holds 256 lines
// in four kilobytes, and twenty seconds of a thumb going at fifty
// milliseconds is four hundred presses. It is a press of a few frames
// followed by a wait of that many milliseconds of *this* clock, so the storm
// is timed in the host's milliseconds, as the scenario describes it.
//
// This PPSSPP build aborts when the debugger socket is closed under it, so
// the process stays connected after the program has run and exits when the
// emulator does -- run.py waits for it after the rig has stopped the
// emulator, and kills it if it outlives that.
import { execSync } from "node:child_process";
import { readFileSync } from "node:fs";

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function port() {
  try {
    return execSync(
      "ss -ltnp | grep PPSSPPSDL | sed -E 's/.*:([0-9]+) .*users.*/\\1/' | head -1",
      { stdio: ["ignore", "pipe", "ignore"] }).toString().trim();
  } catch {
    return "";
  }
}

// The emulator is started by the rig a moment before this; the debugger
// port comes up with it.
let p = "";
for (let i = 0; i < 60 && !p; i++) { p = port(); if (!p) await sleep(500); }
if (!p) { console.error("no debugger port"); process.exit(1); }

const program = JSON.parse(readFileSync(process.argv[2], "utf8"));
const ws = new WebSocket("ws://127.0.0.1:" + p + "/debugger");

ws.onopen = async () => {
  console.log("connected on " + p);
  for (const step of program) {
    if (step.op === "wait") { await sleep(step.ms); continue; }
    if (step.op === "hold") {
      ws.send(JSON.stringify({ event: "input.buttons.press",
                               button: step.button, duration: step.frames }));
      console.log("hold " + step.button + " " + step.frames + " frames");
      // A frame is the guest's, and the guest does not keep up: sixty of
      // them take rather longer than a second of this process's clock, so
      // the wait for a hold to finish is stretched the same way run.py
      // stretches a key file's schedule.
      await sleep(step.frames * 1000 / 60 * 1.9 + 200);
      continue;
    }
    if (step.op === "tap") {
      // Three frames: one is a frame the guest can drop while it is inside a
      // draw, and a press nobody sees is not a key storm.
      ws.send(JSON.stringify({ event: "input.buttons.press",
                               button: step.button, duration: step.frames || 3 }));
      await sleep(step.ms);
      continue;
    }
    if (step.op === "analog") {
      ws.send(JSON.stringify({ event: "input.analog.send", stick: "left",
                               x: step.x, y: step.y }));
      await sleep(step.ms);
      continue;
    }
  }
  console.log("program done, staying connected");
};
ws.onmessage = (m) => {
  // Only the refusals are worth the noise: an unknown event comes back as
  // one of these and would otherwise look like a stress run that did
  // nothing at all.
  try {
    const r = JSON.parse(m.data);
    if (r.event === "error") console.log("debugger error: " + JSON.stringify(r));
  } catch { /* not ours */ }
};
ws.onclose = () => process.exit(0);
ws.onerror = () => { console.error("debugger socket failed"); process.exit(1); };
