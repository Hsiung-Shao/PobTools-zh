<script lang="ts">
  // 版面殼:頂列 / 側欄 + 主視圖 / 狀態列。Phase 1 的主視圖:建置清單與天賦樹。
  import { onMount } from "svelte";
  import { api, bridge } from "$lib/bridge";
  import { loadLocale, t } from "$lib/i18n";
  import { app } from "$lib/state.svelte";
  import { prefs } from "$lib/prefs.svelte";
  import TitleBar from "./components/TitleBar.svelte";
  import StatusBar from "./components/StatusBar.svelte";
  import Sidebar from "./components/Sidebar.svelte";
  import BuildList from "./components/BuildList.svelte";
  import BuildBar from "./components/BuildBar.svelte";
  import TreeView from "./views/TreeView.svelte";
  import ImportView from "./views/ImportView.svelte";
  import ItemsView from "./views/ItemsView.svelte";
  import SkillsView from "./views/SkillsView.svelte";
  import ConfigView from "./views/ConfigView.svelte";
  import CalcsView from "./views/CalcsView.svelte";
  import NotesView from "./views/NotesView.svelte";
  import PartyView from "./views/PartyView.svelte";
  import CompareView from "./views/CompareView.svelte";
  import SettingsView from "./views/SettingsView.svelte";
  import type { ViewId } from "$lib/state.svelte";

  // Tabs by number, save by Ctrl+S: the shortcuts POB's own top bar has.
  const tabOrder: ViewId[] = ["builds", "tree", "items", "skills", "config", "calcs", "notes", "party", "import"];
  function onKey(e: KeyboardEvent) {
    // F2 first: it is the one shortcut that is not Ctrl-based, and it means the
    // same here as in the classic window -- show POB's original English.
    if (e.key === "F2" && !e.ctrlKey && !e.altKey && !e.shiftKey) {
      if (!app.has("translateToggle")) return;
      e.preventDefault();
      void app.toggleEnglish();
      return;
    }
    // Alt+Left / Alt+Right: the browser's back / forward, same as the mouse's side buttons
    if (e.altKey && !e.ctrlKey && !e.shiftKey && (e.key === "ArrowLeft" || e.key === "ArrowRight")) {
      e.preventDefault();
      navigate(e.key === "ArrowLeft");
      return;
    }
    if (!e.ctrlKey || e.altKey) return;
    const k = e.key.toLowerCase();
    if (k === "s" && !e.shiftKey && app.loaded) {
      e.preventDefault();
      void app.saveOrAsk();
      return;
    }
    // each tab has POB's own undo stack (UndoHandler); the tree view handles its own.
    // Notes and Party are not on the list: POB has no undo for them (tab_undo
    // errored) and they are text boxes, whose own Ctrl+Z is the undo that works.
    if ((k === "z" || k === "y") && !e.shiftKey && app.loaded && app.view !== "tree") {
      const tab = ({ items: "items", skills: "skills", config: "config", calcs: "calcs" } as Record<string, string>)[app.view];
      if (tab) {
        e.preventDefault();
        void app.run(() => api.tabUndo(tab, k === "y")).then((r) => r && app.refresh());
        return;
      }
    }
    // Build.lua: Ctrl+I opens the import tab, Ctrl+W leaves the build for the list
    if (k === "i" && !e.shiftKey && app.loaded) {
      e.preventDefault();
      app.view = "import";
      return;
    }
    if (k === "w" && !e.shiftKey && app.loaded) {
      e.preventDefault();
      app.view = "builds";
      return;
    }
    // window zoom, the browser's own shortcuts (WebView2's are switched off)
    if (k === "=" || k === "+") {
      e.preventDefault();
      void prefs.zoomBy(1);
      return;
    }
    if (k === "-") {
      e.preventDefault();
      void prefs.zoomBy(-1);
      return;
    }
    if (k === "0") {
      e.preventDefault();
      void prefs.set({ zoom: 100 });
      return;
    }
    const n = Number(e.key);
    if (n >= 1 && n <= tabOrder.length) {
      const id = tabOrder[n - 1];
      if (id === "builds" || app.loaded) {
        e.preventDefault();
        app.view = id;
      }
    }
  }

  // Back / forward. Not while a dialog is open: it would switch the screen under
  // it and leave the dialog (and whatever was typed in it) floating over another tab.
  function navigate(back: boolean) {
    if (document.querySelector('.modal, [aria-modal="true"]')) return;
    if (back) app.goBack();
    else app.goForward();
  }
  // The mouse's side buttons (button 3 = back, 4 = forward). preventDefault on
  // mouseup is also what stops WebView2 / the browser from running its own
  // history navigation on them.
  function onMouseUp(e: MouseEvent) {
    if (e.button !== 3 && e.button !== 4) return;
    e.preventDefault();
    navigate(e.button === 3);
  }
  // the unsaved-build question when the window's X is pressed
  const closeWindow = () => void bridge.call("host.close").catch(() => {});
  function closeDiscard() {
    app.closeAsk = false;
    closeWindow();
  }
  async function closeSave() {
    app.closeAsk = false;
    // a build with no file yet opens Save As; the window closes after it saves
    await app.saveOrAsk(closeWindow);
  }
  function blockSideButton(e: MouseEvent) {
    if (e.button === 3 || e.button === 4) e.preventDefault();
  }
  function onWheel(e: WheelEvent) {
    if (!e.ctrlKey) return;
    e.preventDefault();
    void prefs.zoomBy(e.deltaY < 0 ? 1 : -1);
  }

  let i18nReady = $state(false);
  onMount(() => {
    prefs.init();
    void loadLocale().then(() => (i18nReady = true));
  });
  // The background is on (data-bg, which app.css keys every see-through surface
  // on) when one is set -- on every tab, or with "only behind the passive tree"
  // just on that tab; every other tab then stays solid and draws no picture.
  const bgOn = $derived(prefs.bgSet && (!prefs.bgTreeOnly || app.view === "tree"));
  $effect(() => {
    document.documentElement.toggleAttribute("data-bg", bgOn);
  });
  // A video background stops while the window is hidden or minimized (the
  // WebView2 host marks the page hidden then) -- it would decode for nobody.
  let video = $state<HTMLVideoElement | null>(null);
  function syncVideo() {
    if (!video) return;
    if (document.hidden || !bgOn) video.pause();
    else void video.play().catch(() => {});
  }
  $effect(() => {
    void bgOn;
    void prefs.bgVideo;
    syncVideo();
  });
