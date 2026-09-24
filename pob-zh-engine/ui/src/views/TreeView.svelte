<script lang="ts" module>
  // Where the camera was when this view last went away. Module scope on
  // purpose: the instance is destroyed and rebuilt whenever the page is
  // re-keyed (F2's language flip), and coming back to a tree scrolled to the
  // middle at default zoom reads as "it lost my place".
  const lastCam = { cx: 0, cy: 0, zoom: 0.12 };
</script>

<script lang="ts">
  // The passive tree. POB has already placed every node and decided its art
  // (bridge tree_data / tree_assets); this view is the camera, the canvas and
  // the hover conversation with the engine (node_hover for the path POB would
  // allocate, node_info for the tooltip POB would show).
  import { onMount } from "svelte";
  import TreeSpecBar from "../components/TreeSpecBar.svelte";
  import TimelessJewelDialog from "../components/TimelessJewelDialog.svelte";
  import { api, type MasteryChoice, type NodeInfo, type NodePower, type TattooOptions, type TooltipLine, type TreeSocket, type TreeState } from "$lib/bridge";
  import { copyText } from "$lib/clipboard";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import { prefs } from "$lib/prefs.svelte";
  import { pobRuns } from "$lib/pobtext";
  import { buildModel, type TreeData, type TreeModel, type TreeNode } from "$lib/tree/model";
  import { Sprites, ART_SCALE } from "$lib/tree/assets";
  import PobText from "../components/PobText.svelte";
  import ClassChangeDialog from "../components/ClassChangeDialog.svelte";

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

  // camera -- restored from the module memo below, so a remount (F2 re-keys the
  // whole page) does not throw the user back to the middle of the tree.
  let cx = lastCam.cx;
  let cy = lastCam.cy;
  let zoom = lastCam.zoom;
  let w = $state(0);
  let h = $state(0);
  let dpr = 1;

  // hover
  let hover = $state<TreeNode | null>(null);
  let hoverPath = $state<Set<number>>(new Set());
  let hoverDep = $state<Set<number>>(new Set());
  let hoverCost = $state<number | null>(null);
  let hoverInfo = $state<NodeInfo | null>(null);
  /** The socketed jewel's own tooltip when the hovered node is a filled socket. */
  let hoverJewel = $state<{ name: string; lines: TooltipLine[] } | null>(null);
  /** Nodes inside the hovered socket's radii, tinted like PassiveTreeView does (node id → colour). */
  let radiusTint = $state<Map<number, string>>(new Map());
  const infoCache = new Map<string, NodeInfo>();
  let mouse = $state({ x: 0, y: 0 });
  let hoverTimer = 0;

  let search = $state("");
  let matches = $state<Set<number>>(new Set());

  /** PoE1's "Find a Timeless Jewel" dialog. */
  let tjOpen = $state(false);

  /** Show Node Power (TreeTab's heat map + Power Report). */
  let power = $state<NodePower | null>(null);
  let powerBusy = $state(false);
  let powerStat = $state("");
  let powerDepth = $state("5");
  let reportOpen = $state(false);
  async function runPower(opts: { enabled?: boolean; stat?: string; maxDepth?: number | null } = {}) {
    powerBusy = true;
    try {
      const r = await api.nodePower(opts);
      power = r.enabled ? r : null;
      if (!r.enabled) reportOpen = false;
      if (r.enabled) powerStat = r.stat ?? "";
      repaint();
    } catch (e: any) {
      app.error = String(e?.message ?? e);
      power = null;
    }
    powerBusy = false;
  }
  function togglePower(on: boolean) {
    if (!on) return void runPower({ enabled: false });
    void runPower({ stat: powerStat || undefined, maxDepth: powerDepth === "all" ? null : Number(powerDepth) });
  }
  // PassiveTreeView's colours: sqrt of the share of the maximum, POB's RED/BLUE theme
  function heatColor(id: number): string | null {
    const p = power?.nodes?.[String(id)];
    const max = power?.powerMax;
    if (!p || !max) return null;
    const band = (v: number, m?: number) => (m && m > 0 ? Math.min(1, Math.sqrt(Math.max(v, 0) / m * 1.5)) : 0);
    if (power?.stat) {
      const c = Math.round(band(p.singleStat ?? 0, max.singleStat) * 255);
      return `rgba(${c},0,0,0.85)`;
    }
    const dps = band(p.offence ?? 0, max.offence);
    const def = band(p.defence ?? 0, max.defence);
    const mix = (Math.max(dps - 0.5, 0) + Math.max(def - 0.5, 0)) / 2;
    return `rgba(${Math.round(dps * 255)},${Math.round(mix * 255)},${Math.round(def * 255)},0.85)`;
  }

  /** Ctrl+D: POB's stat differences in the node tooltip (PassiveTreeView.showStatDifferences). */
  let showDiff = $state(true);
  /** Shift held: the traced path POB would allocate along (PassiveTreeView.tracePath). */
  let trace = $state<number[]>([]);
  /** PoE1 right-click: TreeTab's Replace Modifier (tattoo) dialog. */
  let tattooMenu = $state<(TattooOptions & { x: number; y: number; query: string }) | null>(null);
  /** The compare tree's allocation (TreeTab's Compare), for the overlay. */
  const compareSet = $derived(new Set(tree?.compare?.allocatedNodes ?? []));
  /** The legend's counts: allocated only in the compared tree / only in this one. */
  const compareDiff = $derived.by(() => {
    if (!compareSet.size) return null;
    let there = 0;
    let here = 0;
    for (const id of compareSet) if (!allocated.has(id)) there++;
    for (const id of allocated) if (!compareSet.has(id)) here++;
    return { there, here };
  });

  // questions the engine's click handler asks back
  let masteryMenu = $state<{ id: number; name: string; x: number; y: number; effects: MasteryChoice[]; selected: number | null } | null>(null);
  let classConfirm = $state<{ id: number; className: string; connectFailed: boolean } | null>(null);
  /** PoE2 attribute node: allocate with a choice ("alloc") or switch an allocated one ("switch"). */
  let attrMenu = $state<{ id: number; mode: "alloc" | "switch"; x: number; y: number; options: { index: number; name: string; nameZh: string }[]; last?: number } | null>(null);
  const ATTRS = [
    { index: 1, name: "Strength", nameZh: "" },
    { index: 2, name: "Dexterity", nameZh: "" },
    { index: 3, name: "Intelligence", nameZh: "" },
  ];
  // POB's colorCodes NEGATIVE / POSITIVE: weapon set 1 / 2 allocations
  const SET_COLOR = ["", "#dd0022", "#33ff77"];

  const allocated = $derived(new Set(tree?.allocatedNodes ?? []));
  const nodeModes = $derived(tree?.nodeModes ?? {});
  const modeOf = (id: number) => nodeModes[String(id)] ?? 0;
  const overrides = $derived(tree?.overrides ?? {});
  const socketMap = $derived(new Map<number, TreeSocket>((tree?.sockets ?? []).map((s) => [s.nodeId, s])));
  const jewelRadius = $derived(tree?.jewelRadius ?? []);
  // Timeless jewels cover POB's radius index 3 ("Large"; nodesInRadius[3] in TreeTab)
  const tjRadius = $derived(jewelRadius[2]?.outer ?? Math.max(1200, ...jewelRadius.map((r) => r.outer)));
  /** "^xRRGGBB" → CSS colour (POB's SetDrawColor on the radius rings). */
  const pobColor = (code: string) => pobRuns(code + " ")[0]?.color ?? "#ffffff";
  const currentAsc = $derived(tree?.ascendClassName && tree.ascendClassName !== "None" ? tree.ascendClassName : null);
  const currentClass = $derived(tree?.className ?? null);

  // --- palette (from the page's tokens, read once) ---------------------------
  const pal = { bg: "#0d1014", line: "#2a3038", lineLit: "#4c5563", path: "#5b9dff", alloc: "#e0b35a", allocEdge: "#7a5e2a", dep: "#ff6b6b", search: "#ffcc66", node: "#1a1f26", nodeEdge: "#3a4250" };
  let treeAlpha = 1;
  function readPalette() {
    const cs = getComputedStyle(wrap ?? document.documentElement);
    const v = (n: string, fb: string) => cs.getPropertyValue(n).trim() || fb;
    pal.bg = v("--tree-bg", pal.bg);
    // background image showing through the tree (settings page "Tree backdrop opacity")
    const a = parseFloat(v("--tree-alpha", "1"));
    treeAlpha = Number.isFinite(a) ? Math.min(1, Math.max(0, a)) : 1;
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
    ctx.clearRect(0, 0, w, h);
    ctx.globalAlpha = treeAlpha;
    ctx.fillStyle = pal.bg;
    ctx.fillRect(0, 0, w, h);
    ctx.globalAlpha = 1;

    const pad = 3000 * zoom;
    const x0 = cx - (w / 2 + pad) / zoom;
    const x1 = cx + (w / 2 + pad) / zoom;
    const y0 = cy - (h / 2 + pad) / zoom;
    const y1 = cy + (h / 2 + pad) / zoom;
    const visible = (x: number, y: number) => x >= x0 && x <= x1 && y >= y0 && y <= y1;

    if (S && M.poe2) {
      // PoE2 PassiveTreeView:Draw: background, the class plate (with its
      // ascendancy's art once chosen), BGTreeActive turned to the start node,
      // BGTree, then every ascendancy plate (the current one lit).
      // the tree's own tiled backdrop: what "Tree backdrop" fades (as in the classic window)
      ctx.globalAlpha = treeAlpha;
      S.cover(ctx, "Background2", w, h, 96);
      ctx.globalAlpha = 1;
      const cls = M.classes.find((c) => c.name === currentClass);
      const bg = cls?.background;
      if (cls && bg) {
        const [bx, by] = toScreen(bg.x, bg.y);
        const ascArt = currentAsc ? bg.ascendancies.find((a) => a.key === currentAsc)?.image : undefined;
        S.draw(ctx, ascArt ?? bg.image, bx, by, bg.w * zoom, bg.h * zoom);
        const start = cls.startNodeId != null ? M.nodes.get(cls.startNodeId) : undefined;
        if (start && bg.active) {
          ctx.save();
          ctx.translate(bx, by);
          ctx.rotate(Math.PI / 2 + Math.atan2(start.y - bg.y, start.x - bg.x));
          S.draw(ctx, "BGTreeActive", 0, 0, bg.active.w * zoom, bg.active.h * zoom);
          ctx.restore();
        }
        if (bg.center) S.draw(ctx, "BGTree", bx, by, bg.center.w * zoom, bg.center.h * zoom);
      }
      for (const c of M.classes) {
        for (const a of c.background?.ascendancies ?? []) {
          if (a.replaceBy && a.replaceBy === currentAsc) continue;
          if (a.replace && a.key !== currentAsc) continue;
          if (!visible(a.x, a.y)) continue;
          const [ax, ay] = toScreen(a.x, a.y);
          ctx.globalAlpha = a.key === currentAsc ? 1 : 0.5;
          S.draw(ctx, a.image, ax, ay, a.w * zoom, a.h * zoom);
        }
      }
      ctx.globalAlpha = 1;
      for (const g of M.rings) {
        if (!visible(g.x, g.y)) continue;
        const [gx, gy] = toScreen(g.x, g.y);
        S.drawArt(ctx, g.art, gx, gy, zoom, g.mirrored, data?.artScale ?? 1);
      }
      // notable glows and group centre images sit under the connectors
      if (zoom > 0.03) {
        for (const n of M.nodes.values()) {
          const eff = overrides[String(n.id)]?.effect ?? n.raw.effectImage;
          const d = n.raw.draw;
          if (!eff || !visible(n.x, n.y)) continue;
          const lit = allocated.has(n.id) || hoverPath.has(n.id);
          const hw = (n.kind === "image" ? d?.w : d?.ew) ?? 0;
          const hh = (n.kind === "image" ? d?.h : d?.eh) ?? 0;
          if (!hw) continue;
          const [sx, sy] = toScreen(n.x, n.y);
          ctx.globalAlpha = n.kind === "image" || !lit ? 0.15 : 1;
          S.draw(ctx, eff, sx, sy, hw * zoom, hh * zoom);
        }
        ctx.globalAlpha = 1;
      }
    } else if (S) {
      // the tree's own tiled backdrop: what "Tree backdrop" fades (as in the classic window)
      ctx.globalAlpha = treeAlpha;
      S.cover(ctx, "Background2", w, h, 96);
      ctx.globalAlpha = 1;
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
    type Style = "dim" | "lit" | "path" | "alloc" | "dep" | "set1" | "set2";
    const buckets = new Map<Style, typeof M.edges>();
    for (const e of M.edges) {
      const a = M.nodes.get(e.a)!;
      const b = M.nodes.get(e.b)!;
      if (!visible(a.x, a.y) && !visible(b.x, b.y)) continue;
      const aa = allocated.has(a.id);
      const ab = allocated.has(b.id);
      let st: Style = "dim";
      if (hoverDep.size && hoverDep.has(a.id) && hoverDep.has(b.id)) st = "dep";
      else if (aa && ab) {
        const m = e.asc ? 0 : modeOf(a.id) || modeOf(b.id);
        st = m === 1 ? "set1" : m === 2 ? "set2" : "alloc";
      }
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
      set1: { color: SET_COLOR[1], width: 10, alpha: 1 },
      set2: { color: SET_COLOR[2], width: 10, alpha: 1 },
    };
    ctx.lineCap = "round";
    for (const st of ["dim", "lit", "path", "alloc", "set1", "set2", "dep"] as Style[]) {
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
      if (M.poe2) {
        // PoE2: every piece at GetNodeTargetSize's half extents
        const d = n.raw.draw ?? {};
        if (n.kind === "image") continue;
        if (n.kind === "ascStart") {
          S.draw(ctx, "AscendancyMiddle", sx, sy, (d.ow ?? d.w ?? 50) * zoom, (d.oh ?? d.h ?? 50) * zoom);
          continue;
        }
        if (n.kind === "socket") {
          const fr = n.raw.frames?.[st];
          if (fr && d.w) S.draw(ctx, fr, sx, sy, d.w * zoom, (d.h ?? d.w) * zoom);
          const sk = socketMap.get(n.id);
          if (alloc && sk?.overlay && d.ow) S.draw(ctx, sk.overlay, sx, sy, d.ow * zoom, (d.oh ?? d.ow) * zoom);
        } else {
          if (showIcons && d.w) {
            const icon = iconOf(n, alloc);
            if (icon && S.draw(ctx, icon, sx, sy, d.w * zoom, (d.h ?? d.w) * zoom) && !alloc && hover?.id !== n.id) {
              // PassiveTreeView:LessLuminance on unallocated art
              ctx.beginPath();
              ctx.arc(sx, sy, d.w * zoom, 0, Math.PI * 2);
              ctx.fillStyle = "rgba(0,0,0,0.45)";
              ctx.fill();
            }
          }
          const fr = n.raw.frames?.[st];
          if (fr && d.ow) S.draw(ctx, fr, sx, sy, d.ow * zoom, (d.oh ?? d.ow) * zoom);
          const m = alloc && !n.asc ? modeOf(n.id) : 0;
          if (m && d.ow) {
            ctx.beginPath();
            ctx.arc(sx, sy, d.ow * zoom * 0.8, 0, Math.PI * 2);
            ctx.strokeStyle = SET_COLOR[m];
            ctx.lineWidth = Math.max(1.5, 8 * zoom);
            ctx.stroke();
          }
        }
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
      const tint = radiusTint.get(n.id);
      if (tint && !alloc) {
        ctx.beginPath();
        ctx.arc(sx, sy, Math.max(half * 0.9, 3), 0, Math.PI * 2);
        ctx.fillStyle = tint;
        ctx.globalAlpha = 0.45;
        ctx.fill();
        ctx.globalAlpha = foreign ? 0.55 : 1;
      }
      if (n.kind === "socket") {
        // frame, then the socketed jewel's overlay (PassiveTreeView:Draw, GetJewelSocketOverlay)
        const fr = n.raw.frames?.[st];
        if (fr) S.drawArt(ctx, fr, sx, sy, zoom);
        const sk = socketMap.get(n.id);
        if (alloc && sk?.overlay) S.drawArt(ctx, sk.overlay, sx, sy, zoom);
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
      // Show Node Power: POB tints unallocated nodes by their power
      if (power && !alloc) {
        const col = heatColor(n.id);
        if (col) {
          ctx.beginPath();
          ctx.arc(sx, sy, Math.max(half, 18 * zoom), 0, Math.PI * 2);
          ctx.fillStyle = col;
          ctx.fill();
        }
      }
      // Compare (TreeTab's Compare tick): only in the other tree / only in this one
      if (compareSet.size) {
        const inCompare = compareSet.has(n.id);
        if (inCompare !== alloc) {
          ctx.beginPath();
          ctx.arc(sx, sy, Math.max(half, 20 * zoom) + 3, 0, Math.PI * 2);
          ctx.strokeStyle = inCompare ? "rgba(90,220,140,0.9)" : "rgba(255,107,107,0.9)";
          ctx.lineWidth = 2;
          ctx.stroke();
        }
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

    // jewel radii (PassiveTreeView:Draw's socket pass, above the nodes):
    // every radius while a socket is hovered, the socketed jewel's own ring
    // once allocated. Charm sockets and the inner cluster sockets get none.
    if (S && tree) {
      for (const sk of tree.sockets) {
        if (sk.charm || (sk.expansion && sk.expansionSize !== 2)) continue;
        const n = M.nodes.get(sk.nodeId);
        if (!n || !visible(n.x, n.y)) continue;
        const [sx, sy] = toScreen(n.x, n.y);
        if (hover?.id === n.id) {
          const thread = sk.radiusLabel === "Variable";
          ctx.lineWidth = Math.max(1.5, 5 * zoom);
          for (const rad of jewelRadius) {
            if (thread ? rad.inner === 0 : rad.inner !== 0) continue;
            ctx.strokeStyle = pobColor(rad.col);
            ctx.beginPath();
            ctx.arc(sx, sy, rad.outer * zoom, 0, Math.PI * 2);
            ctx.stroke();
            if (thread) {
              ctx.beginPath();
              ctx.arc(sx, sy, rad.inner * zoom, 0, Math.PI * 2);
              ctx.stroke();
            }
          }
        }
        if (allocated.has(n.id) && sk.radiusIndex) {
          const rad = jewelRadius[sk.radiusIndex - 1];
          if (!rad) continue;
          const outer = rad.outer * zoom;
          const inner = rad.inner * zoom * 1.06;
          if (sk.ringKey) {
            S.drawImage(ctx, `${sk.ringKey}1`, sx, sy, outer, -0.7);
            S.drawImage(ctx, `${sk.ringKey}2`, sx, sy, outer, 0.7);
          } else {
            S.drawImage(ctx, "jewelShadedOuterRing", sx, sy, outer, -0.7);
            S.drawImage(ctx, "jewelShadedOuterRingFlipped", sx, sy, outer, 0.7);
            S.drawImage(ctx, "jewelShadedInnerRing", sx, sy, inner, -0.7);
            S.drawImage(ctx, "jewelShadedInnerRingFlipped", sx, sy, inner, 0.7);
          }
        }
      }
    }
  }

  /** PassiveTree's nodesInRadius, for the hovered socket: first radius (in POB's order) each node falls in. */
  function tintForSocket(n: TreeNode, sk: TreeSocket | undefined): Map<number, string> {
    const out = new Map<number, string>();
    if (!model || !sk || sk.charm || !jewelRadius.length) return out;
    const thread = sk.radiusLabel === "Variable";
    const maxR = Math.max(...jewelRadius.map((r) => r.outer));
    for (const o of model.nodes.values()) {
      if (o.id === n.id || o.kind === "classStart" || o.kind === "ascStart" || o.dynamic) continue;
      const d2 = (o.x - n.x) ** 2 + (o.y - n.y) ** 2;
      if (d2 > maxR * maxR) continue;
      for (const rad of jewelRadius) {
        if (thread ? rad.inner === 0 : rad.inner !== 0) continue;
        if (d2 <= rad.outer * rad.outer && d2 >= rad.inner * rad.inner) {
          out.set(o.id, pobColor(rad.col));
          break;
        }
      }
    }
    return out;
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

  function onKeyUp(e: KeyboardEvent) {
    if (e.key === "Shift") {
      traceMode = false;
      trace = [];
      hoverPath = new Set();
      repaint();
    }
  }

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
  async function onUp(e: PointerEvent) {
    if (!drag) return;
    const wasClick = !drag.moved;
    drag = null;
    if (!wasClick || !hover || app.busy > 0) return;
    const n = hover;
    if (e.button === 2 && !model?.poe2) {
      // PoE1: right-click offers this node's tattoo (TreeTab:ModifyNodePopup)
      void openTattoo(n.id);
      return;
    }
    if (e.button === 2 && model?.poe2 && n.raw.attribute) {
      // PoE2: right-click an attribute node to pick which attribute it grants
      attrMenu = { id: n.id, mode: allocated.has(n.id) ? "switch" : "alloc", x: mouse.x, y: mouse.y, options: attrOptions() };
      return;
    }
    if (e.button !== 0) return;
    if (n.kind === "ascStart" || n.kind === "classStart" || n.kind === "image") return;
    const traced = trace.length > 1 && trace[trace.length - 1] === n.id ? [...trace] : undefined;
    hoverPath = new Set();
    hoverDep = new Set();
    trace = [];
    await clickNode(n.id, traced ? { trace: traced } : {});
  }

  /** Sends the click to POB and follows up on what it asked for. */
  function attrOptions() {
    return ATTRS.map((a) => ({ ...a, nameZh: t(`tree.attr.${a.name}`) }));
  }

  // --- tattoos (PoE1) -------------------------------------------------------
  async function openTattoo(id: number, showLegacy?: boolean) {
    let o: TattooOptions;
    try {
      o = await api.tattooOptions(id, showLegacy);
    } catch (e: any) {
      app.error = String(e?.message ?? e);
      return;
    }
    if (!o.allowed) return;
    tattooMenu = { ...o, x: mouse.x, y: mouse.y, query: "" };
  }
  async function applyTattoo(tattoo?: string | number) {
    if (!tattooMenu) return;
    const id = tattooMenu.id;
    const legacy = tattooMenu.showLegacy;
    tattooMenu = null;
    const r = await app.run(() => api.tattooApply(tattoo == null ? { id, reset: true, showLegacy: legacy } : { id, tattoo, showLegacy: legacy }));
    if (r) applyState(r);
  }

  async function clickNode(id: number, extra: { effect?: number; confirm?: "reset" | "connect"; attribute?: number; trace?: number[] }) {
    const r = await app.run(() => api.treeClick(id, extra));
    if (!r) return;
    if ("needsAttribute" in r && r.needsAttribute) {
      attrMenu = { id: r.id, mode: "alloc", x: mouse.x, y: mouse.y, options: r.options.map((o) => ({ ...o, nameZh: o.nameZh || t(`tree.attr.${o.name}`) })), last: r.last };
      return;
    }
    if ("blocked" in r && r.blocked) {
      app.notice = t("tree.blockedGlobal");
      return;
    }
    if ("needsMastery" in r && r.needsMastery) {
      masteryMenu = { id: r.id, name: r.nameZh || r.name, x: mouse.x, y: mouse.y, effects: r.effects, selected: r.selected ?? null };
      return;
    }
    if ("needsConfirm" in r && r.needsConfirm) {
      classConfirm = { id: r.id, className: r.classNameZh || r.className, connectFailed: !!r.connectFailed };
      return;
    }
    applyState(r as TreeState);
  }

  async function pickAttribute(index: number) {
    if (!attrMenu) return;
    const { id, mode } = attrMenu;
    attrMenu = null;
    if (mode === "alloc") {
      await clickNode(id, { attribute: index });
    } else {
      const r = await app.run(() => api.treeAttribute(id, index));
      if (r) applyState(r);
    }
  }

  async function setAllocMode(mode: number) {
    const r = await app.run(() => api.setAllocMode(mode));
    if (r) applyState(r);
  }

  async function pickMastery(effect: number) {
    if (!masteryMenu) return;
    const id = masteryMenu.id;
    masteryMenu = null;
    const r = await app.run(() => api.selectMastery(id, effect));
    if (r) applyState(r);
  }

  async function answerClass(mode: "reset" | "connect") {
    if (!classConfirm) return;
    const id = classConfirm.id;
    classConfirm = null;
    await clickNode(id, { confirm: mode });
  }

  async function undo() {
    const r = await app.run(() => api.treeUndo());
    if (r) applyState(r);
  }
  async function redo() {
    const r = await app.run(() => api.treeRedo());
    if (r) applyState(r);
  }

  /** A state that came back from a change: draw it now, then let the rest of the page catch up. */
  function applyState(s: TreeState) {
    tree = s;
    infoCache.clear();
    hoverInfo = null;
    rebuild();
    repaint();
    void app.afterTreeChange();
  }
  function setHover(n: TreeNode | null) {
    if (n?.id === hover?.id) return;
    hover = n;
    hoverPath = new Set();
    hoverDep = new Set();
    hoverCost = null;
    hoverInfo = null;
    hoverJewel = null;
    radiusTint = n?.kind === "socket" ? tintForSocket(n, socketMap.get(n.id)) : new Map();
    clearTimeout(hoverTimer);
    if (n) {
      const key = `${n.id}:${app.rev}:${showDiff ? 1 : 0}`;
      const cached = infoCache.get(key) ?? null;
      hoverInfo = cached;
      const sk = n.kind === "socket" ? socketMap.get(n.id) : undefined;
      hoverTimer = window.setTimeout(async () => {
        try {
          const [hv, info, jewel] = await Promise.all([
            api.nodeHover(n.id),
            cached ? Promise.resolve(cached) : api.nodeInfo(n.id, showDiff),
            sk?.itemId ? api.itemTooltip({ id: sk.itemId }) : Promise.resolve(null),
          ]);
          if (hover?.id !== n.id) return;
          // Shift: keep extending the traced path instead of showing the shortest one
          if (traceMode) {
            extendTrace(n.id, hv.path);
            hoverPath = new Set(trace);
            hoverDep = new Set();
          } else {
            hoverPath = new Set(hv.path);
            hoverDep = new Set(hv.depends);
          }
          hoverCost = hv.cost ?? null;
          infoCache.set(key, info);
          hoverInfo = info;
          if (jewel) hoverJewel = { name: sk?.nameZh || sk?.name || "", lines: jewel.lines };
          repaint();
        } catch {
          /* engine busy or node vanished */
        }
      }, 40);
    }
    repaint();
  }
  // PassiveTreeView's trace mode: the first hover seeds the path POB would
  // take, each further hover of a linked node extends it (or trims back to it).
  let traceMode = $state(false);
  function extendTrace(id: number, path: number[]) {
    if (!trace.length) {
      trace = [...path].reverse();
      if (trace[trace.length - 1] !== id) trace.push(id);
      return;
    }
    const last = trace[trace.length - 1];
    if (id === last) return;
    const node = model?.nodes.get(id);
    if (!node || !node.linked.includes(last)) return;
    const at = trace.indexOf(id);
    if (at >= 0) trace = trace.slice(0, at + 1);
    else trace = [...trace, id];
  }
  async function copyNodeText() {
    if (!hover || hover.kind === "socket") return;
    const lines = hoverInfo?.stats ?? hover.raw.stats ?? [];
    if (await copyText(`# ${hover.raw.name}\n${lines.join("\n")}\n`)) app.notice = t("tree.copied");
  }

  function onKey(e: KeyboardEvent) {
    const el = e.target as HTMLElement | null;
    if (e.key === "Shift" && !traceMode) {
      traceMode = true;
      trace = [];
    }
    if (e.ctrlKey && !e.shiftKey && e.key.toLowerCase() === "d") {
      e.preventDefault();
      showDiff = !showDiff;
      infoCache.clear();
      hoverInfo = null;
      app.notice = t(showDiff ? "tree.diffOn" : "tree.diffOff");
      return;
    }
    if (e.ctrlKey && !e.shiftKey && e.key.toLowerCase() === "c" && hover) {
      e.preventDefault();
      void copyNodeText();
      return;
    }
    if (e.key === "Escape") {
      masteryMenu = null;
      classConfirm = null;
      attrMenu = null;
      return;
    }
    if (e.ctrlKey && !e.shiftKey && (e.key === "z" || e.key === "y")) {
      e.preventDefault();
      void (e.key === "z" ? undo() : redo());
      return;
    }
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
      if (n.kind === "classStart" || n.kind === "ascStart" || n.kind === "image") continue;
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
    const dynConnectors = tree?.dynamicConnectors ?? [];
    const key = dyn.map((d) => `${d.id}@${d.x | 0},${d.y | 0}`).join("|") + `#${dynConnectors.length}`;
    if (model && key === dynKey) return;
    dynKey = key;
    model = buildModel(data, dyn, dynGroups, dynConnectors);
    repaint();
  }

  // state follows the build revision (skipped when a click already brought it)
  $effect(() => {
    const rev = app.rev;
    if (!app.loaded) return;
    if (tree && tree.rev === rev) return;
    api
      .getTreeState()
      .then((s) => {
        const switched = tree != null && tree.className !== s.className;
        tree = s;
        infoCache.clear();
        rebuild();
        if (switched) focusClass(); // the build bar changed the class: go look at the new start
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
        if (d.game === "poe2") s.warm("Background2", "PSSkillFrame", "BGTree", "BGTreeActive");
        else s.warm("Background2", "PSSkillFrame", "PSGroupBackground1", "PSGroupBackground2", "PSGroupBackground3");
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

  // Theme / accent changed on the settings page: the canvas has no CSS of its
  // own, so it re-reads the tokens and paints again.
  $effect(() => {
    void prefs.rev;
    readPalette();
    repaint();
  });

  onMount(() => {
    readPalette();
    const ro = new ResizeObserver(resize);
    if (wrap) ro.observe(wrap);
    resize();
    window.addEventListener("keydown", onKey);
    window.addEventListener("keyup", onKeyUp);
    return () => {
      lastCam.cx = cx;
      lastCam.cy = cy;
      lastCam.zoom = zoom;
      ro.disconnect();
      window.removeEventListener("keydown", onKey);
      window.removeEventListener("keyup", onKeyUp);
      if (raf) cancelAnimationFrame(raf);
    };
  });

  const kindLabel = (n: TreeNode) => n.asc ?? t(`tree.kind.${n.kind}`);
</script>

<div class="tree-page">
  <div class="toolbar">
    <TreeSpecBar />
    <span class="vsep"></span>
    <input class="input search" placeholder={t("tree.search")} bind:value={search} bind:this={searchEl} onkeydown={(e) => e.key === "Enter" && nextMatch()} />
    {#if matches.size}<span class="count num">{matches.size}</span>{/if}
    <button class="btn ghost sm" onclick={focusClass}>{t("sidebar.class")}</button>
    <button class="btn ghost sm" onclick={fitAll}>{t("tree.fit")}</button>
    <span class="vsep"></span>
    <button class="btn ghost sm" onclick={undo} disabled={app.busy > 0} title="Ctrl+Z">{t("tree.undo")}</button>
    <button class="btn ghost sm" onclick={redo} disabled={app.busy > 0} title="Ctrl+Y">{t("tree.redo")}</button>
    {#if model?.poe2 && tree}
      <span class="vsep"></span>
      <span class="dim small">{t("tree.allocMode")}</span>
      <div class="seg">
        {#each [0, 1, 2] as m (m)}
          <button class="btn sm" class:primary={(tree.allocMode ?? 0) === m} disabled={app.busy > 0} onclick={() => setAllocMode(m)}>
            {#if m}<i class="dot" style:background={SET_COLOR[m]}></i>{/if}{t(m === 0 ? "tree.allocMain" : m === 1 ? "tree.allocSet1" : "tree.allocSet2")}
          </button>
        {/each}
      </div>
    {/if}
    {#if !model?.poe2 && app.has("timelessJewel")}
      <button class="btn ghost sm" onclick={() => (tjOpen = true)}>{t("tj.open")}</button>
    {/if}
    <span class="vsep"></span>
    <label class="chk small" title={t("tree.powerHint")}>
      <input type="checkbox" checked={!!power} disabled={powerBusy || app.busy > 0} onchange={(e) => togglePower(e.currentTarget.checked)} />
      {t("tree.power")}
    </label>
    {#if power}
      <select class="select sm" value={powerStat} disabled={powerBusy} onchange={(e) => { powerStat = e.currentTarget.value; togglePower(true); }}>
        {#each power.stats as s (s.index)}<option value={s.stat ?? ""}>{s.labelZh || s.label}</option>{/each}
      </select>
      <select class="select sm" value={powerDepth} disabled={powerBusy} onchange={(e) => { powerDepth = e.currentTarget.value; togglePower(true); }} title={t("tree.powerDepth")}>
        <option value="all">{t("tree.powerDepthAll")}</option>
        <option value="3">3</option>
        <option value="5">5</option>
        <option value="10">10</option>
        <option value="15">15</option>
      </select>
      <button class="btn ghost sm" disabled={!power.report?.length} onclick={() => (reportOpen = !reportOpen)}>{t(reportOpen ? "tree.powerHide" : "tree.powerShow")}</button>
    {/if}
    {#if powerBusy}<span class="dim small pulse">{t("tree.powerWorking")}</span>{/if}
    <span class="spacer"></span>
    {#if tree}
      <span class="points num">
        <b>{tree.points.used}</b><span class="dim">/{tree.points.usedMax ?? "?"}</span>
        <span class="dim"> · </span>
        <b>{tree.points.ascUsed}</b><span class="dim">/{tree.points.ascMax ?? "?"}</span>
      </span>
    {/if}
  </div>
  <div class="stage pob-dark" bind:this={wrap}>
    <canvas bind:this={canvas} onwheel={onWheel} onpointerdown={onDown} onpointermove={onMove} onpointerup={onUp} onpointerleave={() => setHover(null)} oncontextmenu={(e) => e.preventDefault()}></canvas>
    {#if artMissing && model}<div class="note">{t("tree.artMissing")}</div>{/if}
    {#if compareDiff && tree?.compare}
      <!-- what the Compare rings mean (the canvas draws them; see paint) -->
      <div class="cmp-legend">
        <span class="cmp-title">{t("tree.compareWith", { name: tree.compare.title || t("tree.specDefault") })}</span>
        <span><i class="ring there"></i>{t("tree.compareOnlyThere", { n: compareDiff.there })}</span>
        <span><i class="ring here"></i>{t("tree.compareOnlyHere", { n: compareDiff.here })}</span>
      </div>
    {/if}
    {#if loadError}
      <div class="veil bad">{loadError}</div>
    {:else if !model}
      <div class="veil dim">{t("tree.loading")}</div>
    {/if}

    {#if masteryMenu}
      <div class="menu" style:left={`${Math.min(masteryMenu.x, w - 380)}px`} style:top={`${Math.min(masteryMenu.y, h - 40 * (masteryMenu.effects.length + 2))}px`}>
        <div class="menu-title">{t("tree.masteryTitle")} · {masteryMenu.name}</div>
        {#each masteryMenu.effects as e (e.effect)}
          <button
            class="choice"
            class:on={e.effect === masteryMenu.selected}
            disabled={e.takenBy != null && e.takenBy !== masteryMenu.id}
            title={e.takenBy != null && e.takenBy !== masteryMenu.id ? t("tree.masteryTaken") : ""}
            onclick={() => pickMastery(e.effect)}
          >
            {#each e.statsZh as s}<span><PobText text={s} muted="var(--c-magic)" /></span>{/each}
          </button>
        {/each}
        <button class="choice cancel" onclick={() => (masteryMenu = null)}>{t("tree.cancel")}</button>
      </div>
    {/if}

    {#if tjOpen}
      <TimelessJewelDialog onclose={() => (tjOpen = false)} {model} {allocated} radius={tjRadius} />
    {/if}

    {#if reportOpen && power?.report?.length}
      <div class="report">
        <div class="rhead">
          <span class="label">{t("tree.powerReport")}</span>
          <span class="dim small">{power.stats.find((s) => (s.stat ?? "") === (power!.stat ?? ""))?.labelZh ?? ""}</span>
          <span class="grow"></span>
          <button class="btn ghost sm" onclick={() => (reportOpen = false)}>×</button>
        </div>
        <table class="rtable">
          <thead>
            <tr><th>{t("tree.powerNode")}</th><th class="n">{t("tree.powerValue")}</th><th class="n">{t("tree.powerPath")}</th><th class="n">{t("tree.powerDist")}</th></tr>
          </thead>
          <tbody>
            {#each power.report.slice(0, 200) as r, i (`${r.id}:${i}`)}
              <tr class:alloc={r.allocated} onclick={() => jumpTo(r.id)}>
                <td title={r.sdZh.join(String.fromCharCode(10))}>{r.nameZh || r.name}</td>
                <td class="n"><PobText text={r.powerStr} /></td>
                <td class="n"><PobText text={r.pathPowerStr} /></td>
                <td class="n dim">{r.pathDist ?? ""}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {/if}

    {#if tattooMenu}
      <div class="menu tattoo" style:left={`${Math.min(tattooMenu.x, w - 420)}px`} style:top={`${Math.min(tattooMenu.y, h - 320)}px`}>
        <div class="menu-title">{t("tree.tattooTitle")} · {tattooMenu.nameZh || tattooMenu.name}{tattooMenu.count ? ` · ${tattooMenu.count}` : ""}</div>
        <input class="input sm" placeholder={t("tree.search")} bind:value={tattooMenu.query} />
        <div class="tattoo-list">
          {#each (tattooMenu.options ?? []).filter((o) => !tattooMenu!.query || (o.nameZh ?? o.name).includes(tattooMenu!.query) || o.linesZh.join(" ").includes(tattooMenu!.query)) as o (o.id)}
            <button class="choice" onclick={() => applyTattoo(o.id)}>
              <span class="tname">{o.nameZh || o.name}</span>
              <span class="dim small">{o.linesZh.join(" / ")}</span>
            </button>
          {/each}
        </div>
        <label class="chk small"><input type="checkbox" checked={tattooMenu.showLegacy} onchange={(e) => openTattoo(tattooMenu!.id, e.currentTarget.checked)} /> {t("tree.tattooLegacy")}</label>
        <button class="choice" onclick={() => applyTattoo()}>{t("tree.tattooReset")}</button>
        <button class="choice cancel" onclick={() => (tattooMenu = null)}>{t("tree.cancel")}</button>
      </div>
    {/if}

    {#if attrMenu}
      <div class="menu" style:left={`${Math.min(attrMenu.x, w - 240)}px`} style:top={`${Math.min(attrMenu.y, h - 200)}px`}>
        <div class="menu-title">{t("tree.attrTitle")}</div>
        {#each attrMenu.options as o (o.index)}
          <button class="choice" class:on={o.index === attrMenu.last} onclick={() => pickAttribute(o.index)}>{o.nameZh || o.name}</button>
        {/each}
        <button class="choice cancel" onclick={() => (attrMenu = null)}>{t("tree.cancel")}</button>
      </div>
    {/if}

    {#if classConfirm}
      <ClassChangeDialog className={classConfirm.className} connectFailed={classConfirm.connectFailed} onanswer={answerClass} oncancel={() => (classConfirm = null)} />
    {/if}

    {#if hover && !masteryMenu && !classConfirm && !attrMenu}
      <div class="tip" style:left={`${Math.min(mouse.x + 16, w - 336)}px`} style:top={`${Math.min(mouse.y + 16, h - 80)}px`}>
        <div class="tip-title">
          <span class="name" class:keystone={hover.kind === "keystone"} class:notable={hover.kind === "notable"}>{nameOf(hover)}</span>
          <span class="kind">{kindLabel(hover)}</span>
        </div>
        <div class="tip-body">
          {#if hoverJewel}
            <div class="line jewel-name">{hoverJewel.name}</div>
            {#each hoverJewel.lines as l, i (i)}
              {#if "sep" in l}
                <hr />
              {:else if i > 0}
                <div class="line" class:center={l.center}><PobText text={l.text} muted="var(--c-magic)" /></div>
              {/if}
            {/each}
          {:else if hoverInfo}
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
            <span class="ok">{t("tree.allocated")}{#if modeOf(hover.id)} · {t("tree.setN", { n: modeOf(hover.id) })}{/if}</span>
            {#if model?.poe2 && hover.raw.attribute}<span class="dim">{t("tree.attrSwitch")}</span>{/if}
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
    flex-wrap: wrap;
    align-items: center;
    gap: 8px;
    padding: 6px 12px;
    border-bottom: 1px solid var(--edge-0);
    background: var(--surface-1);
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
    background: color-mix(in srgb, var(--tree-bg, #0d1014) calc(var(--tree-alpha, 1) * 100%), transparent);
  }
  :global(:root[data-bg]) .stage {
    background: transparent;
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
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-s);
  }
  .cmp-legend {
    position: absolute;
    right: 12px;
    bottom: 12px;
    display: flex;
    flex-direction: column;
    gap: 4px;
    padding: 8px 10px;
    font-size: var(--fs-xs);
    color: var(--ink-1);
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-s);
    pointer-events: none;
  }
  .cmp-title {
    color: var(--ink-0);
  }
  .cmp-legend .ring {
    display: inline-block;
    width: 10px;
    height: 10px;
    margin-right: 6px;
    border-radius: 50%;
    border: 2px solid;
    vertical-align: -1px;
  }
  /* the same two colours paint() strokes the rings in */
  .ring.there {
    border-color: rgba(90, 220, 140, 0.9);
  }
  .ring.here {
    border-color: rgba(255, 107, 107, 0.9);
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
  .vsep {
    width: 1px;
    height: 16px;
    background: var(--edge-1);
    margin: 0 4px;
  }
  .menu {
    position: absolute;
    min-width: 260px;
    max-width: 380px;
    padding: 6px;
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-m);
    box-shadow: var(--shadow-float);
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .menu-title {
    padding: 4px 8px 8px;
    font-size: var(--fs-xs);
    letter-spacing: 0.06em;
    color: var(--ink-2);
  }
  .choice {
    appearance: none;
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: 1px;
    padding: 6px 8px;
    border: 0;
    border-radius: var(--radius-s);
    background: transparent;
    color: var(--ink-0);
    font-size: var(--fs-xs);
    text-align: left;
    white-space: normal;
    cursor: pointer;
  }
  .choice:hover:not(:disabled) {
    background: var(--surface-hover);
  }
  .choice.on {
    box-shadow: inset 2px 0 0 var(--ok);
  }
  .choice:disabled {
    color: var(--ink-4);
    cursor: default;
  }
  .choice.cancel {
    color: var(--ink-3);
    margin-top: 4px;
  }
  .tip {
    position: absolute;
    width: 320px;
    pointer-events: none;
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-top: 2px solid var(--gold);
    border-radius: var(--radius-m);
    box-shadow: var(--shadow-float);
    font-size: var(--fs-sm);
  }
  .tip-title {
    display: flex;
    justify-content: space-between;
    align-items: baseline;
    gap: 8px;
    padding: 8px 12px 6px;
    border-bottom: 1px solid var(--edge-0);
  }
  .tip-title .name {
    font-weight: 600;
    color: var(--ink-0);
  }
  .line.jewel-name {
    color: var(--gold);
    font-weight: 600;
    margin-bottom: 2px;
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
    color: var(--ink-3);
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
    border-top: 1px solid var(--edge-0);
    margin: 5px 0;
  }
  .tip-foot {
    display: flex;
    justify-content: space-between;
    gap: 8px;
    padding: 6px 12px 8px;
    border-top: 1px solid var(--edge-0);
    font-size: var(--fs-xs);
  }
  .seg {
    display: inline-flex;
    gap: 2px;
  }
  .seg .dot {
    display: inline-block;
    width: 8px;
    height: 8px;
    border-radius: 50%;
    margin-right: 5px;
    vertical-align: middle;
  }
  .menu.tattoo {
    width: 420px;
  }
  .tattoo-list {
    max-height: 260px;
    overflow: auto;
  }
  .tname {
    color: var(--ink-0);
  }
  .report {
    position: absolute;
    left: 12px;
    bottom: 12px;
    width: 520px;
    max-height: 46%;
    display: flex;
    flex-direction: column;
    background: color-mix(in srgb, var(--surface-1) 94%, transparent);
    border: 1px solid var(--edge-1);
    border-radius: 6px;
    overflow: hidden;
  }
  .rhead {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 6px 10px;
    border-bottom: 1px solid var(--edge-0);
  }
  .rtable {
    display: block;
    overflow: auto;
    border-collapse: collapse;
    font-size: var(--fs-sm);
    table-layout: fixed;
    width: 100%;
  }
  .rtable thead th {
    position: sticky;
    top: 0;
    background: var(--surface-1);
  }
  .rtable td:first-child,
  .rtable th:first-child {
    width: 55%;
    max-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .rtable th,
  .rtable td {
    padding: 2px 8px;
    text-align: left;
    white-space: nowrap;
  }
  .rtable td.n,
  .rtable th.n {
    text-align: right;
  }
  .rtable tbody tr:hover {
    background: var(--surface-2);
    cursor: default;
  }
  .rtable tr.alloc td:first-child {
    color: var(--gold);
  }
</style>
