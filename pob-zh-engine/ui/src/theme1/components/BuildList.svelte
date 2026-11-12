<script lang="ts" src="$lib/views/BuildList.ts"></script>

<svelte:window onkeydown={onKey} />

<div class="list">
  <div class="bar">
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
    <span class="grow"></span>
    <input class="input search" type="search" placeholder={t("builds.searchHint")} bind:value={filter} oninput={onSearch} />
    <Select class="input sort" value={sortMode} onchange={(v) => refresh(subPath, { sortMode: String(v) })} title={t("builds.sort")} options={sortModes.map((s) => ({ value: s.sortMode, label: t(`builds.sort.${s.sortMode}`) === `builds.sort.${s.sortMode}` ? s.label : t(`builds.sort.${s.sortMode}`) }))} />
    {#if clip}
      <button class="btn sm" disabled={app.busy > 0 || !canMoveTo(clip.entry, subPath) && !(clip.copy && clip.entry.subPath === subPath)} onclick={paste}
        title={t(clip.copy ? "builds.pasteCopyHint" : "builds.pasteMoveHint", { name: nameOf(clip.entry) })}>{t("builds.paste")}</button>
    {/if}
    <button class="btn primary sm" disabled={app.busy > 0} onclick={() => app.newBuild(undefined, subPath)}>{t("builds.new")}</button>
    <button class="btn sm" disabled={app.busy > 0} onclick={() => (dialog = { kind: "newFolder", name: "" })}>{t("builds.newFolder")}</button>
    <span class="vsep"></span>
    {#if subPath}<button class="btn sm" title={t("builds.upHint")} onclick={up}>↑ {t("builds.up")}</button>{/if}
    <button class="btn sm" onclick={() => refresh()}>{t("builds.refresh")}</button>
    <button class="btn sm" onclick={() => bridge.call("host.open_folder", { path: buildPath + subPath })}>{t("builds.openFolder")}</button>
  </div>
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
      <span class="name">📁 ..</span>
      <span class="dim">{t("builds.up")}</span>
    </div>
  {/if}
  {#if loading && entries.length === 0}
    <div class="dim pad">{t("builds.loading")}</div>
  {:else if entries.length === 0}
    <div class="dim pad">{t("builds.empty", { path: buildPath + subPath })}</div>
  {:else}
    <div class="rows">
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
          <span class="name">{e.isFolder ? "📁 " : ""}{#if relPrefix(e)}<span class="dim">{relPrefix(e)}</span>{/if}{nameOf(e)}</span>
          <span class="acts">
            <button class="btn ghost sm" onclick={(ev) => { ev.stopPropagation(); open(e); }}>{t("builds.open")}</button>
            <button class="btn ghost sm" onclick={(ev) => { ev.stopPropagation(); dialog = { kind: "copy", entry: e, name: nameOf(e) }; }}>{t("builds.copy")}</button>
            <button class="btn ghost sm" title={t("builds.cutHint")} onclick={(ev) => { ev.stopPropagation(); clip = { entry: e, copy: false }; }}>{t("builds.cut")}</button>
            <button class="btn ghost sm" onclick={(ev) => { ev.stopPropagation(); dialog = { kind: "rename", entry: e, name: nameOf(e) }; }}>{t("builds.rename")}</button>
            <button class="btn ghost sm danger" onclick={(ev) => { ev.stopPropagation(); void remove(e); }}>{t("builds.delete")}</button>
          </span>
          {#if !e.isFolder}
            <span class="cls dim">{e.ascendClassName ?? e.className ?? ""}</span>
            <span class="lvl num dim">{e.level ? `${t("builds.level")} ${e.level}` : ""}</span>
            <span class="date num dim">{fmtDate(e.modified)}</span>
          {/if}
        </div>
      {/each}
    </div>
    <div class="foot">
      <span class="num">{t("builds.count", { n: entries.filter((e) => !e.isFolder).length })}</span>
      <span>{t("builds.keysHint")}</span>
    </div>
  {/if}

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
    min-height: 0;
  }
  .bar {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 10px;
    padding: 8px 14px;
    border-bottom: 1px solid var(--edge-0);
  }
  .search {
    width: 220px;
  }
  .bar :global(.sort) {
    width: auto;
  }
  .row.cut {
    opacity: 0.55;
  }
  .row.drop {
    outline: 1px dashed var(--gold);
  }
  .crumbs {
    display: flex;
    align-items: center;
    gap: 6px;
    min-width: 0;
    overflow: hidden;
    white-space: nowrap;
  }
  .crumb {
    border: 0;
    background: none;
    padding: 2px 4px;
    border-radius: var(--radius-s);
    color: var(--gold);
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
  .row.parent {
    margin-top: 6px;
  }
  .grow {
    flex: 1;
  }
  .vsep {
    width: 1px;
    height: 16px;
    background: var(--edge-1);
  }
  .pad {
    padding: 16px;
  }
  .rows {
    flex: 1;
    overflow-y: auto;
    padding: 6px 0;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 14px;
    padding: 6px 16px;
    cursor: default;
  }
  .row:hover {
    background: var(--surface-hover);
  }
  .row.sel {
    background: var(--surface-active, var(--surface-hover));
  }
  .name {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: var(--fs-md);
  }
  .folder .name {
    color: var(--gold);
  }
  /* 常駐但淡色,滑過或選取才變亮:以前只在 hover 出現,沒人知道列上有這些動作 */
  .acts {
    display: inline-flex;
    gap: 2px;
    opacity: 0.35;
  }
  .row:hover .acts,
  .row.sel .acts,
  .row:focus-within .acts {
    opacity: 1;
  }
  .foot {
    display: flex;
    gap: 14px;
    padding: 6px 16px;
    border-top: 1px solid var(--edge-0);
    font-size: var(--fs-xs);
    color: var(--ink-3);
  }
  .danger:hover {
    color: var(--bad);
  }
  .cls {
    width: 180px;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .lvl {
    width: 60px;
    text-align: right;
  }
  .date {
    width: 90px;
    text-align: right;
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
    border-left: 3px solid var(--gold);
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
