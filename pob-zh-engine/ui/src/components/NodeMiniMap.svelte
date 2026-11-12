<!-- 天賦節點在整棵樹上的位置:整棵樹縮成小圖,已配置的點亮,目標節點加圈與十字線。
     經典視窗在細項的天賦來源上也會畫出樹的位置。 -->
<script lang="ts" module>
  import { api } from "$lib/bridge";
  import type { TreeData } from "$lib/tree/model";

  // the static tree is large: one request per version for the page's lifetime
  const dataCache = new Map<string, Promise<TreeData>>();
</script>

<script lang="ts">
  import { buildModel } from "$lib/tree/model";
  import { fitTransform } from "$lib/tree/minimap";

  let { node, size = 200 }: { node: number; size?: number } = $props();
  let canvas = $state<HTMLCanvasElement | null>(null);

  $effect(() => {
    const id = node;
    const cv = canvas;
    if (!cv) return;
    let dead = false;
    (async () => {
      const st = await api.getTreeState();
      let dp = dataCache.get(st.treeVersion);
      if (!dp) dataCache.set(st.treeVersion, (dp = api.treeData(st.treeVersion)));
      const model = buildModel(await dp, st.dynamicNodes, st.dynamicGroups, st.dynamicConnectors ?? []);
      if (dead) return;
      const asc = st.ascendClassName && st.ascendClassName !== "None" ? st.ascendClassName : null;
      const alloc = new Set(st.allocatedNodes);
      const target = model.nodes.get(id);
      // other ascendancies' nodes are not part of this character's tree
      const shown = [...model.nodes.values()].filter((n) => n.kind !== "image" && (n.asc === null || n.asc === asc || n === target));
      const ids = new Set(shown.map((n) => n.id));
      const f = fitTransform(shown, size, size);
      const css = getComputedStyle(cv);
      const col = (v: string) => css.getPropertyValue(v).trim();
      const dpr = window.devicePixelRatio || 1;
      cv.width = cv.height = Math.round(size * dpr);
      const ctx = cv.getContext("2d")!;
      ctx.scale(dpr, dpr);
      const X = (n: { x: number }) => n.x * f.s + f.ox;
      const Y = (n: { y: number }) => n.y * f.s + f.oy;
      // shortcut: arcs drawn as straight lines, invisible at this scale; upgrade if the map gets a zoomed mode
      for (const on of [false, true]) {
        ctx.beginPath();
        for (const e of model.edges) {
          if (!ids.has(e.a) || !ids.has(e.b) || (alloc.has(e.a) && alloc.has(e.b)) !== on) continue;
          const a = model.nodes.get(e.a)!;
          const b = model.nodes.get(e.b)!;
          ctx.moveTo(X(a), Y(a));
          ctx.lineTo(X(b), Y(b));
        }
        ctx.strokeStyle = col(on ? "--hl" : "--ink-3");
        ctx.lineWidth = on ? 1.2 : 0.6;
        ctx.stroke();
      }
      ctx.fillStyle = col("--hl");
      for (const n of shown) if (alloc.has(n.id)) ctx.fillRect(X(n) - 1, Y(n) - 1, 2, 2);
      if (!target) return;
      const tx = X(target);
      const ty = Y(target);
      ctx.strokeStyle = ctx.fillStyle = col("--ink-0") || "#fff";
      ctx.lineWidth = 1;
      ctx.globalAlpha = 0.35;
      ctx.beginPath();
      ctx.moveTo(tx, 0);
      ctx.lineTo(tx, size);
      ctx.moveTo(0, ty);
      ctx.lineTo(size, ty);
      ctx.stroke();
      ctx.globalAlpha = 1;
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.arc(tx, ty, 7, 0, Math.PI * 2);
      ctx.stroke();
      ctx.beginPath();
      ctx.arc(tx, ty, 2, 0, Math.PI * 2);
      ctx.fill();
    })().catch(() => {
      /* no tree to draw: the tooltip text still stands */
    });
    return () => {
      dead = true;
    };
  });
</script>

<canvas bind:this={canvas} style:width={`${size}px`} style:height={`${size}px`}></canvas>

<style>
  canvas {
    display: block;
    margin: 8px auto 0;
    border: 1px solid var(--edge-1);
    border-radius: 3px;
    background: var(--surface-1);
  }
</style>
