<script lang="ts">
  // Which layout the keys are in, chosen from a row of pictures.
  //
  // Each icon is a 2x2 tiling of boards coloured by the active preset's own
  // colour generator, as that layout would colour it — so the icons are a live
  // preview rather than fixed artwork: edit the palette and all of them follow.
  // Picking a layout changes only the layout. The palette, the tuning and the
  // anchor all stay as they are.

  import {
    LAYOUTS, CENTRE_KEY, generatePitchAt, hzAt, noteName, referenceOf, tuningError, stepIntervalName,
    type LayoutId,
  } from "../lib/tuning";
  import { generateColorAt } from "../lib/colorMaps";
  import { ledColor } from "../lib/ledColor";
  import { ICON_SHIFT } from "../lib/tilingIcon";
  import { axialToPixel, hexPoints } from "../lib/layout";
  import { activeColors, activePitch, updatePitchGenerator } from "../lib/presets.svelte";
  import TilingIcon from "./TilingIcon.svelte";

  const pitchGen = $derived(activePitch().generator);

  // What the anchor key actually sounds. Equal to `anchorNote` when the note
  // parses; when it does not, the generator falls back and this shows what the
  // board is really doing rather than what was typed.
  const anchorActual = $derived.by(() => {
    if (tuningError(pitchGen.tuning)) return null;
    const p = generatePitchAt(pitchGen, pitchGen.anchorKey[0], pitchGen.anchorKey[1]);
    return { name: noteName(p), hz: hzAt(pitchGen.tuning, p, referenceOf(pitchGen)) };
  });

  function nudgeAnchorKey(axis: 0 | 1, delta: number) {
    const key: [number, number] = [pitchGen.anchorKey[0], pitchGen.anchorKey[1]];
    key[axis] += delta;
    updatePitchGenerator({ anchorKey: key });
  }

  const colorGen = $derived(activeColors().generator);

  // The three directions through a hexagon's edges, each drawn as a key with an
  // arrow leaving it — tilted as the board is tilted — beside the interval that
  // step makes under the selected layout.
  const HEX_R = 7;
  const HEX_POINTS = hexPoints(0, 0, HEX_R);
  const DIRECTIONS = ([[1, 0], [0, 1], [1, -1]] as const).map(([dx, dy]) => {
    const [px, py] = axialToPixel(dx, dy);
    const len = Math.hypot(px, py);
    const ux = px / len, uy = py / len;
    const tip = 14.5, back = 10.5, half = 3; // arrow length and head, in icon units
    return {
      dx, dy,
      line: `M0 0L${(ux * back).toFixed(2)} ${(uy * back).toFixed(2)}`,
      head: [
        [ux * tip, uy * tip],
        [ux * back - uy * half, uy * back + ux * half],
        [ux * back + uy * half, uy * back - ux * half],
      ].map(([x, y]) => `${x.toFixed(2)},${y.toFixed(2)}`).join(" "),
    };
  });
  const steps = $derived(
    DIRECTIONS.map((d) => ({ ...d, name: stepIntervalName(LAYOUTS[pitchGen.layout], d.dx, d.dy) })),
  );

  /** How the live colour generator would fill an icon key under a layout. */
  function fillFor(layout: LayoutId): (x: number, y: number) => string {
    const asLayout = { ...pitchGen, layout };
    const [dx, dy] = ICON_SHIFT[layout] ?? [0, 0];
    return (x, y) => {
      const c = ledColor(generateColorAt(colorGen, asLayout, x + dx, y + dy));
      return c.off ? "var(--panel-2)" : c.flat;
    };
  }
</script>

<section
  class="mb-3.5 rounded-lg border p-4"
  style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
