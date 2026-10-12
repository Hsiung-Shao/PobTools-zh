<!-- The shared drop-down that replaces the native select element. WebView2 places a
     native select's list on its own, and a long one runs past the window (over
     the taskbar); this one is placed by us: below the trigger when it fits,
     otherwise above, capped to the room that side has, and kept inside the
     window horizontally. Long lists (> 25 options) get a search box on top.
     Keyboard: ↑↓ PageUp PageDown Home End Enter Space Esc Tab, and typing
     jumps to the next option starting with the typed letters (also while
     closed). Purely controlled: it shows `value` and reports picks through
     `onchange`; `action` turns it into a menu that always shows the placeholder. -->
<script module lang="ts">
  let counter = 0;
</script>

<script lang="ts">
  import { tick } from "svelte";
  import { t } from "$lib/i18n";
  import { filterOptions, moveIndex, placeList, plainLabel, typeahead, type SelOption } from "$lib/select";

  type V = string | number;
  let {
    value,
    options,
    onchange,
    disabled = false,
    title,
    ariaLabel,
    placeholder = "",
    class: cls = "",
    color,
    action = false,
    searchable,
  }: {
    value?: V | null;
    options: SelOption[];
    onchange?: (v: V) => void;
    disabled?: boolean;
    title?: string;
    ariaLabel?: string;
    placeholder?: string;
    class?: string;
    /** the trigger's text colour (defaults to the current option's colour) */
    color?: string;
    /** a menu: picking reports the option and the trigger keeps showing the placeholder */
    action?: boolean;
    /** force the search box on / off (default: on above 25 options) */
    searchable?: boolean;
  } = $props();

  const SEARCH_AT = 25;
  const MAX_H = 360;
  const SIZER_MAX = 80;
  const listId = `sel-list-${++counter}`;

  let open = $state(false);
  let btn = $state<HTMLButtonElement | null>(null);
  let listEl = $state<HTMLDivElement | null>(null);
  let optsEl = $state<HTMLDivElement | null>(null);
  let searchEl = $state<HTMLInputElement | null>(null);
  let query = $state("");
  let active = $state(-1);
  let pos = $state<{ left: number; top?: number; bottom?: number; minW: number; maxH: number }>({ left: 0, top: 0, minW: 0, maxH: MAX_H });

  const same = (a: V | null | undefined, b: V | null | undefined) => a != null && b != null && String(a) === String(b);
  const hasSearch = $derived(searchable ?? options.length > SEARCH_AT);
  const shown = $derived(hasSearch && query ? filterOptions(options, query) : options);
  const selIdx = $derived(options.findIndex((o) => same(o.value, value)));
  const current = $derived(action ? undefined : options[selIdx]);
  const label = $derived(current ? plainLabel(current.label) : placeholder);
  const sizer = $derived(options.length <= SIZER_MAX ? [placeholder, ...options.map((o) => plainLabel(o.label))].filter((s) => s) : []);

  /** moves the list node to <body>, so no transformed / clipping ancestor affects it */
  function portal(node: HTMLElement) {
    document.body.appendChild(node);
    return {
      destroy() {
        node.remove();
      },
    };
  }

  function place() {
    if (!btn || !listEl) return;
    const r = btn.getBoundingClientRect();
    const vh = window.innerHeight;
    const vw = window.innerWidth;
    const natural = listEl.offsetHeight - (optsEl?.clientHeight ?? 0) + (optsEl?.scrollHeight ?? 0);
    const p = placeList({ below: vh - r.bottom, above: r.top, listH: Math.min(natural, MAX_H), margin: 10 });
    const w = listEl.offsetWidth;
    pos = {
      left: Math.max(8, Math.min(r.left, vw - w - 8)),
      top: p.dir === "down" ? r.bottom + 2 : undefined,
      bottom: p.dir === "up" ? vh - r.top + 2 : undefined,
      minW: r.width,
      maxH: Math.max(60, p.maxH),
    };
  }

  function scrollActive(center = false) {
    if (!optsEl || active < 0) return;
    const el = optsEl.querySelector<HTMLElement>(`[data-i="${active}"]`);
    if (!el) return;
    const top = el.offsetTop;
    const bottom = top + el.offsetHeight;
    if (center) optsEl.scrollTop = Math.max(0, top - (optsEl.clientHeight - el.offsetHeight) / 2);
    else if (top < optsEl.scrollTop) optsEl.scrollTop = top;
    else if (bottom > optsEl.scrollTop + optsEl.clientHeight) optsEl.scrollTop = bottom - optsEl.clientHeight;
  }

  async function openList(at?: number) {
    if (disabled || open || !btn) return;
    query = "";
    active = at ?? (selIdx >= 0 ? selIdx : moveIndex(options, -1, 1));
    const r = btn.getBoundingClientRect();
    pos = { left: r.left, top: r.bottom + 2, minW: r.width, maxH: MAX_H };
    open = true;
    await tick();
    place();
    await tick();
    place();
    scrollActive(true);
    if (hasSearch) searchEl?.focus({ preventScroll: true });
  }

  function close(focus: boolean) {
    if (!open) return;
    open = false;
    query = "";
    if (focus) btn?.focus({ preventScroll: true });
  }

  function pick(o: SelOption | undefined) {
    if (!o || o.disabled) return;
    close(true);
    if (action || !same(o.value, value)) onchange?.(o.value);
  }

  // typeahead: letters typed within 700 ms build one prefix; a repeated single
  // letter cycles through the options starting with it
  let buf = "";
  let bufTimer = 0;
  const startsWith = (o: SelOption | undefined, q: string) => !!o && !o.disabled && plainLabel(o.label).trimStart().toLowerCase().startsWith(q);
  function typeJump(ch: string) {
    clearTimeout(bufTimer);
    bufTimer = window.setTimeout(() => (buf = ""), 700);
    buf += ch.toLowerCase();
    const q = [...buf].every((c) => c === buf[0]) ? buf[0] : buf;
    const list = open ? shown : options;
    const from = open ? active : selIdx;
    const start = q.length > 1 ? from - 1 : from;
    const i = typeahead(list, start, q);
    if (!startsWith(list[i], q)) return;
    if (open) {
      active = i;
      scrollActive();
    } else if (action) {
      void openList(i);
    } else if (!same(list[i].value, value)) {
      onchange?.(list[i].value);
    }
  }
  const printable = (e: KeyboardEvent) => e.key.length === 1 && e.key !== " " && !e.ctrlKey && !e.metaKey && !e.altKey;

  // a key the list handles goes no further: page shortcuts (the tree's letter
  // keys, a dialog's Esc) must not fire while choosing
  const eat = (e: KeyboardEvent) => {
    e.preventDefault();
    e.stopPropagation();
  };
  function onKey(e: KeyboardEvent, fromSearch = false) {
    if (e.isComposing || e.keyCode === 229) return;
    if (!open) {
      if (e.key === "Enter" || e.key === " " || e.key === "ArrowDown" || e.key === "ArrowUp") {
        eat(e);
        void openList();
      } else if (printable(e)) {
        eat(e);
        typeJump(e.key);
      }
      return;
    }
    const move = (d: number) => {
      eat(e);
      active = active < 0 ? moveIndex(shown, d > 0 ? -1 : shown.length, d > 0 ? 1 : -1) : moveIndex(shown, active, d);
      scrollActive();
    };
    switch (e.key) {
      case "ArrowDown":
        return move(1);
      case "ArrowUp":
        return move(-1);
      case "PageDown":
        return move(10);
      case "PageUp":
        return move(-10);
      case "Home":
        if (fromSearch) return;
        eat(e);
        active = moveIndex(shown, -1, 1);
        return scrollActive();
      case "End":
        if (fromSearch) return;
        eat(e);
        active = moveIndex(shown, shown.length, -1);
        return scrollActive();
      case "Enter":
        eat(e);
        return pick(shown[active]);
      case " ":
        if (fromSearch) return;
        eat(e);
        return pick(shown[active]);
      case "Escape":
        eat(e);
        return close(true);
      case "Tab":
        // leave the focus where Tab would go from the trigger
        if (fromSearch) btn?.focus({ preventScroll: true });
        open = false;
        query = "";
        return;
      default:
        if (!fromSearch && printable(e)) {
          eat(e);
          typeJump(e.key);
        }
    }
  }

  function onSearch() {
    active = moveIndex(shown, -1, 1);
    if (optsEl) optsEl.scrollTop = 0;
    void tick().then(place);
  }

  $effect(() => {
    if (!open) return;
    const inside = (n: EventTarget | null) => n instanceof Node && (!!btn?.contains(n) || !!listEl?.contains(n));
    const onScroll = (e: Event) => {
      if (!inside(e.target)) close(false);
    };
    const onDown = (e: PointerEvent) => {
      if (!inside(e.target)) close(false);
    };
    const onResize = () => place();
    const onBlur = () => close(false);
    window.addEventListener("scroll", onScroll, true);
    window.addEventListener("pointerdown", onDown, true);
    window.addEventListener("resize", onResize);
    window.addEventListener("blur", onBlur);
    return () => {
      window.removeEventListener("scroll", onScroll, true);
      window.removeEventListener("pointerdown", onDown, true);
      window.removeEventListener("resize", onResize);
      window.removeEventListener("blur", onBlur);
    };
  });
