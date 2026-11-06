#include "filter_selftest.h"
#include "filter_parser.h"
#include "filter_doc_editor.h"
#include "filter_schema.h"
#include "filter_batch.h"
#include "custom_rules_io.h"
#include "sound_library_service.h"
#include "sound_manager.h"
#include "filter_data.h"
#include "filter_i18n.h"
#include "filter_preview.h"
#include "filter_item_import.h"
#include "item_library.h"
#include "paste_fixtures.h"
#include "clipboard_util.h"
#include "editor_shell.h"
#include "filter_file_watch.h"
#include "filter_card_ui.h"
#include "filter_undo.h"
#include "filter_edit_journal.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <fstream>

#include <imgui.h>

namespace {

struct TestReport {
	std::string text;
	int failures = 0;

	void check(bool ok, const char* what, const std::string& detail = std::string())
	{
		text += ok ? "PASS  " : "FAIL  ";
		text += what;
		if (!detail.empty()) { text += "  ("; text += detail; text += ")"; }
		text += "\n";
		if (!ok) failures++;
	}
	void note(const std::string& s) { text += "      " + s + "\n"; }
};

// Synthetic NeverSink-style filter: BOM + CRLF + final newline, decorated
// comments, quoted multi-value lists, trailing comments, an unknown keyword and
// two blocks sharing the same header text (anchor tie-break material).
std::string synthetic_crlf()
{
	std::string s;
	s += "\xEF\xBB\xBF";
	s += "#===============================================\r\n";
	s += "# NeverSink style preamble\r\n";
	s += "\r\n";
	s += "Show # %D4 $type->currency $tier->t1\r\n";
	s += "    BaseType \"Divine Orb\" \"Exalted Orb\" # tail note\r\n";
	s += "    Rarity >= Rare\r\n";
	s += "    SomeFutureKeyword abc 123\r\n";
	s += "    SetFontSize 45\r\n";
	s += "    SetTextColor 255 0 0 255\r\n";
	s += "\r\n";
	s += "Show # dup\r\n";
	s += "    BaseType \"Chaos Orb\"\r\n";
	s += "    SetFontSize 40\r\n";
	s += "\r\n";
	s += "Show # dup\r\n";
	s += "    BaseType \"Vaal Orb\"\r\n";
	s += "    SetFontSize 32\r\n";
	s += "\r\n";
	s += "Hide\r\n";
	s += "    BaseType \"Scroll of Wisdom\"\r\n";
	return s;
}

// LF variant: no BOM, no final newline.
std::string synthetic_lf()
{
	std::string s;
	s += "# lf file\n";
	s += "Show\n";
	s += "    Class \"Divination Cards\"\n";
	s += "    SetFontSize 38";
	return s;
}

} // namespace

