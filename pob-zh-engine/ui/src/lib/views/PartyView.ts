  import { untrack } from "svelte";
  import { api, type PartyData, type PartyImportState } from "$lib/bridge";
  import PobText from "$lib/ui/PobText.svelte";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import { dataKey } from "$lib/lang.svelte";
  import Select from "$lib/ui/Select.svelte";

  type FieldKey = keyof PartyData["fields"];
  const keys: FieldKey[] = ["partyMemberStats", "aura", "curse", "warcry", "link", "enemyCond", "enemyMods"];

  let data = $state<PartyData | null>(null);
  let drafts = $state<Record<string, string>>({});
  let loadedRev = "";

  async function reload() {
    const r = await app.run(() => api.getParty());
    if (r) {
      // a field still being typed in (not yet applied) keeps its text; the rest
      // take POB's value (a re-read after F2 or another change must not wipe it)
      const prev = data?.fields;
      const d: Record<string, string> = {};
      for (const k of keys) d[k] = prev && drafts[k] !== (prev[k] ?? "") ? drafts[k] : (r.fields[k] ?? "");
      data = r;
      loadedRev = dataKey(r.rev, app.langRev);
      drafts = d;
    }
  }
  $effect(() => {
    const rev = app.dataRev;
    if (!app.loaded) return;
    if (rev !== loadedRev) untrack(() => void reload());
  });

  async function apply(k: FieldKey) {
    if (!data || drafts[k] === data.fields[k]) return;
    const r = await app.run(() => api.setParty(k, drafts[k]));
    if (r) {
      await reload();
      await app.afterTreeChange();
    }
  }
  // PartyTab's import row: a party member's code or build-site URL into one list or all
  let imp = $state<PartyImportState | null>(null);
  let code = $state("");
  let dest = $state(1);
  let append = $state(false);
  let impMsg = $state("");
  // read on load, and again on F2 (its labels come translated; the choices made here stay)
  let impLang = -1;
  $effect(() => {
    const lr = app.langRev;
    if (!app.loaded) return;
    const again = impLang !== -1;
    impLang = lr;
    untrack(async () => {
      try {
        imp = await api.partyImportState();
        if (!again) {
          dest = imp.destination || 1;
          append = imp.append;
        }
      } catch {
        imp = null;
      }
    });
  });
  async function doImport() {
    impMsg = "";
    let r: PartyImportState;
    try {
      r = await api.partyImport(code.trim(), dest, append);
    } catch (e: any) {
      impMsg = String(e?.message ?? e);
      return;
    }
    // a build-site URL downloads first; POB finishes the import when it arrives
    for (let i = 0; r.fetching && i < 60; i++) {
      await new Promise((res) => setTimeout(res, 500));
      r = await api.partyImportState();
    }
    imp = r;
    impMsg = r.fetching ? "" : r.detailZh || r.detail;
    if (!r.fetching) {
      code = "";
      await reload();
      await app.afterTreeChange();
    }
  }
  async function action(a: "clear" | "disable" | "rebuild") {
    if (a === "clear" && !confirm(t("party.clearConfirm"))) return;
    const r = await app.run(() => api.partyAction(a));
    if (r) {
      await reload();
      await app.afterTreeChange();
    }
  }

  async function toggleExport(v: boolean) {
    const r = await app.run(() => api.setPartyExport(v));
    if (r) {
      await reload();
      await app.afterTreeChange();
    }
  }
