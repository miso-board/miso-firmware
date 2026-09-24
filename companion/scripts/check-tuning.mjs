// Checks the pitch pipeline against meantonal and against the firmware's
// integer arithmetic. Run with `npm run check:tuning`.
//
// Node 24 strips TypeScript types natively, so this imports src/lib/tuning.ts
// directly rather than needing a build step or a test runner.
//
// The point of this script is the one thing the app cannot check at runtime:
// that the 17 bytes of the `P` frame mean the same pitches to the companion
// (which asks meantonal) and to the firmware (which does int32 millicents). The
// mirror of the firmware path below is the ONLY copy of that arithmetic outside
// Core/Src/main.c, and it belongs here rather than in src/ — see the comment on
// `midiFor` in tuning.ts.

import { Pitch, SPN } from "meantonal";
import {
  LAYOUTS,
  VALID_EDOS,
  anchorVec,
  fifthCents,
  generatePitchAt,
  midiFor,
  noteName,
  pitchAt,
  pitchFrame,
  tuningError,
  tuningWarning,
  matchToleranceCents,
  bendLsbCents,
  defaultPitchMap,
  FIFTH_MIN,
  FIFTH_MAX,
  MPE_BEND_SEMITONES,
} from "../src/lib/tuning.ts";
import { LED_POS } from "../src/lib/layout.ts";

let failures = 0;
let checks = 0;

function ok(cond, what, detail = "") {
  checks++;
  if (!cond) {
    failures++;
    console.log(`  FAIL  ${what}${detail ? " — " + detail : ""}`);
  }
}

function section(name) {
  console.log(`\n${name}`);
}

// --- the firmware's arithmetic, mirrored for comparison only ----------------

/** C truncates toward zero; the firmware rounds to nearest (main.c div_round). */
function divRound(num, den) {
  return num >= 0
    ? Math.floor((num + Math.floor(den / 2)) / den)
    : -Math.floor((-num + Math.floor(den / 2)) / den);
}

/** What pitch_for_xy() will compute, given the bytes of a `P` frame. */
function firmwarePitch(frame, x, y, bendSemitones = MPE_BEND_SEMITONES) {
  const dv = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
  const fifthMilli = dv.getInt32(1, true);
  const m00 = dv.getInt16(5, true);
  const m01 = dv.getInt16(7, true);
  const m10 = dv.getInt16(9, true);
  const m11 = dv.getInt16(11, true);
  const aw = dv.getInt16(13, true);
  const ah = dv.getInt16(15, true);

  const w = m00 * x + m01 * y + aw;
  const h = m10 * x + m11 * y + ah;
  const cm = fifthMilli * (2 * w - 5 * h) + 1200000 * (3 * h - w);
  const note = divRound(cm, 100000);
  const rem = cm - note * 100000;
  const bend = 8192 + divRound(rem * 8192, bendSemitones * 100 * 1000);
  return { note, bend, cm, w, h };
}

/** Every key of a `boards`-wide horizontal chain, in absolute coordinates. */
function gridCoords(boards = 1) {
  const out = [];
  for (let b = 0; b < boards; b++) {
    for (const [lx, ly] of LED_POS) out.push([lx + 5 * b, ly + 2 * b]); // right = (5,2)
  }
  return out;
}

const pitchMapFor = (patch) => {
  const m = defaultPitchMap();
  m.generator = { ...m.generator, ...patch };
  return m;
};

// --- 1. golden regression against the pre-change implementation -------------
//
// The old tuning.ts computed note+bend from a hardcoded 31-EDO step lattice:
//   step = 5w + 3h ; note = round(step·12/31) ; bend from the remainder.
// Both layouts must still produce exactly that.

section("1. golden: 31-EDO output is unchanged from the pre-change code");
{
  const oldMidi = (p) => {
    const totalCents = ((5 * p.w + 3 * p.h) * 1200) / 31;
    const note = Math.round(totalCents / 100);
    const cents = totalCents - note * 100;
    return { note, bend: 8192 + Math.round((cents / 4800) * 8192) };
  };
  for (const id of ["bosanquet", "wicki-hayden"]) {
    const m = pitchMapFor({ layout: id });
    for (const [x, y] of gridCoords(1)) {
      const p = generatePitchAt(m.generator, x, y);
      const legacy = pitchAt(LAYOUTS[id], x, y); // the untouched original path
      ok(p.w === legacy.w && p.h === legacy.h, `${id} pitch vector at ${x},${y}`,
         `got (${p.w},${p.h}) want (${legacy.w},${legacy.h})`);
      const got = midiFor(p, { kind: "edo", edo: 31 });
      const want = oldMidi(p);
      ok(got.note === want.note, `${id} note at ${x},${y}`, `${got.note} vs ${want.note}`);
      ok(Math.abs(got.bend - want.bend) <= 1, `${id} bend at ${x},${y}`,
         `${got.bend} vs ${want.bend}`);
    }
  }
}

