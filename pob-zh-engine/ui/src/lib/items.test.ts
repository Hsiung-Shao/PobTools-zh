import { describe, expect, it } from "vitest";
import { equippedIn, filterByLoadout, groupSlots, looksLikeItem, looksLikePobRaw, rarityColor, restoreRarity, slotsFor, usedInBadge } from "./items";
import type { ItemSummary } from "./bridge";
import type { ItemSlot } from "./bridge";

const slot = (p: Partial<ItemSlot> & { name: string }): ItemSlot => ({
  label: p.name,
  labelZh: p.name,
  selItemId: 0,
  shown: true,
  inactive: false,
  isFlask: false,
  active: false,
  valid: [],
  ...p,
});

describe("groupSlots", () => {
  it("splits POB's slot list into the blocks the page draws, keeping order and hiding what POB hides", () => {
    const slots = [
      slot({ name: "Weapon 1", weaponSet: 1 }),
      slot({ name: "Weapon 1 Swap", weaponSet: 2, shown: false }),
      slot({ name: "Weapon 1 Abyssal Socket 1", parent: "Weapon 1" }),
      slot({ name: "Weapon 2", weaponSet: 1 }),
      slot({ name: "Helmet" }),
      slot({ name: "Body Armour" }),
      slot({ name: "Amulet" }),
      slot({ name: "Ring 1" }),
      slot({ name: "Ring 3", shown: false }),
      slot({ name: "Belt" }),
      slot({ name: "Flask 1", isFlask: true }),
      slot({ name: "Jewel 100", nodeId: 100 }),
      slot({ name: "Jewel 200", nodeId: 200, shown: false }),
    ];
    const g = groupSlots(slots);
    expect(g.map((x) => x.key)).toEqual(["weapons", "armour", "jewellery", "flasks", "jewels"]);
    expect(g[0].slots.map((s) => s.name)).toEqual(["Weapon 1", "Weapon 1 Abyssal Socket 1", "Weapon 2"]);
    expect(g[2].slots.map((s) => s.name)).toEqual(["Amulet", "Ring 1", "Belt"]);
    expect(g[4].slots.map((s) => s.name)).toEqual(["Jewel 100"]);
  });
});

describe("item <-> slot lookups", () => {
  const slots = [slot({ name: "Ring 1", selItemId: 5, valid: [5, 6] }), slot({ name: "Ring 2", valid: [5], inactive: true }), slot({ name: "Belt", valid: [7] })];
  it("equippedIn finds the slot holding an item", () => expect(equippedIn(slots, 5).map((s) => s.name)).toEqual(["Ring 1"]));
  it("slotsFor uses POB's validity and skips inactive sockets", () => expect(slotsFor(slots, 5).map((s) => s.name)).toEqual(["Ring 1"]));
  it("rarityColor falls back to the base ink", () => {
    expect(rarityColor("UNIQUE")).toContain("--c-unique");
    expect(rarityColor(undefined)).toBe("var(--ink-0)");
  });
});

describe("looksLikeItem", () => {
  it("accepts game item text in English and in the Chinese client", () => {
    expect(looksLikeItem("Rarity: RARE\nDoom Veil\nCobalt Jewel")).toBe(true);
    expect(looksLikeItem("Item Class: Jewels\nRarity: MAGIC\n")).toBe(true);
    expect(looksLikeItem("稀有度: 魔法\n習武的 鈷藍珠寶")).toBe(true);
    expect(looksLikeItem("物品種類: 珠寶\n稀有度: 稀有")).toBe(true);
  });
  it("rejects share codes and prose", () => {
    expect(looksLikeItem("eNrtvQd…")).toBe(false);
    expect(looksLikeItem("the rarity is high")).toBe(false);
  });
});

const RARE_RAW = `New Item
Sinister Quarterstaff
Crafted: true
Prefix: {range:0.87}LocalAddedFireDamageTwoHand7
Prefix: {range:0.73}LocalAddedLightningDamageTwoHand8
Prefix: None
Suffix: {range:0.949}LocalCriticalStrikeChance6
Suffix: {range:0.5}LocalIncreasedAttackSpeed7
Suffix: {range:0.5}EssenceAttackSkillLevel2H1
Item Level: 83
Quality: 20
Sockets: S S S
Rune: Soul Core of Citaqualotl
LevelReq: 67
Implicits: 5
{enchant}{rune}30% increased Elemental Damage with Attacks
{crafted}+3 to Level of all Attack Skills`;

