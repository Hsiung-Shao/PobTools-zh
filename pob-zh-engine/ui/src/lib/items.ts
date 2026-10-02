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

const RARITY_LINE = /^(Rarity|Item Class|稀有度|物品種類|物品类别)\s*[:：]/m;

/** Keys only POB's own raw item writer produces (ItemClass:BuildRaw), never the game's copy format. */
const POB_RAW_KEYS = /^(Prefix|Suffix|Implicits|LevelReq|Item Level|Crafted|Quality|Sockets|Rune|Catalyst|CatalystQuality|Unique ID|League)\s*:/gm;

/** True when the text has no rarity line but carries at least two distinct POB-raw keys. */
export function looksLikePobRaw(s: string): boolean {
  if (RARITY_LINE.test(s)) return false;
  const keys = new Set<string>();
  for (const m of s.matchAll(POB_RAW_KEYS)) keys.add(m[1]);
  return keys.size >= 2;
}

/**
 * POB's own item text always starts with "Rarity: X" (BuildRaw, Item.lua), but a
 * copy can lose that first line; POB then reads the item as UNIQUE. When the line is
 * missing from text that is clearly POB-raw, work the rarity out from the affix-slot
 * lines BuildRaw writes (only for crafted items, only MAGIC and RARE):
 *   - Item.lua:1292-1307 sizes the slots: MAGIC affixLimit 2 (1 prefix + 1 suffix),
 *     RARE 6 (3+3; jewels 4 = 2+2). "+N prefix/suffix modifiers allowed" lines
 *     (Item.lua:966/968) move a list's limit, and a MAGIC list is capped at 2.
 *   - Item.lua:1432-1437 writes one "Prefix:" / "Suffix:" line per slot (None included).
 * So three or more slots of one kind is RARE; two is RARE unless a "modifiers allowed"
 * line could have widened a MAGIC item (then it is ambiguous); at most one of each is
 * MAGIC unless such a line is present. No slot lines at all (or ambiguous) gives null:
 * the rarity cannot be known and is not guessed.
 */
export function restoreRarity(text: string): { text: string; rarity: string | null } {
  if (!looksLikePobRaw(text)) return { text, rarity: null };
  const prefixes = (text.match(/^Prefix\s*:/gm) ?? []).length;
  const suffixes = (text.match(/^Suffix\s*:/gm) ?? []).length;
  if (prefixes + suffixes === 0) return { text, rarity: null };
  const widened = /[+-]\d+\s+(prefix|suffix)\s+modifiers?\s+allowed/i.test(text);
  const most = Math.max(prefixes, suffixes);
  let rarity: string | null;
  if (most >= 3) rarity = "RARE";
  else if (widened) rarity = null;
  else rarity = most >= 2 ? "RARE" : "MAGIC";
  if (!rarity) return { text, rarity: null };
  return { text: "Rarity: " + rarity + "\n" + text, rarity };
}

/** Pasted text that starts like an item (the rarity or item-class line), in either client language, or POB-raw text whose rarity line can be restored. */
export function looksLikeItem(s: string): boolean {
  return RARITY_LINE.test(s) || restoreRarity(s).rarity !== null;
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
