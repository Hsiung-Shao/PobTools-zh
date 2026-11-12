<!-- 匯入 / 匯出:帳號匯入(POB 自己的 OAuth 與帳號名稱兩條流程,下載在引擎
     的子腳本裡跑,頁面輪詢 import_status)、分享碼(貼上 → 預覽 → 匯入)、
     產生分享碼。 -->
<script lang="ts" src="$lib/views/ImportView.ts"></script>

<div class="page">
  <div class="col">
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
      <div class="banner">
        <span class="dot"></span>
        <span class="grow">{t("import.authorized")}</span>
        <button class="btn ghost sm" disabled={app.busy > 0} onclick={logout}>{t("import.logout")}</button>
      </div>
      <div class="actions">
        <div class="inline"><span class="k">{t("import.realm")}</span>
          <Select class="select sm" ariaLabel={t("import.realm")} value={oRealm} options={st.realms.map((r) => ({ value: r.id, label: r.label }))} onchange={(v) => (oRealm = String(v))} />
        </div>
        <button class="btn sm" disabled={app.busy > 0 || busyOauth || rateWait > 0} onclick={fetchOauth}>{oChars.length ? t("import.fetched") : t("import.fetch")}</button>
        {#if busyOauth}<span class="dim">{t("import.working")}</span>{/if}
        {#if rateWait > 0}<span class="bad">{t("import.rateLimited", { s: rateWait })}</span>{/if}
      </div>
      {#if st.oauth.errCode}<div class="bad">{st.oauth.errCode}</div>{/if}
      {#if oChars.length}
        <div class="actions">
          <div class="inline"><span class="k">{t("import.league")}</span>
            <Select class="select sm" ariaLabel={t("import.league")} value={oLeague} options={leagueOpts(oLeagues)} onchange={(v) => (oLeague = String(v))} />
          </div>
          <div class="inline grow"><span class="k">{t("import.character")}</span>
            <Select class="select sm wide" ariaLabel={t("import.character")} value={oChar} options={oFiltered.map((c) => ({ value: c.name, label: charLabel(c) }))} onchange={(v) => (oChar = String(v))} />
          </div>
        </div>
        <div class="optlist">
          <label class="optrow"><span>{t("import.optDeleteJewels")}</span><input type="checkbox" class="sw" bind:checked={oDeleteJewels} /></label>
          <label class="optrow"><span>{t("import.optClearItems")}</span><input type="checkbox" class="sw" bind:checked={oClearItems} /></label>
          <label class="optrow"><span>{t("import.optClearSkills")}</span><input type="checkbox" class="sw" bind:checked={oClearSkills} /></label>
          <label class="optrow"><span>{t("import.optIgnoreSwap")}</span><input type="checkbox" class="sw" bind:checked={oIgnoreSwap} /></label>
        </div>
        <div class="actions end">
          <button class="btn sm" disabled={app.busy > 0 || busyOauth || !oChar} onclick={() => importOauth("tree")}>{t("import.importTree")}</button>
          <button class="btn sm" disabled={app.busy > 0 || busyOauth || !oChar} onclick={() => importOauth("items")}>{t("import.importItems")}</button>
        </div>
      {/if}
    {/if}
    {#if doneMsg}<div class="ok">{doneMsg}</div>{/if}
  </section>

  {#if app.has("siteImport")}
  <section class="card">
    <h2>{t("import.siteTitle")}</h2>
    <p class="dim">{t("import.siteHint")}</p>
    <div class="actions">
      <div class="inline"><span class="k">{t("import.realm")}</span>
        <Select class="select sm" ariaLabel={t("import.realm")} value={sRealm} disabled={siteReady} options={(st?.realms ?? []).map((r) => ({ value: r.id, label: r.label }))} onchange={(v) => (sRealm = String(v))} />
      </div>
      <label class="inline grow"><span class="k">{t("import.accountName")}</span>
        <input class="input sm wide" bind:value={sAccount} list="acct-history" placeholder="Name#1234" disabled={siteReady} onkeydown={(e) => e.key === "Enter" && accountOk && startSite()} />
        <datalist id="acct-history">
          {#each st?.accountHistory ?? [] as a (a)}<option value={a}></option>{/each}
        </datalist>
      </label>
      {#if (st?.accountHistory ?? []).includes(sAccount) && !siteReady}
        <button class="btn ghost sm" title={t("import.historyRemove")} disabled={app.busy > 0}
          onclick={async () => { const r = await app.run(() => api.removeAccountHistory(sAccount)); if (r) { st = r; sAccount = ""; } }}>×</button>
      {/if}
      {#if siteReady}
        <button class="btn ghost sm" disabled={app.busy > 0 || busySite} onclick={closeSite}>{t("import.siteClose")}</button>
      {:else}
        <button class="btn accent sm" disabled={app.busy > 0 || busySite || !accountOk || !st} onclick={startSite}>{t("import.start")}</button>
      {/if}
      {#if busySite}<span class="dim">{t("import.working")}</span>{/if}
    </div>
    {#if st?.site.status && st.site.status !== "Idle"}
      <div class="status"><PobText text={st.site.statusZh || st.site.status} /></div>
    {/if}
    {#if siteReady && sChars.length}
      <div class="actions">
        <div class="inline"><span class="k">{t("import.league")}</span>
          <Select class="select sm" ariaLabel={t("import.league")} value={sLeague} options={leagueOpts(sLeagues)} onchange={(v) => (sLeague = String(v))} />
        </div>
        <div class="inline grow"><span class="k">{t("import.character")}</span>
          <Select class="select sm wide" ariaLabel={t("import.character")} value={sChar} options={sFiltered.map((c) => ({ value: c.name, label: charLabel(c) }))} onchange={(v) => (sChar = String(v))} />
        </div>
      </div>
      <div class="optlist">
        <label class="optrow"><span>{t("import.optDeleteJewels")}</span><input type="checkbox" class="sw" bind:checked={sDeleteJewels} /></label>
        <label class="optrow"><span>{t("import.optClearItems")}</span><input type="checkbox" class="sw" bind:checked={sClearItems} /></label>
        <label class="optrow"><span>{t("import.optClearSkills")}</span><input type="checkbox" class="sw" bind:checked={sClearSkills} /></label>
        <label class="optrow"><span>{t("import.optIgnoreSwap")}</span><input type="checkbox" class="sw" bind:checked={sIgnoreSwap} /></label>
      </div>
      <div class="actions end">
        <button class="btn sm" disabled={app.busy > 0 || busySite || !sChar} onclick={() => importSite("tree")}>{t("import.importTree")}</button>
        <button class="btn sm" disabled={app.busy > 0 || busySite || !sChar} onclick={() => importSite("items")}>{t("import.importItems")}</button>
      </div>
    {/if}
  </section>
  {/if}
  </div>

  <div class="col">
  <section class="card">
    <h2>{t("import.codeTitle")}</h2>
    <p class="dim">{t("import.codeHint")}</p>
    <div class="actions">
      <input class="input sm grow" placeholder={t("import.linkPlaceholder")} bind:value={linkUrl} onkeydown={(e) => e.key === "Enter" && linkUrl.trim() && fromLink()}
        onclick={() => void pullClipboard(true)} onfocus={() => void pullClipboard(true)}
        oncontextmenu={(e) => pasteOver(e, "link")} onpaste={(e) => pasteInto(e, "link")} />
      <button class="btn sm" disabled={!linkUrl.trim() || linkBusy || app.busy > 0} onclick={fromLink}>{t("import.fromLink")}</button>
      {#if linkBusy}<span class="dim">{t("import.working")}</span>{/if}
    </div>
    {#if sites.length}
      <p class="dim small">{t("import.siteList", { sites: sites.filter((s) => s.canImport).map((s) => s.label).join("、") })}</p>
    {/if}
    {#if linkErr}<div class="bad">{linkErr}</div>{/if}
    <textarea class="input area" rows="4" bind:value={code} oninput={onCodeInput} placeholder={t("import.codePlaceholder")}
      onclick={() => void pullClipboard(true)} onfocus={() => void pullClipboard(true)}
      oncontextmenu={(e) => pasteOver(e, "code")} onpaste={(e) => pasteInto(e, "code")}></textarea>
    {#if clipNote}<div class="dim small clipnote">{clipNote}</div>{/if}
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
      <div class="actions end">
        <button class="btn accent" disabled={app.busy > 0} onclick={() => doImport("new")}>{t("import.asNew")}</button>
        <button class="btn" disabled={app.busy > 0 || !app.loaded} onclick={() => doImport("replace")}>{t("import.replace")}</button>
      </div>
    {/if}
  </section>

  <section class="card">
    <h2>{t("import.exportTitle")}</h2>
    <p class="dim">{t("import.exportHint")}</p>
    <div class="actions">
      <button class="btn accent" disabled={app.busy > 0 || !app.loaded} onclick={doExport}>{t("import.generate")}</button>
      {#if exported}<span class="dim num">{exported.length}</span>{/if}
    </div>
    {#if exported}
      <div class="actions">
        <input class="input num selectable grow" readonly value={exported} onfocus={(e) => e.currentTarget.select()} />
        <button class="btn" onclick={copyExport}>{copied ? t("import.copied") : t("import.copy")}</button>
      </div>
    {/if}
    {#if sites.some((s) => s.canShare)}
      <div class="actions">
        <Select class="select sm" value={shareSite} options={sites.filter((s) => s.canShare).map((s) => ({ value: s.id, label: s.label }))} onchange={(v) => (shareSite = String(v))} />
        <button class="btn" disabled={!shareSite || linkBusy || app.busy > 0 || !app.loaded} onclick={share}>{t("import.share")}</button>
      </div>
    {/if}
    {#if shared}
      <div class="sharebar">
        <input class="input selectable grow" readonly value={shared} onfocus={(e) => e.currentTarget.select()} />
        <span class="dim small">{t("import.shareCopied")}</span>
      </div>
    {/if}
  </section>
  </div>
</div>

<style>
  .page {
    height: 100%;
    overflow-y: auto;
    padding: 16px;
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: 12px;
    align-items: start;
    max-width: 1280px;
  }
  @container view (max-width: 899px) {
    .page {
      grid-template-columns: minmax(0, 1fr);
    }
  }
  .col {
    display: flex;
    flex-direction: column;
    gap: 12px;
    min-width: 0;
  }
  .card {
    padding: 16px 20px;
    display: flex;
    flex-direction: column;
    gap: 12px;
  }
  h2 {
    margin: 0;
    font-size: calc(var(--fs-base) + 4px);
    font-weight: 500;
  }
  h2 + p {
    margin-top: -8px;
  }
  p {
    margin: 0;
    font-size: var(--fs-xs);
  }
  .clipnote {
    margin-top: -6px;
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
    color: var(--hl);
    font-weight: 600;
  }
  .actions {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
  }
  .actions.end {
    justify-content: flex-end;
  }
  .grow {
    flex: 1;
    min-width: 0;
  }
  /* 這頁只有「授權」用實心 primary;其他主要動作用強調框 */
  .btn.accent {
    border-color: var(--gold);
    background: var(--gold-soft);
    color: var(--gold-text);
  }
  .btn.accent:hover:not(:disabled) {
    border-color: var(--gold);
    background: color-mix(in srgb, var(--gold) 26%, transparent);
    color: var(--gold-text);
  }
  .banner {
    display: flex;
    align-items: center;
    gap: 8px;
    min-height: 36px;
    padding: 4px 6px 4px 12px;
    border-radius: var(--radius-s);
    background: color-mix(in srgb, var(--ok) 12%, transparent);
    color: var(--ok);
    font-size: var(--fs-sm);
  }
  .banner .dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: currentColor;
    flex: none;
  }
  .optlist {
    display: flex;
    flex-direction: column;
  }
  .optrow {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 16px;
    height: 36px;
    font-size: var(--fs-sm);
    color: var(--ink-1);
    cursor: pointer;
  }
  .optrow + .optrow {
    border-top: 1px solid var(--edge-0);
  }
  .sharebar {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 8px 10px;
    border-radius: var(--radius-s);
    background: var(--surface-2);
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
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
  .page :global(.select.wide),
  .input.wide {
    flex: 1;
    min-width: 200px;
    max-width: 520px;
  }
  .input.sm,
  .page :global(.select.sm) {
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
