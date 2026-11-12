<!-- 隊伍頁:七個文字區(隊友屬性/光環/詛咒/戰吼/連結/敵人狀態/敵人詞綴)各自套用,
     加上「匯出本建置增益」開關與匯出內容(唯讀)。 -->
<script lang="ts" src="$lib/views/PartyView.ts"></script>

<div class="page">
  <section class="card top">
    <div class="tophead">
      <div class="ttl">
        <h2>{t("party.title")}</h2>
        <span class="dim small">{t("party.hint")}</span>
      </div>
      {#if data}
        <label class="chk"><input type="checkbox" class="sw" checked={data.enableExportBuffs} onchange={(e) => toggleExport(e.currentTarget.checked)} /> {t("party.export")}</label>
      {/if}
    </div>
    {#if imp}
      <div class="row">
        <input class="input code" placeholder={t("party.importHint")} bind:value={code} onkeydown={(e) => e.key === "Enter" && code.trim() && doImport()} />
        <Select class="select" value={dest} title={t("party.destination")} options={imp.destinations.map((d) => ({ value: d.index, label: d.labelZh || d.label }))} onchange={(v) => (dest = Number(v))} />
        <label class="chk" title={t("party.appendHint")}><input type="checkbox" class="sw" bind:checked={append} /> {t("party.append")}</label>
        <button class="btn accent" disabled={!code.trim() || app.busy > 0} onclick={doImport}>{t("party.import")}</button>
      </div>
      <div class="row">
        {#if impMsg}<span class="small msg"><PobText text={impMsg} /></span>{/if}
        <span class="grow"></span>
        <button class="btn sm" title={t("party.rebuildHint")} onclick={() => action("rebuild")}>{t("party.rebuild")}</button>
        <button class="btn sm" title={t("party.disableHint")} onclick={() => action("disable")}>{t("party.disable")}</button>
        <button class="btn sm danger" title={t("party.clearHint")} onclick={() => action("clear")}>{t("party.clear")}</button>
      </div>
    {/if}
  </section>
  {#if data}
    <div class="grid">
      {#each keys as k (k)}
        <section class="card field">
          <div class="head">
            <h3>{t(`party.${k}`)}</h3>
            <button class="btn sm" class:accent={drafts[k] !== data.fields[k]} disabled={drafts[k] === data.fields[k] || app.busy > 0} onclick={() => apply(k)}>{t("party.apply")}</button>
          </div>
          <textarea class="area" rows="7" bind:value={drafts[k]} spellcheck="false"></textarea>
        </section>
      {/each}
      {#if data.enableExportBuffs}
        <section class="card field wide">
          <div class="head"><h3>{t("party.exports")}</h3></div>
          <div class="exps">
            {#each Object.entries(data.exports) as [name, txt]}
              {#if txt}
                <div class="k">{name}</div>
                <pre class="exp selectable">{txt}</pre>
              {/if}
            {/each}
          </div>
        </section>
      {/if}
    </div>
  {/if}
</div>

<style>
  .page {
    height: 100%;
    min-height: 0;
    overflow-y: auto;
    padding: 16px;
    display: flex;
    flex-direction: column;
    gap: 12px;
  }
  .grow {
    flex: 1;
  }
  .small {
    font-size: var(--fs-xs);
  }
  .chk {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    font-size: var(--fs-sm);
    color: var(--ink-1);
  }
  .top {
    flex: none;
    padding: 14px 16px;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .tophead {
    display: flex;
    align-items: flex-start;
    justify-content: space-between;
    gap: 16px;
    flex-wrap: wrap;
  }
  .ttl {
    display: flex;
    flex-direction: column;
    gap: 2px;
    min-width: 0;
  }
  h2 {
    margin: 0;
    font-size: var(--fs-lg);
    font-weight: 600;
    color: var(--ink-0);
  }
  .row {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
  }
  .code {
    flex: 1;
    min-width: min(240px, 100%);
  }
  .msg {
    color: var(--ink-1);
  }
  .btn.accent {
    border-color: var(--gold);
    background: var(--gold-soft);
    color: var(--gold-text);
  }
  .btn.accent:hover:not(:disabled) {
    background: color-mix(in srgb, var(--gold) 26%, transparent);
    border-color: var(--gold);
    color: var(--gold-text);
  }
  .btn.danger {
    color: var(--bad);
  }
  .btn.danger:hover:not(:disabled) {
    border-color: var(--bad);
    color: var(--bad);
  }
  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(min(420px, 100%), 1fr));
    gap: 12px;
  }
  .field {
    display: flex;
    flex-direction: column;
    min-width: 0;
    overflow: hidden;
  }
  .field.wide {
    grid-column: 1 / -1;
  }
  .head {
    flex: none;
    height: 42px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    padding: 0 10px 0 14px;
    border-bottom: 1px solid var(--edge-0);
  }
  h3 {
    margin: 0;
    font-size: calc(var(--fs-base) + 1px);
    font-weight: 500;
    color: var(--ink-0);
  }
  .area {
    flex: 1;
    width: 100%;
    min-height: 140px;
    margin: 0;
    padding: 10px 14px;
    border: 0;
    outline: none;
    background: transparent;
    color: var(--ink-0);
    font-family: var(--font-mono);
    font-size: var(--fs-xs);
    line-height: 1.5;
    resize: vertical;
  }
  .area:focus {
    background: var(--surface-2);
  }
  .exps {
    padding: 8px 14px 14px;
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
    margin-top: 4px;
  }
  .exp {
    margin: 0;
    padding: 6px 8px;
    background: var(--surface-0);
    border: 1px solid var(--edge-0);
    border-radius: var(--radius-s);
    font-family: var(--font-mono);
    font-size: var(--fs-2xs);
    white-space: pre-wrap;
    max-height: 200px;
    overflow-y: auto;
  }
</style>
