# Translating PobTools into another language

This page is for translators: how a language is packaged, what the files must
look like, how to test a translation locally, and how to send it in.
Nothing here needs a compiler — a text editor and PobTools itself are enough.

繁體中文版:[TRANSLATING.md](TRANSLATING.md)

## How languages work

PobTools finds languages **on disk**. There is no list to register in. A language
is three folders, one per "slot", each holding a `meta.json`:

```
Data\
  poe1\<locale>\      dictionaries used when Path of Building for PoE1 is running
  poe2\<locale>\      dictionaries used when Path of Building for PoE2 is running
  launcher\<locale>\  the launcher's own labels (buttons, settings, messages)
```

`<locale>` is a folder name such as `zh-rTW`, `zh-rCN`, `ko-KR`, `ja-JP`. Drop the
folders in, restart the launcher, and the language appears in the picker at the
bottom of the window. A locale that exists for only one game is offered with a
"(PoE1 only)" / "(PoE2 only)" mark. `en` is always present and has no folder —
whatever has no dictionary is shown in the original English.

Shipped today: `zh-rTW` (Traditional Chinese, the reference set), `zh-rCN`
(Simplified Chinese), `ko-KR` (Korean).

### `meta.json`

```json
{
  "version": "1.0.0",
  "locale": "ko-KR",
  "display_name": "한국어",
  "source": "poe1",
  "load_order": ["tags.json", "items.json", "gems.json", "ui.json",
                 "stats.json", "passives.json", "uniques.json", "monsters.json"],
  "incomplete_translation_whitelist": ["DPS", "PoB", "DoT", "AoE"],
  "glossary_blacklist": ["UNUSED"]
}
```

| field | meaning |
|---|---|
| `display_name` | what the language picker shows. Optional; the folder name is used when missing. |
| `load_order` | the dictionary files, **in loading order**. Only files listed here are loaded. |
| `incomplete_translation_whitelist` | tokens that are allowed to stay English inside a translated line (abbreviations). |
| `glossary_blacklist` | keys never offered by the in-app editor's glossary. |

All dictionaries of one locale are merged into **one lookup table, later files
win**. That is why `tags.json` (short affix fragments such as "Fire", "Cold")
comes first and the authoritative files (`ui.json`, `stats.json`, `gems.json`…)
come later: a fragment must never overwrite a full name. Keep the reference
`load_order` unless you know why you are changing it.

The launcher slot is one file: `load_order: ["launcher.json"]`.

## Dictionary files

Every dictionary is a JSON object with an `entries` map:

```json
{
  "source_files": ["Items_Armour.csv", "Items_Weapons.csv"],
  "is_base_items": true,
  "entries": {
    "Chaos Orb": "카오스 오브",
    "{0}% increased maximum Life": "최대 생명력 {0}% 증가",
    "Adds {0} to {1} Cold Damage": "냉기 피해 {0}~{1} 추가"
  }
}
```

- **The key is the English text exactly as Path of Building draws it** — same
  spelling, same punctuation, same spaces, same line breaks (`\n` inside the
  JSON string). PobTools looks the drawn string up verbatim; a key that differs
  by one character is never found.
- `source_files` and `is_base_items` are bookkeeping copied from the reference
  set; keep them as they are. `is_base_items: true` marks the file whose entries
  are item base types (used when you paste an item written in your language back
  into PoB).
- A missing key simply shows the English text. **Leave a key out rather than
  guess** — a wrong translation is worse than an English line.

### Placeholders and colour codes — keep them exactly

| in the key | in your value | rule |
|---|---|---|
| `{0}`, `{1}`, `{0:+d}` | the same set of placeholders | each index must appear, in whatever order your language needs. The index decides which number goes where; never renumber. |
| `#` | `#` | a bare `#` is a number slot in stat templates. In PoB's own config labels (`# of Poison on enemy:`) it means "number of" and may become a word. |
| `^7`, `^xRRGGBB` | the same codes, same count | colour codes. PobTools translates colour-delimited segments separately, so keys rarely contain them; if one does, keep every code. |
| `[Term|Display text]` | `[Term|your text]` | game-file markup: the part before `|` is a link target and must stay English; translate only the part after. |

A `{0}`-style value taken from a `#`-style source (the trade site API, for
example) can only be refilled safely when there is exactly one placeholder;
with two or more, word order in your language decides which number is which and
a left-to-right refill silently swaps them. Take such lines from the game files
instead, where the indices are already right.

### Where the text comes from — and the rule about guessing

PobTools follows one rule for every language: **official game text first, and
if the official text does not cover something, it stays English and goes on a
list**. In order of authority:

1. **The game client's own files** (the GGPK) in your language — item, gem,
   passive, unique, monster and stat names exactly as the game prints them.
