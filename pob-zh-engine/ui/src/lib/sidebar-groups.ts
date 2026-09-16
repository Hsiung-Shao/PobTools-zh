// POB's sidebar is one long list; we fold it into sections by the stat key
// the bridge tags each row with. Keys are classified by what they are named
// (POB's stat names are regular: *Cost, *Resist, *MaximumHitTaken, ...), with
// a short table for the few that read differently from what they mean.
// Rows POB adds without a key (skill info, spacers) stay with the section
// they arrived in; minion rows form one section of their own.
import type { SidebarRow } from "./bridge";
import { t } from "./i18n";

export type StatGroup = "offence" | "skill" | "attributes" | "resources" | "mitigation" | "resistances" | "misc" | "fulldps";

/** Section order on screen. */
export const GROUP_ORDER: StatGroup[] = ["offence", "skill", "attributes", "resources", "mitigation", "resistances", "misc", "fulldps"];

/** Keys whose name would land them in the wrong family. */
const EXPLICIT: Record<string, StatGroup> = {
  ReservationDPS: "offence",
  ActiveMinionLimit: "skill",
  BrandTicks: "skill",
  TimeMaxSeals: "skill",
  Devotion: "attributes",
  Tribute: "attributes",
  TotalBuildDegen: "resources",
  TotalNetRegen: "resources",
  PresenceRadiusMetres: "misc",
  EffectiveMovementSpeedMod: "misc",
  MovementSpeedWhileUsingSkill: "misc",
};

/** Name families, first match wins. */
const FAMILIES: [RegExp, StatGroup][] = [
  [/^(FullDPS|FullDotDPS|SkillDPS)$/, "fulldps"],
  [/Resist/, "resistances"],
  [/^(Req)?(Str|Dex|Int)$/, "attributes"],
  [/Cost$|Cooldown|Seal|Radius|Metre|Brand/, "skill"],
  [/EHP|TakenHit|HitTaken|Evasion|Evade|Deflect|Armour|DamageReduction|Block|Dodge|Suppression/, "mitigation"],
  [/^(Life|Mana|EnergyShield|Ward|Rage|Spirit|Darkness|ReservedDarkness)|Regen|Leech|Degen|^Spec:(Life|Mana|EnergyShield)Inc$/, "resources"],
];

export function groupOf(stat: string): StatGroup {
  const e = EXPLICIT[stat];
  if (e) return e;
  for (const [re, g] of FAMILIES) if (re.test(stat)) return g;
  return "offence";
}

export interface SidebarItem {
  row: SidebarRow;
  /** 1-based position in POB's list; sidebar_breakdown addresses rows by it. */
  index: number;
}

export interface SidebarSection {
  key: string;
  label: string | null;
  items: SidebarItem[];
}

const isSpacer = (r: SidebarRow) => !r.lhs && !r.rhs;
const isHeadingOnly = (r: SidebarRow) => !!r.lhs && !r.rhs && !r.align;

/** Drops leading/trailing spacers and collapses runs of them. */
function tidy(items: SidebarItem[]): SidebarItem[] {
  const out: SidebarItem[] = [];
  for (const it of items) {
    if (isSpacer(it.row) && (out.length === 0 || isSpacer(out[out.length - 1].row))) continue;
    out.push(it);
  }
  while (out.length && isSpacer(out[out.length - 1].row)) out.pop();
  return out;
}

export function groupSidebar(rows: SidebarRow[]): SidebarSection[] {
  const lead: SidebarItem[] = [];
  const minion: SidebarItem[] = [];
  const bySection = new Map<StatGroup, SidebarItem[]>();
  let current: StatGroup | null = null;
  const push = (g: StatGroup, it: SidebarItem) => {
    let b = bySection.get(g);
    if (!b) bySection.set(g, (b = []));
    b.push(it);
  };

  rows.forEach((row, i) => {
    const it = { row, index: i + 1 };
    if (row.actor === "minion") {
      minion.push(it);
      return;
    }
    if (row.stat) {
      current = groupOf(row.stat);
      push(current, it);
      return;
    }
    // "Minion:" / "Player:" headings POB inserts: the section label says it.
    if (!row.actor && isHeadingOnly(row)) return;
    if (current) push(current, it);
    else lead.push(it);
  });

  const out: SidebarSection[] = [];
  const leadRows = tidy(lead);
  if (leadRows.length) out.push({ key: "lead", label: null, items: leadRows });
  const minionRows = tidy(minion);
  if (minionRows.length) out.push({ key: "minion", label: t("group.minion"), items: minionRows });
  for (const g of GROUP_ORDER) {
    const items = tidy(bySection.get(g) ?? []);
    if (items.length) out.push({ key: g, label: t(`group.${g}`), items });
  }
  return out;
}