</script>

<svelte:window onkeydown={onKey} onwheel={onWheel} onmouseup={onMouseUp} onmousedown={blockSideButton} onauxclick={blockSideButton} />

{#if app.closeAsk}
  <!-- the window's X with an unsaved build (the classic window's CanExit) -->
  <div class="modal closeask" role="dialog" aria-modal="true">
    <div class="dialog">
      <div class="label">{t("close.title")}</div>
      <p>{t("close.unsaved", { name: app.info?.buildName ?? "" })}</p>
      <div class="dlg-actions">
        <button class="btn ghost" onclick={() => (app.closeAsk = false)}>{t("tree.cancel")}</button>
        <button class="btn danger" onclick={closeDiscard}>{t("close.discard")}</button>
        <button class="btn primary" onclick={closeSave}>{t("close.save")}</button>
      </div>
    </div>
  </div>
{/if}

<!-- keyed on the language flip: t() reads a plain table, so nothing re-renders
     by itself, and the views have to re-fetch because POB's outputRevision does
     not move when only the display language changed. -->
<!-- the background image (settings page "Background"; styled by the --bg-* vars lib/prefs sets) -->
<svelte:document onvisibilitychange={syncVideo} />
{#if prefs.bgVideo}
  <video class="bgimg" bind:this={video} src={prefs.bgVideo} autoplay muted loop playsinline disablepictureinpicture aria-hidden="true"></video>
{:else}
  <div class="bgimg" aria-hidden="true"></div>
{/if}
{#if i18nReady}
  {#key app.langRev}
  <div class="app">
    <TitleBar />
    {#if app.loaded && app.view !== "builds" && app.view !== "settings"}<BuildBar />{/if}
    <div class="body">
      <Sidebar />
      <main class="view">
        {#if app.engine === "gone"}
          <div class="center">
            <p class="bad">{t("app.childExited", { code: app.childExit ?? -1 })}</p>
            <button class="btn" onclick={() => bridge.call("host.restart_engine")}>{t("app.restartEngine")}</button>
          </div>
        {:else if app.view === "settings"}
          <SettingsView />
        {:else if app.engine === "booting"}
          <div class="center"><p class="dim pulse">{t("app.booting")}</p></div>
        {:else if app.gate && !app.gate.ok}
          <div class="center"><p class="bad">{t("app.gateFailed", { failed: app.gate.failed.join("; ") })}</p></div>
        {:else if app.view === "tree" && app.loaded}
          <TreeView />
        {:else if app.view === "import" && app.loaded}
          <ImportView />
        {:else if app.view === "items" && app.loaded}
          <ItemsView />
        {:else if app.view === "skills" && app.loaded}
          <SkillsView />
        {:else if app.view === "config" && app.loaded}
          <ConfigView />
        {:else if app.view === "calcs" && app.loaded}
          <CalcsView />
        {:else if app.view === "notes" && app.loaded}
          <NotesView />
        {:else if app.view === "party" && app.loaded}
          <PartyView />
        {:else if app.view === "compare" && app.loaded}
          <CompareView />
        {:else if app.view !== "builds" && app.loaded}
          <div class="center"><p class="dim">{t("app.viewSoon")}</p></div>
        {:else}
          <BuildList />
        {/if}
      </main>
    </div>
    <StatusBar />
  </div>
  {#if app.link !== "ok"}
    <div class="linkveil" role="alertdialog" aria-live="assertive">
      <div class="linkbox">
        {#if app.link === "closed"}
          <p class="linkhead">{t("app.linkClosed")}</p>
          <p class="dim">{t("app.linkClosedHint")}</p>
        {:else}
          <p class="linkhead bad">{t("app.linkLost")}</p>
          <p class="dim pulse">{t("app.linkLostHint")}</p>
        {/if}
      </div>
    </div>
  {/if}
  {/key}
{/if}

<style>
  .closeask {
    position: fixed;
    inset: 0;
    z-index: 400;
    display: grid;
    place-items: center;
    background: var(--backdrop);
  }
  .closeask .dialog {
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
  .closeask p {
    margin: 0;
  }
  .closeask .dlg-actions {
    display: flex;
    justify-content: flex-end;
    gap: 6px;
  }
  .bgimg {
    display: none;
    position: fixed;
    inset: 0;
    z-index: -1;
    pointer-events: none;
    /* no image: the theme's base colour, which the brightness then dims */
    background: var(--bg-image) center / cover no-repeat var(--surface-0-c);
    filter: brightness(var(--bg-bright, 1)) blur(var(--bg-blur, 0px));
    /* blur pulls the edges in; a little overscan keeps them off-screen */
    transform: scale(1.04);
  }
  video.bgimg {
    width: 100%;
    height: 100%;
    object-fit: cover;
  }
  :global(:root[data-bg]) .bgimg {
    display: block;
  }
  .app {
    height: 100%;
    display: flex;
    flex-direction: column;
  }
  .body {
    flex: 1;
    display: flex;
    min-height: 0;
  }
  .view {
    flex: 1;
    min-width: 0;
    min-height: 0;
    position: relative;
  }
  .center {
    height: 100%;
    display: grid;
    place-items: center;
    text-align: center;
  }
  .bad {
    color: var(--bad);
  }
  .linkveil {
    position: fixed;
    inset: 0;
    z-index: 1000;
    display: grid;
    place-items: center;
    background: color-mix(in srgb, var(--surface-0) 78%, transparent);
  }
  .linkbox {
    max-width: 440px;
    padding: 24px 28px;
    text-align: center;
    background: var(--surface-1);
    border: 1px solid var(--edge-0);
    border-radius: 8px;
  }
  .linkhead {
    font-size: 1.1em;
    font-weight: 600;
    margin: 0 0 8px;
  }
  .pulse {
    animation: pulse 1.6s ease-in-out infinite;
  }
</style>