int RunFilterSelfTest(const std::wstring& exeDir)
{
	// WIN32-subsystem app: surface printf when started from a console.
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}

	TestReport rep;

	// ---- T1: parse/serialize round-trip is byte-exact --------------------
	{
		const std::string src = synthetic_crlf();
		FilterFile f = ParseFilter(src);
		rep.check(SerializeFilter(f) == src, "T1 round-trip CRLF+BOM byte-exact");
		rep.check(f.hadBom && f.crlf && f.finalNewline, "T1 BOM/CRLF/final-newline detected");
		rep.check(f.blocks.size() == 4, "T1 block count",
		          "got " + std::to_string(f.blocks.size()));
		rep.check(!f.blocks[0].hide && f.blocks[3].hide, "T1 Show/Hide verbs");
		rep.check(f.blocks[0].headerComment == "%D4 $type->currency $tier->t1",
		          "T1 header comment extracted", f.blocks[0].headerComment);
		rep.check(f.blocks[0].idxFontSize >= 0 && f.blocks[0].idxTextColor >= 0,
		          "T1 action cache populated");
		bool unknownKept = false;
		for (const FilterLine& ln : f.lines)
			if (ln.kind == FilterLineKind::Unknown && ln.keyword == "SomeFutureKeyword") unknownKept = true;
		rep.check(unknownKept, "T1 unknown keyword kept as Unknown");

		const std::string lfSrc = synthetic_lf();
		FilterFile lf = ParseFilter(lfSrc);
		rep.check(SerializeFilter(lf) == lfSrc, "T1 round-trip LF no-final-newline byte-exact");
		rep.check(!lf.hadBom && !lf.crlf && !lf.finalNewline, "T1 LF flags detected");
	}

	// ---- T2: negation operators ------------------------------------------
	{
		FilterLine a = ParseFilterLine("    Rarity != Unique");
		rep.check(a.op == "!=" && a.values.size() == 1 && a.values[0].text == "Unique",
		          "T2 '!=' operator split", a.op);
		FilterLine b = ParseFilterLine("    Rarity ! Unique");
		rep.check(b.op == "!" && b.values.size() == 1 && b.values[0].text == "Unique",
		          "T2 '!' operator split", b.op);
		rep.check(FilterSerializeLine(a) == "    Rarity != Unique", "T2 clean line emits raw");
		a.dirty = true;
		rep.check(FilterSerializeLine(a) == "    Rarity != Unique", "T2 dirty rebuild keeps '!='");
		FilterLine c = ParseFilterLine("    ItemLevel >= 84");
		rep.check(c.op == ">=" && c.values[0].text == "84", "T2 '>=' unchanged");
	}

	// ---- T3: document CRUD ------------------------------------------------
	{
		const std::string src = synthetic_crlf();
		FilterFile f = ParseFilter(src);
		FilterDocumentEditor doc;
		doc.Attach(&f);

		unsigned v0 = doc.structureVersion();
		int qLine = doc.InsertLine(0, "Quality", ">=", { FilterToken{ "20", false } });
		rep.check(qLine >= 0 && f.lines[qLine].kind == FilterLineKind::Condition,
		          "T3 InsertLine condition kind");
		// The new condition must sit after "Rarity >= Rare" and before the
		// block's actions (conditions group together).
		rep.check(f.lines[qLine - 1].keyword == "Rarity", "T3 condition inserted after last condition",
		          "prev=" + f.lines[qLine - 1].keyword);
		rep.check(f.lines[qLine].indent == f.lines[qLine - 1].indent, "T3 indent copied from neighbour");
		rep.check(doc.structureVersion() > v0, "T3 structureVersion bumped");

		int bLine = doc.InsertLine(0, "SetBorderColor", "", {
			FilterToken{ "255", false }, FilterToken{ "0", false }, FilterToken{ "0", false } });
		rep.check(bLine >= 0 && f.lines[bLine - 1].keyword == "SetTextColor",
		          "T3 action inserted after last action", "prev=" + f.lines[bLine - 1].keyword);

		std::string out = SerializeFilter(f);
		FilterFile f2 = ParseFilter(out);
		rep.check(f2.blocks.size() == 4, "T3 reparse block count stable");
		rep.check(f2.blocks[0].idxBorderColor >= 0, "T3 reparse sees new SetBorderColor");
		rep.check(out.find("    BaseType \"Divine Orb\" \"Exalted Orb\" # tail note\r\n") != std::string::npos,
		          "T3 untouched line bytes preserved");
		rep.check(out.find("    Quality >= 20\r\n") != std::string::npos, "T3 new condition serialized");

		size_t nBlocks = f.blocks.size();
		int nb = doc.CreateBlock(-1, true, "my custom");
		rep.check(nb == (int)nBlocks && f.blocks.size() == nBlocks + 1, "T3 CreateBlock appends");
		rep.check(f.lines[f.blocks[nb].headerLineIdx].keyword == "Hide", "T3 CreateBlock verb");

		int dup = doc.DuplicateBlock(0);
		rep.check(dup == 1 && f.blocks.size() == nBlocks + 2, "T3 DuplicateBlock inserts after source");
		rep.check(FilterSerializeLine(f.lines[f.blocks[dup].headerLineIdx]) ==
		          FilterSerializeLine(f.lines[f.blocks[0].headerLineIdx]),
		          "T3 duplicate header identical");

		size_t nLines = f.lines.size();
		doc.RemoveBlock(dup);
		rep.check(f.blocks.size() == nBlocks + 1, "T3 RemoveBlock restores count");
		rep.check(f.lines.size() < nLines, "T3 RemoveBlock erased lines");

		int rmLine = doc.FindLine(0, "Quality");
		size_t before = f.lines.size();
		doc.RemoveLine(rmLine);
		rep.check(f.lines.size() == before - 1 && doc.FindLine(0, "Quality") == -1,
		          "T3 RemoveLine hard-deletes");
	}

	// ---- T4: disable (#!) / restore --------------------------------------
	{
		FilterFile f = ParseFilter(synthetic_crlf());
		FilterDocumentEditor doc;
		doc.Attach(&f);

		int li = doc.FindLine(0, "SetFontSize");
		const std::string origRaw = f.lines[li].raw;
		doc.CommentOutLine(li);
		rep.check(f.lines[li].kind == FilterLineKind::Comment, "T4 disabled line is a comment");
		rep.check(f.lines[li].raw == "    #! SetFontSize 45", "T4 '#!' marker format", f.lines[li].raw);
		rep.check(doc.IsDisabledLine(li), "T4 IsDisabledLine recognises it");
		rep.check(f.blocks[0].idxFontSize == -1, "T4 action cache cleared");
		rep.check(doc.FindLine(0, "SetFontSize") == -1, "T4 FindLine skips disabled line");

		// Survives a save/load cycle.
		FilterFile f2 = ParseFilter(SerializeFilter(f));
		FilterDocumentEditor doc2;
		doc2.Attach(&f2);
		rep.check(doc2.IsDisabledLine(li), "T4 disabled line survives reparse");

		// Ordinary comments never qualify.
		rep.check(!doc.IsDisabledLine(1), "T4 plain comment not a disabled line");

		bool restored = doc.RestoreLine(li);
		rep.check(restored && f.lines[li].raw == origRaw, "T4 restore is byte-idempotent",
		          f.lines[li].raw);
		rep.check(f.blocks[0].idxFontSize == li, "T4 action cache back after restore");
	}

	// ---- T5: selection anchor across structural mutations ----------------
	{
		FilterFile f = ParseFilter(synthetic_crlf());
		FilterDocumentEditor doc;
		doc.Attach(&f);

		// Anchor the SECOND "Show # dup" block (blocks[2], BaseType "Vaal Orb").
		BlockAnchor a = doc.CaptureAnchor(2);
		rep.check(a.valid() && !a.firstCondRaw.empty(), "T5 anchor captured");

		doc.CreateBlockAtLine(0, false, "front insert");   // shifts every line
		int resolved = doc.ResolveAnchor(a);
		bool ok = resolved >= 0 &&
		          doc.FindLine(resolved, "BaseType") >= 0 &&
		          FilterHasValue(f.lines[doc.FindLine(resolved, "BaseType")], "Vaal Orb");
		rep.check(ok, "T5 anchor resolves to same block after front insert",
		          "resolved=" + std::to_string(resolved));

		doc.CommentOutBlock(0);                            // the block we just added
		resolved = doc.ResolveAnchor(a);
		ok = resolved >= 0 && doc.FindLine(resolved, "BaseType") >= 0 &&
		     FilterHasValue(f.lines[doc.FindLine(resolved, "BaseType")], "Vaal Orb");
		rep.check(ok, "T5 anchor survives CommentOutBlock", "resolved=" + std::to_string(resolved));

		// CommentOutBlock must not leave orphan syntax lines behind.
		bool orphanFree = true;
		FilterFile fr = ParseFilter(SerializeFilter(f));
		if (!fr.blocks.empty())
			for (int idx = 0; idx < fr.blocks[0].headerLineIdx; idx++) {
				FilterLineKind k = fr.lines[idx].kind;
				if (k == FilterLineKind::Condition || k == FilterLineKind::Action ||
				    k == FilterLineKind::Unknown)
					orphanFree = false;
			}
		rep.check(orphanFree, "T5 CommentOutBlock leaves no orphan syntax lines");
	}

	// ---- T6: schema integrity --------------------------------------------
	{
		bool kwOk = true, defOk = true, exclOk = true, enumOk = true, grpOk = true;
		std::string bad;
		for (const CardSchema& c : FilterSchemaAll()) {
			// Every keyword (and alias) must be one the parser recognises, and
			// isAction must match the parser's classification.
			bool known = c.isAction ? FilterIsKnownAction(c.keyword) : FilterIsKnownCondition(c.keyword);
			if (!known) { kwOk = false; bad += std::string(c.keyword) + " "; }
			if (c.alias) {
				bool aknown = c.isAction ? FilterIsKnownAction(c.alias) : FilterIsKnownCondition(c.alias);
				if (!aknown) { kwOk = false; bad += std::string(c.alias) + " "; }
			}
			// The default line must parse back to the same keyword and to the
			// expected kind (a bad default would insert broken syntax).
			FilterLine dl = ParseFilterLine(c.defaultLine);
			bool kindOk = c.isAction ? (dl.kind == FilterLineKind::Action)
			                         : (dl.kind == FilterLineKind::Condition);
			if (dl.keyword != c.keyword || !kindOk) { defOk = false; bad += std::string(c.defaultLine) + " "; }
			if (c.exclusiveGroup > 0 && !c.isAction) exclOk = false;
			for (const SchemaEnumValue& e : c.enums)
				if (!e.token || !e.token[0] || !e.zh || !e.zh[0]) enumOk = false;
			bool grpFound = false;
			for (const char* g : FilterSchemaGroups()) if (g == c.group) grpFound = true;
			if (!grpFound) grpOk = false;
		}
		rep.check(kwOk, "T6 schema keywords known to parser", bad);
		rep.check(defOk, "T6 default lines parse to same keyword+kind", bad);
		rep.check(exclOk, "T6 exclusive groups are actions only");
		rep.check(enumOk, "T6 enum tokens/labels non-empty");
		rep.check(grpOk, "T6 every card belongs to a listed group");
		rep.check(FilterSchemaFind("PlayAlertSoundPositional") != nullptr &&
		          FilterSchemaFind("CustomAlertSoundOptional") != nullptr,
		          "T6 aliases resolve to cards");
		rep.note("schema cards=" + std::to_string(FilterSchemaAll().size()));
	}

	// ---- T7: batch apply --------------------------------------------------
	{
		EditorShell es;   // default-constructed shell: doc + model only, no services
		es.model = ParseFilter(synthetic_crlf());
		es.doc.Attach(&es.model);
		es.loaded = true;

		BatchStyleOp op;
		op.showHide = BatchStyleOp::Tri::Set;   op.hide = true;
		op.fontSize = BatchStyleOp::Tri::Set;   op.size = 38;
		op.borderColor = BatchStyleOp::Tri::Remove;
		op.textColor = BatchStyleOp::Tri::Set;
		op.text[0] = 10; op.text[1] = 20; op.text[2] = 30; op.text[3] = 255;

		int touched = ApplyBatchStyle(es, { 0, 1, 2 }, op);
		rep.check(touched > 0, "T7 batch touched lines", std::to_string(touched));

		FilterFile r = ParseFilter(SerializeFilter(es.model));
		bool allHidden = r.blocks.size() >= 3 && r.blocks[0].hide && r.blocks[1].hide && r.blocks[2].hide;
		rep.check(allHidden, "T7 batch Show->Hide applied");
		bool fontOk = true, textOk = true;
		for (int i = 0; i < 3; i++) {
			const FilterBlock& b = r.blocks[i];
			fontOk = fontOk && b.idxFontSize >= 0 && FilterValueInt(r.lines[b.idxFontSize], 0, -1) == 38;
			int rr, gg, bb, aa; bool ha;
			bool has = b.idxTextColor >= 0;
			if (has) FilterGetColor(r.lines[b.idxTextColor], rr, gg, bb, aa, ha);
			textOk = textOk && has && rr == 10 && gg == 20 && bb == 30;
		}
		rep.check(fontOk, "T7 batch SetFontSize insert-or-update");
		rep.check(textOk, "T7 batch SetTextColor set");
		bool borderGone = true;
		for (int i = 0; i < 3; i++) borderGone = borderGone && r.blocks[i].idxBorderColor < 0;
		rep.check(borderGone, "T7 batch border colour disabled");
		// Block 3 (Hide "Scroll of Wisdom") was not selected: untouched bytes.
		rep.check(SerializeFilter(r).find("    BaseType \"Scroll of Wisdom\"\r\n") != std::string::npos,
		          "T7 unselected block untouched");
	}

	// ---- T8: custom zone + export/import ---------------------------------
	{
		FilterFile f = ParseFilter(synthetic_crlf());
		FilterDocumentEditor doc;
		doc.Attach(&f);

		CustomZone z0 = FindCustomZone(f);
		rep.check(!z0.present(), "T8 no zone in fresh file");

		CustomZone z = EnsureCustomZone(doc);
		rep.check(z.present(), "T8 zone created");
		bool beforeFirst = !f.blocks.empty() && z.endLine < f.blocks[0].headerLineIdx;
		rep.check(beforeFirst, "T8 zone sits before first block header");

		// Export blocks 1+2 from a second file and import them.
		FilterFile src = ParseFilter(synthetic_crlf());
		std::string frag = ExportCustomRules(src, { 1, 2 }, "test");
		rep.check(frag.find("#pobtools-rules") == 0, "T8 export has metadata header");

		std::string err;
		int n = ImportCustomRules(doc, frag, &err);
		rep.check(n == 2, "T8 import adds 2 blocks", err + " n=" + std::to_string(n));
		int again = ImportCustomRules(doc, frag, &err);
		rep.check(again == 0, "T8 re-import skips duplicates", std::to_string(again));

		// The imported blocks live inside the zone.
		CustomZone z2 = FindCustomZone(f);
		int inZone = 0;
		for (const FilterBlock& b : f.blocks)
			if (b.headerLineIdx > z2.beginLine && b.headerLineIdx < z2.endLine) inZone++;
		rep.check(inZone == 2, "T8 imported blocks inside zone", std::to_string(inZone));

		// Round-trips cleanly and the zone survives a reparse.
		FilterFile r = ParseFilter(SerializeFilter(f));
		rep.check(FindCustomZone(r).present(), "T8 zone survives reparse");

		// Damaged zone repair: drop the end sentinel, re-ensure.
		CustomZone z3 = FindCustomZone(r);
		FilterDocumentEditor doc2;
		doc2.Attach(&r);
		doc2.RemoveLine(z3.endLine);
		rep.check(!FindCustomZone(r).present(), "T8 end sentinel removed");
		CustomZone z4 = EnsureCustomZone(doc2);
		rep.check(z4.present(), "T8 damaged zone repaired");
	}

	// ---- T9: rename + reference sync (real files under %TEMP%) -----------
	{
		auto writeFile = [](const std::wstring& p, const std::string& content) {
			std::ofstream o(p, std::ios::binary);
			o.write(content.data(), (std::streamsize)content.size());
		};
		auto readFile = [](const std::wstring& p) {
			std::ifstream in(p, std::ios::binary);
			return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		};
		auto exists = [](const std::wstring& p) {
			DWORD a = GetFileAttributesW(p.c_str());
			return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
		};

		wchar_t tmpBuf[MAX_PATH];
		GetTempPathW(MAX_PATH, tmpBuf);
		std::wstring dir = std::wstring(tmpBuf) + L"pobtools_filter_selftest";
		CreateDirectoryW(dir.c_str(), nullptr);
		for (const wchar_t* n : { L"AAA.mp3", L"BBB.mp3", L"CCC.mp3", L"BBB (2).mp3", L"DDD.mp3" })
			DeleteFileW((dir + L"\\" + n).c_str());
		writeFile(dir + L"\\AAA.mp3", "aaa");
		writeFile(dir + L"\\BBB.mp3", "bbb");

		// Keep the user's persisted sound folder safe.
		std::wstring savedFolder = GetSoundFolder();
		struct FolderGuard {
			std::wstring saved;
			~FolderGuard() { SetSoundFolder(saved); }
		} guard{ savedFolder };

		SoundLibraryService svc;
		svc.Init(exeDir);
		svc.SetFolder(dir);
		rep.check(svc.files().size() == 2, "T9 scan finds 2 files",
		          std::to_string(svc.files().size()));

		std::string filterSrc =
			"Show\r\n"
			"    BaseType \"Divine Orb\"\r\n"
			"    CustomAlertSound \"AAA.mp3\" 300\r\n"
			"\r\n"
			"Show\r\n"
			"    BaseType \"Chaos Orb\"\r\n"
			"    CustomAlertSoundOptional \"sub\\AAA.mp3\"\r\n";
		FilterFile f = ParseFilter(filterSrc);
		FilterDocumentEditor doc;
		doc.Attach(&f);
		rep.check(FindCustomSoundRefs(f, L"AAA.mp3").size() == 2, "T9 refs found (plain + subdir)");
		rep.check(FindCustomSoundRefs(f, L"aaa.MP3").size() == 2, "T9 ref match is case-insensitive");

		// Plain rename with reference sync.
		RenamePlanEntry e = svc.BuildSingleRename(L"AAA.mp3", L"CCC.mp3", &f);
		rep.check(e.state == RenamePlanEntry::State::Rename && e.refLines.size() == 2,
		          "T9 single-rename plan");
		std::vector<RenamePlanEntry> plan{ e };
		SoundLibraryService::ApplyResult r = svc.ApplyRenamePlan(plan, &doc);
		rep.check(r.renamed == 1 && r.refsUpdated == 2 && r.err.empty(), "T9 rename applied + refs synced",
		          r.err);
		rep.check(!exists(dir + L"\\AAA.mp3") && exists(dir + L"\\CCC.mp3"), "T9 disk state after rename");
		rep.check(f.dirty, "T9 model marked dirty (not saved)");
		std::string out = SerializeFilter(f);
		rep.check(out.find("CustomAlertSound \"CCC.mp3\" 300") != std::string::npos &&
		          out.find("CustomAlertSoundOptional \"sub\\CCC.mp3\"") != std::string::npos,
		          "T9 both refs rewritten, path prefix kept");

		// Conflict refused while unresolved.
		RenamePlanEntry c = svc.BuildSingleRename(L"CCC.mp3", L"BBB.mp3", nullptr);
		rep.check(c.state == RenamePlanEntry::State::Conflict, "T9 conflict detected");
		std::vector<RenamePlanEntry> plan2{ c };
		r = svc.ApplyRenamePlan(plan2, nullptr);
		rep.check(!r.err.empty() && exists(dir + L"\\CCC.mp3"), "T9 unresolved conflict refused");

		// Suffix resolution.
		plan2[0].resolution = RenamePlanEntry::Resolution::Suffix;
		r = svc.ApplyRenamePlan(plan2, nullptr);
		rep.check(r.renamed == 1 && exists(dir + L"\\BBB (2).mp3") && !exists(dir + L"\\CCC.mp3"),
		          "T9 suffix resolution", r.err);

		// Swap resolution: contents exchange, names stay.
		writeFile(dir + L"\\DDD.mp3", "ddd");
		svc.Rescan();
		RenamePlanEntry sw = svc.BuildSingleRename(L"BBB (2).mp3", L"DDD.mp3", nullptr);
		rep.check(sw.state == RenamePlanEntry::State::Conflict, "T9 swap starts as conflict");
		std::vector<RenamePlanEntry> plan3{ sw };
		plan3[0].resolution = RenamePlanEntry::Resolution::Swap;
		r = svc.ApplyRenamePlan(plan3, nullptr);
		rep.check(r.swapped == 1 && readFile(dir + L"\\DDD.mp3") == "aaa" &&
		          readFile(dir + L"\\BBB (2).mp3") == "ddd",
		          "T9 swap exchanges contents", r.err);

		// Rule-based batch plan over the remaining files (BBB.mp3, BBB (2).mp3,
		// DDD.mp3): two matches, {n} numbers them without colliding.
		svc.rules().clear();
		svc.rules().push_back(NamingRule{ "test", "BBB", "alert{n}.{ext}", true });
		std::vector<RenamePlanEntry> bp = svc.BuildRenamePlan(nullptr);
		rep.check(bp.size() == 2 &&
		          bp[0].newName == L"alert1.mp3" && bp[1].newName == L"alert2.mp3" &&
		          bp[0].state == RenamePlanEntry::State::Rename &&
		          bp[1].state == RenamePlanEntry::State::Rename,
		          "T9 batch plan expands {n}/{ext} without collisions",
		          "n=" + std::to_string(bp.size()));

		for (const wchar_t* n : { L"AAA.mp3", L"BBB.mp3", L"CCC.mp3", L"BBB (2).mp3", L"DDD.mp3" })
			DeleteFileW((dir + L"\\" + n).c_str());
		RemoveDirectoryW(dir.c_str());
	}

	// ---- T10: naming-rules json round-trip -------------------------------
	{
		std::vector<NamingRule> rules = {
			{ u8"神聖石音效", "divine", "divine{n}.{ext}", true },
			{ "manual-only", "", "boss.mp3", false },
		};
		std::string j = SoundRulesToJson(rules);
		std::vector<NamingRule> back;
		bool ok = SoundRulesFromJson(j, &back);
		rep.check(ok && back.size() == 2 && back[0].name == rules[0].name &&
		          back[0].match == "divine" && back[1].enabled == false,
		          "T10 rules json round-trip");
		std::vector<NamingRule> bad;
		rep.check(!SoundRulesFromJson("{not json!!", &bad) && bad.empty(),
		          "T10 bad json rejected without crash");
		rep.check(!SoundRulesFromJson("[]", &bad), "T10 wrong shape rejected");
	}

	// ---- T10b: 替換引用 (ReplaceSoundRefs) --------------------------------
	{
		std::string src =
			"Show\r\n\tBaseType \"A\"\r\n\tCustomAlertSound \"old.mp3\" 250\r\n"
			"Show\r\n\tBaseType \"B\"\r\n\tCustomAlertSound \"sub/old.mp3\" 100\r\n"
			"Show\r\n\tBaseType \"C\"\r\n\tCustomAlertSound \"other.mp3\" 300\r\n";
		FilterFile f = ParseFilter(src);
		FilterDocumentEditor doc;
		doc.Attach(&f);
		int n = ReplaceSoundRefs(&doc, L"old.mp3", L"new.mp3");
		rep.check(n == 2 && f.dirty, "T10b replace-refs rewrites both refs", std::to_string(n));
		std::string out = SerializeFilter(f);
		rep.check(out.find("CustomAlertSound \"new.mp3\" 250") != std::string::npos &&
		          out.find("CustomAlertSound \"sub/new.mp3\" 100") != std::string::npos &&
		          out.find("\"other.mp3\" 300") != std::string::npos,
		          "T10b volume/prefix kept, other refs untouched");
		rep.check(ReplaceSoundRefs(&doc, L"new.mp3", L"new.mp3") == 0,
		          "T10b same-name replace is a no-op");
	}

	// ---- T11: big-file health (the bundled NeverSink fixture) --------------
	// Reads Filters\default.filter next to the exe -- never the user's own
	// filters in Documents (a selftest does not touch the user's game files).
	{
		std::vector<FilterListEntry> found;
		{
			FilterListEntry e;
			e.path = exeDir + L"Filters\\default.filter";
			e.name = "Filters\\default.filter";
			if (GetFileAttributesW(e.path.c_str()) != INVALID_FILE_ATTRIBUTES) found.push_back(e);
		}
		if (found.empty()) {
			rep.note("T11 skipped: no Filters\\default.filter next to the exe");
		} else {
			bool ok = false;
			FilterFile f = LoadFilter(found.front().path, &ok);
			if (!ok) {
				rep.note("T11 skipped: could not read " + found.front().name);
			} else {
				std::ifstream in(found.front().path, std::ios::binary);
				std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				rep.check(SerializeFilter(f) == raw,
				          "T11 real filter round-trip byte-exact", found.front().name);
				rep.note("T11 " + found.front().name + ": " + std::to_string(f.blocks.size()) +
				         " blocks, " + std::to_string(f.lines.size()) + " lines");
			}
		}
	}

	// ---- T12: NeverSink header zh display ---------------------------------
	{
		rep.check(NeverSinkHeaderZh("%H2 $type->leveling->flasks->life $tier->t2") ==
		          u8"%H2 【練等·藥劑·生命】T2",
		          "T12 type+tier translated",
		          NeverSinkHeaderZh("%H2 $type->leveling->flasks->life $tier->t2"));
		rep.check(NeverSinkHeaderZh("$type->unknownseg->currency $tier->boots_life_based") ==
		          u8"【unknownseg·通貨】boots_life_based",
		          "T12 unknown segments stay English");
		rep.check(NeverSinkHeaderZh(u8"純註解標題 (no markers)") == u8"純註解標題 (no markers)",
		          "T12 non-NeverSink header untouched");
		rep.check(NeverSinkHeaderZh("$type->anyremaining $tier->restex") ==
		          u8"【其餘所有】其餘",
		          "T12 tier keyword translated");
	}

	// ---- T13: drop-preview evaluator --------------------------------------
	{
		const char* src =
			"Show\r\n"
			"\tBaseType == \"Divine Orb\"\r\n"
			"\tSetTextColor 71 255 0 255\r\n"
			"\tSetFontSize 45\r\n"
			"\tCustomAlertSound \"6veryvaluable.mp3\" 220\r\n"
			"\r\n"
			"Show\r\n"
			"\tHasExplicitMod \"Veiled\"\r\n"
			"\tSetTextColor 9 9 9\r\n"
			"\r\n"
			"Show\r\n"
			"\tContinue\r\n"
			"\tClass \"Currency\"\r\n"
			"\tPlayEffect Purple\r\n"
			"\r\n"
			"Show\r\n"
			"\tClass \"Currency\"\r\n"
			"\tSetTextColor 1 2 3\r\n"
			"\r\n"
			"Hide\r\n"
			"\tBaseType \"Scroll\"\r\n";
		FilterFile f = ParseFilter(src);
		FilterI18n emptyI18n;   // ClassNameEn falls back to the input

		PreviewItem divine; divine.baseType = "Divine Orb"; divine.classId = "Stackable Currency";
		PreviewResult r = EvaluatePreview(f, divine, emptyI18n);
		rep.check(r.matched && !r.hidden && r.text[0] == 71 && r.text[1] == 255 && r.fontSize == 45,
		          "T13 first match wins + style override");
		rep.check(r.customSound == "6veryvaluable.mp3" && r.customVol == 220,
		          "T13 custom sound + volume captured");

		PreviewItem chaos; chaos.baseType = "Chaos Orb"; chaos.classId = "Stackable Currency";
		PreviewResult rc = EvaluatePreview(f, chaos, emptyI18n);
		rep.check(rc.matched && rc.text[0] == 1 && rc.text[1] == 2 && rc.text[2] == 3,
		          "T13 substring Class match decides");
		rep.check(rc.playEffect == "Purple", "T13 Continue block styles then keeps going");
		bool sawUnknown = false;
		for (const std::string& k : rc.unknownConds) if (k == "HasExplicitMod") sawUnknown = true;
		rep.check(sawUnknown, "T13 unmodelled condition reported + treated false");

		PreviewItem wis; wis.baseType = "Scroll of Wisdom"; wis.classId = "Quest Items";
		PreviewResult rw = EvaluatePreview(f, wis, emptyI18n);
		rep.check(rw.matched && rw.hidden, "T13 Hide block hides");

		PreviewItem belt; belt.baseType = "Leather Belt"; belt.classId = "Belts"; belt.rarity = 2;
		PreviewResult ru = EvaluatePreview(f, belt, emptyI18n);
		rep.check(!ru.matched && !ru.hidden && ru.text[0] == 255 && ru.text[1] == 255 && ru.text[2] == 119,
		          "T13 no match -> rarity default style");

		PreviewItem synth;
		bool okS = SynthesizePreviewItem(f, 0, emptyI18n, {}, &synth);
		rep.check(okS && synth.baseType == "Divine Orb",
		          "T13 synthesize from exact BaseType block");
		PreviewResult rs = EvaluatePreview(f, synth, emptyI18n);
		rep.check(rs.matched && rs.blockIdx == 0, "T13 synthesized item lands on its block");
		rep.check(!SynthesizePreviewItem(f, 1, emptyI18n, {}, &synth),
		          "T13 synthesize refuses unmodelled block");
	}

	// ---- T14: item-import data layer (base metadata + zh parse tables) ----
	// Needs Data\ next to the exe (run from dist\, like the atlas selftest).
	{
		FilterI18n i18n;
		i18n.Load(exeDir, "zh-rTW");
		rep.check(i18n.loaded(), "T14 i18n loaded (run from dist\\ so Data\\ exists)");

		rep.check(i18n.DropLevelOf("Thicket Bow") == 56, "T14 Thicket Bow drop level 56",
		          std::to_string(i18n.DropLevelOf("Thicket Bow")));
		int w = 0, h = 0;
		rep.check(i18n.SizeOf("Thicket Bow", &w, &h) && w == 2 && h == 3,
		          "T14 Thicket Bow inventory 2x3");
		rep.check(i18n.DropLevelOf("Headhunter") == -1,
		          "T14 unique without drop data -> -1 (caller keeps default)");
		rep.check(!i18n.SizeOf("Abyss Scarab of Crystals", &w, &h),
		          "T14 GGPK-patched entry without size -> false (caller keeps default)");
		rep.check(i18n.DropLevelOf("No Such Item") == -1 && !i18n.SizeOf("No Such Item", &w, &h),
		          "T14 unknown item -> unknown metadata");

		const FilterI18n::ZhTables& z = i18n.Zh();
		rep.check(!z.header.empty() && !z.rarity.empty() && !z.status.empty() &&
		          !z.influence.empty() && !z.itemClass.empty(),
		          "T14 all five zh tables non-empty");
		auto eq = [](const std::unordered_map<std::string, std::string>& m,
		             const char* k, const char* v) {
			auto it = m.find(k);
			return it != m.end() && it->second == v;
		};
		rep.check(eq(z.header, u8"物品種類", "Item Class"), "T14 header table zh->en");
		rep.check(eq(z.rarity, u8"稀有", "Rare"), "T14 rarity table zh->en");
		rep.check(eq(z.status, u8"已汙染", "Corrupted"), "T14 status table zh->en");
		rep.check(eq(z.influence, u8"塑者之物", "Shaper Item"), "T14 influence table zh->en");
		rep.check(eq(z.header, u8"地圖階級", "Map Tier"), "T14 Map Tier supplement");
		rep.check(eq(z.header, u8"堆疊數量", "Stack Size"), "T14 Stack Size supplement");
		rep.check(eq(z.itemClass, u8"深淵珠寶", "Abyss Jewels"), "T14 Abyss Jewels supplement");
	}

	// ---- T15: pasted-item parser — real zh fixtures -----------------------
	{
		FilterI18n i18n;
		i18n.Load(exeDir, "zh-rTW");
		ItemLibrary libLoader;
		libLoader.Load(exeDir, i18n);
		const std::vector<LibItem>& lib = libLoader.items();
		rep.check(!lib.empty(), "T15 item library loaded");

		ImportedItem b = ParseGameItemText(kFxBoots, i18n, lib);
		rep.check(b.ok, "T15 boots parsed");
		rep.check(b.item.rarity == 2 && b.item.itemLevel == 84 && b.item.quality == 29,
		          "T15 boots rarity/ilvl/quality");
		rep.check(b.baseEn == "Paladin Boots" && b.item.classId == "Boots",
		          "T15 boots base + class", b.baseEn + "/" + b.item.classId);
		rep.check(b.item.sockets == 4 && b.item.linkedSockets == 2 &&
		          b.item.socketGroups.size() == 3 && b.item.socketGroups[1] == "GW",
		          "T15 boots sockets R G-W R -> {R,GW,R}");
		rep.check(b.item.influence == 0 && b.exarch && b.eater,
		          "T15 boots exarch/eater flagged, no HasInfluence bits");
		rep.check(b.item.dropLevel == 84 && b.item.width == 2 && b.item.height == 2,
		          "T15 boots drop level + size from item_meta");
		rep.check(b.item.identified && !b.item.corrupted, "T15 boots identified, not corrupted");
		bool warnedExarch = false;
		for (const ImportIssue& w : b.warnings)
			if (w.msg.find("HasSearingExarchImplicit") != std::string::npos) warnedExarch = true;
		rep.check(warnedExarch, "T15 exarch mark surfaced as warning");

		ImportedItem fl = ParseGameItemText(kFxFlask, i18n, lib);
		rep.check(fl.ok && fl.item.rarity == 3 && fl.baseEn == "Silver Flask" &&
		          fl.item.enchanted && fl.item.quality == 20 && fl.item.itemLevel == 85,
		          "T15 unique flask + enchant line", fl.baseEn);

		ImportedItem jw = ParseGameItemText(kFxJewel, i18n, lib);
		rep.check(jw.ok && jw.item.fractured && jw.baseEn == "Crimson Jewel" &&
		          jw.item.itemLevel == 69 && jw.item.sockets == 0,
		          "T15 fractured jewel marker in its own section", jw.baseEn);

		ImportedItem vb = ParseGameItemText(kFxVestigialBoots, i18n, lib);
		rep.check(vb.ok && vb.baseEn == "Stealth Boots" && vb.item.rarity == 3,
		          "T15 vestigial prefix stripped to real base", vb.baseEn);
		rep.check(vb.item.sockets == 3 && vb.item.linkedSockets == 3 &&
		          vb.item.socketGroups.size() == 1 && vb.item.socketGroups[0] == "WWW",
		          "T15 sockets W-W-W with trailing space tolerated");
	}

	// ---- T16: pasted-item parser — en client text -------------------------
	{
		FilterI18n i18n;
		i18n.Load(exeDir, "zh-rTW");
		ItemLibrary libLoader;
		libLoader.Load(exeDir, i18n);
		const std::vector<LibItem>& lib = libLoader.items();

		const char* enRare =
			"Item Class: Body Armours\n"
			"Rarity: Rare\n"
			"Corpse Shell\n"
			"Vaal Regalia\n"
			"--------\n"
			"Quality: +28% (augmented)\n"
			"Energy Shield: 505 (augmented)\n"
			"--------\n"
			"Requirements:\n"
			"Level: 68\n"
			"Int: 194\n"
			"--------\n"
			"Sockets: R-G-B W-W G\n"
			"--------\n"
			"Item Level: 86\n"
			"--------\n"
			"+1 to Level of Socketed Gems\n"
			"--------\n"
			"Corrupted\n";
		ImportedItem e = ParseGameItemText(enRare, i18n, lib);
		rep.check(e.ok && e.baseEn == "Vaal Regalia" && e.item.rarity == 2 &&
		          e.item.itemLevel == 86 && e.item.quality == 28 && e.item.corrupted,
		          "T16 en rare parsed", e.baseEn);
		rep.check(e.item.sockets == 6 && e.item.linkedSockets == 3 &&
		          e.item.socketGroups.size() == 3 && e.item.socketGroups[0] == "RGB",
		          "T16 en sockets R-G-B W-W G");
		rep.check(e.item.gemLevel == 1, "T16 requirements Level is NOT the gem level");

		const char* enGem =
			"Item Class: Skill Gems\n"
			"Rarity: Gem\n"
			"Superior Spectral Shield Throw\n"
			"--------\n"
			"Attack, Projectile, Physical\n"
			"Level: 20 (Max)\n"
			"Quality: +20% (augmented)\n"
			"--------\n"
			"Requirements:\n"
			"Level: 70\n"
			"Dex: 155\n"
			"--------\n"
			"Deals weapon damage as projectiles\n";
		ImportedItem g = ParseGameItemText(enGem, i18n, lib);
		rep.check(g.ok && g.baseEn == "Spectral Shield Throw" && g.item.gemLevel == 20 &&
		          g.item.quality == 20 && g.item.rarity == 0,
		          "T16 superior gem: level from its own section", g.baseEn);
		bool noIlvl = false;
		for (const ImportIssue& w : g.warnings)
			if (w.msg.find(u8"物品等級") != std::string::npos) noIlvl = true;
		rep.check(noIlvl, "T16 missing Item Level warned (gems have none)");

		const char* enCurrency =
			"Item Class: Stackable Currency\n"
			"Rarity: Currency\n"
			"Chaos Orb\n"
			"--------\n"
			"Stack Size: 5/20\n"
			"--------\n"
			"Reforges a rare item with new random modifiers\n";
		ImportedItem c = ParseGameItemText(enCurrency, i18n, lib);
		rep.check(c.ok && c.baseEn == "Chaos Orb" && c.item.stackSize == 5 &&
		          c.item.rarity == 0, "T16 currency Stack Size 5/20 -> 5", c.baseEn);

		const char* enMap =
			"Item Class: Maps\n"
			"Rarity: Normal\n"
			"Blighted Cage Map\n"
			"--------\n"
			"Map Tier: 14 (augmented)\n"
			"--------\n"
			"Item Level: 79\n";
		ImportedItem m = ParseGameItemText(enMap, i18n, lib);
		rep.check(m.ok && m.baseEn == "Cage Map" && m.item.blightedMap &&
		          m.item.mapTier == 14, "T16 Blighted prefix + Map Tier", m.baseEn);

		const char* enReplica =
			"Item Class: Belts\n"
			"Rarity: Unique\n"
			"Replica Headhunter\n"
			"Leather Belt\n"
			"--------\n"
			"Requirements:\n"
			"Level: 40\n"
			"--------\n"
			"Item Level: 83\n";
		ImportedItem rp = ParseGameItemText(enReplica, i18n, lib);
		rep.check(rp.ok && rp.baseEn == "Leather Belt" && rp.item.replica &&
		          rp.item.rarity == 3, "T16 Replica flag from the name line", rp.baseEn);

		const char* enMagic =
			"Item Class: Belts\n"
			"Rarity: Magic\n"
			"Sharpened Rustic Sash of the Whelpling\n"
			"--------\n"
			"Item Level: 12\n";
		ImportedItem mg = ParseGameItemText(enMagic, i18n, lib);
		rep.check(mg.ok && mg.baseEn == "Rustic Sash" && mg.item.rarity == 1,
		          "T16 magic composed name -> longest base substring", mg.baseEn);
	}

	// ---- T17: evaluator extensions (SocketGroup + influence bitmask) ------
	{
		FilterI18n emptyI18n;
		auto evalOne = [&emptyI18n](const std::string& cond, const PreviewItem& it) {
			FilterFile f = ParseFilter("Show\n\t" + cond + "\n\tSetFontSize 45\n");
			return EvaluatePreview(f, it, emptyI18n);
		};

		PreviewItem it;
		it.baseType = "Cobalt Jewel";
		it.classId = "Jewel";
		it.socketGroups = { "R", "GW", "R" };

		PreviewItem rgb = it;
		rgb.socketGroups = { "BGR", "W" };          // one linked group holding R+G+B

		rep.check(evalOne("SocketGroup \"RGB\"", rgb).matched,
		          "T17 SocketGroup RGB matches linked R+G+B in any order");
		rep.check(!evalOne("SocketGroup \"RGB\"", it).matched,
		          "T17 SocketGroup RGB needs the colours in ONE linked group");
		rep.check(evalOne("SocketGroup ! \"RGB\"", it).matched,
		          "T17 SocketGroup negation");
		PreviewItem five = it;
		five.socketGroups = { "GGGGR" };
		rep.check(evalOne("SocketGroup \"5GGG\"", five).matched,
		          "T17 SocketGroup 5GGG: 5-linked with 3 greens matches");
		rep.check(!evalOne("SocketGroup \"5GGG\"", rgb).matched,
		          "T17 SocketGroup 5GGG: 3-link does not match");
		rep.check(evalOne("SocketGroup == \"RGB\"", rgb).matched &&
		          !evalOne("SocketGroup == \"RG\"", rgb).matched,
		          "T17 SocketGroup == exact multiset");
		PreviewItem bare = it;
		bare.socketGroups.clear();
		rep.check(!evalOne("SocketGroup \"RGB\"", bare).matched,
		          "T17 unknown socket groups never match (plain-drop semantics)");
		rep.check(evalOne("SocketGroup \"RGB\"", bare).unknownConds.empty(),
		          "T17 empty socket groups are modelled, not 'unknown'");

		PreviewItem shaped = it;
		shaped.influence = kInfShaper;
		rep.check(evalOne("HasInfluence Shaper", shaped).matched,
		          "T17 HasInfluence bit match");
		rep.check(!evalOne("HasInfluence Elder", shaped).matched,
		          "T17 HasInfluence wrong bit no match");
		rep.check(evalOne("HasInfluence Elder Shaper", shaped).matched,
		          "T17 HasInfluence any-of");
		rep.check(evalOne("HasInfluence ! Shaper", it).matched &&
		          !evalOne("HasInfluence ! Shaper", shaped).matched,
		          "T17 HasInfluence negation");
		rep.check(evalOne("HasInfluence None", it).matched &&
		          !evalOne("HasInfluence None", shaped).matched,
		          "T17 HasInfluence None == uninfluenced (old behaviour kept)");
		rep.check(evalOne("ShaperItem True", shaped).matched &&
		          !evalOne("ElderItem True", shaped).matched &&
		          evalOne("ElderItem False", shaped).matched,
		          "T17 Shaper/ElderItem read the bitmask");

		PreviewResult ru = evalOne("Sockets >= \"AAAA\"", five);
		bool sawSockets = false;
		for (const std::string& k : ru.unknownConds) if (k == "Sockets") sawSockets = true;
		rep.check(!ru.matched && sawSockets,
		          "T17 Sockets colour syntax stays unmodelled + reported");
	}

	// ---- T18: end-to-end — pasted zh boots through a mini filter ----------
	{
		FilterI18n i18n;
		i18n.Load(exeDir, "zh-rTW");
		ItemLibrary libLoader;
		libLoader.Load(exeDir, i18n);
		ImportedItem b = ParseGameItemText(kFxBoots, i18n, libLoader.items());
		rep.check(b.ok, "T18 boots parsed");

		const char* src =
			"Show\n"
			"\tHasExplicitMod \"Veiled\"\n"
			"\tSetFontSize 40\n"
			"\n"
			"Show\n"
			"\tSocketGroup \"RGB\"\n"
			"\tSetFontSize 41\n"
			"\n"
			"Show\n"
			"\tHasInfluence Shaper Elder\n"
			"\tSetFontSize 42\n"
			"\n"
			"Show\n"
			"\tContinue\n"
			"\tDropLevel >= 65\n"
			"\tPlayEffect Purple\n"
			"\n"
			"Show\n"
			"\tRarity Rare\n"
			"\tItemLevel >= 80\n"
			"\tSetTextColor 1 2 3\n";
		FilterFile f = ParseFilter(src);
		PreviewResult res = EvaluatePreview(f, b.item, i18n);
		rep.check(res.matched && res.blockIdx == 4 && res.text[0] == 1 && res.text[1] == 2,
		          "T18 lands on Rare+ilvl block, skips unmodelled/unmatched ones",
		          std::to_string(res.blockIdx));
		rep.check(res.playEffect == "Purple",
		          "T18 DropLevel 84 passes the Continue block, beam applied");
		bool sawHem = false;
		for (const std::string& k : res.unknownConds) if (k == "HasExplicitMod") sawHem = true;
		rep.check(sawHem, "T18 HasExplicitMod reported as not simulated");
	}

	// ---- T19: baseline snapshot + "modified" ------------------------------
	{
		const std::string src = synthetic_crlf();
		FilterFile f = ParseFilter(src);
		FilterDocumentEditor doc;
		doc.Attach(&f);
		const std::string before = SerializeFilter(f);
		doc.CaptureBaseline();
		rep.check(doc.HasBaseline() && SerializeFilter(f) == before && before == src,
		          "T19 capturing the baseline leaves the bytes alone");
		bool allSame = true;
		for (int i = 0; i < (int)f.blocks.size(); i++) allSame &= doc.BlockState(i) == BlockChange::Same;
		rep.check(allSame && doc.UnsavedBlockCount(false) == 0, "T19 fresh baseline: every block Same, 0 unsaved");

		const int fs = doc.FindLine(0, "SetFontSize");
		FilterSetValueInt(f.lines[fs], 0, 40);
		f.dirty = true;
		rep.check(doc.BlockState(0) == BlockChange::Modified && doc.BlockState(1) == BlockChange::Same &&
		          doc.BlockState(2) == BlockChange::Same && doc.BlockState(3) == BlockChange::Same,
		          "T19 a value edit marks only its block Modified");
		const FilterLine* bl = doc.BaselineLine(fs);
		rep.check(bl && FilterValueInt(*bl, 0, 0) == 45 && doc.LineChanged(fs),
		          "T19 the baseline line still says 45 (shown as 原本)",
		          bl ? FilterSerializeLine(*bl) : std::string("null"));
		rep.check(!doc.LineChanged(doc.FindLine(0, "SetTextColor")), "T19 an untouched line is not changed");
		rep.check(doc.UnsavedBlockCount(false) == 1, "T19 unsaved count = 1 block");

		FilterSetValueInt(f.lines[fs], 0, 45);
		rep.check(doc.BlockState(0) == BlockChange::Same, "T19 typing the old value back is Same again");
		rep.check(doc.UnsavedBlockCount(true) == 0 && !f.dirty, "T19 ...and settling marks the file clean");

		const int rl = doc.FindLine(0, "Rarity");
		doc.CommentOutLine(rl);
		f.dirty = true;
		const FilterLine* rb = doc.BaselineLine(rl);
		rep.check(doc.BlockState(0) == BlockChange::Modified && rb && rb->kind == FilterLineKind::Condition,
		          "T19 a disabled (#!) line keeps its identity: the baseline is the live line");
		doc.RestoreLine(rl);
		rep.check(doc.BlockState(0) == BlockChange::Same, "T19 restoring it is Same again");
	}

	// ---- T20: restore one block -> byte-exact round-trip -------------------
	{
		const std::string src = synthetic_crlf();
		FilterFile f = ParseFilter(src);
		FilterDocumentEditor doc;
		doc.Attach(&f);
		doc.CaptureBaseline();
		// block 0: a value edit, an inserted line, a disabled line, Show -> Hide
		FilterSetValueInt(f.lines[doc.FindLine(0, "SetFontSize")], 0, 30);
		doc.InsertLine(0, "Quality", ">=", { FilterToken{ "20", false } });
		doc.CommentOutLine(doc.FindLine(0, "Rarity"));
		{
			FilterLine& h = f.lines[f.blocks[0].headerLineIdx];
			h.keyword = "Hide";
			h.dirty = true;
			f.blocks[0].hide = true;
		}
		// block 3: Hide -> Show
		{
			FilterLine& h = f.lines[f.blocks[3].headerLineIdx];
			h.keyword = "Show";
			h.dirty = true;
			f.blocks[3].hide = false;
		}
		f.dirty = true;
		rep.check(doc.BlockState(0) == BlockChange::Modified && doc.BlockState(3) == BlockChange::Modified &&
		          doc.UnsavedBlockCount(false) == 2, "T20 two blocks edited");
		const int nb = doc.RestoreBlock(0);
		rep.check(nb == 0 && doc.BlockState(0) == BlockChange::Same && !f.blocks[0].hide,
		          "T20 RestoreBlock puts block 0 back (index kept)", std::to_string(nb));
		rep.check(doc.BlockState(3) == BlockChange::Modified && f.dirty, "T20 ...and leaves the other block alone");
		doc.RestoreBlock(3);
		rep.check(SerializeFilter(f) == src, "T20 every edited block restored: the file is byte-for-byte the original");
		rep.check(!f.dirty && doc.UnsavedBlockCount(true) == 0, "T20 ...and clean again");
		// the restored lines are real lines again: edit + restore once more
		FilterSetValueInt(f.lines[doc.FindLine(0, "SetFontSize")], 0, 33);
		f.dirty = true;
		doc.RestoreBlock(0);
		rep.check(SerializeFilter(f) == src && !f.dirty, "T20 restore is repeatable");
	}

	// ---- T21: a new custom rule is Added, not Modified ----------------------
	{
		const std::string src = synthetic_crlf();
		FilterFile f = ParseFilter(src);
		FilterDocumentEditor doc;
		doc.Attach(&f);
		doc.CaptureBaseline();
		CustomZone z = EnsureCustomZone(doc);
		const int nb = doc.CreateBlockAtLine(z.endLine, false, u8"PobTools custom rule");
		doc.InsertLine(nb, "BaseType", "", { FilterToken{ "Divine Orb", true } });
		rep.check(nb >= 0 && doc.BlockState(nb) == BlockChange::Added, "T21 new custom rule is Added (新增)");
		bool othersSame = true;
		for (int i = 0; i < (int)f.blocks.size(); i++)
			if (i != nb) othersSame &= doc.BlockState(i) == BlockChange::Same;
		rep.check(othersSame, "T21 the NeverSink blocks stay Same");
		rep.check(doc.RestoreBlock(nb) == -1, "T21 an added block has nothing to restore");
		rep.check(doc.UnsavedBlockCount(false) == 1, "T21 unsaved count = 1");
		const int nlines = 0;
		(void)nlines;
		doc.RemoveBlock(nb);
		rep.check(doc.RemovedBaselineBlocks() == 0 && doc.UnsavedBlockCount(true) == 1 && f.dirty,
		          "T21 deleting it again still leaves the zone markers unsaved");
		const int dup = doc.DuplicateBlock(1);
		rep.check(dup >= 0 && doc.BlockState(dup) == BlockChange::Added && doc.BlockState(1) == BlockChange::Same,
		          "T21 a duplicated block is Added, its source stays Same");
	}

	// ---- T22/T23/T24 need a scratch folder ---------------------------------
	wchar_t tmpBuf2[MAX_PATH] = L"";
	GetTempPathW(MAX_PATH, tmpBuf2);
	const std::wstring scratch = std::wstring(tmpBuf2) + L"pobtools_fe_st_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
	CreateDirectoryW(scratch.c_str(), nullptr);

	// ---- T22: external modification ----------------------------------------
	{
		const std::wstring path = scratch + L"watch.filter";
		auto writeFile = [](const std::wstring& p, const std::string& data) {
			std::ofstream o(p, std::ios::binary);
			o.write(data.data(), (std::streamsize)data.size());
		};
		auto bumpTime = [](const std::wstring& p, long long secs) {
			HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) return false;
			FILETIME ft{};
			GetFileTime(h, nullptr, nullptr, &ft);
			ULARGE_INTEGER u;
			u.LowPart = ft.dwLowDateTime;
			u.HighPart = ft.dwHighDateTime;
			u.QuadPart += (unsigned long long)(secs * 10000000LL);
			ft.dwLowDateTime = u.LowPart;
			ft.dwHighDateTime = u.HighPart;
			const bool ok = SetFileTime(h, nullptr, nullptr, &ft) != 0;
			CloseHandle(h);
			return ok;
		};
		writeFile(path, synthetic_crlf());

		ExternalChangeWatch w;
		w.Reset(path);
		rep.check(!w.Poll(0.0), "T22 nothing changed yet");
		const bool bumped = bumpTime(path, 10);
		rep.check(bumped && !w.Poll(0.5), "T22 polls at most every 2 s");
		rep.check(w.Poll(2.5) && w.changed(), "T22 a new write time (outside write) is detected");
		w.Acknowledge();
		rep.check(!w.Poll(5.0), "T22 dismissed: the current stamp is the new normal");

		// the editor's own save must not report itself (EditorShell::Save shows
		// a toast, so it runs inside a scratch ImGui context)
		ImGuiContext* prevCtx = ImGui::GetCurrentContext();
		ImGuiContext* ctx = ImGui::CreateContext();
		ImGui::SetCurrentContext(ctx);
		{
			EditorShell sh;
			sh.exeDir = scratch;
			sh.testMode = true;   // no ini writes
			const bool opened = sh.OpenByPath(path, true);
			rep.check(opened && sh.watch.path() == path && sh.doc.HasBaseline(), "T22 OpenByPath starts the watch and the baseline");
			FilterSetValueInt(sh.model.lines[sh.doc.FindLine(0, "SetFontSize")], 0, 41);
			sh.model.dirty = true;
			rep.check(sh.UnsavedCount() == 1, "T22 one unsaved block before saving");
			const bool saved = sh.Save();
			rep.check(saved && !sh.watch.Poll(100.0), "T22 our own save is not reported as an outside change");
			rep.check(sh.UnsavedCount() == 0 && !sh.model.dirty &&
			          sh.doc.BlockState(0) == BlockChange::Same, "T22 after saving, the saved file is the new baseline");
			bumpTime(path, 20);
			rep.check(sh.watch.Poll(200.0), "T22 an outside write after our save is still caught");
		}
		ImGui::DestroyContext(ctx);
		ImGui::SetCurrentContext(prevCtx);
		DeleteFileW(path.c_str());
		DeleteFileW((path + L".bak").c_str());
	}

	// ---- T23: filter chip counts ---------------------------------------------
	{
		EditorShell sh;
		sh.testMode = true;
		sh.model = ParseFilter(synthetic_crlf());
		sh.loaded = true;
		sh.doc.Attach(&sh.model);
		sh.doc.CaptureBaseline();
		EdRebuildRows(sh);
		const ChipCounts c0 = sh.chipCounts;
		rep.check(c0.all == 4 && c0.shown == 3 && c0.hidden == 1 && c0.changed == 0 && c0.custom == 0,
		          "T23 counts on load: 4 all / 3 shown / 1 hidden / 0 changed / 0 custom",
		          std::to_string(c0.all) + "/" + std::to_string(c0.shown) + "/" + std::to_string(c0.hidden) + "/" +
		              std::to_string(c0.changed) + "/" + std::to_string(c0.custom));
		// a custom rule (added) + a value edit (modified)
		CustomZone z = EnsureCustomZone(sh.doc);
		const int nb = sh.doc.CreateBlockAtLine(z.endLine, false, u8"PobTools custom rule");
		sh.doc.InsertLine(nb, "BaseType", "", { FilterToken{ "Divine Orb", true } });
		EdRebuildRows(sh);
		int vaal = -1;
		for (int i = 0; i < (int)sh.model.blocks.size(); i++)
			for (int li : sh.model.blocks[i].lineIdx)
				if (sh.model.lines[li].keyword == "BaseType" && FilterHasValue(sh.model.lines[li], "Vaal Orb")) vaal = i;
		FilterSetValueInt(sh.model.lines[sh.doc.FindLine(vaal, "SetFontSize")], 0, 36);
		sh.model.dirty = true;
		EdRefreshRowStates(sh);
		const ChipCounts c1 = sh.chipCounts;
		rep.check(c1.all == 5 && c1.shown == 4 && c1.hidden == 1 && c1.changed == 2 && c1.custom == 1,
		          "T23 after an add + an edit: 5 / 4 / 1 / 2 changed / 1 custom",
		          std::to_string(c1.all) + "/" + std::to_string(c1.shown) + "/" + std::to_string(c1.hidden) + "/" +
		              std::to_string(c1.changed) + "/" + std::to_string(c1.custom));
		sh.chip = RuleChip::Changed;
		EdRebuildVisRows(sh);
		rep.check((int)sh.visRows.size() == 2, "T23 the 已修改 chip lists exactly those two", std::to_string(sh.visRows.size()));
		sh.chip = RuleChip::Hidden;
		EdRebuildVisRows(sh);
		rep.check((int)sh.visRows.size() == 1 && sh.model.blocks[sh.visRows[0]].hide, "T23 the 隱藏 chip lists the Hide block");
		// hiding one more block under the 隱藏 chip updates the list by itself
		SetBlockHide(sh, sh.model.blocks[vaal], true);
		EdRefreshRowStates(sh);
		rep.check((int)sh.visRows.size() == 2 && sh.chipCounts.hidden == 2, "T23 a show/hide change refreshes counts and the chip list");
		sh.chip = RuleChip::Custom;
		EdRebuildVisRows(sh);
		rep.check((int)sh.visRows.size() == 1 && sh.rows[sh.visRows[0]].custom, "T23 the 自訂 chip lists the custom rule");
		// visList: group headings interleaved, never a heading without a rule after it
		sh.chip = RuleChip::All;
		EdRebuildVisRows(sh);
		bool headingsOk = !sh.visList.empty() && sh.visList.front().block < 0;
		for (size_t i = 0; i + 1 < sh.visList.size(); i++)
			if (sh.visList[i].block < 0 && sh.visList[i + 1].block < 0) headingsOk = false;
		rep.check(headingsOk && sh.visList.back().block >= 0, "T23 the list interleaves group headings correctly");
	}

	// ---- T24: recently used colours --------------------------------------------
	{
		std::vector<std::uint32_t> list;
		for (int i = 0; i < 10; i++) {
			const int c[4] = { i, 10 + i, 20 + i, 255 };
			PushRecentColor(list, c);
		}
		const int again[4] = { 7, 17, 27, 255 };
		PushRecentColor(list, again);
		rep.check((int)list.size() == kRecentColorMax && list[0] == ((7u << 24) | (17u << 16) | (27u << 8) | 255u) &&
		          list[1] == ((9u << 24) | (19u << 16) | (29u << 8) | 255u),
		          "T24 newest first, at most 8, a reused colour moves to the front without a duplicate");
		int dups = 0;
		for (size_t i = 0; i < list.size(); i++)
			for (size_t j = i + 1; j < list.size(); j++) if (list[i] == list[j]) dups++;
		rep.check(dups == 0, "T24 no duplicates");

		EditorShell a;
		a.exeDir = scratch;
		a.recentColors = list;
		SaveEditorSettings(a);
		EditorShell b;
		b.exeDir = scratch;
		LoadEditorSettings(b);
		rep.check(b.recentColors == list, "T24 saved to pob-zh.ini and read back identically",
		          EncodeRecentColors(b.recentColors));
		rep.check(DecodeRecentColors("x;300,0,0,0;1,2,3,4;1,2,3,4") == std::vector<std::uint32_t>{ 0x01020304u },
		          "T24 garbage and out-of-range entries are dropped");
		EditorShell t;
		t.exeDir = scratch;
		t.testMode = true;
		t.recentColors = { 0xffffffffu };
		SaveEditorSettings(t);   // a test run writes nothing
		EditorShell r;
		r.exeDir = scratch;
		LoadEditorSettings(r);
		rep.check(r.recentColors == list, "T24 a test run does not write the ini");
		DeleteFileW((scratch + L"pob-zh.ini").c_str());
	}
	RemoveDirectoryW(scratch.c_str());
	rep.check(GetFileAttributesW(scratch.c_str()) == INVALID_FILE_ATTRIBUTES, "T24 the scratch folder is gone");

	// ---- T25: sound file names with a download suffix ------------------------
	{
		rep.check(SoundNameHasDownloadSuffix(L"6maps (1).mp3") && SoundNameHasDownloadSuffix(L"alert (12).wav") &&
		          !SoundNameHasDownloadSuffix(L"6maps.mp3") && !SoundNameHasDownloadSuffix(L"(1).mp3") &&
		          !SoundNameHasDownloadSuffix(L"map(1).mp3") && !SoundNameHasDownloadSuffix(L"map ().mp3"),
		          "T25 \" (n)\" download suffix detected, plain names not");
	}

	// ---- T26: right-click copy of one rule -----------------------------------
	{
		const FilterFile f = ParseFilter(
			"Show # $type->a\n\tClass \"Rings\"\n\t# note\n\tSetTextColor 255 0 0\n\tPlayAlertSound 3 300\n\n"
			"#=== next section ===\n"
			"Hide\n\tRarity Normal\n");
		rep.check(f.blocks.size() == 2, "T26 fixture parses into two blocks");
		if (f.blocks.size() == 2) {
			rep.check(FilterBlockText(f, f.blocks[0], false) ==
			              "Show # $type->a\r\n\tClass \"Rings\"\r\n\t# note\r\n\tSetTextColor 255 0 0\r\n\tPlayAlertSound 3 300",
			          "T26 whole rule keeps inner comments, drops trailing blank + next banner");
			rep.check(FilterBlockText(f, f.blocks[0], true) == "\tSetTextColor 255 0 0\r\n\tPlayAlertSound 3 300",
			          "T26 look = action lines only");
			rep.check(FilterBlockText(f, f.blocks[1], true).empty(), "T26 no action lines -> empty look");
		}
	}

	// ---- T27: paste a copied look onto another rule ---------------------------
	{
		FilterFile f = ParseFilter(
			"Show # a\n\tClass \"Rings\"\n\tSetTextColor 1 2 3\n\tCustomAlertSound \"x.mp3\" 300\n\n"
			"Show # b\n\tClass \"Amulets\"\n\tSetTextColor 9 9 9\n\tMinimapIcon 0 Red Star\n");
		FilterDocumentEditor doc;
		doc.Attach(&f);
		const std::string bBefore = FilterBlockText(f, f.blocks[1], false);
		const int n = CardPasteLook(doc, 0, "\tSetTextColor 9 9 9\r\n\tPlayAlertSound 6 300\r\n\tSetFontSize 45");
		rep.check(n == 3, "T27 paste reports the three action lines", "n=" + std::to_string(n));
		const int tc = doc.FindLine(0, "SetTextColor");
		rep.check(tc >= 0 && FilterSerializeLine(f.lines[tc]) == "\tSetTextColor 9 9 9", "T27 same keyword is replaced");
		rep.check(doc.FindLine(0, "PlayAlertSound") >= 0 && doc.FindLine(0, "SetFontSize") >= 0, "T27 missing keywords are inserted");
		bool customDisabled = false;
		for (int li : f.blocks[0].lineIdx) customDisabled |= doc.IsDisabledLine(li) && f.lines[li].raw.find("CustomAlertSound") != std::string::npos;
		rep.check(doc.FindLine(0, "CustomAlertSound") < 0 && customDisabled, "T27 the other sound kind is disabled with #!");
		rep.check(FilterBlockText(f, f.blocks[1], false) == bBefore, "T27 the other rule is untouched");
		rep.check(f.dirty, "T27 the file is marked edited");
		const std::string once = SerializeFilter(f);
		const int n2 = CardPasteLook(doc, 0, "\tSetTextColor 9 9 9\r\n\tPlayAlertSound 6 300\r\n\tSetFontSize 45");
		rep.check(n2 == 3 && SerializeFilter(f) == once, "T27 pasting the same look again changes nothing");
		rep.check(CardPasteLook(doc, 0, "Show\n\tClass \"Rings\"") == 0 && SerializeFilter(f) == once,
		          "T27 text without action lines pastes nothing");
		bool ch = true;
		rep.check(CardPasteLook(doc, 0, "Show\n\tSetFontSize 30\nShow\n\tSetFontSize 40", &ch) == -1 && !ch &&
		              SerializeFilter(f) == once,
		          "T27 more than one rule is refused untouched");
		// back again: the #! CustomAlertSound comes back instead of a second line
		CardPasteLook(doc, 0, "\tCustomAlertSound \"y.mp3\" 200", &ch);
		int customLines = 0;
		for (int li : f.blocks[0].lineIdx) customLines += FilterSerializeLine(f.lines[li]).find("CustomAlertSound") != std::string::npos;
		const int cl = doc.FindLine(0, "CustomAlertSound");
		rep.check(ch && customLines == 1 && cl >= 0 && FilterSerializeLine(f.lines[cl]) == "\tCustomAlertSound \"y.mp3\" 200" &&
		              doc.FindLine(0, "PlayAlertSound") < 0,
		          "T27 pasting a sound back restores its #! line (no doubled line)", "customLines=" + std::to_string(customLines));
		rep.check(CardPasteLook(doc, 1, "\tSetTextColor 1 1 1\n\tSetTextColor 2 2 2") == 1 &&
		              FilterSerializeLine(f.lines[doc.FindLine(1, "SetTextColor")]) == "\tSetTextColor 2 2 2",
		          "T27 one line per card, the last one wins");
	}

	// ---- T28: undo / redo history over the whole line list ---------------------
	{
		const std::string fx =
			"Show # a\n\tClass \"Rings\"\n\tSetTextColor 1 2 3\n\tSetFontSize 40\n\n"
			"Show # b\n\tBaseType \"Chaos Orb\"\n\tSetTextColor 4 5 6\n\tSetFontSize 30\n\n"
			"Hide # c\n\tRarity Normal\n";
		// Edit a card value the way the editor does: change the token, flag the line.
		auto setVal = [](FilterFile& file, int blockIdx, FilterDocumentEditor& d, const char* kw, size_t vi, const std::string& v) {
			const int li = d.FindLine(blockIdx, kw);
			if (li < 0 || vi >= file.lines[li].values.size()) return false;
			file.lines[li].values[vi].text = v;
			file.lines[li].dirty = true;
			file.dirty = true;
			return true;
		};

		// T28a / T28b: one value edit, undo, redo
		{
			FilterFile f = ParseFilter(fx);
			FilterDocumentEditor doc;
			doc.Attach(&f);
			FilterUndoHistory h;
			h.Reset(f.lines);
			const std::string before = SerializeFilter(f);
			const bool edited = setVal(f, 0, doc, "SetTextColor", 0, "200");
			const std::string after = SerializeFilter(f);
			rep.check(edited && after != before, "T28 fixture edit changes the file");
			const bool pushed = h.Capture(f.lines);
			rep.check(pushed && h.CanUndo() && !h.CanRedo(), "T28 capture after an edit pushes one step");
			const int u = h.Undo(f.lines);
			rep.check(u >= 0 && SerializeFilter(f) == before, "T28a undo restores the file before the edit", "u=" + std::to_string(u));
			rep.check(h.CanRedo() && !h.CanUndo(), "T28a after undo: redo possible, nothing left to undo");
			const int r = h.Redo(f.lines);
			rep.check(r >= 0 && SerializeFilter(f) == after, "T28b redo brings back the edited file", "r=" + std::to_string(r));
			rep.check(!h.Capture(f.lines), "T28b capture right after redo records nothing");
		}

		// T28c: inserting a line is undone, blocks come back too
		{
			FilterFile f = ParseFilter(fx);
			FilterDocumentEditor doc;
			doc.Attach(&f);
			FilterUndoHistory h;
			h.Reset(f.lines);
			const std::string before = SerializeFilter(f);
			const size_t linesBefore = f.lines.size();
			const size_t blocksBefore = f.blocks.size();
			FilterToken t1, t2, t3;
			t1.text = "7"; t2.text = "8"; t3.text = "9";
			const int ins = doc.InsertLine(0, "SetBorderColor", "", { t1, t2, t3 });
			rep.check(ins >= 0 && f.lines.size() == linesBefore + 1 && SerializeFilter(f) != before, "T28c line inserted");
			rep.check(h.Capture(f.lines), "T28c capture sees the structural change");
			h.Undo(f.lines);
			doc.RebuildBlocks();
			rep.check(SerializeFilter(f) == before && f.lines.size() == linesBefore, "T28c undo removes the inserted line");
			rep.check(f.blocks.size() == blocksBefore && doc.FindLine(0, "SetBorderColor") < 0, "T28c blocks rebuilt to the old shape");
		}

		// T28d: a removed custom rule comes back with the same text
		{
			FilterFile f = ParseFilter(fx);
			FilterDocumentEditor doc;
			doc.Attach(&f);
			const FilterFile src = ParseFilter(fx);
			std::string err;
			const int n = ImportCustomRules(doc, ExportCustomRules(src, { 0 }, "t28"), &err);
			rep.check(n == 1, "T28d fixture: one rule imported into the custom zone", err + " n=" + std::to_string(n));
			auto zoneBlock = [&]() {
				const CustomZone z = FindCustomZone(f);
				for (size_t i = 0; i < f.blocks.size(); i++)
					if (z.present() && f.blocks[i].headerLineIdx > z.beginLine && f.blocks[i].headerLineIdx < z.endLine) return (int)i;
				return -1;
			};
			FilterUndoHistory h;
			h.Reset(f.lines);
			const std::string before = SerializeFilter(f);
			const int zb = zoneBlock();
			rep.check(zb >= 0, "T28d the imported rule sits inside the zone");
			if (zb >= 0) {
				const std::string text = FilterBlockText(f, f.blocks[zb], false);
				doc.RemoveBlock(zb);
				rep.check(zoneBlock() < 0, "T28d removed rule is gone from the zone");
				rep.check(h.Capture(f.lines), "T28d capture sees the removed block");
				h.Undo(f.lines);
				doc.RebuildBlocks();
				const int back = zoneBlock();
				rep.check(back >= 0 && FilterBlockText(f, f.blocks[back], false) == text, "T28d undo brings the rule back with the same text");
				rep.check(SerializeFilter(f) == before, "T28d whole file equals the pre-removal file");
			}
		}

		// T28e: no change, no step
		{
			FilterFile f = ParseFilter(fx);
			FilterDocumentEditor doc;
			doc.Attach(&f);
			FilterUndoHistory h;
			h.Reset(f.lines);
			const bool c1 = h.Capture(f.lines);
			const bool c2 = h.Capture(f.lines);
			rep.check(!c1 && !c2 && !h.CanUndo() && h.UndoCount() == 0, "T28e capture without any edit records nothing");
			setVal(f, 0, doc, "SetFontSize", 0, "50");
			const bool c3 = h.Capture(f.lines);
			const bool c4 = h.Capture(f.lines);
			rep.check(c3 && !c4 && h.UndoCount() == 1, "T28e one edit is one step, a second capture adds none", "count=" + std::to_string(h.UndoCount()));
		}

		// T28f: a new edit after undo drops the redo side
		{
			FilterFile f = ParseFilter(fx);
			FilterDocumentEditor doc;
			doc.Attach(&f);
			FilterUndoHistory h;
			h.Reset(f.lines);
			setVal(f, 0, doc, "SetTextColor", 0, "100");
			h.Capture(f.lines);
			h.Undo(f.lines);
			rep.check(h.CanRedo(), "T28f after undo redo is possible");
			setVal(f, 1, doc, "SetFontSize", 0, "33");
			const bool pushed = h.Capture(f.lines);
			rep.check(pushed && !h.CanRedo() && h.Redo(f.lines) == -1, "T28f a fresh edit clears redo");
		}

		// T28g: the history is capped at kMaxSteps, oldest step dropped
		{
			FilterFile f = ParseFilter(fx);
			FilterDocumentEditor doc;
			doc.Attach(&f);
			FilterUndoHistory h;
			h.Reset(f.lines);
			std::string afterFirst;
			bool allPushed = true;
			for (int i = 1; i <= 101; i++) {
				setVal(f, 0, doc, "SetFontSize", 0, std::to_string(100 + i));
				allPushed &= h.Capture(f.lines);
				if (i == 1) afterFirst = SerializeFilter(f);
			}
			rep.check(allPushed && h.UndoCount() == FilterUndoHistory::kMaxSteps && FilterUndoHistory::kMaxSteps == 100,
			          "T28g 101 edits keep only 100 steps", "count=" + std::to_string(h.UndoCount()));
			int ok = 0;
			while (h.Undo(f.lines) >= 0 && ok < 200) ok++;
			rep.check(ok == 100 && !h.CanUndo(), "T28g exactly 100 undos succeed, then -1", "ok=" + std::to_string(ok));
			rep.check(SerializeFilter(f) == afterFirst, "T28g oldest reachable state is the one after the first edit");
		}

		// T28h: undoing a value edit puts the rule back to unchanged against the baseline
		{
			FilterFile f = ParseFilter(fx);
			FilterDocumentEditor doc;
			doc.Attach(&f);
			doc.CaptureBaseline();
			FilterUndoHistory h;
			h.Reset(f.lines);
			setVal(f, 0, doc, "SetTextColor", 0, "222");
			h.Capture(f.lines);
			rep.check(doc.BlockState(0) == BlockChange::Modified, "T28h edited rule is Modified");
			h.Undo(f.lines);
			doc.RebuildBlocks();
			rep.check(doc.BlockState(0) == BlockChange::Same && doc.BlockState(1) == BlockChange::Same,
			          "T28h after undo the rule is Same as the baseline again");
		}

		// T28i: Reset forgets everything
		{
			FilterFile f = ParseFilter(fx);
			FilterDocumentEditor doc;
			doc.Attach(&f);
			FilterUndoHistory h;
			h.Reset(f.lines);
			setVal(f, 0, doc, "SetTextColor", 0, "11");
			h.Capture(f.lines);
			setVal(f, 0, doc, "SetTextColor", 0, "12");
			h.Capture(f.lines);
			h.Undo(f.lines);
			rep.check(h.CanUndo() && h.CanRedo(), "T28i both directions open before reset");
			h.Reset(f.lines);
			rep.check(!h.CanUndo() && !h.CanRedo() && h.UndoCount() == 0, "T28i reset clears undo and redo");
			rep.check(!h.Capture(f.lines), "T28i the current lines are the new shadow");
		}
	}

	// ---- T29: copy a rule into the custom zone (right-click copy) --------------
	{
		FilterFile f = ParseFilter(
			"Show # %D3 $type->currency\n\tClass \"Currency\"\n\tBaseType \"Chaos Orb\"\n\tSetTextColor 255 0 0\n\tSetFontSize 45\n\n"
			"Hide # junk\n\tRarity Normal\n");
		FilterDocumentEditor doc;
		doc.Attach(&f);
		rep.check(f.blocks.size() == 2 && !FindCustomZone(f).present(), "T29 fixture: two rules, no custom zone yet");
		if (f.blocks.size() == 2) {
			const std::string orig = FilterBlockText(f, f.blocks[0], false);
			const std::string origHeader = f.blocks[0].headerComment;
			std::string err;
			const int n = ImportCustomRules(doc, orig, &err);
			rep.check(n == 1, "T29a copying one rule imports exactly one", err + " n=" + std::to_string(n));
			const CustomZone z = FindCustomZone(f);
			int inZone = 0, outside = -1;
			std::string zoneText;
			for (size_t i = 0; i < f.blocks.size(); i++) {
				const int h = f.blocks[i].headerLineIdx;
				if (z.present() && h > z.beginLine && h < z.endLine) { inZone++; zoneText = FilterBlockText(f, f.blocks[i], false); }
				else if (f.blocks[i].headerComment == origHeader) outside = (int)i;
			}
			rep.check(z.present() && inZone == 1 && f.blocks.size() == 3, "T29a the zone now holds one rule", "inZone=" + std::to_string(inZone));
			rep.check(zoneText == orig, "T29a the copy has the same text as the original");
			rep.check(outside >= 0 && FilterBlockText(f, f.blocks[outside], false) == orig, "T29a the original rule is unchanged");

			const int again = ImportCustomRules(doc, orig, &err);
			rep.check(again == 0, "T29b copying the same rule again adds nothing", "again=" + std::to_string(again));

			const std::string snap = SerializeFilter(f);
			const int look = ImportCustomRules(doc, "\tSetTextColor 1 2 3\r\n\tSetFontSize 20", &err);
			rep.check(look <= 0 && SerializeFilter(f) == snap, "T29c action lines without Show/Hide import nothing", "look=" + std::to_string(look));
		}
	}

	// ---- T30: paste a look onto a rule that has the keyword twice ---------------
	{
		FilterFile f = ParseFilter("Show # a\n\tClass \"Rings\"\n\tSetTextColor 1 2 3\n\tSetTextColor 4 5 6\n\tSetFontSize 40\n");
		FilterDocumentEditor doc;
		doc.Attach(&f);
		const int n = CardPasteLook(doc, 0, "\tSetTextColor 9 9 9");
		rep.check(n == 1, "T30 paste reports one action line", "n=" + std::to_string(n));
		int found = 0, nine = 0;
		if (!f.blocks.empty()) {
			for (int li : f.blocks[0].lineIdx) {
				if (f.lines[li].keyword != "SetTextColor") continue;
				found++;
				nine += FilterSerializeLine(f.lines[li]) == "\tSetTextColor 9 9 9";
			}
		}
		rep.check(found == 2 && nine == 2, "T30 both SetTextColor lines get the pasted value", "found=" + std::to_string(found) + " nine=" + std::to_string(nine));
	}

	// ---- T32: saving twice keeps the first save's edits; a save is no undo step --
	{
		wchar_t tb[MAX_PATH];
		GetTempPathW(MAX_PATH, tb);
		const std::wstring path = std::wstring(tb) + L"pobtools_t32_" + std::to_wstring(GetCurrentProcessId()) + L".filter";
		{
			std::ofstream o(path, std::ios::binary);
			o << "Show # a\r\n\tClass \"Rings\"\r\n\tSetFontSize 40\r\n\r\nShow # b\r\n\tClass \"Amulets\"\r\n\tSetFontSize 40\r\n";
		}
		bool ok = false;
		FilterFile f = LoadFilter(path, &ok);
		FilterDocumentEditor doc;
		doc.Attach(&f);
		FilterUndoHistory h;
		h.Reset(f.lines);
		auto setSize = [&](int bi, const char* v) {
			const int li = doc.FindLine(bi, "SetFontSize");
			if (li < 0) return;
			f.lines[li].values[0].text = v;
			f.lines[li].dirty = true;
			f.dirty = true;
		};
		std::string err;
		setSize(0, "45");
		h.Capture(f.lines);
		const bool s1 = SaveFilter(f, &err);
		h.Rebase(f.lines);
		rep.check(ok && s1 && !h.Capture(f.lines) && h.UndoCount() == 1, "T32 a save adds no undo step (only the edit is one)",
		          "steps=" + std::to_string(h.UndoCount()));
		setSize(1, "33");
		const bool s2 = SaveFilter(f, &err);
		bool ok2 = false;
		FilterFile back = LoadFilter(path, &ok2);
		FilterDocumentEditor d2;
		d2.Attach(&back);
		const int a = d2.FindLine(0, "SetFontSize"), b = d2.FindLine(1, "SetFontSize");
		const std::string av = a >= 0 ? FilterSerializeLine(back.lines[a]) : "", bv = b >= 0 ? FilterSerializeLine(back.lines[b]) : "";
		rep.check(s2 && ok2 && av == "\tSetFontSize 45" && bv == "\tSetFontSize 33",
		          "T32 the second save keeps the first save's edit on disk", "a=" + av + " b=" + bv);
		rep.check(FilterBlockText(f, f.blocks[0], true) == "\tSetFontSize 45", "T32 after a save the copied look is the new value");
		DeleteFileW(path.c_str());
		DeleteFileW((path + L".bak").c_str());
	}

	// ---- T33: move a custom rule up / down inside the zone ----------------------
	{
		// Zone with three rules; c2 ends with a #! disabled line; a blank line sits
		// between c1 and c2, a blank line + a plain note between c2 and c3; one
		// NeverSink-style rule outside the zone.
		const std::string fx =
			"# banner\n"
			"#pobtools-custom-begin v=1\n"
			"Show # c1\n\tClass \"Rings\"\n\tSetTextColor 1 1 1\n\tSetFontSize 40\n"
			"\n"
			"Show # c2\n\tBaseType \"Chaos Orb\"\n\tSetTextColor 2 2 2\n\tSetBorderColor 5 5 5\n\t#! SetFontSize 30\n"
			"\n"
			"# my note\n"
			"Show # c3\n\tClass \"Amulets\"\n\tSetTextColor 3 3 3\n"
			"#pobtools-custom-end\n"
			"\n"
			"Show # %D3 $type->currency\n\tClass \"Currency\"\n\tSetTextColor 255 0 0\n\tSetFontSize 45\n";
		auto zoneIdx = [](const FilterFile& file) {
			std::vector<int> v;
			const CustomZone z = FindCustomZone(file);
			for (size_t i = 0; i < file.blocks.size(); i++)
				if (z.present() && file.blocks[i].headerLineIdx > z.beginLine && file.blocks[i].headerLineIdx < z.endLine) v.push_back((int)i);
			return v;
		};
		auto zoneTexts = [&](const FilterFile& file) {
			std::vector<std::string> v;
			for (int bi : zoneIdx(file)) v.push_back(FilterBlockText(file, file.blocks[bi], false));
			return v;
		};
		auto countKind = [](const FilterFile& file, FilterLineKind k) {
			int c = 0;
			for (const FilterLine& l : file.lines) c += l.kind == k;
			return c;
		};
		auto countRaw = [](const FilterFile& file, const char* needle) {
			int c = 0;
			for (const FilterLine& l : file.lines) c += l.raw.find(needle) != std::string::npos;
			return c;
		};
		auto headerOf = [](const FilterFile& file, const char* label) {
			for (size_t i = 0; i < file.blocks.size(); i++)
				if (file.blocks[i].headerComment.find(label) != std::string::npos) return (int)i;
			return -1;
		};

		// fixture sanity
		{
			FilterFile f = ParseFilter(fx);
			const std::vector<int> z = zoneIdx(f);
			rep.check(f.blocks.size() == 4 && z.size() == 3 && z[2] == 2 && FindCustomZone(f).present(),
			          "T33 fixture: three rules in the zone, one outside", "blocks=" + std::to_string(f.blocks.size()) + " zone=" + std::to_string(z.size()));
		}

		// T33a: first rule down one
		{
			FilterFile f = ParseFilter(fx);
			FilterDocumentEditor doc;
			doc.Attach(&f);
			const std::vector<std::string> o = zoneTexts(f);
			const std::vector<int> z = zoneIdx(f);
			if (o.size() == 3 && z.size() == 3) {
				doc.MoveBlockAfter(z[0], z[1]);
				const std::vector<std::string> n = zoneTexts(f);
				rep.check(n.size() == 3 && n[0] == o[1] && n[1] == o[0] && n[2] == o[2],
				          "T33a first rule moved after the second: order c2, c1, c3, contents unchanged");
			}
		}

		// T33b: last rule up one
		{
			FilterFile f = ParseFilter(fx);
			FilterDocumentEditor doc;
			doc.Attach(&f);
			const std::vector<std::string> o = zoneTexts(f);
			const std::vector<int> z = zoneIdx(f);
			if (o.size() == 3 && z.size() == 3) {
				doc.MoveBlockBefore(z[2], z[1]);
				const std::vector<std::string> n = zoneTexts(f);
				rep.check(n.size() == 3 && n[0] == o[0] && n[1] == o[2] && n[2] == o[1],
				          "T33b last rule moved before the second: order c1, c3, c2, contents unchanged");
			}
		}

		// T33c: the #! line travels with its rule
		{
			FilterFile f = ParseFilter(fx);
			FilterDocumentEditor doc;
			doc.Attach(&f);
			const std::vector<int> z = zoneIdx(f);
			if (z.size() == 3) {
				doc.MoveBlockAfter(z[1], z[2]);
				const int c2 = headerOf(f, "c2"), c3 = headerOf(f, "c3");
				bool inC2 = false, inC3 = false;
				if (c2 >= 0) for (int li : f.blocks[c2].lineIdx) inC2 |= f.lines[li].raw.find("#! SetFontSize 30") != std::string::npos;
				if (c3 >= 0) for (int li : f.blocks[c3].lineIdx) inC3 |= f.lines[li].raw.find("#! SetFontSize 30") != std::string::npos;
				rep.check(inC2 && !inC3 && countRaw(f, "#! SetFontSize 30") == 1,
				          "T33c the #! line moves with its rule, none left behind", "inC2=" + std::to_string(inC2) + " inC3=" + std::to_string(inC3));
				// the zone still reads c1, c3, c2
				const std::vector<int> zn = zoneIdx(f);
				rep.check(zn.size() == 3 && zn[0] == headerOf(f, "c1") && zn[1] == c3 && zn[2] == c2, "T33c order is c1, c3, c2");
			}
		}

		// T33d: gaps, sentinels and the outside rule stay put
		{
			FilterFile f = ParseFilter(fx);
			FilterDocumentEditor doc;
			doc.Attach(&f);
			const std::vector<int> z = zoneIdx(f);
			if (z.size() == 3) {
				const CustomZone z0 = FindCustomZone(f);
				const int blanks = countKind(f, FilterLineKind::Blank);
				const int notes = countRaw(f, "# my note");
				const std::string beginRaw = f.lines[z0.beginLine].raw, endRaw = f.lines[z0.endLine].raw;
				const std::string outside = FilterBlockText(f, f.blocks[3], false);
				doc.MoveBlockAfter(z[0], z[2]);
				const CustomZone z1 = FindCustomZone(f);
				rep.check(countKind(f, FilterLineKind::Blank) == blanks && countRaw(f, "# my note") == notes && notes == 1,
				          "T33d blank lines and the plain note are neither lost nor duplicated");
				rep.check(z1.present() && z1.beginLine == z0.beginLine && z1.endLine == z0.endLine &&
				              f.lines[z1.beginLine].raw == beginRaw && f.lines[z1.endLine].raw == endRaw,
				          "T33d zone sentinels stay on the same lines, unchanged");
				rep.check(f.blocks.size() == 4 && FilterBlockText(f, f.blocks[3], false) == outside && headerOf(f, "%D3") == 3,
				          "T33d the rule outside the zone is unchanged and still last");
			}
		}

		// T33e: the return value points at the moved rule
		{
			FilterFile f = ParseFilter(fx);
			FilterDocumentEditor doc;
			doc.Attach(&f);
			const std::vector<std::string> o = zoneTexts(f);
			const std::vector<int> z = zoneIdx(f);
			if (o.size() == 3 && z.size() == 3) {
				const int r = doc.MoveBlockAfter(z[0], z[1]);
				rep.check(r >= 0 && r < (int)f.blocks.size() && FilterBlockText(f, f.blocks[r], false) == o[0],
				          "T33e MoveBlockAfter returns the new index of the moved rule", "r=" + std::to_string(r));
				const int r2 = doc.MoveBlockBefore(r, z[0]);
				rep.check(r2 >= 0 && r2 < (int)f.blocks.size() && FilterBlockText(f, f.blocks[r2], false) == o[0],
				          "T33e MoveBlockBefore returns the new index of the moved rule", "r2=" + std::to_string(r2));
				const std::string snap = SerializeFilter(f);
				rep.check(doc.MoveBlockAfter(r2, r2) == -1 && SerializeFilter(f) == snap, "T33e moving a rule onto itself returns -1 and changes nothing");
			}
		}

		// T33f: one moved rule counts as one unsaved change
		{
			FilterFile f = ParseFilter(fx);
			FilterDocumentEditor doc;
			doc.Attach(&f);
			doc.CaptureBaseline();
			const std::vector<int> z = zoneIdx(f);
			rep.check(!f.dirty && doc.UnsavedBlockCount(true) == 0, "T33f fresh baseline: nothing unsaved");
			if (z.size() == 3) {
				doc.MoveBlockAfter(z[0], z[1]);
				const int u = doc.UnsavedBlockCount(true);
				rep.check(u == 1 && f.dirty, "T33f one move = one unsaved rule and the file is dirty", "unsaved=" + std::to_string(u));
			}
		}

		// T33g: undo / redo of a move
		{
			FilterFile f = ParseFilter(fx);
			FilterDocumentEditor doc;
			doc.Attach(&f);
			FilterUndoHistory h;
			h.Reset(f.lines);
			const std::string before = SerializeFilter(f);
			const std::vector<int> z = zoneIdx(f);
			if (z.size() == 3) {
				doc.MoveBlockAfter(z[0], z[1]);
				const std::string after = SerializeFilter(f);
				rep.check(after != before && h.Capture(f.lines), "T33g a move changes the file and is captured as a step");
				h.Undo(f.lines);
				doc.RebuildBlocks();
				rep.check(SerializeFilter(f) == before, "T33g undo puts the rule back");
				h.Redo(f.lines);
				doc.RebuildBlocks();
				rep.check(SerializeFilter(f) == after, "T33g redo moves it again");
			}
		}

		// T33h: which rule a move goes past
		{
			FilterFile f = ParseFilter(fx);
			const std::vector<int> z = zoneIdx(f);
			if (z.size() == 3 && f.blocks.size() == 4) {
				const int out = 3;
				rep.check(CustomRuleMoveTarget(f, out, -1) == -1 && CustomRuleMoveTarget(f, out, 1) == -1 &&
				              CustomRuleMoveTarget(f, out, -2) == -1 && CustomRuleMoveTarget(f, out, 2) == -1,
				          "T33h a rule outside the zone cannot move in any direction");
				rep.check(CustomRuleMoveTarget(f, z[0], -1) == -1 && CustomRuleMoveTarget(f, z[0], -2) == -1,
				          "T33h the first custom rule cannot go up or to the top");
				rep.check(CustomRuleMoveTarget(f, z[2], 1) == -1 && CustomRuleMoveTarget(f, z[2], 2) == -1,
				          "T33h the last custom rule cannot go down or to the bottom");
				rep.check(CustomRuleMoveTarget(f, z[1], -1) == z[0] && CustomRuleMoveTarget(f, z[1], 1) == z[2],
				          "T33h the middle rule goes past its neighbours");
				rep.check(CustomRuleMoveTarget(f, z[1], -2) == z[0] && CustomRuleMoveTarget(f, z[1], 2) == z[2],
				          "T33h the middle rule to top / bottom goes past the first / last");
				rep.check(CustomRuleMoveTarget(f, z[0], 2) == z[2] && CustomRuleMoveTarget(f, z[0], 1) == z[1],
				          "T33h the first rule down goes past the second, to the bottom past the last");
				rep.check(CustomRuleMoveTarget(f, z[2], -2) == z[0] && CustomRuleMoveTarget(f, z[2], -1) == z[1],
				          "T33h the last rule up goes past the second, to the top past the first");
			}
		}
	}

	// ---- T33i: the note right above a rule moves with it ------------------------
	{
		FilterFile f = ParseFilter(
			"#pobtools-custom-begin v=1\n# note A\nShow\n\tClass \"Rings\"\n\n#=====\n# note B\nShow\n\tClass \"Amulets\"\n"
			"#pobtools-custom-end\n\nShow # outside\n\tClass \"Belts\"\n");
		FilterDocumentEditor doc;
		doc.Attach(&f);
		const int nb = doc.MoveBlockBefore(1, 0);
		const std::string out = SerializeFilter(f);
		const std::string want =
			"#pobtools-custom-begin v=1\n# note B\nShow\n\tClass \"Amulets\"\n\n#=====\n# note A\nShow\n\tClass \"Rings\"\n"
			"#pobtools-custom-end\n\nShow # outside\n\tClass \"Belts\"\n";
		rep.check(nb == 0 && out == want, "T33i a note right above a rule moves with it; blank, banner and sentinels stay", out);
	}

	// ---- T34: a rule's name in the left list (EdRuleLabel) ----------------------
	{
		FilterI18n i18n;
		i18n.Load(exeDir, "zh-rTW");
		rep.check(i18n.loaded(), "T34 i18n loaded (run from dist\\ so Data\\ exists)");
		auto has = [](const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; };
		// CardConditionZh of the nth condition line with this keyword in a block.
		auto condZh = [&](const FilterFile& file, int bi, const char* kw, int nth = 0) {
			for (int li : file.blocks[bi].lineIdx) {
				const FilterLine& l = file.lines[li];
				if (l.kind != FilterLineKind::Condition || l.keyword != kw) continue;
				if (nth-- == 0) return CardConditionZh(l, i18n);
			}
			return std::string();
		};
		auto labelOf = [&](const FilterFile& file, int bi, std::string* code) {
			return EdRuleLabel(file, file.blocks[bi], i18n, code);
		};

		// T34a: a known tier is shown as T2 and the code is not reported
		{
			const FilterFile f = ParseFilter(
				"Show # $type->currency $tier->t2\n\tItemLevel >= 70\n\tBaseType == \"Chaos Orb\" \"Divine Orb\"\n\tSetFontSize 40\n");
			if (f.blocks.size() == 1) {
				std::string code = "x";
				const std::string n = labelOf(f, 0, &code);
				rep.check(has(n, "T2") && !has(n, "t2") && code.empty(), "T34a known tier t2 -> T2, no tier code reported", n + " code=" + code);
				const std::string z = condZh(f, 0, "ItemLevel");
				rep.check(!z.empty() && !has(n, z), "T34a with a known tier no key condition is added", n);
				const std::string first = i18n.DisplayName("Chaos Orb");
				rep.check(has(n, first) && has(n, "等 2 種"), "T34a first item name plus the count of the others", n);
				rep.check(has(n, "【") && has(n, "】"), "T34a the name starts with a 【category】", n);
			} else {
				rep.check(false, "T34a fixture parses into one rule");
			}
		}

		// T34b: t1exalted -> T1 only, the raw code is reported
		{
			const FilterFile f = ParseFilter(
				"Show # $type->currency $tier->t1exalted\n\tBaseType == \"Chaos Orb\" \"Divine Orb\" \"Exalted Orb\" \"Orb of Alchemy\" \"Orb of Fusing\"\n\tSetFontSize 40\n");
			if (f.blocks.size() == 1) {
				std::string code;
				const std::string n = labelOf(f, 0, &code);
				rep.check(has(n, "T1") && !has(n, "exalted") && code == "t1exalted", "T34b t1exalted -> only T1, code goes to tierCodeOut", n + " code=" + code);
				rep.check(has(n, "等 5 種"), "T34b item count shows the number of base types", n);
			} else {
				rep.check(false, "T34b fixture parses into one rule");
			}
		}

		// T34c: unknown tier -> key conditions (skipping False flags / BaseType / Class) + items
		{
			const FilterFile f = ParseFilter(
				"Show # %D9 $type->crafting->qualityperfection $tier->armors1q1\n\tMirrored False\n\tCorrupted False\n\tQuality >= 28\n"
				"\tClass == \"Gloves\" \"Boots\"\n\tBaseType == \"Sorcerer Gloves\" \"Sorcerer Boots\" \"Gripped Gloves\"\n\tSetFontSize 40\n");
			if (f.blocks.size() == 1) {
				std::string code;
				const std::string n = labelOf(f, 0, &code);
				const std::string q = condZh(f, 0, "Quality"), m = condZh(f, 0, "Mirrored"), c = condZh(f, 0, "Corrupted");
				rep.check(!has(n, "armors1q1") && code == "armors1q1", "T34c unknown tier code is left out of the name and reported", n + " code=" + code);
				rep.check(!q.empty() && has(n, q), "T34c the Quality condition is the key condition", n + " | want " + q);
				rep.check(!m.empty() && !c.empty() && !has(n, m) && !has(n, c), "T34c False flags (Mirrored / Corrupted) are skipped", n);
				rep.check(has(n, i18n.DisplayName("Sorcerer Gloves")) && has(n, "等 3 種"), "T34c first base type plus 等 3 種", n);
			} else {
				rep.check(false, "T34c fixture parses into one rule");
			}
		}

		// T34d: unknown tier, Class only -> ItemLevel condition + the class in Chinese
		{
			const FilterFile f = ParseFilter(
				"Show # $type->gems $tier->someunknown\n\tClass == \"Rings\"\n\tItemLevel >= 86\n\tSetFontSize 40\n");
			if (f.blocks.size() == 1) {
				std::string code;
				const std::string n = labelOf(f, 0, &code);
				const std::string il = condZh(f, 0, "ItemLevel");
				const std::string ringsZh = i18n.ClassNameZh("Rings");
				rep.check(!il.empty() && has(n, il) && !has(n, "someunknown") && code == "someunknown",
				          "T34d ItemLevel is the key condition, unknown code left out", n + " code=" + code);
				rep.check(!has(n, "Rings") && (ringsZh == "Rings" || has(n, ringsZh)), "T34d the class shows in Chinese, not as the English word", n);
			} else {
				rep.check(false, "T34d fixture parses into one rule");
			}
		}

		// T34e: at most two key conditions, in file order
		{
			const FilterFile f = ParseFilter(
				"Show # $type->x $tier->zzz\n\tItemLevel >= 80\n\tQuality >= 10\n\tSockets >= 3\n\tLinkedSockets >= 4\n\tAreaLevel >= 68\n\tSetFontSize 40\n");
			if (f.blocks.size() == 1) {
				std::string code;
				const std::string n = labelOf(f, 0, &code);
				const std::string c1 = condZh(f, 0, "ItemLevel"), c2 = condZh(f, 0, "Quality"), c3 = condZh(f, 0, "Sockets"),
				                  c4 = condZh(f, 0, "LinkedSockets"), c5 = condZh(f, 0, "AreaLevel");
				rep.check(!c1.empty() && !c2.empty() && has(n, c1) && has(n, c2), "T34e the first two conditions are in the name", n);
				rep.check(!c3.empty() && !c4.empty() && !c5.empty() && !has(n, c3) && !has(n, c4) && !has(n, c5),
				          "T34e conditions 3 to 5 are left out", n);
			} else {
				rep.check(false, "T34e fixture parses into one rule");
			}
		}

		// T34f: not a NeverSink rule -> the rules above do not apply
		{
			const FilterFile f = ParseFilter("Show # my rule\n\tBaseType == \"Chaos Orb\"\n\tSetFontSize 40\n");
			if (f.blocks.size() == 1) {
				std::string code = "x";
				const std::string n = labelOf(f, 0, &code);
				rep.check(has(n, "my rule") && !has(n, "【"), "T34f a header without $type-> keeps its own text, no 【】", n);
			} else {
				rep.check(false, "T34f fixture parses into one rule");
			}
		}

		// T34g: every rule of the real NeverSink file
		{
			bool ok = false;
			FilterFile f = LoadFilter(exeDir + L"Filters\\default.filter", &ok);
			if (!ok) {
				rep.note("T34g skipped: Filters\\default.filter not readable");
			} else {
				int withCode = 0, bad = 0, empty = 0;
				std::string firstBad;
				for (size_t i = 0; i < f.blocks.size(); i++) {
					std::string code;
					const std::string n = labelOf(f, (int)i, &code);
					empty += n.empty();
					if (code.empty()) continue;
					withCode++;
					if (has(n, code)) { bad++; if (firstBad.empty()) firstBad = code + " in " + n; }
				}
				rep.note("T34g rules with a left-out tier code: " + std::to_string(withCode) + " of " + std::to_string(f.blocks.size()));
				rep.check(!f.blocks.empty() && bad == 0 && empty == 0, "T34g no name contains its own left-out tier code, none is empty",
				          "bad=" + std::to_string(bad) + " empty=" + std::to_string(empty) + " " + firstBad);
			}
		}
	}

	// ---- T35: base items the catalog lacks (OffCatalogMatches) ------------------
	{
		wchar_t tb[MAX_PATH];
		GetTempPathW(MAX_PATH, tb);
		const std::wstring root = std::wstring(tb) + L"pobtools_t35_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
		const std::wstring dData = root + L"Data\\", dPoe1 = dData + L"poe1\\", dLoc = dPoe1 + L"zh-rTW\\";
		CreateDirectoryW(root.c_str(), nullptr);
		CreateDirectoryW(dData.c_str(), nullptr);
		CreateDirectoryW(dPoe1.c_str(), nullptr);
		CreateDirectoryW(dLoc.c_str(), nullptr);
		auto put = [](const std::wstring& path, const std::string& body) {
			std::ofstream o(path, std::ios::binary);
			o << body;
		};
		std::string items = "{\"entries\":{\"Alpha Ring\":\"甲戒\",\"Bravo Belt\":\"乙腰帶\"";
		for (int i = 30; i >= 1; i--) {   // written in reverse so sorting has to happen
			char nm[32];
			snprintf(nm, sizeof(nm), "Zed Item %02d", i);
			items += std::string(",\"") + nm + "\":\"澤德" + std::to_string(i) + "\"";
		}
		items += "}}";
		put(dData + L"filter_items_zh.json", "{\"Alpha Ring\":\"甲戒\"}");
		put(dLoc + L"items.json", items);
		put(dLoc + L"uniques.json", "{\"entries\":{\"Charlie Crown\":\"丙冠\"}}");

		FilterI18n fi;
		fi.Load(root, "zh-rTW");
		auto hasPair = [](const std::vector<std::pair<std::string, std::string>>& v, const std::string& en, const std::string& zh) {
			for (const auto& p : v) if (p.first == en && p.second == zh) return true;
			return false;
		};
		auto hasEn = [](const std::vector<std::pair<std::string, std::string>>& v, const std::string& en) {
			for (const auto& p : v) if (p.first == en) return true;
			return false;
		};

		rep.check(fi.loaded(), "T35 fixture data loads from the scratch folder");
		const auto a = fi.OffCatalogMatches("bravo", 20);
		rep.check(a.size() == 1 && hasPair(a, "Bravo Belt", "乙腰帶"), "T35a English query (any case) finds an item the catalog lacks", "n=" + std::to_string(a.size()));
		const auto b = fi.OffCatalogMatches("乙腰", 20);
		rep.check(hasPair(b, "Bravo Belt", "乙腰帶"), "T35b Chinese substring finds it too");
		const auto c = fi.OffCatalogMatches("alpha", 20);
		rep.check(!hasEn(c, "Alpha Ring") && c.empty(), "T35c an item that is in the catalog is not offered");
		const auto d = fi.OffCatalogMatches("zed", 5);
		rep.check(d.size() == 5 && d[0].first == "Zed Item 01" && d[4].first == "Zed Item 05",
		          "T35d limit 5 gives exactly 5, sorted by English", d.empty() ? "empty" : d[0].first + ".." + d.back().first);
		const auto all = fi.OffCatalogMatches("zed", 100);
		rep.check(all.size() == 30 && all[0].first == "Zed Item 01" && all[29].first == "Zed Item 30", "T35d a larger limit returns all 30 in order",
		          "n=" + std::to_string(all.size()));
		rep.check(fi.OffCatalogMatches("", 20).empty(), "T35e an empty query matches nothing");
		rep.check(!hasEn(fi.OffCatalogMatches("charlie", 20), "Charlie Crown") && fi.OffCatalogMatches("丙冠", 20).empty(),
		          "T35f uniques.json entries are never offered");

		DeleteFileW((dData + L"filter_items_zh.json").c_str());
		DeleteFileW((dLoc + L"items.json").c_str());
		DeleteFileW((dLoc + L"uniques.json").c_str());
		RemoveDirectoryW(dLoc.c_str());
		RemoveDirectoryW(dPoe1.c_str());
		RemoveDirectoryW(dData.c_str());
		RemoveDirectoryW(root.c_str());
		rep.check(GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES, "T35 the scratch folder is gone");

		// T35g: the real data
		FilterI18n real;
		real.Load(exeDir, "zh-rTW");
		if (!real.loaded()) {
			rep.note("T35g skipped: Data not readable next to the exe");
		} else {
			const auto r = real.OffCatalogMatches("a", 100000);
			rep.note("T35g real data: " + std::to_string(r.size()) + " items off the catalog match \"a\"");
			rep.check(!r.empty(), "T35g the real items.json has base items the catalog lacks", "n=" + std::to_string(r.size()));
		}
	}

	// ---- T36: what Enter adds from the candidate list -------------------------
	{
		auto item = [](const char* en, const char* zh, bool off) {
			LibItem it;
			it.en = en;
			it.zh = zh;
			it.offCatalog = off;
			return it;
		};
		const std::vector<LibItem> cat = { item("Chaos Orb", "混沌石", false), item("Chaos Shard", "混沌碎片", false) };
		rep.check(CardEnterPick(cat, "Chaos Shard") == "Chaos Shard", "T36 catalog: an exact English match wins over the first row");
		rep.check(CardEnterPick(cat, "混沌碎片") == "Chaos Shard", "T36 catalog: an exact Chinese match wins");
		rep.check(CardEnterPick(cat, "混沌") == "Chaos Orb", "T36 catalog: no exact match adds the first row (as before)");
		const std::vector<LibItem> off = { item("Amber Ring", "琥珀戒", true), item("Bone Ring", "骨戒", true) };
		rep.check(CardEnterPick(off, "戒").empty(), "T36 off-catalog: a partial match adds nothing");
		rep.check(CardEnterPick(off, "骨戒") == "Bone Ring", "T36 off-catalog: an exact Chinese match is added");
		rep.check(CardEnterPick(off, "bone ring").empty() && CardEnterPick(off, "Bone Ring") == "Bone Ring",
		          "T36 off-catalog: English must match exactly");
		rep.check(CardEnterPick({}, "x").empty(), "T36 no candidates: nothing");
	}

	// ---- T37: the "preview may differ" note (PreviewEstimateNote) ---------------
	{
		auto has = [](const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; };
		FilterI18n emptyI18n;
		PreviewItem divine;
		divine.baseType = "Divine Orb";
		divine.classId = "Stackable Currency";

		// T37a: nothing unmodelled -> no note
		{
			const FilterFile f = ParseFilter("Show\n\tBaseType == \"Divine Orb\"\n\tSetFontSize 40\n");
			const PreviewResult r = EvaluatePreview(f, divine, emptyI18n);
			rep.check(r.matched && r.unknownConds.empty() && PreviewEstimateNote(r).empty(), "T37a exact result has an empty note");
		}

		// T37b / T37c: an earlier rule uses a condition the preview cannot model
		{
			const FilterFile f = ParseFilter(
				"Show\n\tHasExplicitMod \"Foo\"\n\tBaseType == \"Divine Orb\"\n\tSetFontSize 20\n\n"
				"Show\n\tBaseType == \"Divine Orb\"\n\tSetFontSize 40\n");
			const PreviewResult r = EvaluatePreview(f, divine, emptyI18n);
			const std::string note = PreviewEstimateNote(r);
			rep.check(r.matched && r.blockIdx == 1 && r.fontSize == 40 && !note.empty(),
			          "T37b the second rule decides, and the note is not empty", "blockIdx=" + std::to_string(r.blockIdx));
			const std::string zh = FilterSchemaKeywordZh("HasExplicitMod");
			rep.check(!zh.empty() && has(note, zh) && (zh == "HasExplicitMod" || !has(note, "HasExplicitMod")),
			          "T37c the note names the skipped condition in Chinese, not in English", note);
		}

		// T37d: five different unmodelled conditions, only four are named
		{
			const char* kws[5] = { "HasExplicitMod", "HasEnchantment", "ArchnemesisMod", "BaseDefencePercentile", "HasSearingExarchImplicit" };
			const char* vals[5] = { " \"Foo\"", " \"Bar\"", " \"Baz\"", " >= 90", " >= 1" };
			std::string src;
			for (int i = 0; i < 5; i++)
				src += std::string("Show\n\t") + kws[i] + vals[i] + "\n\tBaseType == \"Divine Orb\"\n\tSetFontSize 20\n\n";
			src += "Show\n\tBaseType == \"Divine Orb\"\n\tSetFontSize 40\n";
			const FilterFile f = ParseFilter(src);
			const PreviewResult r = EvaluatePreview(f, divine, emptyI18n);
			const std::string note = PreviewEstimateNote(r);
			bool order = r.unknownConds.size() >= 5;
			for (int i = 0; order && i < 5; i++) order = r.unknownConds[i] == kws[i];
			rep.check(r.matched && r.blockIdx == 5 && order, "T37d fixture: five unmodelled conditions collected in rule order",
			          "n=" + std::to_string(r.unknownConds.size()) + " blockIdx=" + std::to_string(r.blockIdx));
			bool named4 = true;
			for (int i = 0; i < 4; i++) {
				const std::string zh = FilterSchemaKeywordZh(kws[i]);
				named4 = named4 && !zh.empty() && has(note, zh);
			}
			const std::string fifth = FilterSchemaKeywordZh(kws[4]);
			rep.check(named4, "T37d the note names the first four conditions", note);
			rep.check(!fifth.empty() && !has(note, fifth) && !has(note, kws[4]), "T37d the fifth condition is left out (at most four)", note);
		}

		// T37e: the real NeverSink file, a rare ring
		{
			bool ok = false;
			const FilterFile f = LoadFilter(exeDir + L"Filters\\default.filter", &ok);
			if (!ok) {
				rep.note("T37e skipped: Filters\\default.filter not readable");
			} else {
				FilterI18n i18n;
				i18n.Load(exeDir, "zh-rTW");
				// Only a rule whose modelled conditions all hold makes an estimate,
				// so try the first item of every rule that uses an unmodelled
				// condition, as a rare ilvl 86 drop (the synthesizer skips such rules).
				int tried = 0, estimates = 0;
				std::string example;
				for (const FilterBlock& b : f.blocks) {
					std::string base;
					bool unmodelled = false;
					for (int li : b.lineIdx) {
						const FilterLine& ln = f.lines[li];
						if (ln.kind != FilterLineKind::Condition) continue;
						if (ln.keyword == "BaseType" && !ln.values.empty() && base.empty()) base = ln.values[0].text;
						if (ln.keyword.rfind("Has", 0) == 0 && ln.keyword != "HasInfluence") unmodelled = true;
					}
					if (!unmodelled || base.empty()) continue;
					PreviewItem it;
					it.baseType = base;
					it.rarity = 2;
					it.itemLevel = 86;
					const PreviewResult r = EvaluatePreview(f, it, i18n);
					tried++;
					if (!r.unknownConds.empty()) {
						estimates++;
						if (example.empty()) example = base + ": " + PreviewEstimateNote(r);
					}
				}
				rep.note("T37e " + std::to_string(estimates) + " of " + std::to_string(tried) + " items from unmodelled rules are estimates; e.g. " + example);
				rep.check(estimates > 0 && !example.empty(), "T37e on the real filter some drops are estimates, with a note", "tried=" + std::to_string(tried));
			}
		}
	}

	// ---- T37f: a rule ruled out by a known condition does not make it an estimate
	{
		FilterFile f = ParseFilter(
			"Show\n\tHasExplicitMod \"Foo\"\n\tBaseType == \"Exalted Orb\"\n\tSetFontSize 20\n"
			"Show\n\tBaseType == \"Divine Orb\"\n\tSetFontSize 40\n");
		FilterI18n none;
		PreviewItem it;
		it.baseType = "Divine Orb";
		it.classId = "StackableCurrency";
		const PreviewResult r = EvaluatePreview(f, it, none);
		rep.check(r.blockIdx == 1 && r.unknownConds.empty() && PreviewEstimateNote(r).empty(),
		          "T37f an unmodelled condition in a rule whose item name cannot match is no estimate",
		          "n=" + std::to_string(r.unknownConds.size()));
	}

	// ---- T38/T39/T40: keep your edits across a NeverSink update (edit journal) --
	{
		auto has = [](const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; };
		struct Fx {
			std::string ver = "8.19.2a";
			std::string c1 = "255 0 0", s1 = "45", c2 = "0 255 0", c3 = "0 0 255";
			bool gems = true;
			std::string zoneCol;    // "" = no custom zone; else the custom rule's SetTextColor
			std::string plainCol;   // "" = no rule without $type; else its SetTextColor
		};
		auto build = [](const Fx& x) {
			std::string s = "# VERSION:  " + x.ver + "\n# TYPE:     2-SEMI-STRICT\n\n";
			if (!x.zoneCol.empty())
				s += "#pobtools-custom-begin v=1\nShow # my custom\n\tBaseType == \"Exalted Orb\"\n\tSetTextColor " + x.zoneCol +
				     "\n#pobtools-custom-end\n\n";
			s += "Show # %D9 $type->currency $tier->t1\n\tBaseType == \"Divine Orb\"\n\tSetTextColor " + x.c1 + "\n\tSetFontSize " + x.s1 + "\n\n";
			s += "Show # %D8 $type->currency $tier->t2\n\tBaseType == \"Chaos Orb\"\n\tSetTextColor " + x.c2 + "\n\tSetFontSize 40\n\n";
			if (x.gems) s += "Show # %D7 $type->gems $tier->t1\n\tBaseType == \"Vaal Grace\"\n\tSetTextColor " + x.c3 + "\n\tSetFontSize 38\n";
			if (!x.plainCol.empty()) s += "\nShow # my rule\n\tBaseType == \"Scroll of Wisdom\"\n\tSetTextColor " + x.plainCol + "\n";
			return s;
		};
		const std::string K1 = "$type->currency $tier->t1", K3 = "$type->gems $tier->t1";
		auto blkOf = [](const FilterFile& f, const std::string& key) {
			for (size_t i = 0; i < f.blocks.size(); i++)
				if (FilterRuleKey(f, f.blocks[i]) == key) return (int)i;
			return -1;
		};
		auto ownOf = [&](const FilterFile& f, const std::string& key) {
			const int bi = blkOf(f, key);
			return bi < 0 ? std::vector<std::string>() : FilterOwnLines(f, f.blocks[bi]);
		};
		auto joined = [](const std::vector<std::string>& v) {
			std::string s;
			for (const std::string& l : v) s += l + "\n";
			return s;
		};
		struct Sess { FilterFile f; FilterDocumentEditor doc; };
		auto openSess = [](Sess& s, const std::string& text) { s.f = ParseFilter(text); s.doc.Attach(&s.f); };
		auto recordJ = [&](const Fx& base, const Fx& edited) {
			EditJournal j;
			JournalRecord(j, ParseFilter(build(base)), ParseFilter(build(edited)));
			return j;
		};

		// ---- T38: recording ----
		Fx edited;
		edited.c1 = "9 9 9";
		EditJournal j38;
		{
			const FilterFile base = ParseFilter(build(Fx()));
			const FilterFile cur = ParseFilter(build(edited));
			JournalRecord(j38, base, cur);
			rep.check(j38.rules.size() == 1 && j38.rules.count(K1) == 1, "T38a only the edited NeverSink rule is recorded, under its identity",
			          "n=" + std::to_string(j38.rules.size()));
			if (j38.rules.count(K1)) {
				rep.check(j38.rules[K1].before == ownOf(base, K1) && j38.rules[K1].after == ownOf(cur, K1) &&
				              j38.rules[K1].before != j38.rules[K1].after,
				          "T38a before = NeverSink's lines, after = the edited lines");
			}
			rep.check(j38.fromVersion == "8.19.2a" && j38.filterType == "2-SEMI-STRICT", "T38a version and type come from the banner",
			          j38.fromVersion + " / " + j38.filterType);
		}
		{
			// second save: the baseline is now the first save's file; one more edit on the same rule
			Fx e2 = edited;
			e2.s1 = "50";
			const FilterFile base2 = ParseFilter(build(edited));
			const FilterFile cur2 = ParseFilter(build(e2));
			const std::vector<std::string> original = ownOf(ParseFilter(build(Fx())), K1);
			JournalRecord(j38, base2, cur2);
			rep.check(j38.rules.size() == 1 && j38.rules.count(K1) == 1 && j38.rules[K1].before == original,
			          "T38b a second edit keeps NeverSink's earliest 'before'");
			const std::string aft = j38.rules.count(K1) ? joined(j38.rules[K1].after) : "";
			rep.check(has(aft, "SetTextColor 9 9 9") && has(aft, "SetFontSize 50"), "T38b 'after' carries both the colour and the size edit", aft);
			// third save: back to the original content
			const FilterFile base3 = ParseFilter(build(e2));
			const FilterFile cur3 = ParseFilter(build(Fx()));
			JournalRecord(j38, base3, cur3);
			rep.check(j38.rules.empty(), "T38c editing the rule back to NeverSink's version drops the entry", "n=" + std::to_string(j38.rules.size()));
		}

		// T38d: rules without $type are not recorded; the custom zone is stored
		{
			Fx b, c;
			b.zoneCol = "1 1 1"; b.plainCol = "5 5 5";
			c.zoneCol = "2 2 2"; c.plainCol = "6 6 6";
			const EditJournal j = recordJ(b, c);
			rep.check(j.rules.empty() && j.rules.count("") == 0, "T38d a rule without $type is not recorded", "n=" + std::to_string(j.rules.size()));
			rep.check(has(j.customZone, "my custom") && has(j.customZone, "SetTextColor 2 2 2") && !has(j.customZone, "SetTextColor 1 1 1"),
			          "T38d the custom zone is stored from the current file", j.customZone);
		}

		// T38e: save + load round trip, no temp file left behind
		{
			wchar_t tb[MAX_PATH];
			GetTempPathW(MAX_PATH, tb);
			const std::wstring dir = std::wstring(tb) + L"pobtools_t38_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
			CreateDirectoryW(dir.c_str(), nullptr);
			const std::wstring path = dir + L"journal.json";
			EditJournal a;
			a.filterType = "2-SEMI-STRICT";
			a.fromVersion = "8.19.2a";
			a.rules["$type->currency $tier->t1"].before = { "Show # a \"quoted\" \\ back", "\tBaseType == \"Divine Orb\"", "\t#! 中文\t" };
			a.rules["$type->currency $tier->t1"].after = { "Show # a", "\tSetTextColor 9 9 9" };
			a.rules["$type->gems $tier->t1"].before = { "Show # g" };
			a.rules["$type->gems $tier->t1"].after = {};
			a.customZone = "Show # c\r\n\tx \"q\" \\\n中文";
			std::string err;
			const bool saved = JournalSave(path, a, &err);
			EditJournal b;
			const bool loaded = JournalLoad(path, &b);
			bool same = saved && loaded && a.filterType == b.filterType && a.fromVersion == b.fromVersion && a.customZone == b.customZone &&
			            a.rules.size() == b.rules.size();
			for (const auto& kv : a.rules) {
				const auto it = b.rules.find(kv.first);
				same = same && it != b.rules.end() && it->second.before == kv.second.before && it->second.after == kv.second.after;
			}
			rep.check(same, "T38e save then load gives the same journal (quotes, backslashes, newlines, Chinese)", err);
			int files = 0;
			std::wstring only;
			WIN32_FIND_DATAW fd;
			HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
			if (h != INVALID_HANDLE_VALUE) {
				do {
					if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
					files++;
					only = fd.cFileName;
				} while (FindNextFileW(h, &fd));
				FindClose(h);
			}
			rep.check(files == 1 && only == L"journal.json", "T38e no temp file is left next to the journal", "files=" + std::to_string(files));
			DeleteFileW(path.c_str());
			RemoveDirectoryW(dir.c_str());
			rep.check(GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES, "T38e the scratch folder is gone");
		}

		// ---- T39: applying to a newer file ----
		const EditJournal jColor = recordJ(Fx(), edited);   // currency/t1 colour -> 9 9 9
		{
			Fx nv;   // NeverSink changed nothing in this rule
			nv.ver = "8.20.2";
			Sess s;
			openSess(s, build(nv));
			const JournalReport r = JournalApply(jColor, s.doc, {});
			rep.check(has(joined(ownOf(s.f, K1)), "SetTextColor 9 9 9") && r.applied == 1 && r.conflicts.empty(),
			          "T39a your colour edit is put on the new file", "applied=" + std::to_string(r.applied) + " conflicts=" + std::to_string(r.conflicts.size()));
			rep.check(ownOf(s.f, "$type->currency $tier->t2") == ownOf(ParseFilter(build(nv)), "$type->currency $tier->t2"),
			          "T39a other rules are left alone");
		}
		{
			Fx nv;
			nv.ver = "8.20.2";
			nv.s1 = "60";   // NeverSink changed the size, not the colour
			Sess s;
			openSess(s, build(nv));
			const JournalReport r = JournalApply(jColor, s.doc, {});
			const std::string own = joined(ownOf(s.f, K1));
			rep.check(has(own, "SetTextColor 9 9 9") && has(own, "SetFontSize 60") && r.conflicts.empty(),
			          "T39b your colour and NeverSink's new size both survive, no conflict", own);
		}
		{
			Fx nv;
			nv.ver = "8.20.2";
			nv.c1 = "1 1 1";   // NeverSink changed the same colour too
			Sess s;
			openSess(s, build(nv));
			const JournalReport r = JournalApply(jColor, s.doc, {});
			const bool one = r.conflicts.size() == 1;
			rep.check(one && r.conflicts[0].key == K1 && r.conflicts[0].setting == "SetTextColor" && has(r.conflicts[0].mine, "9 9 9") &&
			              has(r.conflicts[0].theirs, "1 1 1"),
			          "T39c both changed the colour: one conflict with both versions", "n=" + std::to_string(r.conflicts.size()));
			rep.check(has(joined(ownOf(s.f, K1)), "SetTextColor 9 9 9"), "T39c without a choice your version is kept");
			Sess s2;
			openSess(s2, build(nv));
			JournalChoices ch;
			ch[K1 + "|SetTextColor"] = true;
			JournalApply(jColor, s2.doc, ch);
			const std::string own2 = joined(ownOf(s2.f, K1));
			rep.check(has(own2, "SetTextColor 1 1 1") && !has(own2, "SetTextColor 9 9 9"), "T39c choosing NeverSink's version takes the new colour", own2);
		}
		{
			// T39d: the new file no longer has gems/t1
			Fx g;
			g.c3 = "7 7 7";
			const EditJournal jg = recordJ(Fx(), g);
			Fx nv;
			nv.ver = "8.20.2";
			nv.gems = false;
			Sess s;
			openSess(s, build(nv));
			const std::string before = SerializeFilter(s.f);
			const JournalReport r = JournalApply(jg, s.doc, {});
			bool listed = false;
			for (const std::string& m : r.missing) listed |= m == K3;
			rep.check(jg.rules.count(K3) == 1 && listed && SerializeFilter(s.f) == before, "T39d a rule the new file lacks is reported as missing, nothing else changes");
		}
		{
			// T39e: custom zone comes back, once
			Fx c;
			c.zoneCol = "7 7 7";
			const EditJournal jz = recordJ(Fx(), c);
			Fx nv;
			nv.ver = "8.20.2";
			Sess s;
			openSess(s, build(nv));
			rep.check(!FindCustomZone(s.f).present(), "T39e fixture: the new file has no custom zone");
			const JournalReport r = JournalApply(jz, s.doc, {});
			const CustomZone z = FindCustomZone(s.f);
			bool inZone = false;
			for (const FilterBlock& b : s.f.blocks)
				if (z.present() && b.headerLineIdx > z.beginLine && b.headerLineIdx < z.endLine && has(FilterBlockText(s.f, b, false), "SetTextColor 7 7 7")) inZone = true;
			rep.check(z.present() && inZone && r.customImported == 1, "T39e the custom zone and its rule are put back", "imported=" + std::to_string(r.customImported));
			const std::string once = SerializeFilter(s.f);
			const JournalReport r2 = JournalApply(jz, s.doc, {});
			rep.check(r2.customImported == 0 && SerializeFilter(s.f) == once, "T39e applying again imports nothing and changes nothing");
		}
		{
			// T39f: twice = once
			Fx nv;
			nv.ver = "8.20.2";
			Sess s;
			openSess(s, build(nv));
			const JournalReport r1 = JournalApply(jColor, s.doc, {});
			const std::string once = SerializeFilter(s.f);
			const JournalReport r2 = JournalApply(jColor, s.doc, {});
			rep.check(r1.applied == 1 && r2.already == 1 && r2.applied == 0 && SerializeFilter(s.f) == once,
			          "T39f applying twice: the second time everything is 'already' and the file is the same",
			          "applied2=" + std::to_string(r2.applied) + " already2=" + std::to_string(r2.already));
		}
		{
			// T39g: dry run reports like the real thing but changes nothing
			Fx nvA, nvB;
			nvA.ver = nvB.ver = "8.20.2";
			nvA.s1 = "60";     // merges cleanly
			nvB.c1 = "1 1 1";  // conflicts
			const std::string texts[2] = { build(nvA), build(nvB) };
			for (int i = 0; i < 2; i++) {
				Sess dry, real;
				openSess(dry, texts[i]);
				openSess(real, texts[i]);
				const std::string before = SerializeFilter(dry.f);
				const JournalReport rd = JournalApply(jColor, dry.doc, {}, true);
				const JournalReport rr = JournalApply(jColor, real.doc, {}, false);
				rep.check(SerializeFilter(dry.f) == before && SerializeFilter(real.f) != before, i == 0 ? "T39g dry run (clean merge) changes nothing, a real apply does"
				                                                                                       : "T39g dry run (conflict) changes nothing, a real apply does");
				rep.check(rd.applied == rr.applied && rd.already == rr.already && rd.conflicts.size() == rr.conflicts.size() &&
				              rd.missing.size() == rr.missing.size() && rd.customImported == rr.customImported,
				          i == 0 ? "T39g dry run report equals the real report (clean merge)" : "T39g dry run report equals the real report (conflict)",
				          "applied " + std::to_string(rd.applied) + "/" + std::to_string(rr.applied) + " conflicts " + std::to_string(rd.conflicts.size()) + "/" + std::to_string(rr.conflicts.size()));
			}
		}

		// T39h: the real NeverSink file, three rules edited, NeverSink unchanged
		{
			bool ok = false;
			const FilterFile base = LoadFilter(exeDir + L"Filters\\default.filter", &ok);
			if (!ok) {
				rep.note("T39h skipped: Filters\\default.filter not readable");
			} else {
				FilterFile cur = base;
				std::map<std::string, int> keyCount;
				for (const FilterBlock& b : base.blocks) {
					const std::string k = FilterRuleKey(base, b);
					if (!k.empty()) keyCount[k]++;
				}
				std::vector<std::string> picked;
				for (size_t i = 0; i < cur.blocks.size() && picked.size() < 3; i++) {
					const std::string k = FilterRuleKey(cur, cur.blocks[i]);
					if (k.empty() || keyCount[k] != 1) continue;   // unique identity only
					int tc = -1;
					for (int li : cur.blocks[i].lineIdx)
						if (cur.lines[li].kind == FilterLineKind::Action && cur.lines[li].keyword == "SetTextColor" && cur.lines[li].values.size() >= 3) { tc = li; break; }
					if (tc < 0) continue;
					cur.lines[tc].values[0].text = "1";
					cur.lines[tc].values[1].text = "2";
					cur.lines[tc].values[2].text = "3";
					cur.lines[tc].dirty = true;
					cur.dirty = true;
					picked.push_back(k);
				}
				rep.check(picked.size() == 3, "T39h fixture: three rules with a unique identity and a text colour found", "n=" + std::to_string(picked.size()));
				EditJournal j;
				JournalRecord(j, base, cur);
				rep.check(j.rules.size() == 3, "T39h three edited rules are recorded", "n=" + std::to_string(j.rules.size()));
				Sess s;
				bool ok2 = false;
				s.f = LoadFilter(exeDir + L"Filters\\default.filter", &ok2);
				s.doc.Attach(&s.f);
				const JournalReport r = JournalApply(j, s.doc, {});
				bool allSame = !picked.empty();
				for (const std::string& k : picked) allSame = allSame && ownOf(s.f, k) == ownOf(cur, k);
				rep.check(ok2 && r.applied == 3 && r.conflicts.empty() && r.missing.empty() && allSame,
				          "T39h applying onto an unchanged NeverSink file reproduces the three edited rules",
				          "applied=" + std::to_string(r.applied) + " conflicts=" + std::to_string(r.conflicts.size()) + " missing=" + std::to_string(r.missing.size()));
			}
		}

		// ---- T40: game subscription header ----
		{
			const std::string body = "#=====\n# NeverSink's filter\nShow\n\tSetFontSize 40\n";
			const std::string online =
				"#Online Item Filter\n#name:NeverSink-5uberstr-softcore\n#version:3.29.3b\n#hash:abc\n#filterVersion:8.20.2.2026.309.7\n#lastUpdate:2026\n" + body;
			const OnlineFilterHeader h = ParseOnlineHeader(online);
			rep.check(h.online && h.name == "NeverSink-5uberstr-softcore" && h.filterVersion == "8.20.2.2026.309.7",
			          "T40a a subscription header is recognised with its name and filter version", h.name + " / " + h.filterVersion);
			const OnlineFilterHeader p = ParseOnlineHeader("#name:NeverSink-5uberstr-softcore\n" + body);
			rep.check(!p.online, "T40a without the first line it is not a subscription");
			const std::string out = StripOnlineHeader(online);
			rep.check(out.compare(0, 6, "#=====") == 0 && !has(out, "#Online Item Filter") && !has(out, "#hash:") && !has(out, "#name:") &&
			              !has(out, "#version:") && !has(out, "#filterVersion:") && !has(out, "#lastUpdate:"),
			          "T40b the subscription's own header lines are removed", out);
			rep.check(out == body, "T40b everything after the header is byte-for-byte the same");
			std::string crlf;
			for (char ch : online) { if (ch == '\n') crlf += '\r'; crlf += ch; }
			std::string crlfBody;
			for (char ch : body) { if (ch == '\n') crlfBody += '\r'; crlfBody += ch; }
			rep.check(StripOnlineHeader(crlf) == crlfBody, "T40b the same holds with CRLF line endings");
			rep.check(StripOnlineHeader(body) == body, "T40b text without a subscription header is returned unchanged");
		}
	}

	// ---- T38f/T38g: across versions, and a custom-zone copy with the same identity
	{
		auto mk = [](const char* ver, const char* colour, const char* bases) {
			std::string s = std::string("# VERSION:  ") + ver + "\n# TYPE:     2-SEMI-STRICT\n\n"
			                "Show # $type->currency $tier->t1\n\tBaseType == " + bases + "\n\tSetTextColor " + colour + "\n";
			return s;
		};
		// v1 -> your colour; v2 (NeverSink changed the bases) applied and saved
		const FilterFile v1 = ParseFilter(mk("1", "255 0 0", "\"Divine Orb\""));
		FilterFile v1mine = ParseFilter(mk("1", "9 9 9", "\"Divine Orb\""));
		EditJournal j;
		JournalRecord(j, v1, v1mine);
		FilterFile v2 = ParseFilter(mk("2", "255 0 0", "\"Divine Orb\" \"Mirror of Kalandra\""));
		const FilterFile v2pristine = v2;
		{
			FilterDocumentEditor d;
			d.Attach(&v2);
			JournalApply(j, d, {}, false);
		}
		JournalRecord(j, v2pristine, v2);   // the save after applying on v2
		const auto it = j.rules.find("$type->currency $tier->t1");
		const bool rebased = it != j.rules.end() && it->second.before == FilterOwnLines(v2pristine, v2pristine.blocks[0]);
		rep.check(rebased && j.fromVersion == "2", "T38f after saving on a new version, 'before' is that version's NeverSink rule");
		// v3: NeverSink changes the bases again -> no conflict, both changes kept
		FilterFile v3 = ParseFilter(mk("3", "255 0 0", "\"Exalted Orb\""));
		FilterDocumentEditor d3;
		d3.Attach(&v3);
		const JournalReport r3 = JournalApply(j, d3, {}, false);
		const std::string t3 = FilterBlockText(v3, v3.blocks[0], false);
		rep.check(r3.conflicts.empty() && t3.find("Exalted Orb") != std::string::npos && t3.find("9 9 9") != std::string::npos,
		          "T38f the next update keeps NeverSink's new bases and your colour, no false conflict",
		          "conflicts=" + std::to_string(r3.conflicts.size()));
		// a version opened but not applied: the pending edit is kept, not lost
		EditJournal j2;
		JournalRecord(j2, v1, v1mine);
		const FilterFile v2b = ParseFilter(mk("2", "255 0 0", "\"Divine Orb\""));
		JournalRecord(j2, v2b, v2b);   // saved v2 without applying
		rep.check(j2.rules.size() == 1, "T38f saving a new version without applying keeps the pending edit");
		// a pending rule edited by hand on the new version: the old edit is not lost
		{
			auto mk3 = [](const char* ver, const char* colour, const char* size) {
				return ParseFilter(std::string("# VERSION:  ") + ver + "\n# TYPE:     2-SEMI-STRICT\n\n"
				                   "Show # $type->currency $tier->t1\n\tBaseType == \"Divine Orb\"\n\tSetTextColor " + colour +
				                   "\n\tSetFontSize " + size + "\n");
			};
			EditJournal jm;
			JournalRecord(jm, mk3("1", "255 0 0", "45"), mk3("1", "9 9 9", "45"));
			JournalRecord(jm, mk3("2", "255 0 0", "45"), mk3("2", "255 0 0", "30"));   // size edited, colour not applied
			const auto it2 = jm.rules.find("$type->currency $tier->t1");
			bool keepsColour = false;
			if (it2 != jm.rules.end())
				for (const std::string& l : it2->second.after) keepsColour |= l.find("9 9 9") != std::string::npos;
			rep.check(keepsColour, "T38f editing a still-pending rule by hand keeps the old edit in the journal");
			// NeverSink's new version ships exactly your edit: nothing is left to keep
			EditJournal ja;
			JournalRecord(ja, mk3("1", "255 0 0", "45"), mk3("1", "255 0 0", "46"));
			JournalRecord(ja, mk3("2", "255 0 0", "46"), mk3("2", "255 0 0", "46"));
			rep.check(ja.rules.empty(), "T38f an edit NeverSink adopted itself leaves the journal",
			          "n=" + std::to_string(ja.rules.size()));
			// applied choosing NeverSink's colour, then the size tweaked, then saved:
			// the rule is carried onto v2 as it now is (not held back as pending)
			EditJournal jc;
			JournalRecord(jc, mk3("1", "255 0 0", "45"), mk3("1", "9 9 9", "45"));
			FilterFile v2c = mk3("2", "1 2 3", "45");      // NeverSink changed the colour too
			const FilterFile v2cPristine = v2c;
			JournalReport rc;
			{
				FilterDocumentEditor d;
				d.Attach(&v2c);
				rc = JournalApply(jc, d, { { "$type->currency $tier->t1|SetTextColor", true } }, false);
			}
			FilterFile tweaked = mk3("2", "1 2 3", "30");  // kept NeverSink's colour, then changed the size
			JournalRecord(jc, v2cPristine, tweaked, &rc.carried);
			const auto ic = jc.rules.find("$type->currency $tier->t1");
			rep.check(ic != jc.rules.end() && ic->second.before == FilterOwnLines(v2cPristine, v2cPristine.blocks[0]) &&
			              ic->second.after == FilterOwnLines(tweaked, tweaked.blocks[0]),
			          "T38f a rule applied with your choices (and tweaked after) is recorded against the new version");
		}
		// two rules pending on v1; on v2 only one is applied and saved, then saved again
		{
			auto mk2 = [](const char* ver, const char* c1, const char* c2) {
				return ParseFilter(std::string("# VERSION:  ") + ver + "\n# TYPE:     2-SEMI-STRICT\n\n"
				                   "Show # $type->currency $tier->t1\n\tBaseType == \"Divine Orb\"\n\tSetTextColor " + c1 + "\n\n"
				                   "Show # $type->currency $tier->t2\n\tBaseType == \"Chaos Orb\"\n\tSetTextColor " + c2 + "\n");
			};
			EditJournal jj;
			JournalRecord(jj, mk2("1", "255 0 0", "255 0 0"), mk2("1", "9 9 9", "8 8 8"));
			const FilterFile p2 = mk2("2", "255 0 0", "255 0 0");
			const FilterFile s1 = mk2("2", "9 9 9", "255 0 0");   // only t1 carried over
			JournalRecord(jj, p2, s1);                           // first save on v2
			const FilterFile s2 = mk2("2", "7 7 7", "255 0 0");   // t1 edited again on v2
			JournalRecord(jj, s1, s2);                           // second save, baseline = the saved file
			const auto a = jj.rules.find("$type->currency $tier->t1");
			const auto b = jj.rules.find("$type->currency $tier->t2");
			rep.check(jj.rules.size() == 2 && a != jj.rules.end() && b != jj.rules.end() &&
			              a->second.before == FilterOwnLines(p2, p2.blocks[0]),
			          "T38f a second save on the new version keeps both entries (the applied one still against v2)",
			          "n=" + std::to_string(jj.rules.size()));
		}

		// T38g: a copy of the rule in the custom zone (same $type/$tier) is not the rule
		const std::string zoneCopy =
			"#pobtools-custom-begin v=1\nShow # $type->currency $tier->t1\n\tBaseType == \"Chaos Orb\"\n\tSetTextColor 7 7 7\n"
			"#pobtools-custom-end\n\n";
		const FilterFile b0 = ParseFilter(zoneCopy + mk("1", "255 0 0", "\"Divine Orb\""));
		FilterFile c0 = ParseFilter(zoneCopy + mk("1", "9 9 9", "\"Divine Orb\""));
		EditJournal jz;
		JournalRecord(jz, b0, c0);
		const auto jt = jz.rules.find("$type->currency $tier->t1");
		const bool beforeIsRule = jt != jz.rules.end() && !jt->second.before.empty() &&
		                          std::string(jt->second.before.back()).find("255 0 0") != std::string::npos;
		rep.check(beforeIsRule, "T38g the journal records the NeverSink rule, not its custom-zone copy");
		FilterFile n0 = ParseFilter(zoneCopy + mk("2", "255 0 0", "\"Divine Orb\""));
		FilterDocumentEditor dz;
		dz.Attach(&n0);
		JournalApply(jz, dz, {}, false);
		const std::string all = SerializeFilter(n0);
		rep.check(all.find("SetTextColor 7 7 7") != std::string::npos && all.find("SetTextColor 9 9 9") != std::string::npos,
		          "T38g applying leaves the custom copy alone and edits the NeverSink rule");
	}

	// ---- T38h: Show/Hide and NeverSink's header comment are separate settings;
	//      repeated keywords are paired by content, not by position ------------
	{
		auto jr = [](const std::string& before, const std::string& after) {
			EditJournal j;
			JournalRecord(j, ParseFilter("# VERSION: 1\n" + before), ParseFilter("# VERSION: 1\n" + after));
			return j;
		};
		auto apply = [](const EditJournal& j, const std::string& next, JournalReport* rep) {
			FilterFile f = ParseFilter("# VERSION: 2\n" + next);
			FilterDocumentEditor d;
			d.Attach(&f);
			*rep = JournalApply(j, d, {}, false);
			return SerializeFilter(f);
		};
		JournalReport r;
		// you hid the rule; NeverSink only changed its header comment
		const EditJournal jh = jr("Show # $type->6l $tier->hightier\n\tSetFontSize 45\n",
		                          "Hide # $type->6l $tier->hightier\n\tSetFontSize 45\n");
		const std::string oh = apply(jh, "Show # $type->6l $tier->hightier !links_6lt1\n\tSetFontSize 45\n", &r);
		rep.check(r.conflicts.empty() && oh.find("Hide # $type->6l $tier->hightier !links_6lt1") != std::string::npos,
		          "T38h your Hide and NeverSink's new header comment are both kept, no conflict",
		          "conflicts=" + std::to_string(r.conflicts.size()));
		// you disabled the 2nd HasExplicitMod; NeverSink added one in front
		const EditJournal jd = jr(
			"Show # $type->rareid $tier->a\n\tHasExplicitMod \"A\"\n\tHasExplicitMod \"B\"\n\tHasExplicitMod \"C\"\n",
			"Show # $type->rareid $tier->a\n\tHasExplicitMod \"A\"\n\t#! HasExplicitMod \"B\"\n\tHasExplicitMod \"C\"\n");
		const std::string od = apply(jd,
			"Show # $type->rareid $tier->a\n\tHasExplicitMod \"Z\"\n\tHasExplicitMod \"A\"\n\tHasExplicitMod \"B\"\n\tHasExplicitMod \"C\"\n", &r);
		rep.check(r.conflicts.empty() && od.find("#! HasExplicitMod \"B\"") != std::string::npos &&
		              od.find("\tHasExplicitMod \"B\"") == std::string::npos && od.find("HasExplicitMod \"Z\"") != std::string::npos,
		          "T38h a disabled repeated line stays disabled when NeverSink adds another before it", od);
		// you changed ItemLevel < 84 to < 86; NeverSink dropped the ItemLevel >= 75 line
		const EditJournal ji = jr("Show # $type->x $tier->y\n\tItemLevel >= 75\n\tItemLevel < 84\n",
		                          "Show # $type->x $tier->y\n\tItemLevel >= 75\n\tItemLevel < 86\n");
		const std::string oi = apply(ji, "Show # $type->x $tier->y\n\tItemLevel < 84\n", &r);
		rep.check(r.conflicts.empty() && oi.find("ItemLevel < 86") != std::string::npos && oi.find("ItemLevel < 84") == std::string::npos &&
		              oi.find(">= 75") == std::string::npos,
		          "T38h a changed repeated line follows its own line, not its position", oi);
		// a line you added keeps its trailing comment
		const EditJournal jc = jr("Show # $type->x $tier->y\n\tSetFontSize 45\n",
		                          "Show # $type->x $tier->y\n\tSetFontSize 45\n\tPlayEffect Red # mine\n");
		const std::string oc = apply(jc, "Show # $type->x $tier->y\n\tSetFontSize 40\n", &r);
		rep.check(oc.find("\tPlayEffect Red # mine") != std::string::npos, "T38h an added line comes back exactly, comment included", oc);
	}

	// ---- T39i: a real version change (8.19.2a local -> the subscription's copy) ----
	// Read-only use of the user's OnlineFilters\ (never written). Skipped when absent.
	{
		wchar_t docs[MAX_PATH] = {};
		std::wstring onlineDir;
		if (GetEnvironmentVariableW(L"USERPROFILE", docs, MAX_PATH))
			onlineDir = std::wstring(docs) + L"\\Documents\\My Games\\Path of Exile\\OnlineFilters\\";
		std::wstring onlinePath;
		WIN32_FIND_DATAW fd;
		HANDLE h = onlineDir.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((onlineDir + L"*").c_str(), &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { onlinePath = onlineDir + fd.cFileName; break; } } while (FindNextFileW(h, &fd));
			FindClose(h);
		}
		bool ok = false;
		const FilterFile base = LoadFilter(exeDir + L"Filters\\default.filter", &ok);
		std::string onlineText;
		if (!onlinePath.empty()) {
			std::ifstream in(onlinePath, std::ios::binary);
			onlineText.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		}
		if (!ok || onlineText.empty() || !ParseOnlineHeader(onlineText).online) {
			rep.note("T39i skipped: no local default.filter or no subscription file");
		} else {
			// edit the colour of the first 20 NeverSink rules that have one
			FilterFile cur = base;
			int edited = 0;
			for (FilterBlock& b : cur.blocks) {
				if (edited >= 20 || FilterRuleKey(cur, b).empty() || b.idxTextColor < 0) continue;
				FilterLine& ln = cur.lines[b.idxTextColor];
				if (ln.values.size() < 3) continue;
				ln.values[0].text = "1"; ln.values[1].text = "2"; ln.values[2].text = "3";
				ln.dirty = true;
				edited++;
			}
			EditJournal j;
			JournalRecord(j, base, cur);
			FilterFile next = ParseFilter(StripOnlineHeader(onlineText));
			FilterDocumentEditor doc;
			doc.Attach(&next);
			const JournalReport r = JournalApply(j, doc, {}, false);
			rep.note("T39i " + FilterBannerValue(base, "VERSION") + " " + FilterBannerValue(base, "TYPE") + " -> " +
			         FilterBannerValue(next, "VERSION") + " " + FilterBannerValue(next, "TYPE") + ": recorded " +
			         std::to_string(j.rules.size()) + ", applied " + std::to_string(r.applied) + ", already " +
			         std::to_string(r.already) + ", conflicts " + std::to_string(r.conflicts.size()) + ", missing " +
			         std::to_string(r.missing.size()));
			rep.check((int)j.rules.size() == edited && r.applied + r.already + (int)r.missing.size() == (int)j.rules.size(),
			          "T39i every recorded rule is accounted for on the real newer version");
			// the most common edit, Show -> Hide, on the rules both versions have
			{
				FilterFile hid = base;
				int n = 0;
				for (FilterBlock& b : hid.blocks) {
					if (n >= 40 || FilterRuleKey(hid, b).empty() || b.hide) continue;
					FilterLine& h = hid.lines[b.headerLineIdx];
					h.keyword = "Hide";
					h.dirty = true;
					n++;
				}
				EditJournal jh;
				JournalRecord(jh, base, hid);
				FilterFile next2 = ParseFilter(StripOnlineHeader(onlineText));
				FilterDocumentEditor d2;
				d2.Attach(&next2);
				const JournalReport rh = JournalApply(jh, d2, {}, false);
				int hiddenNow = 0;
				for (const auto& [k, rr] : jh.rules) {
					for (const FilterBlock& b : next2.blocks)
						if (FilterRuleKey(next2, b) == k && b.hide) { hiddenNow++; break; }
				}
				rep.note("T39i Show->Hide on " + std::to_string(n) + " rules: applied " + std::to_string(rh.applied) +
				         ", already " + std::to_string(rh.already) + ", conflicts " + std::to_string(rh.conflicts.size()) +
				         ", missing " + std::to_string(rh.missing.size()) + ", hidden after apply " + std::to_string(hiddenNow));
				rep.check(rh.conflicts.empty() && hiddenNow == rh.applied + rh.already,
				          "T39i hiding rules survives the real update with no false conflict");
			}
		}
	}

	// ---- T31: undo capture cost on the real NeverSink file ----------------------
	{
		bool ok = false;
		FilterFile f = LoadFilter(exeDir + L"Filters\\default.filter", &ok);
		if (!ok) {
			rep.note("T31 skipped: no Filters\\default.filter next to the exe");
		} else {
			FilterDocumentEditor doc;
			doc.Attach(&f);
			FilterUndoHistory h;
			h.Reset(f.lines);
			LARGE_INTEGER fq, t0, t1;
			QueryPerformanceFrequency(&fq);
			const int reps = 50;
			QueryPerformanceCounter(&t0);
			bool any = false;
			for (int i = 0; i < reps; i++) any |= h.Capture(f.lines);
			QueryPerformanceCounter(&t1);
			const double idleMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart / reps;
			const std::string before = SerializeFilter(f);
			int li = -1;
			for (int bi = (int)f.blocks.size() / 2; bi < (int)f.blocks.size() && li < 0; bi++) li = doc.FindLine(bi, "SetFontSize");
			if (li >= 0) {
				f.lines[li].values[0].text = "41";
				f.lines[li].dirty = true;
			}
			QueryPerformanceCounter(&t0);
			const bool pushed = h.Capture(f.lines);
			const int u = h.Undo(f.lines);
			QueryPerformanceCounter(&t1);
			const double editMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
			char buf[128];
			snprintf(buf, sizeof(buf), "%zu lines, idle capture %.3f ms, capture+undo %.3f ms", f.lines.size(), idleMs, editMs);
			rep.check(!any && idleMs < 1.0, "T31 idle capture on the real file stays under 1 ms per frame", buf);
			rep.check(li >= 0 && pushed && u >= 0 && SerializeFilter(f) == before, "T31 one edit on the real file undoes byte-exact",
			          "li=" + std::to_string(li) + " pushed=" + std::to_string(pushed) + " u=" + std::to_string(u));
		}
	}

	rep.note("failures=" + std::to_string(rep.failures));
	printf("%s", rep.text.c_str());

	std::ofstream out(exeDir + L"filter_selftest.txt", std::ios::binary);
	if (out) out.write(rep.text.data(), (std::streamsize)rep.text.size());

	return rep.failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// --filter-import-probe: the paste button's exact path, headless.

