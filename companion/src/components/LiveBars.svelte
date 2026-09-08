<script lang="ts">
  import { onMount } from "svelte";
  import { keys, NUM_KEYS } from "../lib/engine";

  let canvas: HTMLCanvasElement;

  const css = (name: string) =>
    getComputedStyle(document.documentElement).getPropertyValue(name).trim();

  function draw() {
    const dpr = window.devicePixelRatio || 1;
    const W = canvas.clientWidth;
    const H = 190;
    canvas.width = Math.round(W * dpr);
    canvas.height = Math.round(H * dpr);
    const ctx = canvas.getContext("2d")!;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

    const padL = 34;
    const padB = 16;
    const padT = 6;
    const plotW = W - padL - 6;
    const plotH = H - padT - padB;
    const y = (v: number) => padT + plotH - (v / 4095) * plotH;

    ctx.clearRect(0, 0, W, H);
    ctx.font = "10px 'IBM Plex Mono', monospace";
    ctx.strokeStyle = css("--grid");
    ctx.fillStyle = css("--text-dim");
    ctx.lineWidth = 1;
    for (const g of [0, 1024, 2048, 3072, 4095]) {
      ctx.beginPath();
      ctx.moveTo(padL, y(g));
      ctx.lineTo(W - 6, y(g));
      ctx.stroke();
      ctx.textAlign = "right";
      ctx.textBaseline = "middle";
      ctx.fillText(String(g), padL - 5, y(g));
    }

    const bw = plotW / NUM_KEYS;
    for (let i = 0; i < NUM_KEYS; i++) {
      const x = padL + i * bw;
      const k = keys[i];
      const base = isNaN(k.rest) ? y(0) : y(k.rest);
      const top = y(k.cur);
      ctx.fillStyle = css("--bar");
      ctx.fillRect(x + bw * 0.18, Math.min(base, top), bw * 0.64, Math.max(1, Math.abs(base - top)));
      if (k.min <= k.max) {
        ctx.fillStyle = css("--accent");
        ctx.fillRect(x + bw * 0.12, y(k.max) - 1, bw * 0.76, 2);
        ctx.fillRect(x + bw * 0.12, y(k.min) - 1, bw * 0.76, 2);
      }
      if (!isNaN(k.rest)) {
        ctx.strokeStyle = css("--text-dim");
        ctx.setLineDash([2, 3]);
        ctx.beginPath();
        ctx.moveTo(x + bw * 0.08, y(k.rest));
        ctx.lineTo(x + bw * 0.92, y(k.rest));
        ctx.stroke();
        ctx.setLineDash([]);
      }
      if (i % 5 === 0 || i === NUM_KEYS - 1) {
        ctx.fillStyle = css("--text-dim");
        ctx.textAlign = "center";
        ctx.textBaseline = "top";
        ctx.fillText(String(i), x + bw / 2, padT + plotH + 4);
      }
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

<canvas bind:this={canvas} class="block w-full" height="190"></canvas>