// --- 2. the anchor convention reproduces the firmware's constants -----------

section("2. anchor: SPN note on a key reproduces the compiled-in anchors");
{
  const b = anchorVec(pitchMapFor({ layout: "bosanquet" }).generator);
  ok(b.w === 23 && b.h === 7, "Bosanquet D4 on (3,3) is (23,7)", `got (${b.w},${b.h})`);
  const wh = anchorVec(pitchMapFor({ layout: "wicki-hayden" }).generator);
  ok(wh.w === 29 && wh.h === 13, "Wicki-Hayden D4 on (3,3) is (29,13)", `got (${wh.w},${wh.h})`);
  for (const id of ["bosanquet", "wicki-hayden"]) {
    const a = anchorVec(pitchMapFor({ layout: id }).generator);
    ok(a.w === LAYOUTS[id].anchor.w && a.h === LAYOUTS[id].anchor.h,
       `${id} derived anchor matches LAYOUTS literal`);
  }
  // The anchor note must land on the anchor key, whatever the layout or note.
  for (const id of ["bosanquet", "wicki-hayden"]) {
    for (const note of ["C4", "D4", "F#3", "Bb5"]) {
      for (const key of [[3, 3], [0, 0], [6, 4]]) {
        const g = pitchMapFor({ layout: id, anchorNote: note, anchorKey: key }).generator;
        const p = generatePitchAt(g, key[0], key[1]);
        const want = SPN.toPitch(note);
        ok(p.w === want.w && p.h === want.h,
           `${id}: ${note} lands on key ${key}`, `got ${noteName(p)}`);
      }
    }
  }
}

// --- 3. matrix extraction into the wire frame ------------------------------

section("3. wire frame: matrix fields are read and signed correctly");
{
  const read = (id) => {
    const f = pitchFrame(pitchMapFor({ layout: id }));
    const dv = new DataView(f.buffer);
    return [dv.getInt16(5, true), dv.getInt16(7, true), dv.getInt16(9, true), dv.getInt16(11, true)];
  };
  ok(String(read("bosanquet")) === "1,0,0,1", "Bosanquet matrix is identity", String(read("bosanquet")));
  ok(String(read("wicki-hayden")) === "1,-2,0,-1", "Wicki-Hayden matrix is (1,-2,0,-1)",
     String(read("wicki-hayden")));
  const f = pitchFrame(defaultPitchMap());
  ok(f.length === 17, "frame is 17 bytes", String(f.length));
  ok(f[0] === 0x50, "frame starts with 'P'");
  ok(new DataView(f.buffer).getInt32(1, true) === 696774, "31-EDO fifth is 696774 millicents",
     String(new DataView(f.buffer).getInt32(1, true)));
}

// --- 4. byte-identical to today's firmware at the 31-EDO default -----------

section("4. the default frame reproduces today's firmware output exactly");
{
  // What Core/Src/main.c computes today, before this change:
  //   w = x + 23 ; h = y + 7 ; step = 5w + 3h
  //   note = div_round(step·12, 31)
  //   bend = 8192 + div_round((step·1200 − note·100·31)·8192, 4800·31)
  const todayFirmware = (x, y) => {
    const w = x + 23, h = y + 7;
    const step = 5 * w + 3 * h;
    const note = divRound(step * 12, 31);
    const off31 = step * 1200 - note * 100 * 31;
    return { note, bend: 8192 + divRound(off31 * 8192, 4800 * 31) };
  };
  const frame = pitchFrame(defaultPitchMap());
  for (const [x, y] of gridCoords(1)) {
    const got = firmwarePitch(frame, x, y);
    const want = todayFirmware(x, y);
    ok(got.note === want.note && got.bend === want.bend,
       `key ${x},${y} unchanged`, `got ${got.note}/${got.bend} want ${want.note}/${want.bend}`);
  }
}

