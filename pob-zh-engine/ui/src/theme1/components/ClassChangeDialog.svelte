<!-- POB's "Class Change" confirm popup (Build.lua:266 / PassiveTreeView's
     ascendancy click): reset the tree, or connect a path to the new class's
     start and keep the points. Shared by the build bar and the tree view. -->
<script lang="ts">
  import { t } from "$lib/i18n";

  let {
    className,
    connectFailed = false,
    onanswer,
    oncancel,
  }: {
    className: string;
    connectFailed?: boolean;
    onanswer: (mode: "reset" | "connect") => void;
    oncancel: () => void;
  } = $props();
</script>

<div class="modal" role="dialog" aria-modal="true">
  <div class="dialog">
    <div class="title">{t("tree.classChangeTitle")}</div>
    <p>{t("tree.classChangeBody", { className })}</p>
    {#if connectFailed}<p class="bad">{t("tree.classChangeConnectFailed")}</p>{/if}
    <div class="actions">
      <button class="btn" onclick={() => onanswer("connect")} disabled={connectFailed}>{t("tree.classChangeConnect")}</button>
      <button class="btn primary" onclick={() => onanswer("reset")}>{t("tree.classChangeReset")}</button>
      <button class="btn ghost" onclick={oncancel}>{t("tree.cancel")}</button>
    </div>
  </div>
</div>

<style>
  .modal {
    position: fixed;
    inset: 0;
    display: grid;
    place-items: center;
    background: var(--backdrop);
    z-index: 30;
  }
  .dialog {
    width: 440px;
    padding: 16px 18px;
    background: var(--surface-1);
    border: 1px solid var(--edge-1);
    border-left: 3px solid var(--gold);
    border-radius: var(--radius-m);
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .title {
    font-size: var(--fs-xs);
    letter-spacing: 0.06em;
    color: var(--ink-2);
  }
  .dialog p {
    margin: 0;
    font-size: var(--fs-sm);
    color: var(--ink-1);
    line-height: 1.5;
  }
  .bad {
    color: var(--bad);
  }
  .actions {
    display: flex;
    gap: 6px;
    justify-content: flex-end;
    flex-wrap: wrap;
  }
</style>
