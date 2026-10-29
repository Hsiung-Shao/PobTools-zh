#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Offline harness for poecharm_inject.lua (CJK search & input).

Why this exists
---------------
Every bug this file guards against had the same shape: the patch reported
success and did nothing.

  * The gem box widened its input filter through `class._constructor`, which
    only the PoE2 fork has -- PoE1 community stores the constructor as a method
    named after the class. The branch was nil, was skipped in silence, and the
    trace still printed "patched GemSelectControl" while Chinese could not be
    typed into the box at all.
  * The item database matched a translated NAME but never a translated MODIFIER
    line, so "some items cannot be found" with no pattern a user could see.

So the rules here are:

  1. Run the REAL `Modules/Common.lua` from each installed fork. The two forks'
     `newClass`/`new` differ in exactly the way that caused the bug; a
     hand-written stub would have agreed with whichever one it was modelled on.
  2. Every behavioural test also runs with the inject OFF and requires the bug
     to reproduce. A test that passes both ways proves nothing.
  3. Separately assert the SHAPE of the installed POB sources (method names,
     the gem box's ASCII filter, the C++ source-patch anchors). That is what
     turns a future upstream rename into a failing test instead of a feature
     silently reverting to English.

Run:  python pob-zh-engine/tests/inject_cjk_test.py
Needs: pip install lupa      (Lua 5.1, no PoB or engine build required)
"""

import os
import re
import sys

try:
    import lupa.lua51 as lua51
except ImportError:  # pragma: no cover
    sys.exit("需要 lupa (Lua 5.1):  python -m pip install lupa")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                      # pob-zh-engine/
# POB_INJECT_TEST_DIST: a worktree has no deployed POB, so point at one that does
# (e.g. D:\codeproject\PobTest). Default stays this checkout's dist/.
DIST = os.environ.get("POB_INJECT_TEST_DIST") or os.path.join(ROOT, "dist")
INJECT = os.path.join(ROOT, "poecharm_inject.lua")
UI_API = os.path.join(ROOT, "ui_api.cpp")

FORKS = {
    "PoE1": os.path.join(DIST, "PathOfBuildingCommunity"),
    "PoE2": os.path.join(DIST, "PathOfBuildingCommunity-PoE2-Portable"),
}

# Fixture dictionary. Values are the real ones the shipped dictionaries return
# (verified with `pob-zh.exe --tr`), because a made-up value would still pass a
# test that only checks "something came back".
#
# "Volatility" is the whole point of the source dimension: the merged map
# answers with the PASSIVE (易爆) while the gem dropdown shows what gems.json
# says (易變輔助). A gem search that matches the unmarked lookup finds nothing
# when the user types what is on screen.
DICT = {
    None: {
        "Volatility": "易爆",
        "Fireball": "火球",
        "Kaom's Heart": "岡姆的壯志",
        "+500 to maximum Mana": "+500 最大魔力",
        "(30-40)% increased Lightning Damage": "增加 (30-40)%閃電傷害",
        "Summoned Skeleton": "召喚骷髏",
        "Spectral Throw": "幻影投擲",
        "Effective Hit Pool": "有效生命池",
        "Enemy Chilled": "敵人冰緩",
        # real shape of a coloured dictionary value -- the search has to see the
        # rendered text, not the "^xRRGGBB" runs that paint it
        "Is the enemy Shocked?": "^xFFFFFF敵人是否被^xADAA47【感電】^7?",
        "10% increased Attack Speed": "增加 10% 攻擊速度",
        "+30 to Dexterity": "+30 敏捷",
    },
    "gems": {
        "Volatility": "易變輔助",
        "Fireball": "火球",
    },
    # Source-only entries for the tooltip tests: none of these exist under the
    # unmarked source, so a lookup under the wrong source comes back untranslated.
    "passives": {"Elemental Overload": "元素超載",
                 # ST-P0:軍團替換後的新名稱,只在 passives 來源
                 "Lord of the Dead": "亡者之主"},
    "stats": {"+500 to maximum Mana": "+500 最大魔力",
              # L2a:未標記與 stats 都有、譯文刻意不同,才分得出走了哪一邊
              "+30 to Dexterity": "+30 點敏捷(stats)",
              # L2b:只有 stats 有,未標記查不到
              "Adds 1 to 2 Fire Damage to Attacks": "攻擊附加 1 - 2 火焰傷害"},
    "uniques": {"Kaom's Heart": "岡姆的壯志"},
    "items": {"Glorious Plate": "榮耀鎧甲"},
    "affixes": {"Heavy": "厚重的", "of the Whale": "鯨之"},
}

# Id-keyed dictionaries (passives_by_id.json / affixes_by_id.json) behind
# PobToolsTranslateId. Same English, different id, different wording -- so a
# lookup that ignores the id cannot produce both.
ID_DICT = {
    "passives": {"90001": "戰神", "90002": "勇士"},
    "affixes": {"TestModHeavyA": "厚重的甲", "TestModHeavyB": "厚重的乙",
                "TestModWhaleA": "鯨之甲"},
}

# Stubs every harness gets. Only what the loaded POB code touches at load time
# or inside the code paths under test -- deliberately not a POB emulator.
PRELUDE = rb"""
bit = setmetatable({}, {__index = function() return function() return 0 end end})
require = function() return setmetatable({}, {__index = function() return function() end end}) end
launch = { devMode = false }
main = { unicode = true, screenW = 1920, screenH = 1080, popups = {} }
colorCodes = setmetatable({}, {__index = function() return "^7" end})
function ConPrintf() end
function GetTime() return 0 end
function DrawString() end
function DrawStringWidth() return 10 end
function DrawStringCursorIndex() return 1 end
function SetDrawColor() end
function SetViewport() end
function DrawImage() end
function IsKeyDown() return false end
function Paste() return nil end
function Copy() end
function StripEscapes(s) return (s:gsub("%^%d", ""):gsub("%^x%x%x%x%x%x%x", "")) end
function PCall(f, ...)
	local r = { pcall(f, ...) }
	if r[1] then return nil, r[2] end
	return r[2]
end
function wipeTable(t) for k in pairs(t) do t[k] = nil end end
function copyTable(t) local n = {} for k, v in pairs(t) do n[k] = v end return n end
function isValueInTable(t, v) for _, x in pairs(t) do if x == v then return true end end end
function NewImageHandle()
	local h = {}
	function h:Load() end
	function h:Unload() end
	function h:IsValid() return false end
	function h:IsLoading() return false end
	function h:SetLoadingPriority() end
	function h:ImageSize() return 0, 0 end
	return h
end
function DrawImageQuad() end
function SetDrawLayer() end
function GetScreenSize() return 1920, 1080 end
function GetCursorPos() return 0, 0 end
function GetAsyncCount() return 0 end
function RenderInit() end
function ConExecute() end
function SetWindowTitle() end
function LoadModule_stub() end
"""


class Fail(Exception):
    pass


class Harness(object):
    """One Lua 5.1 state running a real fork's Common.lua."""

    def __init__(self, fork, with_inject=True, lua_mod=None, game=None, define_game=True):
        self.fork = fork
        # PobToolsGame() 的回傳值(引擎的 translation_game()):預設跟 fork 一致;
        # define_game=False 模擬舊引擎 / 沒給 API,全域 PobToolsGame 不存在。
        self.game = game or ("poe1" if fork == "PoE1" else "poe2")
        self.define_game = define_game
        self.dir = FORKS[fork]
        self.rt = (lua_mod or lua51).LuaRuntime(unpack_returned_tuples=True)
        self.source = None          # current PobToolsSetSource() name
        self.source_seen = set()    # every source active during a lookup
        self.lookups = []           # (text, source) of every lookup, in order
        self.id_lookups = []        # (source, id, en) of every PobToolsTranslateId
        self.title_lookups = []     # (text, source) of every PobToolsItemTitle
        self.enabled = True         # F2: off = every engine lookup answers nil
        # Tooltip lines arrive as "^xRRGGBBText"; the engine answers those by
        # colour segment. Off by default so the older tests keep exact-key lookup.
        self.loose_keys = False
        self._load(PRELUDE)
        self._install_engine_globals()
        g = self.rt.globals()
        g["LoadModule"] = self._load_module
        g["PLoadModule"] = self._load_module
        self._load(self._read(os.path.join(self.dir, "Modules", "Common.lua")))
        self.injected = False
        if with_inject:
            self.apply_inject()

    # ---- plumbing ----
    @staticmethod
    def _read(path):
        with open(path, "rb") as f:
            return f.read()

    def _chunk(self, src, name):
        return self.rt.eval("function(s, n) return assert(loadstring(s, n)) end")(src, name)

    def _load(self, src, name="=harness"):
        return self._chunk(src, name)()

    def _load_module(self, name, *args):
        name = name if name.endswith(".lua") else name + ".lua"
        path = os.path.join(self.dir, name.replace("/", os.sep))
        return self._chunk(self._read(path), "@" + path)(*args)

    def _install_engine_globals(self):
        g = self.rt.globals()

        def translate(text):
            if text is None or not self.enabled:
                return None
            table = DICT.get(self.source) or {}
            self.source_seen.add(self.source)
            self.lookups.append((text, self.source))
            out = table.get(text) or DICT[None].get(text)
            if out is None and self.loose_keys:
                m = re.match(r"^((?:\^x[0-9A-Fa-f]{6}|\^\d)*)(\s*)(.*?)(\s*)$",
                             text, re.S)
                core = m.group(3)
                if core and core != text:
                    hit = table.get(core) or DICT[None].get(core)
                    if hit:
                        out = m.group(1) + m.group(2) + hit + m.group(4)
            return out

        def translate_display(text):
            out = translate(text)
            return None if (out is None or out == text) else out

        def set_source(name=None):
            prev = self.source
            self.source = name
            return prev

        def translate_id(source, id_, en=None):
            # Lua integers may arrive as float; keys are id strings.
            if isinstance(id_, float) and id_.is_integer():
                id_ = int(id_)
            key = None if id_ is None else str(id_)
            self.id_lookups.append((source, key, en))
            if not self.enabled:
                return None         # engine: by-id and fallback both obey F2
            hit = (ID_DICT.get(source) or {}).get(key)
            if hit is not None:
                return hit
            # miss: ordinary lookup of `en` under that source
            prev = self.source
            self.source = source
            try:
                return translate(en)
            finally:
                self.source = prev

        g["__py_translate"] = translate
        g["__py_translate_id"] = translate_id
        g["__py_translate_display"] = translate_display
        g["__py_set_source"] = set_source
        def item_title(text):
            self.title_lookups.append((text, self.source))
            return translate(text)

        g["__py_item_title"] = item_title
        g["__py_enabled"] = lambda: self.enabled
        g["__py_game"] = lambda: self.game
        # Every failure the inject reports, in order. This is the engine-side
        # failure log seen from Lua; the test below is the only thing that proves
        # a broken patch actually reaches it.
        self.logged_errors = []
        g["__py_log_error"] = lambda feature, msg: self.logged_errors.append(
            (feature, msg))
        # Wrapped in real Lua functions on purpose: a Python callable handed
        # straight to Lua is userdata, and the inject's first line is a
        # type(PobToolsTranslate) ~= "function" bail-out. A harness that trips
        # that guard silently tests nothing at all.
        self._load(rb"""
function PobToolsTranslate(s) return __py_translate(s) end
function PobToolsTranslateDisplay(s) return __py_translate_display(s) end
function PobToolsSetSource(n) return __py_set_source(n) end
function PobToolsTranslateId(s, id, en) return __py_translate_id(s, id, en) end
function PobToolsGetTranslate() return __py_enabled() end
function PobToolsItemTitle(s) return __py_item_title(s) end
function PobToolsLogError(f, m) return __py_log_error(f, m) end
""")
        if self.define_game:
            self._load(b"function PobToolsGame() return __py_game() end")

    def apply_inject(self):
        self._load(self._read(INJECT), "@" + INJECT)
        self.injected = True

    # ---- helpers ----
    def run(self, src):
        return self._load(src if isinstance(src, bytes) else src.encode("utf-8"))

    def eval(self, expr):
        return self.rt.eval(expr)

    def klass(self, name):
        return self.rt.eval('common.classes["%s"]' % name)

    def define(self, name, poe1_src, poe2_src):
        """Register a synthetic class the way this fork's real files do."""
        self.run(poe1_src if self.fork == "PoE1" else poe2_src)


# ---------------------------------------------------------------------------
# Synthetic classes.
#
# Bodies are the upstream logic trimmed to what the patch interacts with, but
# they are REGISTERED through the fork's own newClass idiom -- that registration
# is the part the constructor patch depends on and the part that differs.
# ---------------------------------------------------------------------------

GEMSELECT_P1 = """
local C = newClass("GemSelectControl", "EditControl")
function C:GemSelectControl(anchor, rect)
	self:EditControl(anchor, rect, nil, nil, "^ %a':-")
	return self
end
function C:Draw() end
"""

GEMSELECT_P2 = """
local C = newClass("GemSelectControl", "EditControl", function(self, anchor, rect)
	self.EditControl(anchor, rect, nil, nil, "^ %a':-")
end)
function C:Draw() end
"""

# BuildList + the bits it leans on. English matching only, like upstream.
GEMLIST_BODY = """
local C = common.classes["GemSelectControl"]
C.gems = nil
function C:PopulateGemList()
	self.gems = {
		VolatilitySupport = { name = "Volatility" },
		Fireball = { name = "Fireball" },
	}
end
function C:FilterSupport() return true end
function C:SortGemList(list) table.sort(list) end
function C:BuildList(buf)
	self.list = {}
	self.searchStr = buf
	if #buf > 0 then
		for id, gem in pairs(self.gems) do
			if gem.name:lower():find(buf:lower(), 1, true) then table.insert(self.list, id) end
		end
		self:SortGemList(self.list)
	else
		for id in pairs(self.gems) do table.insert(self.list, id) end
	end
	if not self.list[1] then
		self.list[1] = ""
		self.noMatches = true
	else
		self.noMatches = false
	end
end
"""

ITEMDB_P1 = """
local C = newClass("ItemDBControl")
function C:ItemDBControl()
	self.controls = { search = { buf = "" }, searchMode = { selIndex = 1 } }
	return self
end
""" + """
function C:DoesItemMatchFilters(item)
	if not item.eligible then return false end
	local searchStr = self.controls.search.buf:lower()
	if searchStr:match("%S") then
		local found = false
		local mode = self.controls.searchMode.selIndex
		if mode == 1 or mode == 2 then
			if item.name:lower():find(searchStr, 1, true) then found = true end
		end
		if mode == 1 or mode == 3 then
			for _, line in ipairs(item.explicitModLines) do
				if line.line:lower():find(searchStr, 1, true) then found = true break end
			end
		end
		if not found then return false end
	end
	return true
end
"""

ITEMDB_P2 = ITEMDB_P1.replace(
    'local C = newClass("ItemDBControl")\nfunction C:ItemDBControl()\n'
    '\tself.controls = { search = { buf = "" }, searchMode = { selIndex = 1 } }\n'
    '\treturn self\nend\n',
    'local C = newClass("ItemDBControl", function(self)\n'
    '\tself.controls = { search = { buf = "" }, searchMode = { selIndex = 1 } }\n'
    'end)\n')

MINION_P1 = """
local C = newClass("MinionSearchListControl")
function C:MinionSearchListControl()
	self.controls = { searchText = { buf = "" } }
	self.data = {
		minions = { SkelWarrior = { name = "Summoned Skeleton", skillList = { "SpectralThrow" } } },
		skills = { SpectralThrow = { name = "Spectral Throw" } },
	}
	return self
end
function C:DoesEntryMatchFilters(searchStr, minionId, filterMode)
	local minion = self.data.minions[minionId]
	if (filterMode == 1 or filterMode == 3) and minion.name:lower():find(searchStr, 1, true) then
		return true
	end
	if filterMode == 2 or filterMode == 3 then
		for _, id in ipairs(minion.skillList) do
			if self.data.skills[id].name:lower():find(searchStr, 1, true) then return true end
		end
	end
	return false
end
"""

MINION_P2 = MINION_P1.replace(
    'local C = newClass("MinionSearchListControl")\nfunction C:MinionSearchListControl()\n',
    'local C = newClass("MinionSearchListControl", function(self)\n').replace(
    '\treturn self\nend\nfunction C:DoesEntryMatchFilters',
    'end)\nfunction C:DoesEntryMatchFilters')

CALCS_P1 = """
local C = newClass("CalcsTab")
function C:CalcsTab()
	self.controls = { search = { buf = "" } }
	return self
end
function C:SearchMatch(txt)
	local searchStr = self.controls.search.buf:lower()
	return string.len(searchStr) > 0 and txt:lower():find(searchStr, 1, true)
end
"""

CALCS_P2 = CALCS_P1.replace(
    'local C = newClass("CalcsTab")\nfunction C:CalcsTab()\n',
    'local C = newClass("CalcsTab", function(self)\n').replace(
    '\treturn self\nend\nfunction C:SearchMatch',
    'end)\nfunction C:SearchMatch')


def make(h, class_name):
    """new(class):Ctor() on PoE1, new(class) on PoE2 -- the forks differ here."""
    if h.fork == "PoE1":
        return h.eval('new("%s"):%s()' % (class_name, class_name))
    return h.eval('new("%s")' % class_name)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

RESULTS = []


def check(name, cond, detail=""):
    RESULTS.append((name, bool(cond), detail))
    if not cond:
        print("  FAIL  %s %s" % (name, detail))
    return bool(cond)


def gem_box(fork, with_inject):
    """Real EditControl chain + the gem box's one distinguishing ctor line."""
    h = Harness(fork, with_inject=False)
    h.run('LoadModule("Classes/EditControl")')            # loads the real file + parents
    h.define("GemSelectControl", GEMSELECT_P1, GEMSELECT_P2)
    if with_inject:
        h.apply_inject()
    obj = make(h, "GemSelectControl")
    h.eval('function(o) o:Insert("火球") end')(obj)
    return h, obj


def test_gem_input(fork):
    # inject OFF: the bug must reproduce, otherwise the ON case proves nothing.
    _, off = gem_box(fork, with_inject=False)
    check("%s gem box: bug reproduces without inject" % fork,
          off.buf == "", "buf=%r" % off.buf)

    h, on = gem_box(fork, with_inject=True)
    check("%s gem box: CJK survives typing" % fork,
          on.buf == "火球", "buf=%r filter=%r" % (on.buf, on.filter))
    check("%s gem box: filter widened" % fork,
          on.filter == "%c", "filter=%r" % on.filter)
    # the ASCII path upstream cares about must still work
    h.eval('function(o) o.buf = "" o.caret = 1 o:Insert("Fireball") end')(on)
    check("%s gem box: ASCII still accepted" % fork, on.buf == "Fireball",
          "buf=%r" % on.buf)


def test_gem_search(fork):
    for with_inject in (False, True):
        h = Harness(fork, with_inject=False)
        h.run('LoadModule("Classes/EditControl")')
        h.define("GemSelectControl", GEMSELECT_P1, GEMSELECT_P2)
        h.run(GEMLIST_BODY)
        if with_inject:
            h.apply_inject()
        obj = make(h, "GemSelectControl")
        h.eval('function(o) o:PopulateGemList() o:BuildList("易變") end')(obj)
        found = list(h.eval('function(o) return o.list end')(obj).values())
        if with_inject:
            check("%s gem search: 易變 finds the support gem" % fork,
                  "VolatilitySupport" in found, "list=%r" % found)
            # 易變輔助 only exists under source "gems"; matching it proves the
            # lookup ran source-marked and not against the passive 易爆.
            check("%s gem search: ran under source 'gems'" % fork,
                  "gems" in h.source_seen, "sources=%r" % (h.source_seen,))
        else:
            check("%s gem search: bug reproduces without inject" % fork,
                  "VolatilitySupport" not in found, "list=%r" % found)


ITEM = """
{
	eligible = true,
	name = "Kaom's Heart",
	explicitModLines = {
		{ line = "(30-40)% increased Lightning Damage" },
		{ line = "+500 to maximum Mana" },
	},
	implicitModLines = {},
	enchantModLines = {},
}
"""

INELIGIBLE = ITEM.replace("eligible = true", "eligible = false")


def test_itemdb(fork):
    def probe(with_inject, query, mode, item_src=ITEM):
        h = Harness(fork, with_inject=False)
        h.define("ItemDBControl", ITEMDB_P1, ITEMDB_P2)
        if with_inject:
            h.apply_inject()
        obj = make(h, "ItemDBControl")
        fn = h.eval('function(o, q, mode, item) o.controls.search.buf = q '
                    'o.controls.searchMode.selIndex = mode '
                    'return o:DoesItemMatchFilters(item) end')
        return fn(obj, query, mode, h.eval("function() return %s end" % item_src)())

    check("%s item DB: CJK name found (mode Anywhere)" % fork, probe(True, "岡姆", 1))
    check("%s item DB: CJK name found (mode Names)" % fork, probe(True, "岡姆", 2))
    check("%s item DB: CJK mod found (mode Anywhere)" % fork, probe(True, "閃電傷害", 1))
    check("%s item DB: CJK mod found (mode Modifiers)" % fork, probe(True, "閃電傷害", 3))
    check("%s item DB: CJK mod NOT found in Names mode" % fork,
          not probe(True, "閃電傷害", 2))
    check("%s item DB: ineligible item still filtered out" % fork,
          not probe(True, "岡姆", 1, INELIGIBLE))
    check("%s item DB: English search unchanged" % fork, probe(True, "kaom", 1))
    check("%s item DB: non-matching CJK rejected" % fork, not probe(True, "火球", 1))
    # without the inject the mod half is exactly what was missing
    check("%s item DB: CJK mod bug reproduces without inject" % fork,
          not probe(False, "閃電傷害", 1))


def test_minion(fork):
    def probe(with_inject, query, mode):
        h = Harness(fork, with_inject=False)
        h.define("MinionSearchListControl", MINION_P1, MINION_P2)
        if with_inject:
            h.apply_inject()
        obj = make(h, "MinionSearchListControl")
        fn = h.eval('function(o, q, mode) o.controls.searchText.buf = q '
                    'return o:DoesEntryMatchFilters(q:lower(), "SkelWarrior", mode) end')
        return fn(obj, query, mode)

    check("%s minion: CJK name found" % fork, probe(True, "骷髏", 1))
    check("%s minion: CJK skill found" % fork, probe(True, "幻影", 2))
    check("%s minion: CJK skill not matched in Names mode" % fork,
          not probe(True, "幻影", 1))
    check("%s minion: English search unchanged" % fork, probe(True, "skeleton", 1))
    check("%s minion: bug reproduces without inject" % fork, not probe(False, "骷髏", 1))


def test_calcs(fork):
    def probe(with_inject, query, label):
        h = Harness(fork, with_inject=False)
        h.define("CalcsTab", CALCS_P1, CALCS_P2)
        if with_inject:
            h.apply_inject()
        obj = make(h, "CalcsTab")
        fn = h.eval('function(o, q, label) o.controls.search.buf = q '
                    'return o:SearchMatch(label) and true or false end')
        return fn(obj, query, label)

    check("%s calcs: CJK label found" % fork, probe(True, "生命池", "Effective Hit Pool"))
    check("%s calcs: English still found" % fork, probe(True, "hit", "Effective Hit Pool"))
    check("%s calcs: unrelated CJK rejected" % fork,
          not probe(True, "火球", "Effective Hit Pool"))
    # spans two coloured runs in the dictionary value; only works if the
    # translation is stripped of its escapes before matching
    check("%s calcs: query spanning a colour escape" % fork,
          probe(True, "敵人是否被【感電】", "Is the enemy Shocked?"))
    check("%s calcs: bug reproduces without inject" % fork,
          not probe(False, "生命池", "Effective Hit Pool"))


def test_patch_failure_is_logged(fork):
    """A patch that cannot apply has to end up in the failure log.

    This is the whole reason the log exists. When POB changes upstream, the
    symptom a user sees is "Chinese input stopped working" -- there is no crash,
    no message, and the feature simply goes quiet. Before this, the only record
    was a trace file that exists solely when POB_ZH_INJECT_TRACE is set, i.e.
    never on a user's machine.

    The failure is manufactured the way upstream would cause it: the class is
    present but the method the patch wraps is gone.
    """
    h = Harness(fork, with_inject=False)
    # CalcsTab without SearchMatch -- exactly what an upstream rename looks like
    # from in here.
    # A bare table, not newClass(): the two forks disagree about that signature,
    # and what matters here is only that the class EXISTS while the method the
    # patch wraps does not -- which is exactly what an upstream rename leaves
    # behind.
    h.run(b'common.classes.CalcsTab = {}')
    h.apply_inject()

    hits = [(f, m) for (f, m) in h.logged_errors if f == "inject"]
    check("%s inject: a failed patch is written to the failure log" % fork,
          len(hits) >= 1, repr(h.logged_errors))
    if hits:
        msg = hits[0][1]
        # The name alone ("patch FAILED CalcsTab") is not actionable by whoever
        # reads the report, so the symptom has to travel with it.
        check("%s inject: the log line names the symptom, not just the class" % fork,
              "計算頁" in msg, msg)
        check("%s inject: the log line names the class too" % fork,
              "CalcsTab" in msg, msg)


def test_no_patch_failed(fork):
    """Every PATCHES entry that had a class present must have applied cleanly."""
    h = Harness(fork, with_inject=False)
    logged = []
    h.rt.globals()["ConPrintf"] = lambda fmt, msg=None: logged.append(msg)
    for name in ("EditControl", "Tooltip", "SearchHost"):
        h.run('LoadModule("Classes/%s")' % name)
    h.define("GemSelectControl", GEMSELECT_P1, GEMSELECT_P2)
    h.define("ItemDBControl", ITEMDB_P1, ITEMDB_P2)
    h.define("MinionSearchListControl", MINION_P1, MINION_P2)
    h.define("CalcsTab", CALCS_P1, CALCS_P2)
    h.apply_inject()
    failed = [m for m in logged if m and "patch FAILED" in m]
    check("%s inject: no patch reported FAILED" % fork, not failed, repr(failed))
    patched = sorted(m.split(" ", 1)[1] for m in logged if m and m.startswith("patched "))
    for want in ("GemSelectControl", "ItemDBControl", "MinionSearchListControl",
                 "CalcsTab", "EditControl", "Tooltip", "SearchHost"):
        check("%s inject: patched %s" % (fork, want), want in patched, repr(patched))


def test_paste_regression(fork):
    """The v0.24.0 paste fix must keep working -- it shares EditControl."""
    for with_inject in (False, True):
        h = Harness(fork, with_inject=False)
        h.run('LoadModule("Classes/EditControl")')
        h.rt.globals()["Paste"] = lambda: "我愛POE"
        h.rt.globals()["IsKeyDown"] = lambda key: key == "CTRL"
        if with_inject:
            h.apply_inject()
        if h.fork == "PoE1":
            obj = h.eval('new("EditControl")')
            h.eval('function(o) o:EditControl(nil, {0,0,100,20}) end')(obj)
        else:
            obj = h.eval('new("EditControl", nil, {0, 0, 100, 20})')
        h.eval('function(o) o:OnKeyDown("v") end')(obj)
        if with_inject:
            check("%s paste: CJK survives Ctrl+V" % fork, obj.buf == "我愛POE",
                  "buf=%r" % obj.buf)
        else:
            check("%s paste: bug reproduces without inject" % fork,
                  obj.buf != "我愛POE", "buf=%r" % obj.buf)


# ---- source dimension for tooltips (L1-L6) ---------------------------------
#
# The engine now answers per source (passives / affixes / stats / uniques /
# items). A tooltip line looked up under the wrong source still "translates" --
# just to the wrong dictionary's answer -- so every check below asserts the
# source that was ACTIVE at lookup time, recorded by the harness, not merely
# that some Chinese came back.

COLOUR_SETUP = b"""
colorCodes.UNIQUE = "^xAF6025"
colorCodes.RARE = "^xFFFF77"
colorCodes.MAGIC = "^x8888FF"
colorCodes.NORMAL = "^xC8C8C8"
"""

# Upstream AddNodeName/AddNodeTooltip trimmed to the lines that reach the
# tooltip. `node.explode` manufactures the failure L6 needs.
PASSIVE_METHODS = """
function C:AddNodeName(tooltip, node, build)
	local nodeName = node.dn
	tooltip.center = true
	tooltip:AddLine(24, nodeName..(launch.devModeAlt and " ["..node.id.."]" or ""), "FONTIN")
	tooltip.center = false
	if node.explode then error("synthetic AddNodeName failure") end
end
function C:AddNodeTooltip(tooltip, node, build)
	self:AddNodeName(tooltip, node, build)
	for _, line in ipairs(node.sd or {}) do
		if node.unsupported then
			-- 上游 addModInfoToTooltip:不支援的詞綴在 AddLine 時才接上註記
			local l = colorCodes.UNSUPPORTED..line
			l = main.notSupportedModTooltips and (l .. main.notSupportedTooltipText) or l
			tooltip:AddLine(16, l, "FONTIN")
		else
			tooltip:AddLine(16, "^7"..line)
		end
	end
	-- ST-L4:珠寶插槽,上游在 AddNodeTooltip 內呼叫 itemsTab:AddItemTooltip
	if node.jewel and build and build.itemsTab then
		build.itemsTab:AddItemTooltip(tooltip, node.jewel, { nodeId = node.id })
	end
end
"""

PASSIVETREE_P1 = """
local C = newClass("PassiveTreeView")
function C:PassiveTreeView() return self end
""" + PASSIVE_METHODS

PASSIVETREE_P2 = """
local C = newClass("PassiveTreeView", function(self) end)
""" + PASSIVE_METHODS

# Upstream AddItemTooltip's name block verbatim, then `item.otherLines` (L3c:
# requirement / quality style lines, neither name nor base), plus the PoE1 mod-line call
# (4th arg = modLine table). PoE2's 4th AddLine arg is a background, so the
# mod line goes in with nil there.
ITEMTIP_METHODS = """
function C:AddItemTooltip(tooltip, item, slot, dbMode, maxWidth)
	local rarityCode = colorCodes[item.rarity]
	tooltip.center = true
	tooltip.color = rarityCode
	if item.title then
		tooltip:AddLine(20, rarityCode..item.title, "FONTIN SC")
		tooltip:AddLine(20, rarityCode..item.baseName:gsub(" %(.+%)",""), "FONTIN SC")
	else
		tooltip:AddLine(20, rarityCode..item.namePrefix..item.baseName:gsub(" %(.+%)","")..item.nameSuffix, "FONTIN SC")
	end
	tooltip.center = false
	for _, l in ipairs(item.otherLines or {}) do
		tooltip:AddLine(16, l)
	end
	for _, modLine in ipairs(item.explicitModLines or {}) do
		tooltip:AddLine(16, colorCodes.MAGIC..modLine.line, "FONTIN SC", MODLINE_ARG)
	end
	-- 上游「Show flavour text」段(兩分支相同):每行 colorCodes.UNIQUE .. line、FONTIN SC ITALIC
	for _, line in ipairs(item.flavourLines or {}) do
		tooltip:AddLine(20, colorCodes.UNIQUE .. line, "FONTIN SC ITALIC")
	end
	if item.explode then error("synthetic AddItemTooltip failure") end
end
"""

ITEMSTAB_P1 = """
local C = newClass("ItemsTab")
function C:ItemsTab() return self end
""" + ITEMTIP_METHODS.replace("MODLINE_ARG", "modLine")

ITEMSTAB_P2 = """
local C = newClass("ItemsTab", function(self) end)
""" + ITEMTIP_METHODS.replace("MODLINE_ARG", "nil")

# ItemDBControl:GetRowValue verbatim: item.name is "Title, Base" for uniques.
ROWVALUE = """
function C:GetRowValue(column, index, item)
	if item and column == 1 then
		return colorCodes[item.rarity] .. item.name
	end
end
"""

UNIQUE_ITEM = """
{
	rarity = "UNIQUE",
	title = "Kaom's Heart",
	baseName = "Glorious Plate",
	base = {},
	name = "Kaom's Heart, Glorious Plate",
	explicitModLines = { { line = "+500 to maximum Mana" } },
}
"""

MAGIC_ITEM = """
{
	rarity = "MAGIC",
	namePrefix = "Heavy ",
	baseName = "Glorious Plate",
	nameSuffix = " of the Whale",
	base = {},
	name = "Heavy Glorious Plate of the Whale",
	explicitModLines = {},
}
"""

_ESC = re.compile(r"\^x[0-9A-Fa-f]{6}|\^\d")
NOT_LOOKED_UP = "<未查詢>"


def _norm(s):
    return _ESC.sub("", s or "").strip()


def src_of(h, text):
    """Source active at the FIRST lookup of `text` (colour/whitespace ignored)."""
    for t, s in h.lookups:
        if _norm(t) == text:
            return s
    return NOT_LOOKED_UP


def tooltip_harness(fork, with_inject, itemstab=None):
    h = Harness(fork, with_inject=False)
    h.loose_keys = True
    h.run(COLOUR_SETUP)
    h.run('LoadModule("Classes/Tooltip")')
    h.define("PassiveTreeView", PASSIVETREE_P1, PASSIVETREE_P2)
    if itemstab is None:
        h.define("ItemsTab", ITEMSTAB_P1, ITEMSTAB_P2)
    else:
        h.run(itemstab)
    h.define("ItemDBControl", ITEMDB_P1 + ROWVALUE, ITEMDB_P2 + ROWVALUE)
    if with_inject:
        h.apply_inject()
    h.lookups = []     # only what the call under test looks up
    return h


def tooltip_lines(h, tip):
    got = h.eval('function(t) local o = {} for i, l in ipairs(t.lines) do '
                 'o[i] = l.text end return o end')(tip)
    return [got[i] for i in range(1, len(got) + 1)]


def run_node_name(fork, with_inject, explode=False):
    h = tooltip_harness(fork, with_inject)
    view = make(h, "PassiveTreeView")
    tip = make(h, "Tooltip")
    fn = h.eval('function(v, t, boom) local ok, e = pcall(function() '
                'v:AddNodeName(t, { dn = "Elemental Overload", id = 1, explode = boom }, {}) '
                'end) return ok, e end')
    ok, err = fn(view, tip, explode)
    return h, ok, err


def run_item_tooltip(fork, with_inject, item_src, explode=False, itemstab=None):
    h = tooltip_harness(fork, with_inject, itemstab)
    tab = make(h, "ItemsTab")
    tip = make(h, "Tooltip")
    item = h.eval("function() return %s end" % item_src)()
    item.explode = explode
    ok, err = h.eval('function(tab, t, item) local ok, e = pcall(function() '
                     'tab:AddItemTooltip(t, item) end) return ok, e end')(tab, tip, item)
    return h, tip, ok, err


def name_by_passives(h, dn, node_id):
    """名稱行在 passives 來源下查,或以 PobToolsTranslateId("passives", id, dn) 查。"""
    return (src_of(h, dn) == "passives"
            or ("passives", str(node_id), dn) in h.id_lookups)


def test_src_passive_name(fork):
    """L1 -- only the NAME line is passives (source or by id)."""
    h, ok, err = run_node_name(fork, with_inject=False)
    check("%s L1 未注入時重現 bug:天賦名稱行不在 passives 來源也沒依 id 查" % fork,
          not name_by_passives(h, "Elemental Overload", 1),
          "lookups=%r id_lookups=%r" % (h.lookups, h.id_lookups))
    h, ok, err = run_node_name(fork, with_inject=True)
    check("%s L1 AddNodeName 本身不出錯" % fork, ok, repr(err))
    check("%s L1 天賦名稱行在 passives 來源或依 id 查詢" % fork,
          name_by_passives(h, "Elemental Overload", 1),
          "lookups=%r id_lookups=%r" % (h.lookups, h.id_lookups))
    check("%s L1 AddNodeName 結束後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)


def test_src_passive_stat_line(fork):
    """L1b -- AddNodeTooltip's stat lines: the shipping unmarked lookup."""
    h = tooltip_harness(fork, with_inject=True)
    view = make(h, "PassiveTreeView")
    tip = make(h, "Tooltip")
    h.eval('function(v, t) v:AddNodeTooltip(t, { dn = "Elemental Overload", id = 1, '
           'sd = { "10% increased Attack Speed" } }, {}) end')(view, tip)
    check("%s L1b 天賦屬性行走未標記查找(來源 nil)" % fork,
          src_of(h, "10% increased Attack Speed") is None, repr(h.lookups))


def mod_line_harness(fork, text):
    """PoE1 AddLine with a modLine 4th arg, outside any item tooltip."""
    h = tooltip_harness(fork, True)
    tip = make(h, "Tooltip")
    h.eval('function(t, s) t:AddLine(16, "^x8888FF" .. s, "FONTIN SC", { line = s }) end')(
        tip, text)
    lines = tooltip_lines(h, tip)
    return h, (_norm(lines[0]) if lines else "")


def srcs_of(h, text):
    """Every source active at a lookup of `text`, in order."""
    return [s for t, s in h.lookups if _norm(t) == text]


def test_src_mod_line(fork):
    """L2a/L2b -- only PoE1's AddLine has a modLine 4th parameter."""
    if fork != "PoE1":
        print("  N/A   %s L2:此分支 Tooltip:AddLine 第 4 參數是 background,沒有 modLine" % fork)
        return
    # L2a: unmarked has it -> unmarked answer, never stats
    h, out = mod_line_harness(fork, "+30 to Dexterity")
    check("%s L2a 詞綴行先走未標記查找:來源 nil、結果是未標記的值" % fork,
          src_of(h, "+30 to Dexterity") is None and out == "+30 敏捷",
          "out=%r lookups=%r" % (out, h.lookups))
    check("%s L2a AddLine 結束後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)
    # L2b: unmarked misses -> retried under stats
    h, out = mod_line_harness(fork, "Adds 1 to 2 Fire Damage to Attacks")
    srcs = srcs_of(h, "Adds 1 to 2 Fire Damage to Attacks")
    check("%s L2b 詞綴行查不到才用 stats:先試過未標記查找" % fork,
          None in srcs and srcs.index(None) < len(srcs) - 1, repr(h.lookups))
    check("%s L2b 詞綴行查不到才用 stats:最後一次在 stats 下、結果是 stats 的值" % fork,
          srcs and srcs[-1] == "stats" and out == "攻擊附加 1 - 2 火焰傷害",
          "out=%r lookups=%r" % (out, h.lookups))
    check("%s L2b AddLine 結束後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)
    # inside AddItemTooltip the mod line is unmarked too (no outer items scope)
    h, tip, ok, err = run_item_tooltip(fork, True, UNIQUE_ITEM)
    check("%s L2a AddItemTooltip 內的詞綴行也先走未標記查找" % fork,
          src_of(h, "+500 to maximum Mana") is None, repr(h.lookups))


def test_src_unique_title(fork):
    """L3"""
    h, tip, ok, err = run_item_tooltip(fork, False, UNIQUE_ITEM)
    check("%s L3 未注入時重現 bug:傳奇名不在 uniques 來源" % fork,
          src_of(h, "Kaom's Heart") != "uniques", repr(h.lookups))
    h, tip, ok, err = run_item_tooltip(fork, True, UNIQUE_ITEM)
    check("%s L3 AddItemTooltip 本身不出錯" % fork, ok, repr(err))
    check("%s L3 傳奇物品標題行在 uniques 來源下查詢" % fork,
          src_of(h, "Kaom's Heart") == "uniques", repr(h.lookups))
    check("%s L3 傳奇物品基底行在 items 來源下查詢" % fork,
          src_of(h, "Glorious Plate") == "items", repr(h.lookups))
    lines = [_norm(t) for t in tooltip_lines(h, tip)]
    check("%s L3 tooltip 前兩行分別是傳奇名與基底的譯文" % fork,
          lines[:2] == ["岡姆的壯志", "榮耀鎧甲"], repr(lines))
    check("%s L3 AddItemTooltip 結束後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)


def patched_itemstab(fork):
    """The synthetic ItemsTab after the engine's real source patches for
    itemstab.lua (ui_api.cpp) -- the title line then reads
    item.title:gsub("^.+$", PobToolsItemTitle or "%0")."""
    src = ITEMSTAB_P1 if fork == "PoE1" else ITEMSTAB_P2
    out, applied = apply_source_patches(
        src, "itemstab.lua", parse_source_patches(read_text(UI_API)))
    return out, applied


def test_src_unique_title_patched(fork):
    """L3b -- the shipping path: the title is translated by PobToolsItemTitle
    inside the source-patched expression, before AddLine sees it."""
    src, applied = patched_itemstab(fork)
    check("%s L3b 引擎 source patch 改寫了合成 ItemsTab 的標題式" % fork,
          applied >= 1 and 'gsub("^.+$", PobToolsItemTitle' in src, "applied=%d" % applied)
    h, tip, ok, err = run_item_tooltip(fork, False, UNIQUE_ITEM, itemstab=src)
    srcs = [s for t, s in h.title_lookups if t == "Kaom's Heart"]
    check("%s L3b 未注入時重現 bug:PobToolsItemTitle 不在 uniques 來源" % fork,
          srcs and srcs[0] != "uniques", repr(h.title_lookups))
    h, tip, ok, err = run_item_tooltip(fork, True, UNIQUE_ITEM, itemstab=src)
    check("%s L3b AddItemTooltip 本身不出錯" % fork, ok, repr(err))
    srcs = [s for t, s in h.title_lookups if t == "Kaom's Heart"]
    check("%s L3b 傳奇標題經 PobToolsItemTitle 在 uniques 來源下查詢" % fork,
          srcs == ["uniques"], repr(h.title_lookups))
    lines = [_norm(t) for t in tooltip_lines(h, tip)]
    check("%s L3b tooltip 第一行是傳奇名譯文" % fork,
          lines[:1] == ["岡姆的壯志"], repr(lines))
    check("%s L3b 結束後來源還原為 nil" % fork, h.source is None, "source=%r" % h.source)


OTHER_LINES_ITEM = UNIQUE_ITEM.replace(
    "\tbase = {},",
    '\tbase = {},\n\totherLines = { "^x7F7F7FRequires Level 68", "^x7F7F7FQuality: +20%" },')


def test_src_item_other_lines(fork):
    """L3c -- AddItemTooltip lines other than name / base: unmarked lookup."""
    h, tip, ok, err = run_item_tooltip(fork, True, OTHER_LINES_ITEM)
    check("%s L3c AddItemTooltip 本身不出錯" % fork, ok, repr(err))
    srcs = [src_of(h, t) for t in ("Requires Level 68", "Quality: +20%")]
    check("%s L3c 物品需求行、品質行走未標記查找(來源 nil)" % fork,
          srcs == [None, None], "srcs=%r lookups=%r" % (srcs, h.lookups))


def test_src_affix_title(fork):
    """L4 -- no item.title: namePrefix .. baseName .. nameSuffix in one line."""
    h, tip, ok, err = run_item_tooltip(fork, False, MAGIC_ITEM)
    check("%s L4 未注入時重現 bug:前綴不在 affixes 來源" % fork,
          src_of(h, "Heavy") != "affixes", repr(h.lookups))
    h, tip, ok, err = run_item_tooltip(fork, True, MAGIC_ITEM)
    check("%s L4 AddItemTooltip 本身不出錯" % fork, ok, repr(err))
    srcs = [src_of(h, t) for t in ("Heavy", "Glorious Plate", "of the Whale")]
    check("%s L4 無 title 的物品名拆三段,來源依序為 affixes/items/affixes" % fork,
          srcs == ["affixes", "items", "affixes"],
          "srcs=%r lookups=%r" % (srcs, h.lookups))
    lines = tooltip_lines(h, tip)
    first = _norm(lines[0]) if lines else ""
    pos = [first.find(w) for w in ("厚重的", "榮耀鎧甲", "鯨之")]
    check("%s L4 組合後標題依序為前綴、基底、後綴的譯文" % fork,
          -1 not in pos and pos == sorted(pos), repr(first))
    check("%s L4 組合後標題不殘留英文" % fork,
          first and not re.search(r"[A-Za-z]", first), repr(first))


def test_src_list_row(fork):
    """L5 -- list rows read "Title, Base" from item.name."""
    for with_inject in (False, True):
        h = tooltip_harness(fork, with_inject)
        db = make(h, "ItemDBControl")
        item = h.eval("function() return %s end" % UNIQUE_ITEM)()
        h.lookups = []
        val = h.eval('function(o, item) return o:GetRowValue(1, 1, item) end')(db, item)
        srcs = [src_of(h, "Kaom's Heart"), src_of(h, "Glorious Plate")]
        if not with_inject:
            check("%s L5 未注入時重現 bug:清單列沒拆段查詢" % fork,
                  srcs != ["uniques", "items"], repr(h.lookups))
            continue
        check("%s L5 清單列 title, base 拆兩段,來源依序為 uniques/items" % fork,
              srcs == ["uniques", "items"], "srcs=%r lookups=%r" % (srcs, h.lookups))
        out = _norm(val)
        pos = [out.find(w) for w in ("岡姆的壯志", "榮耀鎧甲")]
        check("%s L5 清單列輸出依序是傳奇名與基底的譯文" % fork,
              -1 not in pos and pos == sorted(pos), repr(val))
        check("%s L5 清單列查詢後來源還原為 nil" % fork,
              h.source is None, "source=%r" % h.source)


def test_src_restored_on_error(fork):
    """L6 -- a throwing wrapped function must not leave a source behind."""
    h, ok, err = run_node_name(fork, with_inject=True, explode=True)
    check("%s L6 AddNodeName 拋錯時錯誤照樣往外拋" % fork,
          not ok and "synthetic AddNodeName failure" in str(err), repr((ok, err)))
    # without this the nil check below passes even when nothing was wrapped
    check("%s L6 AddNodeName 拋錯前名稱行確實在 passives 來源或依 id 查" % fork,
          name_by_passives(h, "Elemental Overload", 1),
          "lookups=%r id_lookups=%r" % (h.lookups, h.id_lookups))
    check("%s L6 AddNodeName 拋錯後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)

    h, tip, ok, err = run_item_tooltip(fork, True, UNIQUE_ITEM, explode=True)
    check("%s L6 AddItemTooltip 拋錯時錯誤照樣往外拋" % fork,
          not ok and "synthetic AddItemTooltip failure" in str(err), repr((ok, err)))
    check("%s L6 AddItemTooltip 拋錯前確實在 items 來源下" % fork,
          src_of(h, "Glorious Plate") == "items", repr(h.lookups))
    check("%s L6 AddItemTooltip 拋錯後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)
    # The base line is items because of itemCtx (the wrap point), not an outer
    # scope: after the throw a bare base-name line must not still read as one.
    h.lookups = []
    tip2 = make(h, "Tooltip")
    h.eval('function(t) t:AddLine(20, "Glorious Plate") end')(tip2)
    check("%s L6 AddItemTooltip 拋錯後物品 context 不殘留(基底名不再走 items)" % fork,
          src_of(h, "Glorious Plate") is None, repr(h.lookups))


# ---- id-keyed lookups (L7-L8) -----------------------------------------------
#
# Same English, different id (GraphId / mod Id), different wording: the
# translation is decided by the id. A miss falls back to the English lookup.

def node_name_line(h, node_id, dn):
    view = make(h, "PassiveTreeView")
    tip = make(h, "Tooltip")
    h.eval('function(v, t, id, dn) v:AddNodeName(t, { dn = dn, id = id }, {}) end')(
        view, tip, node_id, dn)
    lines = tooltip_lines(h, tip)
    return _norm(lines[0]) if lines else ""


def test_id_passive_name(fork):
    """L7"""
    h = tooltip_harness(fork, with_inject=True)
    got = [node_name_line(h, i, "Elemental Overload") for i in (90001, 90002, 99999)]
    check("%s L7 天賦名稱行依節點 id 取譯名:同名不同 id 各取己譯,id 未命中退回英文查找" % fork,
          got == ["戰神", "勇士", "元素超載"], "got=%r id_lookups=%r" % (got, h.id_lookups))
    called = [(s, i, en) for s, i, en in h.id_lookups if s == "passives"]
    check("%s L7 名稱行以 PobToolsTranslateId(\"passives\", node.id, node.dn) 查詢" % fork,
          ("passives", "90001", "Elemental Overload") in called, repr(h.id_lookups))
    check("%s L7 依 id 查完後來源還原為 nil" % fork,
          h.id_lookups and h.source is None, "source=%r" % h.source)


def test_id_obeys_f2(fork):
    """L9 -- F2 off: the by-id lookup answers nil like every other lookup."""
    h = tooltip_harness(fork, with_inject=True)
    h.enabled = False
    got = node_name_line(h, 90001, "Elemental Overload")
    check("%s L9 翻譯關閉時天賦名稱行維持英文(依 id 查詢也不翻)" % fork,
          got == "Elemental Overload", "got=%r id_lookups=%r" % (got, h.id_lookups))


# ST-P0:新引擎契約 -- 依 id 查時 (id, en) 都相符才回譯文,英文不符回 nil。
STRICT_ID_DICT = {("passives", "90001"): ("Elemental Overload", "戰神")}


def strict_id_harness(fork):
    """天賦提示 harness,PobToolsTranslateId 換成 (id, en) 皆符才命中的假引擎。"""
    h = tooltip_harness(fork, with_inject=True)

    def translate_id(source, id_, en=None):
        if isinstance(id_, float) and id_.is_integer():
            id_ = int(id_)
        key = None if id_ is None else str(id_)
        h.id_lookups.append((source, key, en))
        hit = STRICT_ID_DICT.get((source, key))
        return hit[1] if hit and hit[0] == en else None

    h.rt.globals()["__py_translate_id"] = translate_id
    return h


def test_id_replaced_node(fork):
    """ST-P0 -- 軍團/刺青替換節點保留原 id、只換 dn:名稱行不得沿用原節點譯名。"""
    h = tooltip_harness(fork, with_inject=True)   # 舊引擎:依 id 命中不比英文
    got = node_name_line(h, 90001, "Lord of the Dead")
    check("%s ST-P0 舊引擎(不比英文)重現 bug:替換節點顯示原節點譯名" % fork,
          got == "戰神", "got=%r id_lookups=%r" % (got, h.id_lookups))
    h = strict_id_harness(fork)
    got = node_name_line(h, 90001, "Lord of the Dead")
    check("%s ST-P0 替換節點(原 id、新 dn)名稱行不顯示原節點譯名" % fork,
          got != "戰神", "got=%r id_lookups=%r" % (got, h.id_lookups))
    check("%s ST-P0 替換節點名稱行改由 passives 來源查到新名稱譯文" % fork,
          got == "亡者之主" and src_of(h, "Lord of the Dead") == "passives",
          "got=%r lookups=%r" % (got, h.lookups))
    check("%s ST-P0 呼叫 PobToolsTranslateId 第 3 參數是替換後的 dn" % fork,
          ("passives", "90001", "Lord of the Dead") in h.id_lookups, repr(h.id_lookups))
    h = strict_id_harness(fork)
    got = node_name_line(h, 90001, "Elemental Overload")
    check("%s ST-P0 一般節點(id 與 dn 相符)仍用 id 譯文" % fork,
          got == "戰神", "got=%r id_lookups=%r" % (got, h.id_lookups))
    check("%s ST-P0 一般節點呼叫 PobToolsTranslateId 第 3 參數是 dn" % fork,
          ("passives", "90001", "Elemental Overload") in h.id_lookups, repr(h.id_lookups))
    check("%s ST-P0 結束後來源還原為 nil" % fork, h.source is None, "source=%r" % h.source)


# ---- ST-E3:被征服 / 刺青節點的天賦提示名 --------------------------------------
#
# POB 欄位(PassiveSpec.lua):
#   node.conqueredBy = item.jewelData.conqueredBy({ id = 種子, conqueror = { type, id } }),
#     BuildAllDependsAndPaths 對軍團珠寶範圍內「所有」節點設定(PoE1 L1187、PoE2 L1464),
#     名稱沒換的小節點也有;換名的節點 dn 改成 legion 節點的 dn、id 不變。
#   node.isTattoo:ReplaceNode(PoE1 L1994 起)從刺青節點複製;id 不變、dn 換成刺青名。
# 規則:id 英文不符時,conqueredBy -> legion、isTattoo -> tattoo,查不到再退 passives。

DICT.setdefault("tattoo", {})["Tattoo of the Kitava Makanga"] = "奇塔弗鑄造的紋身"
DICT.setdefault("legion", {})["Bleed Avoidance"] = "流血避免"
DICT[None].setdefault("Bleed Avoidance", "流血迴避(全域)")
DICT[None]["Tattoo of the Kitava Makanga"] = "奇塔瓦紋身(全域)"


def e3_name(h, fields):
    view = make(h, "PassiveTreeView")
    tip = make(h, "Tooltip")
    node = h.eval("function() return { %s } end" % fields)()
    h.eval('function(v, t, n) v:AddNodeName(t, n, {}) end')(view, tip, node)
    lines = tooltip_lines(h, tip)
    return _norm(lines[0]) if lines else ""


def test_id_conquered_tattoo_name(fork):
    """ST-E3 -- 名稱行 id 英文不符時,被征服節點查 legion、刺青節點查 tattoo,再退 passives。"""
    conq = 'conqueredBy = { id = 1234, conqueror = { type = "vaal", id = 1 } }'
    h = strict_id_harness(fork)
    got = e3_name(h, 'id = 90001, dn = "Bleed Avoidance", ' + conq)
    check("%s ST-E3 被征服節點(英文不符)名稱行用 legion 來源譯名" % fork,
          got == "流血避免" and "legion" in srcs_of(h, "Bleed Avoidance"),
          "got=%r lookups=%r" % (got, h.lookups))
    check("%s ST-E3 被征服節點名稱查詢後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)
    h = strict_id_harness(fork)
    got = e3_name(h, 'id = 90001, dn = "Tattoo of the Kitava Makanga", isTattoo = true')
    check("%s ST-E3 刺青節點(英文不符)名稱行用 tattoo 來源譯名" % fork,
          got == "奇塔弗鑄造的紋身" and "tattoo" in srcs_of(h, "Tattoo of the Kitava Makanga"),
          "got=%r lookups=%r" % (got, h.lookups))
    h = strict_id_harness(fork)
    got = e3_name(h, 'id = 90001, dn = "Lord of the Dead", ' + conq)
    check("%s ST-E3 legion 表沒有的名稱退回 passives 來源" % fork,
          got == "亡者之主", "got=%r lookups=%r" % (got, h.lookups))
    h = strict_id_harness(fork)
    got = e3_name(h, 'id = 90001, dn = "Elemental Overload", ' + conq)
    check("%s ST-E3 被征服但名稱沒換(id 英文相符)仍用 id 譯文" % fork,
          got == "戰神", "got=%r id_lookups=%r" % (got, h.id_lookups))
    h = strict_id_harness(fork)
    got = e3_name(h, 'id = 90001, dn = "Bleed Avoidance"')
    check("%s ST-E3 沒有 conqueredBy / isTattoo 時照 P0 退回 passives(不查 legion)" % fork,
          "legion" not in srcs_of(h, "Bleed Avoidance") and got == "流血迴避(全域)",
          "got=%r lookups=%r" % (got, h.lookups))


# ---- ST-E1:物品詞綴被折成相鄰兩行 ---------------------------------------------
#
# POB AddItemTooltip(PoE1 ItemsTab.lua L4471-4486、PoE2 約 L4530 起):依序走
#   enchant / scourge(PoE1) / rune(PoE2) / implicit / explicit / crucible(PoE1) 各清單,
#   每條 modLine 以 itemLib.formatModLine(modLine, dbMode) 取得顯示字(套 range、
#   extra 時 UNSUPPORTED 色碼並接 main.notSupportedTooltipText),AddLine 第 4 參數
#   PoE1 是 modLine、PoE2 是 background;每個清單後 AddSeparator。
# 顯示字 != modLine.line(range、註記),所以配對要以 formatModLine 的結果為準。

E1_A = "Recover 5% of Life when Leech is"
E1_B = "removed by Filling Unreserved Life"
E1_ZH = ["當汲取因填滿未保留生命而移除時", "回復 5% 生命"]
E1_STAT_DICT = {E1_A + "\n" + E1_B: "\n".join(E1_ZH)}

E1_PRE = b"""
itemLib = itemLib or { }
function itemLib.formatModLine(modLine, dbMode)
	local line = modLine.line
	if modLine.extra then
		line = main.notSupportedModTooltips and (line .. main.notSupportedTooltipText) or line
		return colorCodes.UNSUPPORTED .. line
	end
	return colorCodes.MAGIC .. line
end
colorCodes.UNSUPPORTED = "^xF05050"
main.notSupportedModTooltips = true
main.notSupportedTooltipText = " ^8(Not supported in PoB yet)"
"""

E1_ITEMTIP = """
function C:AddItemTooltip(tooltip, item, slot, dbMode, maxWidth)
	tooltip:AddLine(20, colorCodes[item.rarity]..item.title, "FONTIN SC")
	for _, modList in ipairs{item.enchantModLines, item.implicitModLines, item.explicitModLines} do
		if modList[1] then
			for _, modLine in ipairs(modList) do
				local formattedModLine = itemLib.formatModLine(modLine, dbMode)
				if formattedModLine then
					tooltip:AddLine(16, formattedModLine, "FONTIN SC", MODLINE_ARG)
				end
			end
			tooltip:AddSeparator(10)
		end
	end
end
"""
E1_ITEMSTAB = (
    'local C = newClass("ItemsTab")\nfunction C:ItemsTab() return self end\n'
    + E1_ITEMTIP.replace("MODLINE_ARG", "modLine"),
    'local C = newClass("ItemsTab", function(self) end)\n'
    + E1_ITEMTIP.replace("MODLINE_ARG", "nil"))


def e1_tooltips(fork, stat_dict, items, define=True):
    """依序對多件物品各建一個物品提示;回 (h, ok, err, [[原始詞綴行...] 每件])。"""
    h = stat_harness(fork, stat_dict, define, itemstab=E1_ITEMSTAB, pre=E1_PRE)
    tab = make(h, "ItemsTab")
    out, ok, err = [], True, None
    for src in items:
        tip = make(h, "Tooltip")
        item = h.eval("function() return %s end" % src)()
        o, e = h.eval('function(tab, t, item) local ok, e = pcall(function() '
                      'tab:AddItemTooltip(t, item) end) return ok, e end')(tab, tip, item)
        ok, err = ok and o, err or e
        out.append([l for l in tooltip_lines(h, tip) if l is not None][1:])
    return h, ok, err, out


def e1_item(explicit, implicit="", extra=False):
    def mods(lines):
        return ", ".join('{ line = %r%s }' % (l, ", extra = 'x'" if extra else "")
                         for l in lines)
    return ('{ rarity = "RARE", title = "Doom Loop", base = {}, enchantModLines = { }, '
            'implicitModLines = { %s }, explicitModLines = { %s } }'
            % (mods(implicit.split("|")) if implicit else "", mods(explicit)))


def test_stat_multiline_item_mods(fork):
    """ST-E1 -- 物品詞綴相鄰兩行以 "\\n" 合併查 stat(item),恰回兩行則各取一行。"""
    MAGIC = "^x8888FF"
    h, ok, err, tips = e1_tooltips(
        fork, E1_STAT_DICT, [e1_item([E1_A, E1_B, "+30 to Dexterity"])])
    lines = tips[0]
    check("%s ST-E1 物品提示本身不出錯" % fork, ok, repr(err))
    check("%s ST-E1 以 \"\\n\" 合併句且 ctx item 呼叫 PobToolsTranslateStat" % fork,
          (E1_A + "\n" + E1_B, "item") in [(_norm_ml(l), c) for l, c in h.stat_calls],
          repr(h.stat_calls))
    check("%s ST-E1 合併命中時兩行各顯示對應中文行並保留色碼" % fork,
          lines[:2] == [MAGIC + z for z in E1_ZH], "lines=%r calls=%r" % (lines, h.stat_calls))
    check("%s ST-E1 未配對的詞綴行照現行" % fork,
          _norm(lines[2]) == "+30 敏捷" if len(lines) > 2 else False, "lines=%r" % (lines,))
    # 不支援註記:合併查詢不含註記,命中後補回行尾
    h, ok, err, tips = e1_tooltips(fork, E1_STAT_DICT, [e1_item([E1_A, E1_B], extra=True)])
    note = " ^8(Not supported in PoB yet)"
    check("%s ST-E1 不支援詞綴命中後中文行尾補回註記與原色碼" % fork,
          tips[0][:2] == ["^xF05050" + z + note for z in E1_ZH],
          "lines=%r calls=%r" % (tips[0], h.stat_calls))
    # 未命中:與未定義函式時相同
    h, ok, err, tips = e1_tooltips(fork, {}, [e1_item([E1_A, E1_B])])
    h0, ok0, err0, tips0 = e1_tooltips(fork, None, [e1_item([E1_A, E1_B])], define=False)
    check("%s ST-E1 合併未命中時兩行照現行" % fork,
          ok and tips[0] == tips0[0] and [_norm(l) for l in tips[0][:2]] == [E1_A, E1_B],
          "lines=%r 未定義時=%r" % (tips[0], tips0[0]))
    # 不跨清單配對(implicit 最後一行 + explicit 第一行)
    h, ok, err, tips = e1_tooltips(fork, E1_STAT_DICT, [e1_item([E1_B], implicit=E1_A)])
    check("%s ST-E1 不同詞綴清單的相鄰行不配對" % fork,
          [_norm(l) for l in tips[0][:2]] == [E1_A, E1_B]
          and (E1_A + "\n" + E1_B) not in [_norm_ml(l) for l, c in h.stat_calls],
          "lines=%r calls=%r" % (tips[0], h.stat_calls))
    # 對照只屬該物品:下一件物品的同一行不沿用
    h, ok, err, tips = e1_tooltips(fork, E1_STAT_DICT,
                                   [e1_item([E1_A, E1_B]), e1_item([E1_A, "+30 to Dexterity"])])
    check("%s ST-E1 前提:第一件物品配對成功" % fork,
          tips[0][:2] == [MAGIC + z for z in E1_ZH], "lines=%r" % (tips[0],))
    check("%s ST-E1 物品提示結束後對照清空,下一件同一行不沿用" % fork,
          _norm(tips[1][0]) == E1_A if tips[1] else False, "lines=%r" % (tips[1],))
    check("%s ST-E1 結束後來源還原為 nil" % fork, h.source is None, "source=%r" % h.source)


def magic_item(prefix_mod, suffix_mod):
    def mods(m):
        return "{ { modId = %r } }" % m if m else "nil"
    return """
{
	rarity = "MAGIC",
	namePrefix = "Heavy ",
	baseName = "Glorious Plate",
	nameSuffix = " of the Whale",
	base = {},
	name = "Heavy Glorious Plate of the Whale",
	prefixes = %s,
	suffixes = %s,
	explicitModLines = {},
}
""" % (mods(prefix_mod), mods(suffix_mod))


def magic_title(fork, prefix_mod, suffix_mod):
    h, tip, ok, err = run_item_tooltip(fork, True, magic_item(prefix_mod, suffix_mod))
    lines = tooltip_lines(h, tip)
    return h, ok, err, (_norm(lines[0]) if lines else "")


def test_id_affix_title(fork):
    """L8"""
    ha, oka, erra, a = magic_title(fork, "TestModHeavyA", None)
    hb, okb, errb, b = magic_title(fork, "TestModHeavyB", None)
    hc, okc, errc, c = magic_title(fork, None, None)
    check("%s L8 前綴英文相同、modId 不同的兩件物品各取自己 id 的譯名" % fork,
          a.startswith("厚重的甲") and b.startswith("厚重的乙"),
          "a=%r b=%r id_lookups=%r" % (a, b, ha.id_lookups))
    check("%s L8 前綴以 PobToolsTranslateId(\"affixes\", modId, ...) 查詢" % fork,
          any(s == "affixes" and i == "TestModHeavyA" for s, i, _ in ha.id_lookups),
          repr(ha.id_lookups))
    check("%s L8 沒有 modId 時前綴退回英文查找" % fork,
          okc and c.startswith("厚重的") and not c.startswith("厚重的甲")
          and not c.startswith("厚重的乙") and "榮耀鎧甲" in c
          and not any(s == "affixes" and i == "TestModHeavyA" for s, i, _ in hc.id_lookups)
          and (a.startswith("厚重的甲")),
          "c=%r a=%r" % (c, a))
    hs, oks, errs, sfx = magic_title(fork, None, "TestModWhaleA")
    check("%s L8 後綴帶 modId 時同樣依 id 取譯名" % fork,
          sfx.endswith("鯨之甲") and sfx.startswith("厚重的"),
          "title=%r id_lookups=%r" % (sfx, hs.id_lookups))


# ---- stat 查詢(ST-L1 ~ ST-L3b) ---------------------------------------------
#
# 引擎提供 PobToolsTranslateStat(line, ctx),ctx = "item" / "passive",命中回中文、
# 否則 nil。詞綴行與天賦說明行先問它,未命中才走現行路徑;名稱行不問。
# 假函式以去色碼後的文字為鍵;收到帶色碼的行時連同色碼回傳(模擬引擎保留色碼),
# 所以注入層「去色碼再補回」或「整行交給引擎」兩種做法都會通過 ST-L3b。

STAT_DICT = {
    "+30 to Dexterity": "STAT中文",
    "10% increased Attack Speed": "STAT天賦",
}


def stat_harness(fork, stat_dict, define=True, itemstab=None, pre=None):
    """tooltip_harness 加上假 PobToolsTranslateStat(注入前定義);h.stat_calls 記 (line, ctx)。"""
    h = Harness(fork, with_inject=False)
    h.loose_keys = True
    h.stat_calls = []

    def translate_stat(line, ctx=None):
        h.stat_calls.append((line, ctx))
        if line is None:
            return None
        m = re.match(r"^((?:\^x[0-9A-Fa-f]{6}|\^\d)*)(.*)$", line, re.S)
        hit = (stat_dict or {}).get(m.group(2).strip())
        if hit is None:
            return None
        return m.group(1) + hit

    if define:
        h.rt.globals()["__py_translate_stat"] = translate_stat
        h.run(b"function PobToolsTranslateStat(l, c) return __py_translate_stat(l, c) end")
    h.run(COLOUR_SETUP)
    if pre:
        h.run(pre)
    h.run('LoadModule("Classes/Tooltip")')
    h.define("PassiveTreeView", PASSIVETREE_P1, PASSIVETREE_P2)
    if itemstab:
        h.define("ItemsTab", itemstab[0], itemstab[1])
    else:
        h.define("ItemsTab", ITEMSTAB_P1, ITEMSTAB_P2)
    h.define("ItemDBControl", ITEMDB_P1 + ROWVALUE, ITEMDB_P2 + ROWVALUE)
    h.apply_inject()
    h.lookups = []
    return h


def stat_mod_line(fork, text, stat_dict, define=True):
    """PoE1 AddLine(16, "^x8888FF"..text, font, { line = text });回 (h, ok, err, 原始行)。"""
    h = stat_harness(fork, stat_dict, define)
    tip = make(h, "Tooltip")
    ok, err = h.eval('function(t, s) local ok, e = pcall(function() '
                     't:AddLine(16, "^x8888FF" .. s, "FONTIN SC", { line = s }) end) '
                     'return ok, e end')(
        tip, text)
    lines = tooltip_lines(h, tip)
    return h, ok, err, (lines[0] if lines else "")


def stat_node_tooltip(fork, stat_dict, define=True):
    """AddNodeTooltip:名稱行 Elemental Overload + 說明行 10% increased Attack Speed。"""
    h = stat_harness(fork, stat_dict, define)
    view = make(h, "PassiveTreeView")
    tip = make(h, "Tooltip")
    ok, err = h.eval('function(v, t) local ok, e = pcall(function() v:AddNodeTooltip(t, '
                     '{ dn = "Elemental Overload", id = 1, sd = { "10% increased Attack Speed" } }, '
                     '{}) end) return ok, e end')(view, tip)
    return h, ok, err, [_norm(l) for l in tooltip_lines(h, tip)]


def test_stat_mod_line(fork):
    """ST-L1 / ST-L2 / ST-L2c / ST-L3b -- 只有 PoE1 的 AddLine 有 modLine 第 4 參數。"""
    if fork != "PoE1":
        print("  N/A   %s ST-L1/L2/L2c/L3b:此分支 AddLine 第 4 參數不是 modLine" % fork)
        return
    # ST-L1:命中就直接用,ctx 為 item
    h, ok, err, raw = stat_mod_line(fork, "+30 to Dexterity", STAT_DICT)
    check("%s ST-L1 詞綴行先走 stat 查詢:AddLine 寫入 stat 的譯文" % fork,
          ok and _norm(raw) == "STAT中文",
          "raw=%r calls=%r err=%r" % (raw, h.stat_calls, err))
    check("%s ST-L1 詞綴行呼叫 PobToolsTranslateStat 時 ctx 為 item" % fork,
          any(_norm(l) == "+30 to Dexterity" and c == "item" for l, c in h.stat_calls),
          repr(h.stat_calls))
    check("%s ST-L1 stat 命中後不再走字典查找" % fork,
          not srcs_of(h, "+30 to Dexterity"), "lookups=%r" % h.lookups)
    # ST-L3b:色碼前綴保留,且不重複
    check("%s ST-L3b 帶色碼的詞綴行命中 stat 後保留色碼前綴" % fork,
          raw == "^x8888FFSTAT中文", "raw=%r calls=%r" % (raw, h.stat_calls))

    # ST-L2:假函式回 nil -> 與 L2a/L2b 期望值相同,且與未定義時一致
    for text, want in (("+30 to Dexterity", "+30 敏捷"),
                       ("Adds 1 to 2 Fire Damage to Attacks", "攻擊附加 1 - 2 火焰傷害")):
        h, ok, err, raw = stat_mod_line(fork, text, {})
        h0, ok0, err0, raw0 = stat_mod_line(fork, text, None, define=False)
        check("%s ST-L2 stat 未命中退回現行順序:%s -> %s" % (fork, text, want),
              ok and _norm(raw) == want and raw == raw0,
              "raw=%r 未定義時=%r err=%r" % (raw, raw0, err))
        check("%s ST-L2 stat 未命中時仍有以 item 呼叫過 PobToolsTranslateStat:%s" % (fork, text),
              any(_norm(l) == text and c == "item" for l, c in h.stat_calls),
              repr(h.stat_calls))
        check("%s ST-L2c 舊引擎無 PobToolsTranslateStat 不報錯:%s" % (fork, text),
              ok0 and _norm(raw0) == want, "raw=%r err=%r" % (raw0, err0))


def test_stat_passive(fork):
    """ST-L3 / ST-L2c(天賦)-- 說明行帶 passive 情境,名稱行不呼叫。"""
    h, ok, err, lines = stat_node_tooltip(fork, STAT_DICT)
    check("%s ST-L3 AddNodeTooltip 本身不出錯" % fork, ok, repr(err))
    check("%s ST-L3 天賦說明行呼叫 PobToolsTranslateStat 時 ctx 為 passive" % fork,
          any(_norm(l) == "10% increased Attack Speed" and c == "passive"
              for l, c in h.stat_calls), repr(h.stat_calls))
    check("%s ST-L3 天賦名稱行不呼叫 PobToolsTranslateStat" % fork,
          not any(_norm(l) == "Elemental Overload" for l, c in h.stat_calls),
          repr(h.stat_calls))
    check("%s ST-L3 天賦說明行命中 stat 後顯示 stat 的譯文" % fork,
          lines[1:2] == ["STAT天賦"], "lines=%r calls=%r" % (lines, h.stat_calls))
    check("%s ST-L3 天賦提示中不以 item 情境呼叫" % fork,
          not any(c == "item" for l, c in h.stat_calls), repr(h.stat_calls))
    # 未命中 / 未定義:名稱行與說明行照現行路徑
    want = ["元素超載", "增加 10% 攻擊速度"]
    h, ok, err, lines = stat_node_tooltip(fork, {})
    check("%s ST-L2 天賦說明行 stat 未命中時照現行路徑" % fork,
          ok and lines == want, "lines=%r err=%r" % (lines, err))
    h, ok, err, lines = stat_node_tooltip(fork, None, define=False)
    check("%s ST-L2c 舊引擎無 PobToolsTranslateStat 時天賦提示不報錯且照現行路徑" % fork,
          ok and lines == want, "lines=%r err=%r" % (lines, err))


JEWEL_ITEM = """
{
	rarity = "UNIQUE",
	title = "Kaom's Heart",
	baseName = "Glorious Plate",
	base = {},
	name = "Kaom's Heart, Glorious Plate",
	otherLines = { "^x7F7F7FLimited to: 1", "^x7F7F7FRadius: Large", "^x7F7F7F+12 Life (+1.2%)" },
	explicitModLines = { { line = "+30 to Dexterity" } },
}
"""


def test_stat_jewel_in_node(fork):
    """ST-L4 -- 天賦提示內嵌珠寶物品提示:非詞綴行不以 passive 查;PoE1 詞綴行仍以 item 查。"""
    h = stat_harness(fork, STAT_DICT)
    view = make(h, "PassiveTreeView")
    tip = make(h, "Tooltip")
    tab = make(h, "ItemsTab")
    jewel = h.eval("function() return %s end" % JEWEL_ITEM)()
    ok, err = h.eval('function(v, t, tab, j) local ok, e = pcall(function() v:AddNodeTooltip(t, '
                     '{ dn = "Elemental Overload", id = 1, sd = { "10% increased Attack Speed" }, '
                     'jewel = j }, { itemsTab = tab }) end) return ok, e end')(view, tip, tab, jewel)
    check("%s ST-L4 天賦提示內嵌物品提示本身不出錯" % fork, ok, repr(err))
    check("%s ST-L4 前提:天賦說明行仍以 passive 呼叫" % fork,
          any(_norm(l) == "10% increased Attack Speed" and c == "passive"
              for l, c in h.stat_calls), repr(h.stat_calls))
    item_lines = ("Kaom's Heart", "Glorious Plate", "Limited to: 1", "Radius: Large",
                  "+12 Life (+1.2%)", "+30 to Dexterity")
    bad = [(l, c) for l, c in h.stat_calls if _norm(l) in item_lines and c == "passive"]
    check("%s ST-L4 內嵌物品提示的行不以 passive 呼叫 PobToolsTranslateStat" % fork,
          not bad, "bad=%r calls=%r" % (bad, h.stat_calls))
    if fork == "PoE1":
        check("%s ST-L4 內嵌物品提示的詞綴行仍以 item 呼叫" % fork,
              any(_norm(l) == "+30 to Dexterity" and c == "item" for l, c in h.stat_calls),
              repr(h.stat_calls))
    check("%s ST-L4 結束後來源還原為 nil" % fork, h.source is None, "source=%r" % h.source)


# ---- 塗油 / 軍團 / 刺青的來源(ST-P3) -----------------------------------------
#
# 上游(PoE1 / PoE2 Classes/):
#   ItemsTab:anointItem -> enchantModLines { line = "Allocates " .. node.dn },之後經
#     AddItemTooltip 的 modline AddLine 顯示(PoE1 第 4 參數是 modLine;PoE2 是 background)。
#   NotableDBControl:GetRowValue(column, index, node) -> 色碼 .. node.dn(PoE1 CRAFTED、
#     PoE2 ENCHANTED;排序依 power 時為 ^xRR8080 / ^x808080)。
#   TreeTab:ModifyNodePopup(selectedNode)(只有 PoE1;刺青):區域 buildMods 建 modGroups,
#     label = node.dn .. 48 空白 .. table.concat(node.sd, ","),交給
#     controls.modSelect = DropDownControl,再 main:OpenPopup(...)。
#   TreeTab:FindTimelessJewel()(兩分支;軍團):區域 buildMods 建 modData,
#     label = node.dn .. 48 空白 .. node.sd[1];controls.nodeSelect = DropDownControl(modData);
#     換珠寶類型時 controls.jewelSelect 的 selFunc 會 wipeTable(modData) 後重建。
# 兩個下拉清單都是函式區域變數、在 popup 開著時才繪製(而且可能重建),所以合成版保留
# 這些形狀;測試只看「popup 開出來、畫過之後」清單項的 label 與查找來源。

LABEL_PAD = " " * 48

DICT["anoint"] = {"Whispers of Doom": "末日低語(塗油)"}
DICT["tattoo"].update({"Tattoo of the Ramako Scout": "拉瑪克斥候刺青(刺青)"})
DICT["legion"].update({"Ritual of Immolation": "獻祭儀式(軍團)",
                       "Hit and Run": "打帶跑(軍團)"})
DICT[None].update({
    "Allocates": "配置",
    # 現行整行查找的值(來源表沒有該名稱時要維持這個結果)
    "Allocates Heart of Oak": "配置 橡樹之心",
})

DROPDOWN_BODY = """
function C:Draw(viewPort)
	for _, v in ipairs(self.list or { }) do
		DrawString(0, 0, "LEFT", 16, "VAR", type(v) == "table" and v.label or v)
	end
end
"""
DROPDOWN_P1 = """
local C = newClass("DropDownControl")
function C:DropDownControl(anchor, rect, list, selFunc)
	self.list = list self.selFunc = selFunc self.selIndex = 1
	return self
end
""" + DROPDOWN_BODY
DROPDOWN_P2 = """
local C = newClass("DropDownControl", function(self, anchor, rect, list, selFunc)
	self.list = list self.selFunc = selFunc self.selIndex = 1
end)
""" + DROPDOWN_BODY

POPUP_SETUP = b"""
function main:OpenPopup(width, height, title, controls)
	local popup = { width = width, height = height, title = title, controls = controls }
	table.insert(self.popups, 1, popup)
	return popup
end
function main:ClosePopup() table.remove(self.popups, 1) end
"""

TREETAB_METHODS = """
function C:ModifyNodePopup(selectedNode)
	local controls = { }
	local modGroups = { }
	local treeNodes = self.build.spec.tree.nodes
	local nodeName = treeNodes[selectedNode.id].dn
	local function buildMods(selectedNode)
		wipeTable(modGroups)
		for id, node in pairs(self.build.spec.tree.tattoo.nodes) do
			if nodeName:match(node.targetType:gsub("^Small ", "")) then
				table.insert(modGroups, {
				label = node.dn .. "%(pad)s" .. table.concat(node.sd, ","),
				descriptions = copyTable(node.sd),
				id = id,
				})
			end
		end
		table.sort(modGroups, function(a, b) return a.label < b.label end)
	end
	buildMods(selectedNode)
	controls.modSelect = NEW_DROPDOWN(nil, {175, 25, 250, 18}, modGroups, function(idx) end)
	main:OpenPopup(600, 100, "Modify Node", controls, "save")
end
function C:FindTimelessJewel()
	local treeData = self.build.spec.tree
	local legionNodes = treeData.legion.nodes
	local timelessData = self.build.timelessData
	local controls = { }
	local modData = { }
	local jewelTypes = self.jewelTypes
	local function buildMods()
		wipeTable(modData)
		for _, node in pairs(legionNodes) do
			if node.id:match("^" .. timelessData.jewelType.name .. "_.+") and node["not"] then
				table.insert(modData, {
					label = node.dn .. "%(pad)s" .. node.sd[1],
					descriptions = copyTable(node.sd),
					type = timelessData.jewelType.name,
					id = node.id
				})
			end
		end
		table.sort(modData, function(a, b) return a.label < b.label end)
	end
	controls.jewelSelect = NEW_DROPDOWN(nil, {520, 25, 200, 18}, jewelTypes, function(index, value)
		timelessData.jewelType = value
		buildMods()
	end)
	buildMods()
	controls.nodeSelect = NEW_DROPDOWN(nil, {0, 7, 200, 18}, modData, function(index, value) end)
	main:OpenPopup(910, 517, "Find a Timeless Jewel", controls, nil, nil, "close")
end
""" % {"pad": LABEL_PAD}

TREETAB_P1 = """
local C = newClass("TreeTab")
function C:TreeTab(build) self.build = build return self end
""" + TREETAB_METHODS.replace(
    "NEW_DROPDOWN(", 'new("DropDownControl"):DropDownControl(')
TREETAB_P2 = """
local C = newClass("TreeTab", function(self, build) self.build = build end)
""" + TREETAB_METHODS.replace("NEW_DROPDOWN(", 'new("DropDownControl", ')

# NotableDBControl:GetRowValue 照上游(PoE1 CRAFTED、PoE2 ENCHANTED)
NOTABLE_ROW = """
function C:GetRowValue(column, index, node)
	if column == 1 then
		if self.sortDetail and self.sortDetail.stat then
			return "^x808080"..node.dn
		else
			return colorCodes.ROWCODE..node.dn
		end
	end
end
"""
NOTABLE_P1 = """
local C = newClass("NotableDBControl")
function C:NotableDBControl() self.controls = { search = { buf = "" } } return self end
""" + NOTABLE_ROW.replace("ROWCODE", "CRAFTED")
NOTABLE_P2 = """
local C = newClass("NotableDBControl", function(self) self.controls = { search = { buf = "" } } end)
""" + NOTABLE_ROW.replace("ROWCODE", "ENCHANTED")

P3_BUILD = """
{
	spec = { tree = {
		nodes = { [100] = { dn = "Strength", sd = { "+10 to Strength" } } },
		tattoo = { nodes = {
			ramako = { dn = "Tattoo of the Ramako Scout", targetType = "Small Strength",
				targetValue = "", sd = { "+2 to Strength" } },
			unknown = { dn = "Tattoo of the Unknown", targetType = "Small Strength",
				targetValue = "", sd = { "+1 to Strength" } },
		} },
		legion = { nodes = {
			{ id = "glorious_vanity_notable_1", dn = "Ritual of Immolation", ["not"] = true,
				sd = { "Burn things" } },
			{ id = "glorious_vanity_notable_2", dn = "Unknown Vaal Notable", ["not"] = true,
				sd = { "Something" } },
			{ id = "lethal_pride_notable_1", dn = "Hit and Run", ["not"] = true,
				sd = { "Run things" } },
		} },
	} },
	timelessData = { },
}
"""


def p3_harness(fork, with_inject=True):
    h = Harness(fork, with_inject=False)
    h.loose_keys = True
    h.run(COLOUR_SETUP)
    h.run(b'colorCodes.CRAFTED = "^xB8DAF1" colorCodes.ENCHANTED = "^xB8DAF1"')
    h.run(POPUP_SETUP)
    h.run('LoadModule("Classes/Tooltip")')
    h.define("ItemsTab", ITEMSTAB_P1, ITEMSTAB_P2)
    h.define("DropDownControl", DROPDOWN_P1, DROPDOWN_P2)
    h.define("TreeTab", TREETAB_P1, TREETAB_P2)
    h.define("NotableDBControl", NOTABLE_P1, NOTABLE_P2)
    if with_inject:
        h.apply_inject()
    h.lookups = []
    return h


def p3_tree_tab(h):
    build = h.eval("function() return %s end" % P3_BUILD)()
    if h.fork == "PoE1":
        tab = h.eval('function(b) return new("TreeTab"):TreeTab(b) end')(build)
    else:
        tab = h.eval('function(b) return new("TreeTab", b) end')(build)
    tab.jewelTypes = h.eval('function() return { { id = 1, name = "glorious_vanity" }, '
                            '{ id = 2, name = "lethal_pride" } } end')()
    build.timelessData.jewelType = tab.jewelTypes[1]
    return tab


def p3_popup_labels(h, control):
    """畫一次 popup 的指定下拉後回傳各項 label(去色碼)。"""
    return [_norm(x) for x in h.eval(
        'function(name) local c = main.popups[1].controls[name] c:Draw({}) '
        'local o = {} for i, v in ipairs(c.list) do o[i] = v.label end return o end')(
        control).values()]


def allocates_line(fork, name, with_inject=True):
    h = p3_harness(fork, with_inject)
    tab = make(h, "ItemsTab")
    tip = make(h, "Tooltip")
    item = h.eval('function(n) return { rarity = "RARE", title = "Doom Loop", '
                  'baseName = "Amber Amulet", base = {}, name = "Doom Loop, Amber Amulet", '
                  'explicitModLines = { { crafted = true, line = "Allocates " .. n } } } end')(name)
    h.lookups = []
    ok, err = h.eval('function(tab, t, item) local ok, e = pcall(function() '
                     'tab:AddItemTooltip(t, item) end) return ok, e end')(tab, tip, item)
    lines = [l for l in tooltip_lines(h, tip) if l is not None]
    return h, ok, err, (_norm(lines[-1]) if lines else "")


def test_src_anoint_legion_tattoo(fork):
    """ST-P3 -- 塗油名稱走 anoint、軍團下拉走 legion、刺青下拉走 tattoo。"""
    # (1) Allocates 行
    h, ok, err, line = allocates_line(fork, "Whispers of Doom")
    check("%s ST-P3 Allocates 行本身不出錯" % fork, ok, repr(err))
    check("%s ST-P3 Allocates 行的名稱在 anoint 來源下查詢" % fork,
          "anoint" in srcs_of(h, "Whispers of Doom"), "lookups=%r" % h.lookups)
    check("%s ST-P3 Allocates 行顯示前綴譯文與 anoint 名稱譯文" % fork,
          "配置" in line and "末日低語(塗油)" in line and not re.search(r"[A-Za-z]", line),
          "line=%r" % line)
    check("%s ST-P3 Allocates 行結束後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)
    # (5) 名稱不在 anoint 表:整行照現行
    h, ok, err, line = allocates_line(fork, "Heart of Oak")
    h0, ok0, err0, line0 = allocates_line(fork, "Heart of Oak", with_inject=False)
    check("%s ST-P3 anoint 表沒有該名稱時 Allocates 行照現行" % fork,
          ok and line == "配置 橡樹之心", "line=%r 未注入時=%r" % (line, line0))

    # (2) NotableDBControl 列表
    for with_inject in (True, False):
        h = p3_harness(fork, with_inject)
        db = make(h, "NotableDBControl")
        row = h.eval('function(o, dn) return o:GetRowValue(1, 1, { dn = dn }) end')
        if with_inject:
            v = row(db, "Whispers of Doom")
            check("%s ST-P3 塗油列表項在 anoint 來源下查詢" % fork,
                  "anoint" in srcs_of(h, "Whispers of Doom"), "lookups=%r" % h.lookups)
            check("%s ST-P3 塗油列表項顯示 anoint 譯文且保留色碼" % fork,
                  v == "^xB8DAF1末日低語(塗油)", "v=%r" % v)
            check("%s ST-P3 塗油列表查詢後來源還原為 nil" % fork,
                  h.source is None, "source=%r" % h.source)
            miss = row(db, "Heart of Oak")
        else:
            miss0 = row(db, "Heart of Oak")
    check("%s ST-P3 anoint 表沒有該名稱時列表項同現行" % fork,
          miss == miss0, "v=%r 未注入時=%r" % (miss, miss0))

    # (3) 軍團下拉(兩分支)
    h = p3_harness(fork)
    tab = p3_tree_tab(h)
    h.eval('function(t) t:FindTimelessJewel() end')(tab)
    labels = p3_popup_labels(h, "nodeSelect")
    check("%s ST-P3 軍團下拉的 dn 在 legion 來源下查詢" % fork,
          "legion" in srcs_of(h, "Ritual of Immolation"), "lookups=%r" % h.lookups)
    check("%s ST-P3 軍團下拉 label 的 dn 段換成 legion 譯文、後段 sd 不變" % fork,
          ("獻祭儀式(軍團)" + LABEL_PAD + "Burn things").strip() in labels,
          "labels=%r" % labels)
    check("%s ST-P3 legion 表沒有的 dn 維持英文 label" % fork,
          ("Unknown Vaal Notable" + LABEL_PAD + "Something").strip() in labels,
          "labels=%r" % labels)
    check("%s ST-P3 軍團下拉繪製後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)
    # 換珠寶類型 -> buildMods 重建 modData,新項目同樣要翻
    h.eval('function() local c = main.popups[1].controls.jewelSelect '
           'c.selFunc(2, c.list[2]) end')()
    labels = p3_popup_labels(h, "nodeSelect")
    check("%s ST-P3 換珠寶類型重建清單後新項目也以 legion 譯文顯示" % fork,
          ("打帶跑(軍團)" + LABEL_PAD + "Run things").strip() in labels,
          "labels=%r" % labels)

    # (3) 刺青下拉(只有 PoE1 有 ModifyNodePopup)
    if fork != "PoE1":
        print("  N/A   %s ST-P3 刺青:此分支 TreeTab 沒有 ModifyNodePopup" % fork)
        return
    h = p3_harness(fork)
    tab = p3_tree_tab(h)
    h.eval('function(t) t:ModifyNodePopup({ id = 100 }) end')(tab)
    labels = p3_popup_labels(h, "modSelect")
    check("%s ST-P3 刺青下拉的 dn 在 tattoo 來源下查詢" % fork,
          "tattoo" in srcs_of(h, "Tattoo of the Ramako Scout"), "lookups=%r" % h.lookups)
    check("%s ST-P3 刺青下拉 label 的 dn 段換成 tattoo 譯文、後段 sd 不變" % fork,
          ("拉瑪克斥候刺青(刺青)" + LABEL_PAD + "+2 to Strength") in labels,
          "labels=%r" % labels)
    check("%s ST-P3 tattoo 表沒有的 dn 維持英文 label" % fork,
          ("Tattoo of the Unknown" + LABEL_PAD + "+1 to Strength") in labels,
          "labels=%r" % labels)
    check("%s ST-P3 刺青下拉繪製後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)


# ---- 與引擎 source patch 並存(ST-FB) -----------------------------------------
#
# ui_api.cpp「刺青與軍團珠寶下拉的名稱維持英文」把 treetab.lua 的
#   label = node.dn .. "
# 換成
#   label = ((PobToolsTranslateDisplay and PobToolsTranslateDisplay(node.dn)) or node.dn) .. "
# 建清單時(來源 nil)就寫入全域譯名,注入層之後拿到的 dn 段已是中文,
# legion / tattoo 來源查不到。這組測試在合成 TreeTab 上套真的 source patch。

DICT[None].update({
    "Bleed Avoidance": "流血迴避(全域)",
    "Tattoo of the Ramako Scout": "拉瑪克斥候紋身(全域)",
})
DICT["legion"]["Bleed Avoidance"] = "流血避免"


def fb_harness(fork):
    """p3_harness,但 TreeTab 用套過引擎 treetab.lua source patch 的版本。"""
    src = TREETAB_P1 if fork == "PoE1" else TREETAB_P2
    patched, applied = apply_source_patches(
        src, "treetab.lua", parse_source_patches(read_text(UI_API)))
    h = p3_harness(fork, with_inject=False)
    h.run(patched)
    h.apply_inject()
    h.lookups = []
    return h, patched, applied


def fb_build(h):
    tab = p3_tree_tab(h)
    tab.build.spec.tree.legion.nodes[4] = h.eval(
        'function() return { id = "glorious_vanity_notable_3", dn = "Bleed Avoidance", '
        '["not"] = true, sd = { "Avoid bleed" } } end')()
    return tab


def test_src_legion_tattoo_with_source_patch(fork):
    """ST-FB -- 套用引擎 source patch 後,下拉 label 的 dn 段仍是 legion / tattoo 來源譯名。"""
    h, patched, applied = fb_harness(fork)
    check("%s ST-FB 前提:引擎 source patch 確實改寫了合成 TreeTab 的 label 式" % fork,
          applied >= 1 and "PobToolsTranslateDisplay(node.dn)" in patched,
          "applied=%d" % applied)
    tab = fb_build(h)
    h.eval('function(t) t:FindTimelessJewel() end')(tab)
    labels = p3_popup_labels(h, "nodeSelect")
    want = ("流血避免" + LABEL_PAD + "Avoid bleed").strip()
    check("%s ST-FB 軍團下拉 label 的 dn 段是 legion 來源譯名而非全域譯名" % fork,
          want in labels, "labels=%r lookups=%r" % (labels, h.lookups))
    check("%s ST-FB 軍團下拉 label 不殘留全域譯名" % fork,
          not any(l.startswith("流血迴避(全域)") for l in labels), "labels=%r" % labels)
    check("%s ST-FB 軍團下拉繪製後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)
    if fork != "PoE1":
        print("  N/A   %s ST-FB 刺青:此分支 TreeTab 沒有 ModifyNodePopup" % fork)
        return
    h, patched, applied = fb_harness(fork)
    tab = fb_build(h)
    h.eval('function(t) t:ModifyNodePopup({ id = 100 }) end')(tab)
    labels = p3_popup_labels(h, "modSelect")
    check("%s ST-FB 刺青下拉 label 的 dn 段是 tattoo 來源譯名而非全域譯名" % fork,
          any(l.startswith("拉瑪克斥候刺青(刺青)" + LABEL_PAD) for l in labels),
          "labels=%r lookups=%r" % (labels, h.lookups))
    check("%s ST-FB 刺青下拉 label 不殘留全域譯名" % fork,
          not any(l.startswith("拉瑪克斥候紋身(全域)") for l in labels), "labels=%r" % labels)


# ---- SF / TC:三種下拉「名稱 + 48 空白 + 隱藏搜尋字」(使用者裁定 2026-10-28 方案 C)----------
# SF:inject 替清單每一項設 searchFilter = 畫面 label + 英文名稱 + 英文詞綴 + 中文詞綴(中英名稱、詞綴都搜得到),
#     以畫面 label 開頭,反白位置(POB 依 searchFilter 的位元組位置畫在 label 上)才對得上。
# TC:引擎 treetab.lua 補丁改成逐 sd 行翻譯再以逗號接(句子內含逗號時不再切成半中半英)。
DICT[None].update({"Burn things": "燃燒事物", "+2 to Strength": "+2 力量",
                   "Zzc first, second part": "第一句，含逗號", "Zzc third": "第三句"})


def _sf_item(h, control, prefix):
    return h.eval('function(c, p) local dd = main.popups[1].controls[c] dd:Draw({}) '
                  'for _, v in ipairs(dd.list) do if type(v.label) == "string" and v.label:sub(1, #p) == p then '
                  'return v.label, v.searchFilter end end end')(control, prefix)


def test_sf_dropdown_search_filter(fork):
    """SF1 ~ SF2 -- 下拉清單項的 searchFilter 含英文名稱、英文詞綴、中文名稱、中文詞綴,且以畫面 label 開頭。"""
    h, patched, applied = fb_harness(fork)
    tab = fb_build(h)
    h.eval('function(t) t:FindTimelessJewel() end')(tab)
    label, sf = _sf_item(h, "nodeSelect", "獻祭儀式(軍團)")
    want = ["Ritual of Immolation", "Burn things", "獻祭儀式(軍團)", "燃燒事物"]
    check("%s SF1 永恆珠寶清單項 searchFilter 含英文名稱/英文詞綴/中文名稱/中文詞綴" % fork,
          sf is not None and all(w in sf for w in want), "label=%r sf=%r" % (label, sf))
    check("%s SF1 searchFilter 以畫面 label 開頭(反白位置對得上)" % fork,
          sf is not None and label is not None and sf.startswith(label), "label=%r sf=%r" % (label, sf))
    if fork != "PoE1":
        print("  N/A   %s SF2/TC1:此分支 TreeTab 沒有 ModifyNodePopup" % fork)
        return
    h, patched, applied = fb_harness(fork)
    tab = fb_build(h)
    h.eval('function(t) t:ModifyNodePopup({ id = 100 }) end')(tab)
    label, sf = _sf_item(h, "modSelect", "拉瑪克斥候刺青(刺青)")
    want = ["Tattoo of the Ramako Scout", "+2 to Strength", "拉瑪克斥候刺青(刺青)", "+2 力量"]
    check("%s SF2 紋身清單項 searchFilter 含英文名稱/英文詞綴/中文名稱/中文詞綴" % fork,
          sf is not None and all(w in sf for w in want), "label=%r sf=%r" % (label, sf))
    check("%s SF2 searchFilter 以畫面 label 開頭" % fork,
          sf is not None and label is not None and sf.startswith(label), "label=%r sf=%r" % (label, sf))


def test_tc_tattoo_label_per_line(fork):
    """TC1 -- 紋身下拉 label 的詞綴段逐 sd 行翻譯再以逗號接,句內逗號不再切開。"""
    if fork != "PoE1":
        return
    h, patched, applied = fb_harness(fork)
    tab = fb_build(h)
    h.eval('function(t) t.build.spec.tree.tattoo.nodes.comma = { dn = "Tattoo of the Comma", '
           'targetType = "Small Strength", targetValue = "", sd = { "Zzc first, second part", "Zzc third" } } end')(tab)
    h.eval('function(t) t:ModifyNodePopup({ id = 100 }) end')(tab)
    labels = h.eval('function() local dd = main.popups[1].controls.modSelect local o = {} '
                    'for i, v in ipairs(dd.list) do o[i] = v.label end return o end')()
    labels = [labels[i] for i in range(1, len(labels) + 1)]
    hit = [l for l in labels if l.startswith("Tattoo of the Comma")]
    check("%s TC1 句內含逗號的 sd 行整行翻(不再半中半英)" % fork,
          hit and hit[0].endswith(LABEL_PAD + "第一句，含逗號,第三句"), "labels=%r" % hit)


DICT[None].update({"Zzp first": "第一句。", "Zzp second": "第二句"})


def test_tc_tattoo_label_period(fork):
    """TC3 -- 紋身下拉 label:前一段以「。」結尾時不再補逗號(不出現「。,」)。"""
    if fork != "PoE1":
        return
    h, patched, applied = fb_harness(fork)
    tab = fb_build(h)
    h.eval('function(t) t.build.spec.tree.tattoo.nodes.period = { dn = "Tattoo of the Period", '
           'targetType = "Small Strength", targetValue = "", sd = { "Zzp first", "Zzp second" } } end')(tab)
    h.eval('function(t) t:ModifyNodePopup({ id = 100 }) end')(tab)
    labels = h.eval('function() local dd = main.popups[1].controls.modSelect local o = {} '
                    'for i, v in ipairs(dd.list) do o[i] = v.label end return o end')()
    hit = [labels[i] for i in range(1, len(labels) + 1) if labels[i].startswith("Tattoo of the Period")]
    check("%s TC3 「。」結尾的段後不補逗號" % fork,
          hit and hit[0].endswith(LABEL_PAD + "第一句。第二句"), "labels=%r" % hit)


AD_TREE = """{
	nodes = {},
	legion = {
		nodes = {},
		additions = { { id = "karui_add_armour", dn = "Zzadd Armour", sd = { "+100 to Zzqx Defence" },
			stats = { sid_add_armour = { min = 100, max = 100, index = 1 } } } },
	},
	tattoo = { nodes = {} },
}"""
DICT["legion"]["Zzadd Armour"] = "增加護甲Z"


def test_sf_addition_english_name(fork):
    """AD1 -- 永恆珠寶清單由 legion.additions 來的列(名稱已改成中文)searchFilter 仍含英文名稱。"""
    h = sid_harness(fork, {})
    tab = make(h, "TreeTab")
    tree = h.eval("function() return %s end" % AD_TREE)()
    h.eval("function(tab, tree) tab.build = { spec = { tree = tree } } end")(tab, tree)
    ok, err = h.eval('function(tab) local ok, e = pcall(function() tab:FindTimelessJewel() '
                     'main.popups[1].controls.nodeSelect:Draw({}) end) return ok, e end')(tab)
    item = h.eval('function() for _, v in ipairs(main.popups[1].controls.nodeSelect.list) do '
                  'if v.id == "karui_add_armour" then return v.label, v.searchFilter end end end')()
    label, sf = item if isinstance(item, tuple) else (None, None)
    check("%s AD1 附加項名稱已改成中文(重現條件)" % fork, ok and label == "增加護甲Z", "label=%r err=%r" % (label, err))
    check("%s AD1 附加項 searchFilter 含英文名稱與英文詞綴" % fork,
          sf is not None and "Zzadd Armour" in sf and "+100 to Zzqx Defence" in sf, "sf=%r" % sf)


def test_sh_skip_zhraw_with_search_filter(fork):
    """SH1 -- 中文搜尋補比對:已有 searchFilter 的項目不再整串查表;沒有的照舊用整列翻譯比對。"""
    h = Harness(fork, with_inject=False)
    h.loose_keys = True
    h.run('LoadModule("Classes/SearchHost")')
    h.apply_inject()
    DICT[None]["Zzsh plain row"] = "乙詞列"
    h.lookups = []
    res = h.eval('''function()
		local SH = common.classes.SearchHost
		local list = { { label = "甲名", searchFilter = "甲名 Zzsh Alpha Zzsh stat" }, { label = "Zzsh plain row" } }
		local o = { searchTerm = "乙詞", searchInfos = {}, ignoreOrder = false, UpdateMatchCount = SH.UpdateMatchCount,
			searchListAccessor = function() return list end,
			valueAccessor = function(v) return v.searchFilter or v.label end }
		SH.UpdateSearch(o)
		return o.searchInfos[1] and o.searchInfos[1].matches or false, o.searchInfos[2] and o.searchInfos[2].matches or false
	end''')()
    m1, m2 = res if isinstance(res, tuple) else (None, None)
    looked = [t for t, s in h.lookups]
    check("%s SH1 有 searchFilter 的項目不再拿整串去查表" % fork,
          "甲名 Zzsh Alpha Zzsh stat" not in looked, "lookups=%r" % looked)
    check("%s SH1 沒有 searchFilter 的項目照舊以整列翻譯比對中文" % fork,
          m2 is True and m1 is False and "Zzsh plain row" in looked, "m1=%r m2=%r lookups=%r" % (m1, m2, looked))


TC2_SD = ["Zzb Recovery Rate of Life, Mana and Energy Shield", "zzb per Tribe for which you have a Tattoo",
          "Limited to 1 Zzb Tattoo"]
DICT[None]["Limited to 1 Zzb Tattoo"] = "限制 1 個 Zzb 紋身"


def test_tc_tattoo_label_grouped(fork):
    """TC2 -- 紋身下拉 label:GGPK 一句跨兩個 sd 行(Ancestral Tattoo of Bloodlines 型)照成組查表整句接起來。"""
    if fork != "PoE1":
        return
    h, patched, applied = fb_harness(fork)
    calls = []

    def lines(tbl, ctx=None):
        got = [tbl[i] for i in range(1, len(tbl) + 1)]
        calls.append((got, ctx))
        if got[:2] == TC2_SD[:2]:
            return h.rt.table_from({1: "每個你配置紋身的部落，", 2: "增加 2% 生命、魔力和能量護盾恢復率"})
        return None
    h.rt.globals()["__py_tc2_lines"] = lines
    h.run(b"function PobToolsTranslateLines(t, c) return __py_tc2_lines(t, c) end")
    tab = fb_build(h)
    h.eval('function(t, a, b, c) t.build.spec.tree.tattoo.nodes.blood = { dn = "Tattoo of the Zzb", '
           'targetType = "Small Strength", targetValue = "", sd = { a, b, c } } end')(tab, *TC2_SD)
    h.eval('function(t) t:ModifyNodePopup({ id = 100 }) end')(tab)
    labels = h.eval('function() local dd = main.popups[1].controls.modSelect local o = {} '
                    'for i, v in ipairs(dd.list) do o[i] = v.label end return o end')()
    hit = [labels[i] for i in range(1, len(labels) + 1) if labels[i].startswith("Tattoo of the Zzb")]
    check("%s TC2 跨兩行的一句照 GGPK 整句、接上限制行,沒有英文段也沒有多餘逗號" % fork,
          hit and hit[0].endswith(LABEL_PAD + "每個你配置紋身的部落，增加 2% 生命、魔力和能量護盾恢復率,限制 1 個 Zzb 紋身"),
          "labels=%r calls=%r" % (hit, calls))


# ---- 多行 stat 模板被折成兩個 sd 元素(ST-P2b) --------------------------------
#
# node 51108:GGPK 一個兩行模板,POB 拆成 sd 兩個元素逐行 AddLine。
# 假引擎只認 "\n" 合併句、回兩行中文;單行一律 nil。

SD_A = "10% increased Effect of Arcane Surge on you per"
SD_B = "200 Mana spent Recently, up to 50%"
P2B_STAT_DICT = {SD_A + "\n" + SD_B: "近期每消耗 200 魔力,\n秘術增強對你的效果增加 10%,最多 50%"}
P2B_ZH = ["近期每消耗 200 魔力,", "秘術增強對你的效果增加 10%,最多 50%"]


def p2b_tooltips(fork, stat_dict, nodes, define=True):
    """依序對多個節點各建一個天賦提示;回 (h, ok, err, [[原始行...] 每節點])。"""
    h = stat_harness(fork, stat_dict, define)
    view = make(h, "PassiveTreeView")
    out = []
    ok, err = True, None
    for i, sd in enumerate(nodes):
        tip = make(h, "Tooltip")
        sd_lua = h.eval("function(...) return {...} end")(*sd)
        o, e = h.eval('function(v, t, id, sd) local ok, e = pcall(function() '
                      'v:AddNodeTooltip(t, { dn = "Elemental Overload", id = id, sd = sd }, {}) '
                      'end) return ok, e end')(view, tip, 51108 + i, sd_lua)
        ok, err = ok and o, err or e
        out.append([l for l in tooltip_lines(h, tip) if l is not None])
    return h, ok, err, out


def test_stat_multiline_sd(fork):
    """ST-P2b -- 相鄰兩個 sd 行以 "\\n" 合併查 stat,命中兩行中文則各取一行。"""
    # (1)(2)(5)
    h, ok, err, tips = p2b_tooltips(fork, P2B_STAT_DICT, [[SD_A, SD_B]])
    lines = tips[0]
    check("%s ST-P2b 天賦提示本身不出錯" % fork, ok, repr(err))
    check("%s ST-P2b 合併句命中時兩行各顯示對應的中文行" % fork,
          [_norm(l) for l in lines[1:3]] == P2B_ZH,
          "lines=%r calls=%r" % (lines, h.stat_calls))
    check("%s ST-P2b 以 \"\\n\" 合併句且 ctx passive 呼叫 PobToolsTranslateStat" % fork,
          (SD_A + "\n" + SD_B, "passive") in [(_norm_ml(l), c) for l, c in h.stat_calls],
          repr(h.stat_calls))
    check("%s ST-P2b 帶色碼的說明行對上後補回原色碼前綴" % fork,
          lines[1:3] == ["^7" + z for z in P2B_ZH], "lines=%r" % (lines,))
    # (1b) 前一組配對失敗會往下滑:X+A 不中、A+B 中;X 照現行
    h, ok, err, tips = p2b_tooltips(
        fork, P2B_STAT_DICT, [["10% increased Attack Speed", SD_A, SD_B]])
    got = [_norm(l) for l in tips[0][1:4]]
    check("%s ST-P2b 三行中後兩行配對成功、第一行照現行" % fork,
          got == ["增加 10% 攻擊速度"] + P2B_ZH, "got=%r calls=%r" % (got, h.stat_calls))
    # (3) 合併查不到:兩行照現行逐行路徑(與未定義函式時相同)
    h, ok, err, tips = p2b_tooltips(fork, {}, [[SD_A, SD_B]])
    h0, ok0, err0, tips0 = p2b_tooltips(fork, None, [[SD_A, SD_B]], define=False)
    check("%s ST-P2b 合併句未命中時兩行照現行逐行路徑" % fork,
          ok and tips[0] == tips0[0] and [_norm(l) for l in tips[0][1:3]] == [SD_A, SD_B],
          "lines=%r 未定義時=%r" % (tips[0], tips0[0]))
    # (4) 對照只屬該節點:下一個節點的同一行不沿用
    h, ok, err, tips = p2b_tooltips(fork, P2B_STAT_DICT,
                                    [[SD_A, SD_B], [SD_A, "10% increased Attack Speed"]])
    got1 = [_norm(l) for l in tips[0][1:3]]
    got2 = [_norm(l) for l in tips[1][1:3]]
    check("%s ST-P2b 第一個節點配對成功(前提)" % fork, got1 == P2B_ZH, "got=%r" % got1)
    check("%s ST-P2b 天賦提示結束後對照清空,下一個節點同一行不沿用" % fork,
          got2 == [SD_A, "增加 10% 攻擊速度"], "got2=%r calls=%r" % (got2, h.stat_calls))


# ---- ST-FR3:不支援詞綴的註記 --------------------------------------------------
#
# 註記不在 node.sd 裡:PassiveTreeView addModInfoToTooltip(PoE1 L1558-1561、PoE2 L1719-1722)
# 在 AddLine 當下才組 colorCodes.UNSUPPORTED .. sd .. main.notSupportedTooltipText,
# 後者 = " ^8(Not supported in PoB yet)"(Main.lua),受 main.notSupportedModTooltips 開關。
# 例:node 2275 Nature's Concoction 兩行 sd,單一多行模板。

FR3_A = "Flasks adjacent to Active Tinctures gain 1 Charge"
FR3_B = "when you Hit an Enemy with a Weapon"
FR3_ZH = ["與作用中酊劑相鄰的藥劑", "在你以武器擊中敵人時獲得 1 充能"]
FR3_STAT_DICT = {FR3_A + "\n" + FR3_B: "\n".join(FR3_ZH)}
FR3_UNSUP = "^xF05050"
FR3_NOTE = " ^8(Not supported in PoB yet)"


def fr3_tooltip(fork, stat_dict, note_on=True, define=True):
    h = stat_harness(fork, stat_dict, define)
    h.run(('colorCodes.UNSUPPORTED = "%s" main.notSupportedModTooltips = %s '
           'main.notSupportedTooltipText = "%s"') % (FR3_UNSUP, "true" if note_on else "false",
                                                   FR3_NOTE))
    view = make(h, "PassiveTreeView")
    tip = make(h, "Tooltip")
    ok, err = h.eval('function(v, t, a, b) local ok, e = pcall(function() v:AddNodeTooltip(t, '
                     '{ dn = "Nature\'s Concoction", id = 2275, unsupported = true, sd = { a, b } }, {}) '
                     'end) return ok, e end')(view, tip, FR3_A, FR3_B)
    return h, ok, err, [l for l in tooltip_lines(h, tip) if l is not None]


def test_stat_multiline_sd_unsupported(fork):
    """ST-FR3 -- 不支援註記的兩行 sd:合併查詢不含註記,命中後註記補回各行尾。"""
    h, ok, err, lines = fr3_tooltip(fork, FR3_STAT_DICT)
    check("%s ST-FR3 天賦提示本身不出錯" % fork, ok, repr(err))
    check("%s ST-FR3 合併查詢用不含註記的兩行 sd" % fork,
          (FR3_A + "\n" + FR3_B, "passive") in [(_norm_ml(l), c) for l, c in h.stat_calls]
          and not any("Not supported" in (l or "") and "\n" in (l or "")
                      for l, c in h.stat_calls), repr(h.stat_calls))
    check("%s ST-FR3 命中後兩行顯示中文且行尾補回註記與原色碼" % fork,
          lines[1:3] == [FR3_UNSUP + z + FR3_NOTE for z in FR3_ZH],
          "lines=%r calls=%r" % (lines, h.stat_calls))
    # 註記開關關閉:同一配對,行尾沒有註記
    h, ok, err, lines = fr3_tooltip(fork, FR3_STAT_DICT, note_on=False)
    check("%s ST-FR3 註記關閉時兩行顯示中文且無註記" % fork,
          lines[1:3] == [FR3_UNSUP + z for z in FR3_ZH], "lines=%r" % (lines,))
    # 合併未命中:與未定義函式時相同(英文 + 註記)
    h, ok, err, lines = fr3_tooltip(fork, {})
    h0, ok0, err0, lines0 = fr3_tooltip(fork, None, define=False)
    check("%s ST-FR3 合併未命中時兩行照現行(保留英文與註記)" % fork,
          ok and lines == lines0 and lines[1:3] == [FR3_UNSUP + s + FR3_NOTE for s in (FR3_A, FR3_B)],
          "lines=%r 未定義時=%r" % (lines, lines0))


def _norm_ml(s):
    """去每行色碼後以 \\n 接回(合併句比對用)。"""
    return "\n".join(_norm(p) for p in (s or "").split("\n"))


# ---- 寶石提示(ST-P1) ---------------------------------------------------------
#
# 上游 Classes/GemTooltip.lua 不是 newClass 類別,是模組表:
#   GemTooltip.AddGemTooltip(tooltip, build, gemInstance, options)(點呼叫、4 參數),
#   PoE1 以 require("Classes.GemTooltip") 取得(package.loaded 快取同一張表),
#   PoE2 以 LoadModule("Classes/GemTooltip") 取得(每個呼叫端各跑一次、各一張表)。
#   GemSelectControl:AddGemTooltip(gemInstance) 再轉呼叫它;一個提示可含多個
#   grantedEffect(PoE1 secondaryGrantedEffect、PoE2 additionalGrantedEffects),各有說明行。
# 合成模組照各分支的取得方式註冊,且在注入「之後」才延遲載入(同 POB)。

GEM_COLOUR_SETUP = b"""
colorCodes.GEM = "^x1AA29B"
colorCodes.GEMDESCRIPTION = "^x7F7F7F"
colorCodes.GEMINFO = "^x7F7F7F"
"""

GEMTOOLTIP_SYN_P1 = """
local GemTooltip = { }
-- 上游 addQualityRangeInfo:describeStats 的行再 gsub 成 (0-N),改過的行不在 lineMap 裡
local function addQualityRange(tooltip, build, grantedEffect)
	if not grantedEffect.qualityStats then return end
	local descriptions = build.data.describeStats(grantedEffect.qualityStats,
		grantedEffect.statDescriptionScope, true)
	for _, line in ipairs(descriptions) do
		line = line:gsub("([%+%-]?%d+%.?%d*)", function(v) return "(0-" .. v .. ")" end, 1)
		tooltip:AddLine(16, colorCodes.MAGIC .. line, "FONTIN SC")
	end
end
local function addCommonGemInfo(tooltip, build, gemInstance, grantedEffect)
	if grantedEffect.description then
		tooltip:AddLine(16, colorCodes.GEM .. grantedEffect.description, "FONTIN SC")
	end
	if build.data.describeStats then
		local descriptions, lineMap = build.data.describeStats(grantedEffect.stats or { },
			grantedEffect.statDescriptionScope)
		for _, line in ipairs(descriptions) do
			tooltip:AddLine(16, colorCodes.MAGIC .. line, "FONTIN SC")
		end
		addQualityRange(tooltip, build, grantedEffect)
	end
end
function GemTooltip.AddGemTooltip(tooltip, build, gemInstance, options)
	options = options or { }
	tooltip.center = true
	tooltip.color = colorCodes.GEM
	local primary = gemInstance.gemData.grantedEffect
	local secondary = gemInstance.gemData.secondaryGrantedEffect
	tooltip:AddLine(20, colorCodes.GEM .. primary.name, "FONTIN SC")
	addCommonGemInfo(tooltip, build, gemInstance, primary)
	if secondary and (not secondary.support or gemInstance.gemData.secondaryEffectName) then
		tooltip:AddLine(20, colorCodes.GEM .. secondary.name, "FONTIN SC")
		addCommonGemInfo(tooltip, build, gemInstance, secondary)
	end
end
return GemTooltip
"""

GEMTOOLTIP_SYN_P2 = """
local GemTooltip = { }
-- 上游 addQualityRangeInfo:describeStats 的行再 gsub 成 (0-N),改過的行不在 lineMap 裡
local function addQualityRange(tooltip, build, grantedEffect)
	if not grantedEffect.qualityStats then return end
	local descriptions = build.data.describeStats(grantedEffect.qualityStats,
		grantedEffect.statDescriptionScope, true)
	for _, line in ipairs(descriptions) do
		line = line:gsub("([%+%-]?%d+%.?%d*)", function(v) return "(0-" .. v .. ")" end, 1)
		tooltip:AddLine(16, colorCodes.MAGIC .. line, "FONTIN SC")
	end
end
local function addDescriptionLine(tooltip, line, index)
	local bg = (index % 2 == 0) and "GemHoverModBg" or nil
	tooltip:AddLine(16, colorCodes.MAGIC .. line, "FONTIN SC", bg)
end
local function addGrantedEffectInfo(tooltip, build, gemInstance, grantedEffect)
	tooltip.center = true
	if grantedEffect.description then
		tooltip:AddLine(16, colorCodes.GEMDESCRIPTION .. grantedEffect.description, "FONTIN ITALIC")
	end
end
local function addEffectStats(tooltip, build, gemInstance, grantedEffect)
	local descriptions, lineMap = build.data.describeStats(grantedEffect.stats or { },
		grantedEffect.statDescriptionScope)
	for i, line in ipairs(descriptions) do
		addDescriptionLine(tooltip, line, i)
	end
	addQualityRange(tooltip, build, grantedEffect)
end
function GemTooltip.AddGemTooltip(tooltip, build, gemInstance, options)
	options = options or { }
	tooltip.center = false
	tooltip.color = colorCodes.GEM
	local grantedEffect = gemInstance.gemData.grantedEffect
	tooltip:AddLine(20, colorCodes.GEM .. gemInstance.gemData.name, "FONTIN SC")
	addGrantedEffectInfo(tooltip, build, gemInstance, grantedEffect)
	addEffectStats(tooltip, build, gemInstance, grantedEffect)
	for _, additional in ipairs(gemInstance.gemData.additionalGrantedEffects or { }) do
		tooltip:AddLine(20, colorCodes.GEM .. additional.name, "FONTIN SC")
		addGrantedEffectInfo(tooltip, build, gemInstance, additional)
		addEffectStats(tooltip, build, gemInstance, additional)
	end
end
return GemTooltip
"""

# 上游 GemSelectControl 檔頂取得 gemTooltip、AddGemTooltip 轉呼叫;Draw 只留建提示那段。
GEMSELECT_TIP_BODY = """
function C:AddGemTooltip(gemInstance)
	gemTooltip.AddGemTooltip(self.tooltip, self.skillsTab.build, gemInstance)
end
function C:Draw(viewPort, noTooltip)
	if self.tipGem then self:AddGemTooltip(self.tipGem) end
end
"""

GEMSELECT_TIP_P1 = ('local gemTooltip = require("Classes.GemTooltip")\n'
                    + GEMSELECT_P1.replace("function C:Draw() end\n", "")
                    + GEMSELECT_TIP_BODY)
GEMSELECT_TIP_P2 = ('local gemTooltip = LoadModule("Classes/GemTooltip")\n'
                    + GEMSELECT_P2.replace("function C:Draw() end\n", "")
                    + GEMSELECT_TIP_BODY)

# 照真 Lua require:先 package.loaded,再依序問 package.loaders(含 preload);
# 只有找不到的名稱才退回 PRELUDE 的假模組。
FAITHFUL_REQUIRE = b"""
do
	local dummy = require
	require = function(name)
		if package.loaded[name] ~= nil then return package.loaded[name] end
		for _, loader in ipairs(package.loaders) do
			local f = loader(name)
			if type(f) == "function" then
				local m = f(name)
				if m == nil then m = true end
				package.loaded[name] = m
				return m
			end
		end
		return dummy(name)
	end
end
"""

GEM_STAT_DICT = {"Deals 10 to 20 Fire Damage": "STAT寶石"}
GEM_DESC_ID = {
    ("gems_desc", "Fireball"): ("Unleashes a ball of fire.", "釋放一顆火球。"),
    ("gems_desc", "FireballSecondary"): ("Secondary blast.", "次要爆炸。"),
}
# 現行路徑(Draw 包在 gems 來源)會查到的值:說明行與 stat 行在 gems 來源的譯文
DICT["gems"].update({
    "Unleashes a ball of fire.": "火球說明(gems)",
    "Deals 10 to 20 Fire Damage": "造成 10-20 火焰傷害(gems)",
})
GEM_EXPECT_EXISTING = ["火球", "火球說明(gems)", "造成 10-20 火焰傷害(gems)",
                       "(0-20)% increased Area of Effect"]

# StatDescriber 的替身,回傳形狀照上游 Modules/StatDescriber.lua:
#   return out, lineMap  -- out = 行陣列;lineMap[行] = 單一 stat id 字串
#   (該描述 stats 清單裡「最後一個有值」的 stat;不是全部 id)
SYN_DESCRIBER = b"""
local SYN_DESC = {
	{ stats = { "spell_minimum_base_fire_damage", "spell_maximum_base_fire_damage" },
	  fmt = function(v) return string.format("Deals %d to %d Fire Damage", v[1], v[2]) end },
	{ stats = { "base_skill_area_of_effect_+%" },
	  fmt = function(v) return string.format("%d%% increased Area of Effect", v[1]) end },
}
SynDescribeStats = function(stats, scopeName, quality)
	local out, lineMap = { }, { }
	for _, d in ipairs(SYN_DESC) do
		local v, stat = { }, nil
		for i, s in ipairs(d.stats) do
			v[i] = stats[s] or 0
			if stats[s] then stat = s end
		end
		if stat then
			local line = d.fmt(v)
			table.insert(out, line)
			lineMap[line] = stat
		end
	end
	return out, lineMap
end
data = data or { }
data.describeStats = SynDescribeStats
"""


# 現在的 data.describeStats 與原替身同輸入比對:回傳個數、out 每行、lineMap 每對
DESCRIBE_SAME = """function()
	local stats = { spell_minimum_base_fire_damage = 3, spell_maximum_base_fire_damage = 7,
		["base_skill_area_of_effect_+%"] = 5 }
	local function pack(...)
		local t = { n = select("#", ...) }
		for i = 1, t.n do t[i] = select(i, ...) end
		return t
	end
	local a = pack(data.describeStats(stats, "gem_stat_descriptions", true))
	local b = pack(SynDescribeStats(stats, "gem_stat_descriptions", true))
	if a.n ~= b.n then return "n " .. a.n .. "~=" .. b.n end
	if #a[1] ~= #b[1] then return "out len" end
	for i = 1, #b[1] do if a[1][i] ~= b[1][i] then return "out " .. i end end
	for k, v in pairs(b[2]) do if a[2][k] ~= v then return "lineMap " .. k end end
	for k, v in pairs(a[2]) do if b[2][k] ~= v then return "lineMap extra " .. k end end
	return "same"
end"""


def gem_instance_src(fork, with_secondary=False):
    sec = ('{ id = "FireballSecondary", name = "Fire Burst", '
           'description = "Secondary blast.", stats = { } }')
    extra = ""
    if with_secondary:
        extra = ("secondaryGrantedEffect = %s," % sec if fork == "PoE1"
                 else "additionalGrantedEffects = { %s }," % sec)
    return """
{
	level = 1,
	gemData = {
		name = "Fireball",
		grantedEffect = { id = "Fireball", name = "Fireball",
			description = "Unleashes a ball of fire.",
			stats = { spell_minimum_base_fire_damage = 10, spell_maximum_base_fire_damage = 20 },
			qualityStats = { ["base_skill_area_of_effect_+%%"] = 20 },
			statDescriptionScope = "gem_stat_descriptions" },
		%s
	},
}
""" % extra


def gem_harness(fork, stat_dict, define_stat=True, id_dict=None, gem_tree=False):
    """寶石提示 harness:假 PobToolsTranslateStat + 嚴格依 (id, en) 的 PobToolsTranslateId。"""
    h = Harness(fork, with_inject=False)
    h.loose_keys = True
    h.stat_calls = []
    h.stat_ids = []          # (line, ctx, 第 3 參數 stat ids)

    def translate_stat(line, ctx=None, ids=None):
        h.stat_calls.append((line, ctx))
        h.stat_ids.append((line, ctx, ids))
        if line is None:
            return None
        m = re.match(r"^((?:\^x[0-9A-Fa-f]{6}|\^\d)*)(.*)$", line, re.S)
        hit = (stat_dict or {}).get(m.group(2).strip())
        return None if hit is None else m.group(1) + hit

    def translate_id(source, id_, en=None):
        if isinstance(id_, float) and id_.is_integer():
            id_ = int(id_)
        key = None if id_ is None else str(id_)
        h.id_lookups.append((source, key, en))
        if not h.enabled:
            return None
        hit = (GEM_DESC_ID if id_dict is None else id_dict).get((source, key))
        return hit[1] if hit and hit[0] == en else None

    g = h.rt.globals()
    g["__py_translate_id"] = translate_id
    if define_stat:
        g["__py_translate_stat"] = translate_stat
        h.run(b"function PobToolsTranslateStat(l, c, i) return __py_translate_stat(l, c, i) end")
    h.run(COLOUR_SETUP)
    h.run(GEM_COLOUR_SETUP)
    h.run(SYN_DESCRIBER)
    h.run('LoadModule("Classes/Tooltip")')
    h.run('LoadModule("Classes/EditControl")')
    if not gem_tree:
        h.define("PassiveTreeView", PASSIVETREE_P1, PASSIVETREE_P2)
    h.define("ItemsTab", ITEMSTAB_P1, ITEMSTAB_P2)
    # 合成檔:GemTooltip 與 GemSelectControl 經 LoadModule / require 供應
    syn = {"Classes/GemTooltip": GEMTOOLTIP_SYN_P1 if fork == "PoE1" else GEMTOOLTIP_SYN_P2,
           "Classes/GemSelectControl": GEMSELECT_TIP_P1 if fork == "PoE1" else GEMSELECT_TIP_P2,
           "Classes/PassiveTreeView": GEM_TREE_P1 if fork == "PoE1" else GEM_TREE_P2}
    real_lm = h._load_module

    def load_module(name, *args):
        key = name[:-4] if name.endswith(".lua") else name
        if key in syn:
            return h._chunk(syn[key].encode("utf-8"), "@" + key)(*args)
        return real_lm(name, *args)

    # 包成真 Lua 函式:注入層只在 type(LoadModule) == "function" 時才掛延遲載入的 patch
    g["__py_load_module"] = load_module
    h.run(b"function LoadModule(...) return __py_load_module(...) end "
          b"PLoadModule = LoadModule")
    h.run(FAITHFUL_REQUIRE)
    g["__py_gemtooltip_chunk"] = lambda: h._chunk(GEMTOOLTIP_SYN_P1.encode("utf-8"),
                                                  "@Classes/GemTooltip")
    h.run(b'package.preload["Classes.GemTooltip"] = function(...) '
          b'return __py_gemtooltip_chunk()(...) end')
    h.apply_inject()
    h.run('LoadModule("Classes/GemSelectControl")')   # 注入後才延遲載入
    if gem_tree:
        h.run('LoadModule("Classes/PassiveTreeView")')
    h.lookups = []
    return h


def gem_tooltip_lines(fork, stat_dict, define_stat=True, id_dict=None,
                      with_secondary=False, h=None):
    """GemSelectControl:Draw 建寶石提示;回 (h, ok, err, 去色碼行)。"""
    h = h or gem_harness(fork, stat_dict, define_stat, id_dict)
    obj = make(h, "GemSelectControl")
    tip = make(h, "Tooltip")
    gem = h.eval("function() return %s end" % gem_instance_src(fork, with_secondary))()
    ok, err = h.eval('function(o, t, g) o.tooltip = t o.skillsTab = { build = { data = data } } '
                     'o.tipGem = g local ok, e = pcall(function() o:Draw({}, false) end) '
                     'return ok, e end')(obj, tip, gem)
    lines = [_norm(l) for l in tooltip_lines(h, tip) if l is not None]
    return h, ok, err, lines


def test_stat_gem_tooltip(fork):
    """ST-P1 -- 寶石提示:說明行依 gems_desc + 效果 id,stat 行以 gem 情境查 stat。"""
    # (1)(2) 命中
    h, ok, err, lines = gem_tooltip_lines(fork, GEM_STAT_DICT)
    check("%s ST-P1 寶石提示本身不出錯" % fork, ok, repr(err))
    check("%s ST-P1 說明行以 PobToolsTranslateId(\"gems_desc\", 效果 id, 英文) 查詢" % fork,
          ("gems_desc", "Fireball", "Unleashes a ball of fire.") in h.id_lookups,
          repr(h.id_lookups))
    check("%s ST-P1 說明行採用依 id 查到的譯文" % fork,
          "釋放一顆火球。" in lines, "lines=%r id_lookups=%r" % (lines, h.id_lookups))
    # 合成寶石的 scope 是 gem_stat_descriptions → ctx "gem:gem_stat_descriptions"
    # (引擎 "gem" 預設鏈先 skill_stat,輔助寶石會選錯檔，見 stat_selftest E5-5)
    check("%s ST-P1 stat 行以 ctx gem:gem_stat_descriptions 呼叫 PobToolsTranslateStat" % fork,
          any(_norm(l) == "Deals 10 to 20 Fire Damage" and c == "gem:gem_stat_descriptions"
              for l, c in h.stat_calls), repr(h.stat_calls))
    check("%s ST-P1 stat 行採用 stat 查到的譯文" % fork,
          "STAT寶石" in lines, "lines=%r calls=%r" % (lines, h.stat_calls))
    check("%s ST-P1 寶石提示結束後來源還原為 nil" % fork,
          h.source is None, "source=%r" % h.source)
    # (6) stat 行第 3 參數帶該行 lineMap 的 stat id(空白分隔清單須含它)
    fire = [ids for l, c, ids in h.stat_ids
            if _norm(l) == "Deals 10 to 20 Fire Damage" and c == "gem:gem_stat_descriptions"]
    check("%s ST-P1 寶石 stat 行第 3 參數含該行 lineMap 的 stat id" % fork,
          fire and all(isinstance(i, str) and "spell_maximum_base_fire_damage" in i.split()
                       for i in fire), "ids=%r stat_ids=%r" % (fire, h.stat_ids))
    # (7) 改寫過、不在 lineMap 的行:以 gem 呼叫、第 3 參數 nil
    q = [(c, ids) for l, c, ids in h.stat_ids
         if _norm(l) == "(0-20)% increased Area of Effect"]
    check("%s ST-P1 無 lineMap 對照的寶石行以 gem 呼叫且第 3 參數為 nil" % fork,
          q and all(c == "gem" and ids is None for c, ids in q), "q=%r" % q)
    # (8) describeStats 包裝後回傳值完全不變(個數、行、lineMap)
    same = h.eval(DESCRIBE_SAME)()
    check("%s ST-P1 data.describeStats 包裝後回傳值完全不變" % fork,
          same == "same", repr(same))
    # (1e) 次要/附加效果的說明行用它自己的 id
    h, ok, err, lines = gem_tooltip_lines(fork, GEM_STAT_DICT, with_secondary=True)
    check("%s ST-P1 次要效果說明行以自己的效果 id 查詢並採用" % fork,
          ok and ("gems_desc", "FireballSecondary", "Secondary blast.") in h.id_lookups
          and "次要爆炸。" in lines,
          "lines=%r id_lookups=%r err=%r" % (lines, h.id_lookups, err))
    # (3) 未命中:與現行完全相同
    h, ok, err, lines = gem_tooltip_lines(fork, {}, id_dict={})
    h0, ok0, err0, lines0 = gem_tooltip_lines(fork, None, define_stat=False, id_dict={})
    check("%s ST-P1 stat 與 id 都未命中時結果同現行(gems 來源)" % fork,
          ok and lines == GEM_EXPECT_EXISTING and lines == lines0,
          "lines=%r 未定義 stat 時=%r err=%r" % (lines, lines0, err))
    check("%s ST-P1 舊引擎無 PobToolsTranslateStat 寶石提示不報錯且同現行" % fork,
          ok0 and lines0 == GEM_EXPECT_EXISTING, "lines=%r err=%r" % (lines0, err0))
    # (5) 寶石情境不外漏:之後的天賦/物品提示不以 gem 呼叫
    h, ok, err, lines = gem_tooltip_lines(fork, GEM_STAT_DICT)
    n = len(h.stat_calls)
    view = make(h, "PassiveTreeView")
    tab = make(h, "ItemsTab")
    t1 = make(h, "Tooltip")
    t2 = make(h, "Tooltip")
    h.eval('function(v, t) v:AddNodeTooltip(t, { dn = "Elemental Overload", id = 1, '
           'sd = { "10% increased Attack Speed" } }, {}) end')(view, t1)
    item = h.eval("function() return %s end" % JEWEL_ITEM)()
    h.eval('function(tab, t, item) tab:AddItemTooltip(t, item) end')(tab, t2, item)
    later = h.stat_calls[n:]
    check("%s ST-P1 寶石提示後天賦/物品提示的行不以 gem 情境呼叫" % fork,
          later and not any(isinstance(c, str) and c.startswith("gem") for l, c in later),
          repr(later))


# ---- ST-RV2:天賦提示內嵌寶石提示 ----------------------------------------------
#
# 上游 PassiveTreeView(PoE1 L1643、PoE2 L1836)在 AddNodeTooltip 期間對會給予技能
# 的節點呼叫 gemTooltip.AddGemTooltip(self.skillTooltip, build, gemInst, options)。
# 合成版:檔頂照分支取得 gemTooltip,說明行後內嵌寶石提示(另一個 tooltip),再接 sdAfter。

GEM_TREE_BODY = """
function C:AddNodeName(tooltip, node, build)
	tooltip:AddLine(24, node.dn, "FONTIN")
end
function C:AddNodeTooltip(tooltip, node, build)
	self:AddNodeName(tooltip, node, build)
	for _, line in ipairs(node.sd or {}) do
		tooltip:AddLine(16, "^7"..line)
	end
	if node.gemInst then
		gemTooltip.AddGemTooltip(node.gemTip, build, node.gemInst, { })
	end
	for _, line in ipairs(node.sdAfter or {}) do
		tooltip:AddLine(16, "^7"..line)
	end
end
"""
GEM_TREE_P1 = ('local gemTooltip = require("Classes.GemTooltip")\n'
               'local C = newClass("PassiveTreeView")\n'
               'function C:PassiveTreeView() return self end\n' + GEM_TREE_BODY)
GEM_TREE_P2 = ('local gemTooltip = LoadModule("Classes/GemTooltip")\n'
               'local C = newClass("PassiveTreeView", function(self) end)\n' + GEM_TREE_BODY)

RV2_PAIR_B = "while you are Channelling"
RV2_STAT_DICT = dict(GEM_STAT_DICT)
# 天賦 sd 的兩行配對:第一行刻意與寶石 stat 行同字,配對表會收進同一個鍵
RV2_STAT_DICT["Deals 10 to 20 Fire Damage\n" + RV2_PAIR_B] = "配對第一行\n配對第二行"


def test_stat_gem_in_node(fork):
    """ST-RV2 -- 天賦提示內嵌寶石提示:寶石行一律走寶石分支,之後的天賦行仍走 passive。"""
    h = gem_harness(fork, RV2_STAT_DICT, gem_tree=True)
    view = make(h, "PassiveTreeView")
    tip = make(h, "Tooltip")
    gtip = make(h, "Tooltip")
    gem = h.eval("function() return %s end" % gem_instance_src(fork))()
    ok, err = h.eval('function(v, t, gt, g, b) local ok, e = pcall(function() '
                     'v:AddNodeTooltip(t, { dn = "Elemental Overload", id = 1, '
                     'sd = { "Deals 10 to 20 Fire Damage", b }, '
                     'sdAfter = { "10% increased Attack Speed" }, gemInst = g, gemTip = gt }, '
                     '{ data = data }) end) return ok, e end')(view, tip, gtip, gem, RV2_PAIR_B)
    node_lines = [_norm(l) for l in tooltip_lines(h, tip) if l is not None]
    gem_lines = [_norm(l) for l in tooltip_lines(h, gtip) if l is not None]
    check("%s ST-RV2 天賦提示內嵌寶石提示本身不出錯" % fork, ok, repr(err))
    check("%s ST-RV2 前提:天賦 sd 兩行配對成功" % fork,
          node_lines[1:3] == ["配對第一行", "配對第二行"], "node=%r" % node_lines)
    check("%s ST-RV2 內嵌寶石說明行以 gems_desc + 效果 id + 英文查詢並採用" % fork,
          ("gems_desc", "Fireball", "Unleashes a ball of fire.") in h.id_lookups
          and "釋放一顆火球。" in gem_lines,
          "gem=%r id_lookups=%r" % (gem_lines, h.id_lookups))
    fire = [(c, ids) for l, c, ids in h.stat_ids
            if _norm(l) == "Deals 10 to 20 Fire Damage" and c is not None]
    check("%s ST-RV2 內嵌寶石 stat 行以 ctx gem:gem_stat_descriptions 與 lineMap stat id 呼叫" % fork,
          any(c == "gem:gem_stat_descriptions" and isinstance(i, str) and "spell_maximum_base_fire_damage" in i.split()
              for c, i in fire), "calls=%r" % fire)
    check("%s ST-RV2 內嵌寶石 stat 行不被天賦配對表取代" % fork,
          "STAT寶石" in gem_lines and "配對第一行" not in gem_lines, "gem=%r" % gem_lines)
    check("%s ST-RV2 內嵌寶石行不以 passive 情境呼叫" % fork,
          not any(c == "passive" and _norm(l) in ("Unleashes a ball of fire.",
                                                  "(0-20)% increased Area of Effect")
                  for l, c, _ in h.stat_ids), repr(h.stat_ids))
    check("%s ST-RV2 寶石提示結束後天賦說明行仍以 passive 呼叫" % fork,
          any(_norm(l) == "10% increased Attack Speed" and c == "passive"
              for l, c, _ in h.stat_ids)
          and node_lines[3:4] == ["增加 10% 攻擊速度"],
          "node=%r calls=%r" % (node_lines, h.stat_ids))
    check("%s ST-RV2 結束後來源還原為 nil" % fork, h.source is None, "source=%r" % h.source)


def test_wrapped_arity_gem(fork):
    """ST-P1(4) -- GemTooltip.AddGemTooltip 包裝後 nparams 與上游相同(4、非 vararg)。"""
    import lupa.luajit21 as luajit21
    h = Harness(fork, with_inject=False, lua_mod=luajit21)
    h.run(ARITY_STUBS)
    h.run(FAITHFUL_REQUIRE)
    path = os.path.join(h.dir, "Classes", "GemTooltip.lua")
    h.rt.globals()["__py_gemtooltip_chunk"] = lambda: h._chunk(h._read(path), "@" + path)
    h.run(b'package.preload["Classes.GemTooltip"] = function(...) '
          b'return __py_gemtooltip_chunk()(...) end')
    h.run('data.describeStats = LoadModule("Modules/StatDescriber")')   # 同 Data.lua
    dshape = h.run('return function() local i = debug.getinfo(data.describeStats, "u") '
                   'return i.nparams, i.isvararg and true or false end')
    dbefore = dshape()
    get = ('return function() return require("Classes.GemTooltip") end' if fork == "PoE1"
           else 'return function() return LoadModule("Classes/GemTooltip") end')
    shape = h.run('return function(m) local i = debug.getinfo(m.AddGemTooltip, "u") '
                  'return i.nparams, i.isvararg and true or false end')
    before = shape(h.run(get)())
    h.apply_inject()
    h.run('LoadModule("Classes/GemSelectControl")')
    after = shape(h.run(get)())
    dafter = dshape()
    check("%s ST-P1 注入後 data.describeStats 參數數不變(上游 3 參數非 vararg)" % fork,
          tuple(dbefore) == (3, False) and tuple(dafter) == tuple(dbefore),
          "before=%r after=%r" % (dbefore, dafter))
    check("%s ST-P1 上游 GemTooltip.AddGemTooltip 形狀為 4 參數非 vararg" % fork,
          tuple(before) == (4, False), "before=%r" % (before,))
    check("%s ST-P1 注入後 GemTooltip.AddGemTooltip 參數數不變" % fork,
          tuple(after) == tuple(before), "before=%r after=%r" % (before, after))


# ---- upstream shape checks (catch a rename before a user does) -------------

# ---- 包裝後參數數不變(新介面相容性閘門) -------------------------------------
#
# bridge.lua 的 probe 以 debug.getinfo(fn,"u").nparams 驗 POB 上游形狀(例:
# AddNodeTooltip 要 4 或 5、BUILD.Init 要 6)。inject 的包裝若寫成
# function(self, ...),nparams 變 1,閘門 ok=false,新介面退回經典視窗。
# Lua 5.1 沒有 nparams,所以這組測試跑在 LuaJIT(與 POB 相同)。

ARITY_CLASSES = ("EditControl", "Tooltip", "SearchHost", "SkillListControl",
                 "GemSelectControl", "PassiveTreeView", "NotableDBControl",
                 "ItemDBControl", "MinionSearchListControl", "CalcsTab",
                 "ItemsTab", "Item", "TreeTab", "PassiveMasteryControl")

# 上游頂層會碰到 itemLib / data;這裡只要檔案載得完、方法定義得出來。
ARITY_STUBS = rb"""
local function deep()
	return setmetatable({}, { __index = function(t, k) local v = deep() rawset(t, k, v) return v end,
	                          __call = function() return deep() end })
end
itemLib = itemLib or deep()
data = data or deep()
main.DrawBackground = function(self, viewPort) end
main.modes = {
	BUILD = { Init = function(self, a, b, c, d, e) end, OnFrame = function(self, inputEvents) end },
	LIST = { OnFrame = function(self, inputEvents) end },
}
function PobToolsBackground() return "", 50, 0, 100 end
function PobToolsPerfEnabled() return true end
function PobToolsPerfNow() return 0 end
function PobToolsPerfMark() end
function PobToolsPerfEvent() end
"""

# 記下每個函式(類別方法、main 與 modes 的方法、PobToolsItemTitle)的原物件與形狀。
ARITY_SNAPSHOT = """
return function()
	local snap = {}
	local function rec(key, fn)
		if type(fn) ~= "function" then return end
		local i = debug.getinfo(fn, "u")
		snap[key] = { fn = fn, np = i.nparams, va = i.isvararg and true or false }
	end
	for cname, cls in pairs(common.classes) do
		for k, v in pairs(cls) do rec(cname .. "." .. tostring(k), v) end
	end
	rec("main.DrawBackground", main.DrawBackground)
	for mode, m in pairs(main.modes) do
		for k, v in pairs(m) do rec("main.modes." .. mode .. "." .. tostring(k), v) end
	end
	rec("PobToolsItemTitle", PobToolsItemTitle)
	return snap
end
"""

# 依快照比對:被換掉的函式 -> {key: (原 np, 原 va, 新 np, 新 va)}
ARITY_DIFF = """
return function(snap)
	local out = {}
	local function cur(key)
		local cname, k = key:match("^([^.]+)%.(.+)$")
		if key == "PobToolsItemTitle" then return PobToolsItemTitle end
		if key == "main.DrawBackground" then return main.DrawBackground end
		local mode, mk = key:match("^main%.modes%.([^.]+)%.(.+)$")
		if mode then return main.modes[mode][mk] end
		return common.classes[cname][k]
	end
	for key, s in pairs(snap) do
		local fn = cur(key)
		if fn ~= s.fn and type(fn) == "function" then
			local i = debug.getinfo(fn, "u")
			out[key] = { s.np, s.va, i.nparams, i.isvararg and true or false }
		end
	end
	return out
end
"""

# 至少這些必須被包裝且形狀不變(bridge probe 實際檢查的 + 任務點名的)。
# AddNodeTooltip 不列:外層不再套 passives 來源,可以不包;有包的話下方迴圈照樣驗形狀。
ARITY_MUST = ("PassiveTreeView.AddNodeName",
              "ItemsTab.AddItemTooltip", "ItemDBControl.GetRowValue",
              "Tooltip.AddLine", "main.modes.BUILD.Init")


def test_wrapped_arity(fork):
    import lupa.luajit21 as luajit21
    h = Harness(fork, with_inject=False, lua_mod=luajit21)
    h.run(ARITY_STUBS)
    for c in ARITY_CLASSES:
        h.run('LoadModule("Classes/%s")' % c)
    snap = h.run(ARITY_SNAPSHOT)()
    h.apply_inject()
    diff = h.run(ARITY_DIFF)(snap)
    got = {k: tuple(v[i] for i in range(1, 5)) for k, v in diff.items()}
    for must in ARITY_MUST:
        check("%s 參數數:%s 有被包裝" % (fork, must), must in got, repr(sorted(got)))
    for key in sorted(got):
        np0, va0, np1, va1 = got[key]
        check("%s 參數數:%s 包裝後 nparams/isvararg 不變" % (fork, key),
              (np0, va0) == (np1, va1),
              "原 %s%s → 包裝後 %s%s" % (np0, "+..." if va0 else "", np1, "+..." if va1 else ""))


# 建構子裡才包的 tooltipFunc / selFunc(實例欄位):用合成 ItemsTab。
ITEMSTAB_CTOR_BODY = """
	self.controls = {
		displayItemAffix1 = { tooltipFunc = function(tooltip, mode, index, value) end },
		displayItemAffixRange1 = { tooltipFunc = function(tooltip, val) end },
		displayItemRune1 = { selFunc = function(index, value) end },
	}
	self._orig = {
		affix = self.controls.displayItemAffix1.tooltipFunc,
		range = self.controls.displayItemAffixRange1.tooltipFunc,
		rune = self.controls.displayItemRune1.selFunc,
	}
"""
ITEMSTAB_CTOR_P1 = ('local C = newClass("ItemsTab")\nfunction C:ItemsTab()\n'
                    + ITEMSTAB_CTOR_BODY + '\treturn self\nend\n')
ITEMSTAB_CTOR_P2 = ('local C = newClass("ItemsTab", function(self)\n'
                    + ITEMSTAB_CTOR_BODY + 'end)\n')


def test_wrapped_arity_instance(fork):
    import lupa.luajit21 as luajit21
    h = Harness(fork, with_inject=False, lua_mod=luajit21)
    h.define("ItemsTab", ITEMSTAB_CTOR_P1, ITEMSTAB_CTOR_P2)
    h.apply_inject()
    tab = make(h, "ItemsTab")
    rows = h.eval("""function(t)
		local function sh(fn) local i = debug.getinfo(fn, "u") return i.nparams .. (i.isvararg and "+" or "") end
		local c = t.controls
		return {
			{ "displayItemAffix1.tooltipFunc", c.displayItemAffix1.tooltipFunc ~= t._orig.affix, sh(t._orig.affix), sh(c.displayItemAffix1.tooltipFunc) },
			{ "displayItemAffixRange1.tooltipFunc", c.displayItemAffixRange1.tooltipFunc ~= t._orig.range, sh(t._orig.range), sh(c.displayItemAffixRange1.tooltipFunc) },
			{ "displayItemRune1.selFunc", c.displayItemRune1.selFunc ~= t._orig.rune, sh(t._orig.rune), sh(c.displayItemRune1.selFunc) },
		}
	end""")(tab)
    for i in range(1, 4):
        name, wrapped, before, after = (rows[i][j] for j in range(1, 5))
        check("%s 參數數:ItemsTab %s 有被包裝" % (fork, name), wrapped)
        check("%s 參數數:ItemsTab %s 包裝後 nparams/isvararg 不變" % (fork, name),
              before == after, "原 %s → 包裝後 %s" % (before, after))


def read_text(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


UPSTREAM_SHAPE = {
    "Classes/GemSelectControl.lua": [
        (r'"\^ %a\':-"', "gem box still ships an ASCII-only input filter"),
        (r"function GemSelectClass:BuildList", "BuildList"),
        (r"function GemSelectClass:FilterSupport", "FilterSupport"),
        (r"function GemSelectClass:SortGemList", "SortGemList"),
    ],
    "Classes/ItemDBControl.lua": [
        (r"function ItemDBClass:DoesItemMatchFilters", "DoesItemMatchFilters"),
        (r"self\.controls\.searchMode", "searchMode control"),
        (r"return colorCodes\[item\.rarity\] \.\. item\.name", "GetRowValue row text"),
    ],
    "Classes/MinionSearchListControl.lua": [
        (r"function MinionSearchListClass:DoesEntryMatchFilters", "DoesEntryMatchFilters"),
        (r"self\.controls\.searchText", "searchText control"),
    ],
    "Classes/PassiveTreeView.lua": [
        (r"function PassiveTreeViewClass:AddNodeName\(tooltip, node, build", "AddNodeName"),
        (r"function PassiveTreeViewClass:AddNodeTooltip\(tooltip, node, build", "AddNodeTooltip"),
    ],
    "Classes/ItemsTab.lua": [
        (r"function ItemsTabClass:AddItemTooltip\(tooltip, item", "AddItemTooltip"),
        (r"rarityCode\.\.item\.namePrefix\.\.item\.baseName", "prefix..base..suffix title line"),
    ],
    "Classes/CalcsTab.lua": [
        (r"function CalcsTabClass:SearchMatch", "SearchMatch"),
    ],
    "Classes/NotableDBControl.lua": [
        (r"function NotableDBClass:DoesNotableMatchFilters", "DoesNotableMatchFilters"),
    ],
    # 專精彈窗:選項文字是 stat 行以 " / " 串接(引擎「/」查表的 passive 語境靠這兩個方法包住)
    "Classes/TreeTab.lua": [
        (r"function TreeTabClass:OpenMasteryPopup\(", "OpenMasteryPopup"),
        (r't_concat\(effect\.stats, " / "\)', 'mastery row label = stats joined by " / "'),
    ],
    "Classes/PassiveMasteryControl.lua": [
        (r"function PassiveMasteryControlClass:Draw\(", "Draw"),
    ],
    "Classes/EditControl.lua": [
        (r"function EditClass:OnKeyDown", "OnKeyDown"),
        (r'text:gsub\("\[\\128-\\255\]",\s*"\?"\)', "the '?' line the paste patch drops"),
    ],
}


def test_upstream_shape(fork):
    base = FORKS[fork]
    for rel, checks in UPSTREAM_SHAPE.items():
        path = os.path.join(base, rel.replace("/", os.sep))
        if not os.path.exists(path):
            check("%s shape: %s exists" % (fork, rel), False, "missing")
            continue
        src = read_text(path)
        for pattern, what in checks:
            check("%s shape: %s -- %s" % (fork, rel, what),
                  re.search(pattern, src) is not None)


# ---- C++ source-patch anchors ---------------------------------------------

def parse_source_patches(cpp):
    """Pull (what, file, anchor, insert_after, replace_with, one_fork_only) out of
    kPobToolsSourcePatches[].

    replace_with is None for insert-mode patches. Adjacent C string literals are
    concatenated by the compiler, so the payload is whatever literals follow the
    first three.

    Replace mode is decided by WHERE nullptr sits, not by whether the entry
    contains the word: since one-fork-only entries end in `nullptr, true`, a
    substring test would call every one of them a replace patch and then compare
    the wrong field against the shipped POB source.
    """
    body = cpp.split("kPobToolsSourcePatches[] = {", 1)[1]
    body = body.split("\n};", 1)[0]
    body = re.sub(r"//[^\n]*", "", body)               # comments hold example code
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)  # /*oneForkOnly=*/ markers
    entries = []
    depth = 0
    cur = ""
    for ch in body:
        if ch == "{":
            depth += 1
            if depth == 1:
                cur = ""
                continue
        elif ch == "}":
            depth -= 1
            if depth == 0:
                entries.append(cur)
                continue
        if depth >= 1:
            cur += ch
    out = []
    for e in entries:
        lits = list(re.finditer(r'"((?:[^"\\]|\\.)*)"', e))
        if len(lits) < 3:
            continue
        fields = [unescape_c(m.group(1)) for m in lits]
        what, leaf, anchor = fields[0], fields[1], fields[2]
        payload = "".join(fields[3:])
        # Everything between the anchor literal and the next one. `nullptr` there
        # means the insertAfter slot was skipped, i.e. this is a replace patch.
        gap = e[lits[2].end():lits[3].start()] if len(lits) > 3 else e[lits[2].end():]
        is_replace = "nullptr" in gap
        one_fork = re.search(r"\btrue\b", e[lits[-1].end():]) is not None
        if is_replace:
            out.append((what, leaf, anchor, None, payload, one_fork))
        else:
            out.append((what, leaf, anchor, payload, None, one_fork))
    return out


def apply_source_patches(src, leaf, patches):
    """Mirror of pobtools_loadfile_patched() in ui_api.cpp."""
    applied = 0
    for _what, p_leaf, anchor, insert_after, replace_with, _one_fork in patches:
        if p_leaf != leaf:
            continue
        at = src.find(anchor)
        if at < 0:
            continue                                   # anchor drifted: skipped
        if replace_with is not None:
            applied += src.count(anchor)
            src = src.replace(anchor, replace_with)
        else:
            if src.find(anchor, at + 1) >= 0:
                continue                               # ambiguous: insert mode refuses
            end = at + len(anchor)
            src = src[:end] + insert_after + src[end:]
            applied += 1
    return src, applied


def unescape_c(s):
    return (s.replace("\\\\", "\x00").replace('\\"', '"').replace("\\n", "\n")
             .replace("\\t", "\t").replace("\x00", "\\"))


def test_source_patch_anchors():
    patches = parse_source_patches(read_text(UI_API))
    check("source patches: table parsed", len(patches) >= 3, "n=%d" % len(patches))
    for what, leaf, anchor, insert_after, replace_with, one_fork in patches:
        is_replace = replace_with is not None
        total = 0
        for fork, base in FORKS.items():
            path = None
            for root, _dirs, files in os.walk(base):
                for f in files:
                    if f.lower() == leaf:
                        path = os.path.join(root, f)
                        break
                if path:
                    break
            if not path:
                continue
            n = read_text(path).count(anchor)
            total += n
            if not is_replace:
                # insert mode refuses to apply when the anchor is ambiguous
                check("anchor unique in %s/%s: %r" % (fork, leaf, anchor[:48]), n <= 1,
                      "found %d" % n)
        check("anchor matches somewhere: %s %r" % (leaf, anchor[:48]), total >= 1,
              "found 0 in both forks")
        # oneForkOnly decides whether a miss is reported to the user at runtime
        # (ui_api.cpp logs "[inject] <what> ... anchor drifted"). Flag it wrongly
        # and the log either cries wolf on every launch of one fork, or stays
        # silent about the exact regression it exists to catch -- so the flag is
        # checked against the shipped sources rather than trusted.
        forks_hit = 0
        for fork, base in FORKS.items():
            for root, _dirs, files in os.walk(base):
                if any(f.lower() == leaf for f in files):
                    fp = os.path.join(root, next(f for f in files if f.lower() == leaf))
                    if anchor in read_text(fp):
                        forks_hit += 1
                    break
        if one_fork:
            check("oneForkOnly is true because it really is one fork: %s" % what,
                  forks_hit == 1, "matched in %d fork(s)" % forks_hit)
        else:
            check("a patch not marked oneForkOnly matches both forks: %s" % what,
                  forks_hit == len(FORKS), "matched in %d of %d" % (forks_hit, len(FORKS)))


def test_source_patch_compiles():
    """The patched text must still be valid Lua.

    Anchors are matched blind, so a payload with an unbalanced quote or a stray
    `end` would only surface as a POB that refuses to open a tab. Compiling the
    real file, patched exactly the way the engine patches it, is the cheap way
    to find that here instead.
    """
    patches = parse_source_patches(read_text(UI_API))
    leaves = sorted(set(p[1] for p in patches))   # p[1] is the file leaf
    for fork, base in FORKS.items():
        rt = lua51.LuaRuntime(unpack_returned_tuples=True)
        # always two values: one return value would not arrive as a tuple
        loadstring = rt.eval("function(s, n) local f, e = loadstring(s, n) "
                             "if f then return true, '' end return false, tostring(e) end")
        for leaf in leaves:
            path = None
            for root, _dirs, files in os.walk(base):
                for f in files:
                    if f.lower() == leaf:
                        path = os.path.join(root, f)
                        break
                if path:
                    break
            if not path:
                continue
            with open(path, "rb") as fh:
                raw = fh.read()
            if raw[:3] == b"\xef\xbb\xbf":          # BOM: loadbuffer chokes
                raw = raw[3:]
            src = raw.decode("latin-1")
            patched, applied = apply_source_patches(src, leaf, patches)
            check("%s %s: at least one patch applied" % (fork, leaf), applied >= 1,
                  "applied=%d" % applied)
            # Control first. POB runs on its own Lua build, and the PoE2 fork's
            # ItemsTab.lua uses a `continue` keyword stock Lua 5.1 rejects. That
            # is not something a patch broke, so the comparison is patched-vs-
            # unpatched, never patched-vs-"must compile here".
            base_ok, _ = loadstring(src.encode("latin-1"), "@" + leaf)
            ok, err = loadstring(patched.encode("latin-1"), "@" + leaf)
            if not base_ok:
                check("%s %s: patch does not change compilability" % (fork, leaf),
                      not ok, "unpatched rejected by stock Lua 5.1, patched accepted?")
            else:
                check("%s %s: patched source compiles" % (fork, leaf), ok, str(err))


def test_source_patch_reaches_required_modules():
    """require() 載入的被 patch 檔也要經過 patch 載入器。

    LoadModule 走 pobtools_loadfile_patched,但 Lua 原生 require 不走:上游把
    fuzzyScore 搬進 require("Modules.ConfigModBrowser") 後,錨點就算對得上也
    永遠不會套用。被 require 的檔必須有 ui_api.cpp 註冊的 searcher 接手。
    """
    cpp = read_text(UI_API)
    has_searcher = re.search(r"pobtools_require_searcher", cpp) is not None \
        and re.search(r'"loaders"', cpp) is not None
    leaves = sorted(set(p[1] for p in parse_source_patches(cpp)))
    for leaf in leaves:
        stem = os.path.splitext(leaf)[0]
        req = re.compile(r"require\s*\(?\s*[\"'](?:[\w.]+\.)?%s[\"']" % re.escape(stem),
                         re.IGNORECASE)
        users = []
        for fork, base in FORKS.items():
            for root, _dirs, files in os.walk(base):
                for f in files:
                    if f.lower().endswith(".lua") and req.search(read_text(os.path.join(root, f))):
                        users.append("%s/%s" % (fork, f))
        if users:
            check("require 載入的 %s 也套得到 source patch" % leaf, has_searcher,
                  "被 %s require,但 ui_api.cpp 沒有 require searcher" % ", ".join(users[:3]))


# ---- V2:寶石說明行 id 有命中但英文不符時，退回 gems 來源 ---------------------
def test_gem_desc_id_mismatch_falls_back_to_gems(fork):
    """V2 -- 說明行 id 查不到(英文不符)時，退回查詢須在 gems 來源，不得落到全域表。"""
    desc = "Unleashes a ball of fire."
    # id 在 gems_desc 有條目，但英文與畫面不同:引擎拒答(嚴格 translate_id 回空)
    mismatch = {("gems_desc", "Fireball"): ("A different English description.", "不該採用")}
    DICT[None][desc] = "火球說明(全域)"     # 全域合併表與 gems 來源給不同結果
    try:
        h = gem_harness(fork, RV2_STAT_DICT, gem_tree=True, id_dict=mismatch)
        view = make(h, "PassiveTreeView")
        tip = make(h, "Tooltip")
        gtip = make(h, "Tooltip")
        gem = h.eval("function() return %s end" % gem_instance_src(fork))()
        ok, err = h.eval('function(v, t, gt, g) local ok, e = pcall(function() '
                         'v:AddNodeTooltip(t, { dn = "Elemental Overload", id = 1, sd = { }, '
                         'gemInst = g, gemTip = gt }, { data = data }) end) return ok, e end'
                         )(view, tip, gtip, gem)
        gem_lines = [_norm(l) for l in tooltip_lines(h, gtip) if l is not None]
        srcs = [src for t, src in h.lookups if _norm(t) == desc]
    finally:
        del DICT[None][desc]
    check("%s V2 天賦內嵌寶石提示本身不出錯" % fork, ok, repr(err))
    check("%s V2 前提：說明行先以 gems_desc + 效果 id + 英文查詢" % fork,
          ("gems_desc", "Fireball", desc) in h.id_lookups, repr(h.id_lookups))
    check("%s V2 id 英文不符時退回查詢帶 source=\"gems\"" % fork,
          bool(srcs) and all(s == "gems" for s in srcs), "lookups(src)=%r" % srcs)
    check("%s V2 id 英文不符時採用 gems 來源譯文，不用全域表" % fork,
          "火球說明(gems)" in gem_lines and "火球說明(全域)" not in gem_lines,
          "gem=%r" % gem_lines)
    check("%s V2 結束後來源還原為 nil" % fork, h.source is None, "source=%r" % h.source)


# ---- ST-E5:寶石 stat 行的 ctx 帶該技能的 statDescriptionScope ------------------
# 上游取 scope 的位置(都當 describeStats 第 2 參數傳入):
#   PoE1 Classes/GemTooltip.lua:180  describeStats(stats, grantedEffect.statDescriptionScope)
#   PoE2 Classes/GemTooltip.lua:240  describeStats(stats, statSet.statDescriptionScope)
#        (PoE2 的 scope 在 grantedEffect.statSets[i],grantedEffect 本身沒有)
# 引擎 ctx "gem" 只走預設四檔鏈;光環/召喚物等要 "gem:<scope>"。
GEMTOOLTIP_SYN_P2_STATSET = GEMTOOLTIP_SYN_P2.replace(
    """local function addEffectStats(tooltip, build, gemInstance, grantedEffect)
	local descriptions, lineMap = build.data.describeStats(grantedEffect.stats or { },
		grantedEffect.statDescriptionScope)""",
    """local function addEffectStats(tooltip, build, gemInstance, grantedEffect)
	local statSet = (grantedEffect.statSets or { })[1] or { }
	local descriptions, lineMap = build.data.describeStats(grantedEffect.stats or { },
		statSet.statDescriptionScope)""")


def e5_gem_src(fork, scope):
    sc = "nil" if scope is None else '"%s"' % scope
    if fork == "PoE1":
        where = "statDescriptionScope = %s" % sc
    else:
        where = "statSets = { { statDescriptionScope = %s } }" % sc
    return """
{
	level = 1,
	gemData = {
		name = "Fireball",
		grantedEffect = { id = "Fireball", name = "Fireball",
			description = "Unleashes a ball of fire.",
			stats = { spell_minimum_base_fire_damage = 10, spell_maximum_base_fire_damage = 20 },
			%s },
	},
}
""" % where


def test_stat_gem_scope_ctx(fork):
    """ST-E5 -- 寶石 stat 行以 "gem:" .. statDescriptionScope 查 stat;拿不到 scope 時用 "gem"。"""
    global GEMTOOLTIP_SYN_P2
    saved = GEMTOOLTIP_SYN_P2
    assert GEMTOOLTIP_SYN_P2_STATSET != saved, "PoE2 statSet 版替身沒有替換成功"
    GEMTOOLTIP_SYN_P2 = GEMTOOLTIP_SYN_P2_STATSET
    try:
        res = {}
        for label, scope in (("aura", "aura_skill_stat_descriptions"),
                             ("gemstat", "gem_stat_descriptions"), ("none", None)):
            h = gem_harness(fork, GEM_STAT_DICT)
            obj = make(h, "GemSelectControl")
            tip = make(h, "Tooltip")
            gem = h.eval("function() return %s end" % e5_gem_src(fork, scope))()
            ok, err = h.eval('function(o, t, g) o.tooltip = t o.skillsTab = { build = { data = data } } '
                             'o.tipGem = g local ok, e = pcall(function() o:Draw({}, false) end) '
                             'return ok, e end')(obj, tip, gem)
            ctxs = [c for l, c, _ in h.stat_ids if _norm(l) == "Deals 10 to 20 Fire Damage"]
            res[label] = (ok, err, ctxs)
    finally:
        GEMTOOLTIP_SYN_P2 = saved
    ok, err, ctxs = res["aura"]
    check("%s ST-E5 寶石提示本身不出錯(有 scope)" % fork, ok, repr(err))
    check("%s ST-E5 stat 行 ctx 為 \"gem:aura_skill_stat_descriptions\"" % fork,
          bool(ctxs) and all(c == "gem:aura_skill_stat_descriptions" for c in ctxs),
          "ctx=%r" % ctxs)
    ok, err, ctxs = res["gemstat"]
    check("%s ST-E5 寶石提示本身不出錯(gem_stat scope)" % fork, ok, repr(err))
    check("%s ST-E5 scope 為 gem_stat_descriptions 時 ctx 為 \"gem:gem_stat_descriptions\"" % fork,
          bool(ctxs) and all(c == "gem:gem_stat_descriptions" for c in ctxs),
          "ctx=%r" % ctxs)
    ok, err, ctxs = res["none"]
    check("%s ST-E5 寶石提示本身不出錯(無 scope)" % fork, ok, repr(err))
    check("%s ST-E5 拿不到 scope 時 ctx 為 \"gem\"" % fork,
          bool(ctxs) and all(c == "gem" for c in ctxs), "ctx=%r" % ctxs)


# ---- code review 806975a:RB1 / RI1 / RI2 / RI3 ------------------------------
#
# 假引擎照真引擎:PobToolsTranslateId(source, id, en) 英文相符回 id 譯文;不符時
# 退回該來源依 en 查(passives 依 dn)。
RB_ID = {
    ("passives", "36949"): ("Devotion", "奉獻"),
    ("passives", "59423"): ("Escalation", "進階"),
    ("passives", "90001"): ("Elemental Overload", "戰神"),
}


def rb_harness(fork):
    h = tooltip_harness(fork, with_inject=True)
    g = h.rt.globals()
    plain = g["__py_translate"]

    def translate_id(source, id_, en=None):
        if isinstance(id_, float) and id_.is_integer():
            id_ = int(id_)
        key = None if id_ is None else str(id_)
        h.id_lookups.append((source, key, en))
        if not h.enabled:
            return None
        hit = RB_ID.get((source, key))
        if hit and hit[0] == en:
            return hit[1]
        if en is None:
            return None
        prev = h.source
        h.source = source
        try:
            return plain(en)
        finally:
            h.source = prev

    g["__py_translate_id"] = translate_id
    return h


def _with_dict(adds, fn):
    """暫時加入 DICT 條目，結束時移除(不影響其他測試)。"""
    undo = []
    for src, k, v in adds:
        t = DICT.setdefault(src, {})
        undo.append((t, k, t.get(k, None), k in t))
        t[k] = v
    try:
        return fn()
    finally:
        for t, k, old, had in reversed(undo):
            if had:
                t[k] = old
            else:
                del t[k]


RB_ADDS = [
    ("passives", "Devotion", "奉獻"), ("legion", "Devotion", "還願"),
    ("passives", "Escalation", "進階"), (None, "Escalation", "惡化"),
]


def test_rb1_conquered_unrenamed(fork):
    """RB1 -- 被征服(conqueredBy)但沒改名的節點，名稱仍用 id 譯名;改名的才用 legion。"""
    conq = 'conqueredBy = { id = 1234, conqueror = { type = "karui", id = 1 } }'

    def run():
        out = {}
        for label, fields in (
                ("devotion", 'id = 36949, dn = "Devotion", ' + conq),
                ("escalation", 'id = 59423, dn = "Escalation", ' + conq),
                ("renamed", 'id = 36949, dn = "Bleed Avoidance", ' + conq)):
            h = rb_harness(fork)
            out[label] = (e3_name(h, fields), h)
        return out

    out = _with_dict(RB_ADDS, run)
    got, h = out["devotion"]
    check("%s RB1 被征服未改名(Lethal Pride 範圍內 Devotion)顯示 id 譯名「奉獻」" % fork,
          got == "奉獻", "got=%r lookups=%r id=%r" % (got, h.lookups, h.id_lookups))
    got, h = out["escalation"]
    check("%s RB1 被征服未改名且 legion 無此名(Escalation)顯示「進階」不落全域" % fork,
          got == "進階", "got=%r lookups=%r" % (got, h.lookups))
    got, h = out["renamed"]
    check("%s RB1 對照：被 ReplaceNode 改名(dn 換成 legion 名)顯示 legion 譯名" % fork,
          got == "流血避免", "got=%r lookups=%r" % (got, h.lookups))
    check("%s RB1 結束後來源還原為 nil" % fork, h.source is None, "source=%r" % h.source)


def test_ri1_tattoo_and_conquered(fork):
    """RI1 -- 同時 isTattoo 與 conqueredBy:ReplaceNode 會覆寫 isTattoo,名稱來自刺青 → 查 tattoo。"""
    conq = 'conqueredBy = { id = 1234, conqueror = { type = "vaal", id = 1 } }'
    h = rb_harness(fork)
    got = e3_name(h, 'id = 90001, dn = "Tattoo of the Kitava Makanga", isTattoo = true, ' + conq)
    check("%s RI1 刺青且被征服的節點名稱查 tattoo 來源" % fork,
          got == "奇塔弗鑄造的紋身" and "tattoo" in srcs_of(h, "Tattoo of the Kitava Makanga"),
          "got=%r lookups=%r" % (got, h.lookups))


RI2_DESC = "Summons a ballista totem that fires arrows.\nRequires a Bow."


def ri2_gem_src():
    return """
{
	level = 1,
	gemData = {
		name = "Fireball",
		grantedEffect = { id = "Fireball", name = "Fireball",
			description = %s,
			stats = { spell_minimum_base_fire_damage = 10, spell_maximum_base_fire_damage = 20 },
			statDescriptionScope = "gem_stat_descriptions" },
	},
}
""" % ('"' + RI2_DESC.replace("\n", "\\n") + '"')


def test_ri2_gem_desc_per_line(fork):
    """RI2 -- 說明行 id 未命中、gems 整句也查不到時，逐行在 gems 來源查;不落全域。"""
    mismatch = {("gems_desc", "Fireball"): ("A different English description.", "不該採用")}
    # 整句在 gems 與全域都查不到(真引擎的來源查詢查不到時會再退全域合併表,所以
    # 整句若在全域有值，回全域值是正當的 —— 這裡讓整句兩邊都沒有，才是 L316 的情境)。
    # 單行 "Requires a Bow." 在 gems 與全域譯法不同：逐行須在 gems 來源查(同 V2)。
    adds = [("gems", "Requires a Bow.", "需要弓。"), (None, "Requires a Bow.", "需要弓(全域)")]

    def run():
        h = gem_harness(fork, GEM_STAT_DICT, gem_tree=True, id_dict=mismatch)
        view = make(h, "PassiveTreeView")
        tip = make(h, "Tooltip")
        gtip = make(h, "Tooltip")
        gem = h.eval("function() return %s end" % ri2_gem_src())()
        ok, err = h.eval('function(v, t, gt, g) local ok, e = pcall(function() '
                         'v:AddNodeTooltip(t, { dn = "Elemental Overload", id = 1, sd = { }, '
                         'gemInst = g, gemTip = gt }, { data = data }) end) return ok, e end'
                         )(view, tip, gtip, gem)
        lines = [l for l in tooltip_lines(h, gtip) if l is not None]
        return h, ok, err, lines

    h, ok, err, lines = _with_dict(adds, run)
    # Tooltip 會把含 "\n" 的一行拆成多行：先接回整段再逐行看
    desc = [_norm_ml(l) for l in lines if "ballista" in l or "Bow" in l or "需要弓" in l]
    whole = "\n".join(desc)
    parts = whole.split("\n") if desc else []
    check("%s RI2 天賦內嵌寶石提示本身不出錯" % fork, ok, repr(err))
    check("%s RI2 說明行不是整句原樣英文(逐行退路有執行)" % fork,
          bool(desc) and whole != RI2_DESC, "desc=%r lines=%r" % (desc, lines))
    check("%s RI2 說明第二行逐行在 gems 來源查得「需要弓。」(不是全域的「需要弓(全域)」)" % fork,
          len(parts) == 2 and parts[1] == "需要弓。",
          "parts=%r lookups=%r" % (parts, h.lookups))
    srcs = [src for t, src in h.lookups if _norm(t) == "Requires a Bow."]
    check("%s RI2 逐行查詢帶 source=\"gems\"(同 V2)" % fork,
          bool(srcs) and all(x == "gems" for x in srcs), "srcs=%r" % srcs)


# RI3:上游 AddItemTooltip(PoE1 ItemsTab.lua L4472-4483、PoE2 L3637-3640 起)只畫
# item:GetModLineVariantCount(modLine) > 0 的行;未選中 variant 的行不畫，配對也不得算它。
RI3_ITEMTIP = """
function C:AddItemTooltip(tooltip, item, slot, dbMode, maxWidth)
	tooltip:AddLine(20, colorCodes[item.rarity]..item.title, "FONTIN SC")
	for _, modList in ipairs{item.enchantModLines, item.implicitModLines, item.explicitModLines} do
		if modList[1] then
			for _, modLine in ipairs(modList) do
				local variantCount = item:GetModLineVariantCount(modLine)
				if variantCount > 0 then
					local formattedModLine = itemLib.formatModLine(modLine, dbMode)
					if formattedModLine then
						for _ = 1, variantCount do
							tooltip:AddLine(16, formattedModLine, "FONTIN SC", MODLINE_ARG)
						end
					end
				end
			end
			tooltip:AddSeparator(10)
		end
	end
end
"""
RI3_ITEMSTAB = (
    'local C = newClass("ItemsTab")\nfunction C:ItemsTab() return self end\n'
    + RI3_ITEMTIP.replace("MODLINE_ARG", "modLine"),
    'local C = newClass("ItemsTab", function(self) end)\n'
    + RI3_ITEMTIP.replace("MODLINE_ARG", "nil"))
RI3_HIDDEN = "Hidden Variant Line Xyz"
# 上游 CheckModLineVariant:無 variantList 或含目前 variant 才畫(GetModLineVariantCount 單一 variant 分支)
RI3_ITEM = ('{ rarity = "UNIQUE", title = "Variant Ring", base = {}, variant = 1, '
            'GetModLineVariantCount = function(self, ml) '
            'return (ml.variantList == nil or ml.variantList[self.variant]) and 1 or 0 end, '
            'enchantModLines = { }, implicitModLines = { }, explicitModLines = { '
            '{ line = %r }, { line = %r, variantList = { [2] = true } }, { line = %r } } }'
            % (E1_A, RI3_HIDDEN, E1_B))


def test_ri3_variant_hidden_line_pairs(fork):
    """RI3 -- 未選中 variant 的行夾在兩條可見行之間：可見的兩行仍配對，隱藏行不參與。"""
    MAGIC = "^x8888FF"
    h = stat_harness(fork, E1_STAT_DICT, True, itemstab=RI3_ITEMSTAB, pre=E1_PRE)
    tab = make(h, "ItemsTab")
    tip = make(h, "Tooltip")
    item = h.eval("function() return %s end" % RI3_ITEM)()
    ok, err = h.eval('function(tab, t, item) local ok, e = pcall(function() '
                     'tab:AddItemTooltip(t, item) end) return ok, e end')(tab, tip, item)
    lines = [l for l in tooltip_lines(h, tip) if l is not None][1:]
    check("%s RI3 物品提示本身不出錯" % fork, ok, repr(err))
    check("%s RI3 前提：隱藏 variant 行沒有畫出" % fork,
          not any(RI3_HIDDEN in _norm(l) for l in lines), "lines=%r" % lines)
    check("%s RI3 可見的相鄰兩行(中間夾隱藏行)仍配對成兩行中文" % fork,
          lines[:2] == [MAGIC + z for z in E1_ZH], "lines=%r calls=%r" % (lines, h.stat_calls))
    check("%s RI3 隱藏 variant 行不參與配對查詢" % fork,
          not any(RI3_HIDDEN in _norm_ml(l) for l, c in h.stat_calls if c == "item"),
          "calls=%r" % h.stat_calls)


# ---- E7:配對答案第二行為空 ----------------------------------------------------
# 引擎約定(stat_selftest E7-5/E7-6):中文模板第二行為空時，整段答案為「第一行\n」,
# 依 "\n" 切成兩段、第二段是空字串。注入層的配對(statPair / buildSdPairs)以
# zh:match("^([^\n]*)\n([^\n]*)$") 切兩行，但又要求 z2 ~= "",所以這種答案配對不成立,
# 第二行就落回英文半句。期望：第一行顯示整句中文、第二行顯示空字串。
E7_A = "Grants maximum Energy Shield equal to 10% of your Reserved Mana to"
E7_B = "you and nearby Allies"
E7_Z1 = "賦予你和附近友方等同你 10% 魔力保留的額外能量護盾"
E7_STAT_DICT = {E7_A + "\n" + E7_B: E7_Z1 + "\n"}


def test_e7_pair_empty_second_line(fork):
    """E7 -- 配對答案第二行為空：第一行整句中文、第二行空字串(天賦 sd 與物品詞綴)。"""
    # 天賦說明(nodeSdPairs)
    h, ok, err, tips = p2b_tooltips(fork, E7_STAT_DICT, [[E7_A, E7_B, "10% increased Attack Speed"]])
    lines = tips[0]
    got = [_norm(l) for l in lines[1:4]]
    check("%s E7 天賦提示本身不出錯" % fork, ok, repr(err))
    check("%s E7 前提：以 \"\\n\" 合併句、ctx passive 查詢" % fork,
          (E7_A + "\n" + E7_B, "passive") in [(_norm_ml(l), c) for l, c in h.stat_calls],
          repr(h.stat_calls))
    check("%s E7 天賦說明第二行空：第一行整句中文、第二行空字串" % fork,
          got[:2] == [E7_Z1, ""], "got=%r calls=%r" % (got, h.stat_calls))
    check("%s E7 天賦說明第二行不顯示英文半句" % fork,
          not any(E7_B in _norm(l) for l in lines), "lines=%r" % (lines,))
    check("%s E7 天賦說明之後的行照常" % fork,
          got[2:3] == ["增加 10% 攻擊速度"], "got=%r" % got)
    # 物品詞綴(modPairs)
    h, ok, err, tips = e1_tooltips(fork, E7_STAT_DICT, [e1_item([E7_A, E7_B, "+30 to Dexterity"])])
    got = [_norm(l) for l in tips[0][:3]]
    check("%s E7 物品提示本身不出錯" % fork, ok, repr(err))
    check("%s E7 物品詞綴第二行空：第一行整句中文、第二行空字串" % fork,
          got[:2] == [E7_Z1, ""], "got=%r calls=%r" % (got, h.stat_calls))
    check("%s E7 物品詞綴第二行不顯示英文半句" % fork,
          not any(E7_B in _norm(l) for l in tips[0]), "lines=%r" % (tips[0],))


# ---- S8(使用者重新裁定，撤回依 id):軍團下拉依英文名查 legion -------------
# 查證:POB / poedb / GGPK Name 欄都是 Powerful Faith=暴擊信念、Frenzied Faith=狂怒信念;
# 只有 GGPK 內部 Id 字串對調(templar_notable_minimum_power_charge 的 Name 是
# Frenzied Faith)。裁定撤回依 id 取名：下拉不得呼叫 PobToolsTranslateId("legion", …),
# 依清單項的英文名在 legion 來源查。
# mock:ID_DICT["legion"] 照舊放「依 id 會得到」的對調值，若注入層仍依 id 查就會露餡。
S8_ID = {
    "templar_notable_minimum_power_charge": "狂怒信念",
    "templar_notable_minimum_frenzy_charge": "暴擊信念",
}
S8_TREE = """
{
	{ id = "templar_notable_minimum_power_charge", dn = "Powerful Faith", ["not"] = true, sd = { "PF stat" } },
	{ id = "templar_notable_minimum_frenzy_charge", dn = "Frenzied Faith", ["not"] = true, sd = { "FF stat" } },
}
"""


def s8_labels(fork, jewel_name):
    h = p3_harness(fork)
    tab = p3_tree_tab(h)
    tab.build.spec.tree.legion.nodes = h.eval("function() return %s end" % S8_TREE)()
    tab.build.timelessData.jewelType = h.eval(
        'function(n) return { id = 9, name = n } end')(jewel_name)
    h.eval('function(t) t:FindTimelessJewel() end')(tab)
    return h, p3_popup_labels(h, "nodeSelect")


def test_s8_legion_dropdown_by_id(fork):
    """S8 -- 軍團下拉依英文名查 legion 來源，不依 id(使用者重新裁定)。"""
    saved = ID_DICT.get("legion")
    ID_DICT["legion"] = dict(S8_ID)
    # 依英文的期望值(使用者裁定的 Name 欄譯名)。注意：目前 dist legion.json 沒有這兩個鍵。
    adds = [("legion", "Powerful Faith", "暴擊信念"), ("legion", "Frenzied Faith", "狂怒信念")]
    try:
        h, labels = _with_dict(adds, lambda: s8_labels(fork, "templar"))
    finally:
        if saved is None:
            del ID_DICT["legion"]
        else:
            ID_DICT["legion"] = saved

    def first(ls, stat):
        for l in ls:
            if l.endswith(stat):
                return l[:l.find(LABEL_PAD)] if LABEL_PAD in l else l
        return None
    pf, ff = first(labels, "PF stat"), first(labels, "FF stat")
    check("%s S8 軍團下拉不呼叫 PobToolsTranslateId(\"legion\", …)" % fork,
          not any(s == "legion" for s, _i, _e in h.id_lookups), "id_lookups=%r" % h.id_lookups)
    check("%s S8 Powerful Faith 依英文名得「暴擊信念」" % fork,
          pf == "暴擊信念", "label=%r labels=%r" % (pf, labels))
    check("%s S8 Frenzied Faith 依英文名得「狂怒信念」" % fork,
          ff == "狂怒信念", "label=%r labels=%r" % (ff, labels))
    check("%s S8 下拉以 legion 來源查英文名" % fork,
          "legion" in srcs_of(h, "Powerful Faith"), "lookups=%r" % h.lookups)
    check("%s S8 下拉繪製後來源還原為 nil" % fork, h.source is None, "source=%r" % h.source)


# ---- N2:物品單行 stat 查詢先剝「Not supported」註記 --------------------------
# AddLine 的單行 zhByStat(text, "item") 直接送含註記的整行 → 查不到。期望：剝掉
# main.notSupportedTooltipText 後查，命中就把註記原樣接回(配對路徑已這樣做)。
# GGPK random_curse_when_hit_%_ignoring_curse_limit|100|#(條件固定 100)。
N2_EN = "Curse Enemies which Hit you with a random Hex, ignoring Curse Limit"
N2_ZH = "100% 機率使用 1 個隨機咒術詛咒擊中你的敵人，無視詛咒上限"


def test_n2_item_single_line_note(fork):
    """N2 -- 物品單行帶不支援註記：剝註記查 stat,命中後中文行尾補回註記與原色碼。"""
    if fork != "PoE1":
        # PoE2 上游 AddItemTooltip 的 AddLine 第 4 參數是背景(bg),不是 modLine,注入層
        # 認不出詞綴行，單行本來就不走 zhByStat(text, "item")(現況如此，非本缺口)。
        print("  N/A   %s N2 此分支物品詞綴單行不走 stat 查詢(AddLine 第 4 參數非 modLine)" % fork)
        return
    note = " ^8(Not supported in PoB yet)"
    h, ok, err, tips = e1_tooltips(fork, {N2_EN: N2_ZH}, [e1_item([N2_EN], extra=True)])
    line = tips[0][0] if tips and tips[0] else None
    check("%s N2 物品提示本身不出錯" % fork, ok, repr(err))
    check("%s N2 單行以剝掉註記的英文、ctx item 查 stat" % fork,
          (N2_EN, "item") in [(_norm(l), c) for l, c in h.stat_calls
                              if "Not supported" not in (l or "")],
          "calls=%r" % h.stat_calls)
    check("%s N2 命中後顯示中文並原樣補回註記與色碼" % fork,
          line == "^xF05050" + N2_ZH + note, "line=%r calls=%r" % (line, h.stat_calls))


# ---- N3:配對查詢得到「一行」答案 ---------------------------------------------
# GGPK 英文兩行、中文一行(引擎對 "a\nb" 命中多行模板，回一行中文)。期望：第一行
# 整句中文、第二行留白(與「第二行留空」一致)。
# 判準：一行答案只在「合併句 a\nb 的 stat 查詢本身命中」時採用 —— 引擎的 stat 查詢
# 只會用一個多行模板去對整段，兩條獨立詞綴不會被合成一行;兩條各自獨立的行，合併
# 查詢查不到(nil)時仍各走各的單行路徑，不得留白。
# 資料(dist stats_by_id.json):
#   Tanu Ahi(gloves.lua L682):chance_to_gain_adrenaline_for_2_seconds_on_leech_removed_by_filling_unreserved_life_%
#     "{0}% chance to gain Adrenaline for 2 Seconds when Leech is\nremoved by Filling Unreserved Life"
#     → 當偷取被填滿未保留生命移除時，有 {0}% 機率獲得腎上腺素 2 秒
#   天賦 37358:unattached_sigil_attachment_range_+%_per_second|1|#
#     "Unattached Brands gain {0}% increased Brand Attachment Range per\nsecond, up to a maximum of 100%"
#     → 未附著的烙印每秒增加 {0}% 烙印附著距離
N3_IA = "10% chance to gain Adrenaline for 2 Seconds when Leech is"
N3_IB = "removed by Filling Unreserved Life"
N3_IZ = "當偷取被填滿未保留生命移除時，有 10% 機率獲得腎上腺素 2 秒"
N3_NA = "Unattached Brands gain 20% increased Brand Attachment Range per"
N3_NB = "second, up to a maximum of 100%"
N3_NZ = "未附著的烙印每秒增加 20% 烙印附著距離"
# 反例：兩條獨立詞綴，單行各自命中，合併句沒有模板
N3_X1 = "+30 to Dexterity"
N3_X2 = "10% increased Attack Speed"
N3_STAT = {N3_IA + "\n" + N3_IB: N3_IZ, N3_NA + "\n" + N3_NB: N3_NZ,
           N3_X1: "+30 敏捷(stat)", N3_X2: "增加 10% 攻擊速度(stat)"}


def test_n3_pair_one_line_answer(fork):
    """N3 -- 配對答案只有一行：第一行整句中文、第二行留白;獨立的兩行不被合併。"""
    # 天賦說明
    h, ok, err, tips = p2b_tooltips(fork, N3_STAT, [[N3_NA, N3_NB, N3_X2]])
    got = [_norm(l) for l in tips[0][1:4]]
    check("%s N3 天賦提示本身不出錯" % fork, ok, repr(err))
    check("%s N3 天賦說明一行答案：第一行整句中文、第二行留白" % fork,
          got[:2] == [N3_NZ, ""], "got=%r calls=%r" % (got, h.stat_calls))
    check("%s N3 天賦說明第二行不顯示英文半句" % fork,
          not any(N3_NB in _norm(l) for l in tips[0]), "lines=%r" % (tips[0],))
    # 物品詞綴
    h, ok, err, tips = e1_tooltips(fork, N3_STAT, [e1_item([N3_IA, N3_IB])])
    got = [_norm(l) for l in tips[0][:2]]
    check("%s N3 物品提示本身不出錯" % fork, ok, repr(err))
    check("%s N3 物品詞綴一行答案：第一行整句中文、第二行留白" % fork,
          got == [N3_IZ, ""], "got=%r calls=%r" % (got, h.stat_calls))
    # 反例：兩條獨立詞綴(合併句查不到)各自翻譯，不留白 —— 與各自單獨成行時的結果相同
    h, ok, err, tips = e1_tooltips(fork, N3_STAT, [e1_item([N3_X1, N3_X2])])
    got = [_norm(l) for l in tips[0][:2]]
    _h, _o, _e, alone = e1_tooltips(fork, N3_STAT, [e1_item([N3_X1]), e1_item([N3_X2])])
    want = [_norm(alone[0][0]), _norm(alone[1][0])]
    check("%s N3 反例：兩條獨立詞綴不合併、各自翻譯不留白" % fork,
          got == want and all(g and g not in (N3_X1, N3_X2) for g in got),
          "got=%r 各自單獨=%r calls=%r" % (got, want, h.stat_calls))


# ---- 多行 GGPK 描述的整句重組(GR1 ~ GR7) ---------------------------------------
#
# 引擎 PobToolsTranslateLines(lines, ctx)(ggpk_groups.json):連續幾行是同一句 GGPK 描述時,
# 回每行該畫的中文("" = 留白,整句在第 1 行)。假函式依 GR_GROUPS 逐段比對(長的先),
# 只認「整組英文行都在」;F2 關閉回 nil。每個位置都另跑一次「沒有注入」證明英文半句的
# 問題會重現,以及「F2 關」證明不會在關翻譯時留白。

GR_MAGMA = ["Every 10 seconds, gain 30% of Physical Damage", "as Extra Fire Damage for 4 seconds"]
GR_MAGMA_ZH = ["每 10 秒，獲得 30% 物理傷害的額外火焰傷害 5 秒", ""]
GR_FLAV = ["My ancestral pact was sealed. Forevermore, I would gain sustenance",
           "only from the ravaged flesh of my enemies."]
GR_FLAV_ZH = ["在與先祖定下冥約之後...從此我將只能從蹂躪敵人的肉體中得到心靈的寄託，直到永遠。", ""]
GR_GEMFLAV = ["The Black Sekhema was a formidable fighter", "alongside her dekhara, but she was unstoppable",
              "when she fought atop her trusted mount, Shiyo."]
GR_GEMFLAV_ZH = ["漆黑絲克瑪與她的部下並肩作戰時 已是一名令人敬畏的戰士", "", ""]
GR_TATTOO = ["Trigger level 1 Summon Spirit of Ikiaho Skill when you use a Travel",
             "Skill while a Unique Enemy is in your Presence"]
GR_TATTOO_ZH = ["有傳奇敵人在你的存在範圍內且你使用快行技能時，", "觸發等級 1 的召喚伊基亞呼之靈技能"]
GR_TIER = ["Bleeding Enemies you Kill Explode, dealing (2-5)% of", "their Maximum Life as Physical Damage"]
GR_TIER_ZH = ["擊殺流血的敵人時產生爆炸，造成等同其 (2-5)% 最大生命的物理傷害", ""]
GR_ONE = ["Bleeding Enemies you Kill Explode, dealing (2-3)% of", "their Maximum Life as Physical Damage"]
GR_ONE_ZH = ["擊殺流血的敵人時產生爆炸，造成等同其 (2-3)% 最大生命的物理傷害", ""]
GR_GROUPS = [(GR_GEMFLAV, GR_GEMFLAV_ZH), (GR_MAGMA, GR_MAGMA_ZH), (GR_FLAV, GR_FLAV_ZH),
             (GR_TATTOO, GR_TATTOO_ZH), (GR_TIER, GR_TIER_ZH), (GR_ONE, GR_ONE_ZH)]
GR_UNSUP = "^xF05050"
GR_NOTE = " ^8(Not supported in PoB yet)"

GR_TREE_METHODS = """
function C:AddNodeName(tooltip, node, build)
	tooltip:AddLine(24, node.dn, "FONTIN")
end
function C:AddNodeTooltip(tooltip, node, build)
	self:AddNodeName(tooltip, node, build)
	for _, line in ipairs(node.sd or {}) do tooltip:AddLine(16, "^7"..line) end
	-- 上游 PassiveTreeView.lua「Flavour text」段
	if node.flavourText and main.showFlavourText then
		for _, line in ipairs(node.flavourText) do
			tooltip:AddLine(16, colorCodes.UNIQUE..line, "FONTIN ITALIC")
		end
	end
end
"""
GR_NOTABLE_METHODS = """
-- 上游 NotableDBControl:AddValueTooltip 的節點說明段(名稱行簡化)
function C:AddValueTooltip(tooltip, index, node)
	tooltip:AddLine(24, node.dn, "FONTIN")
	if node.sd[1] then
		tooltip:AddLine(16, "")
		for i, line in ipairs(node.sd) do
			if node.unsupported then
				local l = colorCodes.UNSUPPORTED..line
				l = main.notSupportedModTooltips and (l .. main.notSupportedTooltipText) or l
				tooltip:AddLine(16, l)
			else
				tooltip:AddLine(16, colorCodes.MAGIC..line)
			end
		end
	end
end
"""
# 上游 TreeTab:ModifyNodePopup 的說明標籤(constructUI)與下拉提示,只留畫字相關的部分
GR_TREETAB_METHODS = """
function C:ModifyNodePopup(selectedNode)
	local controls = { }
	local modGroups = { }
	for id, node in pairs(self.build.spec.tree.tattoo.nodes) do
		table.insert(modGroups, { label = node.dn, descriptions = copyTable(node.sd), id = id })
	end
	table.sort(modGroups, function(a, b) return a.label < b.label end)
	local function constructUI(modGroup)
		local i = 1
		while controls[i] do controls[i] = nil i = i + 1 end
		local wrapTable = {}
		for _, desc in ipairs(modGroup.descriptions) do
			for _, w in ipairs(main:WrapString(desc, 16, 375)) do table.insert(wrapTable, w) end
		end
		for idx, desc in ipairs(wrapTable) do controls[idx] = { label = "^7"..desc } end
	end
	controls.modSelect = { list = modGroups, selIndex = 1, Draw = function() end,
		selFunc = function(idx) constructUI(modGroups[idx]) end }
	controls.modSelect.tooltipFunc = function(tooltip, mode, index, value)
		tooltip:Clear()
		if mode ~= "OUT" and value then
			for _, line in ipairs(value.descriptions) do tooltip:AddLine(16, "^7"..line) end
		end
	end
	main.popups[1] = { controls = controls }
	constructUI(modGroups[1])
end
"""
# 上游 ItemsTab 製作區詞綴下拉的 tooltipFunc(單一詞綴 / 多階合併兩支)
GR_ITEMSTAB_CTOR = """
	self.controls = { }
	local drop = { list = { }, selIndex = 1 }
	drop.tooltipFunc = function(tooltip, mode, index, value)
		local modList = value.modList
		tooltip:Clear()
		if value.modId or #modList == 1 then
			local mod = self.displayItem.affixes[value.modId or modList[1]]
			tooltip:AddLine(16, "^7Affix: "..mod.affix)
			for _, line in ipairs(mod) do tooltip:AddLine(14, "^7"..line) end
		else
			tooltip:AddLine(16, "^7"..#modList.." Tiers")
			local minMod = self.displayItem.affixes[modList[1]]
			local maxMod = self.displayItem.affixes[modList[#modList]]
			for l, line in ipairs(minMod) do
				local minLine = line:gsub("%((%d[%d%.]*)%-(%d[%d%.]*)%)", "%1")
				local maxLine = maxMod[l]:gsub("%((%d[%d%.]*)%-(%d[%d%.]*)%)", "%2")
				if maxLine == maxMod[l] then
					tooltip:AddLine(14, maxLine)
				else
					local start = 1
					tooltip:AddLine(14, (minLine:gsub("%d[%d%.]*", function(min)
						local s, e, max = maxLine:find("(%d[%d%.]*)", start)
						start = e + 1
						if min == max then return min else return "("..min.."-"..max..")" end
					end)))
				end
			end
		end
	end
	self.controls.displayItemAffix1 = drop
	self.displayItem = { affixes = {
		Bleed1 = { type = "Prefix", affix = "Gladiator's", "Bleeding Enemies you Kill Explode, dealing (2-3)% of",
		           "their Maximum Life as Physical Damage" },
		Bleed2 = { type = "Prefix", affix = "Gladiator's", "Bleeding Enemies you Kill Explode, dealing (4-5)% of",
		           "their Maximum Life as Physical Damage" } } }
"""


def group_harness(fork, with_inject=True, define_lines=True):
    h = Harness(fork, with_inject=False)
    h.loose_keys = True
    h.line_calls = []

    def translate_lines(tbl, ctx=None):
        lines = [tbl[i] for i in range(1, len(tbl) + 1)]
        h.line_calls.append((lines, ctx))
        if not h.enabled:
            return None
        out, i = {}, 0
        while i < len(lines):
            hit = next(((en, zh) for en, zh in GR_GROUPS if lines[i:i + len(en)] == en), None)
            if hit:
                for j, z in enumerate(hit[1]):
                    out[i + j + 1] = z
                i += len(hit[0])
            else:
                i += 1
        return h.rt.table_from(out) if out else None

    g = h.rt.globals()
    g["__py_lines"] = translate_lines
    h.run(b"function PobToolsTranslateStat(l, c, i) return nil end")
    if define_lines:
        h.run(b"function PobToolsTranslateLines(t, c) return __py_lines(t, c) end")
    h.run(COLOUR_SETUP)
    h.run(('colorCodes.UNSUPPORTED = "%s" main.notSupportedModTooltips = true '
           'main.notSupportedTooltipText = "%s" main.showFlavourText = true '
           'function main:WrapString(str, height, width) return { str } end') % (GR_UNSUP, GR_NOTE))
    h.run('LoadModule("Classes/Tooltip")')
    h.define("PassiveTreeView", 'local C = newClass("PassiveTreeView")\nfunction C:PassiveTreeView() return self end\n'
             + GR_TREE_METHODS, 'local C = newClass("PassiveTreeView", function(self) end)\n' + GR_TREE_METHODS)
    h.define("NotableDBControl", 'local C = newClass("NotableDBControl")\nfunction C:NotableDBControl() return self end\n'
             + GR_NOTABLE_METHODS, 'local C = newClass("NotableDBControl", function(self) end)\n' + GR_NOTABLE_METHODS)
    h.define("TreeTab", 'local C = newClass("TreeTab")\nfunction C:TreeTab() return self end\n' + GR_TREETAB_METHODS,
             'local C = newClass("TreeTab", function(self) end)\n' + GR_TREETAB_METHODS)
    h.define("ItemsTab", 'local C = newClass("ItemsTab")\nfunction C:ItemsTab()\n' + GR_ITEMSTAB_CTOR
             + '\treturn self\nend\n' + ITEMTIP_METHODS.replace("MODLINE_ARG", "modLine"),
             'local C = newClass("ItemsTab", function(self)\n' + GR_ITEMSTAB_CTOR + 'end)\n'
             + ITEMTIP_METHODS.replace("MODLINE_ARG", "nil"))
    if with_inject:
        h.apply_inject()
    return h


def _lua_list(h, xs):
    return h.eval("function(...) return {...} end")(*xs)


def gr_anoint(fork, sd, with_inject=True, enabled=True, unsupported=False, define_lines=True):
    h = group_harness(fork, with_inject, define_lines)
    h.enabled = enabled
    ctl = make(h, "NotableDBControl")
    tip = make(h, "Tooltip")
    ok, err = h.eval('function(c, t, sd, u) local ok, e = pcall(function() '
                     'c:AddValueTooltip(t, 1, { dn = "Magmatic Strikes", id = 5430, sd = sd, unsupported = u }) '
                     'end) return ok, e end')(ctl, tip, _lua_list(h, sd), unsupported)
    return h, ok, err, [l for l in tooltip_lines(h, tip) if l is not None]


def test_groups_anoint_popup(fork):
    """GR1 -- 塗油彈窗節點浮窗:兩個 sd 行是同一句時整句在第 1 行、第 2 行留白。"""
    h, ok, err, lines = gr_anoint(fork, GR_MAGMA)
    check("%s GR1 塗油彈窗浮窗本身不出錯" % fork, ok, repr(err))
    check("%s GR1 塗油彈窗兩行合併為整句、第 2 行留白" % fork,
          [_norm(l) for l in lines[2:4]] == GR_MAGMA_ZH, "lines=%r calls=%r" % (lines, h.line_calls))
    check("%s GR1 以 passive 情境整組詢問引擎" % fork,
          (GR_MAGMA, "passive") in h.line_calls, repr(h.line_calls))
    check("%s GR1 第 1 行保留原色碼" % fork, lines[2:3] == ["^x8888FF" + GR_MAGMA_ZH[0]], repr(lines))
    h0, ok0, err0, lines0 = gr_anoint(fork, GR_MAGMA, with_inject=False)
    check("%s GR1 前提:沒有注入時兩行都是英文半句" % fork,
          [_norm(l) for l in lines0[2:4]] == GR_MAGMA, repr(lines0))
    hf, okf, errf, linesf = gr_anoint(fork, GR_MAGMA, enabled=False)
    check("%s GR1 F2 關閉時不合併、兩行照英文" % fork,
          okf and [_norm(l) for l in linesf[2:4]] == GR_MAGMA, repr(linesf))
    # 不支援註記:整句那行保留註記,留白那行不再只剩註記
    hu, oku, erru, linesu = gr_anoint(fork, GR_MAGMA, unsupported=True)
    check("%s GR1 註記在整句行行尾、留白行完全空白" % fork,
          linesu[2:4] == [GR_UNSUP + GR_MAGMA_ZH[0] + GR_NOTE, ""], repr(linesu))


def test_groups_node_flavour(fork):
    """GR2 -- 天賦樹浮窗風味文字:GGPK 一段、POB 兩行 → 整段在第 1 行、其餘留白。"""
    def run(with_inject=True, enabled=True):
        h = group_harness(fork, with_inject)
        h.enabled = enabled
        view = make(h, "PassiveTreeView")
        tip = make(h, "Tooltip")
        ok, err = h.eval('function(v, t, f) local ok, e = pcall(function() v:AddNodeTooltip(t, '
                         '{ dn = "Vaal Pact", id = 10808, sd = { "10% increased Attack Speed" }, flavourText = f }, {}) '
                         'end) return ok, e end')(view, tip, _lua_list(h, GR_FLAV))
        return h, ok, err, [l for l in tooltip_lines(h, tip) if l is not None]
    h, ok, err, lines = run()
    check("%s GR2 天賦浮窗本身不出錯" % fork, ok, repr(err))
    check("%s GR2 風味文字整段在第 1 行、第 2 行留白" % fork,
          [_norm(l) for l in lines[-2:]] == GR_FLAV_ZH, "lines=%r calls=%r" % (lines, h.line_calls))
    check("%s GR2 說明行仍逐行(單行不入組)" % fork, _norm(lines[1]) == "增加 10% 攻擊速度", repr(lines))
    h0, ok0, err0, lines0 = run(with_inject=False)
    check("%s GR2 前提:沒有注入時風味文字兩行英文" % fork, [_norm(l) for l in lines0[-2:]] == GR_FLAV, repr(lines0))
    hf, okf, errf, linesf = run(enabled=False)
    check("%s GR2 F2 關閉時風味文字照英文" % fork, [_norm(l) for l in linesf[-2:]] == GR_FLAV, repr(linesf))


# 上游 GemTooltip.lua 末段的風味文字(每行一個 AddLine,寶石提示期間)補進兩個合成模組;
# 既有測試的寶石沒有 flavourText,不受影響
GR_GEM_FLAVOUR_BLOCK = """
	local fe = gemInstance.gemData.grantedEffect
	if fe.flavourText and main.showFlavourText then
		for _, line in ipairs(fe.flavourText) do
			tooltip:AddLine(16, colorCodes.UNIQUE .. line, "FONTIN SC ITALIC")
		end
	end
end
return GemTooltip
"""
for _name in ("GEMTOOLTIP_SYN_P1", "GEMTOOLTIP_SYN_P2"):
    _src = globals()[_name]
    assert _src.count("\nend\nreturn GemTooltip\n") == 1, _name
    globals()[_name] = _src.replace("\nend\nreturn GemTooltip\n", GR_GEM_FLAVOUR_BLOCK)


def gr_gem(fork, enabled=True, define_lines=True):
    h = gem_harness(fork, {})
    h.enabled = enabled
    calls = []

    def translate_lines(tbl, ctx=None):
        lines = [tbl[i] for i in range(1, len(tbl) + 1)]
        calls.append((lines, ctx))
        if lines == GR_GEMFLAV and h.enabled:
            return h.rt.table_from({1: GR_GEMFLAV_ZH[0], 2: "", 3: ""})
        return None
    h.rt.globals()["__py_lines"] = translate_lines
    if define_lines:
        h.run(b"function PobToolsTranslateLines(t, c) return __py_lines(t, c) end")
    h.run(b"main.showFlavourText = true colorCodes.UNIQUE = \"^xAF6025\"")
    obj = make(h, "GemSelectControl")
    tip = make(h, "Tooltip")
    gem = h.eval("function() return %s end" % gem_instance_src(fork))()
    h.eval('function(g, f) g.gemData.grantedEffect.flavourText = f end')(gem, _lua_list(h, GR_GEMFLAV))
    ok, err = h.eval('function(o, t, g) o.tooltip = t o.skillsTab = { build = { data = data } } '
                     'o.tipGem = g local ok, e = pcall(function() o:Draw({}, false) end) '
                     'return ok, e end')(obj, tip, gem)
    lines = [_norm(l) for l in tooltip_lines(h, tip) if l is not None]
    return h, ok, err, lines, calls


def test_groups_gem_flavour(fork):
    """GR3 -- 寶石浮窗風味文字三行、GGPK 一段 → 整段在第 1 行、其餘兩行留白。"""
    h, ok, err, lines, calls = gr_gem(fork)
    check("%s GR3 寶石提示本身不出錯" % fork, ok, repr(err))
    check("%s GR3 風味文字整段在第 1 行、其餘兩行留白" % fork,
          lines[-3:] == GR_GEMFLAV_ZH, "lines=%r calls=%r" % (lines, calls))
    check("%s GR3 以 gem 情境整組詢問引擎" % fork, (GR_GEMFLAV, "gem") in calls, repr(calls))
    hf, okf, errf, linesf, callsf = gr_gem(fork, enabled=False)
    check("%s GR3 F2 關閉時風味文字照英文" % fork, okf and linesf[-3:] == GR_GEMFLAV, repr(linesf))
    h0, ok0, err0, lines0, calls0 = gr_gem(fork, define_lines=False)
    check("%s GR3 舊引擎(無 PobToolsTranslateLines)風味文字照英文、不報錯" % fork,
          ok0 and lines0[-3:] == GR_GEMFLAV, "lines=%r err=%r" % (lines0, err0))


def gr_tattoo(fork, with_inject=True, enabled=True):
    h = group_harness(fork, with_inject)
    h.enabled = enabled
    tab = make(h, "TreeTab")
    tree = h.eval('function(sd) return { tattoo = { nodes = { ["Loyalty Tattoo of Ikiaho"] = '
                  '{ dn = "Loyalty Tattoo of Ikiaho", sd = sd } } }, nodes = {} } end')(_lua_list(h, GR_TATTOO))
    ok, err = h.eval('function(tab, tree) tab.build = { spec = { tree = tree } } '
                     'local ok, e = pcall(function() tab:ModifyNodePopup({ id = 1 }) end) return ok, e end')(tab, tree)
    labels = h.eval('function() local c = main.popups[1].controls local o = {} '
                    'local i = 1 while c[i] do o[i] = c[i].label i = i + 1 end return o end')()
    labels = [labels[i] for i in range(1, len(labels) + 1)]
    return h, ok, err, labels


def test_groups_tattoo_popup(fork):
    """GR4 -- 紋身彈窗說明標籤與下拉提示:兩個 sd 行依 GGPK 中文分行,不再半句中文半句英文。"""
    h, ok, err, labels = gr_tattoo(fork)
    check("%s GR4 紋身彈窗本身不出錯" % fork, ok, repr(err))
    check("%s GR4 說明標籤依 GGPK 中文分行" % fork,
          labels == ["^7" + z for z in GR_TATTOO_ZH], "labels=%r calls=%r" % (labels, h.line_calls))
    # 下拉 selFunc 重畫(驅動器 / 使用者換選取時)也是同一結果
    ok2, err2 = h.eval('function() local ok, e = pcall(function() main.popups[1].controls.modSelect.selFunc(1) end) return ok, e end')()
    labels2 = h.eval('function() local c = main.popups[1].controls local o = {} '
                     'local i = 1 while c[i] do o[i] = c[i].label i = i + 1 end return o end')()
    labels2 = [labels2[i] for i in range(1, len(labels2) + 1)]
    check("%s GR4 selFunc 重畫說明標籤仍整句重組" % fork, ok2 and labels2 == labels, "labels=%r %r" % (labels2, err2))
    # 下拉提示(tooltipFunc,每行一個 AddLine)
    tip = make(h, "Tooltip")
    ok3, err3 = h.eval('function(t) local dd = main.popups[1].controls.modSelect '
                       'local ok, e = pcall(function() dd.tooltipFunc(t, "HOVER", 1, dd.list[1]) end) return ok, e end')(tip)
    tl = [l for l in tooltip_lines(h, tip) if l is not None]
    check("%s GR4 下拉提示依 GGPK 中文分行" % fork, ok3 and [_norm(l) for l in tl] == GR_TATTOO_ZH,
          "tip=%r %r" % (tl, err3))
    # 說明標籤之外 WrapString 不受影響
    after = h.eval('function(s) return main:WrapString(s, 16, 375)[1] end')(GR_TATTOO[1])
    check("%s GR4 彈窗建完後 WrapString 不再套用分組" % fork, after == GR_TATTOO[1], repr(after))
    h0, ok0, err0, labels0 = gr_tattoo(fork, with_inject=False)
    check("%s GR4 前提:沒有注入時說明標籤是英文兩行" % fork, labels0 == ["^7" + s for s in GR_TATTOO], repr(labels0))
    hf, okf, errf, labelsf = gr_tattoo(fork, enabled=False)
    check("%s GR4 F2 關閉時說明標籤照英文" % fork, labelsf == ["^7" + s for s in GR_TATTOO], repr(labelsf))


def gr_craft(fork, value_src, with_inject=True):
    h = group_harness(fork, with_inject)
    tab = make(h, "ItemsTab")
    tip = make(h, "Tooltip")
    ok, err = h.eval('function(tab, t) local ok, e = pcall(function() '
                     'tab.controls.displayItemAffix1.tooltipFunc(t, "HOVER", 1, %s) end) return ok, e end' % value_src)(tab, tip)
    return h, ok, err, [l for l in tooltip_lines(h, tip) if l is not None]


def test_groups_craft_affix_tip(fork):
    """GR5 -- 製作詞綴下拉浮窗:單一詞綴與多階合併的兩行都整句一行、第 2 行留白。"""
    h, ok, err, lines = gr_craft(fork, '{ modList = { "Bleed1", "Bleed2" } }')
    check("%s GR5 多階浮窗本身不出錯" % fork, ok, repr(err))
    check("%s GR5 多階合併的兩行整句一行、第 2 行留白" % fork,
          [_norm(l) for l in lines[1:3]] == GR_TIER_ZH, "lines=%r calls=%r" % (lines, h.line_calls))
    check("%s GR5 以 item 情境詢問" % fork, any(c == "item" for _, c in h.line_calls), repr(h.line_calls))
    h1, ok1, err1, lines1 = gr_craft(fork, '{ modList = { "Bleed1" } }')
    check("%s GR5 單一詞綴兩行整句一行、第 2 行留白" % fork,
          ok1 and [_norm(l) for l in lines1[1:3]] == GR_ONE_ZH, "lines=%r %r" % (lines1, err1))
    h0, ok0, err0, lines0 = gr_craft(fork, '{ modList = { "Bleed1", "Bleed2" } }', with_inject=False)
    check("%s GR5 前提:沒有注入時兩行英文" % fork, [_norm(l) for l in lines0[1:3]] == GR_TIER, repr(lines0))


def test_groups_old_engine(fork):
    """GR6 -- 沒有 PobToolsTranslateLines 的引擎:不報錯,照舊(兩行英文、無假合併)。"""
    h, ok, err, lines = gr_anoint(fork, GR_MAGMA, define_lines=False)
    check("%s GR6 舊引擎塗油彈窗不報錯且照舊逐行" % fork,
          ok and [_norm(l) for l in lines[2:4]] == GR_MAGMA, "lines=%r err=%r" % (lines, err))


# ---- R1:寶石浮窗不支援的 stat 行(使用者裁定 2026-10-25)----------------------
# 上游 GemTooltip(PoE1 L199-201):lineMap 對不到 statMap 的行畫成
#   colorCodes.UNSUPPORTED .. line [.. main.notSupportedTooltipText]
# 期望:剝掉註記再查 stat(含 lineMap 的 stat id),譯文後接回原註記。
GEMTOOLTIP_SYN_P1_UNSUP = GEMTOOLTIP_SYN_P1.replace(
    """		for _, line in ipairs(descriptions) do
			tooltip:AddLine(16, colorCodes.MAGIC .. line, "FONTIN SC")
		end
		addQualityRange(tooltip, build, grantedEffect)""",
    """		for _, line in ipairs(descriptions) do
			local l = colorCodes.UNSUPPORTED .. line
			l = main.notSupportedModTooltips and (l .. main.notSupportedTooltipText) or l
			tooltip:AddLine(16, l, "FONTIN SC")
		end
		addQualityRange(tooltip, build, grantedEffect)""")
GEMTOOLTIP_SYN_P2_UNSUP = GEMTOOLTIP_SYN_P2.replace(
    """	tooltip:AddLine(16, colorCodes.MAGIC .. line, "FONTIN SC", bg)""",
    """	local l = colorCodes.UNSUPPORTED .. line
	l = main.notSupportedModTooltips and (l .. main.notSupportedTooltipText) or l
	tooltip:AddLine(16, l, "FONTIN SC", bg)""")


def r1_lines(fork, stat_dict, note_on=True, define_stat=True):
    """回 (h, ok, err, 原樣行):GemSelectControl:Draw 建出的寶石提示,不去色碼。"""
    global GEMTOOLTIP_SYN_P1, GEMTOOLTIP_SYN_P2
    saved = (GEMTOOLTIP_SYN_P1, GEMTOOLTIP_SYN_P2)
    assert GEMTOOLTIP_SYN_P1_UNSUP != saved[0] and GEMTOOLTIP_SYN_P2_UNSUP != saved[1], "不支援行替身沒有替換成功"
    GEMTOOLTIP_SYN_P1, GEMTOOLTIP_SYN_P2 = GEMTOOLTIP_SYN_P1_UNSUP, GEMTOOLTIP_SYN_P2_UNSUP
    try:
        h = gem_harness(fork, stat_dict, define_stat)
        h.run(('colorCodes.UNSUPPORTED = "%s" main.notSupportedModTooltips = %s '
               'main.notSupportedTooltipText = "%s"') % (GR_UNSUP, "true" if note_on else "false", GR_NOTE))
        obj = make(h, "GemSelectControl")
        tip = make(h, "Tooltip")
        gem = h.eval("function() return %s end" % gem_instance_src(fork))()
        ok, err = h.eval('function(o, t, g) o.tooltip = t o.skillsTab = { build = { data = data } } '
                         'o.tipGem = g local ok, e = pcall(function() o:Draw({}, false) end) '
                         'return ok, e end')(obj, tip, gem)
        lines = [l for l in tooltip_lines(h, tip) if l is not None]
    finally:
        GEMTOOLTIP_SYN_P1, GEMTOOLTIP_SYN_P2 = saved
    return h, ok, err, lines


def test_r1_gem_unsupported_note(fork):
    """R1 -- 寶石不支援的 stat 行:剝註記查 stat,中文後接回原註記與色碼。"""
    en = "Deals 10 to 20 Fire Damage"
    h, ok, err, lines = r1_lines(fork, GEM_STAT_DICT)
    check("%s R1 寶石提示本身不出錯" % fork, ok, repr(err))
    check("%s R1 以剝掉註記的英文查 stat(ctx gem:gem_stat_descriptions)" % fork,
          (en, "gem:gem_stat_descriptions") in [(_norm(l), c) for l, c in h.stat_calls]
          and not any("Not supported" in (l or "") for l, c in h.stat_calls if c and c.startswith("gem")),
          repr(h.stat_calls))
    ids = [i for l, c, i in h.stat_ids if _norm(l) == en and c == "gem:gem_stat_descriptions"]
    check("%s R1 剝註記後仍帶該行 lineMap 的 stat id" % fork,
          ids and all(isinstance(i, str) and "spell_maximum_base_fire_damage" in i.split() for i in ids),
          "ids=%r" % ids)
    check("%s R1 命中後中文行尾接回註記、保留不支援色碼" % fork,
          GR_UNSUP + "STAT寶石" + GR_NOTE in lines, "lines=%r calls=%r" % (lines, h.stat_calls))
    hn, okn, errn, linesn = r1_lines(fork, GEM_STAT_DICT, note_on=False)
    check("%s R1 註記開關關閉時中文行沒有註記" % fork,
          okn and GR_UNSUP + "STAT寶石" in linesn, "lines=%r" % linesn)
    hm, okm, errm, linesm = r1_lines(fork, {})
    h0, ok0, err0, lines0 = r1_lines(fork, None, define_stat=False)
    check("%s R1 未命中時與舊引擎相同(英文 + 註記)" % fork,
          okm and linesm == lines0 and GR_UNSUP + en + GR_NOTE in linesm,
          "lines=%r 舊引擎=%r" % (linesm, lines0))


# ---- R2:紋身彈窗說明標籤「前一行 + 本行」(使用者裁定 2026-10-25)--------------
# 三個忠誠紋身共用續行「a Unique Enemy is in your Presence」,各自的 GGPK 中文不同:
# 只看本行的表在那一行衝突(false),說明標籤第 2 行掉回英文。
GR_KAOM = ["Trigger level 1 Summon Spirit of Kaom Skill when a Totem dies while",
           "a Unique Enemy is in your Presence"]
GR_KAOM_ZH = ["有傳奇敵人在你的存在範圍內且有圖騰死亡時，", "觸發等級 1 的召喚岡姆之靈技能"]
GR_AKOYA = ["Trigger level 1 Summon Spirit of Akoya Skill when you reach Maximum Rage while",
            "a Unique Enemy is in your Presence"]
GR_AKOYA_ZH = ["有傳奇敵人在你的存在範圍內且你達到最大怒火時，", "觸發等級 1 的召喚阿寇亞之靈技能"]
GR_LIMIT = "Limited to 1 Loyalty Tattoo"
GR_GROUPS += [(GR_KAOM, GR_KAOM_ZH), (GR_AKOYA, GR_AKOYA_ZH)]


def r2_labels(fork, enabled=True):
    h = group_harness(fork)
    h.enabled = enabled
    tab = make(h, "TreeTab")
    tree = h.eval('function(a, b) return { tattoo = { nodes = { '
                  '["Loyalty Tattoo of Kaom"] = { dn = "Loyalty Tattoo of Kaom", sd = a }, '
                  '["Loyalty Tattoo of Akoya"] = { dn = "Loyalty Tattoo of Akoya", sd = b } } }, nodes = {} } end')(
        _lua_list(h, GR_KAOM + [GR_LIMIT]), _lua_list(h, GR_AKOYA + [GR_LIMIT]))
    ok, err = h.eval('function(tab, tree) tab.build = { spec = { tree = tree } } '
                     'local ok, e = pcall(function() tab:ModifyNodePopup({ id = 1 }) end) return ok, e end')(tab, tree)
    read = 'function() local c = main.popups[1].controls local o = {} local i = 1 ' \
           'while c[i] do o[i] = c[i].label i = i + 1 end return o end'
    first = h.eval(read)()     # modGroups 依名稱排序:1 = Akoya、2 = Kaom
    ok2, err2 = h.eval('function() local ok, e = pcall(function() main.popups[1].controls.modSelect.selFunc(2) end) '
                       'return ok, e end')()
    second = h.eval(read)()
    as_list = lambda t: [t[i] for i in range(1, len(t) + 1)]
    after = h.eval('function(s) return main:WrapString(s, 16, 375)[1] end')(GR_KAOM[1])
    return h, ok and ok2, (err, err2), as_list(first), as_list(second), after


def test_r2_tattoo_label_prev_line(fork):
    """R2 -- 紋身說明標籤:同一英文續行在不同紋身各取自己的 GGPK 中文(看前一行)。"""
    h, ok, err, akoya, kaom, after = r2_labels(fork)
    check("%s R2 紋身彈窗本身不出錯" % fork, ok, repr(err))
    check("%s R2 Akoya 說明標籤兩行都是自己的 GGPK 中文" % fork,
          akoya == ["^7" + z for z in GR_AKOYA_ZH] + ["^7" + GR_LIMIT], "labels=%r" % akoya)
    check("%s R2 換選 Kaom 後共用續行改用 Kaom 的中文" % fork,
          kaom == ["^7" + z for z in GR_KAOM_ZH] + ["^7" + GR_LIMIT], "labels=%r" % kaom)
    check("%s R2 彈窗建完後 WrapString 不再套用分組" % fork, after == GR_KAOM[1], repr(after))
    hf, okf, errf, akf, kaf, _ = r2_labels(fork, enabled=False)
    check("%s R2 F2 關閉時說明標籤照英文" % fork,
          okf and kaf == ["^7" + s for s in GR_KAOM + [GR_LIMIT]], "labels=%r" % kaf)


# ---- SC:專精彈窗的「/」查表語境(審查第 5 輪)------------------------------------
# 上游 TreeTab:OpenMasteryPopup 以 t_concat(effect.stats, " / ") 當選項文字、建 PassiveMasteryControl
# 時量寬;PassiveMasteryControl:Draw 畫選項。兩者執行期間引擎的「/」查表要用 passive 語境,結束(含出錯)還原。
SC_TREETAB_METHODS = """
function C:ModifyNodePopup(selectedNode) end
function C:OpenMasteryPopup(node, viewPort)
	__seen[#__seen + 1] = "open:" .. tostring(__slash_ctx)
	if node and node.explode then error("boom open") end
end
"""
SC_MASTERY_METHODS = """
function C:Draw(viewPort)
	__seen[#__seen + 1] = "draw:" .. tostring(__slash_ctx)
	if viewPort and viewPort.explode then error("boom draw") end
end
"""


def sc_harness(fork, define_api=True):
    h = Harness(fork, with_inject=False)
    h.loose_keys = True
    h.run(b"__seen = {} __slash_ctx = nil")
    if define_api:
        h.run(b"function PobToolsSetSlashContext(c) local p = __slash_ctx __slash_ctx = c return p end")
    h.run(COLOUR_SETUP)
    h.define("TreeTab", 'local C = newClass("TreeTab")\nfunction C:TreeTab() return self end\n' + SC_TREETAB_METHODS,
             'local C = newClass("TreeTab", function(self) end)\n' + SC_TREETAB_METHODS)
    h.define("PassiveMasteryControl",
             'local C = newClass("PassiveMasteryControl")\nfunction C:PassiveMasteryControl() return self end\n'
             + SC_MASTERY_METHODS,
             'local C = newClass("PassiveMasteryControl", function(self) end)\n' + SC_MASTERY_METHODS)
    h.apply_inject()
    return h


SC_RUN = """function(tab, ctl)
	local o = {}
	__slash_ctx = "gem"
	tab:OpenMasteryPopup({}, {})
	o[1] = tostring(__slash_ctx)
	ctl:Draw({})
	o[2] = tostring(__slash_ctx)
	local ok1 = pcall(function() tab:OpenMasteryPopup({ explode = true }, {}) end)
	local ok2 = pcall(function() ctl:Draw({ explode = true }) end)
	o[3] = tostring(__slash_ctx)
	o[4] = tostring(ok1)
	o[5] = tostring(ok2)
	for i, s in ipairs(__seen) do o[5 + i] = s end
	return o
end"""


def test_slash_ctx_mastery(fork):
    """SC -- 專精彈窗建立與繪製期間「/」查表用 passive 語境,結束後(含出錯)還原原語境。"""
    h = sc_harness(fork)
    tab = make(h, "TreeTab")
    ctl = make(h, "PassiveMasteryControl")
    r = h.eval(SC_RUN)(tab, ctl)
    r = [r[i] for i in range(1, len(r) + 1)]
    check("%s SC 建彈窗(量寬)與繪製選項時語境為 passive" % fork,
          r[5:7] == ["open:passive", "draw:passive"], repr(r))
    check("%s SC 結束後還原呼叫前的語境" % fork, r[0:2] == ["gem", "gem"], repr(r))
    check("%s SC 出錯時照樣拋出且語境還原" % fork, r[2:5] == ["gem", "false", "false"], repr(r))
    h0 = sc_harness(fork, define_api=False)
    tab0 = make(h0, "TreeTab")
    ctl0 = make(h0, "PassiveMasteryControl")
    ok0, r0 = h0.eval('function(f, a, b) local ok, r = pcall(f, a, b) return ok, r end')(h0.eval(SC_RUN), tab0, ctl0)
    r0 = [r0[i] for i in range(1, len(r0) + 1)] if ok0 else r0
    check("%s SC 舊引擎(無 PobToolsSetSlashContext)照常執行、不報錯" % fork,
          ok0 and r0[5:7] == ["open:gem", "draw:gem"], repr(r0))


# ---- N4:天賦浮窗單行帶不支援註記(使用者裁定 2026-10-26:註記維持英文、留在該行;本文照翻)----------
# 上游 PassiveTreeView(PoE1 L1558-1561)/NotableDBControl:不支援的 sd 行畫成
#   colorCodes.UNSUPPORTED .. line [.. main.notSupportedTooltipText]
# 永恆珠寶征服後的節點(timeless_node_stat)走同一條。期望:剝註記以 passive 查 stat,中文後接回原註記。
N4_EN = "10% increased Attack Speed"
N4_ZH = "增加 10% 攻擊速度(stat)"


def n4_tooltip(fork, stat_dict, note_on=True, define=True):
    h = stat_harness(fork, stat_dict, define)
    h.run(('colorCodes.UNSUPPORTED = "%s" main.notSupportedModTooltips = %s '
           'main.notSupportedTooltipText = "%s"') % (FR3_UNSUP, "true" if note_on else "false", FR3_NOTE))
    view = make(h, "PassiveTreeView")
    tip = make(h, "Tooltip")
    ok, err = h.eval('function(v, t, a) local ok, e = pcall(function() v:AddNodeTooltip(t, '
                     '{ dn = "Vaal Potency", id = 9001, unsupported = true, sd = { a } }, {}) '
                     'end) return ok, e end')(view, tip, N4_EN)
    return h, ok, err, [l for l in tooltip_lines(h, tip) if l is not None]


def test_n4_node_single_line_note(fork):
    """N4 -- 天賦浮窗單行不支援註記:剝註記以 passive 查 stat,中文後接回原註記與色碼。"""
    h, ok, err, lines = n4_tooltip(fork, {N4_EN: N4_ZH})
    check("%s N4 天賦浮窗本身不出錯" % fork, ok, repr(err))
    check("%s N4 以剝掉註記的英文、ctx passive 查 stat" % fork,
          (N4_EN, "passive") in [(_norm(l), c) for l, c in h.stat_calls if "Not supported" not in (l or "")],
          "calls=%r" % h.stat_calls)
    check("%s N4 命中後中文行尾接回英文註記、保留不支援色碼" % fork,
          FR3_UNSUP + N4_ZH + FR3_NOTE in lines, "lines=%r calls=%r" % (lines, h.stat_calls))
    hn, okn, errn, linesn = n4_tooltip(fork, {N4_EN: N4_ZH}, note_on=False)
    check("%s N4 註記關閉時中文行沒有註記" % fork, okn and FR3_UNSUP + N4_ZH in linesn, "lines=%r" % linesn)
    hm, okm, errm, linesm = n4_tooltip(fork, {})
    h0, ok0, err0, lines0 = n4_tooltip(fork, None, define=False)
    check("%s N4 stat 未命中時與舊引擎相同(其餘路徑照舊)" % fork,
          okm and linesm == lines0, "lines=%r 舊引擎=%r" % (linesm, lines0))


# ---- SID:有 stat id 的位置帶語境+stat id 查 stat(使用者裁定 2026-10-26「先做有 stat id 的位置」)----------
# POB 樹資料 tree.legion(nodes / additions)與 tree.tattoo.nodes 每個節點帶 stats[statId];
# 永恆珠寶清單提示、征服節點浮窗、紋身/符文下拉提示與說明標籤這幾處,說明行以 passive + 該節點的
# stat id 查 stat。假 PobToolsTranslateStat 依 (英文, ctx, ids) 回答;ids 對不到時走原本的路。
SID_DEXINT = "+(6-8) to Dexterity and Intelligence"
SID_DEXINT_ROLLED = "+7 to Dexterity and Intelligence"
SID_TATTOO = "+2 to Dexterity"
SID_HYBRID = "This is a hybrid node containing all additions to Strength"
SID_STAT = {
    (SID_DEXINT, "passive", "sid_dex_int"): "SID +(6-8) 敏捷和智慧",
    (SID_DEXINT, "passive", None): "P 增加 +(6-8) 敏捷與智慧",
    (SID_DEXINT_ROLLED, "passive", "sid_dex_int"): "SID +7 敏捷和智慧",
    (SID_DEXINT_ROLLED, "passive", None): "P 增加 +7 敏捷與智慧",
    (SID_TATTOO, "passive", "sid_tattoo_dex"): "SID +2 敏捷",
}
SID_TREETAB_METHODS = """
function C:ModifyNodePopup(selectedNode)
	local controls = { }
	local modGroups = { }
	for id, node in pairs(self.build.spec.tree.tattoo.nodes) do
		table.insert(modGroups, { label = node.dn, descriptions = copyTable(node.sd), id = id })
	end
	table.sort(modGroups, function(a, b) return a.label < b.label end)
	local function constructUI(modGroup)
		local i = 1
		while controls[i] do controls[i] = nil i = i + 1 end
		local wrapTable = {}
		for _, desc in ipairs(modGroup.descriptions) do
			for _, w in ipairs(main:WrapString(desc, 16, 375)) do table.insert(wrapTable, w) end
		end
		for idx, desc in ipairs(wrapTable) do controls[idx] = { label = "^7"..desc } end
	end
	controls.modSelect = { list = modGroups, selIndex = 1, Draw = function() end,
		selFunc = function(idx) constructUI(modGroups[idx]) end }
	controls.modSelect.tooltipFunc = function(tooltip, mode, index, value)
		tooltip:Clear()
		if mode ~= "OUT" and value then
			for _, line in ipairs(value.descriptions) do tooltip:AddLine(16, "^7"..line) end
		end
	end
	main.popups[1] = { controls = controls }
	constructUI(modGroups[1])
end
function C:FindTimelessJewel()
	local controls = { }
	local modData = { }
	local tree = self.build.spec.tree
	for _, node in pairs(tree.legion.nodes) do
		table.insert(modData, { label = node.dn, descriptions = copyTable(node.sd), id = node.id })
	end
	for _, addition in pairs(tree.legion.additions) do
		table.insert(modData, { label = addition.dn, descriptions = copyTable(addition.sd), id = addition.id })
	end
	table.sort(modData, function(a, b) return a.label < b.label end)
	table.insert(modData, 1, { label = "Total Strength", descriptions = { "%(hybrid)s" }, id = "total_strength" })
	controls.nodeSelect = { list = modData, selIndex = 1, Draw = function() end }
	controls.nodeSelect.tooltipFunc = function(tooltip, mode, index, value)
		tooltip:Clear()
		if mode ~= "OUT" and value.descriptions then
			for _, line in ipairs(value.descriptions) do
				tooltip:AddLine(16, "^7" .. line)
			end
		end
	end
	main.popups[1] = { controls = controls }
end
""" % {"hybrid": SID_HYBRID}
SID_TREE = """{
	nodes = { [4242] = { dn = "Some Notable", sd = { "Zzsid original line" } } },
	legion = {
		nodes = { { id = "lethal_pride_notable_dexint", dn = "Hit and Run", ["not"] = true,
			sd = { "%(dexint)s" }, stats = { sid_dex_int = { min = 6, max = 8, index = 1 } } } },
		additions = { { id = "lethal_pride_add_other", dn = "Other", sd = { "Zzsid other line" },
			stats = { sid_other = { min = 1, max = 1, index = 1 } } } },
	},
	tattoo = { nodes = { ["Tattoo of the Sid"] = { dn = "Tattoo of the Sid", sd = { "%(tattoo)s" },
		stats = { sid_tattoo_dex = { min = 2, max = 2 } } } } },
}""" % {"dexint": SID_DEXINT, "tattoo": SID_TATTOO}


def sid_harness(fork, stat_answers, define=True, with_inject=True):
    """有 stat id 的彈窗/浮窗。h.stat_calls 記 (英文, ctx, ids);stat_answers 鍵 (英文本文, ctx, ids 或 None)。"""
    h = Harness(fork, with_inject=False)
    h.loose_keys = True
    h.stat_calls = []

    def translate_stat(line, ctx=None, ids=None):
        h.stat_calls.append((line, ctx, ids))
        if line is None:
            return None
        m = re.match(r"^((?:\^x[0-9A-Fa-f]{6}|\^\d)*)(.*)$", line, re.S)
        hit = (stat_answers or {}).get((m.group(2).strip(), ctx, ids))
        return None if hit is None else m.group(1) + hit

    if define:
        h.rt.globals()["__py_sid_stat"] = translate_stat
        h.run(b"function PobToolsTranslateStat(l, c, i) return __py_sid_stat(l, c, i) end")
    h.run(COLOUR_SETUP)
    h.run(('main.notSupportedModTooltips = true main.notSupportedTooltipText = "%s" '
           'function main:WrapString(str, height, width) return { str } end') % GR_NOTE)
    h.run('LoadModule("Classes/Tooltip")')
    h.define("PassiveTreeView", 'local C = newClass("PassiveTreeView")\nfunction C:PassiveTreeView() return self end\n'
             + GR_TREE_METHODS, 'local C = newClass("PassiveTreeView", function(self) end)\n' + GR_TREE_METHODS)
    h.define("TreeTab", 'local C = newClass("TreeTab")\nfunction C:TreeTab() return self end\n' + SID_TREETAB_METHODS,
             'local C = newClass("TreeTab", function(self) end)\n' + SID_TREETAB_METHODS)
    if with_inject:
        h.apply_inject()
    return h


def sid_tree_tab(h):
    tab = make(h, "TreeTab")
    tree = h.eval("function() return %s end" % SID_TREE)()
    h.eval("function(tab, tree) tab.build = { spec = { tree = tree } } end")(tab, tree)
    return tab, tree


def sid_popup_tip(fork, method, control, stat_answers, define=True, with_inject=True):
    """開彈窗後對下拉每一項跑 tooltipFunc;回 (h, ok, err, [[各項的提示行]])。"""
    h = sid_harness(fork, stat_answers, define, with_inject)
    tab, _ = sid_tree_tab(h)
    ok, err = h.eval('function(tab, m) local ok, e = pcall(function() tab[m](tab, { id = 1 }) end) '
                     'return ok, e end')(tab, method)
    tips = []
    n = h.eval('function(c) return #main.popups[1].controls[c].list end')(control) if ok else 0
    for i in range(1, n + 1):
        tip = make(h, "Tooltip")
        okT, errT = h.eval('function(t, c, i) local dd = main.popups[1].controls[c] local ok, e = pcall(function() '
                           'dd.tooltipFunc(t, "HOVER", i, dd.list[i]) end) return ok, e end')(tip, control, i)
        ok, err = ok and okT, err or errT
        tips.append([l for l in tooltip_lines(h, tip) if l is not None])
    return h, ok, err, tips


def sid_labels(h):
    labels = h.eval('function() local c = main.popups[1].controls local o = {} '
                    'local i = 1 while c[i] do o[i] = c[i].label i = i + 1 end return o end')()
    return [labels[i] for i in range(1, len(labels) + 1)]


def sid_node_tip(fork, sd_line, conquered, stat_answers, define=True):
    h = sid_harness(fork, stat_answers, define)
    view = make(h, "PassiveTreeView")
    tip = make(h, "Tooltip")
    _, tree = sid_tree_tab(h)
    ok, err = h.eval('function(v, t, tree, s, c) local ok, e = pcall(function() v:AddNodeTooltip(t, '
                     '{ dn = "Some Notable", id = 4242, sd = { s }, conqueredBy = c and { id = 1, conqueror = '
                     '{ type = "karui", id = 1 } } or nil }, { spec = { tree = tree } }) end) return ok, e end')(
        view, tip, tree, sd_line, conquered)
    return h, ok, err, [l for l in tooltip_lines(h, tip) if l is not None]


def test_sid_stat_id_positions(fork):
    """SID1 ~ SID6 -- 永恆珠寶清單提示 / 征服節點浮窗 / 紋身下拉提示與說明標籤:passive + 節點 stat id 查 stat。"""
    # SID1 永恆珠寶清單提示:每項以該 legion 節點(或附加)的 stat id 查
    h, ok, err, tips = sid_popup_tip(fork, "FindTimelessJewel", "nodeSelect", SID_STAT)
    flat = [_norm(l) for t in tips for l in t]
    check("%s SID1 永恆珠寶清單提示本身不出錯" % fork, ok, repr(err))
    check("%s SID1 清單提示說明行帶 passive 與該節點的 stat id 查 stat" % fork,
          (SID_DEXINT, "passive", "sid_dex_int") in [(_norm(l), c, i) for l, c, i in h.stat_calls],
          "calls=%r" % h.stat_calls)
    check("%s SID1 清單提示顯示帶 id 的答案(不是不帶 id 的)" % fork,
          "SID +(6-8) 敏捷和智慧" in flat and not any(x.startswith("P ") for x in flat), "tips=%r" % tips)
    check("%s SID1 不是樹資料 stat 行的說明(合成節點說明)不帶 id 查 stat" % fork,
          not any(_norm(l) == SID_HYBRID and i for l, c, i in h.stat_calls), "calls=%r" % h.stat_calls)
    # SID5 帶 id 查不到:與沒有 PobToolsTranslateStat 的舊引擎畫面相同(照原本的路)
    hm, okm, errm, tipsm = sid_popup_tip(fork, "FindTimelessJewel", "nodeSelect", {})
    h0, ok0, err0, tips0 = sid_popup_tip(fork, "FindTimelessJewel", "nodeSelect", None, define=False)
    check("%s SID5 清單提示帶 id 查不到時照原本的路(與舊引擎相同)" % fork,
          okm and ok0 and tipsm == tips0, "tips=%r 舊引擎=%r" % (tipsm, tips0))
    # SID2 征服節點浮窗:sd 行(值已擲出)依樹資料樣板找到 stat id
    h, ok, err, lines = sid_node_tip(fork, SID_DEXINT_ROLLED, True, SID_STAT)
    check("%s SID2 征服節點浮窗本身不出錯" % fork, ok, repr(err))
    check("%s SID2 征服節點說明行以 passive + 樹資料的 stat id 查(擲出的值也對得到樣板)" % fork,
          (SID_DEXINT_ROLLED, "passive", "sid_dex_int") in [(_norm(l), c, i) for l, c, i in h.stat_calls],
          "calls=%r" % h.stat_calls)
    check("%s SID2 征服節點顯示帶 id 的答案" % fork, "SID +7 敏捷和智慧" in [_norm(l) for l in lines], "lines=%r" % lines)
    hn, okn, errn, linesn = sid_node_tip(fork, SID_DEXINT_ROLLED, False, SID_STAT)
    check("%s SID2 沒被征服的節點不帶 id(照 N4 的 passive 查表)" % fork,
          okn and "P 增加 +7 敏捷與智慧" in [_norm(l) for l in linesn]
          and not any(i for l, c, i in hn.stat_calls), "lines=%r calls=%r" % (linesn, hn.stat_calls))
    hx, okx, errx, linesx = sid_node_tip(fork, SID_DEXINT_ROLLED, True,
                                         {k: v for k, v in SID_STAT.items() if k[2] is None})
    check("%s SID5 征服節點帶 id 查不到時退回不帶 id 的 passive 查表(現行答案)" % fork,
          okx and "P 增加 +7 敏捷與智慧" in [_norm(l) for l in linesx], "lines=%r calls=%r" % (linesx, hx.stat_calls))
    # SID3 紋身下拉提示 / SID4 說明標籤:以該紋身節點的 stat id 查
    h, ok, err, tips = sid_popup_tip(fork, "ModifyNodePopup", "modSelect", SID_STAT)
    check("%s SID3 紋身彈窗本身不出錯" % fork, ok, repr(err))
    check("%s SID3 紋身下拉提示帶 passive 與紋身的 stat id 查 stat" % fork,
          ok and [_norm(l) for l in tips[0]] == ["SID +2 敏捷"]
          and (SID_TATTOO, "passive", "sid_tattoo_dex") in [(_norm(l), c, i) for l, c, i in h.stat_calls],
          "tips=%r calls=%r" % (tips, h.stat_calls))
    check("%s SID4 紋身說明標籤帶 stat id 查 stat" % fork, sid_labels(h) == ["^7SID +2 敏捷"], repr(sid_labels(h)))
    okS, errS = h.eval('function() local ok, e = pcall(function() local dd = main.popups[1].controls.modSelect '
                       'dd.selFunc(1, dd.list[1]) end) return ok, e end')()
    check("%s SID4 換選取(selFunc)重建說明標籤仍帶 stat id" % fork,
          okS and sid_labels(h) == ["^7SID +2 敏捷"], "labels=%r %r" % (sid_labels(h), errS))
    after = h.eval('function(s) return main:WrapString(s, 16, 375)[1] end')(SID_TATTOO)
    check("%s SID4 彈窗建完後 WrapString 不再帶 stat id 查" % fork, after == SID_TATTOO, repr(after))
    # SID6 沒有樹資料(tree 不帶 legion/tattoo)時不報錯、不帶 id
    hz = sid_harness(fork, SID_STAT)
    view = make(hz, "PassiveTreeView")
    tip = make(hz, "Tooltip")
    okz, errz = hz.eval('function(v, t, s) local ok, e = pcall(function() v:AddNodeTooltip(t, '
                        '{ dn = "N", id = 1, sd = { s }, conqueredBy = { id = 1 } }, {}) end) return ok, e end')(
        view, tip, SID_DEXINT_ROLLED)
    check("%s SID6 征服節點但拿不到樹資料:不報錯、不帶 id" % fork,
          okz and not any(i for l, c, i in hz.stat_calls), "err=%r calls=%r" % (errz, hz.stat_calls))


# SID7(審查第 10 輪):被征服節點「原本就有」的說明行不帶 legion 的 stat id。
# 情境 = Divine Fervour(樹節點 30693,原有「+10 to Strength and Intelligence」)被征服、附加一行同樣板的 legion 行:
# 原行照 passive 查表(不帶 id),只有附加/取代進來的行帶 legion id。
SID7_ORIG = "+10 to Strength and Intelligence"
SID7_ADD = "+3 to Strength and Intelligence"
SID7_STAT = {
    (SID7_ORIG, "passive", None): "增加 +10 力量與智慧",
    (SID7_ORIG, "passive", "sid_add_strint"): "+10 力量和智慧(legion id)",
    (SID7_ADD, "passive", None): "增加 +3 力量與智慧",
    (SID7_ADD, "passive", "sid_add_strint"): "SID +3 力量和智慧",
}
SID7_TREE = """{
	nodes = { [30693] = { dn = "Divine Fervour", sd = { "%(orig)s" } } },
	legion = {
		nodes = {},
		additions = { { id = "karui_add_strint", dn = "Strength and Intelligence",
			sd = { "+(2-4) to Strength and Intelligence" }, stats = { sid_add_strint = { min = 2, max = 4, index = 1 } } } },
	},
	tattoo = { nodes = {} },
}""" % {"orig": SID7_ORIG}


def test_sid_conquered_original_lines(fork):
    """SID7 -- 被征服節點原有的說明行照 passive 查表(不帶 legion id),附加的 legion 行才帶 id。"""
    h = sid_harness(fork, SID7_STAT)
    view = make(h, "PassiveTreeView")
    tip = make(h, "Tooltip")
    tree = h.eval("function() return %s end" % SID7_TREE)()
    ok, err = h.eval('function(v, t, tree, a, b) local ok, e = pcall(function() v:AddNodeTooltip(t, '
                     '{ dn = "Divine Fervour", id = 30693, sd = { a, b }, conqueredBy = { id = 1, conqueror = '
                     '{ type = "karui", id = 1 } } }, { spec = { tree = tree } }) end) return ok, e end')(
        view, tip, tree, SID7_ORIG, SID7_ADD)
    lines = [_norm(l) for l in tooltip_lines(h, tip) if l is not None]
    calls = [(_norm(l), c, i) for l, c, i in h.stat_calls]
    check("%s SID7 征服節點浮窗本身不出錯" % fork, ok, repr(err))
    check("%s SID7 節點原有的行不帶 legion id、照 passive 查表(與沒插珠寶同字)" % fork,
          "增加 +10 力量與智慧" in lines and (SID7_ORIG, "passive", "sid_add_strint") not in calls,
          "lines=%r calls=%r" % (lines, calls))
    check("%s SID7 附加進來的 legion 行帶 legion id" % fork,
          "SID +3 力量和智慧" in lines, "lines=%r calls=%r" % (lines, calls))


# ---- FL:傳奇風味文字維持英文(使用者裁定 2026-10-24「傳奇風味文字不要」、2026-10-27 現在修)----------
# 上游 ItemsTab:AddItemTooltip 的風味文字段:每行 AddLine(size, colorCodes.UNIQUE .. line, "FONTIN SC ITALIC")
# (物品浮窗裡唯一的斜體行)。期望:傳奇/遺物物品的這些行不翻,並向引擎登記整串維持英文(繪製時也不翻)。
FL_LINE = "A lightless world"
DICT[None]["A lightless world"] = "A 無光芒 世界地圖"


def fl_tooltip(fork, rarity, define_keep=True, other=False):
    """回 (h, ok, err, 物品浮窗各行, 登記維持英文的字串)。other=True 時同一句改當一般行(非斜體)畫。"""
    h = stat_harness(fork, {})
    kept = []
    if define_keep:
        h.rt.globals()["__py_keep"] = lambda s: kept.append(s)
        h.run(b"function PobToolsKeepEnglish(s) __py_keep(s) end")
    tab = make(h, "ItemsTab")
    tip = make(h, "Tooltip")
    item = h.eval('function(r, l, o) local it = { rarity = r, title = "Crown of the Pale King", '
                  'baseName = "Regicide Mask", base = {}, name = "Crown of the Pale King, Regicide Mask" } '
                  'if o then it.otherLines = { colorCodes.UNIQUE .. l } else it.flavourLines = { l } end '
                  'return it end')(rarity, FL_LINE, other)
    ok, err = h.eval('function(tab, t, item) local ok, e = pcall(function() '
                     'tab:AddItemTooltip(t, item) end) return ok, e end')(tab, tip, item)
    return h, ok, err, [l for l in tooltip_lines(h, tip) if l is not None], kept


FL_WRAPPED = "Line one of zzqfl / line two of zzqfl"
DICT[None]["line two of zzqfl"] = "第二段"


def fl_wrapped(fork):
    """長風味行被 WrapString 切成兩段:回 (h, ok, err, 全域登記, 繪製時每段的 (文字, 當下範圍), 繪製後範圍, Clear 後繪製的範圍)。"""
    h = stat_harness(fork, {})
    kept, scope_log = [], []
    state = {"scope": []}

    def set_scope(tbl=None):
        state["scope"] = [tbl[i] for i in range(1, len(tbl) + 1)] if tbl is not None else []
    h.rt.globals()["__py_keep"] = lambda s: kept.append(s)
    h.rt.globals()["__py_scope"] = set_scope
    h.rt.globals()["__py_draw"] = lambda t: scope_log.append((t, list(state["scope"])))
    h.run(b"function PobToolsKeepEnglish(s) __py_keep(s) end "
          b"function PobToolsSetKeepEnglishScope(t) __py_scope(t) end "
          b"function DrawString(x, y, a, h, f, t) __py_draw(t) end "
          b"function main:WrapString(str) local o = {} for piece in (str .. ' / '):gmatch('(.-) / ') do o[#o + 1] = piece end return o end")
    tab = make(h, "ItemsTab")
    tip = make(h, "Tooltip")
    item = h.eval('function(l) return { rarity = "UNIQUE", title = "Crown of the Pale King", baseName = "Regicide Mask", '
                  'base = {}, name = "Crown of the Pale King, Regicide Mask", flavourLines = { l } } end')(FL_WRAPPED)
    ok, err = h.eval('function(tab, t, item) t.maxWidth = 500 local ok, e = pcall(function() '
                     'tab:AddItemTooltip(t, item) t:Draw(0, 0, nil, nil, { x = 0, y = 0, width = 1920, height = 1080 }) '
                     'end) return ok, e end')(tab, tip, item)
    drawn = list(scope_log)
    after = list(state["scope"])
    del scope_log[:]
    ok2, err2 = h.eval('function(t) local ok, e = pcall(function() t:Clear() t:AddLine(16, "^7plain") '
                       't:Draw(0, 0, nil, nil, { x = 0, y = 0, width = 1920, height = 1080 }) end) return ok, e end')(tip)
    return h, ok and ok2, err or err2, kept, drawn, after, list(scope_log)


def test_fl_wrapped_scope(fork):
    """FL5 ~ FL6 -- 風味行斷行後:帶色碼的整行/首段全域登記;無色碼的後續段只在該浮窗繪製期間維持英文。"""
    h, ok, err, kept, drawn, after, cleared = fl_wrapped(fork)
    check("%s FL5 斷行的風味浮窗本身不出錯(含繪製)" % fork, ok, repr(err))
    check("%s FL5 全域登記只有帶色碼的整行與首段,不含無色碼的後續段" % fork,
          ok and "^xAF6025" + FL_WRAPPED in kept and "^xAF6025Line one of zzqfl" in kept
          and all(k.startswith("^x") for k in kept), "kept=%r" % kept)
    piece = [sc for t, sc in drawn if t == "line two of zzqfl"]
    check("%s FL6 後續段在該浮窗繪製時位於維持英文範圍內" % fork,
          ok and piece and "line two of zzqfl" in piece[0], "drawn=%r" % drawn)
    check("%s FL6 繪製結束後範圍清空(不影響別處同字串)" % fork, ok and after == [], "after=%r" % after)
    check("%s FL6 浮窗 Clear 重建後不再帶舊範圍" % fork,
          ok and all("line two of zzqfl" not in sc for t, sc in cleared), "cleared=%r" % cleared)


def test_fl_unique_flavour_stays_english(fork):
    """FL1 ~ FL4 -- 傳奇物品浮窗的風味文字行維持英文並登記;非傳奇物品、非風味行照舊翻。"""
    want_en = "^xAF6025" + FL_LINE
    h, ok, err, lines, kept = fl_tooltip(fork, "UNIQUE")
    check("%s FL1 傳奇物品浮窗本身不出錯" % fork, ok, repr(err))
    check("%s FL1 傳奇風味文字行維持英文" % fork, want_en in lines, "lines=%r" % lines)
    check("%s FL1 該行整串向引擎登記維持英文(繪製時也不翻)" % fork, kept == [want_en], "kept=%r" % kept)
    hr, okr, errr, linesr, keptr = fl_tooltip(fork, "RELIC")
    check("%s FL1 遺物物品同樣維持英文" % fork, okr and want_en in linesr and keptr == [want_en],
          "lines=%r kept=%r" % (linesr, keptr))
    h2, ok2, err2, lines2, kept2 = fl_tooltip(fork, "RARE")
    check("%s FL2 非傳奇物品的斜體行照舊翻譯(不登記)" % fork,
          ok2 and "^xAF6025A 無光芒 世界地圖" in lines2 and not kept2, "lines=%r kept=%r" % (lines2, kept2))
    h3, ok3, err3, lines3, kept3 = fl_tooltip(fork, "UNIQUE", other=True)
    check("%s FL3 傳奇物品的一般行(非斜體)照舊翻譯" % fork,
          ok3 and "^xAF6025A 無光芒 世界地圖" in lines3 and not kept3, "lines=%r kept=%r" % (lines3, kept3))
    h4, ok4, err4, lines4, kept4 = fl_tooltip(fork, "UNIQUE", define_keep=False)
    check("%s FL4 舊引擎(無 PobToolsKeepEnglish)風味行仍不翻、不報錯" % fork,
          ok4 and want_en in lines4, "lines=%r err=%r" % (lines4, err4))


# ---- R15:下拉列改用語言無關 id(使用者裁定 2026-10-28「更改」)----------------------------------
# DO:有 _ptRowZh 的下拉在自己繪製期間把「該列英文 → 依 id 翻的中文」交給引擎覆寫表;浮窗不帶。
# CR/PM:製作詞綴下拉(modList)、腐化 / 自訂 / 異能彈窗(mod 表 → Mods.Id)以詞綴的 stat id 翻整列。
# GN:寶石清單列名以 GrantedEffects id 取;AS:昇華下拉以 Ascendancy id 取;
# CT:星團詞綴浮窗的大天賦說明行以該天賦 stat id 走 passive;TA:紋身隱藏段先走 stat id。
R15_IDS = {
    "mod_stats": {"V2MaxPowerChargesCorrupted": "max_power_charges",
                  "OnslaughtOnKillInfluence1": "chance_to_gain_onslaught_on_kill_%",
                  "OnslaughtOnKillInfluence2_": "chance_to_gain_onslaught_on_kill_%",
                  "DelveWeaponChanceToGainOnslaughtOnKill2h1": "chance_to_gain_onslaught_on_kill_%",
                  "ScorchedGroundWhileMovingEldritchImplicit3": "boots_implicit_ground_scorched_duration_ms"},
    "passive_stats": {"42687": "base_dexterity_and_intelligence two_handed_melee_attack_speed_+%"},
    "gem_names": {"GemDetonateMines": "遙控地雷"},
    "ascendancy": {"Raider": "守林人"},
}
R15_TPL = {"gem_names": {"GemDetonateMines": "Detonate Mines"}, "ascendancy": {"Raider": "Warden"}}
R15_ONS = "#% chance to gain Onslaught for # seconds on Kill"
R15_DELVE = "20% chance to gain Onslaught for 4 seconds on Kill (Suffix)"
R15_SCORCH = "Drops Scorched Ground while moving, lasting 4 seconds"
R15_ROWS = {
    ("+1 to Maximum Power Charges", "item", "max_power_charges"): "+1 最大暴擊球",
    (R15_ONS, "item", "chance_to_gain_onslaught_on_kill_%"): "擊殺時 #% 機率獲得 # 秒猛攻",
    (R15_DELVE, "item", "chance_to_gain_onslaught_on_kill_%"): "擊殺時 20% 機率獲得 4 秒猛攻 (後綴)",
    (R15_SCORCH, "item", "boots_implicit_ground_scorched_duration_ms"): "移動時留下焦灼地面，持續 4 秒",
    ("Zzq two line row", "item", "max_power_charges"): "若你近期有攻擊，你和\n附近友方 +#% 格擋率",
}
R15_STAT = {
    ("+15 to Dexterity and Intelligence", "passive", "base_dexterity_and_intelligence two_handed_melee_attack_speed_+%"):
        "增加 +15 敏捷與智慧",
    ("Skills fire an additional Projectile", "passive", "number_of_additional_projectiles"): "技能有額外一個投射物",
    ("Zzq trigger line", "passive", "zzq_trigger"): "有傳奇敵人時，\n觸發 10%\n技能",
}
DICT[None]["Limited to 1 Zzq Tattoo"] = "限制 1 個 Zzq 紋身"
DICT[None].update({"+1 to Maximum Power Charges": "+1 最大暴擊球數量", R15_SCORCH: "移動時掉落焦灼地面，持續 4 秒",
                   "+15 to Dexterity and Intelligence": "+15 點敏捷與智慧",
                   "Skills fire an additional Projectile": "技能發射 1 個額外投射物",
                   "Warden": "守護者", "Detonate Mines": "引爆地雷"})

R15_DD_BODY = """
function C:Draw(viewPort)
	local sel = self.list[self.selIndex]
	DrawString(0, 0, "LEFT", 16, "VAR", type(sel) == "table" and sel.label or sel or "")
	self:DrawTooltip()
	if self.dropped then
		for _, v in ipairs(self.list) do DrawString(0, 0, "LEFT", 16, "VAR", type(v) == "table" and v.label or v) end
	end
end
function C:DrawTooltip()
	local sel = self.list[self.selIndex]
	if type(sel) == "table" then DrawString(0, 0, "LEFT", 16, "VAR", sel.label) end
end
function C:CheckDroppedWidth()
	local w = 0
	for _, v in ipairs(self.list) do w = math.max(w, DrawStringWidth(16, "VAR", type(v) == "table" and v.label or v)) end
	self.droppedWidth = w
end
"""
R15_DD_P1 = """
local C = newClass("DropDownControl")
function C:DropDownControl(anchor, rect, list, selFunc)
	self.list = list self.selFunc = selFunc self.selIndex = 1
	return self
end
""" + R15_DD_BODY
R15_DD_P2 = """
local C = newClass("DropDownControl", function(self, anchor, rect, list, selFunc)
	self.list = list self.selFunc = selFunc self.selIndex = 1
end)
""" + R15_DD_BODY

R15_SETUP = """
function r15NewDD(list)
	if R15_FORK == "PoE1" then return new("DropDownControl"):DropDownControl(nil, nil, list) end
	return new("DropDownControl", nil, nil, list)
end
data = { itemMods = {
	Corrupted = { V2MaxPowerChargesCorrupted = { "+1 to Maximum Power Charges", type = "Corrupted" },
		ZzqNoStats = { "+1 to Zzq Things", type = "Corrupted" } },
	Delve = { DelveWeaponChanceToGainOnslaughtOnKill2h1 = { "20% chance to gain Onslaught for 4 seconds on Kill", type = "Suffix" } },
	Eldritch = { ScorchedGroundWhileMovingEldritchImplicit3 = { "Drops Scorched Ground while moving, lasting 4 seconds", type = "Exarch" } },
	JewelCluster = { AfflictionNotableGracefulExecution_ = { "1 Added Passive Skill is Graceful Execution", type = "Prefix" } },
} }
r15Affixes = {
	OnslaughtOnKillInfluence1 = { "(5-7)% chance to gain Onslaught for 4 seconds on Kill" },
	OnslaughtOnKillInfluence2_ = { "(8-10)% chance to gain Onslaught for 4 seconds on Kill" },
	Life1 = { "+20 to maximum Life" },
	AfflictionNotableGracefulExecution_ = data.itemMods.JewelCluster.AfflictionNotableGracefulExecution_,
}
r15Build = { spec = { tree = { clusterNodeMap = { ["Graceful Execution"] = { id = 42687, dn = "Graceful Execution",
	sd = { "+15 to Dexterity and Intelligence" } } } } } }
package.loaded["Modules.AddImplicitPopup"] = { AddImplicitToDisplayItem = function(itemsTab, item)
	local controls = { modSelect = r15NewDD({ { label = "Drops Scorched Ground while moving, lasting 4 seconds",
		mod = data.itemMods.Eldritch.ScorchedGroundWhileMovingEldritchImplicit3 } }) }
	main:OpenPopup(100, 100, "Add Implicit", controls)
end }
main.modes = { BUILD = {} }
function main.modes.BUILD:Init()
	self.spec = { curClassId = 2, tree = { classes = { [2] = { name = "Ranger", classes = {
		[0] = { name = "None" }, [1] = { id = "Raider", name = "Warden" }, [2] = { id = "Deadeye", name = "Deadeye" } } } } } }
	self.controls = { ascendDrop = r15NewDD({ { label = "None", ascendClassId = 0 }, { label = "Warden", ascendClassId = 1 },
		{ label = "Deadeye", ascendClassId = 2 } }) }
end
"""
R15_ITEMSTAB_METHODS = """
function C:R15Init()
	self.build = r15Build
	self.displayItem = { affixes = r15Affixes }
	self.controls = {}
	local drop = r15NewDD({ "None",
		{ label = "#% chance to gain Onslaught for # seconds on Kill", modList = { "OnslaughtOnKillInfluence1", "OnslaughtOnKillInfluence2_" } },
		{ label = "+20 to maximum Life", modList = { "Life1" } },
		{ label = "1 Added Passive Skill is Graceful Execution", modList = { "AfflictionNotableGracefulExecution_" } } })
	drop.tooltipFunc = function(tooltip, mode, index, value)
		local mod = self.displayItem.affixes[value.modId or value.modList[1]]
		for _, line in ipairs(mod) do tooltip:AddLine(14, "^7" .. line) end
		local notableName = mod[1] and mod[1]:match("1 Added Passive Skill is (.*)")
		local node = notableName and self.build.spec.tree.clusterNodeMap[notableName]
		if node then
			for _, line in ipairs(node.sd) do tooltip:AddLine(16, colorCodes.MAGIC .. line) end
		end
	end
	self.controls.displayItemAffix1 = drop
end
function C:CorruptDisplayItem()
	local controls = {
		implicit1 = r15NewDD({ { label = "None" },
			{ label = "+1 to Maximum Power Charges", mod = data.itemMods.Corrupted.V2MaxPowerChargesCorrupted },
			{ label = "+1 to Zzq Things", mod = data.itemMods.Corrupted.ZzqNoStats } }),
		implicit2 = r15NewDD({ { label = "None" },
			{ label = "+1 to Maximum Power Charges", mod = data.itemMods.Corrupted.V2MaxPowerChargesCorrupted } }),
	}
	main:OpenPopup(100, 100, "Corrupt Item", controls)
end
function C:AddCustomModifierToDisplayItem()
	local controls = { modSelect = r15NewDD({ { label = "20% chance to gain Onslaught for 4 seconds on Kill (Suffix)",
		mod = data.itemMods.Delve.DelveWeaponChanceToGainOnslaughtOnKill2h1 } }) }
	main:OpenPopup(100, 100, "Add Modifier", controls)
end
"""
R15_ITEMSTAB_P1 = """
local C = newClass("ItemsTab")
function C:ItemsTab() self:R15Init() return self end
""" + R15_ITEMSTAB_METHODS
R15_ITEMSTAB_P2 = """
local C = newClass("ItemsTab", function(self) self:R15Init() end)
""" + R15_ITEMSTAB_METHODS


def r15_harness(fork, define_api=True, with_inject=True, ids=None, game=None):
    """R15 下拉 / 彈窗 / 昇華 / 星團浮窗。h.r15 記覆寫表、畫出的字、量寬、stat 查詢。"""
    h = Harness(fork, with_inject=False, game=game)
    h.loose_keys = True
    st = {"override": None, "drawn": [], "widths": [], "sets": 0, "rows": [], "stats": []}
    h.r15 = st
    id_tables = R15_IDS if ids is None else ids

    def set_override(t=None):
        st["sets"] += 1
        st["override"] = dict(t.items()) if t is not None else None

    def shown(t):
        ov = st["override"] or {}
        return ov.get(t, t)

    def draw(t):
        st["drawn"].append((shown(t), st["override"] is not None))

    def width(t):
        s = shown(t)
        st["widths"].append(s)
        return len(s)

    def stat_row(row, ctx=None, ids_=None):
        st["rows"].append((row, ctx, ids_))
        return R15_ROWS.get((row, ctx, ids_))

    def translate_stat(line, ctx=None, ids_=None):
        st["stats"].append((line, ctx, ids_))
        m = re.match(r"^((?:\^x[0-9A-Fa-f]{6}|\^\d)*)(.*)$", line or "", re.S)
        hit = R15_STAT.get((m.group(2).strip(), ctx, ids_))
        return None if hit is None else m.group(1) + hit

    def translate_id(source, id_, en=None):
        if isinstance(id_, float) and id_.is_integer():
            id_ = int(id_)
        key = None if id_ is None else str(id_)
        if not h.enabled:
            return None
        hit = (id_tables.get(source) or {}).get(key)
        tpl = (R15_TPL.get(source) or {}).get(key)
        if hit is not None and (en is None or tpl is None or tpl == en):
            return hit
        if en is None:
            return None
        prev = h.source
        h.source = source
        try:
            return DICT.get(source, {}).get(en) or DICT[None].get(en)
        finally:
            h.source = prev
    g = h.rt.globals()
    g["__py_r15_draw"] = draw
    g["__py_r15_width"] = width
    g["__py_r15_set"] = set_override
    g["__py_r15_row"] = stat_row
    g["__py_r15_stat"] = translate_stat
    g["__py_r15_id"] = translate_id
    g["R15_FORK"] = fork
    h.run(b"function DrawString(x, y, a, hh, f, t) __py_r15_draw(t) end "
          b"function DrawStringWidth(hh, f, t) return __py_r15_width(t) end "
          b"function PobToolsTranslateId(s, id, en) return __py_r15_id(s, id, en) end "
          b"function PobToolsTranslateStat(l, c, i) return __py_r15_stat(l, c, i) end")
    if define_api:
        h.run(b"function PobToolsSetDrawOverride(t) __py_r15_set(t) end "
              b"function PobToolsTranslateStatRow(r, c, i) return __py_r15_row(r, c, i) end")
    h.run(COLOUR_SETUP)
    h.run(POPUP_SETUP)
    h.run(R15_SETUP)
    h.run('LoadModule("Classes/Tooltip")')
    h.define("DropDownControl", R15_DD_P1, R15_DD_P2)
    h.define("ItemsTab", R15_ITEMSTAB_P1, R15_ITEMSTAB_P2)
    if with_inject:
        h.apply_inject()
    return h


def r15_draw(h, expr, dropped):
    """畫一次下拉;回 (ok, err, [(畫出的字, 當下有無覆寫)], 畫完後覆寫表)。"""
    del h.r15["drawn"][:]
    ok, err = h.eval('function(d) local dd = %s local ok, e = pcall(function() dd.dropped = d dd:Draw({}) end) '
                     'return ok, e end' % expr)(dropped)
    return ok, err, list(h.r15["drawn"]), h.r15["override"]


def test_r15_dropdown_override(fork):
    """DO1 ~ DO6 -- 有 _ptRowZh 的下拉繪製期間依 id 翻好的列畫中文;浮窗、其他下拉、F2 關閉都照舊。"""
    h = r15_harness(fork)
    tab = make(h, "ItemsTab")
    h.eval('function(t) t:CorruptDisplayItem() end')(tab)
    h.eval('function() main.popups[1].controls.implicit1.selIndex = 2 end')()
    ok, err, drawn, after = r15_draw(h, "main.popups[1].controls.implicit1", False)
    check("%s DO1 關閉的下拉:選中列畫成依 stat id 翻的中文" % fork,
          ok and drawn[:1] == [("+1 最大暴擊球", True)], "drawn=%r err=%r" % (drawn, err))
    check("%s DO2 下拉畫的浮窗不帶覆寫表(浮窗行有自己的翻譯路徑)" % fork,
          ok and len(drawn) > 1 and drawn[1] == ("+1 to Maximum Power Charges", False), "drawn=%r" % drawn)
    check("%s DO3 畫完覆寫表清掉" % fork, ok and after is None, "after=%r" % after)
    ok, err, drawn, after = r15_draw(h, "main.popups[1].controls.implicit1", True)
    rows = [t for t, o in drawn[2:]]
    check("%s DO4 展開清單:有 stat id 的列中文、沒有的列照舊英文" % fork,
          ok and rows == ["None", "+1 最大暴擊球", "+1 to Zzq Things"], "rows=%r err=%r" % (rows, err))
    del h.r15["widths"][:]
    h.eval('function() main.popups[1].controls.implicit1:CheckDroppedWidth() end')()
    check("%s DO5 量清單寬度時也照覆寫後的字量" % fork, "+1 最大暴擊球" in h.r15["widths"], "widths=%r" % h.r15["widths"])
    sets = h.r15["sets"]
    ok, err, drawn, after = r15_draw(h, "r15NewDD({ { label = '+1 to Maximum Power Charges', mod = data.itemMods.Corrupted.V2MaxPowerChargesCorrupted } })", True)
    check("%s DO6 沒設 _ptRowZh 的下拉不碰覆寫表" % fork,
          ok and h.r15["sets"] == sets and all(t == "+1 to Maximum Power Charges" for t, o in drawn), "drawn=%r" % drawn)
    h.enabled = False
    sets = h.r15["sets"]
    ok, err, drawn, after = r15_draw(h, "main.popups[1].controls.implicit1", False)
    check("%s DO7 F2 關閉時不設覆寫表(畫英文)" % fork,
          ok and h.r15["sets"] == sets and drawn[:1] == [("+1 to Maximum Power Charges", False)], "drawn=%r" % drawn)
    h.enabled = True
    ok, err, drawn, _ = r15_draw(h, "(function() local d = main.popups[1].controls.implicit1 d.list = { "
                         "{ label = '+1 to Maximum Power Charges', mod = data.itemMods.Corrupted.V2MaxPowerChargesCorrupted }, "
                         "{ label = '+1 to Maximum Power Charges', mod = data.itemMods.Corrupted.ZzqNoStats } } d.selIndex = 1 return d end)()", True)
    check("%s DO9 同英文的兩列一列有 id 譯文、一列沒有:兩列都照舊(不讓別列借用)" % fork,
          ok and [t for t, o in drawn[2:]] == ["+1 to Maximum Power Charges"] * 2, "drawn=%r err=%r" % (drawn, err))
    ok, err, drawn, _ = r15_draw(h, "(function() local d = main.popups[1].controls.implicit1 d.list = { "
                         "{ label = 'Zzq two line row', mod = data.itemMods.Corrupted.V2MaxPowerChargesCorrupted } } d.selIndex = 1 return d end)()", True)
    check("%s DO10 GGPK 兩行的譯文在單行下拉列接成一行" % fork,
          ok and [t for t, o in drawn[2:]] == ["若你近期有攻擊，你和附近友方 +#% 格擋率"], "drawn=%r err=%r" % (drawn, err))
    h0 = r15_harness(fork, define_api=False)
    tab0 = make(h0, "ItemsTab")
    h0.eval('function(t) t:CorruptDisplayItem() main.popups[1].controls.implicit1.selIndex = 2 end')(tab0)
    ok0, err0, drawn0, _ = r15_draw(h0, "main.popups[1].controls.implicit1", True)
    check("%s DO8 舊引擎(無覆寫 API)照舊畫、不報錯" % fork, ok0 and drawn0[:1] == [("+1 to Maximum Power Charges", False)],
          "drawn=%r err=%r" % (drawn0, err0))


def test_r15_popup_rows(fork):
    """CR1 ~ PM3 -- 製作詞綴下拉、腐化 / 自訂 / 異能彈窗的列以詞綴 stat id 走整列查詢。"""
    h = r15_harness(fork)
    tab = make(h, "ItemsTab")
    h.eval('function(t) t.controls.displayItemAffix1.selIndex = 2 end')(tab)
    del h.r15["drawn"][:]
    okA, errA = h.eval('function(t) local ok, e = pcall(function() local d = t.controls.displayItemAffix1 '
                       'd.dropped = true d:Draw({}) end) return ok, e end')(tab)
    rows = [t for t, o in h.r15["drawn"][2:]]
    check("%s CR1 製作詞綴下拉:多階合併列以各階 stat id(去重)翻" % fork,
          okA and rows[:2] == ["None", "擊殺時 #% 機率獲得 # 秒猛攻"], "rows=%r err=%r" % (rows, errA))
    check("%s CR1 以 item 情境、合併後的 stat id 詢問" % fork,
          (R15_ONS, "item", "chance_to_gain_onslaught_on_kill_%") in h.r15["rows"], "rows=%r" % h.r15["rows"])
    check("%s CR2 沒有 stat id 的詞綴列不問整列查詢、照舊英文" % fork,
          okA and "+20 to maximum Life" in rows and not any(r[0] == "+20 to maximum Life" for r in h.r15["rows"]),
          "rows=%r calls=%r" % (rows, h.r15["rows"]))
    h.eval('function(t) t:AddCustomModifierToDisplayItem() end')(tab)
    ok, err, drawn, _ = r15_draw(h, "main.popups[1].controls.modSelect", False)
    check("%s PM1 自訂詞綴彈窗(帶 (Suffix))以 mod 表找回 Mods.Id 再以 stat id 翻" % fork,
          ok and drawn[:1] == [("擊殺時 20% 機率獲得 4 秒猛攻 (後綴)", True)], "drawn=%r err=%r" % (drawn, err))
    okI, errI = h.eval('function(t) local ok, e = pcall(function() package.loaded["Modules.AddImplicitPopup"].AddImplicitToDisplayItem(t, t.displayItem) end) '
                       'return ok, e end')(tab)
    ok, err, drawn, _ = r15_draw(h, "main.popups[1].controls.modSelect", False)
    check("%s PM2 異能固定詞綴彈窗的列以 stat id 選對同英文的兩個 stat" % fork,
          okI and ok and drawn[:1] == [("移動時留下焦灼地面，持續 4 秒", True)], "drawn=%r err=%r %r" % (drawn, errI, err))
    h.eval('function(t) t:CorruptDisplayItem() main.popups[1].controls.implicit2.selIndex = 2 end')(tab)
    ok, err, drawn, _ = r15_draw(h, "main.popups[1].controls.implicit2", False)
    check("%s PM3 腐化彈窗第二個下拉同樣以 stat id 翻" % fork,
          ok and drawn[:1] == [("+1 最大暴擊球", True)], "drawn=%r err=%r" % (drawn, err))
    hn = r15_harness(fork, ids={})
    tabn = make(hn, "ItemsTab")
    hn.eval('function(t) t:CorruptDisplayItem() main.popups[1].controls.implicit1.selIndex = 2 end')(tabn)
    okn, errn, drawnn, _ = r15_draw(hn, "main.popups[1].controls.implicit1", True)
    check("%s PM4 出貨資料沒有詞綴 stat id 時整個下拉照舊(不設覆寫、不問整列查詢)" % fork,
          okn and all(o is False for t, o in drawnn) and not hn.r15["rows"], "drawn=%r rows=%r" % (drawnn, hn.r15["rows"]))


def test_r15_gem_list_name(fork):
    """GN1 ~ GN5 -- 寶石選擇清單列名以 GrantedEffects id 取 GGPK 名稱;英文對不上、沒資料、F2 關閉都不給。"""
    h = r15_harness(fork)
    f = h.eval("function(g) return PobToolsGemListName and PobToolsGemListName(g) end")
    mk = h.eval("function(n, id) return { name = n, grantedEffectId = id, grantedEffect = { id = id } } end")
    check("%s GN1 id 命中且英文相同:給 GGPK 名稱" % fork, f(mk("Detonate Mines", "GemDetonateMines")) == "遙控地雷",
          repr(f(mk("Detonate Mines", "GemDetonateMines"))))
    check("%s GN2 英文對不上不給(不退回一般查表)" % fork, f(mk("Detonate Minez", "GemDetonateMines")) is None)
    check("%s GN3 沒有 id 資料不給" % fork, f(mk("Fireball", "Fireball")) is None)
    h.enabled = False
    check("%s GN4 F2 關閉不給" % fork, f(mk("Detonate Mines", "GemDetonateMines")) is None)
    patches = parse_source_patches(read_text(UI_API))
    path = os.path.join(FORKS[fork], "Classes", "GemSelectControl.lua")
    patched, applied = apply_source_patches(read_text(path), "gemselectcontrol.lua", patches)
    check("%s GN5 引擎 source patch 讓清單列名(gemText)經 PobToolsGemListName" % fork,
          applied >= 1 and "PobToolsGemListName(gemData)" in patched, "applied=%d" % applied)


def test_r15_ascendancy_drop(fork):
    """AS1 ~ AS2 -- 昇華下拉的列以 Ascendancy id 取 GGPK 名稱;沒有 id 的列照舊。"""
    h = r15_harness(fork)
    ok, err = h.eval('function() local ok, e = pcall(function() main.modes.BUILD:Init() end) return ok, e end')()
    okD, errD, drawn, _ = r15_draw(h, "main.modes.BUILD.controls.ascendDrop", True)
    rows = [t for t, o in drawn[1:]]
    check("%s AS1 昇華下拉:Warden 以 Ascendancy id(Raider)取 GGPK 名稱" % fork,
          ok and okD and "守林人" in rows, "rows=%r err=%r %r" % (rows, err, errD))
    check("%s AS2 沒有 id 資料 / 沒有 id 的列照舊" % fork, okD and "None" in rows and "Deadeye" in rows, "rows=%r" % rows)


def test_r15_cluster_tip_notable(fork):
    """CT1 ~ CT2 -- 星團詞綴浮窗的大天賦說明行以該天賦的 stat id 走 passive;沒資料照舊。"""
    def run(ids):
        h = r15_harness(fork, ids=ids)
        tab = make(h, "ItemsTab")
        tip = make(h, "Tooltip")
        ok, err = h.eval('function(t, tip) local ok, e = pcall(function() local d = t.controls.displayItemAffix1 '
                         'd.tooltipFunc(tip, "HOVER", 4, d.list[4]) end) return ok, e end')(tab, tip)
        return h, ok, err, [_norm(l) for l in tooltip_lines(h, tip) if l is not None]
    h, ok, err, lines = run(None)
    check("%s CT1 大天賦說明行以天賦 stat id 走 passive" % fork, ok and "增加 +15 敏捷與智慧" in lines,
          "lines=%r err=%r stats=%r" % (lines, err, h.r15["stats"]))
    h0, ok0, err0, lines0 = run({k: v for k, v in R15_IDS.items() if k != "passive_stats"})
    check("%s CT2 沒有天賦 stat id 資料時照舊" % fork, ok0 and "+15 點敏捷與智慧" in lines0,
          "lines=%r err=%r" % (lines0, err0))


def test_r15_tattoo_label_stat_first(fork):
    """TA1 -- 紋身下拉隱藏段:先以節點 stat id 走 passive,查不到才退回一般查表。"""
    h = r15_harness(fork)
    f = h.eval("function(n) local r = PobToolsTattooLabelSd(n) return r and r[1] end")
    node = h.eval('function() return { sd = { "Skills fire an additional Projectile" }, '
                  'stats = { number_of_additional_projectiles = { min = 1, max = 1 } } } end')()
    plain = h.eval('function() return { sd = { "Skills fire an additional Projectile" } } end')()
    check("%s TA1 有 stat id 的紋身行先走 stat id(GGPK 字面)" % fork, f(node) == "技能有額外一個投射物", repr(f(node)))
    check("%s TA1 沒有 stat id 的紋身行照舊一般查表" % fork, f(plain) == "技能發射 1 個額外投射物", repr(f(plain)))
    two = h.eval('function() return { sd = { "Zzq trigger line", "Limited to 1 Zzq Tattoo" }, '
                 'stats = { zzq_trigger = { min = 1, max = 1 } } } end')()
    got = f(two)
    check("%s TA2 GGPK 兩行的譯文在單行下拉列裡接成一行(不畫到下一列)" % fork,
          got == "有傳奇敵人時，觸發 10% 技能,限制 1 個 Zzq 紋身", repr(got))


def test_r15_search_row_override(fork):
    """SH2 -- 中文搜尋補比對:有 _ptRowZh 的下拉以依 id 翻的列比對(與畫面一致)。"""
    h = r15_harness(fork, with_inject=False)
    h.run('LoadModule("Classes/SearchHost")')
    h.apply_inject()
    res = h.eval('''function()
		local SH = common.classes.SearchHost
		local list = { { label = "Drops Scorched Ground while moving, lasting 4 seconds",
			mod = data.itemMods.Eldritch.ScorchedGroundWhileMovingEldritchImplicit3 } }
		local o = { searchTerm = "留下焦灼", searchInfos = {}, ignoreOrder = false, UpdateMatchCount = SH.UpdateMatchCount,
			searchListAccessor = function() return list end,
			valueAccessor = function(v) return v.searchFilter or v.label end }
		local plain = { searchTerm = "留下焦灼", searchInfos = {}, ignoreOrder = false, UpdateMatchCount = SH.UpdateMatchCount,
			searchListAccessor = function() return list end,
			valueAccessor = function(v) return v.searchFilter or v.label end }
		local dd = r15NewDD(list)
		o._ptRowZh = rawget(dd, "_ptRowZh")
		package.loaded["Modules.AddImplicitPopup"].AddImplicitToDisplayItem({}, {})
		o._ptRowZh = rawget(main.popups[1].controls.modSelect, "_ptRowZh")
		SH.UpdateSearch(o)
		SH.UpdateSearch(plain)
		return o.searchInfos[1] and o.searchInfos[1].matches or false, plain.searchInfos[1] and plain.searchInfos[1].matches or false
	end''')()
    m1, m0 = res if isinstance(res, tuple) else (None, None)
    check("%s SH2 依 id 翻的列文字搜得到(對照:一般整列翻譯搜不到)" % fork, m1 is True and m0 is False,
          "with=%r without=%r" % (m1, m0))


# ---- R15b(審查修正,使用者核准 2026-10-28「兩個實作問題都改」)-----------------------------------
# SH3/SH4:同英文的兩列(一列有 id 譯文、一列沒有)= 衝突 → 繪製與搜尋都退回一般翻譯;收合框也依整份清單判斷。
# GN6:遙控地雷在寶石清單列、寶石輸入框、技能清單三處同名。AS3:守林人在昇華下拉與(新介面側欄用的)id 名稱同名。
R15_ROWS[("Zzq conflict row", "item", "max_power_charges")] = "依編號列"
DICT[None]["Zzq conflict row"] = "畫面列"
DICT["gems"] = dict(DICT.get("gems") or {}, **{"Detonate Mines": "引爆地雷"})

R15B_GEMSEL_P1 = """
local C = newClass("GemSelectControl")
function C:GemSelectControl(buf) self.buf = buf or "" self.gems = {} self.list = {} return self end
function C:Draw() DrawString(0, 0, "LEFT", 16, "VAR", self.buf) end
"""
R15B_GEMSEL_P2 = """
local C = newClass("GemSelectControl", function(self, buf) self.buf = buf or "" self.gems = {} self.list = {} end)
function C:Draw() DrawString(0, 0, "LEFT", 16, "VAR", self.buf) end
"""
R15B_SKILLLIST_P1 = """
local C = newClass("SkillListControl")
function C:SkillListControl(list) self.list = list return self end
function C:Draw() for _, g in ipairs(self.list) do DrawString(0, 0, "LEFT", 16, "VAR", g.displayLabel) end end
"""
R15B_SKILLLIST_P2 = """
local C = newClass("SkillListControl", function(self, list) self.list = list end)
function C:Draw() for _, g in ipairs(self.list) do DrawString(0, 0, "LEFT", 16, "VAR", g.displayLabel) end end
"""
R15B_GEMS = """
data.gems = {
	["Metadata/Items/Gems/SkillGemGemDetonateMines"] = { name = "Detonate Mines", grantedEffectId = "GemDetonateMines",
		grantedEffect = { id = "GemDetonateMines", name = "Detonate Mines" } },
}
r15DetGem = data.gems["Metadata/Items/Gems/SkillGemGemDetonateMines"]
"""


def r15b_harness(fork, with_search=False):
    h = r15_harness(fork, with_inject=False)
    h.run(R15B_GEMS)
    if with_search:
        h.run('LoadModule("Classes/SearchHost")')
    h.define("GemSelectControl", R15B_GEMSEL_P1, R15B_GEMSEL_P2)
    h.define("SkillListControl", R15B_SKILLLIST_P1, R15B_SKILLLIST_P2)
    h.apply_inject()
    return h


def test_r15b_conflict_search_and_closed(fork):
    """SH3 ~ SH4 -- 同英文衝突列:繪製(含收合框)與中文搜尋都用一般翻譯;依 id 的譯文搜不到。"""
    h = r15b_harness(fork, with_search=True)
    res = h.eval('''function()
		local SH = common.classes.SearchHost
		local list = { { label = "Zzq conflict row", mod = data.itemMods.Corrupted.V2MaxPowerChargesCorrupted },
			{ label = "Zzq conflict row", mod = data.itemMods.Corrupted.ZzqNoStats } }
		package.loaded["Modules.AddImplicitPopup"].AddImplicitToDisplayItem({}, {})
		local dd = main.popups[1].controls.modSelect
		dd.list = list
		local function search(term)
			local o = { searchTerm = term, searchInfos = {}, ignoreOrder = false, UpdateMatchCount = SH.UpdateMatchCount,
				searchListAccessor = function() return list end,
				valueAccessor = function(v) return v.searchFilter or v.label end,
				_ptRowZh = rawget(dd, "_ptRowZh"), list = list }
			SH.UpdateSearch(o)
			return (o.searchInfos[1] and o.searchInfos[1].matches or false), (o.searchInfos[2] and o.searchInfos[2].matches or false)
		end
		local a1, a2 = search("畫面")
		local b1, b2 = search("編號")
		return a1, a2, b1, b2
	end''')()
    a1, a2, b1, b2 = res if isinstance(res, tuple) else (None,) * 4
    check("%s SH3 衝突列以畫面上的字(一般翻譯)搜得到" % fork, a1 is True and a2 is True, "a=%r,%r" % (a1, a2))
    check("%s SH3 衝突列以 id 譯文搜不到(和畫面一致)" % fork, b1 is False and b2 is False, "b=%r,%r" % (b1, b2))
    ok, err, drawn, _ = r15_draw(h, "(function() local d = main.popups[1].controls.modSelect d.selIndex = 1 return d end)()", False)
    check("%s SH4 收合框也依整份清單判斷衝突(選中列不另用 id 譯文)" % fork,
          ok and drawn[:1] == [("Zzq conflict row", False)], "drawn=%r err=%r" % (drawn, err))


def test_r15b_gem_same_name(fork):
    """GN6 -- 遙控地雷在寶石清單列、寶石輸入框、技能清單三處同名(都依 GrantedEffects id)。"""
    h = r15b_harness(fork)
    row = h.eval("function() return PobToolsGemListName(r15DetGem) end")()
    if fork == "PoE1":
        box = h.eval('new("GemSelectControl"):GemSelectControl("Detonate Mines")')
        sl = h.eval('function() return new("SkillListControl"):SkillListControl({ { displayLabel = "Detonate Mines", '
                    'gemList = { { gemData = r15DetGem, enabled = true } } } }) end')()
    else:
        box = h.eval('new("GemSelectControl", "Detonate Mines")')
        sl = h.eval('function() return new("SkillListControl", { { displayLabel = "Detonate Mines", '
                    'gemList = { { gemData = r15DetGem, enabled = true } } } }) end')()
    del h.r15["drawn"][:]
    h.eval("function(c) c:Draw({}) end")(box)
    boxDrawn = [t for t, o in h.r15["drawn"]]
    del h.r15["drawn"][:]
    h.eval("function(c) c:Draw({}) end")(sl)
    slDrawn = [t for t, o in h.r15["drawn"]]
    check("%s GN6 清單列、輸入框、技能清單三處都是遙控地雷" % fork,
          row == "遙控地雷" and boxDrawn == ["遙控地雷"] and slDrawn == ["遙控地雷"],
          "row=%r box=%r skilllist=%r" % (row, boxDrawn, slDrawn))
    check("%s GN6 畫完覆寫表清掉" % fork, h.r15["override"] is None, "override=%r" % h.r15["override"])


def test_r15b_ascendancy_same_name(fork):
    """AS3 -- 守林人:昇華下拉與 id 名稱函式(新介面側欄、標題、天賦樹頁用)同名。"""
    h = r15b_harness(fork)
    h.eval('function() main.modes.BUILD:Init() end')()
    okD, errD, drawn, _ = r15_draw(h, "main.modes.BUILD.controls.ascendDrop", True)
    side = h.eval('function() return PobToolsAscendancyNameZh and PobToolsAscendancyNameZh("Raider", "Warden") end')()
    miss = h.eval('function() return PobToolsAscendancyNameZh and PobToolsAscendancyNameZh("Raider", "Wardenz") end')()
    rows = [t for t, o in drawn[1:]]
    check("%s AS3 昇華下拉與 id 名稱同為守林人" % fork, okD and "守林人" in rows and side == "守林人", "rows=%r side=%r" % (rows, side))
    check("%s AS3 英文對不上不給(呼叫端退回原路徑)" % fork, miss is None, "miss=%r" % miss)


def gt_lines(fork, gem_src, id_dict):
    h = gem_harness(fork, GEM_STAT_DICT, id_dict=id_dict)

    # 照引擎語意:帶英文時英文要對得上;不帶英文時回 id 表本身的值(zhByIdOnly 靠兩者比對)
    def translate_id(source, id_, en=None):
        key = None if id_ is None else str(id_)
        hit = id_dict.get((source, key))
        if not hit:
            return None
        return hit[1] if (en is None or hit[0] == en) else None
    h.rt.globals()["__py_translate_id"] = translate_id
    obj = make(h, "GemSelectControl")
    tip = make(h, "Tooltip")
    gem = h.eval("function() return %s end" % gem_src)()
    ok, err = h.eval('function(o, t, g) o.tooltip = t o.skillsTab = { build = { data = data } } '
                     'o.tipGem = g local ok, e = pcall(function() o:Draw({}, false) end) '
                     'return ok, e end')(obj, tip, gem)
    return ok, err, [_norm(l) for l in tooltip_lines(h, tip) if l is not None]


def test_r15b_gem_tooltip_title(fork):
    """GT1 ~ GT2 -- 寶石浮窗標題(gem_name)依該效果的 id 取名;同名兩個效果 id 不同時不猜,照舊。"""
    ids = dict(GEM_DESC_ID)
    ids[("gem_names", "Fireball")] = ("Fireball", "火球ID")
    ids[("gem_names", "FireballSecondary")] = ("Fireball", "火球副ID")
    ok, err, lines = gt_lines(fork, gem_instance_src(fork), ids)
    check("%s GT1 寶石浮窗標題依 GrantedEffects id 取名" % fork, ok and lines[:1] == ["火球ID"], "lines=%r err=%r" % (lines[:3], err))
    if fork != "PoE1":
        return
    same = gem_instance_src(fork).replace(
        '\t\t%s\n', '').replace(
        'statDescriptionScope = "gem_stat_descriptions" },',
        'statDescriptionScope = "gem_stat_descriptions" },\n\t\tsecondaryGrantedEffect = { id = "FireballSecondary", '
        'name = "Fireball", description = "Secondary blast.", stats = { } },')
    ok2, err2, lines2 = gt_lines(fork, same, ids)
    titles = [l for l in lines2 if l in ("火球ID", "火球副ID", "火球", "Fireball")]
    check("%s GT2 同名的主副效果 id 不同:標題不依 id(照舊)" % fork,
          ok2 and titles and all(t not in ("火球ID", "火球副ID") for t in titles), "titles=%r err=%r" % (titles, err2))


# R15c:清單原地重建(如 ItemsTab 腐化彈窗 wipeTable 後 t_insert),長度與首尾列不變、只換中段列。
R15C_SWAP = """function(midLabel, midMod)
	local d = main.popups[1].controls.implicit1
	local l = d.list
	local first, last = l[1], l[#l]
	wipeTable(l)
	table.insert(l, first)
	table.insert(l, { label = midLabel, mod = midMod })
	table.insert(l, last)
	d.selIndex = 1
end"""


def test_r15c_row_map_rebuilt_in_place(fork):
    """RC1 ~ RC2 -- 同一個清單原地換掉中段列(長度、首尾不變):覆寫表重算,不沿用舊列的譯文。"""
    h = r15_harness(fork)
    tab = make(h, "ItemsTab")
    h.eval('function(t) t:CorruptDisplayItem() end')(tab)
    ok0, err0, drawn0, _ = r15_draw(h, "main.popups[1].controls.implicit1", True)
    swap = h.eval(R15C_SWAP)
    swap(R15_SCORCH, h.eval('function() return data.itemMods.Eldritch.ScorchedGroundWhileMovingEldritchImplicit3 end')())
    ok, err, drawn, _ = r15_draw(h, "main.popups[1].controls.implicit1", True)
    rows = [t for t, o in drawn[2:]]
    check("%s RC1 中段換成新列:新列畫自己的 id 譯文" % fork,
          ok0 and ok and rows == ["None", "移動時留下焦灼地面，持續 4 秒", "+1 to Zzq Things"],
          "rows=%r err=%r/%r" % (rows, err0, err))
    swap("+1 to Maximum Power Charges", h.eval('function() return data.itemMods.Corrupted.V2MaxPowerChargesCorrupted end')())
    r15_draw(h, "main.popups[1].controls.implicit1", True)
    swap("+1 to Maximum Power Charges", h.eval('function() return data.itemMods.Corrupted.ZzqNoStats end')())
    ok2, err2, drawn2, _ = r15_draw(h, "main.popups[1].controls.implicit1", True)
    rows2 = [t for t, o in drawn2[2:]]
    check("%s RC2 中段換成同英文、沒有 id 譯文的列:照舊英文,不借舊列譯文" % fork,
          ok2 and rows2 == ["None", "+1 to Maximum Power Charges", "+1 to Zzq Things"], "rows=%r err=%r" % (rows2, err2))


# ---- R16:下拉列「詞綴名 ^8[行/行]」形狀依 Id 組出中文(PoE2 第 4 步第 4 輪,使用者核准 2026-10-29)--------
# (v5.1:僅 PoE2 生效,PoE1 維持現行)L1:帶詞綴名的列 = 詞綴名(依 Mods.Id 取)+ 原空白色碼 + [行中文(依 stat id 整列翻)]。
# L2:尾巴 (Suffix) (Desecrated) 逐字走一般字典;精髓 / 情感列的詞綴名走一般字典。
# L3:任一段沒答案整列不覆寫。L4:同英文不同 Id 兩列都不覆寫。L5:兩個 fork 都套用。L6:製作詞綴下拉(modList)。
R15_IDS["mod_stats"].update({
    "R16HeavyCharge": "max_power_charges",
    "R16TwoLine": "r16_stat_a r16_stat_b",
    "R16Ess": "max_power_charges",
    "R16Emo": "max_power_charges",
    "R16NoAffix": "max_power_charges",
    "R16Dup1": "max_power_charges",
    "R16Dup2": "max_power_charges",
    "R16HalfOk": "max_power_charges",
})
R15_IDS["affixes"] = {
    "R16HeavyCharge": "厚重壞", "R16TwoLine": "雙行詞",
    "R16Ess": "不該用", "R16Emo": "不該用2",
    "R16NoStats": "無統計詞", "R16Dup1": "重甲", "R16Dup2": "重乙", "R16HalfOk": "半詞",
}
R15_TPL["affixes"] = {
    "R16HeavyCharge": "Zzq Heavy", "R16TwoLine": "Zzq Twin",
    "R16Ess": "Zzq Ess Head", "R16Emo": "Zzq Emo Head",
    "R16NoStats": "Zzq NoStats", "R16Dup1": "Zzq Dup", "R16Dup2": "Zzq Dup", "R16HalfOk": "Zzq Half",
}
R15_ROWS[("Line A/Line B", "item", "r16_stat_a r16_stat_b")] = "甲行/乙行"
DICT[None].update({"Suffix": "後綴", "Desecrated": "瀆靈", "Zzq Ess Head": "精髓頭", "Zzq Emo Head": "情感頭",
                   "Zzq Unknown Affix": "不該出現"})

R16_SETUP = """
data.itemMods.R16 = {
	R16HeavyCharge = { "+1 to Maximum Power Charges", affix = "Zzq Heavy", type = "Prefix" },
	R16TwoLine = { "Line A", "Line B", affix = "Zzq Twin", type = "Prefix" },
	R16Ess = { "+1 to Maximum Power Charges", affix = "Zzq Ess Head", type = "Prefix" },
	R16Emo = { "+1 to Maximum Power Charges", affix = "Zzq Emo Head", type = "Prefix" },
	R16NoAffix = { "+1 to Maximum Power Charges", affix = "Zzq Unknown Affix", type = "Prefix" },
	R16NoStats = { "+1 to Maximum Power Charges", affix = "Zzq NoStats", type = "Prefix" },
	R16Dup1 = { "+1 to Maximum Power Charges", affix = "Zzq Dup", type = "Prefix" },
	R16Dup2 = { "+1 to Maximum Power Charges", affix = "Zzq Dup", type = "Prefix" },
	R16HalfOk = { "+1 to Maximum Power Charges", affix = "Zzq Half", type = "Prefix" },
}
R16_LIST = function() return {} end
local C = common.classes["ItemsTab"]
function C:R15Init()
	self.build = r15Build
	self.displayItem = { affixes = r15Affixes }
	self.controls = {}
	self.controls.displayItemAffix1 = r15NewDD(R16_LIST())
end
function C:AddCustomModifierToDisplayItem()
	main:OpenPopup(100, 100, "Add Modifier", { modSelect = r15NewDD(R16_LIST()) })
end
package.loaded["Modules.AddImplicitPopup"].AddImplicitToDisplayItem = function(itemsTab, item)
	main:OpenPopup(100, 100, "Add Implicit", { modSelect = r15NewDD(R16_LIST()) })
end
"""


def r16_harness(fork, ids=None, game=None):
    h = r15_harness(fork, with_inject=False, ids=ids, game=game)
    h.run(R16_SETUP)
    h.apply_inject()
    return h


def r16_rows_lua(rows):
    """rows:[(label, 其餘欄位的 Lua 片段)] -> Lua 清單內容;M 是 data.itemMods.R16。"""
    return ", ".join('{ label = "%s", %s }' % (label, rest) for label, rest in rows)


def r16_set_list(h, rows):
    h.run("R16_LIST = function() local M = data.itemMods.R16 return { %s } end" % r16_rows_lua(rows))


def r16_popup(h, rows, dropped=True, via="custom"):
    """畫一次 modSelect 彈窗(via=custom 走新增詞綴、implicit 走異能彈窗);回 (ok, err, 畫出的字清單, 列文字)。"""
    r16_set_list(h, rows)
    tab = make(h, "ItemsTab")
    if via == "implicit":
        h.eval('function(t) package.loaded["Modules.AddImplicitPopup"].AddImplicitToDisplayItem(t, t.displayItem) end')(tab)
    else:
        h.eval("function(t) t:AddCustomModifierToDisplayItem() end")(tab)
    ok, err, drawn, _ = r15_draw(h, "main.popups[1].controls.modSelect", dropped)
    return ok, err, drawn, [t for t, o in drawn[2:]]


R16_HEAVY = "Zzq Heavy   ^8[+1 to Maximum Power Charges]"
R16_HEAVY_ZH = "厚重壞   ^8[+1 最大暴擊球]"


def want(fork, zh, orig):
    """形狀 H 新規則只在 PoE2 生效(v5.1);PoE1 維持現行(整個 label 交給整列查詢,這些替身列查無 → 原英文)。"""
    return zh if fork == "PoE2" else orig


def test_r16_affix_name_row(fork):
    """L1 -- 新增詞綴彈窗帶詞綴名的列:PoE2 依 Id 組出中文(空白色碼保留),PoE1 維持原樣。"""
    h = r16_harness(fork)
    labels = [R16_HEAVY, "Zzq Twin   ^8[Line A/Line B]", "Zzq Heavy   [+1 to Maximum Power Charges]",
              "Zzq Heavy  ^7[+1 to Maximum Power Charges]"]
    zhs = [R16_HEAVY_ZH, "雙行詞   ^8[甲行/乙行]", "厚重壞   [+1 最大暴擊球]", "厚重壞  ^7[+1 最大暴擊球]"]
    mods = ["R16HeavyCharge", "R16TwoLine", "R16HeavyCharge", "R16HeavyCharge"]
    rows = [(lb, "mod = M.%s" % m) for lb, m in zip(labels, mods)]
    ok, err, drawn, got = r16_popup(h, rows)
    check("%s L1 詞綴名列展開:%s" % (fork, "詞綴名取 Id 譯名、行取 stat id 譯文、三個空白與 ^8 保留" if fork == "PoE2" else "PoE1 維持原樣(原英文)"),
          ok and got[:1] == [want(fork, zhs[0], labels[0])], "rows=%r err=%r" % (got, err))
    check("%s L1 兩行詞綴:%s" % (fork, "整段 body 以 / 相接交給整列查詢,結果放回方括號" if fork == "PoE2" else "PoE1 維持原樣"),
          ok and got[1:2] == [want(fork, zhs[1], labels[1])], "rows=%r" % got)
    check("%s L1 沒有色碼的列:%s" % (fork, "空白保留、不補色碼" if fork == "PoE2" else "PoE1 維持原樣"),
          ok and got[2:3] == [want(fork, zhs[2], labels[2])], "rows=%r" % got)
    check("%s L1 兩個空白加 ^7:%s" % (fork, "原樣保留" if fork == "PoE2" else "PoE1 維持原樣"),
          ok and got[3:4] == [want(fork, zhs[3], labels[3])], "rows=%r" % got)
    asked = h.r15["rows"]
    if fork == "PoE2":
        check("%s L1 整列查詢只問方括號內的行文字(不把詞綴名帶進去)" % fork,
              ("+1 to Maximum Power Charges", "item", "max_power_charges") in asked
              and ("Line A/Line B", "item", "r16_stat_a r16_stat_b") in asked
              and not any(r[0].startswith("Zzq ") for r in asked), "asked=%r" % asked)
    else:
        check("%s L1 PoE1 整列查詢問的仍是整個 label(行為同現行)" % fork,
              (R16_HEAVY, "item", "max_power_charges") in asked
              and not any(r[0] == "+1 to Maximum Power Charges" for r in asked), "asked=%r" % asked)
    okc, errc, drawnc, _ = r16_popup(r16_harness(fork), rows, dropped=False)
    check("%s L1 收合框的選中列:%s" % (fork, "畫成依 Id 組出的中文" if fork == "PoE2" else "PoE1 維持原英文"),
          okc and drawnc[:1] == [(R16_HEAVY_ZH, True)] if fork == "PoE2" else okc and drawnc[:1] == [(R16_HEAVY, False)],
          "drawn=%r err=%r" % (drawnc, errc))


def test_r16_desecrated_essence_tail(fork):
    """L2 -- 瀆靈 / 後綴尾巴逐字翻、順序不變;精髓與情感列的詞綴名走一般字典(僅 PoE2;PoE1 維持原樣)。"""
    h = r16_harness(fork)
    spec = [(R16_HEAVY + " (Suffix) (Desecrated)", "mod = M.R16HeavyCharge", "厚重壞   ^8[+1 最大暴擊球] (後綴) (瀆靈)", "尾巴逐字翻、順序不變"),
            ("Zzq Twin   ^8[Line A/Line B] (Desecrated) (Suffix)", "mod = M.R16TwoLine", "雙行詞   ^8[甲行/乙行] (瀆靈) (後綴)", "尾巴順序反過來也照原順序"),
            (R16_HEAVY + " ()", "mod = M.R16HeavyCharge", "厚重壞   ^8[+1 最大暴擊球] ()", "空字串尾巴保持 ()"),
            ("Zzq Ess Head   ^8[+1 to Maximum Power Charges] (Suffix)", "mod = M.R16Ess, essence = {}", "精髓頭   ^8[+1 最大暴擊球] (後綴)", "精髓列(essence 為表):詞綴名走一般字典、不用 Id 表的譯名"),
            ("Zzq Emo Head   ^8[+1 to Maximum Power Charges]", "mod = M.R16Emo, emotion = {}", "情感頭   ^8[+1 最大暴擊球]", "情感列(emotion 為表):詞綴名同樣走一般字典")]
    ok, err, drawn, got = r16_popup(h, [(lb, m) for lb, m, _, _ in spec])
    for i, (lb, _, zh, title) in enumerate(spec):
        check("%s L2 %s%s" % (fork, title, "" if fork == "PoE2" else "(PoE1 維持原樣)"),
              ok and got[i:i + 1] == [want(fork, zh, lb)], "rows=%r err=%r" % (got, err))


def test_r16_missing_piece_keeps_english(fork):
    """L3 -- 任一段沒答案,整列畫原英文(不出現半中半英);對照列 PoE2 翻成中文、PoE1 維持原樣。"""
    h = r16_harness(fork)
    rows = [("Zzq Unknown Affix   ^8[+1 to Maximum Power Charges]", "mod = M.R16NoAffix"),
            (R16_HEAVY + " (Suffix) (Zzqnoword)", "mod = M.R16HeavyCharge"),
            ("Zzq Heavy   ^8[Zzq no row text]", "mod = M.R16HeavyCharge"),
            ("Zzq NoStats   ^8[+1 to Maximum Power Charges]", "mod = M.R16NoStats"),
            ("Zzq Heavy2   ^8[+1 to Maximum Power Charges] (Suffix)", 'mod = { "x" }'),
            ("Zzq Ess Nodict   ^8[+1 to Maximum Power Charges]", "mod = M.R16Ess, essence = {}"),
            ("Zzq Twin   ^8[Line A/Line B]", "mod = M.R16TwoLine")]
    ok, err, drawn, got = r16_popup(h, rows)
    check("%s L3 對照列:%s" % (fork, "照常翻成中文(同一個清單裡有答案的列不受影響)" if fork == "PoE2" else "PoE1 維持原樣"),
          ok and got[6:7] == [want(fork, "雙行詞   ^8[甲行/乙行]", rows[6][0])], "rows=%r err=%r" % (got, err))
    check("%s L3 詞綴名查無(Id 表沒有,即使一般字典有該英文也不退回):整列英文" % fork,
          ok and got[0:1] == [rows[0][0]], "rows=%r" % got)
    check("%s L3 某個尾巴字查無:整列英文,不只翻一半" % fork,
          ok and got[1:2] == [rows[1][0]], "rows=%r" % got)
    check("%s L3 行文字查無:整列英文" % fork, ok and got[2:3] == [rows[2][0]], "rows=%r" % got)
    check("%s L3 該詞綴沒有 stat id:整列英文" % fork, ok and got[3:4] == [rows[3][0]], "rows=%r" % got)
    check("%s L3 mod 不在 data.itemMods(找不到詞綴 Id):整列英文" % fork, ok and got[4:5] == [rows[4][0]], "rows=%r" % got)
    check("%s L3 精髓列的詞綴名一般字典查無:整列英文" % fork, ok and got[5:6] == [rows[5][0]], "rows=%r" % got)
    hn = r16_harness(fork, ids={})
    okn, errn, drawnn, gotn = r16_popup(hn, rows[-1:] + [(R16_HEAVY, "mod = M.R16HeavyCharge")])
    check("%s L3 出貨資料沒有 id 表時整個下拉照舊(不設覆寫、不問整列查詢)" % fork,
          okn and all(o is False for t, o in drawnn) and not hn.r15["rows"], "drawn=%r rows=%r" % (drawnn, hn.r15["rows"]))


def test_r16_same_label_conflict(fork):
    """L4 -- 同英文不同 Id 且答案不同(含一有一無)的兩列都不覆寫;其他列 PoE2 照常翻、PoE1 維持原樣。"""
    h = r16_harness(fork)
    dup = "Zzq Dup   ^8[+1 to Maximum Power Charges]"
    rows = [(dup, "mod = M.R16Dup1"), (dup, "mod = M.R16Dup2"), (R16_HEAVY, "mod = M.R16HeavyCharge")]
    ok, err, drawn, got = r16_popup(h, rows)
    check("%s L4 對照列:%s" % (fork, "照常翻成中文" if fork == "PoE2" else "PoE1 維持原樣"),
          ok and got[2:3] == [want(fork, R16_HEAVY_ZH, R16_HEAVY)], "rows=%r err=%r" % (got, err))
    check("%s L4 同英文、詞綴名 Id 譯名不同:兩列都畫原英文" % fork,
          ok and got[:2] == [dup, dup], "rows=%r" % got)
    okc, errc, drawnc, _ = r16_popup(r16_harness(fork), rows, dropped=False)
    check("%s L4 收合框選中衝突列也畫原英文" % fork,
          okc and drawnc[:1] == [(dup, False)], "drawn=%r err=%r" % (drawnc, errc))
    half = "Zzq Half   ^8[+1 to Maximum Power Charges]"
    h2 = r16_harness(fork)
    rows2 = [(half, "mod = M.R16NoStats"), (half, "mod = M.R16HalfOk"), (R16_HEAVY, "mod = M.R16HeavyCharge")]
    ok2, err2, drawn2, got2 = r16_popup(h2, rows2)
    check("%s L4 同英文一列有答案一列沒有:兩列都畫原英文(不讓別列借用);對照列%s" % (fork, "翻中文" if fork == "PoE2" else "維持原樣"),
          ok2 and got2[:2] == [half, half] and got2[2:3] == [want(fork, R16_HEAVY_ZH, R16_HEAVY)],
          "rows=%r err=%r" % (got2, err2))


def test_r16_implicit_popup(fork):
    """L5 -- 異能彈窗(AddImplicitPopup)入口:PoE1 維持原樣、PoE2 依 Id 組出(兩邊各有斷言)。"""
    h = r16_harness(fork)
    labels = [R16_HEAVY, "Zzq Twin   ^8[Line A/Line B] (Desecrated)"]
    rows = [(labels[0], "mod = M.R16HeavyCharge"), (labels[1], "mod = M.R16TwoLine")]
    ok, err, drawn, got = r16_popup(h, rows, via="implicit")
    if fork == "PoE2":
        check("%s L5 異能彈窗:PoE2 詞綴名列依 Id 組出中文" % fork,
              ok and got == [R16_HEAVY_ZH, "雙行詞   ^8[甲行/乙行] (瀆靈)"], "rows=%r err=%r" % (got, err))
    else:
        check("%s L5 異能彈窗:PoE1 維持原樣(原英文)" % fork, ok and got == labels, "rows=%r err=%r" % (got, err))
        check("%s L5 異能彈窗:PoE1 整列查詢問整個 label(同現行)" % fork,
              (labels[0], "item", "max_power_charges") in h.r15["rows"], "asked=%r" % h.r15["rows"])


def test_r16_affix_drop_modlist(fork):
    """L6 -- 物品編輯詞綴下拉(value.modList):PoE2 單一 Id 取該 Id、有 modId 取 modId、多階無 modId 不覆寫;PoE1 維持原樣;非詞綴名形狀兩邊照舊。"""
    h = r16_harness(fork)
    labels = [R16_HEAVY, "Zzq Twin   ^8[Line A/Line B] (Suffix)", "Zzq Dup   ^8[+1 to Maximum Power Charges]",
              "Zzq Multi   ^8[+1 to Maximum Power Charges]"]
    r16_set_list(h, [
        (labels[0], 'modList = { "R16HeavyCharge" }'),
        (labels[1], 'modList = { "R16TwoLine" }'),
        (labels[2], 'modList = { "R16Dup1", "R16Dup2" }, modId = "R16Dup1"'),
        (labels[3], 'modList = { "R16HeavyCharge", "R16Dup1" }'),
        (R15_ONS, 'modList = { "OnslaughtOnKillInfluence1", "OnslaughtOnKillInfluence2_" }')])
    tab = make(h, "ItemsTab")
    h.eval("function(t) R16_TAB = t end")(tab)
    ok, err, drawn, _ = r15_draw(h, "R16_TAB.controls.displayItemAffix1", True)
    got = [t for t, o in drawn[2:]]
    check("%s L6 單一階 modList:%s" % (fork, "詞綴名取 modList[1]、行取其 stat id" if fork == "PoE2" else "PoE1 維持原樣"),
          ok and got[:1] == [want(fork, R16_HEAVY_ZH, labels[0])], "rows=%r err=%r" % (got, err))
    check("%s L6 單一階帶尾巴:%s" % (fork, "尾巴逐字翻" if fork == "PoE2" else "PoE1 維持原樣"),
          ok and got[1:2] == [want(fork, "雙行詞   ^8[甲行/乙行] (後綴)", labels[1])], "rows=%r" % got)
    check("%s L6 多階但有 modId:%s" % (fork, "詞綴名取 modId 的 Id 譯名" if fork == "PoE2" else "PoE1 維持原樣"),
          ok and got[2:3] == [want(fork, "重甲   ^8[+1 最大暴擊球]", labels[2])], "rows=%r" % got)
    check("%s L6 多階且沒有 modId:不猜,整列畫原英文" % fork, ok and got[3:4] == [labels[3]], "rows=%r" % got)
    check("%s L6 不是詞綴名形狀的多階合併列:兩邊都維持依合併 stat id 整列翻" % fork,
          ok and got[4:5] == ["擊殺時 #% 機率獲得 # 秒猛攻"], "rows=%r" % got)


def test_r16_empty_head(fork):
    """L7 -- head 是空字串的列(只有空白加色碼加 [行]):PoE2 不查詢詞綴名、空白色碼保留;PoE1 維持原樣。"""
    h = r16_harness(fork)
    labels = ["   ^8[+1 to Maximum Power Charges] (Suffix) (Desecrated)",
              "   ^8[Line A/Line B]",
              "  [+1 to Maximum Power Charges]"]
    zhs = ["   ^8[+1 最大暴擊球] (後綴) (瀆靈)", "   ^8[甲行/乙行]", "  [+1 最大暴擊球]"]
    # R16NoAffix 沒有詞綴名 Id 譯名:head 空時不該去查,所以仍要翻出中文
    mods = ["R16NoAffix", "R16TwoLine", "R16HeavyCharge"]
    ok, err, drawn, got = r16_popup(h, [(lb, "mod = M.%s" % m) for lb, m in zip(labels, mods)])
    check("%s L7 head 空 + 尾巴:%s" % (fork, "空白色碼保留、行與尾巴翻中文、不查詢詞綴名" if fork == "PoE2" else "PoE1 維持原樣"),
          ok and got[0:1] == [want(fork, zhs[0], labels[0])], "rows=%r err=%r" % (got, err))
    check("%s L7 head 空 + 兩行 body:%s" % (fork, "空白色碼保留" if fork == "PoE2" else "PoE1 維持原樣"),
          ok and got[1:2] == [want(fork, zhs[1], labels[1])], "rows=%r" % got)
    check("%s L7 head 空 + 無色碼:%s" % (fork, "兩個空白保留、不補色碼" if fork == "PoE2" else "PoE1 維持原樣"),
          ok and got[2:3] == [want(fork, zhs[2], labels[2])], "rows=%r" % got)
    h2 = r16_harness(fork)
    r16_set_list(h2, [(labels[0], 'modList = { "R16NoAffix" }')])
    tab = make(h2, "ItemsTab")
    h2.eval("function(t) R16_TAB = t end")(tab)
    ok2, err2, drawn2, _ = r15_draw(h2, "R16_TAB.controls.displayItemAffix1", True)
    got2 = [t for t, o in drawn2[2:]]
    check("%s L7 製作詞綴下拉(modList)的 head 空列:%s" % (fork, "同樣依 stat id 組出" if fork == "PoE2" else "PoE1 維持原樣"),
          ok2 and got2 == [want(fork, zhs[0], labels[0])], "rows=%r err=%r" % (got2, err2))


# ---- GS:引擎拆分第 1 期 -- 遊戲判斷(PobToolsGame / POB_GAME / latestTreeVersion)與入口分流 ----------
# 契約:inject 入口先看 PobToolsGame() 回 "poe1"/"poe2",否則看 os.getenv("POB_GAME"),再否則看
# latestTreeVersion 是否以 "0_" 開頭(→ poe2),都不符合是 poe1。判定後以全域 loadfile 載入同資料夾的
# inject_<game>.lua(每次只載一個)。遊戲檔缺漏 / 載入出錯 → PobToolsLogError("inject", 含檔名),共用補丁照套。
# 舊的 fork 偵測(DropDownControl._constructor 等)拿掉,PoE2 下拉逐段翻譯改依遊戲判斷。

# 在 Lua 端包裝全域 loadfile 與 ConPrintf:記錄被載入的路徑 / 輸出的訊息。
#   GS_STUB_GAME_FILES = true:inject_poe*.lua 不真的執行(只記錄,回一個空函式),
#       讓「載了誰」與「遊戲檔裡做什麼」分開驗。
#   GS_FAIL_MATCH = "inject_poe2.lua":路徑含這串就回 nil, "cannot open"(模擬遊戲檔缺漏)。
GS_SPY = rb"""
GS_LOADED = {}
GS_CONP = {}
GS_STUB_GAME_FILES = true
GS_FAIL_MATCH = nil
local real_loadfile = loadfile
function loadfile(path, ...)
	GS_LOADED[#GS_LOADED + 1] = tostring(path)
	if GS_FAIL_MATCH and type(path) == "string" and path:find(GS_FAIL_MATCH, 1, true) then
		return nil, "cannot open"
	end
	if GS_STUB_GAME_FILES and type(path) == "string" and path:find("inject_poe", 1, true) then
		return function() end
	end
	return real_loadfile(path, ...)
end
function ConPrintf(fmt, ...)
	local ok, s = pcall(string.format, fmt, ...)
	GS_CONP[#GS_CONP + 1] = ok and s or tostring(fmt)
end
"""

# 契約列的 15 個 PATCHES class(SkillListControl … DropDownControl 共 15 個);集合不變。
GS_PATCH_CLASSES = sorted([
    "CalcsTab", "DropDownControl", "EditControl", "GemSelectControl", "Item", "ItemDBControl",
    "ItemsTab", "MinionSearchListControl", "NotableDBControl", "PassiveMasteryControl",
    "PassiveTreeView", "SearchHost", "SkillListControl", "Tooltip", "TreeTab"])
# 這 13 個用 fork 的真檔載入;Item / ItemsTab 載不起來(依賴太多),用最小替身只為了讓補丁「被嘗試」。
GS_REAL_CLASSES = [c for c in GS_PATCH_CLASSES if c not in ("Item", "ItemsTab")]
GS_STUB_ITEMSTAB_P1 = """
local C = newClass("ItemsTab")
function C:ItemsTab() return self end
"""
GS_STUB_ITEMSTAB_P2 = """
local C = newClass("ItemsTab", function(self) end)
"""
GS_STUB_ITEM_P1 = """
local C = newClass("Item")
function C:Item(raw) return self end
function C:ParseRaw() end
"""
GS_STUB_ITEM_P2 = """
local C = newClass("Item", function(self, raw) end)
function C:ParseRaw() end
"""


def gs_harness(fork, game=None, define_game=True, tree_version=None, stub_game_files=True,
               fail_match=None, load_classes=False):
    """入口還沒載入的 harness;包好 loadfile / ConPrintf 再套 inject。load_classes:先載 15 個補丁 class。"""
    h = Harness(fork, with_inject=False, game=game, define_game=define_game)
    h.run(GS_SPY)
    h.run("GS_STUB_GAME_FILES = %s" % ("true" if stub_game_files else "false"))
    if fail_match:
        h.run('GS_FAIL_MATCH = "%s"' % fail_match)
    if tree_version is not None:
        h.run('latestTreeVersion = "%s"' % tree_version)
    h.gs_load_errors = []
    if load_classes:
        for name in GS_REAL_CLASSES:
            try:
                h.run('LoadModule("Classes/%s")' % name)
            except Exception as e:      # 載不起來要看得到,不能默默少一個 class
                h.gs_load_errors.append((name, str(e)[:120]))
        h.define("ItemsTab", GS_STUB_ITEMSTAB_P1, GS_STUB_ITEMSTAB_P2)
        h.define("Item", GS_STUB_ITEM_P1, GS_STUB_ITEM_P2)
    h.apply_inject()
    return h


def gs_loaded_game_files(h):
    """被 loadfile 載入的遊戲檔檔名(依載入順序)。"""
    raw = h.eval("function() return table.concat(GS_LOADED, '\\n') end")()
    paths = [p for p in (raw or "").split("\n") if p]
    return [p.replace("\\", "/") for p in paths if re.search(r"inject_poe\d\.lua$", p.replace("\\", "/"))]


def gs_conprintf(h):
    raw = h.eval("function() return table.concat(GS_CONP, '\\n') end")()
    return [m for m in (raw or "").split("\n") if m]


def gs_patch_names(h):
    """(成功的 class 集合, 被嘗試但失敗的 class 集合),取自 inject 印出的 patched / patch FAILED 行。"""
    ok, bad = set(), set()
    for m in gs_conprintf(h):
        m = m.split("] ", 1)[1] if m.startswith("[PobTools] ") else m
        if m.startswith("patched "):
            ok.add(m.split(" ", 1)[1].strip())
        elif m.startswith("patch FAILED "):
            bad.add(m.split(" ")[2].rstrip(":"))
    return ok, bad


def test_gs5_inject_dispatch_by_game(fork):
    """GS5 -- inject 入口依 PobToolsGame() 只載入對應的遊戲檔(inject_poe1.lua / inject_poe2.lua),且在引擎資料夾。"""
    for game in ("poe1", "poe2"):
        h = gs_harness(fork, game=game)
        files = gs_loaded_game_files(h)
        want_name = "inject_%s.lua" % game
        check("%s GS5 PobToolsGame 回 %s:只載入 %s" % (fork, game, want_name),
              [os.path.basename(p) for p in files] == [want_name], "loaded=%r" % files)
        if files:
            same_dir = os.path.normcase(os.path.normpath(os.path.dirname(files[0]))) == os.path.normcase(os.path.normpath(ROOT))
            check("%s GS5 遊戲檔取自 poecharm_inject.lua 所在資料夾(%s)" % (fork, game), same_dir,
                  "path=%r root=%r" % (files[0], ROOT))
        else:
            check("%s GS5 遊戲檔取自 poecharm_inject.lua 所在資料夾(%s)" % (fork, game), False, "沒有載入任何遊戲檔")


def test_gs5b_fallback_without_pobtoolsgame(fork):
    """GS5b -- 引擎沒有 PobToolsGame 時退路:latestTreeVersion 以 0_ 開頭選 poe2,3_26 選 poe1(不看 fork)。"""
    saved = os.environ.pop("POB_GAME", None)       # 第二順位是 os.getenv("POB_GAME"),先清掉以免干擾
    try:
        for tree, game in (("0_3", "poe2"), ("3_26", "poe1")):
            h = gs_harness(fork, define_game=False, tree_version=tree)
            has_api = h.eval('type(PobToolsGame)')
            files = [os.path.basename(p) for p in gs_loaded_game_files(h)]
            check("%s GS5b 沒有 PobToolsGame、latestTreeVersion=%s:只載入 inject_%s.lua" % (fork, tree, game),
                  has_api == "nil" and files == ["inject_%s.lua" % game], "api=%s loaded=%r" % (has_api, files))
    finally:
        if saved is not None:
            os.environ["POB_GAME"] = saved


def test_gs6_missing_game_file_is_logged(fork):
    """GS6 -- 遊戲檔缺漏(loadfile 回 nil, 錯誤)要寫失敗記錄(含檔名),共用補丁照常套用。"""
    h = gs_harness(fork, game="poe2", fail_match="inject_poe2.lua", load_classes=True)
    hits = [(f, m) for (f, m) in h.logged_errors if f == "inject" and "inject_poe2.lua" in m]
    check("%s GS6 inject_poe2.lua 載不到:PobToolsLogError(inject, 含 inject_poe2.lua)" % fork,
          len(hits) >= 1, "logged=%r" % h.logged_errors)
    ok, bad = gs_patch_names(h)
    shared = {"GemSelectControl", "EditControl", "Tooltip", "CalcsTab"}
    check("%s GS6 遊戲檔缺漏時共用補丁照常套用(GemSelectControl / EditControl / Tooltip / CalcsTab)" % fork,
          shared <= ok, "patched=%r bad=%r" % (sorted(ok), sorted(bad)))
    hc = gs_harness(fork, game="poe2", load_classes=True)
    check("%s GS6 對照:遊戲檔載得到時不寫缺漏記錄" % fork,
          not [1 for (f, m) in hc.logged_errors if f == "inject" and "inject_poe2.lua" in m],
          "logged=%r" % hc.logged_errors)
    check("%s GS6 對照:載得到的遊戲檔確實是被詢問過的那一個" % fork,
          [os.path.basename(p) for p in gs_loaded_game_files(hc)] == ["inject_poe2.lua"],
          "loaded=%r" % gs_loaded_game_files(hc))


def test_gs7_patch_set_unchanged(fork):
    """GS7 -- 拆檔後補丁清單不變:契約列的 15 個 class 都被套用或至少被嘗試;兩個遊戲都一樣。"""
    for game in ("poe1", "poe2"):
        h = gs_harness(fork, game=game, load_classes=True)
        ok, bad = gs_patch_names(h)
        attempted = ok | bad
        check("%s GS7 fork 真檔載得起來的 13 個 class 沒有載入失敗(game=%s)" % (fork, game),
              not h.gs_load_errors, "errors=%r" % h.gs_load_errors)
        check("%s GS7 被嘗試的補丁 class 集合 = 契約 15 個(game=%s)" % (fork, game),
              attempted == set(GS_PATCH_CLASSES),
              "少=%r 多=%r" % (sorted(set(GS_PATCH_CLASSES) - attempted), sorted(attempted - set(GS_PATCH_CLASSES))))
        check("%s GS7 13 個真 class 的補丁都成功、沒有 FAILED(game=%s)" % (fork, game),
              set(GS_REAL_CLASSES) <= ok and not (bad & set(GS_REAL_CLASSES)),
              "patched=%r failed=%r" % (sorted(ok), sorted(bad)))


def test_gs2p_poe2_segment_translation_follows_game(fork):
    """GS2' -- PoE2 下拉「詞綴名 + 空白 + [行] + 尾巴」逐段翻譯由遊戲判斷決定:PoE2 fork 但遊戲是 poe1 時走整列查詢。"""
    rows = [(R16_HEAVY, "mod = M.R16HeavyCharge")]
    en = R16_HEAVY
    zh = R16_HEAVY_ZH
    # 對照:遊戲 poe2 → 逐段翻譯(詞綴名取 Id 譯名、行取 stat id、尾巴逐字翻)
    h2 = r16_harness(fork, game="poe2")
    ok2, err2, _, got2 = r16_popup(h2, rows)
    asked2 = h2.r15["rows"]
    if fork == "PoE2":
        check("%s GS2' 遊戲 poe2(對照):逐段翻譯,整列查詢只問方括號內的行文字" % fork,
              ok2 and got2[:1] == [zh] and ("+1 to Maximum Power Charges", "item", "max_power_charges") in asked2,
              "rows=%r asked=%r err=%r" % (got2, asked2, err2))
    # 遊戲 poe1 → 不走逐段翻譯:整列原英文,整列查詢問的是整個 label
    h1 = r16_harness(fork, game="poe1")
    ok1, err1, _, got1 = r16_popup(h1, rows)
    asked1 = h1.r15["rows"]
    check("%s GS2' 遊戲 poe1:不做逐段翻譯,列畫原英文(跟 PoE1 一樣)" % fork,
          ok1 and got1[:1] == [en], "rows=%r err=%r" % (got1, err1))
    check("%s GS2' 遊戲 poe1:整列查詢問整個 label、不拆方括號(跟 PoE1 一樣)" % fork,
          ok1 and (R16_HEAVY, "item", "max_power_charges") in asked1
          and not any(r[0] == "+1 to Maximum Power Charges" for r in asked1),
          "asked=%r" % asked1)


def main():
    missing = [f for f, d in FORKS.items() if not os.path.isdir(d)]
    if missing:
        sys.exit("找不到 POB 安裝(需要已部署的 dist/):%s" % ", ".join(missing))

    for fork in FORKS:
        print("== %s ==" % fork)
        test_gem_input(fork)
        test_gem_search(fork)
        test_itemdb(fork)
        test_minion(fork)
        test_calcs(fork)
        test_paste_regression(fork)
        test_no_patch_failed(fork)
        test_patch_failure_is_logged(fork)
        test_src_passive_name(fork)
        test_src_passive_stat_line(fork)
        test_src_mod_line(fork)
        test_src_unique_title(fork)
        test_src_unique_title_patched(fork)
        test_src_item_other_lines(fork)
        test_src_affix_title(fork)
        test_src_list_row(fork)
        test_src_restored_on_error(fork)
        test_id_passive_name(fork)
        test_id_affix_title(fork)
        test_id_obeys_f2(fork)
        test_id_replaced_node(fork)
        test_id_conquered_tattoo_name(fork)
        test_stat_multiline_item_mods(fork)
        test_stat_mod_line(fork)
        test_stat_passive(fork)
        test_stat_jewel_in_node(fork)
        test_stat_multiline_sd(fork)
        test_stat_multiline_sd_unsupported(fork)
        test_src_anoint_legion_tattoo(fork)
        test_src_legion_tattoo_with_source_patch(fork)
        test_sf_dropdown_search_filter(fork)
        test_tc_tattoo_label_per_line(fork)
        test_tc_tattoo_label_grouped(fork)
        test_tc_tattoo_label_period(fork)
        test_sf_addition_english_name(fork)
        test_sh_skip_zhraw_with_search_filter(fork)
        test_stat_gem_tooltip(fork)
        test_wrapped_arity_gem(fork)
        test_stat_gem_in_node(fork)
        test_gem_desc_id_mismatch_falls_back_to_gems(fork)
        test_stat_gem_scope_ctx(fork)
        test_rb1_conquered_unrenamed(fork)
        test_ri1_tattoo_and_conquered(fork)
        test_ri2_gem_desc_per_line(fork)
        test_ri3_variant_hidden_line_pairs(fork)
        test_e7_pair_empty_second_line(fork)
        test_s8_legion_dropdown_by_id(fork)
        test_n2_item_single_line_note(fork)
        test_n3_pair_one_line_answer(fork)
        test_n4_node_single_line_note(fork)
        test_sid_stat_id_positions(fork)
        test_sid_conquered_original_lines(fork)
        test_fl_unique_flavour_stays_english(fork)
        test_fl_wrapped_scope(fork)
        test_groups_anoint_popup(fork)
        test_groups_node_flavour(fork)
        test_groups_gem_flavour(fork)
        test_groups_tattoo_popup(fork)
        test_groups_craft_affix_tip(fork)
        test_groups_old_engine(fork)
        test_r1_gem_unsupported_note(fork)
        test_r2_tattoo_label_prev_line(fork)
        test_slash_ctx_mastery(fork)
        test_r15_dropdown_override(fork)
        test_r15_popup_rows(fork)
        test_r15_gem_list_name(fork)
        test_r15_ascendancy_drop(fork)
        test_r15_cluster_tip_notable(fork)
        test_r15_tattoo_label_stat_first(fork)
        test_r15_search_row_override(fork)
        test_r15b_conflict_search_and_closed(fork)
        test_r15b_gem_same_name(fork)
        test_r15b_ascendancy_same_name(fork)
        test_r15b_gem_tooltip_title(fork)
        test_r15c_row_map_rebuilt_in_place(fork)
        test_r16_affix_name_row(fork)
        test_r16_desecrated_essence_tail(fork)
        test_r16_missing_piece_keeps_english(fork)
        test_r16_same_label_conflict(fork)
        test_r16_implicit_popup(fork)
        test_r16_affix_drop_modlist(fork)
        test_r16_empty_head(fork)
        test_gs5_inject_dispatch_by_game(fork)
        test_gs5b_fallback_without_pobtoolsgame(fork)
        test_gs6_missing_game_file_is_logged(fork)
        test_gs7_patch_set_unchanged(fork)
        test_gs2p_poe2_segment_translation_follows_game(fork)
        test_wrapped_arity(fork)
        test_wrapped_arity_instance(fork)
        test_upstream_shape(fork)
    print("== C++ source patches ==")
    test_source_patch_anchors()
    test_source_patch_compiles()
    test_source_patch_reaches_required_modules()

    bad = [r for r in RESULTS if not r[1]]
    print("\n%d checks, %d failed" % (len(RESULTS), len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
