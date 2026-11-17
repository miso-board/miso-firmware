<script lang="ts">
  // Which layout the keys are in, chosen from a row of pictures.
  //
  // Each icon is a 2x2 tiling of boards coloured by the active preset's own
  // colour generator, as that layout would colour it — so the icons are a live
  // preview rather than fixed artwork: edit the palette and all of them follow.
  // Picking a layout changes only the layout. The palette, the tuning and the
  // anchor all stay as they are.

  import {
    LAYOUTS, CENTRE_KEY, generatePitchAt, hzAt, noteName, referenceOf, tuningError,
    type LayoutId,
  } from "../lib/tuning";
  import { LED_POS, axialToPixel, hexPoints } from "../lib/layout";
  import { RIGHT, DOWN } from "../lib/mesh.svelte";
  import { generateColorAt } from "../lib/colorMaps";
  import { ledColor } from "../lib/ledColor";
  import { activeColors, activePitch, updatePitchGenerator } from "../lib/presets.svelte";

  const S = 10; // hex radius in icon units
  const PAD = S * 1.2;

  // Four boards, tiled the way the hardware tiles: one, its right-hand
  // neighbour, the one below, and the one diagonally across.
  const BOARD_OFFSETS: readonly (readonly [number, number])[] = [
    [0, 0],
    RIGHT,
    DOWN,
    [RIGHT[0] + DOWN[0], RIGHT[1] + DOWN[1]],
  ];
  const KEYS = BOARD_OFFSETS.flatMap(([ox, oy]) =>
    LED_POS.map(([lx, ly]) => {
      const x = lx + ox;
      const y = ly + oy;
      const [px, py] = axialToPixel(x, y, S);
      return { x, y, points: hexPoints(px, py, S * 0.94), px, py };
    }),
  );
  const VIEW_BOX = (() => {
    const xs = KEYS.map((k) => k.px);
    const ys = KEYS.map((k) => k.py);
    const minX = Math.min(...xs) - PAD;
    const minY = Math.min(...ys) - PAD;
    return `${minX} ${minY} ${Math.max(...xs) + PAD - minX} ${Math.max(...ys) + PAD - minY}`;
  })();

  // Which stretch of the pattern each icon shows, as a grid offset from the
  // boards' own coordinates. Purely a matter of framing: these two read better
  // moved (2, 1) along from where the anchor happens to put them, which means
  // each icon key samples the colour from (2, 1) back.
  const ICON_SHIFT: Partial<Record<LayoutId, readonly [number, number]>> = {
    "wicki-hayden": [-2, -1],
    "harmonic-table": [-2, -1],
  };

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

  /** The fill of every icon key, under each layout, from the live generators. */
  const fills = $derived(
    Object.fromEntries(
      Object.values(LAYOUTS).map((l) => {
        const asLayout = { ...pitchGen, layout: l.id };
        return [
          l.id,
          KEYS.map((k) => {
            const [dx, dy] = ICON_SHIFT[l.id] ?? [0, 0];
            const c = ledColor(generateColorAt(colorGen, asLayout, k.x + dx, k.y + dy));
            return c.off ? "var(--panel-2)" : c.flat;
          }),
        ];
      }),
    ) as Record<LayoutId, string[]>,
  );
</script>

<section
  class="mb-3.5 rounded-lg border p-4"
  style="background: var(--panel); border-color: var(--line); box-shadow: var(--shadow)"
>
  <h2 class="m-0 mb-1 text-[11px] font-semibold uppercase tracking-[0.14em]" style="color: var(--text-dim)">
    Layout
  </h2>
  <p class="m-0 mb-3 text-xs" style="color: var(--text-dim)">
    Which interval each direction across the keys is, whatever the tuning.
    <span class="mono">{LAYOUTS[pitchGen.layout].axes}</span>
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
        <svg viewBox={VIEW_BOX} class="icon" aria-hidden="true">
          {#each KEYS as k, i}
            <polygon points={k.points} fill={fills[l.id][i]} />
          {/each}
        </svg>
        <span class="name">{l.name}</span>
      </button>
    {/each}

    <!-- Choosing two basis vectors by hand. Not built yet: it needs pitch
         arithmetic that can step outside a single octave/fifth-generated basis. -->
    <button class="option" role="radio" aria-checked="false" disabled title="Not available yet">
      <svg viewBox={VIEW_BOX} class="icon" aria-hidden="true">
        {#each KEYS as k}
          <polygon points={k.points} fill="var(--panel-2)" stroke="var(--line)" stroke-width="1" />
        {/each}
      </svg>
      <span class="name">Custom</span>
    </button>
  </div>

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