</script>

<button
  bind:this={btn}
  type="button"
  class={["select", cls, "seltrig"]}
  class:ph={!current}
  style:color={color ?? current?.color}
  title={title ?? (current ? (current.title ?? label) : undefined)}
  aria-label={ariaLabel}
  aria-haspopup="listbox"
  aria-expanded={open}
  aria-controls={open ? listId : undefined}
  {disabled}
  onclick={() => (open ? close(true) : openList())}
  onkeydown={(e) => onKey(e)}
>
  <span class="cur">{label}</span>
  {#each sizer as s}<span class="sz" aria-hidden="true">{s}</span>{/each}
</button>

{#if open}
  <div
    use:portal
    bind:this={listEl}
    class="sellist"
    style:left={`${pos.left}px`}
    style:top={pos.top != null ? `${pos.top}px` : undefined}
    style:bottom={pos.bottom != null ? `${pos.bottom}px` : undefined}
    style:min-width={`${pos.minW}px`}
    style:max-height={`${pos.maxH}px`}
  >
    {#if hasSearch}
      <input
        bind:this={searchEl}
        bind:value={query}
        class="input sm selsearch"
        placeholder={t("edit.search")}
        aria-label={t("edit.search")}
        aria-controls={listId}
        aria-activedescendant={active >= 0 ? `${listId}-${active}` : undefined}
        oninput={onSearch}
        onkeydown={(e) => onKey(e, true)}
      />
    {/if}
    <div bind:this={optsEl} id={listId} class="opts" role="listbox" tabindex="-1" aria-label={ariaLabel ?? title}>
      {#each shown as o, i (i)}
        <div
          id={`${listId}-${i}`}
          data-i={i}
          class="opt"
          class:act={i === active}
          class:sel={!action && same(o.value, value)}
          class:dis={o.disabled}
          role="option"
          tabindex="-1"
          aria-selected={!action && same(o.value, value)}
          aria-disabled={o.disabled || undefined}
          title={o.title}
          style:color={o.color}
          onmousemove={() => {
            if (active !== i && !o.disabled) active = i;
          }}
          onmousedown={(e) => e.preventDefault()}
          onclick={() => pick(o)}
          onkeydown={() => {}}
        >
          {plainLabel(o.label) || " "}
        </div>
      {:else}
        <div class="empty">—</div>
      {/each}
    </div>
  </div>
{/if}

<style>
  /* looks like the native select it replaces (.select in app.css); the hidden
     .sz copies give it the native width: as wide as its widest option */
  .seltrig {
    display: inline-grid;
    grid-template-columns: minmax(0, 1fr);
    align-items: center;
    justify-items: start;
    text-align: left;
    overflow: hidden;
    white-space: nowrap;
    cursor: pointer;
  }
  .seltrig:disabled {
    cursor: default;
    opacity: var(--dim-opacity);
  }
  .seltrig > span {
    grid-area: 1 / 1;
    max-width: 100%;
  }
  .cur {
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .seltrig.ph .cur {
    color: var(--ink-2);
  }
  .sz {
    visibility: hidden;
    height: 0;
    overflow: hidden;
  }
  /* fixed and moved to <body>, so no scroll box or panel clips it */
  .sellist {
    position: fixed;
    z-index: 1100;
    display: flex;
    flex-direction: column;
    gap: 4px;
    max-width: min(560px, calc(100vw - 16px));
    padding: 4px;
    background: var(--surface-2-c);
    border: 1px solid var(--edge-1);
    border-radius: var(--radius-m);
    box-shadow: var(--shadow-float);
    color: var(--ink-0);
    font-size: var(--fs-sm);
  }
  .selsearch {
    flex: 0 0 auto;
    height: 24px;
    font-size: var(--fs-xs);
  }
  .opts {
    flex: 1 1 auto;
    min-height: 0;
    overflow-y: auto;
    outline: none;
  }
  .opt {
    padding: 3px 8px;
    border-radius: var(--radius-s);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    cursor: pointer;
  }
  .opt.act {
    background: var(--surface-hover);
  }
  .opt.sel {
    font-weight: 600;
    box-shadow: inset 2px 0 0 var(--accent);
  }
  .opt.dis {
    opacity: var(--dim-opacity);
    cursor: default;
  }
  .empty {
    padding: 3px 8px;
    color: var(--ink-3);
  }
</style>
