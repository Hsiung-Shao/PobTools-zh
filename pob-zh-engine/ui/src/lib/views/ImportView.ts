  import { onDestroy, onMount, untrack } from "svelte";
  import { api, type AccountChar, type CodeInfo, type ImportStatus } from "$lib/bridge";
  import { classifyClipboard, looksLikeBuildUrl, looksLikeShareCode, shouldAutoFill } from "$lib/buildCode";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import PobText from "$lib/ui/PobText.svelte";
  import Select from "$lib/ui/Select.svelte";
  import { copyText } from "$lib/clipboard";

  // --- account import ------------------------------------------------------
  let st = $state<ImportStatus | null>(null);
  let pollTimer = 0;
  let lastImported = -1;
  let doneMsg = $state<string | null>(null);
  let oRealm = $state("PC");
  let oLeague = $state("");
  let oChar = $state("");
  let oDeleteJewels = $state(true);
  let oClearItems = $state(true);
  let oClearSkills = $state(true);
  let oIgnoreSwap = $state(false);
  let sRealm = $state("PC");
  let sAccount = $state("");
  let sLeague = $state("");
  let sChar = $state("");
  let sDeleteJewels = $state(true);
  let sClearItems = $state(true);
  let sClearSkills = $state(true);
  let sIgnoreSwap = $state(false);
  let seeded = false;

  const oChars = $derived((st && st.characters[realmCode(oRealm)]) ?? []);
  const leagueOpts = (l: string[]) => [{ value: "", label: t("import.anyLeague") }, ...l.map((x) => ({ value: x, label: x }))];
  const oLeagues = $derived([...new Set(oChars.map((c) => c.league).filter((x): x is string => !!x))].sort());
  const oFiltered = $derived(oChars.filter((c) => !oLeague || c.league === oLeague));
  const sChars = $derived(st?.site.characters ?? []);
  const sLeagues = $derived([...new Set(sChars.map((c) => c.league).filter((x): x is string => !!x))].sort());
  const sFiltered = $derived(sChars.filter((c) => !sLeague || c.league === sLeague));
  const busyOauth = $derived(!!st && (st.oauth.loading || (!st.authorized && st.oauth.timer != null)));
  const busySite = $derived(!!st && (st.site.mode === "DOWNLOADCHARLIST" || st.site.mode === "IMPORTING"));
  const authWait = $derived(st && !st.authorized && st.oauth.timer != null ? Math.max(0, st.oauth.timer + 60 - st.oauth.now) : 0);
  const rateWait = $derived(st?.oauth.rateLimitEnd ? Math.max(0, st.oauth.rateLimitEnd - st.oauth.now) : 0);

  function realmCode(id: string): string {
    return st?.realms.find((r) => r.id === id)?.realmCode ?? id.toLowerCase();
  }
  function charLabel(c: AccountChar): string {
    return `${c.name}  ·  ${c.classZh || c.class || "?"}  Lv ${c.level ?? "?"}  ·  ${c.league ?? "?"}`;
  }

  async function poll() {
    try {
      const s = await api.importStatus();
      st = s;
      if (!seeded) {
        seeded = true;
        if (s.lastRealm) {
          oRealm = s.lastRealm;
          sRealm = s.lastRealm;
        }
        if (s.lastAccountName) sAccount = s.lastAccountName;
        if (s.site.accountName) sAccount = s.site.accountName;
        lastImported = s.imported;
      }
      if (s.imported !== lastImported) {
        lastImported = s.imported;
        doneMsg = s.lastImport ? t(`import.done.${s.lastImport}`) : null;
        await app.afterReinit();
      } else if (s.recalculated) {
        await app.refresh();
      }
      // PoE2's POB has one realm ("PoE2"); fall back to the first it lists
      if (s.realms.length && !s.realms.some((r) => r.id === oRealm)) oRealm = s.realms[0].id;
      if (s.realms.length && !s.realms.some((r) => r.id === sRealm)) sRealm = s.realms[0].id;
      if (!oFiltered.some((c) => c.name === oChar)) oChar = oFiltered[0]?.name ?? "";
      if (!sFiltered.some((c) => c.name === sChar)) sChar = sFiltered[0]?.name ?? "";
    } catch (e: any) {
      app.error = String(e?.message ?? e);
    }
    clearTimeout(pollTimer);
    const active = st && (busyOauth || busySite || authWait > 0);
    pollTimer = window.setTimeout(poll, active ? 700 : 4000);
  }
  $effect(() => {
    if (!app.loaded) return;
    untrack(() => void poll());
  });
  onDestroy(() => clearTimeout(pollTimer));

  async function authorize() {
    doneMsg = null;
    const r = await app.run(() => api.oauthStart());
    if (r) await poll();
  }
  async function logout() {
    await app.run(() => api.oauthLogout());
    await poll();
  }
  async function fetchOauth() {
    doneMsg = null;
    const r = await app.run(() => api.fetchCharacters({ source: "oauth", realm: oRealm }));
    if (r) await poll();
  }
  async function importOauth(what: "tree" | "items") {
    if (!oChar) return;
    if (what === "tree" && st?.hasPoints && !confirm(t("import.treeOverwrite"))) return;
    doneMsg = null;
    const r = await app.run(() =>
      api.importAccountCharacter({
        source: "oauth",
        realm: oRealm,
        name: oChar,
        league: oLeague || undefined,
        what,
        deleteJewels: oDeleteJewels,
        clearItems: oClearItems,
        clearSkills: oClearSkills,
        ignoreWeaponSwap: oIgnoreSwap,
      }),
    );
    if (r) await poll();
  }
  async function startSite() {
    doneMsg = null;
    const r = await app.run(() => api.fetchCharacters({ source: "site", realm: sRealm, accountName: sAccount.trim() }));
    if (r) await poll();
  }
  async function importSite(what: "tree" | "items") {
    if (!sChar) return;
    if (what === "tree" && st?.hasPoints && !confirm(t("import.treeOverwrite"))) return;
    doneMsg = null;
    const r = await app.run(() =>
      api.importAccountCharacter({
        source: "site",
        realm: sRealm,
        name: sChar,
        league: sLeague || undefined,
        what,
        deleteJewels: sDeleteJewels,
        clearItems: sClearItems,
        clearSkills: sClearSkills,
        ignoreWeaponSwap: sIgnoreSwap,
      }),
    );
    if (r) await poll();
  }
  async function closeSite() {
    await app.run(() => api.importSiteReset());
    await poll();
  }
  const siteReady = $derived(st?.site.mode === "SELECTCHAR" || st?.site.mode === "IMPORTING");
  const accountOk = $derived(/\S[#-]\d{4}$/.test(sAccount.trim()));

  let code = $state("");
  let preview = $state<CodeInfo | null>(null);
  let previewErr = $state<string | null>(null);
  let decodeTimer = 0;

  let exported = $state("");
  let copied = $state(false);


  function onCodeInput() {
    clearTimeout(decodeTimer);
    preview = null;
    previewErr = null;
    const c = code.trim();
    if (!c) return;
    decodeTimer = window.setTimeout(async () => {
      try {
        preview = await api.decodeCode(c);
      } catch (e: any) {
        previewErr = String(e?.message ?? e);
      }
    }, 250);
  }

  async function doImport(mode: "replace" | "new") {
    if (!preview) return;
    if (mode === "replace" && !confirm(t("import.replaceConfirm"))) return;
    const r = await app.run(() => api.importCode(code.trim(), mode));
    if (r) {
      await app.afterReinit();
      app.view = "tree";
    }
  }

  // build sites: a link from one of them, or sharing this build to one
  let sites = $state<{ id: string; label: string; canImport: boolean; canShare: boolean }[]>([]);
  let shareSite = $state("");
  let linkUrl = $state("");
  let linkBusy = $state(false);
  let linkErr = $state("");
  let shared = $state("");
  $effect(() => {
    if (!sites.length) {
      untrack(async () => {
        try {
          const r = await api.listBuildSites();
          sites = r.sites;
          shareSite = r.lastExport && r.sites.some((s) => s.id === r.lastExport && s.canShare) ? r.lastExport : (r.sites.find((s) => s.canShare)?.id ?? "");
        } catch {
          sites = [];
        }
      });
    }
  });
  async function fromLink() {
    linkErr = "";
    linkBusy = true;
    try {
      const r = await api.importFromUrl(linkUrl.trim());
      code = r.code;
      linkUrl = "";
      await onCodeInput();
    } catch (e: any) {
      linkErr = String(e?.message ?? e);
    }
    linkBusy = false;
  }
  async function share() {
    linkErr = "";
    shared = "";
    linkBusy = true;
    try {
      const r = await api.shareBuild(shareSite);
      shared = r.url;
      await copyText(shared);
    } catch (e: any) {
      linkErr = String(e?.message ?? e);
    }
    linkBusy = false;
  }

  // --- clipboard: a share code / build link comes in on its own (entering the
  // page, the window getting focus back, a click into either box) when its box
  // is empty; a right click on a box replaces that box with the clipboard.
  // Only ever fills a field: importing is always the user's button.
  let lastAuto = "";
  let clipNote = $state<string | null>(null);
  function putCode(text: string) {
    code = text;
    onCodeInput();
  }
  // explicit = a click into a box: the user is there, so text brought in
  // before (and since cleared) may come in again
  async function pullClipboard(explicit = false) {
    if (app.view !== "import") return;
    const found = classifyClipboard(await api.readClipboard());
    if (!found) return;
    const seen = explicit ? "" : lastAuto;
    if (found.kind === "code" && shouldAutoFill(found, code, seen)) {
      lastAuto = found.text;
      putCode(found.text);
      clipNote = t("import.fromClipboard");
    } else if (found.kind === "url" && shouldAutoFill(found, linkUrl, seen)) {
      lastAuto = found.text;
      linkUrl = found.text;
      clipNote = t("import.linkFromClipboard");
    }
  }
  const onWindowFocus = () => void pullClipboard();
  onMount(() => {
    void pullClipboard();
    window.addEventListener("focus", onWindowFocus);
    return () => window.removeEventListener("focus", onWindowFocus);
  });

  // Right click: paste the clipboard over the box (whatever it is -- the user
  // asked); a link aimed at the code box, or a code at the link box, goes to
  // the box it belongs in.
  async function pasteOver(e: MouseEvent, target: "code" | "link") {
    e.preventDefault();
    const text = (await api.readClipboard()).trim();
    if (!text) return;
    lastAuto = text;
    if (target === "code" && looksLikeBuildUrl(text)) {
      linkUrl = text;
      clipNote = t("import.linkFromClipboard");
    } else if (target === "link" && looksLikeShareCode(text)) {
      putCode(text);
      clipNote = t("import.fromClipboard");
    } else if (target === "code") {
      putCode(text);
      clipNote = null;
    } else {
      linkUrl = text;
      clipNote = null;
    }
  }
  // Ctrl+V of a link into the code box (or a code into the link box) goes where it belongs.
  function pasteInto(e: ClipboardEvent, target: "code" | "link") {
    const text = e.clipboardData?.getData("text/plain")?.trim() ?? "";
    if (target === "code" && looksLikeBuildUrl(text)) {
      e.preventDefault();
      linkUrl = text;
      clipNote = t("import.linkFromClipboard");
    } else if (target === "link" && looksLikeShareCode(text)) {
      e.preventDefault();
      putCode(text);
      clipNote = t("import.fromClipboard");
    }
  }

  async function doExport() {
    copied = false;
    const r = await app.run(() => api.exportCode());
    if (r) exported = r.code;
  }

  async function copyExport() {
    try {
      copied = await copyText(exported);
    } catch {
      copied = false;
    }
  }
