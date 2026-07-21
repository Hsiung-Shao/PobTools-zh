#include "custom_rules_io.h"
#include "filter_parser.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fstream>
#include <vector>

namespace {

const char* kBegin = "#pobtools-custom-begin";
const char* kEnd = "#pobtools-custom-end";

bool line_is(const FilterLine& ln, const char* sentinel)
{
	if (ln.kind != FilterLineKind::Comment) return false;
	std::string code = ln.raw.substr(ln.indent.size());
	return code.compare(0, strlen(sentinel), sentinel) == 0;
}

FilterLine comment_line(const std::string& text)
{
	return ParseFilterLine(text);
}

// The block's condition signature: header + every condition line, serialized.
std::string block_signature(const FilterFile& f, const FilterBlock& b)
{
	std::string sig = FilterSerializeLine(f.lines[b.headerLineIdx]);
	for (int li : b.lineIdx) {
		if (f.lines[li].kind != FilterLineKind::Condition) continue;
		sig += '\n';
		sig += FilterSerializeLine(f.lines[li]);
	}
	return sig;
}

} // namespace

CustomZone FindCustomZone(const FilterFile& f)
{
	CustomZone z;
	for (int i = 0; i < (int)f.lines.size(); i++) {
		if (z.beginLine < 0 && line_is(f.lines[i], kBegin)) { z.beginLine = i; continue; }
		if (z.beginLine >= 0 && line_is(f.lines[i], kEnd)) { z.endLine = i; break; }
	}
	return z;
}

CustomZone EnsureCustomZone(FilterDocumentEditor& doc)
{
	FilterFile* f = doc.file();
	CustomZone z = FindCustomZone(*f);
	if (z.present()) return z;

	if (z.beginLine >= 0) {
		// Damaged: begin without end. Close the zone before the next block
		// header (or at end of file).
		int at = (int)f->lines.size();
		for (int i = z.beginLine + 1; i < (int)f->lines.size(); i++)
			if (f->lines[i].kind == FilterLineKind::BlockHeader) { at = i; break; }
		f->lines.insert(f->lines.begin() + at, comment_line(kEnd));
		f->dirty = true;
		doc.RebuildBlocks();
		return FindCustomZone(*f);
	}

	// Create: after the leading comment/blank banner, before the first header.
	int at = 0;
	for (int i = 0; i < (int)f->lines.size(); i++) {
		FilterLineKind k = f->lines[i].kind;
		if (k == FilterLineKind::Comment || k == FilterLineKind::Blank) { at = i + 1; continue; }
		break;
	}
	std::vector<FilterLine> ins;
	ins.push_back(comment_line("#==============================================="));
	ins.push_back(comment_line(std::string(kBegin) + " v=1"));
	ins.push_back(comment_line(u8"# ===== PobTools 自訂區（最高優先，由編輯器管理）====="));
	ins.push_back(comment_line(kEnd));
	ins.push_back(comment_line("#==============================================="));
	ins.push_back(FilterLine{});  // blank separator (default kind is fine: raw "")
	ins.back().kind = FilterLineKind::Blank;
	f->lines.insert(f->lines.begin() + at, ins.begin(), ins.end());
	f->dirty = true;
	doc.RebuildBlocks();
	return FindCustomZone(*f);
}

std::string ExportCustomRules(const FilterFile& f, const std::vector<int>& blockIdx,
                              const std::string& name)
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	char date[32];
	snprintf(date, sizeof(date), "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);

	std::string out;
	out += "#pobtools-rules v=1 name=" + name + " saved=" + date + " source=" + f.name + "\r\n";
	out += "\r\n";
	for (int bi : blockIdx) {
		if (bi < 0 || bi >= (int)f.blocks.size()) continue;
		const FilterBlock& b = f.blocks[bi];
		for (int li = b.lineIdx.front(); li <= b.lineIdx.back(); li++) {
			out += FilterSerializeLine(f.lines[li]);
			out += "\r\n";
		}
		if (f.lines[b.lineIdx.back()].kind != FilterLineKind::Blank) out += "\r\n";
	}
	return out;
}

bool SaveCustomRulesFile(const std::wstring& path, const std::string& fragment, std::string* err)
{
	std::ofstream o(path, std::ios::binary);
	if (!o) { if (err) *err = "cannot open file for writing"; return false; }
	o.write("\xEF\xBB\xBF", 3);
	o.write(fragment.data(), (std::streamsize)fragment.size());
	o.close();
	if (!o) { if (err) *err = "write failed"; return false; }
	return true;
}

int ImportCustomRules(FilterDocumentEditor& doc, const std::string& fragment, std::string* err)
{
	FilterFile sub = ParseFilter(fragment);
	if (sub.blocks.empty()) {
		if (err) *err = u8"檔案中沒有可導入的規則區塊";
		return -1;
	}

	CustomZone z = EnsureCustomZone(doc);
	if (!z.present()) {
		if (err) *err = u8"無法建立自訂區";
		return -1;
	}
	FilterFile* f = doc.file();

	// Existing zone signatures for duplicate detection.
	std::vector<std::string> have;
	for (const FilterBlock& b : f->blocks)
		if (b.headerLineIdx > z.beginLine && b.headerLineIdx < z.endLine)
			have.push_back(block_signature(*f, b));

	int imported = 0;
	int insertAt = z.endLine;  // grows as we insert; sentinel stays below
	for (const FilterBlock& sb : sub.blocks) {
		std::string sig = block_signature(sub, sb);
		bool dup = false;
		for (const std::string& h : have)
			if (h == sig) { dup = true; break; }
		if (dup) continue;

		std::vector<FilterLine> copy(sub.lines.begin() + sb.lineIdx.front(),
		                             sub.lines.begin() + sb.lineIdx.back() + 1);
		// Blocks glued to EOF may lack a trailing separator.
		if (copy.back().kind != FilterLineKind::Blank) {
			FilterLine blank;
			blank.kind = FilterLineKind::Blank;
			copy.push_back(blank);
		}
		f->lines.insert(f->lines.begin() + insertAt, copy.begin(), copy.end());
		insertAt += (int)copy.size();
		have.push_back(std::move(sig));
		imported++;
	}

	if (imported > 0) {
		f->dirty = true;
		doc.RebuildBlocks();
	}
	return imported;
}
