<!-- Where a jewel socket sits: a small map of the tree around it, the way POB's
     own timeless-jewel socket drop-down shows a zoomed tree on hover. Drawn from
     the page's tree model (positions, connectors) and the allocated set -- no
     sprites, just enough to recognise the spot: allocated paths and nodes in the
     accent colour, keystones named, the jewel's radius as a ring. -->
<script lang="ts">
  import type { TreeModel } from "$lib/tree/model";

  let {
    model,
    allocated,
    nodeId,
    radius,
    size = 260,
  }: { model: TreeModel; allocated: Set<number>; nodeId: number; radius: number; size?: number } = $props();

  let canvas = $state<HTMLCanvasElement | null>(null);

  $effect(() => {
    const cv = canvas;
    const centre = model.nodes.get(nodeId);
    if (!cv) return;
    const dpr = window.devicePixelRatio || 1;
    cv.width = Math.round(size * dpr);
    cv.height = Math.round(size * dpr);
    const ctx = cv.getContext("2d")!;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    const cs = getComputedStyle(cv);
    const tok = (n: string, fb: string) => cs.getPropertyValue(n).trim() || fb;
    const bg = tok("--tree-bg", "#0b0e13");
    const line = tok("--tree-line", "#262e3a");
    const gold = tok("--gold", "#d8aa4b");
    const ink = tok("--ink-1", "#c3bfb4");
    const dim = tok("--ink-3", "#6d6c66");
    ctx.fillStyle = bg;
    ctx.fillRect(0, 0, size, size);
    if (!centre) return;

    const view = Math.max(radius, 600) * 1.25; // tree units from centre to edge
    const k = size / 2 / view;
    const sx = (x: number) => size / 2 + (x - centre.x) * k;
    const sy = (y: number) => size / 2 + (y - centre.y) * k;
    const near = (x: number, y: number) => Math.abs(x - centre.x) < view * 1.2 && Math.abs(y - centre.y) < view * 1.2;

    // connectors
    ctx.lineWidth = 1.5;
    for (const e of model.edges) {
      const a = model.nodes.get(e.a);
      const b = model.nodes.get(e.b);
      if (!a || !b || e.asc || !(near(a.x, a.y) || near(b.x, b.y))) continue;
      ctx.strokeStyle = allocated.has(a.id) && allocated.has(b.id) ? gold : line;
      ctx.beginPath();
      if (e.arc) {
        const a0 = Math.atan2(a.y - e.arc.cy, a.x - e.arc.cx);
        let a1 = Math.atan2(b.y - e.arc.cy, b.x - e.arc.cx);
        let d = a1 - a0;
        while (d > Math.PI) d -= Math.PI * 2;
        while (d < -Math.PI) d += Math.PI * 2;
        a1 = a0 + d;
        ctx.arc(sx(e.arc.cx), sy(e.arc.cy), e.arc.r * k, a0, a1, d < 0);
      } else {
        ctx.moveTo(sx(a.x), sy(a.y));
        ctx.lineTo(sx(b.x), sy(b.y));
      }
      ctx.stroke();
    }

    // the jewel's radius
    ctx.strokeStyle = gold;
    ctx.globalAlpha = 0.55;
    ctx.setLineDash([4, 4]);
    ctx.beginPath();
    ctx.arc(size / 2, size / 2, radius * k, 0, Math.PI * 2);
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.globalAlpha = 1;

    // nodes
    const labels: { x: number; y: number; text: string }[] = [];
    for (const n of model.nodes.values()) {
      if (n.asc || n.kind === "classStart" || n.kind === "ascStart" || n.kind === "image" || !near(n.x, n.y)) continue;
      const r = n.kind === "keystone" ? 7 : n.kind === "notable" ? 5 : n.kind === "socket" ? 5 : n.kind === "mastery" ? 3 : 2.5;
      const on = allocated.has(n.id);
      ctx.beginPath();
      ctx.arc(sx(n.x), sy(n.y), r, 0, Math.PI * 2);
      if (n.kind === "socket") {
        ctx.strokeStyle = on ? gold : ink;
        ctx.lineWidth = 1.5;
        ctx.stroke();
      } else {
        ctx.fillStyle = on ? gold : n.kind === "mastery" ? dim : ink;
        ctx.globalAlpha = on ? 1 : n.kind === "normal" ? 0.55 : 0.85;
        ctx.fill();
        ctx.globalAlpha = 1;
      }
      if (n.kind === "keystone") labels.push({ x: sx(n.x), y: sy(n.y) - r - 3, text: n.raw.nameZh || n.raw.name });
    }

    // the socket itself
    ctx.strokeStyle = gold;
    ctx.lineWidth = 2.5;
    ctx.beginPath();
    ctx.arc(size / 2, size / 2, 9, 0, Math.PI * 2);
    ctx.stroke();

    ctx.font = `600 ${Math.max(10, Math.round(size / 24))}px ${getComputedStyle(document.documentElement).getPropertyValue("--font-ui")}`;
    ctx.textAlign = "center";
    ctx.textBaseline = "bottom";
    for (const l of labels) {
      if (l.x < 0 || l.x > size || l.y < 12 || l.y > size) continue;
      ctx.lineWidth = 3;
      ctx.strokeStyle = bg;
      ctx.strokeText(l.text, l.x, l.y);
      ctx.fillStyle = ink;
      ctx.fillText(l.text, l.x, l.y);
    }
  });
</script>

<canvas class="pob-dark" bind:this={canvas} style:width={`${size}px`} style:height={`${size}px`}></canvas>

<style>
  canvas {
    display: block;
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-m);
  }
</style>
