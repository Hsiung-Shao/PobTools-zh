<!-- 匯入 / 匯出:帳號匯入(POB 自己的 OAuth 與帳號名稱兩條流程,下載在引擎
     的子腳本裡跑,頁面輪詢 import_status)、分享碼(貼上 → 預覽 → 匯入)、
     產生分享碼、角色 JSON 匯入。 -->
<script lang="ts">
  import { onDestroy, untrack } from "svelte";
  import { api, type AccountChar, type CodeInfo, type ImportStatus } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import PobText from "../components/PobText.svelte";

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

  let itemsJson = $state("");
  let passivesJson = $state("");
  let importTree = $state(true);
  let importItems = $state(true);
  let deleteJewels = $state(false);
  let clearItems = $state(false);
  let clearSkills = $state(false);
  let ignoreWeaponSwap = $state(false);
  let charResult = $state<string | null>(null);

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

  async function doExport() {
    copied = false;
    const r = await app.run(() => api.exportCode());
    if (r) exported = r.code;
  }

  async function copyExport() {
    try {
      await navigator.clipboard.writeText(exported);
      copied = true;
    } catch {
      copied = false;
    }
  }

  async function doCharImport() {
    charResult = null;
    const r = await app.run(() =>
      api.importCharacter({
        items: itemsJson.trim(),
        passives: passivesJson.trim(),
        importTree,
        importItems,
        deleteJewels,
        clearItems,
        clearSkills,
        ignoreWeaponSwap,
      }),
    );
    if (r) {
      charResult = r.imported.join(" + ");
      await app.refresh();
    }
  }
</script>

