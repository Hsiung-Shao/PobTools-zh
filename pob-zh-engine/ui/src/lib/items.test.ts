import { describe, expect, it } from "vitest";
import { equippedIn, groupSlots, rarityColor, slotsFor } from "./items";
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
