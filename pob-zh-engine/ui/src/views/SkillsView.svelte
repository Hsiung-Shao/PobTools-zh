<!-- 技能頁:左 = 插槽組清單(啟用、標籤、槽位、主技能標記),右 = 選中組的寶石表
     (名稱搜尋、等級/品質/啟用/全域/數量)。每個改動都經 POB 的 ProcessSocketGroup。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import { api, type GemHit, type GemInstance, type GemOptions, type GroupExtras, type SkillsList, type SocketGroup, type TooltipLine } from "$lib/bridge";
  import { copyText } from "$lib/clipboard";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import TooltipCard from "../components/TooltipCard.svelte";

  let data = $state<SkillsList | null>(null);
  let sel = $state<number>(1);
  let loadedRev = -1;

  let tip = $state<{ lines: TooltipLine[]; x: number; y: number } | null>(null);
  let tipTimer = 0;
  const tipCache = new Map<string, TooltipLine[]>();

  // gem search box (add row) and inline rename of an existing gem
  let addQuery = $state("");
  let addHits = $state<GemHit[]>([]);
  let addOpen = $state(false);
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
      extras = x;
      countDraft = String(x.count);
    } catch {
      extras = null;
    }
  }
  $effect(() => {
    const i = sel;
    const rev = app.rev;
    void rev;
    if (!data || !data.groups.some((g) => g.index === i)) return;
    untrack(() => void loadExtras(i));
  });
  async function applyCount() {
    if (!group || !extras || String(extras.count) === countDraft) return;
    const n = Math.max(1, Math.floor(Number(countDraft) || 1));
    const r = await app.run(() => api.setGroupCount(group.index, n));
    if (r) await changed();
  }
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
  async function toggleGemOpts() {
    gemOptsOpen = !gemOptsOpen;
    if (gemOptsOpen) {
      const r = await app.run(() => api.getGemOptions());
      if (r) {
        gemOpts = r;
        qualityDraft = String(r.defaultGemQuality ?? 0);
      }
    }
  }
  async function patchGemOpts(p: Parameters<typeof api.setGemOptions>[0]) {
    const r = await app.run(() => api.setGemOptions(p));
    if (r) {
      gemOpts = r;
      qualityDraft = String(r.defaultGemQuality ?? 0);
    }
  }
  const choiceLabel = (c: { label: string; labelZh?: string }) => c.labelZh || c.label;

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
    if (r) {
      data = r;
      loadedRev = r.rev;
      tipCache.clear();
      if (!r.groups.some((g) => g.index === sel)) sel = r.groups.length ? Math.min(sel, r.groups.length) : 1;
      labelDraft = r.groups.find((g) => g.index === sel)?.label ?? "";
    }
  }
  $effect(() => {
    const rev = app.rev;
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
  function showTip(key: string, fetch: () => Promise<{ lines: TooltipLine[] }>, e: MouseEvent) {
    clearTimeout(tipTimer);
    const x = Math.round(Math.min(e.clientX + 18, window.innerWidth - 360));
    const y = Math.round(Math.min(e.clientY + 12, window.innerHeight - 360));
    const hit = tipCache.get(key);
    if (hit) {
      tip = { lines: hit, x, y };
      return;
    }
    tipTimer = window.setTimeout(async () => {
      try {
        const r = await fetch();
        tipCache.set(key, r.lines);
        tip = { lines: r.lines, x, y };
      } catch {
        tip = null;
      }
    }, 120);
  }
  const hideTip = () => {
    clearTimeout(tipTimer);
    tip = null;
  };

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
  let searchBusy = $state(false);
  function search(q: string, into: (hits: GemHit[]) => void, slot?: number) {
    clearTimeout(searchTimer);
    if (!q.trim()) {
      into([]);
      return;
    }
    searchTimer = window.setTimeout(async () => {
      const byDps = !!gemOpts?.sortGemsByDPS && !!group && slot != null;
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
  const dpsColor = (h: GemHit) => (h.dpsColor && h.dpsColor.startsWith("^x") ? `#${h.dpsColor.slice(2)}` : undefined);
  const dpsText = (h: GemHit) => (typeof h.dps === "number" ? Math.round(h.dps).toLocaleString() : "");
  async function addGem(hit: GemHit) {
    if (!group) return;
    addQuery = "";
    addHits = [];
    addOpen = false;
    const r = await app.run(() => api.addGem(group.index, { gemId: hit.gemId }));
    if (r) await changed();
  }
  async function addGemByText() {
    if (!group || !addQuery.trim()) return;
    if (addHits.length) return addGem(addHits[0]);
    const q = addQuery.trim();
    addQuery = "";
    addOpen = false;
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
</script>

<!-- svelte-ignore a11y_no_static_element_interactions -->
<div class="page" onmouseleave={hideTip}>
  <section class="col left">
    <div class="head">
      <span class="label">{t("skills.groups")}</span>
      {#if data}
        <select class="select sm" value={String(data.activeSkillSetId)} onchange={(e) => setChange(e.currentTarget.value)} title={t("skills.skillSet")}>
          {#each data.skillSets as s}
            <option value={String(s.id)}>{setTitle(s)}</option>
          {/each}
        </select>
        <button class="btn ghost sm" onclick={() => (setDialog = { mode: "new", title: t("skills.newSetName"), copy: false })}>+</button>
        <button class="btn ghost sm" onclick={() => (setDialog = { mode: "rename", title: data!.skillSets.find((s) => s.id === data!.activeSkillSetId)?.title ?? "", copy: false })}>✎</button>
        <button class="btn ghost sm" disabled={data.skillSets.length <= 1} onclick={deleteSet}>×</button>
      {/if}
    </div>
    <div class="scroll">
      {#if data}
        {#each data.groups as g (g.index)}
          <!-- svelte-ignore a11y_click_events_have_key_events -->
          <div
            class="grow"
            class:sel={g.index === sel}
            class:off={!g.enabled || !g.slotEnabled}
            role="option"
            aria-selected={g.index === sel}
            tabindex="0"
            onclick={(e) => rowClick(e, g)}
            oncontextmenu={(e) => rowContext(e, g)}
            onmouseenter={(e) => showTip(`g${g.index}`, () => api.groupTooltip(g.index), e)}
            onmouseleave={hideTip}
          >
            <input type="checkbox" checked={g.enabled} onclick={(e) => e.stopPropagation()} onchange={(e) => app.run(() => api.setGroup(g.index, { enabled: e.currentTarget.checked })).then(changed)} />
            <span class="gname">{g.displayLabelZh || g.displayLabel || g.label || "—"}</span>
            {#if g.isMain}<span class="badge gold">{t("skills.main")}</span>{/if}
            {#if g.slot}<span class="badge">{data.slotOptions.find((s) => s.name === g.slot)?.labelZh ?? g.slot}</span>{/if}
            {#if g.source}<span class="badge dim">{g.sourceName ?? t("skills.fromItem")}</span>{/if}
          </div>
        {/each}
        {#if !data.groups.length}<p class="dim pad">{t("skills.noGroups")}</p>{/if}
      {/if}
    </div>
    <div class="actions">
      <button class="btn sm" onclick={addGroup}>{t("skills.newGroup")}</button>
      <button class="btn ghost sm" disabled={!group || group.index <= 1} onclick={() => moveGroup(-1)}>↑</button>
      <button class="btn ghost sm" disabled={!group || !data || group.index >= data.groups.length} onclick={() => moveGroup(1)}>↓</button>
      <button class="btn ghost sm" disabled={!group} title={t("skills.copyGroupHint")} onclick={copyGroup}>{t("skills.copyGroup")}</button>
      <button class="btn ghost sm" onclick={openPaste}>{t("skills.pasteGroup")}</button>
      <span class="grow"></span>
      <button class="btn sm danger" disabled={!group || group.source} onclick={deleteGroup}>{t("skills.deleteGroup")}</button>
    </div>
    <div class="gemopts">
      <button class="btn ghost sm" onclick={toggleGemOpts}>{gemOptsOpen ? "▾" : "▸"} {t("skills.gemOptions")}</button>
      {#if gemOptsOpen && gemOpts}
        <div class="optgrid">
          <label class="chk"><input type="checkbox" checked={gemOpts.sortGemsByDPS} onchange={(e) => patchGemOpts({ sortGemsByDPS: e.currentTarget.checked })} /> {t("skills.sortByDps")}</label>
          <select class="select sm" value={gemOpts.sortGemsByDPSField} disabled={!gemOpts.sortGemsByDPS} onchange={(e) => patchGemOpts({ sortGemsByDPSField: e.currentTarget.value })}>
            {#each gemOpts.sortFields as c}<option value={c.value}>{choiceLabel(c)}</option>{/each}
          </select>
          <span class="k">{t("skills.defaultLevel")}</span>
          <select class="select sm" value={gemOpts.defaultGemLevel} onchange={(e) => patchGemOpts({ defaultGemLevel: e.currentTarget.value })}
            title={gemOpts.defaultGemLevels.find((c) => c.value === gemOpts!.defaultGemLevel)?.description ?? ""}>
            {#each gemOpts.defaultGemLevels as c}<option value={c.value} title={c.description ?? ""}>{choiceLabel(c)}</option>{/each}
          </select>
          <span class="k">{t("skills.defaultQuality")}</span>
          <input class="input sm num" type="number" min="0" max="23" bind:value={qualityDraft}
            onchange={() => patchGemOpts({ defaultGemQuality: Math.max(0, Math.min(23, Math.floor(Number(qualityDraft) || 0))) })} />
          <span class="k">{t("skills.showSupports")}</span>
          <select class="select sm" value={gemOpts.showSupportGemTypes} onchange={(e) => patchGemOpts({ showSupportGemTypes: e.currentTarget.value })}>
            {#each gemOpts.supportGemTypes as c}<option value={c.value}>{choiceLabel(c)}</option>{/each}
          </select>
          <label class="chk"><input type="checkbox" checked={gemOpts.showLegacyGems} onchange={(e) => patchGemOpts({ showLegacyGems: e.currentTarget.checked })} /> {t("skills.showLegacy")}</label>
        </div>
      {/if}
    </div>
  </section>

  <section class="col right">
    {#if group && data}
      <div class="detail">
        <div class="frow">
          <label class="f">
            <span class="k">{t("skills.label")}</span>
            <input class="input sm" placeholder={t("skills.labelPlaceholder")} bind:value={labelDraft} onblur={() => labelDraft !== group!.label && patchGroup({ label: labelDraft })} onkeydown={(e) => e.key === "Enter" && (e.currentTarget as HTMLInputElement).blur()} />
          </label>
          <label class="f">
            <span class="k">{t("skills.slot")}</span>
            <select class="select sm" value={group.slot ?? ""} onchange={(e) => patchGroup({ slot: e.currentTarget.value })}>
              <option value="">{t("skills.slotNone")}</option>
              {#each data.slotOptions as s}
                <option value={s.name}>{s.labelZh || s.label}</option>
              {/each}
            </select>
          </label>
          <label class="chk"><input type="checkbox" checked={group.enabled} onchange={(e) => patchGroup({ enabled: e.currentTarget.checked })} /> {t("skills.enabled")}</label>
          <label class="chk"><input type="checkbox" checked={group.includeInFullDPS} onchange={(e) => patchGroup({ includeInFullDPS: e.currentTarget.checked })} /> {t("skills.fullDps")}</label>
          {#if group.isMain}
            <span class="badge gold">{t("skills.main")}</span>
          {:else}
            <button class="btn sm" disabled={!group.skills.length} onclick={setMain}>{t("skills.setMain")}</button>
          {/if}
        </div>
        {#if group.skills.length > 1}
          <div class="frow">
            <label class="f">
              <span class="k">{t("skills.mainSkill")}</span>
              <select class="select sm" value={group.mainActiveSkill} onchange={(e) => patchGroup({ mainActiveSkill: Number(e.currentTarget.value) })}>
                {#each group.skills as s}
                  <option value={s.index}>{s.nameZh || s.name}</option>
                {/each}
              </select>
            </label>
          </div>
        {/if}
        {#if extras && (extras.countShown || extras.imbued?.shown || extras.optimiseSockets?.shown)}
          <div class="frow">
            {#if extras.countShown}
              <label class="f">
                <span class="k">{t("skills.groupCount")}</span>
                <input class="input sm num cnt" type="number" min="1" bind:value={countDraft} onblur={applyCount} onkeydown={(e) => e.key === "Enter" && (e.currentTarget as HTMLInputElement).blur()} />
              </label>
            {/if}
            {#if extras.optimiseSockets?.shown}
              <button class="btn sm" disabled={!extras.optimiseSockets.enabled} title={t("skills.optimiseHint")} onclick={optimise}>{t("skills.optimise")}</button>
            {/if}
            {#if extras.imbued?.shown}
              <span class="f">
                <span class="k">{t("skills.imbued")}</span>
                <span class="combo imb">
                  <input class="input sm" disabled={!extras.imbued.enabled} placeholder={extras.imbued.nameZh || extras.imbued.name || t("skills.imbuedNone")}
                    bind:value={imbuedQuery}
                    oninput={() => { imbuedOpen = true; searchImbued(imbuedQuery); }}
                    onfocus={() => (imbuedOpen = true)}
                    onblur={() => setTimeout(() => (imbuedOpen = false), 150)} />
                  {#if imbuedOpen && imbuedHits.length}
                    <div class="drop">
                      {#each imbuedHits as h}
                        <button class="hit" onmousedown={(e) => { e.preventDefault(); void setImbued(h.gemId); }}>
                          <span style:color={gemColor(h)}>{h.nameZh || h.name}</span>
                          <span class="dim small">{h.name}</span>
                        </button>
                      {/each}
                    </div>
                  {/if}
                </span>
                {#if extras.imbued.name}
                  <span class="imbname">{extras.imbued.nameZh || extras.imbued.name}</span>
                  <button class="btn ghost sm" disabled={!extras.imbued.enabled} title={t("skills.imbuedClear")} onclick={() => setImbued()}>×</button>
                {/if}
              </span>
            {/if}
          </div>
        {/if}
        {#if group.enabled && !group.slotEnabled}<div class="warn small">{t("skills.disabledSet")}</div>{/if}

        <table class="gems">
          <thead>
            <tr>
              <th class="w"></th>
              <th>{t("skills.gemName")}</th>
              <th class="n">{t("skills.level")}</th>
              <th class="n">{t("skills.quality")}</th>
              <th class="c" title={t("skills.enabled")}>✓</th>
              <th class="c">{t("skills.global1")}</th>
              <th class="c">{t("skills.global2")}</th>
              <th class="n">{t("skills.count")}</th>
              <th class="w2"></th>
            </tr>
          </thead>
          <tbody>
            {#each group.gems as g (g.index)}
              <tr class:off={!g.enabled}>
                <td class="w"><span class="dot" style:background={gemColor(g)}></span></td>
                <td class="name">
                  {#if editGem && editGem.index === g.index}
                    <div class="combo">
                      <!-- svelte-ignore a11y_autofocus -->
                      <input class="input sm" autofocus bind:value={editGem.query} oninput={() => search(editGem!.query, (h) => (editGem!.hits = h))} onkeydown={(e) => { if (e.key === "Escape") editGem = null; if (e.key === "Enter" && editGem?.hits[0]) void pickGem(g, editGem.hits[0]); }} onblur={() => setTimeout(() => (editGem = null), 150)} />
                      {#if editGem.hits.length}
                        <div class="drop">
                          {#each editGem.hits as h}
                            <button class="hit" onmousedown={(e) => { e.preventDefault(); void pickGem(g, h); }}>
                              <span style:color={gemColor(h)}>{h.nameZh || h.name}</span>
                              {#if h.support}<span class="dim small">{t("skills.support")}</span>{/if}
                            </button>
                          {/each}
                        </div>
                      {/if}
                    </div>
                  {:else}
                    <!-- svelte-ignore a11y_click_events_have_key_events -->
                    <span
                      class="gemname"
                      class:bad={!!g.errMsg}
                      style:color={g.errMsg ? "var(--bad)" : gemColor(g)}
                      role="button"
                      tabindex="0"
                      title={g.errMsg ?? ""}
                      onclick={() => !g.fromItem && !g.fromNode && (editGem = { index: g.index, query: g.name ?? g.nameSpec, hits: [] })}
                      onmouseenter={(e) => showTip(`gem${group!.index}:${g.index}:${app.rev}`, () => api.gemTooltip(group!.index, g.index), e)}
                      onmouseleave={hideTip}
                    >
                      {g.nameZh || g.name || g.nameSpec}
                      {#if g.support}<span class="dim small"> · {t("skills.support")}</span>{/if}
                      {#if g.fromItem}<span class="dim small"> · {t("skills.fromItem")}</span>{/if}
                      {#if g.fromNode}<span class="dim small"> · {t("skills.fromNode")}</span>{/if}
                      {#if group.slot && !g.matchesSocket}<span class="warn small" title={t("skills.socketMismatch")}> ◇</span>{/if}
                    </span>
                  {/if}
                </td>
                <td class="n"><input class="input sm num" type="number" min="1" max="40" value={g.level} onchange={(e) => patchGem(g, { level: Number(e.currentTarget.value) })} /></td>
                <td class="n"><input class="input sm num" type="number" min="0" max="23" value={g.quality} onchange={(e) => patchGem(g, { quality: Number(e.currentTarget.value) })} /></td>
                <td class="c"><input type="checkbox" checked={g.enabled} onchange={(e) => patchGem(g, { enabled: e.currentTarget.checked })} /></td>
                <td class="c">{#if g.hasGlobalEffect}<input type="checkbox" checked={g.enableGlobal1} onchange={(e) => patchGem(g, { enableGlobal1: e.currentTarget.checked })} />{/if}</td>
                <td class="c">{#if g.hasGlobalEffect}<input type="checkbox" checked={g.enableGlobal2} onchange={(e) => patchGem(g, { enableGlobal2: e.currentTarget.checked })} />{/if}</td>
                <td class="n"><input class="input sm num" type="number" min="1" value={g.count ?? 1} onchange={(e) => patchGem(g, { count: Number(e.currentTarget.value) })} /></td>
                <td class="w2">
                  <button class="btn ghost sm" disabled={g.index <= 1} onclick={() => moveGem(g, -1)}>↑</button>
                  <button class="btn ghost sm" disabled={g.index >= group!.gems.length} onclick={() => moveGem(g, 1)}>↓</button>
                  <button class="btn ghost sm" disabled={g.fromItem || g.fromNode} title={t("skills.deleteGem")} onclick={() => deleteGem(g)}>×</button>
                </td>
              </tr>
            {/each}
            <tr class="addrow">
              <td class="w"></td>
              <td class="name" colspan="7">
                <div class="combo">
                  <input
                    class="input sm"
                    placeholder={t("skills.searchGem")}
                    bind:value={addQuery}
                    onfocus={() => (addOpen = true)}
                    oninput={() => { addOpen = true; search(addQuery, (h) => (addHits = h), (group?.gems.length ?? 0) + 1); }}
                    onkeydown={(e) => { if (e.key === "Enter") void addGemByText(); if (e.key === "Escape") addOpen = false; }}
                    onblur={() => setTimeout(() => (addOpen = false), 150)}
                  />
                  {#if addOpen && addHits.length}
                    <div class="drop">
                      {#each addHits as h}
                        <button class="hit" onmousedown={(e) => { e.preventDefault(); void addGem(h); }}>
                          <span style:color={gemColor(h)}>{h.nameZh || h.name}</span>
                          <span class="dim small">{h.name}{h.support ? ` · ${t("skills.support")}` : ""}</span>
                          {#if h.dps != null}<span class="small num" style:color={dpsColor(h)}>{dpsText(h)}</span>{/if}
                        </button>
                      {/each}
                    </div>
                  {/if}
                </div>
              </td>
              <td class="w2"></td>
            </tr>
          </tbody>
        </table>
      </div>
    {:else}
      <div class="center dim">{t("skills.noGroups")}</div>
    {/if}
  </section>

  {#if tip}<TooltipCard lines={tip.lines} x={tip.x} y={tip.y} width={360} />{/if}

  {#if pasteDialog}
    <div class="modal">
      <div class="dialog">
        <div class="label">{t("skills.pasteGroup")}</div>
        <p class="dim small">{t("skills.pasteHint")}</p>
        <textarea class="input pastebox" bind:value={pasteDialog.text}></textarea>
        <div class="btns"><button class="btn ghost" onclick={() => (pasteDialog = null)}>{t("tree.cancel")}</button><button class="btn primary" disabled={!pasteDialog.text.trim()} onclick={pasteOk}>OK</button></div>
      </div>
    </div>
  {/if}

  {#if setDialog}
    <div class="modal">
      <div class="dialog">
        <div class="label">{setDialog.mode === "new" ? t("skills.skillSet") : t("skills.skillSet")}</div>
        <input class="input" placeholder={t("items.setName")} bind:value={setDialog.title} onkeydown={(e) => e.key === "Enter" && setDialogOk()} />
        {#if setDialog.mode === "new"}<label class="chk"><input type="checkbox" bind:checked={setDialog.copy} /> {t("items.newSetCopy")}</label>{/if}
        <div class="btns"><button class="btn ghost" onclick={() => (setDialog = null)}>{t("tree.cancel")}</button><button class="btn primary" disabled={!setDialog.title.trim()} onclick={setDialogOk}>OK</button></div>
      </div>
    </div>
  {/if}
</div>

<style>
  .page {
    height: 100%;
    display: grid;
    grid-template-columns: 360px 1fr;
    min-height: 0;
  }
  /* column rules as borders, not a solid fill behind a 1px gap: the fill hid the
     background image, and a second surface here stacked on the one .view paints */
  .col {
    display: flex;
    flex-direction: column;
    min-height: 0;
  }
  .col + .col {
    border-left: 1px solid var(--edge-0);
  }
  /* wraps onto a second row in a narrow column instead of clipping its right
     end (the set drop-down, its buttons and the tree selector did) */
  .head {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 4px 6px;
    min-height: 36px;
    padding: 4px 12px;
    border-bottom: 1px solid var(--edge-0);
    background: var(--surface-1);
  }
  .head .label {
    white-space: nowrap;
  }
  .head .select {
    min-width: 0;
    max-width: 100%;
  }
  .scroll {
    flex: 1;
    overflow-y: auto;
    padding: 6px 8px;
  }
  .pad {
    padding: 8px;
    font-size: var(--fs-xs);
  }
  .grow {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 6px 8px;
    border-radius: var(--radius-s);
    font-size: var(--fs-sm);
    cursor: default;
  }
  .grow:hover {
    background: var(--surface-hover);
  }
  .grow.sel {
    background: var(--gold-soft);
    box-shadow: inset 2px 0 0 var(--gold);
  }
  .grow.off .gname {
    color: var(--ink-3);
  }
  .gname {
    flex: 1;
    min-width: 0;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .badge {
    font-size: var(--fs-2xs);
    padding: 1px 6px;
    border: 1px solid var(--edge-1);
    border-radius: 9px;
    color: var(--ink-2);
    white-space: nowrap;
  }
  .badge.gold {
    color: var(--gold);
    border-color: var(--gold-dim);
  }
  .actions {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
    padding: 8px 10px;
    border-top: 1px solid var(--edge-0);
    background: var(--surface-1);
  }
  .actions .grow {
    flex: 1;
    padding: 0;
  }
  .btn.danger:hover:not(:disabled) {
    color: var(--bad);
    border-color: var(--bad);
  }
  .detail {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    padding: 12px 14px;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .frow {
    display: flex;
    align-items: flex-end;
    gap: 12px;
    flex-wrap: wrap;
  }
  .f {
    display: flex;
    flex-direction: column;
    gap: 3px;
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
  }
  .chk {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    font-size: var(--fs-xs);
    height: 24px;
  }
  .select.sm,
  .input.sm {
    height: 24px;
    font-size: var(--fs-xs);
  }
  .input.num {
    width: 56px;
    text-align: right;
  }
  .gems {
    width: 100%;
    border-collapse: collapse;
    font-size: var(--fs-sm);
  }
  .gems th {
    text-align: left;
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    font-weight: 600;
    color: var(--ink-3);
    padding: 4px 6px;
    border-bottom: 1px solid var(--edge-1);
  }
  .gems td {
    padding: 3px 6px;
    border-bottom: 1px solid var(--edge-0);
    vertical-align: middle;
  }
  .gems tr.off td {
    opacity: 0.55;
  }
  .gems .w {
    width: 14px;
  }
  .gems .w2 {
    width: 90px;
    white-space: nowrap;
    text-align: right;
  }
  .gems .n {
    width: 64px;
    text-align: right;
  }
  .gems .c {
    width: 52px;
    text-align: center;
  }
  .dot {
    display: inline-block;
    width: 8px;
    height: 8px;
    border-radius: 50%;
  }
  .gemname {
    cursor: text;
  }
  .combo {
    position: relative;
  }
  .combo .input {
    width: 100%;
  }
  .drop {
    position: absolute;
    left: 0;
    top: 26px;
    z-index: 25;
    min-width: 280px;
    max-height: 260px;
    overflow-y: auto;
    background: var(--surface-2);
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-s);
    box-shadow: var(--shadow-float);
    display: flex;
    flex-direction: column;
  }
  .hit {
    appearance: none;
    border: 0;
    background: transparent;
    color: var(--ink-0);
    text-align: left;
    padding: 5px 9px;
    display: flex;
    justify-content: space-between;
    gap: 10px;
    font-size: var(--fs-xs);
  }
  .hit:hover {
    background: var(--surface-hover);
  }
  .small {
    font-size: var(--fs-2xs);
  }
  .warn {
    color: var(--warn);
  }
  .bad {
    color: var(--bad);
  }
  .center {
    height: 100%;
    display: grid;
    place-items: center;
  }
  .modal {
    position: fixed;
    inset: 0;
    display: grid;
    place-items: center;
    background: var(--backdrop);
    z-index: 30;
  }
  .dialog {
    width: 380px;
    padding: 16px 18px;
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-left: 3px solid var(--gold);
    border-radius: var(--radius-m);
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .btns {
    display: flex;
    justify-content: flex-end;
    gap: 6px;
  }
  .gemopts {
    border-top: 1px solid var(--edge-0);
    padding: 6px 10px;
  }
  .optgrid {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: 6px 10px;
    align-items: center;
    padding: 6px 2px;
  }
  .cnt {
    width: 70px;
  }
  .imb {
    position: relative;
    width: 220px;
  }
  .imbname {
    color: var(--ink-0);
  }
  .pastebox {
    width: 100%;
    min-height: 140px;
    font-family: var(--font-mono, monospace);
  }
</style>