<div class="page">
  <section class="card">
    <h2>{t("import.oauthTitle")}</h2>
    <p class="dim">{t("import.oauthHint")}</p>
    {#if !st}
      <p class="dim">{t("import.working")}</p>
    {:else if !st.authorized}
      <div class="actions">
        <button class="btn primary" disabled={app.busy > 0 || authWait > 0} onclick={authorize}>{t("import.authorize")}</button>
        {#if authWait > 0}
          <span class="dim">{t("import.authorizing", { s: authWait })}</span>
          {#if st.oauth.url}<a class="link" href={st.oauth.url} target="_blank" rel="noreferrer">{t("import.openUrl")}</a>{/if}
        {:else}
          <span class="dim">{t("import.notAuthorized")}</span>
        {/if}
      </div>
      {#if st.oauth.errCode}<div class="bad">{st.oauth.errCode}</div>{/if}
    {:else}
      <div class="actions">
        <span class="ok">{t("import.authorized")}</span>
        <button class="btn ghost sm" disabled={app.busy > 0} onclick={logout}>{t("import.logout")}</button>
        <span class="vsep"></span>
        <label class="inline"><span class="k">{t("import.realm")}</span>
          <select class="select sm" bind:value={oRealm}>
            {#each st.realms as r (r.id)}<option value={r.id}>{r.label}</option>{/each}
          </select>
        </label>
        <button class="btn sm" disabled={app.busy > 0 || busyOauth || rateWait > 0} onclick={fetchOauth}>{oChars.length ? t("import.fetched") : t("import.fetch")}</button>
        {#if busyOauth}<span class="dim">{t("import.working")}</span>{/if}
        {#if rateWait > 0}<span class="bad">{t("import.rateLimited", { s: rateWait })}</span>{/if}
      </div>
      {#if st.oauth.errCode}<div class="bad">{st.oauth.errCode}</div>{/if}
      {#if oChars.length}
        <div class="actions">
          <label class="inline"><span class="k">{t("import.league")}</span>
            <select class="select sm" bind:value={oLeague}>
              <option value="">{t("import.anyLeague")}</option>
              {#each oLeagues as l (l)}<option value={l}>{l}</option>{/each}
            </select>
          </label>
          <label class="inline grow"><span class="k">{t("import.character")}</span>
            <select class="select sm wide" bind:value={oChar}>
              {#each oFiltered as c (c.name)}<option value={c.name}>{charLabel(c)}</option>{/each}
            </select>
          </label>
        </div>
        <div class="opts">
          <button class="btn sm" disabled={app.busy > 0 || busyOauth || !oChar} onclick={() => importOauth("tree")}>{t("import.importTree")}</button>
          <label><input type="checkbox" bind:checked={oDeleteJewels} /> {t("import.optDeleteJewels")}</label>
          <span class="vsep"></span>
          <button class="btn sm" disabled={app.busy > 0 || busyOauth || !oChar} onclick={() => importOauth("items")}>{t("import.importItems")}</button>
          <label><input type="checkbox" bind:checked={oClearItems} /> {t("import.optClearItems")}</label>
          <label><input type="checkbox" bind:checked={oClearSkills} /> {t("import.optClearSkills")}</label>
          <label><input type="checkbox" bind:checked={oIgnoreSwap} /> {t("import.optIgnoreSwap")}</label>
        </div>
      {/if}
    {/if}
    {#if doneMsg}<div class="ok">{doneMsg}</div>{/if}
  </section>

  <section class="card">
    <h2>{t("import.siteTitle")}</h2>
    <p class="dim">{t("import.siteHint")}</p>
    <div class="actions">
      <label class="inline"><span class="k">{t("import.realm")}</span>
        <select class="select sm" bind:value={sRealm} disabled={siteReady}>
          {#each st?.realms ?? [] as r (r.id)}<option value={r.id}>{r.label}</option>{/each}
        </select>
      </label>
      <label class="inline grow"><span class="k">{t("import.accountName")}</span>
        <input class="input sm wide" bind:value={sAccount} list="acct-history" placeholder="Name#1234" disabled={siteReady} onkeydown={(e) => e.key === "Enter" && accountOk && startSite()} />
        <datalist id="acct-history">
          {#each st?.accountHistory ?? [] as a (a)}<option value={a}></option>{/each}
        </datalist>
      </label>
      {#if siteReady}
        <button class="btn ghost sm" disabled={app.busy > 0 || busySite} onclick={closeSite}>{t("import.siteClose")}</button>
      {:else}
        <button class="btn primary sm" disabled={app.busy > 0 || busySite || !accountOk || !st} onclick={startSite}>{t("import.start")}</button>
      {/if}
      {#if busySite}<span class="dim">{t("import.working")}</span>{/if}
    </div>
    {#if st?.site.status && st.site.status !== "Idle"}
      <div class="status"><PobText text={st.site.statusZh || st.site.status} /></div>
    {/if}
    {#if siteReady && sChars.length}
      <div class="actions">
        <label class="inline"><span class="k">{t("import.league")}</span>
          <select class="select sm" bind:value={sLeague}>
            <option value="">{t("import.anyLeague")}</option>
            {#each sLeagues as l (l)}<option value={l}>{l}</option>{/each}
          </select>
        </label>
        <label class="inline grow"><span class="k">{t("import.character")}</span>
          <select class="select sm wide" bind:value={sChar}>
            {#each sFiltered as c (c.name)}<option value={c.name}>{charLabel(c)}</option>{/each}
          </select>
        </label>
      </div>
      <div class="opts">
        <button class="btn sm" disabled={app.busy > 0 || busySite || !sChar} onclick={() => importSite("tree")}>{t("import.importTree")}</button>
        <label><input type="checkbox" bind:checked={sDeleteJewels} /> {t("import.optDeleteJewels")}</label>
        <span class="vsep"></span>
        <button class="btn sm" disabled={app.busy > 0 || busySite || !sChar} onclick={() => importSite("items")}>{t("import.importItems")}</button>
        <label><input type="checkbox" bind:checked={sClearItems} /> {t("import.optClearItems")}</label>
        <label><input type="checkbox" bind:checked={sClearSkills} /> {t("import.optClearSkills")}</label>
        <label><input type="checkbox" bind:checked={sIgnoreSwap} /> {t("import.optIgnoreSwap")}</label>
      </div>
    {/if}
  </section>

  <section class="card">
    <h2>{t("import.codeTitle")}</h2>
    <p class="dim">{t("import.codeHint")}</p>
    <textarea class="input area" rows="4" bind:value={code} oninput={onCodeInput} placeholder={t("import.codePlaceholder")}></textarea>
    {#if previewErr}
      <div class="bad">{previewErr}</div>
    {:else if preview}
      <div class="preview">
        <span class="gold">{preview.ascendClassNameZh || preview.ascendClassName || preview.classNameZh || preview.className || "?"}</span>
        <span class="dim">{t("sidebar.level")} {preview.level ?? "?"}</span>
        <span class="dim">·</span>
        <span>{t("import.items", { n: preview.itemCount ?? 0 })}</span>
        <span>{t("import.skills", { n: preview.skillCount ?? 0 })}</span>
        {#if preview.hasTree}<span>{t("import.tree")}</span>{/if}
        <span class="dim">{preview.targetVersion ?? ""}</span>
      </div>
      <div class="actions">
        <button class="btn primary" disabled={app.busy > 0} onclick={() => doImport("new")}>{t("import.asNew")}</button>
        <button class="btn" disabled={app.busy > 0 || !app.loaded} onclick={() => doImport("replace")}>{t("import.replace")}</button>
      </div>
    {/if}
  </section>

  <section class="card">
    <h2>{t("import.exportTitle")}</h2>
    <p class="dim">{t("import.exportHint")}</p>
    <div class="actions">
      <button class="btn primary" disabled={app.busy > 0 || !app.loaded} onclick={doExport}>{t("import.generate")}</button>
      {#if exported}
        <button class="btn" onclick={copyExport}>{copied ? t("import.copied") : t("import.copy")}</button>
        <span class="dim num">{exported.length}</span>
      {/if}
    </div>
    {#if exported}
      <textarea class="input area selectable" rows="4" readonly value={exported}></textarea>
    {/if}
  </section>

  <section class="card">
    <h2>{t("import.charTitle")}</h2>
    <p class="dim">{t("import.charHint")}</p>
    <div class="two">
      <label class="col">
        <span class="k">{t("import.charItems")}</span>
        <textarea class="input area" rows="5" bind:value={itemsJson} placeholder={'{ "items": [...], "character": {...} }'}></textarea>
      </label>
      <label class="col">
        <span class="k">{t("import.charPassives")}</span>
        <textarea class="input area" rows="5" bind:value={passivesJson} placeholder={'{ "hashes": [...], "items": [...] }'}></textarea>
      </label>
    </div>
    <div class="opts">
      <label><input type="checkbox" bind:checked={importTree} /> {t("import.optTree")}</label>
      <label><input type="checkbox" bind:checked={deleteJewels} disabled={!importTree} /> {t("import.optDeleteJewels")}</label>
      <span class="vsep"></span>
      <label><input type="checkbox" bind:checked={importItems} /> {t("import.optItems")}</label>
      <label><input type="checkbox" bind:checked={clearItems} disabled={!importItems} /> {t("import.optClearItems")}</label>
      <label><input type="checkbox" bind:checked={clearSkills} disabled={!importItems} /> {t("import.optClearSkills")}</label>
      <label><input type="checkbox" bind:checked={ignoreWeaponSwap} disabled={!importItems} /> {t("import.optIgnoreSwap")}</label>
    </div>
    <div class="actions">
      <button class="btn primary" disabled={app.busy > 0 || !app.loaded || (!itemsJson.trim() && !passivesJson.trim())} onclick={doCharImport}>{t("import.charGo")}</button>
      {#if charResult}<span class="ok">{t("import.charDone", { what: charResult })}</span>{/if}
    </div>
  </section>
</div>

<style>
  .page {
    height: 100%;
    overflow-y: auto;
    padding: 16px 18px 24px;
    display: flex;
    flex-direction: column;
    gap: 14px;
    max-width: 980px;
  }
  .card {
    padding: 14px 16px 16px;
    background: var(--surface-1);
    border: 1px solid var(--edge-0);
    border-radius: var(--radius-m);
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  h2 {
    margin: 0;
    font-size: var(--fs-md);
    font-weight: 600;
    padding-left: 10px;
    position: relative;
  }
  h2::before {
    content: "";
    position: absolute;
    left: 0;
    top: 3px;
    bottom: 3px;
    width: 2px;
    background: var(--gold);
    border-radius: 1px;
  }
  p {
    margin: 0;
    font-size: var(--fs-xs);
  }
  .area {
    height: auto;
    padding: 8px 9px;
    font-family: var(--font-mono);
    font-size: var(--fs-xs);
    resize: vertical;
    line-height: 1.4;
  }
  .preview {
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
    align-items: baseline;
    font-size: var(--fs-sm);
  }
  .gold {
    color: var(--gold);
    font-weight: 600;
  }
  .actions {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
  }
  .two {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 10px;
  }
  .col {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
  }
  .opts {
    display: flex;
    flex-wrap: wrap;
    gap: 6px 14px;
    font-size: var(--fs-xs);
    color: var(--ink-1);
  }
  .opts label {
    display: inline-flex;
    align-items: center;
    gap: 5px;
  }
  .vsep {
    width: 1px;
    height: 14px;
    background: var(--edge-1);
    align-self: center;
  }
  .bad {
    color: var(--bad);
    font-size: var(--fs-xs);
  }
  .ok {
    color: var(--ok);
    font-size: var(--fs-xs);
  }
  .inline {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    min-width: 0;
  }
  .inline.grow {
    flex: 1;
  }
  .wide {
    flex: 1;
    min-width: 200px;
    max-width: 520px;
  }
  .input.sm,
  .select.sm {
    height: 24px;
    font-size: var(--fs-xs);
  }
  .status {
    font-size: var(--fs-xs);
    color: var(--ink-1);
  }
  .link {
    color: var(--accent);
    font-size: var(--fs-xs);
  }
</style>
