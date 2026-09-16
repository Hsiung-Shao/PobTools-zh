<script lang="ts">
  // The passive tree. POB has already placed every node and decided its art
  // (bridge tree_data / tree_assets); this view is the camera, the canvas and
  // the hover conversation with the engine (node_hover for the path POB would
  // allocate, node_info for the tooltip POB would show).
  import { onMount } from "svelte";
  import { api, type NodeInfo, type TreeState } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import { buildModel, type TreeData, type TreeModel, type TreeNode } from "$lib/tree/model";
  import { Sprites, ART_SCALE } from "$lib/tree/assets";
  import PobText from "../components/PobText.svelte";

  let canvas = $state<HTMLCanvasElement | null>(null);
  let wrap = $state<HTMLDivElement | null>(null);
  let searchEl = $state<HTMLInputElement | null>(null);

  let data: TreeData | null = null;
  let model = $state<TreeModel | null>(null);
  let sprites: Sprites | null = null;
  let dynKey = "";
  let loadError = $state<string | null>(null);
  let artMissing = $state(false);
  let tree = $state<TreeState | null>(null);

  // camera
  let cx = 0;
  let cy = 0;
  let zoom = 0.12;
  let w = $state(0);
  let h = $state(0);
  let dpr = 1;

  // hover
  let hover = $state<TreeNode | null>(null);
  let hoverPath = $state<Set<number>>(new Set());
  let hoverDep = $state<Set<number>>(new Set());
  let hoverCost = $state<number | null>(null);
  let hoverInfo = $state<NodeInfo | null>(null);
  const infoCache = new Map<string, NodeInfo>();
  let mouse = $state({ x: 0, y: 0 });
  let hoverTimer = 0;

  let search = $state("");
  let matches = $state<Set<number>>(new Set());

  const allocated = $derived(new Set(tree?.allocatedNodes ?? []));
  const overrides = $derived(tree?.overrides ?? {});
  const currentAsc = $derived(tree?.ascendClassName && tree.ascendClassName !== "None" ? tree.ascendClassName : null);
  const currentClass = $derived(tree?.className ?? null);

  // --- palette (from the page's tokens, read once) ---------------------------
  const pal = { bg: "#0d1014", line: "#2a3038", lineLit: "#4c5563", path: "#5b9dff", alloc: "#e0b35a", allocEdge: "#7a5e2a", dep: "#ff6b6b", search: "#ffcc66", node: "#1a1f26", nodeEdge: "#3a4250" };
  function readPalette() {
    const cs = getComputedStyle(wrap ?? document.documentElement);
    const v = (n: string, fb: string) => cs.getPropertyValue(n).trim() || fb;
    pal.bg = v("--tree-bg", pal.bg);
    pal.line = v("--tree-line", pal.line);
    pal.lineLit = v("--tree-line-lit", pal.lineLit);
    pal.path = v("--accent", pal.path);
    pal.alloc = v("--gold", pal.alloc);
    pal.allocEdge = v("--gold-dim", pal.allocEdge);
    pal.dep = v("--bad", pal.dep);
    pal.search = v("--warn", pal.search);
  }

  let dirty = true;
  let raf = 0;
  const repaint = () => {
    dirty = true;
    if (!raf) raf = requestAnimationFrame(paint);
  };

  const toScreen = (x: number, y: number): [number, number] => [(x - cx) * zoom + w / 2, (y - cy) * zoom + h / 2];
  const toWorld = (sx: number, sy: number): [number, number] => [(sx - w / 2) / zoom + cx, (sy - h / 2) / zoom + cy];

  type State = "alloc" | "path" | "unalloc";
  function stateOf(n: TreeNode): State {
    if (allocated.has(n.id) || hover?.id === n.id) return "alloc";
    if (hoverPath.has(n.id)) return "path";
    return "unalloc";
  }
  function iconOf(n: TreeNode, alloc: boolean): string | undefined {
    const ov = overrides[String(n.id)];
    if (ov?.icon) return ov.icon;
    const r = n.raw;
    if (n.kind === "mastery") return alloc ? r.activeIcon : r.inactiveIcon ?? r.icon;
    return r.icon;
  }
  function nameOf(n: TreeNode): string {
    return overrides[String(n.id)]?.nameZh ?? n.raw.nameZh ?? n.raw.name;
  }

  // --- paint -----------------------------------------------------------------
  function paint() {
    raf = 0;
    if (!dirty || !canvas || !model) return;
    dirty = false;
    const M = model;
    const S = sprites;
    const ctx = canvas.getContext("2d")!;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.imageSmoothingEnabled = true;
    ctx.fillStyle = pal.bg;
    ctx.fillRect(0, 0, w, h);

    const pad = 3000 * zoom;
    const x0 = cx - (w / 2 + pad) / zoom;
    const x1 = cx + (w / 2 + pad) / zoom;
    const y0 = cy - (h / 2 + pad) / zoom;
    const y1 = cy + (h / 2 + pad) / zoom;
    const visible = (x: number, y: number) => x >= x0 && x <= x1 && y >= y0 && y <= y1;

    if (S) {
      S.cover(ctx, "Background2", w, h, 96);
      // class illustration + group rings + class start plates (POB's layer order)
      const cls = M.classes.find((c) => c.name === currentClass);
      if (cls?.art && visible(cls.art.x, cls.art.y)) {
        const [ax, ay] = toScreen(cls.art.x, cls.art.y);
        S.drawArt(ctx, cls.art.name, ax, ay, zoom);
      }
      for (const g of M.rings) {
        if (!visible(g.x, g.y)) continue;
        const [gx, gy] = toScreen(g.x, g.y);
        S.drawArt(ctx, g.art, gx, gy, zoom, g.mirrored);
      }
      for (const c of M.classes) {
        if (c.startNodeId == null) continue;
        const n = M.nodes.get(c.startNodeId);
        if (!n || !visible(n.x, n.y)) continue;
        const [sx, sy] = toScreen(n.x, n.y);
        S.drawArt(ctx, c.name === currentClass && n.raw.startArt ? n.raw.startArt : "PSStartNodeBackgroundInactive", sx, sy, zoom);
      }
      for (const p of M.plates) {
        if (!visible(p.x, p.y)) continue;
        const [px, py] = toScreen(p.x, p.y);
        ctx.globalAlpha = p.asc === currentAsc ? 1 : 0.4;
        S.drawArt(ctx, p.art, px, py, zoom);
      }
      ctx.globalAlpha = 1;
      // mastery / tattoo glows sit under the connectors
      if (zoom > 0.05) {
        for (const n of M.nodes.values()) {
          const eff = overrides[String(n.id)]?.effect ?? (n.kind === "mastery" && allocated.has(n.id) ? n.raw.effectImage : undefined);
          if (!eff || !visible(n.x, n.y)) continue;
          const [sx, sy] = toScreen(n.x, n.y);
          S.drawArt(ctx, eff, sx, sy, zoom);
        }
      }
    }

    // connectors: one pass per style so each style is a single stroke
    type Style = "dim" | "lit" | "path" | "alloc" | "dep";
    const buckets = new Map<Style, typeof M.edges>();
    for (const e of M.edges) {
      const a = M.nodes.get(e.a)!;
      const b = M.nodes.get(e.b)!;
      if (!visible(a.x, a.y) && !visible(b.x, b.y)) continue;
      const aa = allocated.has(a.id);
      const ab = allocated.has(b.id);
      let st: Style = "dim";
      if (hoverDep.size && hoverDep.has(a.id) && hoverDep.has(b.id)) st = "dep";
      else if (aa && ab) st = "alloc";
      else if (hoverPath.size) {
        const on = (n: TreeNode) => n.id === hover?.id || hoverPath.has(n.id) || allocated.has(n.id);
        if (on(a) && on(b)) st = "path";
      }
      if (st === "dim" && e.asc && e.asc !== currentAsc) st = "dim";
      let list = buckets.get(st);
      if (!list) buckets.set(st, (list = []));
      list.push(e);
    }
    const strokes: Record<Style, { color: string; width: number; alpha: number }> = {
      dim: { color: pal.line, width: 7, alpha: 1 },
      lit: { color: pal.lineLit, width: 7, alpha: 1 },
      path: { color: pal.path, width: 9, alpha: 1 },
      alloc: { color: pal.alloc, width: 10, alpha: 1 },
      dep: { color: pal.dep, width: 10, alpha: 1 },
    };
    ctx.lineCap = "round";
    for (const st of ["dim", "lit", "path", "alloc", "dep"] as Style[]) {
      const list = buckets.get(st);
      if (!list?.length) continue;
      ctx.beginPath();
      for (const e of list) {
        const a = M.nodes.get(e.a)!;
        const b = M.nodes.get(e.b)!;
        const [ax, ay] = toScreen(a.x, a.y);
        if (e.arc) {
          const [ccx, ccy] = toScreen(e.arc.cx, e.arc.cy);
          const a1 = Math.atan2(a.y - e.arc.cy, a.x - e.arc.cx);
          const a2 = Math.atan2(b.y - e.arc.cy, b.x - e.arc.cx);
          let d = a2 - a1;
          while (d > Math.PI) d -= 2 * Math.PI;
          while (d < -Math.PI) d += 2 * Math.PI;
          ctx.moveTo(ax, ay);
          ctx.arc(ccx, ccy, e.arc.r * zoom, a1, a2, d < 0);
        } else {
          const [bx, by] = toScreen(b.x, b.y);
          ctx.moveTo(ax, ay);
          ctx.lineTo(bx, by);
        }
      }
      const s = strokes[st];
      ctx.strokeStyle = s.color;
      ctx.globalAlpha = s.alpha;
      ctx.lineWidth = Math.max(1, s.width * zoom);
      ctx.stroke();
    }
    ctx.globalAlpha = 1;

    // nodes
    const showIcons = zoom > 0.035;
    for (const n of M.nodes.values()) {
      if (!visible(n.x, n.y) || n.kind === "classStart") continue;
      const [sx, sy] = toScreen(n.x, n.y);
      const alloc = allocated.has(n.id);
      const st = stateOf(n);
      const half = n.size * zoom;
      const foreign = n.asc !== null && n.asc !== currentAsc;
      ctx.globalAlpha = foreign ? 0.55 : 1;

      if (!S) {
        // no art at all: a plain disc says where the node is
        ctx.beginPath();
        ctx.arc(sx, sy, Math.max(half * 0.6, 1.5), 0, Math.PI * 2);
        ctx.fillStyle = alloc ? pal.alloc : st === "path" ? pal.path : pal.node;
        ctx.fill();
        continue;
      }
      if (n.kind === "ascStart") {
        S.drawArt(ctx, n.raw.frames?.unalloc ?? "AscendancyMiddle", sx, sy, zoom);
        continue;
      }
      if (n.kind === "mastery") {
        const icon = iconOf(n, alloc);
        if (icon) {
          const r = S.rect(icon);
          if (r) S.draw(ctx, icon, sx, sy, r.w * ART_SCALE * zoom, r.h * ART_SCALE * zoom, !alloc && hover?.id !== n.id);
        }
        continue;
      }
      if (n.kind === "socket") {
        const fr = n.raw.frames?.[st];
        if (fr) S.drawArt(ctx, fr, sx, sy, zoom);
        continue;
      }
      if (showIcons) {
        const icon = iconOf(n, alloc);
        if (icon) {
          const r = S.rect(icon, !alloc);
          if (r) S.draw(ctx, icon, sx, sy, r.w * ART_SCALE * zoom, r.h * ART_SCALE * zoom, !alloc);
        }
      }
      const fr = n.raw.frames?.[st];
      if (fr) S.drawArt(ctx, fr, sx, sy, zoom);
      ctx.globalAlpha = 1;

      if (hoverDep.has(n.id) && hover?.id !== n.id) {
        ctx.beginPath();
        ctx.arc(sx, sy, Math.max(half, 3), 0, Math.PI * 2);
        ctx.fillStyle = "rgba(255,107,107,0.35)";
        ctx.fill();
      }
      if (matches.has(n.id)) {
        ctx.beginPath();
        ctx.arc(sx, sy, Math.max(half, 24 * zoom) + 5, 0, Math.PI * 2);
        ctx.strokeStyle = pal.search;
        ctx.lineWidth = 1.5;
        ctx.stroke();
      }
    }
    ctx.globalAlpha = 1;
  }

  // --- camera ----------------------------------------------------------------
  function resize() {
    if (!canvas || !wrap) return;
    dpr = window.devicePixelRatio || 1;
    w = wrap.clientWidth;
    h = wrap.clientHeight;
    canvas.width = Math.floor(w * dpr);
    canvas.height = Math.floor(h * dpr);
    canvas.style.width = `${w}px`;
    canvas.style.height = `${h}px`;
    repaint();
  }
  function zoomTo(z: number, sx = w / 2, sy = h / 2) {
    const [wx, wy] = toWorld(sx, sy);
    zoom = Math.min(1.5, Math.max(0.01, z));
    cx = wx - (sx - w / 2) / zoom;
    cy = wy - (sy - h / 2) / zoom;
    repaint();
  }
  function focusClass() {
    if (!model) return;
    const id = currentClass ? model.classStart.get(currentClass) : undefined;
    const n = id != null ? model.nodes.get(id) : undefined;
    if (!n) return fitAll();
    cx = n.x;
    cy = n.y;
    zoom = 0.14;
    repaint();
  }
  function fitAll() {
    if (!model) return;
    const b = model.bounds;
    cx = (b.minX + b.maxX) / 2;
    cy = (b.minY + b.maxY) / 2;
    zoom = Math.min(w / (b.maxX - b.minX), h / (b.maxY - b.minY)) * 0.95;
    repaint();
  }
  function jumpTo(id: number) {
    const n = model?.nodes.get(id);
    if (!n) return;
    cx = n.x;
    cy = n.y;
    if (zoom < 0.2) zoom = 0.25;
    repaint();
  }
  let matchCursor = -1;
  function nextMatch() {
    if (!model || !matches.size) return;
    const ids = [...matches].sort((a, b) => {
      const na = model!.nodes.get(a)!;
      const nb = model!.nodes.get(b)!;
      return Math.hypot(na.x - cx, na.y - cy) - Math.hypot(nb.x - cx, nb.y - cy);
    });
    matchCursor = (matchCursor + 1) % ids.length;
    jumpTo(ids[matchCursor]);
  }

  // --- input -----------------------------------------------------------------
  let drag: { sx: number; sy: number; cx0: number; cy0: number; moved: boolean } | null = null;

  function onWheel(e: WheelEvent) {
    e.preventDefault();
    const r = canvas!.getBoundingClientRect();
    zoomTo(zoom * Math.exp(-e.deltaY * 0.0015), e.clientX - r.left, e.clientY - r.top);
  }
  function onDown(e: PointerEvent) {
    canvas!.setPointerCapture(e.pointerId);
    drag = { sx: e.clientX, sy: e.clientY, cx0: cx, cy0: cy, moved: false };
  }
  function onMove(e: PointerEvent) {
    const r = canvas!.getBoundingClientRect();
    const sx = e.clientX - r.left;
    const sy = e.clientY - r.top;
    mouse = { x: sx, y: sy };
    if (drag) {
      const dx = e.clientX - drag.sx;
      const dy = e.clientY - drag.sy;
      if (Math.abs(dx) + Math.abs(dy) > 4) drag.moved = true;
      if (drag.moved) {
        cx = drag.cx0 - dx / zoom;
        cy = drag.cy0 - dy / zoom;
        repaint();
      }
      return;
    }
    if (!model) return;
    const [wx, wy] = toWorld(sx, sy);
    setHover(model.hit.at(wx, wy));
  }
  function onUp() {
    // 1c is read-only; 1d turns a click into tree_click.
    drag = null;
  }
  function setHover(n: TreeNode | null) {
    if (n?.id === hover?.id) return;
    hover = n;
    hoverPath = new Set();
    hoverDep = new Set();
    hoverCost = null;
    hoverInfo = null;
    clearTimeout(hoverTimer);
    if (n) {
      const key = `${n.id}:${app.rev}`;
      const cached = infoCache.get(key) ?? null;
      hoverInfo = cached;
      hoverTimer = window.setTimeout(async () => {
        try {
          const [hv, info] = await Promise.all([api.nodeHover(n.id), cached ? Promise.resolve(cached) : api.nodeInfo(n.id)]);
          if (hover?.id !== n.id) return;
          hoverPath = new Set(hv.path);
          hoverDep = new Set(hv.depends);
          hoverCost = hv.cost ?? null;
          infoCache.set(key, info);
          hoverInfo = info;
          repaint();
        } catch {
          /* engine busy or node vanished */
        }
      }, 40);
    }
    repaint();
  }
  function onKey(e: KeyboardEvent) {
    const el = e.target as HTMLElement | null;
    if (el && (el.tagName === "INPUT" || el.tagName === "TEXTAREA" || el.tagName === "SELECT")) return;
    if (e.key === "+" || e.key === "=") zoomTo(zoom * 1.3);
    else if (e.key === "-") zoomTo(zoom / 1.3);
    else if (e.key === "h" || e.key === "Home") focusClass();
    else if (e.key === "f") fitAll();
    else if (e.key === "/") searchEl?.focus();
    else return;
    e.preventDefault();
  }

  // --- search (both languages: guides quote English node names) -------------
  $effect(() => {
    const q = search.trim().toLowerCase();
    if (!model || q.length < 2) {
      matches = new Set();
      repaint();
      return;
    }
    const s = new Set<number>();
    for (const n of model.nodes.values()) {
      if (n.kind === "classStart" || n.kind === "ascStart") continue;
      const r = n.raw;
      if (
        r.name.toLowerCase().includes(q) ||
        r.nameZh.toLowerCase().includes(q) ||
        r.stats.some((x) => x.toLowerCase().includes(q)) ||
        r.statsZh.some((x) => x.toLowerCase().includes(q))
      )
        s.add(n.id);
    }
    matches = s;
    repaint();
  });

  // --- data ------------------------------------------------------------------
  function rebuild() {
    if (!data) return;
    const dyn = tree?.dynamicNodes ?? [];
    const dynGroups = tree?.dynamicGroups ?? [];
    const key = dyn.map((d) => `${d.id}@${d.x | 0},${d.y | 0}`).join("|");
    if (model && key === dynKey) return;
    dynKey = key;
    model = buildModel(data, dyn, dynGroups);
    repaint();
  }

  // state follows the build revision
  $effect(() => {
    void app.rev;
    if (!app.loaded) return;
    api
      .getTreeState()
      .then((s) => {
        tree = s;
        infoCache.clear();
        rebuild();
      })
      .catch((e) => (app.error = String(e?.message ?? e)));
  });

  // the static tree follows the version
  let loadedVersion = "";
  $effect(() => {
    const v = tree?.treeVersion;
    if (!v || v === loadedVersion) return;
    loadedVersion = v;
    (async () => {
      try {
        const [d, s] = await Promise.all([api.treeData(v), Sprites.load(v, repaint)]);
        data = d;
        sprites = s;
        artMissing = s.manifest.missingSheets.length > 0;
        s.warm("Background2", "PSSkillFrame", "PSGroupBackground1", "PSGroupBackground2", "PSGroupBackground3");
        dynKey = "";
        rebuild();
        loadError = null;
        resize();
        focusClass();
      } catch (e: any) {
        loadError = String(e?.message ?? e);
      }
    })();
  });

  onMount(() => {
    readPalette();
    const ro = new ResizeObserver(resize);
    if (wrap) ro.observe(wrap);
    resize();
    window.addEventListener("keydown", onKey);
    return () => {
      ro.disconnect();
      window.removeEventListener("keydown", onKey);
      if (raf) cancelAnimationFrame(raf);
    };
  });

  const kindLabel = (n: TreeNode) => n.asc ?? t(`tree.kind.${n.kind}`);
