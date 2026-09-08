<script lang="ts">
  import { onMount } from "svelte";
  import { traces } from "../lib/engine";
  import { ui } from "../lib/ui.svelte";

  let canvas: HTMLCanvasElement;

  const css = (name: string) =>
    getComputedStyle(document.documentElement).getPropertyValue(name).trim();

  function draw() {
    const dpr = window.devicePixelRatio || 1;
    const W = canvas.clientWidth;
    const H = 300;
    canvas.width = Math.round(W * dpr);
    canvas.height = Math.round(H * dpr);
    const ctx = canvas.getContext("2d")!;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

    const padL = 44;
    const padB = 20;
    const padT = 8;
    const padR = 8;
    const plotW = W - padL - padR;
    const plotH = H - padT - padB;
    ctx.clearRect(0, 0, W, H);
    ctx.font = "10px 'IBM Plex Mono', monospace";

    const tMin = -30;
    const tMax = 200;
    let vMin = Infinity;
    let vMax = -Infinity;
    for (const tr of traces)
      for (const [, v] of tr.pts) {
        if (v < vMin) vMin = v;
        if (v > vMax) vMax = v;
      }
    if (!isFinite(vMin)) {
      vMin = 2000;
      vMax = 2800;
    }
    const span = Math.max(60, vMax - vMin);
    vMin -= span * 0.08;
    vMax += span * 0.08;
    const x = (t: number) => padL + ((t - tMin) / (tMax - tMin)) * plotW;
    const y = (v: number) => padT + plotH - ((v - vMin) / (vMax - vMin)) * plotH;

    ctx.strokeStyle = css("--grid");
    ctx.fillStyle = css("--text-dim");
    ctx.lineWidth = 1;
    for (let t = 0; t <= tMax; t += 50) {
      ctx.beginPath();
      ctx.moveTo(x(t), padT);
      ctx.lineTo(x(t), padT + plotH);
      ctx.stroke();
      ctx.textAlign = "center";
      ctx.textBaseline = "top";
      ctx.fillText(`${t} ms`, x(t), padT + plotH + 6);
    }
    const step = Math.pow(10, Math.floor(Math.log10((vMax - vMin) / 4)));
    const inc = Math.ceil((vMax - vMin) / 4 / step) * step;
    for (let v = Math.ceil(vMin / inc) * inc; v <= vMax; v += inc) {
      ctx.beginPath();
      ctx.moveTo(padL, y(v));
      ctx.lineTo(W - padR, y(v));
      ctx.stroke();
      ctx.textAlign = "right";
      ctx.textBaseline = "middle";
      ctx.fillText(String(v), padL - 5, y(v));
    }

    ctx.strokeStyle = css("--text-dim");
    ctx.setLineDash([3, 4]);
    ctx.beginPath();
    ctx.moveTo(x(0), padT);
    ctx.lineTo(x(0), padT + plotH);
    ctx.stroke();
    ctx.setLineDash([]);

    for (const tr of traces) {
      ctx.strokeStyle = tr.color;
      ctx.lineWidth = 1.6;
      ctx.beginPath();
      let first = true;
      for (const [t, v] of tr.pts) {
        if (first) {
          ctx.moveTo(x(t), y(v));
          first = false;
        } else {
          ctx.lineTo(x(t), y(v));
        }
      }
      ctx.stroke();
    }

    if (traces.length === 0) {
      ctx.fillStyle = css("--text-dim");
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      ctx.fillText("waiting for a key press…", padL + plotW / 2, padT + plotH / 2);
    }
  }

  onMount(() => {
    let running = true;
    const loop = () => {
      if (!running) return;
      draw();
      requestAnimationFrame(loop);
    };
    loop();
    return () => {
      running = false;
    };
  });
</script>

<canvas bind:this={canvas} class="block w-full" height="300"></canvas>

<ul class="mono mt-2 flex list-none flex-col gap-0.5 p-0 text-xs">
  {#each ui.traces as tr}
    <li class="flex items-center gap-3" style="color: var(--text-dim)">
      <span class="h-2.5 w-2.5 rounded-sm" style="background: {tr.color}"></span>
      <span>
        key <b class="font-medium" style="color: var(--text)">{String(tr.key).padStart(2, "0")}</b>
        · depth <b class="font-medium" style="color: var(--text)">{Math.round(tr.peakDev)}</b> counts
        · transit 20→70%
        <b class="font-medium" style="color: var(--text)">
          {tr.transitMs === null ? "—" : `${tr.transitMs.toFixed(1)} ms`}
        </b>
      </span>
    </li>
  {/each}
</ul>
