import { describe, expect, it } from "vitest";
import { groupOf, groupSidebar, headlineStat } from "./sidebar-groups";
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

describe("headlineStat: 側欄主數字", () => {
  const row = (p: Partial<SidebarRow>): SidebarRow => ({ h: 16, hasBreakdown: false, ...p });
  const p = (stat: string, rhs = "1") => row({ lhs: `${stat}:`, rhs, stat, actor: "player" });

  it("有完整 DPS 列時取它當主數字", () => {
    const rows = [
      row({ lhs: "^7Hit DPS:", rhs: "^77,031,006.2", stat: "TotalDPS", actor: "player" }),
      p("CombinedDPS", "^79,000,000"),
      row({ lhs: "^xAA9E82Full DPS:", rhs: "^710,493,990.4", stat: "FullDPS", actor: "player" }),
    ];
    // 索引沿用 groupSidebar 的 1-based(第 3 列)
    expect(headlineStat(rows)).toEqual({ index: 3, lhs: "^xAA9E82Full DPS:", rhs: "^710,493,990.4" });
  });

  it("沒有完整 DPS 時依序退到下一個", () => {
    const a = [p("TotalDPS", "1"), p("CombinedDPS", "2")];
    expect(headlineStat(a)).toEqual({ index: 2, lhs: "CombinedDPS:", rhs: "2" });
    const b = [p("Life", "9"), p("TotalDPS", "3")];
    expect(headlineStat(b)).toEqual({ index: 2, lhs: "TotalDPS:", rhs: "3" });
    const c = [p("Life", "9"), p("TotalDot", "4")];
    expect(headlineStat(c)).toEqual({ index: 2, lhs: "TotalDot:", rhs: "4" });
  });

  it("沒有任何傷害列時不顯示主數字", () => {
    expect(headlineStat([p("Life"), p("Mana")])).toBeNull();
    expect(headlineStat([])).toBeNull();
    expect(headlineStat([p("FullDPS", "")])).toBeNull();
  });

  it("召喚物的 DPS 列不會蓋過玩家的", () => {
    const rows = [
      row({ lhs: "Minion Full DPS:", rhs: "999", stat: "FullDPS", actor: "minion" }),
      row({ lhs: "Hit DPS:", rhs: "7", stat: "TotalDPS", actor: "player" }),
    ];
    expect(headlineStat(rows)).toEqual({ index: 2, lhs: "Hit DPS:", rhs: "7" });
  });
});