>
  <h2 class="m-0 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
    Layout
  </h2>
  <p class="m-0 mb-3 text-xs" style="color: var(--text-dim)">
    Choose how notes are mapped onto the keyboard.
  </p>

  <div class="grid grid-cols-2 gap-2.5 min-[720px]:grid-cols-4" role="radiogroup" aria-label="Layout">
    {#each Object.values(LAYOUTS) as l}
      {@const selected = pitchGen.layout === l.id}
      <button
        class="option"
        class:selected
        role="radio"
        aria-checked={selected}
        onclick={() => updatePitchGenerator({ layout: l.id })}
      >
        <span class="icon"><TilingIcon fill={fillFor(l.id)} /></span>
        <span class="name">{l.name}</span>
      </button>
    {/each}

    <!-- Choosing two basis vectors by hand. Not built yet: it needs pitch
         arithmetic that can step outside a single octave/fifth-generated basis. -->
    <button class="option" role="radio" aria-checked="false" disabled title="Not available yet">
      <span class="icon"><TilingIcon /></span>
      <span class="name">Custom</span>
    </button>
  </div>

  <ul class="steps" aria-label="Interval in each direction">
    {#each steps as s}
      <li>
        <svg viewBox="-16 -16 32 32" width="38" height="38" aria-hidden="true">
          <polygon points={HEX_POINTS} fill="var(--panel-2)" stroke="var(--text-dim)" stroke-width="1.2" />
          <path d={s.line} stroke="var(--accent)" stroke-width="1.8" stroke-linecap="round" fill="none" />
          <polygon points={s.head} fill="var(--accent)" />
        </svg>
        {s.name}
      </li>
    {/each}
  </ul>

  <!-- Where the layout sits on the keys: one note pinned to one key, and every
       other key follows from the layout. -->
  <div class="mt-4 flex flex-wrap items-end gap-x-6 gap-y-3">
    <label class="field">
      Anchor note
      <input
        class="btn mono text-sm normal-case tracking-normal"
        style="width: 72px"
        value={pitchGen.anchorNote}
        onchange={(e) => updatePitchGenerator({ anchorNote: e.currentTarget.value.trim() || "D4" })}
        aria-label="Anchor note in scientific pitch notation"
        title="Scientific pitch notation, e.g. D4, F#3, Bb5"
      />
    </label>

    <div class="field">
      on key
      <div class="flex items-center gap-1">
        {#each [0, 1] as axis}
          <span class="mono ml-1 text-xs normal-case" style="color: var(--text-dim)">{axis === 0 ? "x" : "y"}</span>
          <button class="btn px-2 py-1" aria-label="decrease {axis === 0 ? 'x' : 'y'}" onclick={() => nudgeAnchorKey(axis as 0 | 1, -1)}>−</button>
          <span class="mono w-6 text-center text-sm" style="color: var(--text)">{pitchGen.anchorKey[axis]}</span>
          <button class="btn px-2 py-1" aria-label="increase {axis === 0 ? 'x' : 'y'}" onclick={() => nudgeAnchorKey(axis as 0 | 1, 1)}>+</button>
        {/each}
      </div>
    </div>

    <div class="field">
      Sounds
      <span class="mono text-base normal-case tracking-normal" style="color: var(--accent)">
        {anchorActual ? `${anchorActual.name} · ${anchorActual.hz.toFixed(2)} Hz` : "—"}
      </span>
    </div>

    <button
      class="btn text-sm"
      onclick={() => updatePitchGenerator({ anchorKey: [CENTRE_KEY[0], CENTRE_KEY[1]], anchorNote: "D4" })}
    >Re-centre on D4</button>
  </div>
</section>

<style>
  .btn {
    font: inherit;
    color: var(--text);
    background: var(--panel-2);
    border: 1px solid var(--line);
    border-radius: 6px;
    padding: 6px 12px;
    cursor: pointer;
  }
  .btn:hover:not(:disabled) { border-color: var(--accent-dim); }
  .field {
    display: flex;
    flex-direction: column;
    gap: 4px;
    font-size: 10px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--text-dim);
  }
  .steps {
    display: flex;
    flex-wrap: wrap;
    gap: 6px 22px;
    margin: 12px 0 0;
    padding: 0;
    list-style: none;
    font-size: 13px;
    color: var(--text);
  }
  .steps li {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .option {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 8px;
    font: inherit;
    color: var(--text-dim);
    background: var(--panel-2);
    border: 1px solid var(--line);
    border-radius: 8px;
    padding: 10px 10px 8px;
    cursor: pointer;
  }
  .option:hover:not(:disabled) {
    border-color: var(--accent-dim);
  }
  .option.selected {
    border-color: var(--accent);
    color: var(--text);
  }
  .option:disabled {
    opacity: 0.5;
    cursor: default;
  }
  .icon {
    display: block;
    width: 100%;
    height: 84px;
  }
  .name {
    font-size: 13px;
    font-weight: 600;
  }
</style>
