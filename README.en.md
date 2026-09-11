# PobTools — Traditional Chinese launcher for Path of Building

[繁體中文](README.md) | **English**

A **Traditional Chinese localization tool for Path of Building (POB)** that uses the
local game client files as its translation source. It injects the translation and the
localized UI from *outside* POB in a "zero-pollution" way: **your original POB is never
modified**, and POB's own auto-update keeps working.

> Unofficial fan-made tool, not affiliated with Grinding Gear Games. Code is MIT-licensed.

💬 Questions, suggestions or feature requests: join the **[Discord community](https://discord.gg/6VamPQb8nC)**.

---

## What it is

Translation data for Path of Exile's Taiwanese-server terminology has never had a
stable source (community data lags behind patches, the official API goes offline).
PobTools takes the official client data as the reference, produces roughly 100,000
English → Chinese pairs, and builds them into an **engine-in-exe** launcher.

Because the translation lives inside the engine and the POB installation stays
completely clean, **POB can update however it likes and still runs** — which is exactly
the "localization breaks every time POB updates" problem this project set out to solve.

### Main features

| Feature                   | Description                                                                                   |
| ------------------------- | --------------------------------------------------------------------------------------------- |
| Localized POB launcher    | Supports POE1 and POE2; UI language switchable between Traditional Chinese / Simplified Chinese / Korean / English |
| Translation editor        | Built-in dictionary editor; edit or add translations on the spot                              |
| Filter editor             | NeverSink tier-list style item filter editor, with Chinese display and icons                  |
| Atlas strategy            | Atlas passive tree planner: multiple builds, export / share codes, **auto-update for new leagues** |
| Timeless jewel calculator | Timeless jewel seed calculation, with Chinese search and trade-site links                     |
| Regex generator           | Tick affixes to generate a regular expression for the in-game search box; POE1 and POE2 lists |
| Switchable font           | Noto Sans TC (OFL) by default; swap in any `.ttf` at any time                                 |

Usage details are in **[docs/USAGE.en.md](docs/USAGE.en.md)**.

---

## Download and install

1. Download **`PobTools-<version>.zip`** from [Releases](../../releases) and extract it.
2. Get **Path of Building Community** separately (PobTools does not bundle POB):
   [latest official POB release](https://github.com/PathOfBuildingCommunity/PathOfBuilding/releases/latest).
3. Full steps, folder layout, translation data updates and FAQ:
   **[docs/INSTALL.en.md](docs/INSTALL.en.md)**.
4. To translate PobTools into another language, or to review an existing translation, see
   **[docs/TRANSLATING.en.md](docs/TRANSLATING.en.md)**.

---

## Disclaimer

**You use this program at your own risk; all consequences are your own responsibility.**

### What it does and does not do

- **It does not modify the POB files on your disk.** Translation and UI are injected at
  launch time; the POB installation stays clean and POB's own auto-update keeps working.
- **It intercepts POB's text drawing and paste handling in memory**, replacing English
  with Traditional Chinese; it also patches some of POB's Lua scripts while they load —
  **in memory only, nothing is written back to your POB files**.
- **It never touches the Path of Exile game itself**: no injection into the game process,
  no reading or writing of game memory, no modification of game files. This program only
  interacts with POB, a third-party calculator.
- It writes settings, translation dictionaries and caches into its own folder and into
  the POB folder.
- The APIs it talks to (the official trade site, GitHub, poecdn icons) are all official,
  public resources. **Please do not hammer them with large numbers of requests in a short
  time.**

### Relationship with the official parties

This product isn't affiliated with or endorsed by Grinding Gear Games or Garena in any way.

This program is an **unofficial fan-made tool** with no affiliation to, and no endorsement
from, Grinding Gear Games or Garena. Path of Exile and all of its in-game content are
copyright Grinding Gear Games.

### Antivirus false positives and update verification

- This program is **not signed with a Windows code-signing certificate**. Combined with
  behaviour such as auto-updating, loading modules dynamically and patching POB scripts in
  memory, antivirus heuristics may flag it (an `!ml` suffix means a machine-learning
  guess). The source is public on GitHub — you are welcome to review and build it yourself.
- If you **download manually**, verify the hashes with the `SHA256SUMS-<version>.txt`
  attached to each release (`certutil -hashfile <file> SHA256`) to confirm the file is
  byte-for-byte identical to the published one.
- **Auto-update needs nothing from you**: since v0.26.0, both the program and the
  translation data are verified before installation against an ECDSA P-256 signature on
  the release manifest, using a **public key compiled into the executable**
  (public key: [`pob-zh-engine/host/update_pubkeys.h`](pob-zh-engine/host/update_pubkeys.h)).
  A missing or invalid signature **always rejects the install** — it never falls back to
  "install anyway".
- Note: this verifies that an update *came from the maintainer*. It is **not** Windows
  code signing and does not prevent the antivirus false positives described above.

---

## Data sources and licensing

The project's own original code is licensed under **MIT** (see [LICENSE](LICENSE)). Game
data is copyright **Grinding Gear Games**; the translation dataset in this project is for
fan-tool use only and is distributed with its attribution intact. Third-party components
that are bundled or depended on, and their licenses, are listed in
**[NOTICE.md](NOTICE.md)**.
