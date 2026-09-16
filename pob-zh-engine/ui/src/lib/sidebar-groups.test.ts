import { describe, expect, it } from "vitest";
import { groupOf, groupSidebar } from "./sidebar-groups";
import type { SidebarRow } from "./bridge";

describe("groupOf: POB stat keys land in the section a player expects", () => {
  const cases: [string, ReturnType<typeof groupOf>][] = [
    ["TotalDPS", "offence"],
    ["CombinedDPS", "offence"],
    ["AverageHit", "offence"],
    ["HitChance", "offence"],
    ["CritChance", "offence"],
    ["Speed", "offence"],
    ["ReservationDPS", "offence"],
    ["ManaCost", "skill"],
    ["LifePerSecondCost", "skill"],
    ["Cooldown", "skill"],
    ["AreaOfEffectRadiusMetres", "skill"],
    ["ActiveMinionLimit", "skill"],
    ["Str", "attributes"],
    ["ReqInt", "attributes"],
    ["Life", "resources"],
    ["Spec:LifeInc", "resources"],
    ["ManaUnreservedPercent", "resources"],
    ["EnergyShieldRegenRecovery", "resources"],
    ["LifeLeechGainRate", "resources"],
    ["NetLifeRegen", "resources"],
    ["TotalBuildDegen", "resources"],
    ["TotalEHP", "mitigation"],
    ["FireMaximumHitTaken", "mitigation"],
    ["PvPTotalTakenHit", "mitigation"],
    ["Armour", "mitigation"],
    ["Spec:ArmourInc", "mitigation"],
    ["EffectiveSpellSuppressionChance", "mitigation"],
    ["FireResist", "resistances"],
    ["ChaosResistOverCap", "resistances"],
    ["EffectiveMovementSpeedMod", "misc"],
    ["PresenceRadiusMetres", "misc"],
    ["FullDPS", "fulldps"],
    ["SkillDPS", "fulldps"],
  ];
  for (const [k, g] of cases) it(`${k} -> ${g}`, () => expect(groupOf(k)).toBe(g));
});

describe("groupSidebar", () => {
  const row = (p: Partial<SidebarRow>): SidebarRow => ({ h: 16, hasBreakdown: false, ...p });

  it("keeps POB's 1-based row index, folds spacers, drops actor headings", () => {
    const rows = [
      row({ lhs: "Skill info" }),               // 1: untagged heading-only before any stat -> dropped
      row({}),                                   // 2: spacer
      row({ lhs: "Life:", rhs: "100", stat: "Life", actor: "player" }), // 3
      row({}),                                   // 4
      row({}),                                   // 5 (double spacer)
      row({ lhs: "Mana:", rhs: "50", stat: "Mana", actor: "player" }),  // 6
      row({ lhs: "Minion:" }),                   // 7: heading -> dropped
      row({ lhs: "Minion DPS:", rhs: "9", stat: "TotalDPS", actor: "minion" }), // 8
      row({ lhs: "Fire Res:", rhs: "75%", stat: "FireResist", actor: "player" }), // 9
    ];
    const s = groupSidebar(rows);
    expect(s.map((x) => x.key)).toEqual(["minion", "resources", "resistances"]);
    const res = s.find((x) => x.key === "resources")!;
    expect(res.items.map((i) => i.index)).toEqual([3, 4, 6]);
    expect(s.find((x) => x.key === "minion")!.items[0].index).toBe(8);
  });

  it("an unkeyed row after a keyed one stays in that section", () => {
    const rows = [
      row({ lhs: "DPS:", rhs: "1", stat: "TotalDPS", actor: "player" }),
      row({ lhs: "note", align: "CENTER_X", actor: "player" }),
    ];
    const s = groupSidebar(rows);
    expect(s).toHaveLength(1);
    expect(s[0].items).toHaveLength(2);
  });
});
