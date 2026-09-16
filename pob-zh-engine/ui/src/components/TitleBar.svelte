<script lang="ts">
  import { api } from "$lib/bridge";
  import { t } from "$lib/i18n";
  import { app, type ViewId } from "$lib/state.svelte";

  const tabs: { id: ViewId; key: string }[] = [
    { id: "builds", key: "title.builds" },
    { id: "tree", key: "title.tree" },
  ];

  // The OS title carries the build name for the taskbar.
  $effect(() => {
    const name = app.info?.buildName;
    void api.setTitle(name ? `${name}${app.info?.unsaved ? " •" : ""}` : "");
  });
</script>

<header class="titlebar">
  <span class="brand">PobTools</span>
  <nav>
    {#each tabs as tb}
      <button class="tab" class:on={app.view === tb.id} disabled={tb.id !== "builds" && !app.loaded} onclick={() => (app.view = tb.id)}>
        {t(tb.key)}
      </button>
    {/each}
  </nav>
  <span class="grow"></span>
  {#if app.info}
    <span class="bname">{app.info.buildName}</span>
    <span class="dim">{app.info.ascendClassNameZh ?? app.info.classNameZh ?? ""}</span>
    {#if app.info.unsaved}<span class="warn">{t("title.unsaved")}</span>{/if}
  {/if}
</header>

<style>
  .titlebar {
    height: var(--titlebar-h);
    display: flex;
    align-items: center;
    gap: 14px;
    padding: 0 14px;
    background: var(--bg-1);
    border-bottom: 1px solid var(--line-0);
  }
  .brand {
    font-weight: 600;
    letter-spacing: 0.04em;
  }
  nav {
    display: flex;
    gap: 2px;
  }
  .tab {
    appearance: none;
    border: 0;
    background: none;
    padding: 6px 12px;
    border-radius: var(--r-1);
    color: var(--fg-2);
    font-size: var(--fs-sm);
  }
  .tab:hover:not(:disabled) {
    color: var(--fg-0);
    background: var(--bg-hover);
  }
  .tab.on {
    color: var(--fg-0);
    background: var(--bg-active);
  }
  .tab:disabled {
    opacity: var(--fade-off);
  }
  .grow {
    flex: 1;
  }
  .bname {
    font-weight: 600;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    max-width: 40vw;
  }
  .warn {
    color: var(--warn);
    font-size: var(--fs-xs);
  }
</style>
