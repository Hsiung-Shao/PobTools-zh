// Pure helpers for the items page: the slot grid layout and rarity colours.
import type { ItemSlot, ItemSummary, ItemUsedIn } from "./bridge";

export const RARITY_COLOR: Record<string, string> = {
  NORMAL: "var(--c-normal)",
  MAGIC: "var(--c-magic)",
  RARE: "var(--c-rare)",
  UNIQUE: "var(--c-unique)",
  RELIC: "var(--c-relic, #60c060)",
};

export function rarityColor(r: string | undefined): string {
  return (r && RARITY_COLOR[r]) || "var(--ink-0)";
}

export interface SlotGroup {
  key: "weapons" | "armour" | "jewellery" | "flasks" | "abyssal" | "jewels";
  slots: ItemSlot[];
}

/**
 * The slot grid as POB shows it: only shown slots, in POB's own order, split
 * into the groups the page draws as blocks. Abyssal sockets follow their
 * parent, jewel sockets (tree) come last.
 */
export function groupSlots(slots: ItemSlot[]): SlotGroup[] {
  const weapons: ItemSlot[] = [];
  const armour: ItemSlot[] = [];
  const jewellery: ItemSlot[] = [];
  const flasks: ItemSlot[] = [];
  const jewels: ItemSlot[] = [];
  const abyssalByParent = new Map<string, ItemSlot[]>();
  for (const s of slots) {
    if (!s.shown) continue;
    if (s.parent) {
      let a = abyssalByParent.get(s.parent);
      if (!a) abyssalByParent.set(s.parent, (a = []));
      a.push(s);
      continue;
    }
    if (s.nodeId != null) jewels.push(s);
    else if (s.name.startsWith("Weapon")) weapons.push(s);
    else if (s.isFlask) flasks.push(s);
    else if (/^(Amulet|Ring|Belt)/.test(s.name)) jewellery.push(s);
    else armour.push(s);
  }
  const withAbyssal = (list: ItemSlot[]) => list.flatMap((s) => [s, ...(abyssalByParent.get(s.name) ?? [])]);
  const out: SlotGroup[] = [];
  if (weapons.length) out.push({ key: "weapons", slots: withAbyssal(weapons) });
  if (armour.length) out.push({ key: "armour", slots: withAbyssal(armour) });
  if (jewellery.length) out.push({ key: "jewellery", slots: withAbyssal(jewellery) });
  if (flasks.length) out.push({ key: "flasks", slots: flasks });
  if (jewels.length) out.push({ key: "jewels", slots: jewels });
  return out;
}

/** Slot label -> the item equipped there, for the "equipped in" badge. */
export function equippedIn(slots: ItemSlot[], itemId: number): ItemSlot[] {
  return slots.filter((s) => s.shown && s.selItemId === itemId);
}

/** Slots this item may go into (POB decided per slot via `valid`). */
export function slotsFor(slots: ItemSlot[], itemId: number): ItemSlot[] {
  return slots.filter((s) => s.shown && !s.inactive && s.valid.includes(itemId));
}

export function itemById(items: ItemSummary[], id: number): ItemSummary | undefined {
  return items.find((i) => i.id === id);
}

/** Pasted text that starts like an item (the rarity or item-class line), in either client language. */
export function looksLikeItem(s: string): boolean {
  return /^(Rarity|Item Class|稀有度|物品種類|物品类别)\s*[:：]/m.test(s);
}

/** The list's loadout filter (ItemListControl's dropdown): any / current set / unused / one item set. */
export type LoadoutFilter = "any" | "current" | "unused" | { setId: number };

export function filterByLoadout(items: ItemSummary[], f: LoadoutFilter, activeSetId: number): ItemSummary[] {
  if (f === "any") return items;
  return items.filter((it) => {
    const u = it.usedIn;
    if (f === "unused") return !u;
    if (!u) return false;
    if (f === "current") return u.kind !== "slot" ? !u.otherSet : (u.setId ?? activeSetId) === activeSetId;
    return u.kind === "slot" && (u.setId ?? activeSetId) === f.setId;
  });
}

/** What the list shows after the name: nothing (used here), "(Unused)" or "(Used in 'X')". */
export function usedInBadge(u: ItemUsedIn | null | undefined): { kind: "unused" } | { kind: "elsewhere"; where: string } | null {
  if (!u) return { kind: "unused" };
  if (u.kind === "abyss" || u.kind === "jewel") return u.otherSet ? { kind: "elsewhere", where: u.setTitle ?? u.specTitle ?? "" } : null;
  return u.otherSet && u.setTitle ? { kind: "elsewhere", where: u.setTitle } : null;
}