int RunFilterImportProbe(const std::wstring& exeDir, const std::wstring& itemFile,
                         const std::wstring& filterFile)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	std::string repText;
	auto line = [&repText](const std::string& s) { repText += s + "\n"; };

	std::string text;
	if (!itemFile.empty()) {
		std::ifstream in(itemFile, std::ios::binary);
		text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	} else {
		text = ReadClipboardUtf8(nullptr);   // the same call the paste button makes
	}
	line("input: " + std::to_string(text.size()) + " bytes (" +
	     (itemFile.empty() ? "clipboard" : "file") + ")");

	FilterI18n i18n;
	i18n.Load(exeDir, "zh-rTW");
	ItemLibrary lib;
	lib.Load(exeDir, i18n);

	ImportedItem r = ParseGameItemText(text, i18n, lib.items());
	line("ok=" + std::to_string(r.ok ? 1 : 0));
	line("name='" + r.name + "'  base='" + r.baseRaw + "' -> '" + r.baseEn +
	     "'  classId='" + r.item.classId + "'");
	line("rarity=" + std::to_string(r.item.rarity) +
	     " ilvl=" + std::to_string(r.item.itemLevel) +
	     " drop=" + std::to_string(r.item.dropLevel) +
	     " q=" + std::to_string(r.item.quality) +
	     " size=" + std::to_string(r.item.width) + "x" + std::to_string(r.item.height) +
	     " sockets=" + std::to_string(r.item.sockets) +
	     "/" + std::to_string(r.item.linkedSockets));
	{
		std::string g = "groups=";
		for (const std::string& s : r.item.socketGroups) g += "[" + s + "]";
		g += "  influence=" + std::to_string(r.item.influence);
		g += std::string("  corrupted=") + (r.item.corrupted ? "1" : "0");
		g += std::string(" identified=") + (r.item.identified ? "1" : "0");
		g += std::string(" fractured=") + (r.item.fractured ? "1" : "0");
		g += std::string(" enchanted=") + (r.item.enchanted ? "1" : "0");
		line(g);
	}
	for (const ImportIssue& w : r.warnings) line(u8"warn: " + w.msg);

	int rc = r.ok ? 0 : 1;
	if (r.ok) {
		std::wstring fpath = filterFile.empty() ? exeDir + L"Filters\\default.filter"
		                                        : filterFile;
		bool okF = false;
		FilterFile f = LoadFilter(fpath, &okF);
		if (!okF) {
			line("filter: could not load (pass a .filter path as the 2nd argument)");
			rc = 2;
		} else {
			PreviewResult res = EvaluatePreview(f, r.item, i18n);
			line("filter blocks=" + std::to_string(f.blocks.size()));
			line(std::string("matched=") + (res.matched ? "1" : "0") +
			     " hidden=" + (res.hidden ? "1" : "0") +
			     " blockIdx=" + std::to_string(res.blockIdx) +
			     " fontSize=" + std::to_string(res.fontSize));
			if (res.blockIdx >= 0 && res.blockIdx < (int)f.blocks.size()) {
				line("rule: " + f.blocks[res.blockIdx].headerComment);
				line("rule zh: " + NeverSinkHeaderZh(f.blocks[res.blockIdx].headerComment));
			}
			if (!res.customSound.empty()) line("sound: " + res.customSound);
			if (!res.playEffect.empty()) line("beam: " + res.playEffect);
			if (!res.minimapIcon.empty()) line("icon: " + res.minimapIcon);
			std::string u = "unknownConds:";
			for (const std::string& k : res.unknownConds) u += " " + k;
			line(u);
		}
	}

	printf("%s", repText.c_str());
	std::ofstream out(exeDir + L"filter_import_probe.txt", std::ios::binary);
	if (out) out.write(repText.data(), (std::streamsize)repText.size());
	return rc;
}
