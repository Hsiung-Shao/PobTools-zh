  import { untrack } from "svelte";
  import { api, type SpecList } from "$lib/bridge";
  import { copyText } from "$lib/clipboard";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import Select from "$lib/ui/Select.svelte";

  let { compact = false }: { compact?: boolean } = $props();

  let specs = $state<SpecList | null>(null);
  let loadedRev = "";
  let manage = $state(false);
  let manageSel = $state(0);
  let nameDialog = $state<{ op: "new" | "copy" | "rename"; index?: number; title: string } | null>(null);
  let convert = $state<string | null>(null);
  let reset = $state(false);
  let linkIn = $state<{ url: string; title: string; error: string } | null>(null);
  let linkOut = $state<string | null>(null);

  async function reload() {
    try {
      specs = await api.listSpecs();
      loadedRev = app.dataRev;
    } catch {
      specs = null;
    }
  }
  $effect(() => {
    const rev = app.dataRev;
    if (!app.loaded) return;
    if (rev !== loadedRev) untrack(() => void reload());
  });

  async function done(r: SpecList | null | undefined | false) {
    if (!r) return;
    specs = r;
    await app.afterTreeChange();
    loadedRev = app.dataRev;
  }
  const specLabel = (s: SpecList["specs"][number]) =>
    `${s.latest ? "" : `[${s.versionLabel}] `}${s.title || t("tree.specDefault")} (${s.ascendClassNameZh || s.ascendClassName || s.classNameZh || s.className}, ${s.points})`;

  async function pick(v: string) {
    const i = Number(v);
    if (!specs || i === specs.activeSpec) return;
    await done(await app.run(() => api.setActiveSpec(i)));
  }
  async function nameOk() {
    if (!nameDialog || !nameDialog.title.trim()) return;
    const d = nameDialog;
    nameDialog = null;
    await done(await app.run(() => api.specOp({ op: d.op, index: d.index, title: d.title.trim() })));
  }
  async function del(i: number) {
    const s = specs?.specs[i - 1];
    if (!s || !confirm(t("tree.specDeleteConfirm", { name: s.title || t("tree.specDefault") }))) return;
    await done(await app.run(() => api.specOp({ op: "delete", index: i })));
    manageSel = 0;
  }
  async function move(i: number, d: -1 | 1) {
    if (!specs || i + d < 1 || i + d > specs.specs.length) return;
    await done(await app.run(() => api.specOp({ op: "move", index: i, to: i + d })));
    manageSel = i + d;
  }
  async function doConvert(mode: "replace" | "copy" | "all") {
    const v = convert;
    convert = null;
    if (!v) return;
    await done(await app.run(() => api.convertTree(v, { copy: mode === "copy", all: mode === "all" })));
  }
  async function doReset(tattoos: boolean) {
    reset = false;
    const r = await app.run(() => api.resetTree(tattoos));
    if (r) {
      await app.afterTreeChange();
      await reload();
    }
  }
  async function importLink() {
    if (!linkIn) return;
    const d = linkIn;
    try {
      const r = await api.importTreeUrl(d.url, d.title);
      linkIn = null;
      await done(r);
    } catch (e: any) {
      d.error = String(e?.message ?? e);
    }
  }
  // TreeTab's Compare tick + its tree drop-down in one control. What is being
  // compared comes back from POB with the spec list, so leaving the tab and coming
  // back (or switching trees) still shows it.
  const compare = $derived(specs?.compareSpec ?? null);
  async function setCompare(v: string) {
    const i = v ? Number(v) : null;
    const r = await app.run(() => api.setCompareSpec(i ?? undefined));
    if (!r) return;
    await app.afterTreeChange();
    await reload();
    app.treeNonce++; // no recalculation happened, so the revision alone would not redraw the rings
  }

  async function exportLink() {
    const r = await app.run(() => api.exportTreeUrl());
    if (r) linkOut = r.url;
  }
