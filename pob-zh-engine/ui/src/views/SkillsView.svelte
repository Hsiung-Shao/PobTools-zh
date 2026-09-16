<!-- 技能頁:左 = 插槽組清單(啟用、標籤、槽位、主技能標記),右 = 選中組的寶石表
     (名稱搜尋、等級/品質/啟用/全域/數量)。每個改動都經 POB 的 ProcessSocketGroup。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import { api, type GemHit, type GemInstance, type SkillsList, type SocketGroup, type TooltipLine } from "$lib/bridge";
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
    const x = Math.min(e.clientX + 18, window.innerWidth - 360);
    const y = Math.min(e.clientY + 12, window.innerHeight - 360);
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
  function search(q: string, into: (hits: GemHit[]) => void) {
    clearTimeout(searchTimer);
    if (!q.trim()) {
      into([]);
      return;
    }
    searchTimer = window.setTimeout(async () => {
      try {
        const r = await api.gemSearch({ query: q.trim(), limit: 20 });
        into(r.gems);
      } catch {
        into([]);
      }
    }, 120);
  }
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
    if (!setDialog || !data) return;
    const d = setDialog;
    const active = data.activeSkillSetId;
    setDialog = null;
    const r = d.mode === "new" ? await app.run(() => api.newSkillSet(d.title.trim(), d.copy)) : await app.run(() => api.renameSkillSet(active, d.title.trim()));
    if (r) await changed();
  }
  async function deleteSet() {
    if (!data || data.skillSets.length <= 1 || !confirm(t("items.deleteSet") + "?")) return;
    const id = data.activeSkillSetId;
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
        <button class="btn ghost sm" onclick={() => (setDialog = { mode: "new", title: "", copy: false })}>+</button>
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
            onclick={() => select(g.index)}
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
      <span class="grow"></span>
      <button class="btn sm danger" disabled={!group || group.source} onclick={deleteGroup}>{t("skills.deleteGroup")}</button>
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
                    oninput={() => { addOpen = true; search(addQuery, (h) => (addHits = h)); }}
                    onkeydown={(e) => { if (e.key === "Enter") void addGemByText(); if (e.key === "Escape") addOpen = false; }}
                    onblur={() => setTimeout(() => (addOpen = false), 150)}
                  />
                  {#if addOpen && addHits.length}
                    <div class="drop">
                      {#each addHits as h}
                        <button class="hit" onmousedown={(e) => { e.preventDefault(); void addGem(h); }}>
                          <span style:color={gemColor(h)}>{h.nameZh || h.name}</span>
                          <span class="dim small">{h.name}{h.support ? ` · ${t("skills.support")}` : ""}</span>
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

  {#if setDialog}
    <div class="modal">
      <div class="dialog">
        <div class="label">{setDialog.mode === "new" ? t("skills.skillSet") : t("skills.skillSet")}</div>
        <input class="input" placeholder={t("items.setName")} bind:value={setDialog.title} onkeydown={(e) => e.key === "Enter" && setDialogOk()} />
        {#if setDialog.mode === "new"}<label class="chk"><input type="checkbox" bind:checked={setDialog.copy} /> {t("items.newSetCopy")}</label>{/if}
        <div class="btns"><button class="btn ghost" onclick={() => (setDialog = null)}>{t("tree.cancel")}</button><button class="btn primary" onclick={setDialogOk}>OK</button></div>
      </div>
    </div>
  {/if}
</div>

<style>
  .page {
    height: 100%;
    display: grid;
    grid-template-columns: 360px 1fr;
    gap: 1px;
    background: var(--edge-0);
    min-height: 0;
  }
  .col {
    display: flex;
    flex-direction: column;
    min-height: 0;
    background: var(--surface-0);
  }
  .head {
    display: flex;
    align-items: center;
    gap: 6px;
    height: 36px;
    padding: 0 12px;
    border-bottom: 1px solid var(--edge-0);
    background: var(--surface-1);
  }
  .head .select {
    max-width: 150px;
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
</style>
