<!-- An item slot's drop-down that shows each candidate's tooltip while the list is
     open (ItemSlotControl: hovering an entry previews it against what is equipped).
     A native <select> cannot: its options get no mouse events. -->
<script lang="ts">
  import { t } from "$lib/i18n";

  type Option = { id: number; label: string; color: string };
  let {
    value,
    options,
    color,
    label,
    title = "",
    onpick,
    ontip,
    onleave,
  }: {
    value: number;
    options: Option[];
    /** the current item's rarity colour (or the "none" grey) */
    color: string;
    label: string;
    title?: string;
    onpick: (id: number) => void;
    /** hovering an entry (or the closed control): show that item's tooltip at the pointer */
    ontip: (id: number, e: MouseEvent) => void;
    onleave: () => void;
  } = $props();

  let open = $state(false);
  let btn = $state<HTMLButtonElement | null>(null);
  let pos = $state({ x: 0, y: 0, w: 0, up: false });
  let active = $state(-1);
  const all = $derived<Option[]>([{ id: 0, label: t("items.none"), color: "var(--ink-3)" }, ...options]);

  function toggle() {
    if (open) return close();
    if (!btn) return;
    const r = btn.getBoundingClientRect();
    // open upwards when the list would run off the bottom of the window
    const up = r.bottom + Math.min(all.length * 26 + 8, 320) > window.innerHeight;
    pos = { x: r.left, y: up ? r.top : r.bottom, w: r.width, up };
    active = all.findIndex((o) => o.id === value);
    open = true;
  }
  function close() {
    open = false;
    onleave();
  }
  function pick(id: number) {
    close();
    if (id !== value) onpick(id);
  }
  function onKey(e: KeyboardEvent) {
    if (!open) {
      if (e.key === "Enter" || e.key === " " || e.key === "ArrowDown") {
        e.preventDefault();
        toggle();
      }
      return;
    }
    if (e.key === "Escape") {
      e.preventDefault();
      close();
    } else if (e.key === "ArrowDown" || e.key === "ArrowUp") {
      e.preventDefault();
      active = Math.max(0, Math.min(all.length - 1, active + (e.key === "ArrowDown" ? 1 : -1)));
    } else if (e.key === "Enter") {
      e.preventDefault();
      if (all[active]) pick(all[active].id);
    }
  }
  function outside(e: PointerEvent) {
    if (!open) return;
    const el = e.target as HTMLElement | null;
    if (el?.closest(".slotlist") || el === btn || btn?.contains(el)) return;
    close();
  }
</script>

<svelte:window onpointerdown={outside} onblur={() => open && close()} />

<button
  bind:this={btn}
  class="select sm it slotdd"
  style:color={color}
  {title}
  aria-haspopup="listbox"
  aria-expanded={open}
  onclick={toggle}
  onkeydown={onKey}
  onmouseenter={(e) => !open && value && ontip(value, e)}
  onmouseleave={() => !open && onleave()}
>
  <span class="cur">{label}</span>
</button>

{#if open}
  <div
    class="slotlist"
    role="listbox"
    tabindex="-1"
    style:left={`${pos.x}px`}
    style:top={pos.up ? undefined : `${pos.y + 2}px`}
    style:bottom={pos.up ? `${window.innerHeight - pos.y + 2}px` : undefined}
    style:min-width={`${pos.w}px`}
    onmouseleave={onleave}
  >
    {#each all as o, i (o.id)}
      <div
        class="opt"
        class:sel={o.id === value}
        class:act={i === active}
        role="option"
        tabindex="-1"
        aria-selected={o.id === value}
        style:color={o.color}
        onmouseenter={(e) => {
          active = i;
          if (o.id) ontip(o.id, e);
          else onleave();
        }}
        onclick={() => pick(o.id)}
        onkeydown={() => {}}
      >
        {o.label}
      </div>
    {/each}
  </div>
{/if}

<style>
  .slotdd {
    display: flex;
    align-items: center;
    text-align: left;
    cursor: pointer;
    min-width: 0;
  }
  .cur {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  /* fixed, so the slot column's scroll box does not clip it */
  .slotlist {
    position: fixed;
    z-index: 60;
    max-height: 320px;
    max-width: 420px;
    overflow-y: auto;
    padding: 4px;
    background: var(--surface-2);
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-m);
    box-shadow: var(--shadow-float);
  }
  .opt {
    padding: 4px 8px;
    border-radius: var(--radius-s);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    cursor: pointer;
    font-size: var(--fs-sm);
  }
  .opt.act {
    background: var(--surface-hover);
  }
  .opt.sel {
    font-weight: 600;
  }
</style>
