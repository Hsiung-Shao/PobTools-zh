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
DIST = os.path.join(ROOT, "dist")
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
    },
    "gems": {
        "Volatility": "易變輔助",
        "Fireball": "火球",
    },
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

    def __init__(self, fork, with_inject=True):
        self.fork = fork
        self.dir = FORKS[fork]
        self.rt = lua51.LuaRuntime(unpack_returned_tuples=True)
        self.source = None          # current PobToolsSetSource() name
        self.source_seen = set()    # every source active during a lookup
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
            if text is None:
                return None
            table = DICT.get(self.source) or {}
            self.source_seen.add(self.source)
            return table.get(text) or DICT[None].get(text)

        def translate_display(text):
            out = translate(text)
            return None if (out is None or out == text) else out

        def set_source(name=None):
            prev = self.source
            self.source = name
            return prev

        g["__py_translate"] = translate
        g["__py_translate_display"] = translate_display
        g["__py_set_source"] = set_source
        g["__py_item_title"] = translate
        # Wrapped in real Lua functions on purpose: a Python callable handed
        # straight to Lua is userdata, and the inject's first line is a
        # type(PobToolsTranslate) ~= "function" bail-out. A harness that trips
        # that guard silently tests nothing at all.
        self._load(rb"""
function PobToolsTranslate(s) return __py_translate(s) end
function PobToolsTranslateDisplay(s) return __py_translate_display(s) end
function PobToolsSetSource(n) return __py_set_source(n) end
function PobToolsGetTranslate() return true end
function PobToolsItemTitle(s) return __py_item_title(s) end
""")

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


# ---- upstream shape checks (catch a rename before a user does) -------------

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
    ],
    "Classes/MinionSearchListControl.lua": [
        (r"function MinionSearchListClass:DoesEntryMatchFilters", "DoesEntryMatchFilters"),
        (r"self\.controls\.searchText", "searchText control"),
    ],
    "Classes/CalcsTab.lua": [
        (r"function CalcsTabClass:SearchMatch", "SearchMatch"),
    ],
    "Classes/NotableDBControl.lua": [
        (r"function NotableDBClass:DoesNotableMatchFilters", "DoesNotableMatchFilters"),
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
    """Pull (file, anchor, insert_after, replace_with) out of kPobToolsSourcePatches[].

    replace_with is None for insert-mode patches. Adjacent C string literals are
    concatenated by the compiler, so the payload is whatever literals follow the
    first two; `nullptr` in the entry is what marks replace mode.
    """
    body = cpp.split("kPobToolsSourcePatches[] = {", 1)[1]
    body = body.split("\n};", 1)[0]
    body = re.sub(r"//[^\n]*", "", body)               # comments hold example code
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
        lits = re.findall(r'"((?:[^"\\]|\\.)*)"', e)
        if len(lits) < 2:
            continue
        fields = [unescape_c(x) for x in lits]
        leaf, anchor = fields[0], fields[1]
        payload = "".join(fields[2:])
        if "nullptr" in e:
            out.append((leaf, anchor, None, payload))
        else:
            out.append((leaf, anchor, payload, None))
    return out


def apply_source_patches(src, leaf, patches):
    """Mirror of pobtools_loadfile_patched() in ui_api.cpp."""
    applied = 0
    for p_leaf, anchor, insert_after, replace_with in patches:
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
    for leaf, anchor, insert_after, replace_with in patches:
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


def test_source_patch_compiles():
    """The patched text must still be valid Lua.

    Anchors are matched blind, so a payload with an unbalanced quote or a stray
    `end` would only surface as a POB that refuses to open a tab. Compiling the
    real file, patched exactly the way the engine patches it, is the cheap way
    to find that here instead.
    """
    patches = parse_source_patches(read_text(UI_API))
    leaves = sorted(set(p[0] for p in patches))
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
        test_upstream_shape(fork)
    print("== C++ source patches ==")
    test_source_patch_anchors()
    test_source_patch_compiles()

    bad = [r for r in RESULTS if not r[1]]
    print("\n%d checks, %d failed" % (len(RESULTS), len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
