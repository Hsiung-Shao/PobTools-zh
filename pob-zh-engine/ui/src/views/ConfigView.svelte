<!-- 配置頁:照 POB 原版 ConfigTab —— 區段寬 360、欄距 370;每個區段先放進 ConfigOptions
     指定的欄(col),那一欄放不下(超出可視高度)才改放最矮的欄(ConfigTab:Draw 的規則);
     每列標籤靠右、控制項對齊在同一欄(POB 的控制項固定從 x=234 起);POB 判定不相關的選項
     預設隱藏;tooltip 走我們的浮層;自訂詞綴區塊是 POB 的 col 1 最後一段。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import { api, type ConfigItem, type ConfigList, type ConfigSection, type CustomModBlock, type TooltipLine } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import PobText from "../components/PobText.svelte";
  import TooltipCard from "../components/TooltipCard.svelte";

  let data = $state<ConfigList | null>(null);
  // tooltip (POB's per-control tooltipText), on hover after a short delay
  let tip = $state<{ lines: TooltipLine[]; x: number; y: number } | null>(null);
  let tipTimer = 0;
  function tipEnter(it: ConfigItem, e: MouseEvent) {
    clearTimeout(tipTimer);
    const text = it.tooltipZh || it.tooltip;
    if (!text) return;
    const el = e.currentTarget as HTMLElement;
    tipTimer = window.setTimeout(() => {
      const r = el.getBoundingClientRect();
      const lines: TooltipLine[] = text.split("\n").map((s) => ({ size: 14, text: s, raw: s, center: false }));
      const x = Math.round(Math.min(r.left + 24, window.innerWidth - 380));
      const y = Math.round(r.bottom + 4 + 260 > window.innerHeight ? Math.max(8, r.top - 8 - Math.min(260, 20 * lines.length + 24)) : r.bottom + 4);
      tip = { lines, x, y };
    }, 220);
  }
  function tipLeave() {
    clearTimeout(tipTimer);
    tip = null;
  }
  const selectedLabel = (it: ConfigItem) => (it.list ?? []).find((o) => String(o.val ?? "") === String(it.value ?? ""));
  let loadedRev = -1;
  let filter = $state("");
  let showAll = $state(false);
  let custom = $state<CustomModBlock[]>([]);
  let customDirty = $state(false);
  // POB's Mod Browser for a custom-modifier group (Modules/ConfigModBrowser)
  let modBrowser = $state<{ block: number; query: string; busy: boolean; total: number; mods: { text: string; textZh?: string; sources: string[] }[] } | null>(null);
  let modTimer = 0;
  async function openModBrowser(block: number) {
    if (customDirty) await applyCustom();
    modBrowser = { block, query: "", busy: true, total: 0, mods: [] };
    await searchMods();
  }
  async function searchMods() {
    if (!modBrowser) return;
    const b = modBrowser;
    b.busy = true;
    try {
      const r = await api.configModSearch(b.block, b.query, 200);
      if (modBrowser !== b) return;
      b.mods = r.mods;
      b.total = r.total;
    } catch (e: any) {
      app.error = String(e?.message ?? e);
    }
    b.busy = false;
  }
  function queueModSearch() {
    clearTimeout(modTimer);
    modTimer = window.setTimeout(() => void searchMods(), 200);
  }
  async function addMod(text: string) {
    if (!modBrowser) return;
    const block = modBrowser.block;
    modBrowser = null;
    const r = await app.run(() => api.configModAdd(block, text));
    if (r) {
      custom = r.customMods.map((b) => ({ ...b }));
      customDirty = false;
      await app.afterTreeChange();
    }
  }
  let setDialog = $state<{ mode: "new" | "rename"; title: string; copy: boolean } | null>(null);

  async function reload() {
    const r = await app.run(() => api.listConfig());
    if (r) {
      data = r;
      loadedRev = r.rev;
      if (!customDirty) custom = r.customMods.map((b) => ({ ...b }));
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

  function matches(it: ConfigItem): boolean {
    if (!showAll && !it.visible) return false;
    const f = filter.trim().toLowerCase();
    if (!f) return true;
    return (it.labelZh ?? "").toLowerCase().includes(f) || (it.label ?? "").toLowerCase().includes(f) || it.var.toLowerCase().includes(f);
  }
  const hiddenCount = $derived(data ? data.sections.reduce((n, s) => n + s.items.filter((i) => !i.visible).length, 0) : 0);

  // --- POB's column layout (ConfigTab:Draw) ------------------------------------
  // Sections in POB's order; each goes into its own `col` when that column still
  // has room for it within the visible height, otherwise into the shortest
  // column. Heights are the cards' measured heights (a card's height does not
  // depend on its column), with a row-count estimate before the first measure.
  const COL_PITCH = 370;
  const CUSTOM = "__custom";
  let viewW = $state(0);
  let viewH = $state(0);
  const heights = $state<Record<string, number>>({});
  type Placed = { key: string; sec: ConfigSection | null; items: ConfigItem[] };
  const columns = $derived.by((): Placed[][] => {
    if (!data) return [];
    const entries: (Placed & { col: number; est: number })[] = [];
    for (const sec of data.sections) {
      const items = sec.items.filter(matches);
      if (!items.length) continue;
      const est = 34 + items.reduce((h, it) => h + (it.type === "text" ? 96 : 24), 0);
      entries.push({ key: sec.name, sec, items, col: sec.col || 1, est });
    }
    entries.push({ key: CUSTOM, sec: null, items: [], col: 1, est: 90 + custom.length * 120 });
    const maxCol = Math.max(1, Math.floor((viewW - 10) / COL_PITCH));
    const colY: number[] = Array.from({ length: maxCol }, () => 0);
    const out: Placed[][] = Array.from({ length: maxCol }, () => []);
    for (const e of entries) {
      const h = heights[e.key] ?? e.est;
      let col: number;
      if (e.col >= 1 && e.col <= maxCol && colY[e.col - 1] + h + 28 <= viewH) {
        col = e.col - 1;
      } else {
        col = 0;
        for (let c = 1; c < maxCol; c++) if (colY[c] < colY[col]) col = c;
      }
      out[col].push(e);
      colY[col] += h + 10;
    }
    return out.filter((c) => c.length);
  });

  async function set(it: ConfigItem, value: unknown) {
    const r = await app.run(() => api.setConfig(it.var, value as any));
    if (r) await changed();
  }
  async function reset(it: ConfigItem) {
    const r = await app.run(() => api.resetConfig(it.var));
    if (r) await changed();
  }
  function numVal(v: unknown): string {
    return v == null || v === "" ? "" : String(v);
  }
  function isDefault(it: ConfigItem): boolean {
    return it.value == null || it.value === false || it.value === "";
  }

  async function applyCustom() {
    const r = await app.run(() => api.setCustomMods(custom));
    customDirty = false;
    if (r) await changed();
  }

  async function setChange(v: string) {
    const r = await app.run(() => api.setConfigSet(Number(v)));
    if (r) await changed();
  }
  async function setDialogOk() {
    if (!setDialog || !data || !setDialog.title.trim()) return;
    const d = setDialog;
    const active = data.activeConfigSetId;
    setDialog = null;
    const r = d.mode === "new" ? await app.run(() => api.newConfigSet(d.title.trim(), d.copy)) : await app.run(() => api.renameConfigSet(active, d.title.trim()));
    if (r) await changed();
  }
  async function deleteSet() {
    if (!data || data.configSets.length <= 1) return;
    const id = data.activeConfigSetId;
    const cur = data.configSets.find((s) => s.id === id);
    if (!confirm(t("config.deleteSetConfirm", { name: cur ? setTitle(cur) : "" }))) return;
    const r = await app.run(() => api.deleteConfigSet(id));
    if (r) await changed();
  }
  const setTitle = (s: { id: number; title?: string }) => s.title || t("items.defaultSet");
</script>

<div class="page">
  <div class="bar">
    <input class="input sm" placeholder={t("config.search")} bind:value={filter} />
    <label class="chk"><input type="checkbox" bind:checked={showAll} /> {t("config.showAll")}</label>
    {#if hiddenCount}<span class="dim small">{t("config.hidden", { n: hiddenCount })}</span>{/if}
    <span class="grow"></span>
    {#if data}
      <span class="k">{t("config.configSet")}</span>
      <select class="select sm" value={String(data.activeConfigSetId)} onchange={(e) => setChange(e.currentTarget.value)}>
        {#each data.configSets as s}<option value={String(s.id)}>{setTitle(s)}</option>{/each}
      </select>
      <button class="btn ghost sm" onclick={() => (setDialog = { mode: "new", title: t("config.newSetName"), copy: true })}>+</button>
      <button class="btn ghost sm" onclick={() => (setDialog = { mode: "rename", title: data!.configSets.find((s) => s.id === data!.activeConfigSetId)?.title ?? "", copy: false })}>✎</button>
      <button class="btn ghost sm" disabled={data.configSets.length <= 1} onclick={deleteSet}>×</button>
    {/if}
  </div>

  <div class="scroll" bind:clientWidth={viewW} bind:clientHeight={viewH}>
    {#if data}
      <div class="cols">
        {#each columns as col, ci (ci)}
          <div class="col">
            {#each col as entry (entry.key)}
              {#if entry.sec}
                <section class="card" bind:clientHeight={heights[entry.key]}>
                  <h3>{entry.sec.nameZh || entry.sec.name}</h3>
                  {#each entry.items as it (it.var)}
                    <!-- svelte-ignore a11y_no_static_element_interactions -->
                    <div class="opt" class:muted={!it.visible} class:text={it.type === "text"} onmouseenter={(e) => tipEnter(it, e)} onmouseleave={tipLeave}>
                      {#if it.type === "check"}
                        <label class="lbl" for={"cfg-" + it.var}><PobText text={it.labelZh || it.label} muted="var(--ink-1)" /></label>
                        <span class="ctl"><input id={"cfg-" + it.var} type="checkbox" checked={!!it.value} onchange={(e) => set(it, e.currentTarget.checked)} /></span>
                      {:else if it.type === "list"}
                        {@const sel = selectedLabel(it)}
                        <span class="lbl"><PobText text={it.labelZh || it.label} muted="var(--ink-1)" /></span>
                        <span class="ctl">
                          <select class="select sm list" title={sel ? sel.labelZh || sel.label : ""} value={it.value == null ? "" : String(it.value)} onchange={(e) => { const v = e.currentTarget.value; const o = (it.list ?? []).find((x) => String(x.val ?? "") === v); void set(it, o ? o.val : null); }}>
                            {#each it.list ?? [] as o}
                              <option value={String(o.val ?? "")}>{o.labelZh || o.label}</option>
                            {/each}
                          </select>
                        </span>
                      {:else if it.type === "text"}
                        <span class="lbl wide"><PobText text={it.labelZh || it.label} muted="var(--ink-1)" /></span>
                        <textarea class="input area" rows="3" value={numVal(it.value)} onchange={(e) => set(it, e.currentTarget.value)}></textarea>
                      {:else}
                        <span class="lbl"><PobText text={it.labelZh || it.label} muted="var(--ink-1)" /></span>
                        <span class="ctl">
                          <input
                            class="input sm num"
                            type="text"
                            inputmode="decimal"
                            value={numVal(it.value)}
                            placeholder={it.placeholder != null ? String(it.placeholder) : ""}
                            onchange={(e) => { const v = e.currentTarget.value.trim(); if (v === "") return void set(it, null); const n = Number(v); if (Number.isFinite(n)) void set(it, n); else e.currentTarget.value = numVal(it.value); }}
                            onkeydown={(e) => e.key === "Enter" && (e.currentTarget as HTMLInputElement).blur()}
                          />
                        </span>
                      {/if}
                      {#if !isDefault(it) && it.type !== "text"}
                        <button class="rst" title={t("config.reset")} onclick={() => reset(it)}>↺</button>
                      {:else if it.type !== "text"}
                        <span></span>
                      {/if}
                    </div>
                  {/each}
                </section>
              {:else}
                <section class="card" bind:clientHeight={heights[entry.key]}>
                  <h3>{t("config.custom")}</h3>
                  <p class="dim small">{t("config.customHint")}</p>
                  {#each custom as blk, i}
                    <div class="block">
                      <div class="bhead">
                        <input type="checkbox" bind:checked={blk.enabled} onchange={() => (customDirty = true)} />
                        <input class="input sm" placeholder={t("config.customTitle")} bind:value={blk.title} oninput={() => (customDirty = true)} />
                        {#if app.has("configModBrowser")}
                          <button class="btn ghost sm" title={t("config.addModHint")} onclick={() => openModBrowser(i + 1)}>{t("config.addMod")}</button>
                        {/if}
                        <button class="btn ghost sm" onclick={() => { custom.splice(i, 1); customDirty = true; }}>×</button>
                      </div>
                      <textarea class="input area" rows="4" bind:value={blk.text} oninput={() => (customDirty = true)}></textarea>
                    </div>
                  {/each}
                  <div class="btns">
                    <button class="btn sm" onclick={() => { custom.push({ title: `Group ${custom.length + 1}`, text: "", enabled: true }); customDirty = true; }}>{t("config.customAdd")}</button>
                    <button class="btn sm primary" disabled={!customDirty || app.busy > 0} onclick={applyCustom}>{t("config.customApply")}</button>
                  </div>
                </section>
              {/if}
            {/each}
          </div>
        {/each}
      </div>
    {/if}
  </div>

  {#if tip}<TooltipCard lines={tip.lines} x={tip.x} y={tip.y} width={360} />{/if}

  {#if setDialog}
    <div class="modal">
      <div class="dialog">
        <div class="label">{t("config.configSet")}</div>
        <input class="input" placeholder={t("items.setName")} bind:value={setDialog.title} onkeydown={(e) => e.key === "Enter" && setDialogOk()} />
        {#if setDialog.mode === "new"}<label class="chk"><input type="checkbox" bind:checked={setDialog.copy} /> {t("items.newSetCopy")}</label>{/if}
        <div class="btns right"><button class="btn ghost" onclick={() => (setDialog = null)}>{t("tree.cancel")}</button><button class="btn primary" disabled={!setDialog.title.trim()} onclick={setDialogOk}>OK</button></div>
      </div>
    </div>
  {/if}
</div>

{#if modBrowser}
  <div class="modal">
    <div class="dialog wide">
      <div class="label">{t("config.addMod")}</div>
      <input class="input" placeholder={t("config.search")} bind:value={modBrowser.query} oninput={queueModSearch} />
      <p class="dim small">{modBrowser.busy ? t("builds.loading") : t("config.modCount", { count: modBrowser.total })}</p>
      <div class="modlist">
        {#each modBrowser.mods as m (m.text)}
          <button class="modrow" title={m.sources.join(String.fromCharCode(10))} onclick={() => addMod(m.text)}>{m.textZh || m.text}</button>
        {/each}
      </div>
      <div class="btns"><button class="btn ghost" onclick={() => (modBrowser = null)}>{t("tree.cancel")}</button></div>
    </div>
  </div>
{/if}

<style>
  .page {
    height: 100%;
    display: flex;
    flex-direction: column;
    min-height: 0;
  }
  .bar {
    display: flex;
    align-items: center;
    gap: 8px;
    height: 36px;
    padding: 0 12px;
    border-bottom: 1px solid var(--edge-0);
    background: var(--surface-1);
  }
  .bar .input {
    width: 220px;
  }
  .grow {
    flex: 1;
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
  }
  .small {
    font-size: var(--fs-2xs);
  }
  .select.sm,
  .input.sm {
    height: 24px;
    font-size: var(--fs-xs);
  }
  .scroll {
    flex: 1;
    overflow: auto;
    padding: 10px 10px 24px;
  }
  /* POB: 360px sections on a 370px pitch */
  .cols {
    display: flex;
    align-items: flex-start;
    gap: 10px;
  }
  .col {
    width: 360px;
    flex: none;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .card {
    padding: 4px 6px 6px;
    background: var(--surface-1);
    border: 1px solid var(--edge-0);
    border-radius: var(--radius-s);
  }
  h3 {
    margin: 0 0 4px;
    padding-left: 8px;
    position: relative;
    font-size: var(--fs-2xs);
    letter-spacing: 0.12em;
    font-weight: 600;
    color: var(--ink-2);
  }
  h3::before {
    content: "";
    position: absolute;
    left: 0;
    top: 2px;
    bottom: 2px;
    width: 2px;
    background: var(--gold);
  }
  /* POB's row: the label ends against the control column, controls start at
     one x down the whole section; the last column holds the reset arrow */
  .opt {
    display: grid;
    grid-template-columns: minmax(0, 1fr) 128px 16px;
    align-items: center;
    column-gap: 6px;
    min-height: 22px;
    padding: 1px 0 1px 4px;
    border-radius: var(--radius-s);
    font-size: var(--fs-xs);
  }
  .opt:hover {
    background: var(--surface-hover);
  }
  .opt.muted {
    opacity: 0.55;
  }
  .opt.text {
    grid-template-columns: 1fr;
    row-gap: 3px;
    padding: 3px 4px;
  }
  .lbl {
    min-width: 0;
    text-align: right;
    line-height: 1.25;
    cursor: default;
  }
  .lbl.wide {
    text-align: left;
  }
  .ctl {
    display: flex;
    align-items: center;
    min-width: 0;
  }
  .ctl input[type="checkbox"] {
    margin: 0;
  }
  .ctl .select.sm,
  .ctl .input.sm {
    height: 20px;
    font-size: var(--fs-xs);
  }
  .num {
    width: 90px;
    font-family: var(--font-mono);
    font-variant-numeric: tabular-nums;
  }
  .list {
    width: 128px;
    min-width: 0;
    text-overflow: ellipsis;
  }
  .rst {
    appearance: none;
    border: 0;
    background: none;
    color: var(--ink-3);
    padding: 0;
    font-size: var(--fs-2xs);
    cursor: pointer;
    opacity: 0;
  }
  .opt:hover .rst {
    opacity: 1;
  }
  .rst:hover {
    color: var(--gold);
  }
  .area {
    height: auto;
    padding: 6px 8px;
    font-family: var(--font-mono);
    font-size: var(--fs-xs);
    resize: vertical;
    line-height: 1.4;
    width: 100%;
  }
  .block {
    margin: 6px 0;
    padding: 6px;
    border: 1px solid var(--edge-0);
    border-radius: var(--radius-s);
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .bhead {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .bhead .input {
    flex: 1;
  }
  .btns {
    display: flex;
    gap: 6px;
    margin-top: 6px;
  }
  .btns.right {
    justify-content: flex-end;
  }
  p {
    margin: 0 0 6px;
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
  .modal {
    position: fixed;
    inset: 0;
    z-index: 200;
    display: grid;
    place-items: center;
    background: color-mix(in srgb, var(--surface-0) 70%, transparent);
  }
  .dialog.wide {
    width: min(760px, calc(100vw - 32px));
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding: 16px 18px;
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-radius: 8px;
  }
  .modlist {
    max-height: 420px;
    overflow: auto;
    border: 1px solid var(--edge-0);
    border-radius: 4px;
  }
  .modrow {
    display: block;
    width: 100%;
    text-align: left;
    appearance: none;
    border: 0;
    background: none;
    color: var(--c-magic, var(--ink-1));
    padding: 3px 10px;
    font-size: var(--fs-sm);
  }
  .modrow:hover {
    background: var(--surface-2);
  }
</style>
