<!-- 物品頁:左 = 裝備欄格(POB 的 slot 佈局,含武器組切換、藥劑啟用、珠寶插槽),
     中 = 所有物品(hover tooltip、裝備/卸下/複製/刪除),右 = 貼上新增 + 傳奇/稀有資料庫。
     每個動作都是 POB 自己的 AddItem / DeleteItem / SetSelItemId。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import { api, type ItemDbPage, type ItemSlot, type ItemSummary, type ItemsList, type ItemTooltip } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { equippedIn, groupSlots, itemById, looksLikeItem, rarityColor, slotsFor } from "$lib/items";
  import { app } from "$lib/state.svelte";
  import { copyText } from "$lib/clipboard";
  import TooltipCard from "../components/TooltipCard.svelte";

  let data = $state<ItemsList | null>(null);
  let selectedId = $state<number | null>(null);
  let loadedRev = -1;

  // hover tooltip (build items by id + slot; db entries by raw text)
  let tip = $state<{ lines: ItemTooltip["lines"]; color?: string; x: number; y: number } | null>(null);
  let tipTimer = 0;
  const tipCache = new Map<string, ItemTooltip>();

  // right column
  let rightTab = $state<"paste" | "db">("paste");
  let pasteText = $state("");
  let pasteErr = $state<string | null>(null);
  let pasteNote = $state<string | null>(null);
  let dbKind = $state<"unique" | "rare">("unique");
  let dbQuery = $state("");
  let dbType = $state("");
  let dbPage = $state(1);
  let db = $state<ItemDbPage | null>(null);
  let dbTimer = 0;

  // item set dialogs
  let setDialog = $state<{ mode: "new" | "rename"; title: string; copy: boolean } | null>(null);

  const slotGroups = $derived(data ? groupSlots(data.slots) : []);
  const selected = $derived(data && selectedId != null ? itemById(data.items, selectedId) : undefined);

  async function reload() {
    const r = await app.run(() => api.listItems());
    if (r) {
      data = r;
      loadedRev = r.rev;
      tipCache.clear();
      if (selectedId != null && !itemById(r.items, selectedId)) selectedId = null;
    }
  }

  // Only `rev` / `loaded` are dependencies: reload() runs through app.run,
  // whose busy counter must not be tracked here (it would re-trigger forever).
  $effect(() => {
    const rev = app.rev;
    if (!app.loaded) return;
    if (rev !== loadedRev) untrack(() => void reload());
  });

  /** A change went through POB: pull items now, then let the sidebar catch up. */
  async function changed() {
    await reload();
    await app.afterTreeChange();
  }

  // --- tooltips ---------------------------------------------------------------
  function showTip(key: string, fetch: () => Promise<ItemTooltip>, e: MouseEvent) {
    clearTimeout(tipTimer);
    const x = Math.round(Math.min(e.clientX + 18, window.innerWidth - 360));
    const y = Math.round(Math.min(e.clientY + 12, window.innerHeight - 320));
    const hit = tipCache.get(key);
    if (hit) {
      tip = { lines: hit.lines, color: hit.color, x, y };
      return;
    }
    tipTimer = window.setTimeout(async () => {
      try {
        const r = await fetch();
        tipCache.set(key, r);
        tip = { lines: r.lines, color: r.color, x, y };
      } catch {
        tip = null;
      }
    }, 120);
  }
  function hideTip() {
    clearTimeout(tipTimer);
    tip = null;
  }
  const tipItem = (id: number, slotName: string | undefined, e: MouseEvent) =>
    showTip(`i${id}:${slotName ?? ""}`, () => api.itemTooltip({ id, slotName }), e);
  const tipRaw = (raw: string, rarity: string, e: MouseEvent) => showTip(`r${rarity}:${raw}`, () => api.itemTooltip({ raw, rarity, dbMode: true }), e);

  // --- slots -------------------------------------------------------------------
  async function slotChange(slot: ItemSlot, value: string) {
    const id = Number(value);
    const r = id > 0 ? await app.run(() => api.equipItem(id, slot.name)) : await app.run(() => api.unequipSlot(slot.name));
    if (r) await changed();
  }
  async function toggleFlask(slot: ItemSlot) {
    const r = await app.run(() => api.setSlotActive(slot.name, !slot.active));
    if (r) await changed();
  }
  async function weaponSwap(on: boolean) {
    if (!data || data.useSecondWeaponSet === on) return;
    const r = await app.run(() => api.setWeaponSwap(on));
    if (r) await changed();
  }
  function groupTitle(key: string): string {
    if (key === "weapons") return data?.useSecondWeaponSet ? t("items.weaponSet2") : t("items.weaponSet1");
    if (key === "jewels") return t("items.jewels");
    return "";
  }

  // --- item actions ------------------------------------------------------------
  async function equipTo(id: number, slotName: string) {
    const r = await app.run(() => api.equipItem(id, slotName));
    if (r) await changed();
  }
  async function unequipAll(id: number) {
    if (!data) return;
    for (const s of equippedIn(data.slots, id)) await app.run(() => api.unequipSlot(s.name));
    await changed();
  }
  async function deleteItem(id: number) {
    const r = await app.run(() => api.deleteItem(id));
    if (r) {
      if (selectedId === id) selectedId = null;
      await changed();
    }
  }
  // "Copy text": the item's raw text to the clipboard, with the outcome shown
  // on the button (a copy that silently failed looked like a paste that did nothing).
  let copyState = $state<"idle" | "ok" | "fail">("idle");
  let copyTimer = 0;
  async function copyItem(id: number) {
    const r = await app.run(() => api.itemRaw(id));
    if (!r) return;
    copyState = (await copyText(r.raw)) ? "ok" : "fail";
    clearTimeout(copyTimer);
    copyTimer = window.setTimeout(() => (copyState = "idle"), 1800);
  }

  // --- paste / add ---------------------------------------------------------------
  async function addPasted(equip: boolean) {
    pasteErr = null;
    const raw = pasteText.trim();
    if (!raw) return;
    try {
      const r = await api.addItem(raw, { equip });
      pasteText = "";
      pasteNote = r.reversed ? t("items.reversed") : null;
      selectedId = r.item.id ?? null;
      await changed();
    } catch (e: any) {
      pasteErr = String(e?.message ?? e);
    }
  }
  /** Shows the pasted text the way POB would read it, without adding it. */
  async function previewPasted(e: MouseEvent) {
    pasteErr = null;
    const raw = pasteText.trim();
    if (!raw) return;
    try {
      const r = await api.itemTooltip({ raw });
      const el = e.currentTarget as HTMLElement;
      const b = el.getBoundingClientRect();
      tip = { lines: r.lines, color: r.color, x: Math.round(Math.max(8, b.left - 360)), y: Math.round(Math.max(8, Math.min(b.top - 200, window.innerHeight - 420))) };
      pasteNote = (r as { reversed?: boolean }).reversed ? t("items.reversed") : null;
    } catch (err: any) {
      pasteErr = String(err?.message ?? err);
    }
  }
  function onPasteBox(e: ClipboardEvent) {
    // Pasting only fills the box: the item is added when one of the buttons
    // below is pressed (the user asked for a confirmation step). Text that is
    // not an item gets a note saying so.
    if (pasteText.trim()) return;
    const text = e.clipboardData?.getData("text") ?? "";
    pasteErr = null;
    pasteNote = null;
    if (text.trim() && !looksLikeItem(text)) pasteErr = t("items.notItem");
  }

  // --- database ------------------------------------------------------------------
  function dbSearch(resetPage = true) {
    clearTimeout(dbTimer);
    if (resetPage) dbPage = 1;
    dbTimer = window.setTimeout(async () => {
      const r = await app.run(() => api.itemDb({ kind: dbKind, query: dbQuery.trim(), type: dbType, page: dbPage, size: 40 }));
      if (r) db = r;
    }, 150);
  }
  $effect(() => {
    if (rightTab === "db" && !db) untrack(() => dbSearch());
  });
  async function addFromDb(it: ItemSummary) {
    if (!it.raw) return;
    const r = await app.run(() => api.addItem(it.raw!, { equip: false }));
    if (r) {
      selectedId = r.item.id ?? null;
      await changed();
    }
  }

  // --- item sets ----------------------------------------------------------------
  async function setChange(value: string) {
    const r = await app.run(() => api.setItemSet(Number(value)));
    if (r) await changed();
  }
  async function setDialogOk() {
    if (!setDialog || !data) return;
    const d = setDialog;
    setDialog = null;
    const r = d.mode === "new" ? await app.run(() => api.newItemSet(d.title.trim(), d.copy)) : await app.run(() => api.renameItemSet(data!.activeItemSetId, d.title.trim()));
    if (r) await changed();
  }
  async function deleteSet() {
    if (!data || data.itemSets.length <= 1) return;
    if (!confirm(t("items.deleteSet") + "?")) return;
    const id = data.activeItemSetId;
    const r = await app.run(() => api.deleteItemSet(id));
    if (r) await changed();
  }
  const setTitle = (s: { id: number; title?: string }) => s.title || t("items.defaultSet");
