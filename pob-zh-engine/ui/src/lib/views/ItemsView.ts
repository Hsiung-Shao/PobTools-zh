  import { untrack } from "svelte";
  import TreeSpecBar from "../components/TreeSpecBar.svelte";
  import SharedItemsDialog from "$lib/ui/SharedItemsDialog.svelte";
  import TradeDialog from "$lib/ui/TradeDialog.svelte";
  import { api, type CraftOptions, type ItemEditState, type ItemSlot, type ItemSummary, type ItemsList, type ItemTooltip } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { equippedIn, filterByLoadout, groupSlots, itemById, looksLikeItem, looksLikePobRaw, rarityColor, restoreRarity, resumeEdit, slotsFor, usedInBadge, type LoadoutFilter } from "$lib/items";
  import { app } from "$lib/state.svelte";
  import { dataKey } from "$lib/lang.svelte";
  import { copyText } from "$lib/clipboard";
  import TooltipCard from "$lib/ui/TooltipCard.svelte";
  import SlotSelect from "$lib/ui/SlotSelect.svelte";
  import ItemEditor from "$lib/ui/ItemEditor.svelte";
  import ItemDb from "$lib/ui/ItemDb.svelte";
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
