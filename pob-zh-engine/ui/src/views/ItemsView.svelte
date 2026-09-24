<!-- 物品頁:左 = 裝備欄格(POB 的 slot 佈局,含武器組切換、藥劑啟用、珠寶插槽;可接受拖放),
     中 = 所有物品(ItemListControl 的 loadout 篩選 / 排序 / 刪未使用 / 全刪、(未使用) 標記、
     雙擊編輯、Ctrl+點擊裝到主欄位、拖曳),右 = 貼上新增 / 編輯面板 / 傳奇與稀有資料庫。
     每個動作都是 POB 自己的函式或控制項回呼;hover tooltip 預設帶「替換後差異」。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import TreeSpecBar from "../components/TreeSpecBar.svelte";
  import SharedItemsDialog from "../components/SharedItemsDialog.svelte";
  import TradeDialog from "../components/TradeDialog.svelte";
  import { api, type CraftOptions, type ItemEditState, type ItemSlot, type ItemSummary, type ItemsList, type ItemTooltip } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { equippedIn, filterByLoadout, groupSlots, itemById, looksLikeItem, rarityColor, slotsFor, usedInBadge, type LoadoutFilter } from "$lib/items";
  import { app } from "$lib/state.svelte";
  import { copyText } from "$lib/clipboard";
  import TooltipCard from "../components/TooltipCard.svelte";
  import ItemEditor from "../components/ItemEditor.svelte";
  import ItemDb from "../components/ItemDb.svelte";

  let data = $state<ItemsList | null>(null);
  let selectedId = $state<number | null>(null);
  let loadedRev = -1;

  // hover tooltip (build items by id + slot; db entries by raw text); the
  // stat-difference block is POB's Ctrl+D toggle, remembered per viewer
  let tip = $state<{ lines: ItemTooltip["lines"]; color?: string; x: number; y: number } | null>(null);
  let tipTimer = 0;
  const tipCache = new Map<string, ItemTooltip>();
  let compare = $state(true);
  try {
    compare = localStorage.getItem("pobtools.items.compare") !== "0";
  } catch {
    /* no storage */
  }
  function setCompare(v: boolean) {
    compare = v;
    tipCache.clear();
    try {
      localStorage.setItem("pobtools.items.compare", v ? "1" : "0");
    } catch {
      /* no storage */
    }
  }

  // list toolbar
  let loadout = $state<string>("any");
  const loadoutFilter = $derived<LoadoutFilter>(loadout === "any" || loadout === "current" || loadout === "unused" ? loadout : { setId: Number(loadout) });
  const listItems = $derived(data ? filterByLoadout(data.items, loadoutFilter, data.activeItemSetId) : []);

  // right column
  let rightTab = $state<"paste" | "db" | "edit">("paste");
  let pasteText = $state("");
  let pasteErr = $state<string | null>(null);
  let pasteNote = $state<string | null>(null);
  /** For a pasted item: lines the reverse translation left Chinese, lines POB could not use. */
  let editReport = $state<{ untranslated: string[]; unsupported: string[] } | null>(null);
  let edit = $state<ItemEditState | null>(null);

  // dialogs
  let setDialog = $state<{ mode: "new" | "rename"; title: string; copy: boolean } | null>(null);
  let craft = $state<{ opts: CraftOptions; rarity: number; type: number; base: number; title: string } | null>(null);

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
  // Every hide bumps tipGen, and a fetch only shows its card if nothing hid it in
  // the meantime: without that, a tooltip requested on the way through a row
  // arrived after the mouse had left (or the row was gone -- double-click opens
  // the editor and unmounts it, so its mouseleave never fires) and stayed on
  // screen over the editor with nothing left to close it.
  let tipGen = 0;
  function showTip(key: string, fetch: () => Promise<ItemTooltip>, e: MouseEvent) {
    clearTimeout(tipTimer);
    const gen = ++tipGen;
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
        if (gen === tipGen) tip = { lines: r.lines, color: r.color, x, y };
      } catch {
        if (gen === tipGen) tip = null;
      }
    }, 120);
  }
  function hideTip() {
    clearTimeout(tipTimer);
    tipGen++;
    tip = null;
  }
  const tipItem = (id: number, slotName: string | undefined, e: MouseEvent) =>
    showTip(`i${id}:${slotName ?? ""}:${compare}`, () => api.itemTooltip({ id, slotName, compare }), e);
  const tipRaw = (raw: string, rarity: string, e: MouseEvent) => showTip(`r${rarity}:${compare}:${raw}`, () => api.itemTooltip({ raw, rarity, dbMode: true, compare }), e);

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

  // drag an item from the list onto a slot (ItemSlotControl:CanReceiveDrag = IsItemValidForSlot, via `valid`)
  let dragId = $state<number | null>(null);
  function dragStart(e: DragEvent, id: number) {
    dragId = id;
    e.dataTransfer?.setData("text/plain", String(id));
    if (e.dataTransfer) e.dataTransfer.effectAllowed = "move";
  }
  const canDrop = (s: ItemSlot) => dragId != null && s.valid.includes(dragId);
  function dragOver(e: DragEvent, s: ItemSlot) {
    if (canDrop(s)) {
      e.preventDefault();
      if (e.dataTransfer) e.dataTransfer.dropEffect = "move";
    }
  }
  async function drop(e: DragEvent, s: ItemSlot) {
    e.preventDefault();
    const id = dragId ?? Number(e.dataTransfer?.getData("text/plain"));
    dragId = null;
    if (id && s.valid.includes(id)) await equipTo(id, s.name);
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
  function rowClick(e: MouseEvent, it: ItemSummary) {
    selectedId = it.id!;
    if (e.ctrlKey) void equipPrimary(it.id!, e.shiftKey);
  }
  async function equipPrimary(id: number, alt: boolean) {
    const r = await app.run(() => api.equipPrimary(id, alt));
    if (r) await changed();
  }
  async function sortItems() {
    const r = await app.run(() => api.sortItems());
    if (r) await changed();
  }
  async function deleteUnused() {
    if (!data) return;
    const n = data.items.filter((i) => !i.usedIn).length;
    if (!n || !confirm(t("items.delUnusedConfirm", { n }))) return;
    const r = await app.run(() => api.deleteUnusedItems());
    if (r) await changed();
  }
  async function deleteAll() {
    if (!data?.items.length || !confirm(t("items.delAllConfirm"))) return;
    const r = await app.run(() => api.deleteAllItems());
    if (r) {
      selectedId = null;
      await changed();
    }
  }

  // --- editing (POB's displayItem) ---------------------------------------------
  async function openEdit(p: { id?: number; raw?: string; craft?: { rarity: string; type: string; base: string; title?: string } }) {
    hideTip();
    pasteErr = null;
    try {
      // a pasted text also gets POB's reading of it line by line, so the editor
      // can say which lines did not survive (still Chinese / not understood)
      const [r, parsed] = await Promise.all([
        app.run(() => api.itemEditBegin(p)),
        p.raw ? api.parseItemText(p.raw).catch(() => null) : Promise.resolve(null),
      ]);
      if (r) {
        edit = r;
        editReport = parsed
          ? { untranslated: parsed.untranslated, unsupported: (parsed.lines ?? []).filter((l) => l.unsupported && !l.line.includes("??")).map((l) => l.lineZh || l.line) }
          : null;
        rightTab = "edit";
      }
    } catch (e: any) {
      pasteErr = String(e?.message ?? e);
    }
  }
  function editClosed() {
    edit = null;
    editReport = null;
    if (rightTab === "edit") rightTab = "paste";
  }
  async function editDone(r: { id: number; added: boolean }) {
    edit = null;
    editReport = null;
    pasteText = "";
    rightTab = "paste";
    selectedId = r.id;
    await changed();
  }
  async function openCraft() {
    const opts = await app.run(() => api.craftItemOptions());
    if (opts) craft = { opts, rarity: opts.defaults.rarity, type: opts.defaults.type, base: opts.defaults.base, title: "" };
  }
  async function craftCreate() {
    if (!craft) return;
    const c = craft;
    craft = null;
    const ty = c.opts.types[c.type - 1];
    const base = ty?.bases[c.base - 1];
    if (!ty || !base) return;
    await openEdit({ craft: { rarity: c.opts.rarities[c.rarity - 1]?.rarity ?? "RARE", type: ty.type, base: base.name, title: c.title.trim() || undefined } });
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
    // the button's box has to be read now: after the await `currentTarget` is null
    const b = (e.currentTarget as HTMLElement).getBoundingClientRect();
    try {
      const r = await api.itemTooltip({ raw, compare });
      tip = { lines: r.lines, color: r.color, x: Math.round(Math.max(8, b.left - 360)), y: Math.round(Math.max(8, Math.min(b.top - 200, window.innerHeight - 420))) };
      pasteNote = (r as { reversed?: boolean }).reversed ? t("items.reversed") : null;
    } catch (err: any) {
      pasteErr = String(err?.message ?? err);
    }
  }
  function onPasteBox(e: ClipboardEvent) {
    // Pasting opens the item in the editor, where POB's own tooltip shows what
    // it read; nothing is added until one of the editor's buttons is pressed.
    // Text that is not an item gets a note saying so.
    if (pasteText.trim()) return;
    const text = e.clipboardData?.getData("text") ?? "";
    pasteErr = null;
    pasteNote = null;
    if (!text.trim()) return;
    if (!looksLikeItem(text)) {
      pasteErr = t("items.notItem");
      return;
    }
    e.preventDefault();
    pasteText = text;
    void openEdit({ raw: text });
  }

  // --- database ------------------------------------------------------------------
  async function addFromDb(it: ItemSummary, equip: boolean) {
    if (!it.raw) return;
    const r = await app.run(() => api.addItem(it.raw!, { equip }));
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
  // POB's "Buy Similar": it builds the trade search, the page opens it
  async function buySimilar(id: number) {
    const r = await app.run(() => api.buySimilar({ id }));
    if (r?.url) {
      await copyText(r.url);
      window.open(r.url, "_blank", "noopener");
    }
  }

  // POB's shared item / item-set lists and its trade pane
  let sharedOpen = $state(false);
  let tradeOpen = $state(false);
  async function shareItem(id: number) {
    await app.run(() => api.shareItem(id));
    app.notice = t("items.shareThis");
  }
  async function shareCurrentSet() {
    if (!data) return;
    const setId = data.activeItemSetId;
    await app.run(() => api.shareItemSet(setId));
    app.notice = t("items.shareSet");
  }
</script>

<!-- svelte-ignore a11y_no_static_element_interactions -->
<!-- any click or scroll dismisses a hover card: the row under it may be about to go away or move -->
<div class="page" onmouseleave={hideTip} onpointerdown={hideTip} onwheel={hideTip}>
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
        <button class="btn ghost sm" title={t("items.shareSet")} onclick={shareCurrentSet}>⇪</button>
        <span class="grow"></span>
        <TreeSpecBar compact />
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
            <div class="slot" class:sub={!!s.parent} class:drop={canDrop(s)} ondragover={(e) => dragOver(e, s)} ondrop={(e) => drop(e, s)}>
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
      {#if data}<span class="dim">{t("items.count", { n: listItems.length })}</span>{/if}
      <span class="grow"></span>
      <label class="chk" title={t("items.compare")}><input type="checkbox" checked={compare} onchange={(e) => setCompare(e.currentTarget.checked)} /> {t("items.compare")}</label>
    </div>
    <div class="tools">
      {#if data}
        <select class="select sm" bind:value={loadout}>
          <option value="any">{t("items.loadoutAny")}</option>
          <option value="current">{t("items.loadoutCurrent")}</option>
          <option value="unused">{t("items.loadoutUnused")}</option>
          {#each data.itemSets as s}<option value={String(s.id)}>{setTitle(s)}</option>{/each}
        </select>
      {/if}
      <button class="btn ghost sm" disabled={!data || app.busy > 0} onclick={sortItems}>{t("items.sort")}</button>
      <button class="btn ghost sm" disabled={!data?.items.some((i) => !i.usedIn) || app.busy > 0} onclick={deleteUnused}>{t("items.delUnused")}</button>
      <button class="btn ghost sm danger" disabled={!data?.items.length || app.busy > 0} onclick={deleteAll}>{t("items.delAll")}</button>
      <span class="grow"></span>
      <button class="btn ghost sm" disabled={!data || app.busy > 0} onclick={() => (sharedOpen = true)}>{t("items.shared")}</button>
      {#if app.has("tradeQuery")}
        <button class="btn ghost sm" disabled={!data || app.busy > 0} onclick={() => (tradeOpen = true)}>{t("trade.open")}</button>
      {/if}
      <button class="btn sm" disabled={!data || app.busy > 0} onclick={openCraft}>{t("items.craft")}</button>
    </div>
    <div class="scroll">
      {#if data}
        {#each listItems as it (it.id)}
          {@const where = equippedIn(data.slots, it.id!)}
          {@const badge = usedInBadge(it.usedIn)}
          <!-- svelte-ignore a11y_click_events_have_key_events -->
          <div
            class="row"
            class:sel={selectedId === it.id}
            role="option"
            aria-selected={selectedId === it.id}
            tabindex="0"
            draggable="true"
            ondragstart={(e) => dragStart(e, it.id!)}
            ondragend={() => (dragId = null)}
            onclick={(e) => rowClick(e, it)}
            ondblclick={() => openEdit({ id: it.id! })}
            onmouseenter={(e) => tipItem(it.id!, where[0]?.name, e)}
            onmouseleave={hideTip}
          >
            <span class="nm" style:color={rarityColor(it.rarity)}>{it.nameZh || it.name}</span>
            {#if badge?.kind === "unused"}<span class="where unused">{t("items.unused")}</span>
            {:else if badge?.kind === "elsewhere"}<span class="where">{t("items.usedIn", { where: badge.where })}</span>
            {:else if where.length}<span class="where">{where.map((s) => s.labelZh || s.label).join(", ")}</span>{/if}
            {#if it.unsupported}<span class="bad small">!</span>{/if}
          </div>
        {/each}
      {/if}
    </div>
    <p class="dim small hint">{t("items.listHint")}</p>
    {#if selected && data}
      <div class="actions">
        <button class="btn sm primary" onclick={() => openEdit({ id: selected!.id! })}>{t("items.edit")}</button>
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
        <button class="btn sm" onclick={() => shareItem(selected!.id!)}>{t("items.shareThis")}</button>
        {#if app.has("buySimilar")}
          <button class="btn sm" title={t("items.buySimilarHint")} onclick={() => buySimilar(selected!.id!)}>{t("items.buySimilar")}</button>
        {/if}
        <button class="btn sm danger" onclick={() => deleteItem(selected!.id!)}>{t("items.delete")}</button>
      </div>
    {/if}
  </section>

  <!-- 右:新增 / 編輯 / 資料庫 -->
  <section class="col right">
    <div class="head tabs">
      <button class="tab" class:on={rightTab === "paste"} onclick={() => (rightTab = "paste")}>{t("items.paste")}</button>
      {#if edit}<button class="tab" class:on={rightTab === "edit"} onclick={() => (rightTab = "edit")}>{t("edit.title")}</button>{/if}
      <button class="tab" class:on={rightTab === "db"} onclick={() => (rightTab = "db")}>{t("items.db")}</button>
    </div>
    {#if rightTab === "edit" && edit}
      <ItemEditor item={edit} report={editReport} onchange={(s) => (edit = s)} onclose={editClosed} ondone={editDone} />
    {:else if rightTab === "paste"}
      <div class="pane">
        <p class="dim">{t("items.pasteHint")}</p>
        <textarea class="input area" rows="14" bind:value={pasteText} onpaste={onPasteBox} placeholder="Rarity: RARE&#10;…"></textarea>
        {#if pasteErr}<div class="bad selectable">{pasteErr}</div>{/if}
        {#if pasteNote}<div class="ok">{pasteNote}</div>{/if}
        <div class="btns">
          <button class="btn primary" disabled={!pasteText.trim() || app.busy > 0} onclick={() => addPasted(true)}>{t("items.addEquip")}</button>
          <button class="btn" disabled={!pasteText.trim() || app.busy > 0} onclick={() => addPasted(false)}>{t("items.add")}</button>
          <button class="btn ghost" disabled={!pasteText.trim() || app.busy > 0} onclick={previewPasted} onmouseleave={() => (tip = null)}>{t("items.preview")}</button>
          <button class="btn ghost" disabled={!pasteText.trim() || app.busy > 0} onclick={() => openEdit({ raw: pasteText.trim() })}>{t("items.edit")}</button>
        </div>
      </div>
    {:else}
      <ItemDb onadd={addFromDb} onedit={(it) => openEdit({ raw: it.raw! })} ontip={tipRaw} onleave={hideTip} />
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

  {#if craft}
    <div class="modal">
      <div class="dialog craft">
        <div class="label">{t("items.craftTitle")}</div>
        <div class="crow">
          <span class="k">{t("items.craftRarity")}</span>
          <select class="select" bind:value={craft.rarity}>
            {#each craft.opts.rarities as r, i}<option value={i + 1}>{r.labelZh || r.label}</option>{/each}
          </select>
        </div>
        {#if craft.rarity >= 3}
          <div class="crow">
            <span class="k">{t("items.craftName")}</span>
            <input class="input" bind:value={craft.title} />
          </div>
        {/if}
        <div class="crow">
          <span class="k">{t("items.craftType")}</span>
          <select class="select" bind:value={craft.type} onchange={() => (craft!.base = 1)}>
            {#each craft.opts.types as ty, i}<option value={i + 1}>{ty.typeZh || ty.type}</option>{/each}
          </select>
        </div>
        <div class="crow">
          <span class="k">{t("items.craftBase")}</span>
          <select class="select" bind:value={craft.base}>
            {#each craft.opts.types[craft.type - 1]?.bases ?? [] as b, i}<option value={i + 1}>{b.labelZh || b.label}</option>{/each}
          </select>
        </div>
        <div class="btns right-align">
          <button class="btn ghost" onclick={() => (craft = null)}>{t("tree.cancel")}</button>
          <button class="btn primary" onclick={craftCreate}>{t("items.craftCreate")}</button>
        </div>
      </div>
    </div>
  {/if}
</div>

{#if tradeOpen}
  <TradeDialog onclose={() => (tradeOpen = false)} />
{/if}

{#if sharedOpen}
  <SharedItemsDialog onclose={() => (sharedOpen = false)} onedit={(raw) => openEdit({ raw })} />
{/if}

<style>
  .page {
    height: 100%;
    display: grid;
    grid-template-columns: 340px minmax(260px, 1fr) minmax(360px, 1.2fr);
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
  .tools {
    display: flex;
    align-items: center;
    gap: 4px;
    padding: 6px 10px;
    border-bottom: 1px solid var(--edge-0);
    flex-wrap: wrap;
  }
  .tools .select {
    max-width: 130px;
  }
  .grow {
    flex: 1;
  }
  .chk {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    font-size: var(--fs-2xs);
    color: var(--ink-2);
    white-space: nowrap;
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
    border-radius: var(--radius-s);
  }
  .slot.drop {
    outline: 1px dashed var(--gold);
    outline-offset: 1px;
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
  .select.sm {
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
  .where.unused {
    opacity: 0.7;
  }
  .small {
    font-size: var(--fs-2xs);
  }
  .hint {
    margin: 0;
    padding: 2px 12px 6px;
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
    padding: 0 10px;
    height: 100%;
    color: var(--ink-2);
    font-size: var(--fs-sm);
  }
  .tab.on {
    color: var(--ink-0);
    border-bottom-color: var(--gold);
  }
  .pane {
    display: flex;
    flex-direction: column;
    gap: 8px;
    padding: 10px 12px;
    min-height: 0;
  }
  .pane p {
    margin: 0;
    font-size: var(--fs-xs);
  }
  .area {
    height: auto;
    padding: 8px 9px;
    font-family: var(--font-mono);
    font-size: var(--fs-xs);
    resize: vertical;
    line-height: 1.4;
  }
  .btns {
    display: flex;
    gap: 6px;
    flex-wrap: wrap;
  }
  .right-align {
    justify-content: flex-end;
  }
  .ok {
    color: var(--good);
    font-size: var(--fs-xs);
  }
  .bad {
    color: var(--bad);
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
    width: 340px;
    padding: 14px 16px;
    background: var(--surface-2);
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-m);
    box-shadow: var(--shadow-float);
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .dialog.craft {
    width: 380px;
  }
  .crow {
    display: grid;
    grid-template-columns: 64px 1fr;
    align-items: center;
    gap: 8px;
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
  }
</style>