// --- 5. app and firmware agree across tunings, layouts and a 4-board grid ---

section("5. app (meantonal) vs firmware (int32) across tunings and layouts");
{
  const TOLERANCE_CENTS = matchToleranceCents(); // the MidiMonitor's match tolerance
  let worstCents = 0;
  let worstCm = 0;
  const tunings = [
    { kind: "edo", edo: 12 }, { kind: "edo", edo: 19 }, { kind: "edo", edo: 31 },
    { kind: "edo", edo: 41 }, { kind: "edo", edo: 53 }, { kind: "edo", edo: 72 },
    { kind: "fifth", cents: 696.578 },        // 1/4-comma meantone
    { kind: "fifth", cents: FIFTH_MIN },      // both endpoints of the legal range
    { kind: "fifth", cents: FIFTH_MAX },
  ];
  for (const tuning of tunings) {
    ok(tuningError(tuning) === null, `tuning ${JSON.stringify(tuning)} is accepted`,
       String(tuningError(tuning)));
    for (const id of ["bosanquet", "wicki-hayden"]) {
      const m = pitchMapFor({ layout: id, tuning });
      const frame = pitchFrame(m);
      for (const [x, y] of gridCoords(4)) {
        const p = generatePitchAt(m.generator, x, y);
        const app = midiFor(p, tuning);
        const fw = firmwarePitch(frame, x, y);
        worstCm = Math.max(worstCm, Math.abs(fw.cm));
        // The firmware must place the key on the same pitch vector.
        ok(fw.w === p.w && fw.h === p.h, `${id}: firmware vector at ${x},${y}`,
           `got (${fw.w},${fw.h}) want (${p.w},${p.h})`);
        if (app.note < 0 || app.note > 127) continue; // out of MIDI range, firmware drops it
        ok(app.note === fw.note, `${id} ${JSON.stringify(tuning)} note at ${x},${y}`,
           `app ${app.note} vs fw ${fw.note}`);
        const dCents = (app.bend - fw.bend) * (4800 / 8192);
        worstCents = Math.max(worstCents, Math.abs(dCents));
        ok(Math.abs(dCents) < TOLERANCE_CENTS,
           `${id} ${JSON.stringify(tuning)} bend at ${x},${y}`, `${dCents.toFixed(4)}¢ apart`);
      }
    }
  }
  console.log(
    `  worst app/firmware disagreement: ${worstCents.toFixed(4)}¢ ` +
      `(tolerance ${TOLERANCE_CENTS.toFixed(4)}¢, one bend LSB is ${bendLsbCents().toFixed(4)}¢)`,
  );
  ok(worstCents <= bendLsbCents() + 1e-9, "disagreement never exceeds one bend LSB",
     `${worstCents.toFixed(4)}¢`);
  console.log(`  worst |millicents|: ${worstCm.toExponential(2)} (int32 limit 2.15e+9)`);
  ok(worstCm < 2147483647 / 4, "int32 has 4x headroom over a 4-board grid");
}

// --- 6. the 12-TET self-check ----------------------------------------------
//
// At a 700¢ fifth every key must land on a whole MIDI note with a centred bend.
// This catches a wrong coefficient (it would not collapse to 12-TET), a wrong
// anchor (note names would not match meantonal's own `midi` accessor) and a
// rounding bias (bend would drift off 8192).

section("6. 12-TET self-check: whole notes, centred bends");
{
  const tuning = { kind: "fifth", cents: 700 };
  for (const id of ["bosanquet", "wicki-hayden"]) {
    const m = pitchMapFor({ layout: id, tuning });
    const frame = pitchFrame(m);
    for (const [x, y] of gridCoords(4)) {
      const p = generatePitchAt(m.generator, x, y);
      const app = midiFor(p, tuning);
      const fw = firmwarePitch(frame, x, y);
      ok(app.note === p.midi, `${id} app note == Pitch.midi at ${x},${y}`,
         `${app.note} vs ${p.midi}`);
      ok(app.bend === 8192, `${id} app bend centred at ${x},${y}`, String(app.bend));
      ok(fw.bend === 8192, `${id} firmware bend centred at ${x},${y}`, String(fw.bend));
      ok(fw.note === p.midi, `${id} firmware note == Pitch.midi at ${x},${y}`,
         `${fw.note} vs ${p.midi}`);
    }
  }
}

