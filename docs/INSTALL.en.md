# PobTools installation guide

[繁體中文](INSTALL.md) | **English**

PobTools is a **Traditional Chinese localization tool for Path of Building (POB)**. It
injects the Chinese translation and UI from *outside* POB in a "zero-pollution" way and
**never modifies your original POB**.

> Unofficial fan-made tool, not affiliated with Grinding Gear Games.

---

## 1. What you need

1. **Windows** (10 / 11).
2. **Path of Building Community** (POB itself). PobTools **does not bundle** POB (for
   copyright / ownership reasons), so install it yourself:
   - POE1: [latest official POB](https://github.com/PathOfBuildingCommunity/PathOfBuilding/releases/latest)
   - POE2: [latest official POB for PoE2](https://github.com/PathOfBuildingCommunity/PathOfBuilding-PoE2/releases/latest)
   - or a POB folder that is already installed on your PC.

---

## 2. Installation steps

### Step 1. Download and extract PobTools

Download **`PobTools-<version>.zip`** and extract it to any folder (for example
`D:\PobTools\`). After extraction you will see `pob-zh.exe` together with the `engine`,
`Data` and `Fonts` folders.

### Step 2. Put the POB folder next to pob-zh.exe

Place the POB folder at the **same level** as `pob-zh.exe`. **The folder name does not
matter** — any folder containing `Launch.lua` is detected (`PathOfBuildingCommunity`,
`PathOfBuildingCommunity-Portable`, or a name of your own all work).

| Game            | How it is detected                                                             |
| --------------- | ------------------------------------------------------------------------------ |
| Path of Exile 1 | Any folder that contains `Launch.lua`                                          |
| Path of Exile 2 | The folder name must contain `PoE2` (e.g. `PathOfBuildingCommunity-PoE2-Portable`) |

The result looks like this (folder names are only examples):

```
D:\PobTools\
├─ pob-zh.exe
├─ engine\
├─ Data\
├─ Fonts\
├─ PathOfBuildingCommunity\              ← your POE1 POB
└─ PathOfBuildingCommunity-PoE2-Portable\ ← your POE2 POB (optional)
```

> If you only play POE1, put just that one folder; if you only play POE2, put just the
> folder whose name contains `PoE2`.
> Putting `pob-zh.exe` together with `engine\`, `Data\` and `Fonts\` *inside* the POB
> folder (`Launch.lua` and `pob-zh.exe` at the same level) is also supported, but the
> side-by-side layout above is recommended.
> If several matching folders exist, the official name is preferred; otherwise the first
> one in name order is used.

### Step 3. Launch

Double-click **`pob-zh.exe`**, pick the game version in the launcher and press
"Launch". POB opens in Traditional Chinese.

> How to use each feature (translation editor, filter editor, atlas strategy, font and
> language switching) is described in **[USAGE.en.md](USAGE.en.md)**.

---

## 3. Updating the translation (without re-downloading everything)

Translation data is its own release line, versioned `data-1`, `data-2`, … independently of
the program version. **The program normally updates it by itself**, so there is nothing to
do. To update manually, find the release tagged `data-<n>` on the Releases page, download
**`PobTools-Data-<n>.zip`**, extract it and **overwrite** the `Data` folder in your
PobTools installation folder with the one inside.

> If you do not want it overwritten automatically (for example because you edit the
> translations yourself): set "Automatically update translation data" to "No" on the
> Settings page. You will still be notified when new data is available and can apply it
> once with "Apply once now".

> The three files on a release page: `PobTools-<version>.zip` is the full package for a
> first install; `PobTools-update-<version>.zip` is what the program's auto-update uses
> and **contains no translation data** — downloading it by hand gives you a POB with no
> Chinese; `PobTools-Data-<n>.zip` contains only the translation data.

> The atlas (atlas passive tree) can be updated online from the toolbar button inside the
> program; manual file swapping is normally unnecessary.

---

## 4. FAQ

- **The launcher says "No POB detected"**
  → Check that the POB folder name is right (see the table above) and that it sits at
  the **same level** as `pob-zh.exe`.
- **Garbled text / no Chinese characters after launch**
  → Make sure the `Fonts\` folder contains a font file (`NotoSansTC-Regular.ttf` by
  default; check it was not lost during extraction).
- **I want a different font**
  → The "Font" dropdown at the bottom of the launcher switches between any `.ttf` in
  `Fonts\` on the spot. To add your own font, drop the `.ttf` into `Fonts\` and restart
  the launcher (a static TrueType font is recommended).
- **My antivirus blocks it**
  → This tool starts POB and injects the translation, which can trigger false positives;
  add it to your trusted list at your own discretion.
- **I want to change the UI language**
  → The language dropdown at the bottom of the launcher (Traditional Chinese / English).

---

## 5. License and support

The code is MIT-licensed; the translation data is derived from game content (copyright
GGG) and is for fan-tool use only. See [LICENSE](../LICENSE) and
[NOTICE.md](../NOTICE.md).

If this tool helps you, you are welcome to buy the author a coffee ☕ from the "About"
tab in the launcher.
