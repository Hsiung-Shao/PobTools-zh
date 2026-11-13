  import { untrack } from "svelte";
  import { api, type GemHit, type GemInstance, type GemOptions, type GroupExtras, type SkillsList, type SocketGroup, type TooltipLine } from "$lib/bridge";
  import { copyText } from "$lib/clipboard";
  import { dpsDeltaColor, dpsDeltaText, formatSignedNumber } from "$lib/gemDps";
  import { t } from "$lib/i18n";
  import { pobPlain } from "$lib/pobtext";
  import { app } from "$lib/state.svelte";
  import { dataKey } from "$lib/lang.svelte";
  import PobText from "$lib/ui/PobText.svelte";
  import TooltipCard from "$lib/ui/TooltipCard.svelte";
  import Select from "$lib/ui/Select.svelte";

  let data = $state<SkillsList | null>(null);
  let sel = $state<number>(1);
  let loadedRev = "";

  type TipData = { lines: TooltipLine[] };
  let tip = $state<{ lines: TooltipLine[]; x: number; y: number } | null>(null);
  let tipTimer = 0;
  // bumped by every show/hide, so a tooltip that finishes loading after the
  // mouse has moved on does not pop up
  let tipSeq = 0;
  const tipCache = new Map<string, TipData>();

  // gem search box (add row) and inline rename of an existing gem
  let addQuery = $state("");
  let addHits = $state<GemHit[]>([]);
  let addOpen = $state(false);
  // "supportable": the empty add box lists the supports that can support this
  // group, best DPS gain first; "search": what was typed
  let addMode = $state<"supportable" | "search">("search");
  let addBusy = $state(false);
  let addSeq = 0;
  const supportableCache = new Map<string, GemHit[]>();
  let searchTimer = 0;
  let editGem = $state<{ index: number; query: string; hits: GemHit[] } | null>(null);
  let labelDraft = $state("");
  let setDialog = $state<{ mode: "new" | "rename"; title: string; copy: boolean } | null>(null);

  const group = $derived(data ? data.groups.find((g) => g.index === sel) : undefined);

  // the selected group's count / imbued support / Optimise Sockets
  let extras = $state<GroupExtras | null>(null);
  let countDraft = $state("");
  let imbuedQuery = $state("");
  let imbuedHits = $state<GemHit[]>([]);
  let imbuedOpen = $state(false);
  let imbuedTimer = 0;
  async function loadExtras(i: number) {
    try {
      const x = await api.groupExtras(i);
      if (x.index !== sel) return;
      // a count being typed (not yet applied) survives a re-read (F2, a recalculation)
      const typing = extras?.index === x.index && String(countDraft) !== String(extras.count);
      extras = x;
      if (!typing) countDraft = String(x.count);
    } catch {
      extras = null;
    }
  }
  $effect(() => {
    const i = sel;
    const rev = app.dataRev;
    void rev;
    if (!data || !data.groups.some((g) => g.index === i)) return;
    untrack(() => void loadExtras(i));
  });
  async function applyCount() {
    if (!group || !extras || String(extras.count) === String(countDraft)) return;
    const n = Math.max(1, Math.floor(Number(countDraft) || 1));
    const r = await app.run(() => api.setGroupCount(group.index, n));
    if (r) {
      countDraft = String(n); // what POB got (0 / 2.7 are clamped), so the re-read below takes it
      await changed();
    }
  }
  // F2: lists fetched once (gem options, the open search results) come
  // translated -- read them again; the queries stay as typed
  let seenLang = app.langRev;
  $effect(() => {
    const lr = app.langRev;
    if (lr === seenLang) return;
    seenLang = lr;
    untrack(() => {
      if (gemOpts) void loadGemOpts(true); // the default-quality box may be mid-edit
      if (addOpen) addInput();
      if (imbuedOpen && imbuedQuery.trim()) searchImbued(imbuedQuery);
    });
  });
  function searchImbued(q: string) {
    clearTimeout(imbuedTimer);
    if (!q.trim()) {
      imbuedHits = [];
      return;
    }
    imbuedTimer = window.setTimeout(async () => {
      try {
        imbuedHits = (await api.gemSearch({ query: q.trim(), limit: 20, supportOnly: true })).gems;
      } catch {
        imbuedHits = [];
      }
    }, 120);
  }
  async function setImbued(gemId?: string) {
    if (!group) return;
    imbuedOpen = false;
    imbuedQuery = "";
    const r = await app.run(() => api.setImbuedSupport(group.index, gemId));
    if (r) await changed();
  }
  async function optimise() {
    if (!group) return;
    const r = await app.run(() => api.optimiseSockets(group.index));
    if (r) await changed();
  }

  // CopySocketGroup / PasteSocketGroup
  let pasteDialog = $state<{ text: string } | null>(null);
  async function copyGroup() {
    if (!group) return;
    const r = await app.run(() => api.copyGroup(group.index));
    if (r && (await copyText(r.text))) app.notice = t("skills.copied");
  }
  async function openPaste() {
    let text = "";
    try {
      text = (await navigator.clipboard?.readText?.()) ?? "";
    } catch {
      text = "";
    }
    pasteDialog = { text };
  }
  async function pasteOk() {
    if (!pasteDialog) return;
    const text = pasteDialog.text;
    pasteDialog = null;
    const r = await app.run(() => api.pasteGroup(text));
    if (r) {
      sel = r.index;
      await changed();
    }
  }

  // Gem Options (the section under POB's group list)
  let gemOpts = $state<GemOptions | null>(null);
  let gemOptsOpen = $state(false);
  let qualityDraft = $state("");
  async function loadGemOpts(keepDraft = false) {
    const r = await app.run(() => api.getGemOptions());
    if (r) {
      gemOpts = r;
      if (!keepDraft) qualityDraft = String(r.defaultGemQuality ?? 0);
    }
  }
  async function toggleGemOpts() {
    gemOptsOpen = !gemOptsOpen;
    if (gemOptsOpen) await loadGemOpts();
  }
  async function patchGemOpts(p: Parameters<typeof api.setGemOptions>[0]) {
    const r = await app.run(() => api.setGemOptions(p));
    if (r) {
      // the DPS field / default level and quality feed every cached compare
      tipCache.clear();
      supportableCache.clear();
      gemOpts = r;
      qualityDraft = String(r.defaultGemQuality ?? 0);
    }
  }
  const choiceLabel = (c: { label: string; labelZh?: string }) => c.labelZh || c.label;
  // what the number at the right of a search hit is (the gem-options sort field)
  const dpsHint = $derived.by(() => {
    const f = gemOpts?.sortFields.find((c) => c.value === gemOpts!.sortGemsByDPSField);
    return t("skills.dpsHint", { field: f ? choiceLabel(f) : "DPS" });
  });
  const supportableHint = $derived.by(() => {
    const f = gemOpts?.sortFields.find((c) => c.value === gemOpts!.sortGemsByDPSField);
    return t("skills.supportableHint", { field: f ? choiceLabel(f) : "DPS" });
  });

  // SkillListControl: right click = main group, Ctrl+right = Full DPS, Ctrl+click = enable
  async function rowContext(e: MouseEvent, g: SocketGroup) {
    e.preventDefault();
    const r = e.ctrlKey
      ? await app.run(() => api.setGroup(g.index, { includeInFullDPS: !g.includeInFullDPS }))
      : await app.run(() => api.setBuildField("mainSocketGroup", g.index));
    if (r) await changed();
  }
  async function rowClick(e: MouseEvent, g: SocketGroup) {
    if (e.ctrlKey) {
      const r = await app.run(() => api.setGroup(g.index, { enabled: !g.enabled }));
      if (r) await changed();
      return;
    }
    select(g.index);
  }

  async function reload() {
    const r = await app.run(() => api.listSkills());
    // The gem search reads "sort by DPS" from these options, so load them even while the
    // panel is collapsed (POB's default is on).
    if (!gemOpts) void loadGemOpts();
    if (r) {
      // the label being typed (not yet applied) survives a re-read (F2, a recalculation)
      const prevLabel = data?.groups.find((g) => g.index === sel)?.label ?? "";
      const typing = data !== null && labelDraft !== prevLabel;
      data = r;
      loadedRev = dataKey(r.rev, app.langRev);
      tipCache.clear();
      supportableCache.clear();
      if (!r.groups.some((g) => g.index === sel)) sel = r.groups.length ? Math.min(sel, r.groups.length) : 1;
      if (!typing) labelDraft = r.groups.find((g) => g.index === sel)?.label ?? "";
    }
  }
  $effect(() => {
    const rev = app.dataRev;
    if (!app.loaded) return;
    if (rev !== loadedRev) untrack(() => void reload());
  });
  async function changed() {
    await reload();
    await app.afterTreeChange();
  }
  function select(i: number) {
    sel = i;
    labelDraft = group?.label ?? data?.groups.find((g) => g.index === i)?.label ?? "";
    editGem = null;
    addOpen = false;
  }

  // --- tooltips ---------------------------------------------------------------
  function showTip(key: string, fetch: () => Promise<TipData>, e: MouseEvent, delay = 120, at?: { x: number; y: number }) {
    clearTimeout(tipTimer);
    const seq = ++tipSeq;
    const x = Math.round(at ? at.x : Math.min(e.clientX + 18, window.innerWidth - 360));
    const y = Math.round(at ? at.y : Math.min(e.clientY + 12, window.innerHeight - 360));
    const show = (d: TipData) => {
      if (seq === tipSeq) tip = { lines: d.lines, x, y };
    };
    const hit = tipCache.get(key);
    if (hit) {
      show(hit);
      return;
    }
    tipTimer = window.setTimeout(async () => {
      try {
        const r = await fetch();
        tipCache.set(key, { lines: r.lines });
        show(r);
      } catch {
        if (seq === tipSeq) tip = null;
      }
    }, delay);
  }
  const hideTip = () => {
    clearTimeout(tipTimer);
    tipSeq++;
    tip = null;
  };
  // a candidate in a gem drop-down: POB's picker hover, beside the list
  function showCandidateTip(e: MouseEvent, slot: number, h: GemHit) {
    if (!group || !app.has("gemCompareTooltip")) return;
    const g = group.index;
    const drop = (e.currentTarget as HTMLElement).closest(".drop")?.getBoundingClientRect();
    const row = (e.currentTarget as HTMLElement).getBoundingClientRect();
    // right of the list, or left of it when that would run off the window (never over it)
    const at = drop ? { x: drop.right + 376 <= window.innerWidth ? drop.right + 8 : Math.max(8, drop.left - 368), y: row.top } : undefined;
    showTip(`cand:${app.dataRev}:${g}:${slot}:${h.gemId}`, () => api.gemCandidateTooltip(g, slot, h.gemId), e, 150, at);
  }

  // --- groups -------------------------------------------------------------------
  async function addGroup() {
    const r = await app.run(() => api.addGroup({}));
    if (r) {
      sel = r.index;
      await changed();
      addOpen = true;
    }
  }
  async function deleteGroup() {
    if (!group) return;
    // SkillListControl:OnSelDelete asks first when the group has gems in it
    if (group.gems.length && !confirm(t("skills.deleteGroupConfirm", { name: pobPlain(group.displayLabelZh || group.displayLabel || group.label) }))) return;
    const r = await app.run(() => api.deleteGroup(group.index));
    if (r) await changed();
  }
  async function patchGroup(p: Parameters<typeof api.setGroup>[1]) {
    if (!group) return;
    const r = await app.run(() => api.setGroup(group.index, p));
    if (r) await changed();
  }
  async function moveGroup(d: -1 | 1) {
    if (!group || !data) return;
    const to = group.index + d;
    if (to < 1 || to > data.groups.length) return;
    const r = await app.run(() => api.moveGroup(group.index, to));
    if (r) {
      sel = to;
      await changed();
    }
  }
  async function setMain() {
    if (!group) return;
    const r = await app.run(() => api.setBuildField("mainSocketGroup", group.index));
    if (r) await changed();
  }

  // --- gems -------------------------------------------------------------------------
  // With "sort gems by DPS" on, POB's own picker does the search and the DPS
  // estimate per gem (it is slow, so only for the slot being edited).
  // The add row always asks for the DPS order (alwaysDps); replacing a gem
  // follows the Gem Options switch, as POB's own picker does.
  let searchBusy = $state(false);
  function search(q: string, into: (hits: GemHit[]) => void, slot?: number, alwaysDps = false) {
    clearTimeout(searchTimer);
    if (!q.trim()) {
      into([]);
      return;
    }
    searchTimer = window.setTimeout(async () => {
      const byDps = (alwaysDps ? app.has("gemDpsSort") : !!gemOpts?.sortGemsByDPS) && !!group && slot != null;
      searchBusy = byDps;
      try {
        const r = await api.gemSearch(byDps ? { query: q.trim(), limit: 20, group: group!.index, index: slot, byDps: true } : { query: q.trim(), limit: 20 });
        into(r.gems);
      } catch {
        into([]);
      }
      searchBusy = false;
    }, 120);
  }
  // Clicking into the empty add box: every support POB says can support this
  // group, sorted by how much DPS it adds (its own picker's estimate). Kept per
  // build revision and group, so focusing again does not recalculate.
  async function openSupportable() {
    if (!group) return;
    addOpen = true;
    if (addQuery.trim()) return;
    clearTimeout(searchTimer);
    addMode = "supportable";
    const g = group.index;
    const slot = group.gems.length + 1;
    const key = `${app.dataRev}:${g}`;
    const cached = supportableCache.get(key);
    if (cached) {
      addHits = cached;
      return;
    }
    const seq = ++addSeq;
    addHits = [];
    addBusy = true;
    let hits: GemHit[] = [];
    let byDps = false;
    try {
      const r = await api.gemSearch({ query: "", limit: 60, group: g, index: slot, byDps: true, supportableOnly: true });
      hits = r.gems;
      byDps = !!r.byDps;
    } catch {
      // PoE2's POB (no DPS sort) or an older bridge: the plain support list
      try {
        hits = (await api.gemSearch({ query: "", limit: 60, supportOnly: true })).gems;
      } catch {
        hits = [];
      }
    }
    if (seq !== addSeq) return;
    addBusy = false;
    if (byDps) supportableCache.set(key, hits);
    if (addMode === "supportable" && !addQuery.trim()) addHits = hits;
  }
  function addInput() {
    addOpen = true;
    if (!addQuery.trim()) {
      void openSupportable();
      return;
    }
    addSeq++;
    addBusy = false;
    addMode = "search";
    search(addQuery, (h) => (addHits = h), (group?.gems.length ?? 0) + 1, true);
  }
  function closeAdd() {
    addOpen = false;
    hideTip();
  }
  const dpsColor = (h: GemHit) => (h.dpsColor && h.dpsColor.startsWith("^x") ? `#${h.dpsColor.slice(2)}` : undefined);
  const dpsText = (h: GemHit) => (typeof h.dps === "number" ? Math.round(h.dps).toLocaleString() : "");
  async function addGem(hit: GemHit) {
    if (!group) return;
    addQuery = "";
    addHits = [];
    closeAdd();
    const r = await app.run(() => api.addGem(group.index, { gemId: hit.gemId }));
    if (r) await changed();
  }
  async function addGemByText() {
    if (!group || !addQuery.trim()) return;
    if (addHits.length) return addGem(addHits[0]);
    const q = addQuery.trim();
    addQuery = "";
    closeAdd();
    const r = await app.run(() => api.addGem(group.index, { nameSpec: q }));
    if (r) await changed();
  }
  async function patchGem(g: GemInstance, p: Parameters<typeof api.setGem>[2]) {
    if (!group) return;
    const r = await app.run(() => api.setGem(group.index, g.index, p));
    if (r) await changed();
  }
  async function pickGem(g: GemInstance, hit: GemHit) {
    editGem = null;
    hideTip();
    await patchGem(g, { gemId: hit.gemId });
  }
  async function deleteGem(g: GemInstance) {
    if (!group) return;
    const r = await app.run(() => api.deleteGem(group.index, g.index));
    if (r) await changed();
  }
  async function moveGem(g: GemInstance, d: -1 | 1) {
    if (!group) return;
    const to = g.index + d;
    if (to < 1 || to > group.gems.length) return;
    const r = await app.run(() => api.moveGem(group.index, g.index, to));
    if (r) await changed();
  }
  function gemColor(g: { color?: string | number }): string {
    const c = g.color;
    if (typeof c === "number") return c === 1 ? "var(--c-life)" : c === 2 ? "var(--ok)" : c === 3 ? "var(--c-mana)" : "var(--ink-0)";
    const m = typeof c === "string" ? /^\^x([0-9a-fA-F]{6})/.exec(c) : null;
    return m ? `#${m[1]}` : "var(--ink-0)";
  }

  // --- skill sets -------------------------------------------------------------------
  async function setChange(v: string) {
    const r = await app.run(() => api.setSkillSet(Number(v)));
    if (r) await changed();
  }
  async function setDialogOk() {
    if (!setDialog || !data || !setDialog.title.trim()) return;
    const d = setDialog;
    const active = data.activeSkillSetId;
    setDialog = null;
    const r = d.mode === "new" ? await app.run(() => api.newSkillSet(d.title.trim(), d.copy)) : await app.run(() => api.renameSkillSet(active, d.title.trim()));
    if (r) await changed();
  }
  async function deleteSet() {
    if (!data || data.skillSets.length <= 1) return;
    const id = data.activeSkillSetId;
    const cur = data.skillSets.find((s) => s.id === id);
    if (!confirm(t("skills.deleteSetConfirm", { name: cur ? setTitle(cur) : "" }))) return;
    const r = await app.run(() => api.deleteSkillSet(id));
    if (r) await changed();
  }
  const setTitle = (s: { id: number; title?: string }) => s.title || t("items.defaultSet");
