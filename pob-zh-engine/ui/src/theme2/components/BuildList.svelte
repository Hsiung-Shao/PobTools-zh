<script lang="ts" src="$lib/views/BuildList.ts"></script>

<svelte:window onkeydown={onKey} />

<div class="list">
  <div class="head">
    <h1>{t("builds.title")}</h1>
    <span class="num count">{t("builds.count", { n: entries.filter((e) => !e.isFolder).length })}</span>
    <span class="grow"></span>
    {#if clip}
      <button class="btn ghost sm" disabled={app.busy > 0 || !canMoveTo(clip.entry, subPath) && !(clip.copy && clip.entry.subPath === subPath)} onclick={paste}
        title={t(clip.copy ? "builds.pasteCopyHint" : "builds.pasteMoveHint", { name: nameOf(clip.entry) })}>{t("builds.paste")}</button>
    {/if}
    {#if subPath}<button class="btn ghost sm" title={t("builds.upHint")} onclick={up}>↑ {t("builds.up")}</button>{/if}
    <button class="btn ghost sm" onclick={() => refresh()}>{t("builds.refresh")}</button>
    <button class="btn ghost sm" onclick={() => bridge.call("host.open_folder", { path: buildPath + subPath })}>{t("builds.openFolder")}</button>
    <span class="vsep"></span>
    <span class="search">
      <svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="11" cy="11" r="7" /><path d="M20 20l-3.5-3.5" /></svg>
      <input class="input" type="search" placeholder={t("builds.searchHint")} bind:value={filter} oninput={onSearch} />
    </span>
    <Select class="input sort" value={sortMode} onchange={(v) => refresh(subPath, { sortMode: String(v) })} title={t("builds.sort")} options={sortModes.map((s) => ({ value: s.sortMode, label: t(`builds.sort.${s.sortMode}`) === `builds.sort.${s.sortMode}` ? s.label : t(`builds.sort.${s.sortMode}`) }))} />
    <button class="btn" disabled={app.busy > 0} onclick={() => (dialog = { kind: "newFolder", name: "" })}>{t("builds.newFolder")}</button>
    <button class="btn accent" disabled={app.busy > 0} onclick={() => app.newBuild(undefined, subPath)}>{t("builds.new")}</button>
  </div>
  <nav class="crumbs" title={buildPath + subPath}>
    {#if subPath}
      <button class="crumb" onclick={() => refresh("")}>{t("builds.title")}</button>
    {:else}
      <span class="label">{t("builds.title")}</span>
    {/if}
    {#each crumbs as c, i (c.path)}
      <span class="sep dim">›</span>
      {#if i < crumbs.length - 1}
        <button class="crumb" onclick={() => refresh(c.path)}>{c.name}</button>
      {:else}
        <span class="label here">{c.name}</span>
      {/if}
    {/each}
  </nav>
  <div class="card table">
    <div class="row colhead card-title">
      <span></span>
      <span>{t("builds.colName")}</span>
      <span class="cls">{t("sidebar.class")}</span>
      <span class="lvl">{t("sidebar.level")}</span>
      <span class="date">{t("builds.colModified")}</span>
    </div>
    <div class="rows">
      {#if subPath}
        <!-- "..": up one folder; builds can be dragged onto it too -->
        <div
          class="row folder parent"
          class:drop={dropOn === "..parent"}
          role="button"
          tabindex="0"
          title={t("builds.upHint")}
          ondblclick={up}
          onkeydown={(k) => k.key === "Enter" && up()}
          ondragover={(ev) => { if (dragging && canMoveTo(dragging, parentPath)) { ev.preventDefault(); dropOn = "..parent"; } }}
          ondragleave={() => { if (dropOn === "..parent") dropOn = null; }}
          ondrop={(ev) => { ev.preventDefault(); const d = dragging; dragging = null; dropOn = null; if (d) void moveTo(d, parentPath, false); }}
        >
          <svg class="ico folder-ico" viewBox="0 0 24 24" aria-hidden="true"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" /></svg>
          <span class="name">..</span>
          <span class="cls dim">{t("builds.up")}</span>
        </div>
      {/if}
      {#if loading && entries.length === 0}
        <div class="dim pad">{t("builds.loading")}</div>
      {:else if entries.length === 0}
        <div class="dim pad">{t("builds.empty", { path: buildPath + subPath })}</div>
      {:else}
        {#each entries as e (e.fullFileName)}
          <div
            class="row"
            class:sel={selected === e.fullFileName}
            class:folder={e.isFolder}
            class:cut={clip && !clip.copy && clip.entry.fullFileName === e.fullFileName}
            class:drop={dropOn === e.fullFileName}
            role="button"
            tabindex="0"
            draggable="true"
            ondragstart={(ev) => { dragging = e; ev.dataTransfer?.setData("text/plain", e.fullFileName); }}
            ondragend={() => { dragging = null; dropOn = null; }}
            ondragover={(ev) => { if (e.isFolder && dragging && dragging !== e && canMoveTo(dragging, `${e.subPath}${e.folderName}/`)) { ev.preventDefault(); dropOn = e.fullFileName; } }}
            ondragleave={() => { if (dropOn === e.fullFileName) dropOn = null; }}
            ondrop={(ev) => { ev.preventDefault(); const d = dragging; dragging = null; dropOn = null; if (d && e.isFolder) void moveTo(d, `${e.subPath}${e.folderName}/`, false); }}
            onclick={() => (selected = e.fullFileName)}
            ondblclick={() => open(e)}
            onkeydown={(k) => k.key === "Enter" && open(e)}
          >
            {#if e.isFolder}
              <svg class="ico folder-ico" viewBox="0 0 24 24" aria-hidden="true"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" /></svg>
            {:else}
              <svg class="ico" viewBox="0 0 24 24" aria-hidden="true"><path d="M7 3h7l5 5v12a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1zM14 3v5h5" /></svg>
            {/if}
            <span class="name">{#if relPrefix(e)}<span class="dim">{relPrefix(e)}</span>{/if}{nameOf(e)}</span>
            {#if !e.isFolder}
              <span class="cls">{e.ascendClassName ?? e.className ?? ""}</span>
              <span class="lvl num">{e.level ?? ""}</span>
              <span class="date num dim">{fmtDate(e.modified)}</span>
            {/if}
          </div>
        {/each}
      {/if}
    </div>
  </div>
  <!-- what the rows' own buttons did, now for the selected row -->
  <div class="actions">
    <span class="picked">{#if selEntry}{t("builds.selected", { name: nameOf(selEntry) })}{:else}{t("builds.keysHint")}{/if}</span>
    <span class="grow"></span>
    <button class="btn sm" disabled={!selEntry} onclick={() => { if (selEntry) dialog = { kind: "rename", entry: selEntry, name: nameOf(selEntry) }; }}>{t("builds.rename")}</button>
    <button class="btn sm" disabled={!selEntry} onclick={() => { if (selEntry) dialog = { kind: "copy", entry: selEntry, name: nameOf(selEntry) }; }}>{t("builds.copy")}</button>
    <button class="btn sm" disabled={!selEntry} title={t("builds.cutHint")} onclick={() => { if (selEntry) clip = { entry: selEntry, copy: false }; }}>{t("builds.cut")}</button>
    <button class="btn sm danger" disabled={!selEntry} onclick={() => { if (selEntry) void remove(selEntry); }}>{t("builds.delete")}</button>
    <button class="btn sm open" disabled={!selEntry} onclick={() => { if (selEntry) open(selEntry); }}>{t("builds.open")}</button>
  </div>

  {#if dialog}
    <div class="modal">
      <div class="dialog">
        <div class="label">{t(dialog.kind === "newFolder" ? "builds.newFolder" : dialog.kind === "copy" ? "builds.copyTitle" : "builds.renameTitle")}</div>
        {#if dialog.entry}<p class="dim">{nameOf(dialog.entry)}</p>{/if}
        <!-- svelte-ignore a11y_autofocus -->
        <input class="input" autofocus bind:value={dialog.name} onkeydown={(k) => k.key === "Enter" && dialogOk()} />
        <div class="dlg-actions">
          <button class="btn ghost" onclick={() => (dialog = null)}>{t("tree.cancel")}</button>
          <button class="btn primary" disabled={!nameOk} onclick={dialogOk}>OK</button>
        </div>
      </div>
    </div>
  {/if}
</div>

<style>
  .list {
    height: 100%;
    display: flex;
    flex-direction: column;
    gap: 16px;
    min-height: 0;
    padding: 20px 24px;
  }
  .head {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 8px;
  }
  h1 {
    margin: 0 4px 0 0;
    font-size: calc(var(--fs-base) + 9px);
    font-weight: 500;
    color: var(--ink-0);
  }
  .count {
    font-size: var(--fs-xs);
    color: var(--ink-3);
  }
  .search {
    position: relative;
    display: inline-flex;
    width: 260px;
    max-width: 100%;
  }
  .search svg {
    position: absolute;
    left: 8px;
    top: 50%;
    width: 14px;
    height: 14px;
    transform: translateY(-50%);
    fill: none;
    stroke: var(--ink-3);
    stroke-width: 1.75;
    stroke-linecap: round;
    pointer-events: none;
  }
  .search .input {
    width: 100%;
    padding-left: 28px;
  }
  .head :global(.sort) {
    width: auto;
  }
  .btn.accent {
    border-color: var(--gold);
    background: var(--gold-soft);
    color: var(--gold-text);
    font-weight: 500;
  }
  .btn.accent:hover:not(:disabled) {
    border-color: var(--gold);
    background: color-mix(in srgb, var(--gold) 26%, transparent);
    color: var(--gold-text);
  }
  .crumbs {
    display: flex;
    align-items: center;
    gap: 6px;
    min-width: 0;
    margin-top: -6px;
    overflow: hidden;
    white-space: nowrap;
  }
  .crumb {
    border: 0;
    background: none;
    padding: 2px 4px;
    border-radius: var(--radius-s);
    color: var(--gold-text);
    font: inherit;
    cursor: pointer;
  }
  .crumb:hover {
    background: var(--surface-hover);
  }
  .label.here {
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .grow {
    flex: 1;
  }
  .pad {
    padding: 16px;
  }
  .table {
    flex: 1;
    min-height: 0;
    overflow: hidden;
    display: flex;
    flex-direction: column;
  }
  .rows {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
  }
  .row {
    display: grid;
    grid-template-columns: 36px minmax(0, 1fr) 190px 70px 150px;
    align-items: center;
    gap: 0 12px;
    height: 44px;
    padding: 0 16px 0 8px;
    border-bottom: 1px solid var(--edge-0);
    cursor: default;
  }
  .row.colhead {
    height: 34px;
    border-bottom-color: var(--edge-1);
  }
  .rows .row:hover {
    background: var(--surface-hover);
  }
  .rows .row.sel {
    background: var(--gold-soft);
  }
  .row.cut {
    opacity: 0.55;
  }
  .row.drop {
    outline: 1px dashed var(--gold);
    outline-offset: -1px;
  }
  .ico {
    justify-self: center;
    width: 18px;
    height: 18px;
    fill: none;
    stroke: var(--ink-2);
    stroke-width: 1.75;
    stroke-linejoin: round;
    stroke-linecap: round;
  }
  .ico.folder-ico {
    stroke: var(--warn);
  }
  .name {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: calc(var(--fs-base) + 1px);
    color: var(--ink-0);
  }
  .cls {
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .rows .cls {
    color: var(--hl);
  }
  .rows .parent .cls {
    color: var(--ink-3);
  }
  .lvl,
  .date {
    text-align: right;
    white-space: nowrap;
  }
  .actions {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 6px;
    margin-top: -4px;
  }
  .picked {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: var(--fs-xs);
    color: var(--ink-3);
  }
  .danger:hover:not(:disabled) {
    color: var(--bad);
  }
  .btn.open {
    border-color: var(--edge-2);
    background: var(--surface-3);
    color: var(--ink-0);
    font-weight: 500;
  }
  @container view (max-width: 759px) {
    .row {
      grid-template-columns: 36px minmax(0, 1fr) 70px;
    }
    .cls,
    .date {
      display: none;
    }
    .search {
      flex: 1 1 200px;
    }
  }
  .modal {
    position: fixed;
    inset: 0;
    display: grid;
    place-items: center;
    background: var(--backdrop);
    z-index: 30;
  }
  .dialog {
    width: 420px;
    padding: 16px 18px;
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-m);
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .dialog p {
    margin: 0;
    font-size: var(--fs-xs);
  }
  .dlg-actions {
    display: flex;
    justify-content: flex-end;
    gap: 6px;
  }
</style>