</script>

<!-- svelte-ignore a11y_no_static_element_interactions -->
<div class="page" onmouseleave={hideTip}>
  <!-- 左:裝備欄 -->
  <section class="col slots">
    <div class="head">
      <span class="label">{t("items.equipped")}</span>
      {#if data}
        <select class="select sm" value={String(data.activeItemSetId)} onchange={(e) => setChange(e.currentTarget.value)} title={t("items.itemSet")}>
          {#each data.itemSets as s}
            <option value={String(s.id)}>{setTitle(s)}</option>
          {/each}
        </select>
        <button class="btn ghost sm" onclick={() => (setDialog = { mode: "new", title: "", copy: false })}>+</button>
        <button class="btn ghost sm" onclick={() => (setDialog = { mode: "rename", title: data!.itemSets.find((s) => s.id === data!.activeItemSetId)?.title ?? "", copy: false })}>✎</button>
        <button class="btn ghost sm" disabled={data.itemSets.length <= 1} onclick={deleteSet}>×</button>
      {/if}
    </div>
    <div class="scroll">
      {#each slotGroups as g (g.key)}
        <div class="group">
          {#if g.key === "weapons"}
            <div class="gtitle">
              <span>{groupTitle(g.key)}</span>
              <span class="seg">
                <button class:on={!data?.useSecondWeaponSet} onclick={() => weaponSwap(false)}>I</button>
                <button class:on={data?.useSecondWeaponSet} onclick={() => weaponSwap(true)}>II</button>
              </span>
            </div>
          {:else if groupTitle(g.key)}
            <div class="gtitle"><span>{groupTitle(g.key)}</span></div>
          {/if}
          {#each g.slots as s (s.name)}
            {@const it = data ? itemById(data.items, s.selItemId) : undefined}
            <div class="slot" class:sub={!!s.parent}>
              <span class="sl" title={s.name}>{s.nodeId != null ? t("items.socketN", { n: s.socketIndex ?? "" }) : s.labelZh || s.label}</span>
              <select
                class="select sm it"
                style:color={it ? rarityColor(it.rarity) : "var(--ink-3)"}
                value={String(s.selItemId)}
                onchange={(e) => slotChange(s, e.currentTarget.value)}
                onmouseenter={(e) => it && tipItem(it.id!, s.name, e)}
                onmouseleave={hideTip}
              >
                <option value="0">{t("items.none")}</option>
                {#each s.valid as id}
                  {@const v = data ? itemById(data.items, id) : undefined}
                  {#if v}<option value={String(id)}>{v.nameZh || v.name}</option>{/if}
                {/each}
              </select>
              {#if s.isFlask}
                <input type="checkbox" title={t("items.flaskActive")} checked={s.active} disabled={s.selItemId === 0} onchange={() => toggleFlask(s)} />
              {/if}
            </div>
          {/each}
        </div>
      {/each}
    </div>
  </section>

  <!-- 中:所有物品 -->
  <section class="col list">
    <div class="head">
      <span class="label">{t("items.list")}</span>
      {#if data}<span class="dim">{t("items.count", { n: data.items.length })}</span>{/if}
    </div>
    <div class="scroll">
      {#if data}
        {#each data.items as it (it.id)}
          {@const where = equippedIn(data.slots, it.id!)}
          <!-- svelte-ignore a11y_click_events_have_key_events -->
          <div
            class="row"
            class:sel={selectedId === it.id}
            role="option"
            aria-selected={selectedId === it.id}
            tabindex="0"
            onclick={() => (selectedId = it.id!)}
            onmouseenter={(e) => tipItem(it.id!, where[0]?.name, e)}
            onmouseleave={hideTip}
          >
            <span class="nm" style:color={rarityColor(it.rarity)}>{it.nameZh || it.name}</span>
            {#if where.length}<span class="where">{where.map((s) => s.labelZh || s.label).join(", ")}</span>{/if}
            {#if it.unsupported}<span class="bad small">!</span>{/if}
          </div>
        {/each}
      {/if}
    </div>
    {#if selected && data}
      <div class="actions">
        <select class="select sm" value="" onchange={(e) => { const v = e.currentTarget.value; e.currentTarget.value = ""; if (v) void equipTo(selected!.id!, v); }}>
          <option value="">{t("items.equipTo")}</option>
          {#each slotsFor(data.slots, selected.id!) as s}
            <option value={s.name}>{s.labelZh || s.label}</option>
          {/each}
        </select>
        <button class="btn sm" disabled={!equippedIn(data.slots, selected.id!).length} onclick={() => unequipAll(selected!.id!)}>{t("items.unequip")}</button>
        <button class="btn sm" class:ok={copyState === "ok"} class:danger={copyState === "fail"} onclick={() => copyItem(selected!.id!)}>
          {copyState === "ok" ? t("items.copied") : copyState === "fail" ? t("items.copyFailed") : t("items.copy")}
        </button>
        <button class="btn sm danger" onclick={() => deleteItem(selected!.id!)}>{t("items.delete")}</button>
      </div>
    {/if}
  </section>

  <!-- 右:新增 / 資料庫 -->
  <section class="col right">
    <div class="head tabs">
      <button class="tab" class:on={rightTab === "paste"} onclick={() => (rightTab = "paste")}>{t("items.paste")}</button>
      <button class="tab" class:on={rightTab === "db"} onclick={() => (rightTab = "db")}>{t("items.db")}</button>
    </div>
    {#if rightTab === "paste"}
      <div class="pane">
        <p class="dim">{t("items.pasteHint")}</p>
        <textarea class="input area" rows="14" bind:value={pasteText} onpaste={onPasteBox} placeholder="Rarity: RARE&#10;…"></textarea>
        {#if pasteErr}<div class="bad selectable">{pasteErr}</div>{/if}
        {#if pasteNote}<div class="ok">{pasteNote}</div>{/if}
        <div class="btns">
          <button class="btn primary" disabled={!pasteText.trim() || app.busy > 0} onclick={() => addPasted(true)}>{t("items.addEquip")}</button>
          <button class="btn" disabled={!pasteText.trim() || app.busy > 0} onclick={() => addPasted(false)}>{t("items.add")}</button>
          <button class="btn ghost" disabled={!pasteText.trim() || app.busy > 0} onclick={previewPasted} onmouseleave={() => (tip = null)}>{t("items.preview")}</button>
        </div>
      </div>
    {:else}
      <div class="pane db">
        <div class="dbbar">
          <span class="seg">
            <button class:on={dbKind === "unique"} onclick={() => { dbKind = "unique"; dbType = ""; dbSearch(); }}>{t("items.dbUnique")}</button>
            <button class:on={dbKind === "rare"} onclick={() => { dbKind = "rare"; dbType = ""; dbSearch(); }}>{t("items.dbRare")}</button>
          </span>
          <input class="input sm grow" placeholder={t("items.dbSearch")} bind:value={dbQuery} oninput={() => dbSearch()} />
          {#if db}
            <select class="select sm" bind:value={dbType} onchange={() => dbSearch()}>
              <option value="">{t("items.dbAllTypes")}</option>
              {#each db.types as ty}
                <option value={ty.type}>{ty.typeZh || ty.type}</option>
              {/each}
            </select>
          {/if}
        </div>
        <div class="scroll">
          {#if db}
            {#each db.items as it (it.name)}
              <div class="row dbrow" onmouseenter={(e) => tipRaw(it.raw!, it.rarity, e)} onmouseleave={hideTip} role="listitem">
                <span class="nm" style:color={rarityColor(it.rarity)}>{it.nameZh || it.name}</span>
                <span class="dim small">{it.typeZh || it.type || ""}</span>
                <button class="btn ghost sm" onclick={() => addFromDb(it)}>{t("items.dbAdd")}</button>
              </div>
            {/each}
          {/if}
        </div>
        {#if db}
          <div class="pager">
            <span class="dim">{t("items.dbTotal", { n: db.total })}</span>
            <span class="grow"></span>
            <button class="btn ghost sm" disabled={dbPage <= 1} onclick={() => { dbPage--; dbSearch(false); }}>‹</button>
            <span class="num dim">{dbPage} / {Math.max(1, Math.ceil(db.total / db.size))}</span>
            <button class="btn ghost sm" disabled={dbPage * db.size >= db.total} onclick={() => { dbPage++; dbSearch(false); }}>›</button>
          </div>
        {/if}
      </div>
    {/if}
  </section>

  {#if tip}
    <TooltipCard lines={tip.lines} accent={tip.color} x={tip.x} y={tip.y} />
  {/if}

  {#if setDialog}
    <div class="modal">
      <div class="dialog">
        <div class="label">{setDialog.mode === "new" ? t("items.newSet") : t("items.itemSet")}</div>
        <input class="input" placeholder={t("items.setName")} bind:value={setDialog.title} onkeydown={(e) => e.key === "Enter" && setDialogOk()} />
        {#if setDialog.mode === "new"}
          <label class="chk"><input type="checkbox" bind:checked={setDialog.copy} /> {t("items.newSetCopy")}</label>
        {/if}
        <div class="btns right-align">
          <button class="btn ghost" onclick={() => (setDialog = null)}>{t("tree.cancel")}</button>
          <button class="btn primary" onclick={setDialogOk}>OK</button>
        </div>
      </div>
    </div>
  {/if}
</div>

<style>
  .page {
    height: 100%;
    display: grid;
    grid-template-columns: 340px minmax(260px, 1fr) minmax(320px, 1.1fr);
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
    padding: 8px 10px 14px;
  }
  .group {
    margin-bottom: 10px;
  }
  .gtitle {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin: 6px 0 4px;
    padding-left: 8px;
    position: relative;
    font-size: var(--fs-2xs);
    letter-spacing: 0.12em;
    color: var(--ink-2);
  }
  .gtitle::before {
    content: "";
    position: absolute;
    left: 0;
    top: 2px;
    bottom: 2px;
    width: 2px;
    background: var(--gold);
  }
  .seg {
    display: inline-flex;
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-s);
    overflow: hidden;
  }
  .seg button {
    appearance: none;
    border: 0;
    background: transparent;
    color: var(--ink-2);
    padding: 1px 8px;
    font-size: var(--fs-2xs);
  }
  .seg button.on {
    background: var(--gold-soft);
    color: var(--gold);
  }
  .slot {
    display: grid;
    grid-template-columns: 78px 1fr auto;
    align-items: center;
    gap: 6px;
    margin: 2px 0;
  }
  .slot.sub {
    grid-template-columns: 78px 1fr auto;
    opacity: 0.85;
  }
  .slot.sub .sl {
    padding-left: 10px;
    color: var(--ink-3);
  }
  .sl {
    font-size: var(--fs-xs);
    color: var(--ink-2);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .select.sm,
  .input.sm {
    height: 24px;
    font-size: var(--fs-xs);
  }
  .select.it {
    width: 100%;
    min-width: 0;
    text-overflow: ellipsis;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 4px 8px;
    border-radius: var(--radius-s);
    font-size: var(--fs-sm);
    cursor: default;
  }
  .row:hover {
    background: var(--surface-hover);
  }
  .row.sel {
    background: var(--gold-soft);
    box-shadow: inset 2px 0 0 var(--gold);
  }
  .nm {
    flex: 1;
    min-width: 0;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .where {
    font-size: var(--fs-2xs);
    color: var(--ink-3);
    white-space: nowrap;
  }
  .small {
    font-size: var(--fs-2xs);
  }
  .actions {
    display: flex;
    gap: 6px;
    padding: 8px 10px;
    border-top: 1px solid var(--edge-0);
    background: var(--surface-1);
    flex-wrap: wrap;
  }
  .btn.danger:hover:not(:disabled) {
    color: var(--bad);
    border-color: var(--bad);
  }
  .tabs {
    gap: 0;
    padding: 0 6px;
  }
  .tab {
    appearance: none;
    border: 0;
    border-bottom: 2px solid transparent;
    background: none;
    height: 100%;
    padding: 0 10px;
    color: var(--ink-2);
    font-size: var(--fs-sm);
  }
  .tab.on {
    color: var(--ink-0);
    border-bottom-color: var(--gold);
  }
  .pane {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: column;
    gap: 8px;
    padding: 10px 12px;
  }
  .pane p {
    margin: 0;
    font-size: var(--fs-xs);
  }
  .area {
    flex: 1;
    height: auto;
    min-height: 160px;
    padding: 8px 9px;
    font-family: var(--font-mono);
    font-size: var(--fs-xs);
    resize: none;
    line-height: 1.4;
  }
  .btns {
    display: flex;
    gap: 6px;
  }
  .right-align {
    justify-content: flex-end;
  }
  .ok {
    color: var(--ok);
    font-size: var(--fs-xs);
  }
  .bad {
    color: var(--bad);
    font-size: var(--fs-xs);
  }
  .db {
    padding: 0;
    gap: 0;
  }
  .dbbar {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 8px 10px;
    border-bottom: 1px solid var(--edge-0);
  }
  .grow {
    flex: 1;
    min-width: 0;
  }
  .dbrow .btn {
    opacity: 0;
  }
  .dbrow:hover .btn {
    opacity: 1;
  }
  .pager {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 6px 10px;
    border-top: 1px solid var(--edge-0);
    font-size: var(--fs-xs);
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
  .chk {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    font-size: var(--fs-xs);
  }
</style>
