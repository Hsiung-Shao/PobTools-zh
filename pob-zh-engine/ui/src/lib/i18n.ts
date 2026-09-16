// Strings the page itself owns (~30% of what is on screen; the rest is POB
// text already translated by the engine's dictionaries). Keyed, with the
// English built in as the fallback and the other locales loaded from
// Data\launcher\<locale>\ui.json through the host's data.pobtools mapping --
// the same folder the launcher's own strings live in, so the signed
// translation-data line can update them without an app release.
import { hostInfo, isHosted } from "./bridge";

const EN: Record<string, string> = {
  "app.title": "PobTools",
  "app.booting": "Starting the POB engine (loading POB and the dictionaries)…",
  "app.childExited": "The POB engine process ended (exit {code}).",
  "app.restartEngine": "Restart engine",
  "app.gateFailed": "This POB version is not compatible with the new interface: {failed}",
  "app.gateOk": "Compatible",
  "builds.title": "Builds",
  "builds.empty": "No builds in {path}",
  "builds.openFolder": "Open folder",
  "builds.refresh": "Refresh",
  "builds.level": "Lv",
  "builds.loading": "Loading…",
  "sidebar.build": "Build",
  "sidebar.class": "Class",
  "sidebar.ascendancy": "Ascendancy",
  "sidebar.none": "None",
  "sidebar.level": "Level",
  "sidebar.points": "Points",
  "sidebar.asc": "asc",
  "sidebar.warnings": "Warnings",
  "sidebar.noBuild": "No build loaded",
  "sidebar.breakdown": "Breakdown",
  "sidebar.pinned": "pinned",
  "breakdown.radius": "Area of effect radius: {radius}",
  "breakdown.none": "No breakdown available.",
  "group.info": "",
  "group.minion": "Minion",
  "group.offence": "Offence",
  "group.skill": "Skill",
  "group.attributes": "Attributes",
  "group.resources": "Resources",
  "group.mitigation": "Mitigation",
  "group.resistances": "Resistances",
  "group.misc": "Misc",
  "group.fulldps": "Full DPS",
  "status.engine": "engine",
  "status.pob": "PoB",
  "status.updateAvailable": "POB update available",
  "status.updateApply": "Apply update",
  "status.updateChecking": "checking for POB updates…",
  "status.updateNone": "POB is up to date",
  "status.updateBlocked": "PobTools app updates wait while this window is open",
  "status.rev": "rev",
  "title.builds": "Builds",
  "title.tree": "Tree",
  "title.unsaved": "unsaved",
  "tree.loading": "Loading tree…",
  "tree.artMissing": "Some tree art files were not found; showing a wireframe",
  "tree.search": "Search nodes…",
  "tree.fit": "Fit",
  "tree.undo": "Undo",
  "tree.redo": "Redo",
  "tree.allocated": "allocated",
  "tree.clickRemove": "click to remove",
  "tree.clickRemoveN": "click removes {n}",
  "tree.clickAllocate": "click to allocate",
  "tree.pointsN": "{n} point(s)",
  "tree.chooseEffect": "click to choose an effect",
  "tree.unreachable": "not reachable",
  "tree.masteryTitle": "Choose a mastery effect",
  "tree.masteryTaken": "taken by another mastery of this kind",
  "tree.cancel": "Cancel",
  "tree.classChangeTitle": "Class change",
  "tree.classChangeBody": "Allocating this node switches your class to {className}. Reset the tree, or connect a path to keep it?",
  "tree.classChangeConnect": "Connect a path",
  "tree.classChangeReset": "Reset tree and switch",
  "tree.kind.normal": "Passive",
  "tree.kind.notable": "Notable",
  "tree.kind.keystone": "Keystone",
  "tree.kind.socket": "Jewel socket",
  "tree.kind.mastery": "Mastery",
  "tree.kind.classStart": "Class start",
  "tree.kind.ascStart": "Ascendancy start",
};

let table: Record<string, string> = { ...EN };
let ready: Promise<void> | null = null;

export function locale(): string {
  return hostInfo.locale || "en";
}

/** Loads Data\launcher\<locale>\ui.json (silently keeps English when absent). */
export function loadLocale(): Promise<void> {
  if (ready) return ready;
  const loc = locale();
  if (!isHosted || loc === "en") return (ready = Promise.resolve());
  ready = fetch(`https://${hostInfo.hosts.data}/launcher/${loc}/ui.json`)
    .then((r) => (r.ok ? r.json() : null))
    .then((j) => {
      if (j && typeof j === "object") table = { ...EN, ...(j as Record<string, string>) };
    })
    .catch(() => {});
  return ready;
}

export function t(key: string, vars?: Record<string, string | number>): string {
  let s = table[key] ?? EN[key] ?? key;
  if (vars) for (const [k, v] of Object.entries(vars)) s = s.replace(`{${k}}`, String(v));
  return s;
}