2. **The official trade site's data API** for your realm
   (`https://<realm>.pathofexile.com/api/trade/data/stats`, joined to the
   English realm by `id`). Same words as the game, useful as a second witness.
3. **Community translations** (for example an older fan tool) — only for the
   text that does not exist in the game at all: Path of Building's own tabs,
   calculation labels, config options, settings. This is roughly half of every
   dictionary, and it is where a translator's own work goes.

Where a community source and the official source disagree, the official value
is used and the disagreement is written to a conflicts list. Please do the same
in your own edits: if the game already has a term, use the game's term.

`ko-KR` was built exactly this way in September 2026: ~82% of the reference keys
are covered (official game files and trade API for game content, the PoeCharm3
community files for PoB's own interface); the remaining lines show English.
The launcher labels for Korean are a **machine draft, not yet reviewed by a
native speaker** — that file (`Data\launcher\ko-KR\launcher.json`) is the first
place a Korean translator can help.

## The launcher's own labels (`Data\launcher\<locale>\launcher.json`)

```json
{
  "entries": {
    "Launch": "실행",
    "Settings": "설정",
    "Check for updates": "업데이트 확인"
  }
}
```

The keys are the English labels compiled into the launcher; any key you leave
out falls back to the English label, so a partial file is fine. To get the
complete list of keys as a template for a new language:

```
pob-zh.exe --launcher-strings-export ko-KR
```

writes `Data\launcher\ko-KR\launcher.json` with every key filled with its
English text (an existing file is merged, never overwritten — your values win,
missing keys are filled in, keys the program no longer knows are kept at the
end). For `zh-rTW` the same command fills in the compiled Chinese instead.

Labels must be drawable by the shipped fonts (see below). The launcher builds
its glyph atlas once from every installed language's labels; a character no
shipped font has is drawn as `?`.

## Fonts

The default face is Noto Sans TC. Every other `.ttf` in `Fonts\` is used as a
**glyph fallback** by both the launcher and the POB window, so a language whose
script Noto Sans TC lacks needs a font dropped into `Fonts\`. Shipped: Noto Sans
TC (Chinese, Latin, kana), Noto Sans KR (Hangul), FZ_ZY (optional Chinese
alternative). Requirements for a new font:

- TrueType outlines (`.ttf` with a `glyf` table). OpenType/CFF fonts render in
  the POB window but the launcher cannot read them.
- A licence that allows redistribution if you want it shipped (Google's Noto
  fonts are SIL OFL 1.1 and are the recommended choice; a variable font must be
  instanced to a static Regular first).
- Declared in `NOTICE.md` before it can go into a release — the packaging script
  refuses undeclared fonts.

For your own machine none of that matters: put any `.ttf` in `Fonts\` and pick
it from the font list at the bottom of the launcher.

## Testing locally

1. Put your folders under `Data\`, restart the launcher, choose the language.
2. On the **Settings** page set *Automatically update translation data* to
   **No**, otherwise the next data release overwrites your files. (You can point
   each slot at a folder **outside** the install instead — see the
   *Translation data* section on that page; updates never touch external folders.)
3. Launch POB and look. The in-app **Translation editor** edits the currently
   selected language's dictionaries live.
4. Headless checks, all of them GUI-less and reporting through the exit code
   (0 = pass), run from the install folder:

   ```
   pob-zh.exe --tr "Chaos Orb" ko-KR          # what the engine returns for one key
   pob-zh.exe --font-coverage-selftest        # every dictionary character drawable by some shipped font
   pob-zh.exe --launcher-strings-selftest     # launcher.json shape, fallback to English
   pob-zh.exe --launcher-config-selftest      # language discovery on disk
   ```

   `--tr` writes its report to standard output; when run from a shell that
   cannot show it, redirect to a file. `pob-zh.exe` is a GUI-subsystem program:
   check `$LASTEXITCODE` / `%ERRORLEVEL%`, not the console text.

## Sending it in

Open a pull request against `pob-zh-engine/dist/Data/<slot>/<locale>/` in
[PobTools-zh](https://github.com/Hsiung-Shao/PobTools-zh). In the description say:

- which locale and which files, and whether it is a new language or an update;
- **where each part came from** (game files / trade API / your own translation)
  — the maintainer's review compares every changed key against the official game
  text, and knowing the source up front makes that fast;
- for launcher labels: that you are a native speaker (or not).

Translation data ships on its own release line (`data-<n>`), separate from the
program, so a merged translation reaches users without a program update. A new
language that needs a new font waits for the next program release.

## Known limits

- Path of Building's own interface (tabs, calculation labels, config options)
  has no official translation in any language; that half is community work.
- The in-app editor and `--tr` default to `zh-rTW`; pass the locale explicitly
  as shown above.
- `item_metadata.json` and `synonyms.json` (item-paste parsing and search
  synonyms) exist only for `zh-rTW`; other languages parse pasted items in
  English only.