// --- 7. tuning validation and the degenerate families ----------------------

section("7. validation: rejected EDOs, flagged degenerate tunings");
{
  for (const edo of [6, 8, 9, 11, 13, 16, 18, 23]) {
    ok(tuningError({ kind: "edo", edo }) !== null, `${edo}-EDO is rejected`);
    ok(!VALID_EDOS.includes(edo), `${edo}-EDO is absent from VALID_EDOS`);
  }
  for (const edo of [5, 7, 12, 19, 31, 41, 53, 72]) {
    ok(tuningError({ kind: "edo", edo }) === null, `${edo}-EDO is accepted`);
  }
  for (const edo of [5, 10, 15, 20, 25, 30]) {
    ok(/vanishes/.test(tuningWarning({ kind: "edo", edo }) ?? ""),
       `${edo}-EDO warns that the semitone vanishes`, String(tuningWarning({ kind: "edo", edo })));
  }
  for (const edo of [7, 14, 21, 28, 35]) {
    ok(/same width/.test(tuningWarning({ kind: "edo", edo }) ?? ""),
       `${edo}-EDO warns that whole tone == semitone`, String(tuningWarning({ kind: "edo", edo })));
  }
  for (const edo of [12, 19, 31, 41, 53]) {
    ok(tuningWarning({ kind: "edo", edo }) === null, `${edo}-EDO is not flagged`);
  }
  ok(tuningError({ kind: "fifth", cents: 684 }) !== null, "a 684¢ fifth is rejected");
  ok(tuningError({ kind: "fifth", cents: 721 }) !== null, "a 721¢ fifth is rejected");
  ok(tuningError({ kind: "fifth", cents: NaN }) !== null, "NaN is rejected");
  ok(tuningError({ kind: "edo", edo: 31.5 }) !== null, "a fractional EDO is rejected");
  // Anything tuningError accepts, meantonal must accept too.
  for (const edo of VALID_EDOS) {
    let threw = false;
    try { midiFor(SPN.toPitch("C4"), { kind: "edo", edo }); } catch { threw = true; }
    ok(!threw, `meantonal accepts ${edo}-EDO`);
  }
}

// --- 8. 1/4-comma meantone has a pure major third -------------------------

section("8. the cents form buys something no EDO can: a pure 5:4");
{
  const tuning = { kind: "fifth", cents: 696.5784285 }; // 1/4-comma meantone
  const c4 = SPN.toPitch("C4");
  const e4 = SPN.toPitch("E4");
  const { tuningMap } = await import("../src/lib/tuning.ts");
  const T = tuningMap(tuning);
  const ratio = T.toHz(e4) / T.toHz(c4);
  ok(Math.abs(ratio - 1.25) < 1e-6, "E4/C4 is exactly 5:4", `got ${ratio.toFixed(9)}`);
}

// --- 9. overrides are tuning-independent and win over the generator --------

section("9. overrides: layered on, never materialised");
{
  const { pitchOn, isOverridden } = await import("../src/lib/tuning.ts");
  const m = defaultPitchMap();
  const target = SPN.toPitch("G#5");
  m.overrides["3,3"] = { w: target.w, h: target.h };
  ok(isOverridden(m, 3, 3), "the overridden key reports as overridden");
  ok(!isOverridden(m, 4, 3), "a neighbour does not");
  const got = pitchOn(m, 3, 3);
  ok(got.w === target.w && got.h === target.h, "override wins over the generator",
     noteName(got));
  // Changing the tuning must not move an override.
  m.generator = { ...m.generator, tuning: { kind: "edo", edo: 19 } };
  const after = pitchOn(m, 3, 3);
  ok(after.w === target.w && after.h === target.h, "override survives a tuning change");
  // A coordinate no board has occupied yet still resolves — the lazy property.
  const far = pitchOn(m, 500, -200);
  ok(Number.isFinite(far.w) && Number.isFinite(far.h),
     "an unseen coordinate still resolves", `(${far.w},${far.h})`);
  ok(m.generator !== undefined, "the generator is never removed");
}

// --- summary ---------------------------------------------------------------

console.log(
  `\n${failures === 0 ? "PASS" : "FAIL"} — ${checks - failures}/${checks} checks passed`,
);
process.exit(failures === 0 ? 0 : 1);
