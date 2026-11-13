<!-- 比較分頁(POB 的 Compare tab):載入另一個建置(分享碼或建置檔),
     摘要用 POB 兩邊的輸出逐項比,天賦樹/物品/技能/配置列出差異並可複製回來。 -->
<script lang="ts" src="$lib/views/CompareView.ts"></script>

<div class="page">
  <div class="toolbar">
    <span class="k">{t("compare.title")}</span>
    {#if st?.builds?.length}
      <Select class="select sm" value={String(st.activeIndex)} disabled={busy} options={st.builds.map((b) => ({ value: String(b.index), label: b.label || b.buildName || "" }))} onchange={(v) => run(() => api.compareSelect(Number(v)))} />
      <button class="btn ghost sm" disabled={busy} onclick={() => run(() => api.compareRemove(st!.activeIndex))}>{t("compare.remove")}</button>
    {/if}
    {#if st?.activeIndex}
      <span class="vsep"></span>
      <div class="tabs" role="tablist">
        {#each [["summary", "compare.summary"], ["tree", "compare.tree"], ["items", "compare.items"], ["skills", "compare.skills"], ["config", "compare.config"]] as [id, key]}
          <button role="tab" aria-selected={view === id} disabled={busy} onclick={() => loadView(id as SubView)}>{t(key)}</button>
        {/each}
      </div>
      {#if busy}<span class="dim pulse">{t("tree.powerWorking")}</span>{/if}
    {/if}
    <span class="grow"></span>
    <input class="input sm code" placeholder={t("compare.codeHint")} bind:value={code} />
    <button class="btn sm" disabled={!code.trim() || busy} onclick={() => run(() => api.compareLoad({ code: code.trim(), label: t("compare.pasted") }))}>{t("compare.importCode")}</button>
    <button class="btn sm" disabled={busy} onclick={openPicker}>{t("compare.importFile")}</button>
  </div>

  <div class="scroll">
    {#if err}<p class="bad">{err}</p>{/if}

    {#if !st?.activeIndex}
      <p class="dim">{t("compare.none")}</p>
    {:else if view === "summary"}
      <div class="card">
        <table>
          <thead><tr><th class="card-title">{t("compare.stat")}</th><th class="card-title n">{t("compare.mine")}</th><th class="card-title n">{t("compare.theirs")}</th><th class="card-title n">{t("compare.diff")}</th></tr></thead>
          <tbody>
            {#each st.stats ?? [] as r, i (i)}
              {#if r.gap}
                <tr class="gap"><td colspan="4"></td></tr>
              {:else}
                <tr>
                  <td>{r.labelZh || r.label}</td>
                  <td class="n">{r.primary}</td>
                  <td class="n cmp">{r.compare}</td>
                  <td class="n diff" class:good={r.better === true} class:bad={r.better === false}>{r.diff ?? ""}{r.diffPercent ? ` (${r.diffPercent})` : ""}</td>
                </tr>
              {/if}
            {/each}
          </tbody>
        </table>
      </div>
    {:else if view === "tree" && tree}
      <div class="card">
        <div class="head card-title">{t("compare.tree")}</div>
        <div class="body">
          <p>{t("compare.treePoints", { n: tree.points ?? 0 })}</p>
          <p class="good">{t("compare.treeOnlyCompare", { n: tree.onlyInCompare.length })}</p>
          <p class="bad">{t("compare.treeOnlyPrimary", { n: tree.onlyInPrimary.length })}</p>
          <div class="acts">
            <button class="btn" disabled={busy} onclick={() => use("tree")}>{t("compare.useTree")}</button>
          </div>
        </div>
      </div>
    {:else if view === "items" && items}
      <div class="card">
        <table>
          <thead><tr><th class="card-title">{t("compare.slot")}</th><th class="card-title">{t("compare.mine")}</th><th class="card-title">{t("compare.theirs")}</th><th class="card-title"></th></tr></thead>
          <tbody>
            {#each items.rows as r (r.slot)}
              <tr class:same={r.same}>
                <td>{r.slotZh || r.slot}</td>
                <td>{r.primary ? r.primary.nameZh || r.primary.name : "—"}</td>
                <td class="cmp">{r.compare ? r.compare.nameZh || r.compare.name : "—"}</td>
                <td class="act">{#if r.compare && !r.same}<button class="btn ghost sm" disabled={busy} onclick={() => use("item", r.slot)}>{t("compare.useItem")}</button>{/if}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {:else if view === "skills" && skills}
      <div class="cols">
        {#each [{ who: "mine", list: skills.primary }, { who: "theirs", list: skills.compare }] as side (side.who)}
          <div class="card">
            <div class="head card-title">{t(side.who === "mine" ? "compare.mine" : "compare.theirs")}</div>
            {#each side.list as g (g.index)}
              <div class="group" class:off={!g.enabled}>
                <div class="gname"><PobText text={g.labelZh || g.label} />{g.isMain ? " ★" : ""}</div>
                <div class="gems dim small">{g.gems.map((x: { name: string; nameZh?: string; level?: number; quality?: number }) => `${x.nameZh || x.name} ${x.level}/${x.quality}`).join(" · ")}</div>
              </div>
            {/each}
          </div>
        {/each}
      </div>
    {:else if view === "config" && config}
      {#if config.rows.length === 0}
        <p class="dim">{t("compare.configSame")}</p>
      {:else}
        <div class="card">
          <table>
            <thead><tr><th class="card-title">{t("compare.option")}</th><th class="card-title">{t("compare.mine")}</th><th class="card-title">{t("compare.theirs")}</th></tr></thead>
            <tbody>
              {#each config.rows as r (r.var)}
                <tr><td>{r.labelZh || r.label}</td><td>{r.primary}</td><td class="cmp">{r.compare}</td></tr>
              {/each}
            </tbody>
          </table>
        </div>
        <div class="acts"><button class="btn" disabled={busy} onclick={() => use("config")}>{t("compare.useConfig")}</button></div>
      {/if}
    {/if}
  </div>

  {#if picker}
    <div class="modal">
      <div class="dialog">
        <div class="label">{t("compare.importFile")}</div>
        <div class="list">
          {#each builds as b (b.fullFileName)}
            <button class="frow" onclick={() => loadFile(b)}>{b.buildName ?? b.fileName}</button>
          {/each}
        </div>
        <div class="dlg-actions"><button class="btn ghost" onclick={() => (picker = false)}>{t("tree.cancel")}</button></div>
      </div>
    </div>
  {/if}
</div>

<style>
  .page {
    height: 100%;
    display: flex;
    flex-direction: column;
    min-height: 0;
  }
  .k {
    font-size: var(--fs-2xs);
    letter-spacing: 0.1em;
    color: var(--ink-3);
    white-space: nowrap;
  }
  .code {
    width: min(260px, 100%);
  }
  .scroll {
    flex: 1;
    overflow: auto;
    min-height: 0;
    padding: 16px;
    display: flex;
    flex-direction: column;
    gap: 12px;
  }
  .scroll > p {
    margin: 0;
  }
  .card {
    overflow: hidden;
    flex: none;
  }
  table {
    width: 100%;
    border-collapse: collapse;
    font-size: var(--fs-sm);
  }
  th,
  td {
    padding: 0 14px;
    text-align: left;
  }
  th {
    height: 40px;
    border-bottom: 1px solid var(--edge-1);
  }
  td {
    height: 32px;
  }
  tbody tr + tr td {
    border-top: 1px solid var(--edge-0);
  }
  th.n,
  td.n {
    text-align: right;
    font-family: var(--font-mono);
    font-variant-numeric: tabular-nums;
  }
  td.act {
    text-align: right;
  }
  tbody tr:not(.gap):hover {
    background: var(--surface-hover);
  }
  /* POB 的空行 = 分組分隔:一條 30px 的底色列 */
  tr.gap td {
    height: 30px;
    background: var(--surface-0);
  }
  tr.same td {
    opacity: 0.55;
  }
  .cmp {
    color: var(--accent);
  }
  .diff {
    color: var(--ink-2);
  }
  .good {
    color: var(--ok);
  }
  .bad {
    color: var(--bad);
  }
  .head {
    display: flex;
    align-items: center;
    height: 40px;
    padding: 0 14px;
    border-bottom: 1px solid var(--edge-1);
  }
  .body {
    padding: 12px 14px;
  }
  .body p {
    margin: 0 0 6px;
  }
  .cols {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(340px, 100%), 1fr));
    gap: 12px;
    align-items: start;
  }
  .group {
    padding: 6px 14px;
  }
  .group + .group {
    border-top: 1px solid var(--edge-0);
  }
  .group.off {
    opacity: 0.5;
  }
  .gname {
    color: var(--ink-0);
  }
  .small {
    font-size: var(--fs-xs);
  }
  .acts {
    display: flex;
    gap: 8px;
  }
  .body .acts {
    margin-top: 10px;
  }
  .modal {
    position: fixed;
    inset: 0;
    z-index: 200;
    display: grid;
    place-items: center;
    background: color-mix(in srgb, var(--surface-0) 70%, transparent);
  }
  .dialog {
    width: min(520px, calc(100vw - 32px));
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding: 16px 18px;
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
  }
  .list {
    max-height: 360px;
    overflow: auto;
    border: 1px solid var(--edge-0);
    border-radius: 4px;
  }
  .frow {
    display: block;
    width: 100%;
    text-align: left;
    appearance: none;
    border: 0;
    background: none;
    color: var(--ink-1);
    padding: 3px 10px;
  }
  .frow:hover {
    background: var(--surface-2);
  }
  .grow {
    flex: 1;
  }
</style>
