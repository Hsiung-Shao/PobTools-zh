<!-- 頂列:品牌、分頁、右側目前建置。分頁用底線標示,不用膠囊。 -->
<script lang="ts">
  import { api } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app, type ViewId } from "$lib/state.svelte";

  const tabs: { id: ViewId; key: string }[] = [
    { id: "builds", key: "title.builds" },
    { id: "tree", key: "title.tree" },
    { id: "items", key: "title.items" },
    { id: "skills", key: "title.skills" },
    { id: "config", key: "title.config" },
    { id: "calcs", key: "title.calcs" },
    { id: "notes", key: "title.notes" },
    { id: "party", key: "title.party" },
    { id: "import", key: "title.import" },
  ];

  // The OS window title carries the build name for the taskbar.
  $effect(() => {
    const name = app.info?.buildName;
    void api.setTitle(name ? `${name}${app.info?.unsaved ? " •" : ""}` : "");
  });
</script>

<header class="top">
  <span class="brand"><i class="mark"></i>PobTools</span>
  <nav>
    {#each tabs as tb}
      <button class="tab" class:on={app.view === tb.id} disabled={tb.id !== "builds" && !app.loaded} onclick={() => (app.view = tb.id)}>
        {t(tb.key)}
      </button>
    {/each}
  </nav>
  <span class="grow"></span>
  <button class="gear" class:on={app.view === "settings"} title={t("settings.title")} aria-label={t("settings.title")} onclick={() => (app.view = "settings")}>
    <svg viewBox="0 0 20 20" width="16" height="16" aria-hidden="true">
      <path
        fill="currentColor"
        d="M8.6 1.5h2.8l.4 2.1c.5.2 1 .4 1.4.7l2-.8 1.4 2.4-1.6 1.4c.1.5.1 1 0 1.5l1.6 1.4-1.4 2.4-2-.8c-.4.3-.9.6-1.4.7l-.4 2.1H8.6l-.4-2.1a6 6 0 0 1-1.4-.7l-2 .8-1.4-2.4 1.6-1.4a6 6 0 0 1 0-1.5L3.4 5.9l1.4-2.4 2 .8c.4-.3.9-.6 1.4-.7l.4-2.1ZM10 7a3 3 0 1 0 0 6 3 3 0 0 0 0-6Z"
      />
    </svg>
  </button>
  {#if app.info}
    <span class="build">
      <span class="bname">{app.info.buildName}</span>
      <span class="bcls">{app.info.ascendClassNameZh ?? app.info.classNameZh ?? ""}</span>
      {#if app.info.unsaved}<span class="unsaved">{t("title.unsaved")}</span>{/if}
    </span>
  {/if}
</header>

<style>
  .top {
    height: var(--titlebar-h);
    display: flex;
    align-items: center;
    gap: 18px;
    padding: 0 16px;
    background: var(--surface-1);
    border-bottom: 1px solid var(--edge-0);
  }
  .brand {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    font-weight: 600;
    letter-spacing: 0.05em;
  }
  .mark {
    width: 10px;
    height: 10px;
    background: var(--gold);
    transform: rotate(45deg);
    border-radius: 2px;
  }
  nav {
    display: flex;
    gap: 4px;
  }
  .tab {
    appearance: none;
    border: 0;
    border-bottom: 2px solid transparent;
    background: none;
    padding: 0 10px;
    color: var(--ink-2);
    font-size: var(--fs-sm);
    letter-spacing: 0.02em;
  }
  .tab:hover:not(:disabled) {
    color: var(--ink-0);
  }
  .tab.on {
    color: var(--ink-0);
    border-bottom-color: var(--gold);
  }
  .tab:disabled {
    opacity: var(--dim-opacity);
  }
  .grow {
    flex: 1;
  }
  .gear {
    appearance: none;
    border: 0;
    background: none;
    color: var(--ink-2);
    width: 28px;
    height: 28px;
    border-radius: var(--radius-s);
    display: inline-grid;
    place-items: center;
    cursor: pointer;
  }
  .gear:hover,
  .gear.on {
    color: var(--ink-0);
    background: var(--surface-hover);
  }
  .build {
    display: inline-flex;
    align-items: center;
    gap: 10px;
    min-width: 0;
  }
  .bname {
    font-weight: 600;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    max-width: 40vw;
  }
  .bcls {
    color: var(--gold);
    font-size: var(--fs-sm);
  }
  .unsaved {
    color: var(--warn);
    font-size: var(--fs-xs);
    padding: 1px 6px;
    border: 1px solid var(--warn);
    border-radius: 9px;
  }
</style>
