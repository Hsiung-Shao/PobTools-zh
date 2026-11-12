<!-- 物品頁:左 = 裝備欄格(POB 的 slot 佈局,含武器組切換、藥劑啟用、珠寶插槽;可接受拖放),
     中 = 所有物品(ItemListControl 的 loadout 篩選 / 排序 / 刪未使用 / 全刪、(未使用) 標記、
     雙擊編輯、Ctrl+點擊裝到主欄位、拖曳),右 = 貼上新增 / 編輯面板 / 傳奇與稀有資料庫。
     每個動作都是 POB 自己的函式或控制項回呼;hover tooltip 預設帶「替換後差異」。 -->
<script lang="ts">
  import { untrack } from "svelte";
  import TreeSpecBar from "../components/TreeSpecBar.svelte";
  import SharedItemsDialog from "$lib/ui/SharedItemsDialog.svelte";
  import TradeDialog from "../components/TradeDialog.svelte";
  import { api, type CraftOptions, type ItemEditState, type ItemSlot, type ItemSummary, type ItemsList, type ItemTooltip } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { equippedIn, filterByLoadout, groupSlots, itemById, looksLikeItem, looksLikePobRaw, rarityColor, restoreRarity, resumeEdit, slotsFor, usedInBadge, type LoadoutFilter } from "$lib/items";
  import { app } from "$lib/state.svelte";
  import { dataKey } from "$lib/lang.svelte";
  import { copyText } from "$lib/clipboard";
  import TooltipCard from "../components/TooltipCard.svelte";
  import SlotSelect from "$lib/ui/SlotSelect.svelte";
  import ItemEditor from "../components/ItemEditor.svelte";
  import ItemDb from "../components/ItemDb.svelte";
  import Select from "$lib/ui/Select.svelte";

  let data = $state<ItemsList | null>(null);
  let selectedId = $state<number | null>(null);
  let loadedRev = "";

  // hover tooltip (build items by id + slot; db entries by raw text); the
  // stat-difference block is POB's Ctrl+D toggle, remembered per viewer
  let tip = $state<{ lines: ItemTooltip["lines"]; color?: string; x: number; y: number; avoid?: { left: number; top: number; right: number; bottom: number } } | null>(null);
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
  let rightTab = $state<"paste" | "db" | "edit" | "craft">("paste");
  // A right-click on a tree socket lands here (app.focusSocketNode): scroll its
  // slot into view, flash it and put the focus on its drop-down, which is where
  // "choose the jewel for this socket" happens.
  let flashSlot = $state<string | null>(null);
  $effect(() => {
    const nodeId = app.focusSocketNode;
    const slots = data?.slots;
    if (nodeId == null || !slots) return;
    const s = slots.find((x) => x.nodeId === nodeId);
    untrack(() => {
      app.focusSocketNode = null;
      if (!s) return;
      flashSlot = s.name;
      queueMicrotask(() => {
        const el = document.querySelector<HTMLElement>(`.slot[data-slot="${CSS.escape(s.name)}"]`);
        el?.scrollIntoView({ block: "center" });
        el?.querySelector<HTMLElement>(".slotdd")?.focus();
      });
      setTimeout(() => (flashSlot = null), 1600);
    });
  });
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
      loadedRev = dataKey(r.rev, app.langRev);
      tipCache.clear();
      if (selectedId != null && !itemById(r.items, selectedId)) selectedId = null;
    }
  }

  // Only `rev` / `loaded` are dependencies: reload() runs through app.run,
  // whose busy counter must not be tracked here (it would re-trigger forever).
  $effect(() => {
    const rev = app.dataRev;
    if (!app.loaded) return;
    if (rev !== loadedRev) untrack(() => void reload());
  });

  // Leaving this tab un-mounts it; POB keeps its displayItem (a fresh craft, never
  // added), so pick the edit back up from it when the page comes back.
  let resumed = false;
  $effect(() => {
    if (!app.loaded || resumed) return;
    resumed = true;
    untrack(() =>
      void resumeEdit(() => api.itemEditState()).then((s) => {
        if (s && !edit) {
          edit = s;
          rightTab = "edit";
        }
      }),
    );
  });

  // F2: the item being edited and the craft lists come translated -- read them
  // again in the other language; the craft choices (indices) and the edit stay
  let seenLang = app.langRev;
  $effect(() => {
    const lr = app.langRev;
    if (lr === seenLang) return;
    seenLang = lr;
    untrack(() => {
      if (edit) void resumeEdit(() => api.itemEditState()).then((s) => s && edit && (edit = s));
      if (craft) {
        void api.craftItemOptions().then((opts) => craft && (craft = { ...craft, opts })).catch(() => {});
      }
    });
  });

  /** A change went through POB: pull items now, then let the sidebar catch up. */
  async function changed() {
    await reload();
    await app.afterTreeChange();
  }

  // --- tooltips ---------------------------------------------------------------
  // Every hide bumps tipGen, and a fetch only shows its card if nothing hid it in the
  // meantime (the row may be gone: double-click opens the editor and unmounts it, so its
  // mouseleave never fires).
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
  // Craft: the base pointed at in the open list gets POB's own preview tooltip
  // (the Craft Item dialog's base.tooltipFunc), placed beside the option.
  // The open list is portalled to <body>: entering it fires the page's mouseleave,
  // which must not clear this tooltip (pageLeave); closing the list (null) does.
  let craftListActive = false;
  function craftBaseActive(v: string | number | null, rect: DOMRect | null) {
    craftListActive = !!(craft && v != null && rect);
    if (!craft || v == null || !rect) return hideTip();
    const c = { rarity: craft.rarity, type: craft.type, base: Number(v), title: craft.title.trim() || undefined, compare };
    const key = `craft:${c.rarity}:${c.type}:${c.base}:${c.compare}:${c.title ?? ""}`;
    clearTimeout(tipTimer);
    const gen = ++tipGen;
    const place = (r: ItemTooltip) => {
      if (gen === tipGen) tip = { lines: r.lines, color: r.color, x: Math.round(rect.right + 8), y: Math.round(rect.top), avoid: { left: rect.left, top: rect.top, right: rect.right, bottom: rect.bottom } };
    };
    const hit = tipCache.get(key);
    if (hit) return place(hit);
    tipTimer = window.setTimeout(async () => {
      try {
        const r = (await api.craftBaseTooltip(c)) as ItemTooltip;
        tipCache.set(key, r);
        place(r);
      } catch {
        if (gen === tipGen) tip = null;
      }
    }, 120);
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

  function restoredNote(rarity: string): string {
    return t("items.rarityRestored", { r: t("notes.col." + rarity) });
  }

  // --- editing (POB's displayItem) ---------------------------------------------
  async function openEdit(p: { id?: number; raw?: string; craft?: { rarity: string; type: string; base: string; title?: string } }) {
    hideTip();
    pasteErr = null;
    if (p.raw) {
      // POB's own text starts with "Rarity:"; when that line was lost POB would read the item as unique
      const fixed = restoreRarity(p.raw);
      if (fixed.rarity) {
        p = { ...p, raw: fixed.text };
        pasteNote = restoredNote(fixed.rarity);
      } else if (looksLikePobRaw(p.raw)) {
        pasteErr = t("items.noRarity");
        return;
      }
    }
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
    pasteNote = null;
    if (rightTab === "edit") rightTab = "paste";
  }
  async function editDone(r: { id: number; added: boolean }) {
    edit = null;
    editReport = null;
    pasteText = "";
    pasteNote = null;
    rightTab = "paste";
    selectedId = r.id;
    await changed();
  }
  // "Craft" is the right pane's third tab; its choices stay as they were between visits.
  async function openCraft() {
    rightTab = "craft";
    if (craft) return;
    const opts = await app.run(() => api.craftItemOptions());
    if (opts) craft = { opts, rarity: opts.defaults.rarity, type: opts.defaults.type, base: opts.defaults.base, title: "" };
  }
  async function craftCreate() {
    if (!craft) return;
    const c = craft;
    const ty = c.opts.types[c.type - 1];
    const base = ty?.bases[c.base - 1];
    if (!ty || !base) return;
    await openEdit({ craft: { rarity: c.opts.rarities[c.rarity - 1]?.rarity ?? "RARE", type: ty.type, base: base.name, title: c.title.trim() || undefined } });
  }

  // --- paste / add ---------------------------------------------------------------
  async function addPasted(equip: boolean) {
    pasteErr = null;
    let raw = pasteText.trim();
    if (!raw) return;
    const fixed = restoreRarity(raw);
    if (fixed.rarity) raw = fixed.text;
    else if (looksLikePobRaw(raw)) {
      pasteErr = t("items.noRarity");
      return;
    }
    try {
      const r = await api.addItem(raw, { equip });
      pasteText = "";
      pasteNote = r.reversed ? t("items.reversed") : fixed.rarity ? restoredNote(fixed.rarity) : null;
      selectedId = r.item.id ?? null;
      await changed();
    } catch (e: any) {
      pasteErr = String(e?.message ?? e);
    }
  }
  /** Shows the pasted text the way POB would read it, without adding it. */
  async function previewPasted(e: MouseEvent) {
    pasteErr = null;
    let raw = pasteText.trim();
    if (!raw) return;
    const fixed = restoreRarity(raw);
    if (fixed.rarity) raw = fixed.text;
    else if (looksLikePobRaw(raw)) {
      pasteErr = t("items.noRarity");
      return;
    }
    // the button's box has to be read now: after the await `currentTarget` is null
    const b = (e.currentTarget as HTMLElement).getBoundingClientRect();
    try {
      const r = await api.itemTooltip({ raw, compare });
      tip = { lines: r.lines, color: r.color, x: Math.round(Math.max(8, b.left - 360)), y: Math.round(Math.max(8, Math.min(b.top - 200, window.innerHeight - 420))) };
      pasteNote = (r as { reversed?: boolean }).reversed ? t("items.reversed") : fixed.rarity ? restoredNote(fixed.rarity) : null;
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
      pasteErr = t(looksLikePobRaw(text) ? "items.noRarity" : "items.notItem");
      return;
    }
    e.preventDefault();
    // a lost "Rarity:" line is put back (openEdit adds the note); the box shows the restored text
    pasteText = restoreRarity(text).text;
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
    if (!setDialog || !data || !setDialog.title.trim()) return;
    const d = setDialog;
    setDialog = null;
    const r = d.mode === "new" ? await app.run(() => api.newItemSet(d.title.trim(), d.copy)) : await app.run(() => api.renameItemSet(data!.activeItemSetId, d.title.trim()));
    if (r) await changed();
  }
  async function deleteSet() {
    if (!data || data.itemSets.length <= 1) return;
    const id = data.activeItemSetId;
    const cur = data.itemSets.find((s) => s.id === id);
    if (!confirm(t("items.deleteSetConfirm", { name: cur ? setTitle(cur) : "" }))) return;
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
<div class="page" onmouseleave={() => craftListActive || hideTip()} onpointerdown={hideTip} onwheel={hideTip}>
  <div class="toolbar">
    {#if data}
      <span class="tlabel">{t("items.itemSet")}</span>
      <Select class="select sm iv" value={String(data.activeItemSetId)} options={data.itemSets.map((s) => ({ value: String(s.id), label: setTitle(s) }))} onchange={(v) => setChange(String(v))} title={t("items.itemSet")} />
      <button class="btn ghost sm ico" title={t("items.newSet")} aria-label={t("items.newSet")} onclick={() => (setDialog = { mode: "new", title: t("items.newSetName"), copy: false })}>+</button>
      <button class="btn ghost sm ico" title={t("builds.rename")} aria-label={t("builds.rename")} onclick={() => (setDialog = { mode: "rename", title: data!.itemSets.find((s) => s.id === data!.activeItemSetId)?.title ?? "", copy: false })}>✎</button>
      <button class="btn ghost sm ico" title={t("items.deleteSet")} aria-label={t("items.deleteSet")} disabled={data.itemSets.length <= 1} onclick={deleteSet}>×</button>
      <button class="btn ghost sm ico" title={t("items.shareSet")} aria-label={t("items.shareSet")} onclick={shareCurrentSet}>⇪</button>
      <span class="vsep"></span>
      <TreeSpecBar compact />
    {/if}
    <span class="grow"></span>
    <button class="btn ghost sm" disabled={!data || app.busy > 0} onclick={() => (sharedOpen = true)}>{t("items.shared")}</button>
    {#if app.has("tradeQuery")}
      <button class="btn ghost sm" disabled={!data || app.busy > 0} onclick={() => (tradeOpen = true)}>{t("trade.open")}</button>
    {/if}
    <button class="btn sm" class:on={rightTab === "craft"} disabled={!data} onclick={openCraft}>{t("items.craft")}</button>
    <button class="btn sm accent" onclick={() => (rightTab = "paste")}>{t("items.paste")}</button>
  </div>

  <div class="cols">
    <!-- 左:裝備欄 -->
    <section class="col slots">
      <div class="chead"><span class="card-title">{t("items.equipped")}</span></div>
      <div class="scroll">
        {#each slotGroups as g (g.key)}
          <div class="group">
            {#if g.key === "weapons"}
              <div class="gtitle">
                <span class="card-title">{groupTitle(g.key)}</span>
                <span class="seg">
                  <button class:on={!data?.useSecondWeaponSet} onclick={() => weaponSwap(false)}>I</button>
                  <button class:on={data?.useSecondWeaponSet} onclick={() => weaponSwap(true)}>II</button>
                </span>
              </div>
            {:else if groupTitle(g.key)}
              <div class="gtitle"><span class="card-title">{groupTitle(g.key)}</span></div>
            {/if}
            {#each g.slots as s (s.name)}
              {@const it = data ? itemById(data.items, s.selItemId) : undefined}
              <div class="slot" class:sub={!!s.parent} class:drop={canDrop(s)} class:flash={flashSlot === s.name} data-slot={s.name} ondragover={(e) => dragOver(e, s)} ondrop={(e) => drop(e, s)}>
                <span class="sl" title={s.name}>{s.nodeId != null ? t("items.socketN", { n: s.socketIndex ?? "" }) : s.labelZh || s.label}</span>
                <!-- each entry previews against what the slot holds (ItemSlotControl) -->
                <SlotSelect
                  value={s.selItemId}
                  label={it ? it.nameZh || it.name : t("items.none")}
                  color={it ? rarityColor(it.rarity) : "var(--ink-3)"}
                  options={s.valid.flatMap((id) => {
                    const v = data ? itemById(data.items, id) : undefined;
                    return v ? [{ id, label: v.nameZh || v.name, color: rarityColor(v.rarity) }] : [];
                  })}
                  onpick={(id) => slotChange(s, String(id))}
                  ontip={(id, e) => tipItem(id, s.name, e)}
                  onleave={hideTip}
                />
                {#if s.isFlask}
                  <input type="checkbox" class="sw" title={t("items.flaskActive")} aria-label={t("items.flaskActive")} checked={s.active} disabled={s.selItemId === 0} onchange={() => toggleFlask(s)} />
                {/if}
              </div>
            {/each}
          </div>
        {/each}
      </div>
    </section>

    <!-- 中:所有物品 -->
    <section class="col list">
      <div class="chead">
        <span class="card-title">{t("items.list")}</span>
        {#if data}<span class="num dim small">{t("items.count", { n: listItems.length })}</span>{/if}
        <span class="grow"></span>
        <label class="chk" title={t("items.compare")}>{t("items.compare")} <input type="checkbox" class="sw" checked={compare} onchange={(e) => setCompare(e.currentTarget.checked)} /></label>
      </div>
      <div class="tools">
        {#if data}
          <Select
            class="select sm iv"
            value={loadout}
            options={[
              { value: "any", label: t("items.loadoutAny") },
              { value: "current", label: t("items.loadoutCurrent") },
              { value: "unused", label: t("items.loadoutUnused") },
              ...data.itemSets.map((s) => ({ value: String(s.id), label: setTitle(s) })),
            ]}
            onchange={(v) => (loadout = String(v))}
          />
        {/if}
        <button class="btn ghost sm" disabled={!data || app.busy > 0} onclick={sortItems}>{t("items.sort")}</button>
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
              {:else if badge?.kind === "elsewhere"}<span class="where" title={t("items.usedIn", { where: badge.where })}>{t("items.usedIn", { where: badge.where })}</span>
              {:else if where.length}{@const ws = where.map((s) => s.labelZh || s.label).join(", ")}<span class="where" title={ws}>{ws}</span>{/if}
              {#if it.unsupported}<span class="bad small">!</span>{/if}
            </div>
          {/each}
        {/if}
      </div>
      <p class="dim small hint">{t("items.listHint")}</p>
      {#if selected && data}
        <div class="actions">
          <button class="btn sm" onclick={() => openEdit({ id: selected!.id! })}>{t("items.edit")}</button>
          <!-- a menu: picking a slot equips, the control goes back to its prompt -->
          <Select class="select sm iv" action placeholder={t("items.equipTo")} options={slotsFor(data.slots, selected.id!).map((s) => ({ value: s.name, label: s.labelZh || s.label }))} onchange={(v) => void equipTo(selected!.id!, String(v))} />
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
      <div class="foot">
        <button class="btn sm" disabled={!data?.items.some((i) => !i.usedIn) || app.busy > 0} onclick={deleteUnused}>{t("items.delUnused")}</button>
        <button class="btn sm danger" disabled={!data?.items.length || app.busy > 0} onclick={deleteAll}>{t("items.delAll")}</button>
      </div>
    </section>

    <!-- 右:新增 / 編輯 / 資料庫 -->
    <section class="col right">
      <div class="rhead">
        <div class="tabs">
          <button class:on={rightTab === "paste"} onclick={() => (rightTab = "paste")}>{t("items.paste")}</button>
          {#if edit}<button class:on={rightTab === "edit"} onclick={() => (rightTab = "edit")}>{t("edit.title")}</button>{/if}
          <button class:on={rightTab === "db"} onclick={() => (rightTab = "db")}>{t("items.db")}</button>
          <button class:on={rightTab === "craft"} disabled={!data} onclick={openCraft}>{t("items.craft")}</button>
        </div>
      </div>
      {#if rightTab === "edit" && edit}
        {#if pasteNote}<div class="ok edit-note">{pasteNote}</div>{/if}
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
      {:else if rightTab === "craft"}
        <div class="pane craft">
          <p class="dim">{t("items.craftTitle")}</p>
          {#if craft}
            <div class="crow">
              <span class="k">{t("items.craftRarity")}</span>
              <Select class="select" ariaLabel={t("items.craftRarity")} value={craft.rarity} options={craft.opts.rarities.map((r, i) => ({ value: i + 1, label: r.labelZh || r.label }))} onchange={(v) => (craft!.rarity = Number(v))} />
            </div>
            {#if craft.rarity >= 3}
              <div class="crow">
                <span class="k">{t("items.craftName")}</span>
                <input class="input" bind:value={craft.title} />
              </div>
            {/if}
            <div class="crow">
              <span class="k">{t("items.craftType")}</span>
              <Select class="select" ariaLabel={t("items.craftType")} value={craft.type} options={craft.opts.types.map((ty, i) => ({ value: i + 1, label: ty.typeZh || ty.type }))} onchange={(v) => { craft!.type = Number(v); craft!.base = 1; }} />
            </div>
            <div class="crow">
              <span class="k">{t("items.craftBase")}</span>
              <Select class="select" ariaLabel={t("items.craftBase")} value={craft.base} options={(craft.opts.types[craft.type - 1]?.bases ?? []).map((b, i) => ({ value: i + 1, label: b.labelZh || b.label }))} onchange={(v) => (craft!.base = Number(v))} onactive={craftBaseActive} />
            </div>
            <div class="btns">
              <button class="btn primary" disabled={app.busy > 0} onclick={craftCreate}>{t("items.craftCreate")}</button>
            </div>
          {:else}
            <p class="dim pulse">{t("app.booting")}</p>
          {/if}
        </div>
      {:else}
        <ItemDb onadd={addFromDb} onedit={(it) => openEdit({ raw: it.raw! })} ontip={tipRaw} onleave={hideTip} />
      {/if}
    </section>
  </div>

  {#if tip}
    <TooltipCard lines={tip.lines} accent={tip.color} x={tip.x} y={tip.y} avoid={tip.avoid ?? null} />
  {/if}

  {#if setDialog}
    <div class="modal">
      <div class="dialog">
        <div class="label">{setDialog.mode === "new" ? t("items.newSet") : t("items.itemSet")}</div>
        <input class="input" placeholder={t("items.setName")} bind:value={setDialog.title} onkeydown={(e) => e.key === "Enter" && setDialogOk()} />
        {#if setDialog.mode === "new"}
          <label class="chk"><input type="checkbox" class="sw" bind:checked={setDialog.copy} /> {t("items.newSetCopy")}</label>
        {/if}
        <div class="btns right-align">
          <button class="btn ghost" onclick={() => (setDialog = null)}>{t("tree.cancel")}</button>
          <button class="btn primary" disabled={!setDialog.title.trim()} onclick={setDialogOk}>OK</button>
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
    display: flex;
    flex-direction: column;
    min-height: 0;
  }
  .toolbar {
    flex: none;
  }
  .tlabel {
    font-size: var(--fs-xs);
    color: var(--ink-2);
    white-space: nowrap;
  }
  .toolbar :global(.select.iv) {
    min-width: 0;
    max-width: 200px;
  }
  .ico {
    min-width: 24px;
    justify-content: center;
  }
  .btn.accent {
    border-color: var(--gold);
    background: var(--gold-soft);
    color: var(--gold-text);
  }
  .btn.accent:hover:not(:disabled) {
    background: color-mix(in srgb, var(--gold) 26%, transparent);
    border-color: var(--gold);
    color: var(--gold-text);
  }
  .btn.on {
    border-color: var(--edge-2);
    color: var(--ink-0);
  }
  .cols {
    flex: 1;
    min-height: 0;
    display: grid;
    grid-template-columns: 316px minmax(260px, 1fr) minmax(360px, 1.2fr);
  }
  /* RWD on the main area's width (App's `view` container, sidebar excluded).
     Medium: equipment over the item list on the left, the add / edit / craft
     column keeps the full height on the right. Narrow: one column, each part
     with a usable height, the page scrolling as a whole. */
  @container view (max-width: 979px) {
    .cols {
      grid-template-columns: minmax(0, 1fr) minmax(0, 1.15fr);
      grid-template-rows: minmax(200px, 1fr) minmax(200px, 1fr);
      overflow-y: auto;
    }
    .slots {
      grid-area: 1 / 1;
    }
    .list {
      grid-area: 2 / 1;
      border-left: 0;
      border-top: 1px solid var(--edge-0);
    }
    .right {
      grid-area: 1 / 2 / 3 / 3;
    }
  }
  @container view (max-width: 759px) {
    .cols {
      grid-template-columns: minmax(0, 1fr);
      grid-template-rows: 360px 360px 600px;
    }
    .slots,
    .list,
    .right {
      grid-area: auto;
      border-left: 0;
    }
    .list,
    .right {
      border-top: 1px solid var(--edge-0);
    }
  }
  /* column rules as borders, not a solid fill behind a 1px gap: the fill hid the
     background image, and a second surface here stacked on the one .view paints */
  .col {
    display: flex;
    flex-direction: column;
    min-height: 0;
    min-width: 0;
  }
  .col + .col {
    border-left: 1px solid var(--edge-0);
  }
  .chead {
    flex: none;
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 4px 8px;
    min-height: 40px;
    padding: 6px 14px;
  }
  .chead .card-title {
    white-space: nowrap;
  }
  .tools {
    flex: none;
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 0 10px 6px;
    flex-wrap: wrap;
  }
  .tools :global(.select.iv) {
    flex: 1;
    min-width: 0;
    max-width: 200px;
  }
  .grow {
    flex: 1;
  }
  .chk {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    font-size: var(--fs-xs);
    color: var(--ink-2);
    white-space: nowrap;
  }
  .scroll {
    flex: 1;
    overflow-y: auto;
    padding: 0 8px 12px;
  }
  .group {
    margin-bottom: 10px;
  }
  .gtitle {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    margin: 6px 6px 4px;
  }
  .slot {
    display: grid;
    grid-template-columns: 72px minmax(0, 1fr) auto;
    align-items: center;
    gap: 8px;
    min-height: 38px;
    padding: 0 6px;
    border-radius: var(--radius-m);
  }
  .slot:hover {
    background: var(--surface-hover);
  }
  .slot.drop {
    outline: 1px dashed var(--gold);
    outline-offset: -1px;
  }
  .slot.sub {
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
  .page :global(.select.sm.iv) {
    height: 24px;
    font-size: var(--fs-xs);
  }
  /* the slot drop-down is SlotSelect's button: reach it across the component */
  .slot :global(.slotdd) {
    width: 100%;
    min-width: 0;
    height: 28px;
    font-size: var(--fs-xs);
  }
  .row {
    display: flex;
    align-items: center;
    gap: 8px;
    min-height: 34px;
    padding: 0 10px;
    border-radius: var(--radius-m);
    font-size: var(--fs-sm);
    cursor: default;
  }
  .row:hover {
    background: var(--surface-hover);
  }
  .row.sel {
    background: var(--gold-soft);
  }
  .nm {
    flex: 1;
    min-width: 0;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  /* 用在哪裡:固定成右側一欄,方便往下掃 */
  .where {
    flex: none;
    width: 30%;
    max-width: 140px;
    text-align: right;
    font-size: var(--fs-2xs);
    color: var(--ink-3);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .where.unused {
    color: var(--warn);
  }
  .small {
    font-size: var(--fs-2xs);
  }
  .hint {
    flex: none;
    margin: 0;
    padding: 4px 14px 6px;
  }
  .actions {
    flex: none;
    display: flex;
    gap: 6px;
    padding: 8px 10px;
    border-top: 1px solid var(--edge-0);
    flex-wrap: wrap;
  }
  .foot {
    flex: none;
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 6px;
    padding: 8px 10px;
    border-top: 1px solid var(--edge-0);
  }
  .foot .btn {
    justify-content: center;
  }
  .btn.danger:hover:not(:disabled) {
    color: var(--bad);
    border-color: var(--bad);
  }
  .rhead {
    flex: none;
    padding: 4px 16px 0;
    border-bottom: 1px solid var(--edge-0);
  }
  .rhead .tabs {
    flex-wrap: wrap;
    row-gap: 0;
  }
  .rhead .tabs > button {
    font-size: var(--fs-sm);
  }
  .rhead .tabs > button:disabled {
    opacity: var(--dim-opacity);
  }
  .pane {
    display: flex;
    flex-direction: column;
    gap: 8px;
    padding: 12px 16px;
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
    color: var(--ok);
    font-size: var(--fs-xs);
  }
  .edit-note {
    flex: none;
    padding: 6px 16px 0;
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
  .slot.flash {
    outline: 2px solid var(--gold);
    outline-offset: -1px;
  }
  .pane.craft .crow + .crow {
    margin-top: 8px;
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