describe("restoreRarity", () => {
  it("restores RARE from three prefix and three suffix slots", () => {
    const r = restoreRarity(RARE_RAW);
    expect(r.rarity).toBe("RARE");
    expect(r.text).toBe("Rarity: RARE\n" + RARE_RAW);
    expect(looksLikeItem(RARE_RAW)).toBe(true);
  });
  it("restores MAGIC from one prefix and one suffix slot", () => {
    const magic = "Hale Sinister Quarterstaff of Haste\nCrafted: true\nPrefix: {range:0.5}LocalIncreasedPhysicalDamage1\nSuffix: None\nItem Level: 40\nLevelReq: 30\nImplicits: 0";
    const r = restoreRarity(magic);
    expect(r.rarity).toBe("MAGIC");
    expect(r.text.startsWith("Rarity: MAGIC\n")).toBe(true);
  });
  it("treats a two-slot list as RARE (rare jewel) and CRLF text the same", () => {
    expect(restoreRarity("Crafted: true\r\nPrefix: None\r\nPrefix: None\r\nSuffix: None\r\nSuffix: None\r\nLevelReq: 1").rarity).toBe("RARE");
  });
  it("does not guess when a 'modifiers allowed' line makes two slots ambiguous", () => {
    const t = "Crafted: true\nPrefix: None\nPrefix: None\nSuffix: None\nLevelReq: 1\n+1 Prefix Modifier allowed";
    expect(restoreRarity(t)).toEqual({ text: t, rarity: null });
    expect(restoreRarity(t + "\nPrefix: None").rarity).toBe("RARE");
  });
  it("gives null for POB raw text without Prefix/Suffix lines", () => {
    const t = "Some Unique\nCoral Ring\nItem Level: 80\nLevelReq: 50\nImplicits: 1\n+10 to Strength";
    expect(looksLikePobRaw(t)).toBe(true);
    expect(restoreRarity(t)).toEqual({ text: t, rarity: null });
    expect(looksLikeItem(t)).toBe(false);
  });
  it("leaves text that already has a rarity line alone", () => {
    const t = "Rarity: UNIQUE\n" + RARE_RAW;
    expect(restoreRarity(t)).toEqual({ text: t, rarity: null });
    expect(looksLikePobRaw(t)).toBe(false);
    expect(looksLikeItem(t)).toBe(true);
  });
  it("leaves Chinese game copy alone", () => {
    const t = "稀有度: 稀有\n末日面紗\n鈷藍珠寶\nPrefix: x\nSuffix: y";
    expect(restoreRarity(t)).toEqual({ text: t, rarity: null });
    expect(looksLikeItem(t)).toBe(true);
  });
  it("rejects random text and a lone key", () => {
    for (const t of ["eNrtvQd…", "the rarity is high", "Prefix: something\nand prose", "hello\nworld"]) {
      expect(restoreRarity(t)).toEqual({ text: t, rarity: null });
      expect(looksLikeItem(t)).toBe(false);
    }
  });
});

const item = (p: Partial<ItemSummary> & { id: number }): ItemSummary => ({
  name: `item ${p.id}`,
  nameZh: `item ${p.id}`,
  rarity: "RARE",
  unsupported: false,
  corrupted: false,
  sockets: [],
  influences: [],
  clusterJewel: false,
  ...p,
});

describe("filterByLoadout / usedInBadge", () => {
  const items = [
    item({ id: 1, usedIn: { kind: "slot", slot: "Helmet", otherSet: false } }),
    item({ id: 2, usedIn: { kind: "slot", slot: "Helmet", setId: 2, setTitle: "Boss", otherSet: true } }),
    item({ id: 3 }),
    item({ id: 4, usedIn: { kind: "jewel", specTitle: "Default", otherSet: false } }),
    item({ id: 5, usedIn: { kind: "abyss", setTitle: "Boss", otherSet: true } }),
  ];
  it("any keeps everything, unused keeps items with no usedIn, current keeps the active set's items", () => {
    expect(filterByLoadout(items, "any", 1).map((i) => i.id)).toEqual([1, 2, 3, 4, 5]);
    expect(filterByLoadout(items, "unused", 1).map((i) => i.id)).toEqual([3]);
    expect(filterByLoadout(items, "current", 1).map((i) => i.id)).toEqual([1, 4]);
    expect(filterByLoadout(items, { setId: 2 }, 1).map((i) => i.id)).toEqual([2]);
  });
  it("badges: unused, used elsewhere (with the set/tree title), or nothing when used in the active set", () => {
    expect(usedInBadge(items[0].usedIn)).toBeNull();
    expect(usedInBadge(items[1].usedIn)).toEqual({ kind: "elsewhere", where: "Boss" });
    expect(usedInBadge(undefined)).toEqual({ kind: "unused" });
    expect(usedInBadge(items[3].usedIn)).toBeNull();
    expect(usedInBadge(items[4].usedIn)).toEqual({ kind: "elsewhere", where: "Boss" });
  });
});
