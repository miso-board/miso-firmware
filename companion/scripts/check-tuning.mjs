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
  // A 'Q' frame carries a concert-pitch offset after the anchor; a 'P' has none.
  const offset = frame.byteLength >= 21 ? dv.getInt32(17, true) : 0;
  const cm = fifthMilli * (2 * w - 5 * h) + 1200000 * (3 * h - w) + offset;
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
  for (const id of ["bosanquet", "wicki-hayden", "harmonic-table"]) {
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
  const ht = anchorVec(pitchMapFor({ layout: "harmonic-table" }).generator);
  ok(ht.w === 23 && ht.h === 13, "Harmonic Table D4 on (3,3) is (23,13)", `got (${ht.w},${ht.h})`);
  for (const id of ["bosanquet", "wicki-hayden", "harmonic-table"]) {
    const a = anchorVec(pitchMapFor({ layout: id }).generator);
    ok(a.w === LAYOUTS[id].anchor.w && a.h === LAYOUTS[id].anchor.h,
       `${id} derived anchor matches LAYOUTS literal`);
  }
  // The anchor note must land on the anchor key, whatever the layout or note.
  for (const id of ["bosanquet", "wicki-hayden", "harmonic-table"]) {
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
  ok(String(read("harmonic-table")) === "2,-1,0,-1", "Harmonic Table matrix is (2,-1,0,-1)",
     String(read("harmonic-table")));
  // The three hex directions are a major third, a fifth and a minor third.
  {
    const g = pitchMapFor({ layout: "harmonic-table" }).generator;
    const step = (dx, dy) => {
      const a = generatePitchAt(g, 3, 3), b = generatePitchAt(g, 3 + dx, 3 + dy);
      return `${b.w - a.w},${b.h - a.h}`;
    };
    ok(step(1, 0) === "2,0", "Harmonic Table +x is a major third", step(1, 0));
    ok(step(1, -1) === "3,1", "Harmonic Table up-right is a fifth", step(1, -1));
    ok(step(0, -1) === "1,1", "Harmonic Table up-left is a minor third", step(0, -1));
  }
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
    for (const id of ["bosanquet", "wicki-hayden", "harmonic-table"]) {
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
  for (const id of ["bosanquet", "wicki-hayden", "harmonic-table"]) {
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
  // The selectable list drops EDOs whose fifth cannot reach all their steps.
  {
    const { SELECTABLE_EDOS, effectiveEdo } = await import("../src/lib/tuning.ts");
    for (const edo of [24, 36, 48, 72, 10, 14, 34, 38]) {
      ok(!SELECTABLE_EDOS.includes(edo) && VALID_EDOS.includes(edo),
         `${edo}-EDO is not offered, but still loads`);
    }
    ok(effectiveEdo(24) === 12 && effectiveEdo(72) === 12 && effectiveEdo(34) === 17 &&
       effectiveEdo(10) === 5, "…because they play as 12, 12, 17 and 5");
    for (const edo of [5, 7, 12, 17, 19, 22, 31, 43, 50, 53, 55]) {
      ok(SELECTABLE_EDOS.includes(edo), `${edo}-EDO is offered`);
    }
    ok(SELECTABLE_EDOS.every((e) => SELECTABLE_EDOS.every((m) => m >= e || e % m !== 0 ||
       effectiveEdo(e) === e)), "every offered EDO reaches all of its own steps");
  }

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

// --- 10. colour partitions --------------------------------------------------

section("10. colour partitions: accidental rows are a special case; 7 + 5 repeats every 12");
{
  const {
    defaultGenerator, generateColorAt, partitionsFromAccidentals, partitionIndexAt,
    asAccidentalScheme, colorgenFrame, fifthsOf, fifthsName,
  } = await import("../src/lib/colorMaps.ts");

  ok(fifthsOf(SPN.toPitch("F4")) === 0 && fifthsOf(SPN.toPitch("F#4")) === 7 &&
     fifthsOf(SPN.toPitch("Bb4")) === -1, "fifths position: F = 0, F♯ = 7, B♭ = −1");
  ok(fifthsName(0) === "F" && fifthsName(6) === "B" && fifthsName(7) === "F♯" &&
     fifthsName(-1) === "B♭", "fifths positions are named correctly");

  // Saying the accidental scheme as partitions must not move a single colour,
  // and must still be something the firmware's frame can carry, byte for byte.
  for (const startAccidental of [-2, 0, 1, -7]) {
    for (const palette of [["#fa1414"], ["#fa1414", "#32fa32"], defaultGenerator().palette]) {
      const acc = { ...defaultGenerator(), palette, startAccidental, lightenCDE: true };
      const part = { ...acc, scheme: "partitions", ...partitionsFromAccidentals(acc) };
      for (const id of ["bosanquet", "wicki-hayden", "harmonic-table"]) {
        const g = pitchMapFor({ layout: id }).generator;
        let same = true;
        for (const [x, y] of gridCoords(4)) {
          if (generateColorAt(acc, g, x, y) !== generateColorAt(part, g, x, y)) same = false;
        }
        ok(same, `partitions of 7 colour like accidentals (${id}, start ${startAccidental}, ${palette.length} rows)`);
        // The start accidental may differ by a whole number of palettes, which
        // changes nothing on any key, so compare what the board would draw.
        const back = asAccidentalScheme(part);
        const viaFrame = { ...acc, ...back };
        let frameSame = back !== null;
        for (const [x, y] of gridCoords(4)) {
          if (back && generateColorAt(viaFrame, g, x, y) !== generateColorAt(acc, g, x, y)) frameSame = false;
        }
        ok(frameSame && colorgenFrame(part, g) !== null,
           `…and still fit the firmware's frame (${id}, start ${startAccidental})`);
      }
    }
  }

  // Seven then five, F starting the sevens.
  const sevenFive = {
    ...defaultGenerator(), scheme: "partitions",
    partitions: [{ width: 7, color: "#ffffff" }, { width: 5, color: "#000001" }],
    anchorLetter: 0, anchorPartition: 0,
  };
  const at = (note) => partitionIndexAt(sevenFive, fifthsOf(SPN.toPitch(note)));
  ok(["F4", "C4", "G4", "D4", "A4", "E4", "B4"].every((n) => at(n) === 0), "7 + 5: the naturals are partition 1");
  ok(["F#4", "C#4", "G#4", "D#4", "A#4"].every((n) => at(n) === 1), "7 + 5: the five sharps are partition 2");
  ok(at("E#4") === 0 && at("B#4") === 0, "7 + 5: E♯ and B♯ wrap round to the naturals' colour");
  ok(["Bb4", "Eb4", "Ab4", "Db4", "Gb4"].every((n) => at(n) === 1), "7 + 5: the five flats share the sharps' colour");
  ok(at("Cb4") === 0, "7 + 5: C♭ wraps round to the naturals' colour");
  ok(asAccidentalScheme(sevenFive) === null &&
     colorgenFrame(sevenFive, defaultPitchMap().generator) === null,
     "7 + 5 is not an accidental scheme, so older firmware gets no parameter frame");
  {
    const f = colorgenFrame(sevenFive, defaultPitchMap().generator, { partitions: true, lighten: false });
    ok(f !== null && f.length === 41 && f[14] === 0x82 && f[13] === 0 &&
       f[17 + 18] === 7 && f[17 + 19] === 5,
       "…but firmware with partitions gets one: 0x80|2, origin 0, widths 7 and 5");
    const seven = { ...sevenFive, partitions: Array.from({ length: 7 }, () => ({ width: 3, color: "#010101" })) };
    ok(colorgenFrame(seven, defaultPitchMap().generator, { partitions: true, lighten: true }) === null,
       "seven partitions do not fit the frame");
  }

  // The landmark tint picks the command: 'G' carries any letters and amount,
  // 'K' only "off" or "C D E by 35%".
  {
    const { lightenMask, COLORGEN_CAPS_LATEST } = await import("../src/lib/colorMaps.ts");
    const bos = defaultPitchMap().generator;
    const wh = pitchMapFor({ layout: "wicki-hayden" }).generator;
    const g = { ...defaultGenerator(), lightenCDE: true, lightenLetter: true, lightenLetterIndex: 4,
                lightenCDEAmount: 60, lightenLetterAmount: 60 };
    ok(lightenMask(g, "wicki-hayden") === 0b0000111, "Wicki-Hayden's landmark is C D E");
    ok(lightenMask(g, "bosanquet") === 0b0010000, "Bosanquet's is the one chosen letter (G)");
    ok(lightenMask(g, "harmonic-table") === 0, "the Harmonic Table has none");
    {
      const { lightenPercent } = await import("../src/lib/colorMaps.ts");
      const two = { ...g, lightenCDEAmount: 20, lightenLetterAmount: 80 };
      ok(lightenPercent(two, "wicki-hayden") === 20 && lightenPercent(two, "bosanquet") === 80,
         "each landmark keeps its own amount");
    }
    ok(lightenMask({ ...g, lightenCDE: false }, "wicki-hayden") === 0 &&
       lightenMask({ ...g, lightenLetter: false }, "bosanquet") === 0,
       "each landmark has its own switch");
    const f = colorgenFrame(g, bos, COLORGEN_CAPS_LATEST);
    ok(f !== null && f.length === 42 && f[0] === 0x47 && f[15] === 0b0010000 && f[41] === 60,
       "current firmware gets 'G' with the letter mask and the amount");
    ok(colorgenFrame(g, bos) === null && colorgenFrame(g, wh) === null,
       "older firmware cannot be sent a letter landmark, or C D E at 60%");
    const k = colorgenFrame({ ...g, lightenCDEAmount: 35 }, wh);
    ok(k !== null && k.length === 41 && k[0] === 0x4b && k[15] === 0x01,
       "…but C D E at 35% still goes out as the old 'K' flag");
    ok(colorgenFrame({ ...g, lightenLetter: false }, bos)?.[0] === 0x4b,
       "…as does no landmark at all");
  }

  // The piano scheme is seven-then-five on F; with the centre keys on, D and
  // G♯ / A♭ are picked out, and it is still few enough partitions for the board.
  {
    const { pianoPartitions, COLORGEN_CAPS_LATEST } = await import("../src/lib/colorMaps.ts");
    const colours = { natural: "#000001", accidental: "#000002", centres: false,
                      centreNatural: "#000003", centreAccidental: "#000004" };
    const piano = { ...defaultGenerator(), brightness: 100, scheme: "piano", piano: colours };
    const marked = { ...piano, piano: { ...colours, centres: true } };
    const asParts = { ...sevenFive, brightness: 100, partitions: pianoPartitions(colours) };
    // Find a key playing each note under Bosanquet and ask what colour it gets.
    const g = defaultPitchMap().generator;
    const colourOf = (gen, note) => {
      const want = SPN.toPitch(note);
      for (let x = -40; x <= 40; x++) for (let y = -40; y <= 40; y++) {
        const p = generatePitchAt(g, x, y);
        if (p.w === want.w && p.h === want.h) return generateColorAt(gen, g, x, y);
      }
      return "not found";
    };
    let same = true;
    for (const [x, y] of gridCoords(4)) {
      if (generateColorAt(piano, g, x, y) !== generateColorAt(asParts, g, x, y)) same = false;
    }
    ok(same, "piano colours every key as seven-then-five partitions on F do");
    ok(["C4", "D4", "E4", "F4", "G4", "A4", "B4"].every((n) => colourOf(piano, n) === "#000001"),
       "piano: the naturals take one colour");
    ok(["F#4", "C#4", "G#4", "D#4", "A#4", "Bb4", "Eb4", "Ab4", "Db4", "Gb4"]
         .every((n) => colourOf(piano, n) === "#000002"), "piano: sharps and flats take the other");
    ok(colourOf(marked, "D4") === "#000003" && colourOf(marked, "D5") === "#000003",
       "centre keys: D is picked out among the naturals");
    ok(colourOf(marked, "G#4") === "#000004" && colourOf(marked, "Ab4") === "#000004",
       "centre keys: G♯ and A♭ are picked out among the sharps and flats");
    ok(["C4", "E4", "F4", "G4", "A4", "B4"].every((n) => colourOf(marked, n) === "#000001") &&
       ["F#4", "C#4", "D#4", "A#4", "Bb4", "Eb4", "Db4", "Gb4"].every((n) => colourOf(marked, n) === "#000002"),
       "…and nothing else changes colour");
    ok(pianoPartitions({ ...colours, centres: true }).length === 6 &&
       colorgenFrame(marked, g, COLORGEN_CAPS_LATEST) !== null,
       "with the centre keys it is six partitions, which the board can generate");
  }

  // Moving the anchor slides the whole pattern.
  const fromC = { ...sevenFive, anchorLetter: 1 };
  const atC = (note) => partitionIndexAt(fromC, fifthsOf(SPN.toPitch(note)));
  ok(atC("C4") === 0 && atC("F#4") === 0 && atC("F4") === 1 && atC("C#4") === 1,
     "anchoring on C starts the sevens at C: C…F♯, then C♯…");
  const onSecond = { ...sevenFive, anchorPartition: 1 };
  const at2 = (note) => partitionIndexAt(onSecond, fifthsOf(SPN.toPitch(note)));
  ok(at2("F4") === 1 && at2("A4") === 1 && at2("E4") === 0 && at2("Bb4") === 0,
     "anchoring F on the second partition starts the fives at F");
}

// --- 11. concert pitch -------------------------------------------------------

section("11. concert pitch: the reference note lands on its frequency, here and on the board");
{
  const {
    DEFAULT_REFERENCE, hzAt, referenceOf, referenceOffsetCents, referenceOffsetMilli, referenceError,
  } = await import("../src/lib/tuning.ts");

  const def = defaultPitchMap();
  ok(referenceOffsetMilli(def.generator) === 0, "the default reference moves nothing");
  ok(DEFAULT_REFERENCE.hz.toFixed(2) === "261.63", "…and reads as C4 = 261.63 Hz");
  ok(referenceOffsetCents({ kind: "edo", edo: 12 }, { note: "A4", hz: 440 }) === 0,
     "A4 = 440 in 12-TET is the same statement as the default");
  ok(Math.abs(referenceOffsetCents({ kind: "edo", edo: 31 }, { note: "A4", hz: 440 })) > 1,
     "…but in 31-EDO it is a different one");
  ok(referenceError({ note: "H4", hz: 440 }) !== null && referenceError({ note: "A4", hz: 0 }) !== null,
     "a bad note name or frequency is refused");
  ok(referenceOffsetCents({ kind: "edo", edo: 31 }, { note: "H4", hz: 440 }) === 0,
     "…and changes nothing");

  // 'P' is 'Q' without the offset, so older firmware gets the same tuning.
  {
    const m = pitchMapFor({ referenceNote: "A4", referenceHz: 440 });
    const p = pitchFrame(m), q = pitchFrame(m, true);
    ok(p.length === 17 && p[0] === 0x50 && q.length === 21 && q[0] === 0x51, "frames: 'P' is 17 bytes, 'Q' is 21");
    ok(p.slice(1).every((b, i) => b === q[i + 1]), "'Q' carries the same sixteen bytes as 'P', then the offset");
    ok(new DataView(q.buffer).getInt32(17, true) === referenceOffsetMilli(m.generator),
       "…and the offset is the concert-pitch shift in millicents");
  }

  const TOLERANCE_CENTS = matchToleranceCents();
  const references = [
    { note: "C4", hz: DEFAULT_REFERENCE.hz }, { note: "A4", hz: 440 }, { note: "A4", hz: 415 },
    { note: "C4", hz: 256 }, { note: "F#3", hz: 180 }, { note: "Bb4", hz: 466.16 },
  ];
  const tunings = [
    { kind: "edo", edo: 12 }, { kind: "edo", edo: 19 }, { kind: "edo", edo: 31 },
    { kind: "edo", edo: 53 }, { kind: "fifth", cents: 696.578 },
    { kind: "fifth", cents: FIFTH_MIN }, { kind: "fifth", cents: FIFTH_MAX },
  ];
  let worst = 0;
  for (const ref of references) {
    for (const tuning of tunings) {
      const got = hzAt(tuning, SPN.toPitch(ref.note), ref);
      ok(Math.abs(1200 * Math.log2(got / ref.hz)) < 1e-6,
         `${ref.note} sounds at ${ref.hz} Hz in ${JSON.stringify(tuning)}`, `${got}`);
      for (const id of ["bosanquet", "wicki-hayden", "harmonic-table"]) {
        const m = pitchMapFor({ layout: id, tuning, referenceNote: ref.note, referenceHz: ref.hz });
        const frame = pitchFrame(m, true);
        let agree = true;
        for (const [x, y] of gridCoords(4)) {
          const p = generatePitchAt(m.generator, x, y);
          const app = midiFor(p, tuning, MPE_BEND_SEMITONES, referenceOf(m.generator));
          const fw = firmwarePitch(frame, x, y);
          if (app.note < 0 || app.note > 127) continue;
          // Compare as pitch, not note-then-bend: with an offset in play the two
          // can round to neighbouring notes at a boundary and still agree.
          const d = (app.note - fw.note) * 100 + (app.bend - fw.bend) * (4800 / 8192);
          worst = Math.max(worst, Math.abs(d));
          if (Math.abs(d) >= TOLERANCE_CENTS) agree = false;
        }
        ok(agree, `${id} at ${ref.note} = ${ref.hz} Hz, ${JSON.stringify(tuning)}: app and firmware agree`);
      }
    }
  }
  console.log(`  worst app/firmware disagreement with a concert pitch: ${worst.toFixed(4)}¢`);
}

// --- 12. colours from the tuning ---------------------------------------------

section("12. colours from the tuning: a hue per fifth, a colour per row, always the same");
{
  const { tuningHue, autoColor, autoColorsFor, hueOf, rowSpacing, colourDifference, ledDifference } =
    await import("../src/lib/autoColors.ts");
  const { defaultGenerator, pianoPartitions } = await import("../src/lib/colorMaps.ts");
  const fifthOf = (edo) => fifthCents({ kind: "edo", edo });
  const near = (a, b, tol = 0.5) => Math.abs(a - b) <= tol;

  ok(near(tuningHue(fifthOf(7)), 0), "7-EDO is red");
  ok(near(tuningHue(fifthOf(19)), 22), "19-EDO is deep orange");
  ok(near(tuningHue(fifthOf(31)), 45), "31-EDO is gold");
  ok(near(tuningHue(1200 * Math.log2(1.5)), 120), "a pure fifth is green");
  ok(tuningHue(fifthOf(53)) > 110 && tuningHue(fifthOf(41)) < 150, "53- and 41-EDO are green too");
  ok(near(tuningHue(fifthOf(22)), 225), "22-EDO is blue");
  ok(near(tuningHue(fifthOf(5)), 310), "5-EDO is magenta");
  ok(tuningHue(600) === 0 && tuningHue(800) === 310, "outside the range it holds the end colour");
  {
    let rising = true, last = -1;
    for (let c = FIFTH_MIN; c <= FIFTH_MAX; c += 0.05) {
      const h = tuningHue(c);
      if (h < last) rising = false;
      last = h;
    }
    ok(rising, "the hue only ever moves one way as the fifth widens");
  }

  // The home row wears the tuning's own hue, and is the same whatever the count.
  for (const edo of [7, 19, 31, 12, 53, 22]) {
    const f = fifthOf(edo);
    ok(near(hueOf(autoColor(f, 0, 5)), tuningHue(f), 1.5) && autoColor(f, 0, 2) === autoColor(f, 0, 12),
       `${edo}-EDO: the home row is the tuning's hue`);
  }

  // Any number of rows: every row gets a colour, all different, and asking
  // twice gives the same answer.
  for (const rows of [1, 2, 3, 5, 8, 12]) {
    const g = { ...defaultGenerator(), palette: Array(rows).fill("#000000"), startAccidental: -Math.floor(rows / 2),
                partitions: Array.from({ length: rows }, () => ({ width: 3, color: "#000000" })),
                anchorPartition: Math.floor(rows / 3) };
    const a = autoColorsFor(g, fifthOf(31)), b = autoColorsFor(g, fifthOf(31));
    ok(JSON.stringify(a) === JSON.stringify(b), `${rows} rows: the same tuning gives the same colours`);
    ok(a.palette.length === rows && new Set(a.palette).size === rows, `${rows} rows: each accidental row its own colour`);
    ok(a.partitions.length === rows && new Set(a.partitions.map((p) => p.color)).size === rows &&
       a.partitions.every((p) => p.width === 3), `${rows} partitions: each its own colour, widths untouched`);
    ok(near(hueOf(a.partitions[g.anchorPartition].color), 45, 1.5), `${rows} partitions: the anchored one is home`);
  }

  // The point of all the measuring: at EVERY tuning, neighbouring rows are far
  // enough apart to tell — on a screen, and as LED light, where it is hue that
  // separates two keys and the greens, cyans and purples need far wider steps.
  for (const rows of [2, 5, 8, 12]) {
    const want = rowSpacing(rows);
    let screen = Infinity, led = Infinity, ledHue = Infinity, closest = Infinity;
    for (let c = FIFTH_MIN; c <= FIFTH_MAX; c += 0.25) {
      const below = Math.floor((rows - 1) / 2);
      const all = Array.from({ length: rows }, (_, i) => autoColor(c, i - below, rows));
      for (let i = 1; i < rows; i++) {
        const d = ledDifference(all[i - 1], all[i]);
        screen = Math.min(screen, colourDifference(all[i - 1], all[i]));
        led = Math.min(led, d.distance);
        ledHue = Math.min(ledHue, d.hue);
      }
      for (let i = 0; i < rows; i++) for (let j = i + 1; j < rows; j++) {
        closest = Math.min(closest, colourDifference(all[i], all[j]));
      }
    }
    console.log(`  ${rows} rows, least over every fifth: screen ${screen.toFixed(3)} (want ${want.screen.toFixed(3)}), ` +
                `LED ${led.toFixed(3)} (${want.led.toFixed(3)}), LED hue ${ledHue.toFixed(1)}° (${want.ledHue.toFixed(1)}°)`);
    // Give or take rounding a colour to eight bits a channel.
    ok(screen >= want.screen * 0.95, `${rows} rows: neighbours differ enough on a screen at every tuning`, screen.toFixed(3));
    ok(led >= want.led * 0.95, `${rows} rows: …and as LED light`, led.toFixed(3));
    ok(ledHue >= want.ledHue * 0.95, `${rows} rows: …and in hue as LED light`, ledHue.toFixed(1));
    // Rows that are not neighbours must not come back round to meet each other.
    ok(closest >= want.screen * 0.95, `${rows} rows: no two rows anywhere are closer than neighbours`,
       closest.toFixed(3));
  }

  // The naturals' row of an accidental scheme is home, wherever it sits.
  {
    const g = { ...defaultGenerator(), startAccidental: -2 };
    ok(near(hueOf(autoColorsFor(g, fifthOf(22)).palette[2]), 225, 1.5), "the naturals take the tuning's hue");
  }
  // Two colours on their own sit as far apart as a five-row scheme's flats and
  // sharps, not as close as two neighbouring rows — in every scheme.
  for (const edo of [7, 19, 31, 53, 41, 17, 22, 5]) {
    const f = fifthOf(edo);
    const g = { ...defaultGenerator(), palette: ["#000000", "#000000"], startAccidental: 0,
                partitions: [{ width: 7, color: "#000000" }, { width: 5, color: "#000000" }] };
    const a = autoColorsFor(g, f);
    const pair = [autoColor(f, 0, 5), autoColor(f, -2, 5)];
    // Piano and a pair of partitions put the second colour to the FLAT side of
    // home — the same two colours as accidental rows of just ♭ and ♮.
    const flatNatural = autoColorsFor({ ...g, startAccidental: -1 }, f).palette;
    ok(a.piano.natural === pair[0] && a.piano.accidental === pair[1] &&
       a.partitions[0].color === pair[0] && a.partitions[1].color === pair[1] &&
       flatNatural[1] === pair[0] && flatNatural[0] === pair[1],
       `${edo}-EDO: piano and a pair of partitions match accidental rows of ♭ and ♮`);
    // Accidental rows of ♮ and ♯ still go the sharp way, as their names say.
    ok(a.palette[0] === pair[0] && a.palette[1] === autoColor(f, 2, 5),
       `${edo}-EDO: accidental rows of ♮ and ♯ are home and two steps sharp`);
    const flatSharp = colourDifference(autoColor(f, -1, 5), autoColor(f, 1, 5));
    const two = colourDifference(pair[0], pair[1]);
    ok(two >= rowSpacing(5).screen * 1.4 && two >= flatSharp * 0.6,
       `${edo}-EDO: …which is about a flat-to-sharp difference`, `${two.toFixed(3)} vs ${flatSharp.toFixed(3)}`);
    const on = autoColorsFor({ ...g, piano: { ...g.piano, centres: true } }, f).piano;
    ok(on.natural === a.piano.natural && on.accidental === a.piano.accidental,
       `${edo}-EDO: switching the centre keys on leaves the two main colours alone`);
  }

  // Piano: four colours, all different, with the naturals at home.
  {
    const p = autoColorsFor(defaultGenerator(), fifthOf(19)).piano;
    ok(new Set([p.natural, p.accidental, p.centreNatural, p.centreAccidental]).size === 4 &&
       near(hueOf(p.natural), 22, 1.5), "piano: naturals at home, and four distinct colours");
    ok(pianoPartitions({ ...p, centres: true }).length === 6, "…still six partitions");
  }
}

// --- 13. rotation --------------------------------------------------------------

section("13. rotation: a mapping turns in sixths about its anchor, and the board follows");
{
  const { layoutMatrix, rotateStep, stepIntervalName } = await import("../src/lib/tuning.ts");

  ok(String(rotateStep(1, 0)) === "0,1" && String(rotateStep(0, 1)) === "-1,1" &&
     String(rotateStep(1, -1)) === "1,0", "one turn clockwise takes each edge direction to the next");
  ok(String(rotateStep(1, 0, 6)) === "1,0" && String(rotateStep(1, 0, -1)) === "1,-1",
     "six turns come back round, and a negative turn goes the other way");

  // Bosanquet turned 120° clockwise, as asked for: the whole tone, the diatonic
  // semitone and the chromatic semitone each move two edges round.
  {
    const g = pitchMapFor({ rotation: 2 }).generator;
    const m = layoutMatrix(g);
    ok(stepIntervalName(m, -1, 1) === "whole tone", "120°: (−1, 1) is the ascending whole tone");
    ok(stepIntervalName(m, -1, 0) === "diatonic semitone", "120°: (−1, 0) is the ascending diatonic semitone");
    ok(stepIntervalName(m, 0, 1) === "chromatic semitone", "120°: (0, 1) is the chromatic semitone");
  }

  for (const id of ["bosanquet", "wicki-hayden", "harmonic-table"]) {
    const base = pitchMapFor({ layout: id, anchorNote: "F#3", anchorKey: [1, 5] }).generator;
    const names = [[1, 0], [0, 1], [1, -1]].map(([x, y]) => stepIntervalName(layoutMatrix(base), x, y));
    for (let r = 0; r < 6; r++) {
      const g = { ...base, rotation: r };
      // Each interval is found one edge further round per turn.
      ok([[1, 0], [0, 1], [1, -1]].every(([x, y], i) =>
           stepIntervalName(layoutMatrix(g), ...rotateStep(x, y, r)) === names[i]),
         `${id} at ${r * 60}°: the three intervals have turned with it`);
      // The anchor note stays on the anchor key.
      ok(noteName(generatePitchAt(g, 1, 5)) === "F♯3", `${id} at ${r * 60}°: the anchor note stays put`,
         noteName(generatePitchAt(g, 1, 5)));
      // A key and the same key turned about the anchor play the same note.
      let same = true;
      for (const [x, y] of gridCoords(2)) {
        const [dx, dy] = rotateStep(x - 1, y - 5, r);
        const a = generatePitchAt(base, x, y), b = generatePitchAt(g, 1 + dx, 5 + dy);
        if (a.w !== b.w || a.h !== b.h) same = false;
      }
      ok(same, `${id} at ${r * 60}°: every key has turned about the anchor`);
      // And the firmware, given the frame, agrees with the app.
      const frame = pitchFrame({ generator: g, overrides: {} }, true);
      let agree = true;
      for (const [x, y] of gridCoords(4)) {
        const p = generatePitchAt(g, x, y);
        const fw = firmwarePitch(frame, x, y);
        if (fw.w !== p.w || fw.h !== p.h) agree = false;
      }
      ok(agree, `${id} at ${r * 60}°: the firmware puts every key on the same pitch`);
    }
    const back = { ...base, rotation: 6 };
    ok(String(Object.values(layoutMatrix(back))) === String(Object.values(layoutMatrix(base))),
       `${id}: six turns is no turn`);
  }
}

// --- summary ---------------------------------------------------------------

console.log(
  `\n${failures === 0 ? "PASS" : "FAIL"} — ${checks - failures}/${checks} checks passed`,
);
process.exit(failures === 0 ? 0 : 1);
