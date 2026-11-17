<script lang="ts">
  // A 2x2 tiling of boards, each key filled by whatever the caller says.

  import { ICON_KEYS, ICON_VIEW_BOX } from "../lib/tilingIcon";

  interface Props {
    /** Fill for the key at an absolute grid coordinate. Omit for a blank outline. */
    fill?: (x: number, y: number) => string;
  }
  let { fill }: Props = $props();
</script>

<svg viewBox={ICON_VIEW_BOX} class="block h-full w-full" aria-hidden="true">
  {#each ICON_KEYS as k}
    {#if fill}
      <polygon points={k.points} fill={fill(k.x, k.y)} />
    {:else}
      <polygon points={k.points} fill="var(--panel-2)" stroke="var(--line)" stroke-width="1" />
    {/if}
  {/each}
</svg>
