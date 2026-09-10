# PobTools usage guide

[繁體中文](USAGE.md) | **English**

This document explains how to use each PobTools feature. For a first install, read the
[installation guide INSTALL.en.md](INSTALL.en.md) first.

---

## 1. The launcher

Double-click `pob-zh.exe` to open the launcher. It automatically detects the POB folders
placed at the same level:

- Each detected game version is listed as a row showing the POB version number and a
  **Launch** button.
- The toolbar at the top opens the translation editor, filter editor, atlas strategy,
  timeless jewel calculator and regex generator.
- Tools open as separate windows (since v0.5.0); the launcher stays open, so several tools
  can be used at once.
- The status bar at the bottom switches the **UI language** (Traditional Chinese /
  Simplified Chinese / English) and the **font**.

### Switching the language

The language dropdown at the bottom switches the launcher and the POB UI language on the
spot. The matching translation data must exist (`zh-rTW` Traditional Chinese and `zh-rCN`
Simplified Chinese are shipped by default).

### Switching the font

The font dropdown at the bottom lists every `.ttf` in the `Fonts\` folder. To add your own
font, put the `.ttf` into `Fonts\` and restart the launcher (a static TrueType font is
recommended; a variable font must first be instanced to a single weight). The default is
Noto Sans TC (OFL license).

### Font size and window size

These two settings affect only the launcher and its tool windows; POB itself is untouched
(since v1.3.0):

- **Font size**: a slider in the "Interface" section of the Settings page, 14–26 px,
  default 19. The whole launcher UI (text, buttons, spacing) scales together and applies as
  soon as you release the slider; tools opened as separate windows (filter editor, atlas
  strategy, …) pick up the same size the next time they open. "Reset to default" returns
  to 19 px.
- **Window size**: the launcher window can simply be resized by dragging its edge; the
  size after release is remembered automatically. You can also type width x height
  directly in the "Interface" section of the Settings page. "Separate desktop windows" and
  "As tabs inside this launcher" each remember their own size independently. "Reset to
  default" returns to that mode's default (1000x700 for separate windows, 1500x950 for
  tabs, scaled by the display scaling).

### Appearance tab (look of the POB window)

The launcher's "Appearance" tab controls what the POB window looks like, with **one set of
settings each for PoE1 and PoE2** (the "Applies to" switch at the top of the page). Changes
apply live to that game's open POB windows; newly opened ones use them directly. The whole
tab appears on Windows only.

#### POB panel opacity

0%–100%, default 100% (opaque, i.e. off). This affects the **POB window itself**, not the
launcher, and the window **never shows the desktop through**:

- What fades is POB's **frame**: the background of the side bar (including the stat list
  box), the top bar, the toolbar at the bottom of the tree tab, and the separators between
  them. At 0% they disappear completely, leaving only text, icons and controls. Text, icons,
  buttons, dropdowns, tooltips and popups all stay opaque.
- Under the frame there is a fixed bottom layer: on the tree tab the passive tree canvas
  extends to the whole window, with the side bar and toolbars stacked on top; on the other
  tabs it is the usual striped backdrop. The part of the tree under the side bar can be
  seen but not clicked; the mouse still operates the side bar there.
- While dragging, open POB windows follow live; the value is written to the settings when
  you release. Newly opened POB windows use it directly. Both window modes are supported.
- Windows only; the option is not shown under macOS/CrossOver.

#### Background image and frost (frosted glass)

The other four settings on the same tab, also stored per game and all applied live:

- **Background image**: drop PNG/JPG/WebP files into `PobTools\Backgrounds\` ("Open
  folder" goes straight there; press "Refresh" after adding files) and pick one from the
  dropdown. The image fills the whole POB window as the bottom layer (cropped to fit, not
  moving with the passive tree), with the tree and the tabs drawn over it. "Default" goes
  back to POB's built-in striped backdrop. This folder is never overwritten by updates.
- **Background brightness**: how much the image is dimmed, default 50%, so it does not
  overpower nodes and text.
- **Frost strength**: turns the side bar, top bar and tree-tab toolbar into frosted glass —
  what is under a panel is blurred first, then the panel's tint goes on top (its density is
  the "panel opacity" above). 0% = no blur; combined with a panel opacity of 0–30% it looks
  most like liquid glass.
- **Tree backdrop**: the opacity of the dark tiled layer under the nodes, default 100%
  (same as stock). Lower it to see the background image on the tree tab too; at 0% it is
  not drawn at all and the nodes sit directly on the background image.

### Auto-update

The launcher checks GitHub for new versions once a day in the background (since v0.3.0):

- **Translation data updates** (data line): downloaded and applied automatically; a short
  "Translation data updated" notice appears at the top right, nothing to do. It takes
  effect the next time POB starts.
- **Program updates** (e.g. 0.3.x → 0.4.0): an orange "New version" button appears at the
  top right. Click it and the program downloads, swaps and restarts itself. The old files
  are kept as `.old` and cleaned up on the next start; if any step fails, the previous
  version is restored.
- Your personal settings (`pob-zh.ini`, the `PobTools\` folder) and the POB folder itself
  are never touched by an update.

Advanced: you can also run `pob-zh.exe --app-update-check` (check only) or
`pob-zh.exe --app-update` (check and update, no automatic restart) from the command line.

> **Networks that cannot reach GitHub** (e.g. regions that need a proxy): the "Network"
> section of the Settings page accepts an HTTP proxy (such as `127.0.0.1:7890`); leave it
> empty to follow the system proxy automatically, so the system proxy of tools like Clash /
> V2Ray works directly. Update checks, translation data and icon downloads all go through
> that proxy.

### Chinese search inside POB

Since v0.5.0, the unique / base item database search boxes on POB's Items tab accept
**Chinese item names directly** (English still works as before). This is done by an
in-memory patch at load time; the POB files are not modified. If a future POB update
removes the patch point, it silently falls back to English search without affecting other
features.

---

## 2. Translation editor

For viewing and editing the Traditional Chinese dictionaries — handy for fixing an
individual mistranslation or adding a new term.

- Search entries by the English source text and edit the Chinese translation directly.
- Saving writes back to `Data\<game>\<locale>\*.json`; it takes effect the next time POB
  starts.
- The dictionaries are plain JSON, so advanced users can edit the files directly.

---

## 3. Filter editor

A localized item-filter editor with a three-column block layout:

- **Left column**: the list of all rule blocks in the filter.
- **Middle column**: a card view of the selected block, showing its conditions (item class,
  rarity, tier, …) and styles (colours, border, sound, minimap icon). Item classes and
  terms are shown in Chinese with matching icons.
- **Right column**: add conditions / styles.
- **Sound management** tab: manage the alert sounds the filter uses.

The output is still a standard `.filter` file (localized display, English output that the
game can read) and can be dropped straight into the game's filter folder.

---

## 4. Atlas strategy

Plans the atlas passive tree. Open it from the launcher toolbar or directly with
`pob-zh.exe --atlas`.

### Basics

- **Pan**: drag the canvas; **zoom**: mouse wheel.
- **Allocate**: click a node to allocate / deallocate; the shortest path (BFS) from the
  start to the target is computed automatically.
- Node names and stats are shown in Chinese (F2 or the button switches to the English
  reference).

### Side-bar statistics

The right-hand panel aggregates the bonuses of the current allocation live:

- The top shows the passive points used and a progress bar.
- The search box filters the statistics and the node list by Chinese or English keywords.
- Clicking a node in the list **glides** the view to it and marks it with a gold ring.

### Multiple builds and sharing

- Several builds can be created and switched, added or removed from a dropdown.
- **Export** to a json file, or generate a **share code** (`PTAT1|...`); others can restore
  it by importing the file or pasting the code.

### Automatic updates for new leagues

The update button on the toolbar (or `pob-zh.exe --atlas-update`) checks GitHub for the
latest atlas data and downloads and hot-reloads the new league's tree and images in one
click. If the Chinese names have not caught up yet, new nodes show in English first and are
filled in automatically on the next check.

---

## 5. Timeless jewel calculator

Calculates how each Timeless Jewel seed changes the passive nodes:

- Choose the jewel type and faction, enter a seed or search by conditions, and it lists the
  matching node changes.
- Nodes and affixes are shown in Chinese; **the search box accepts Chinese keywords**.
- Trade-site search links can be generated directly (international and Taiwan realms).

---

## 6. Regex generator

Produces regular expressions for the in-game search box, similar to poe.re:

- Pick the game (POE1 / POE2) and then an item list; tick the affixes you want to match
  and the search string is composed live at the top — one click copies it into the
  in-game search box.
- POE2 offers waystone, tablet, relic and expedition-artifact lists.
- Frequently used combinations can be saved as **bookmarks** (kept separately per game).
- Besides avoiding the text of the other affixes in the list, the generated fragments also
  avoid text the in-game search box matches although it is not printed on the affix line:
  affix names, tiers, tags, tooltip descriptions, item names and the fixed lines every item
  has (since v1.3.0). Hover a list entry to see that text.

---

## 7. Updating the translation data

### Regular users

Normally nothing to do: translation data is its own release line (`data-1`, `data-2`, …)
and the program checks and updates it itself. The current translation data version is shown
at the bottom of the Settings page.

To swap it manually, find the release tagged `data-<n>` on the Releases page, download
`PobTools-Data-<n>.zip`, and overwrite the `Data` folder in the installation folder with
the one inside — including `Data\translations_version.json`, the version stamp; without it
the program thinks it has never updated.

If you edit the translations yourself: set "Automatically update translation data" to "No"
on the Settings page and updates will not overwrite your dictionaries (the program itself
still updates). You are still notified when new data is available and can take it once with
"Apply once now".

## 8. When POB or PobTools freezes

"Open the problem log folder" at the bottom of the Settings page opens
`PobTools\logs\`. Whenever something fails, the reason is written there; when
nothing is wrong the folder stays empty.

Since v1.4.0, **freezes and crashes leave a reason behind too**:

- **Not responding**: when POB's or the launcher's window has been frozen for
  more than 20 seconds, a `hang-<date>-<time>-<role>.txt` is written. It says
  what was being done at the time (loading the translation dictionaries, running
  one of POB's modules, …) and where each thread stopped, and
  `error-<date>.log` gets a one-line summary.
- **Crashes**: an unexpected exit leaves a `crash-<date>-<time>-<role>.txt` the
  same way.
- While POB is not responding, a line at the top of the launcher says the reason
  has been written down. It clears itself once POB responds again or is closed.
  **The launcher will not end POB for you** — sometimes it is only busy with a
  slow calculation, and killing it would throw away an unsaved build.
- Pauses caused by dragging the window, opening a menu or a file dialog do not
  count as "not responding" and produce nothing.
- Attaching that day's files to a bug report is the most useful thing you can do.
  They contain only the program's own code positions and its module list, never
  your build data.
- Like the logs, these files are kept for 30 days and then removed automatically.

## FAQ

Other installation and startup questions (POB not detected, garbled text, antivirus, …)
are covered in the [INSTALL.en.md FAQ](INSTALL.en.md#4-faq).