</script>

<div class="tree-page">
  <div class="toolbar">
    <input class="input search" placeholder={t("tree.search")} bind:value={search} bind:this={searchEl} onkeydown={(e) => e.key === "Enter" && nextMatch()} />
    {#if matches.size}<span class="count num">{matches.size}</span>{/if}
    <button class="btn ghost sm" onclick={focusClass}>{t("sidebar.class")}</button>
    <button class="btn ghost sm" onclick={fitAll}>{t("tree.fit")}</button>
    <span class="spacer"></span>
    {#if tree}
      <span class="points num">
        <b>{tree.points.used}</b><span class="dim">/{tree.points.usedMax ?? "?"}</span>
        <span class="dim"> · </span>
        <b>{tree.points.ascUsed}</b><span class="dim">/{tree.points.ascMax ?? "?"}</span>
      </span>
    {/if}
  </div>
  <div class="stage" bind:this={wrap}>
    <canvas bind:this={canvas} onwheel={onWheel} onpointerdown={onDown} onpointermove={onMove} onpointerup={onUp} onpointerleave={() => setHover(null)} oncontextmenu={(e) => e.preventDefault()}></canvas>
    {#if artMissing && model}<div class="note">{t("tree.artMissing")}</div>{/if}
    {#if loadError}
      <div class="veil bad">{loadError}</div>
    {:else if !model}
      <div class="veil dim">{t("tree.loading")}</div>
    {/if}

    {#if hover}
      <div class="tip" style:left={`${Math.min(mouse.x + 16, w - 336)}px`} style:top={`${Math.min(mouse.y + 16, h - 80)}px`}>
        <div class="tip-title">
          <span class="name" class:keystone={hover.kind === "keystone"} class:notable={hover.kind === "notable"}>{nameOf(hover)}</span>
          <span class="kind">{kindLabel(hover)}</span>
        </div>
        <div class="tip-body">
          {#if hoverInfo}
            {#each hoverInfo.lines as l, i (i)}
              {#if "sep" in l}
                <hr />
              {:else if i > 0}
                <div class="line" class:center={l.center}><PobText text={l.text} muted="var(--c-magic)" /></div>
              {/if}
            {/each}
          {:else}
            {#each hover.raw.statsZh as s}<div class="line"><PobText text={s} muted="var(--c-magic)" /></div>{/each}
          {/if}
        </div>
        <div class="tip-foot">
          {#if allocated.has(hover.id)}
            <span class="ok">{t("tree.allocated")}</span>
            <span class="dim">{hoverDep.size > 1 ? t("tree.clickRemoveN", { n: hoverDep.size }) : t("tree.clickRemove")}</span>
          {:else if hoverCost != null}
            <span>{t("tree.pointsN", { n: hoverCost })}</span>
            <span class="dim">{hover.kind === "mastery" ? t("tree.chooseEffect") : t("tree.clickAllocate")}</span>
          {:else}
            <span class="dim">{t("tree.unreachable")}</span>
          {/if}
          <span class="dim num">#{hover.id}</span>
        </div>
      </div>
    {/if}
  </div>
</div>

<style>
  .tree-page {
    height: 100%;
    display: flex;
    flex-direction: column;
    min-height: 0;
  }
  .toolbar {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 6px 12px;
    border-bottom: 1px solid var(--line-0);
    background: var(--bg-1);
  }
  .search {
    width: 240px;
  }
  .count {
    color: var(--warn);
    font-size: var(--fs-xs);
  }
  .spacer {
    flex: 1;
  }
  .points {
    font-size: var(--fs-sm);
  }
  .stage {
    position: relative;
    flex: 1;
    min-height: 0;
    overflow: hidden;
    background: var(--tree-bg, #0d1014);
  }
  canvas {
    display: block;
    cursor: crosshair;
    touch-action: none;
  }
  .note {
    position: absolute;
    left: 12px;
    bottom: 12px;
    padding: 6px 10px;
    font-size: var(--fs-xs);
    color: var(--warn);
    background: var(--bg-1);
    border: 1px solid var(--line-1);
    border-radius: var(--r-1);
  }
  .veil {
    position: absolute;
    inset: 0;
    display: grid;
    place-items: center;
    pointer-events: none;
  }
  .bad {
    color: var(--bad);
  }
  .ok {
    color: var(--ok);
  }
  .tip {
    position: absolute;
    width: 320px;
    pointer-events: none;
    background: var(--bg-1);
    border: 1px solid var(--line-1);
    border-top: 2px solid var(--gold);
    border-radius: var(--r-2);
    box-shadow: var(--shadow-pop);
    font-size: var(--fs-sm);
  }
  .tip-title {
    display: flex;
    justify-content: space-between;
    align-items: baseline;
    gap: 8px;
    padding: 8px 12px 6px;
    border-bottom: 1px solid var(--line-0);
  }
  .tip-title .name {
    font-weight: 600;
    color: var(--fg-0);
  }
  .tip-title .name.keystone {
    color: var(--c-rare);
  }
  .tip-title .name.notable {
    color: var(--gold);
  }
  .tip-title .kind {
    font-size: var(--fs-2xs);
    letter-spacing: 0.08em;
    text-transform: uppercase;
    color: var(--fg-3);
    white-space: nowrap;
  }
  .tip-body {
    padding: 6px 12px;
  }
  .tip-body .line {
    line-height: 1.4;
    white-space: pre-wrap;
  }
  .tip-body .line.center {
    text-align: center;
  }
  .tip-body hr {
    border: 0;
    border-top: 1px solid var(--line-0);
    margin: 5px 0;
  }
  .tip-foot {
    display: flex;
    justify-content: space-between;
    gap: 8px;
    padding: 6px 12px 8px;
    border-top: 1px solid var(--line-0);
    font-size: var(--fs-xs);
  }
</style>
