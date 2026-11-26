#include "pob_build_spec.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "win_util.h"

namespace {

// Opening tag text "<Name ...>" starting at or after `from`; npos when absent.
// Requires the next char after the name to end it, so "<Spec" never matches "<Specs".
size_t find_tag(const std::string& xml, const char* name, size_t from, std::string& tag)
{
	const std::string open = std::string("<") + name;
	for (size_t p = xml.find(open, from); p != std::string::npos; p = xml.find(open, p + 1)) {
		const char c = p + open.size() < xml.size() ? xml[p + open.size()] : '\0';
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '>' && c != '/') continue;
		const size_t end = xml.find('>', p);
		if (end == std::string::npos) return std::string::npos;
		tag = xml.substr(p, end - p);
		return p;
	}
	return std::string::npos;
}

std::string attr(const std::string& tag, const char* key)
{
	const std::string k = std::string(" ") + key + "=\"";
	size_t a = tag.find(k);
	if (a == std::string::npos) { // attributes may also be split by newlines/tabs
		for (const char sep : { '\n', '\t', '\r' }) {
			a = tag.find(std::string(1, sep) + key + "=\"");
			if (a != std::string::npos) break;
		}
		if (a == std::string::npos) return "";
	}
	a += k.size();
	const size_t b = tag.find('"', a);
	return b == std::string::npos ? "" : tag.substr(a, b - a);
}

void list_rec(const std::wstring& dir, std::vector<std::wstring>& out, int depth)
{
	if (depth > 16) return; // junction loops
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		const std::wstring n = fd.cFileName;
		if (n == L"." || n == L"..") continue;
		const std::wstring p = dir + L"\\" + n;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) list_rec(p, out, depth + 1);
		else if (n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".xml") == 0) out.push_back(p);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

} // namespace

std::vector<std::wstring> ListPobBuilds(const std::wstring& buildDir)
{
	std::vector<std::wstring> out;
	std::wstring d = buildDir;
	while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
	if (!d.empty()) list_rec(d, out, 0);
	std::sort(out.begin(), out.end());
	return out;
}

bool ParsePobBuildSpec(const std::string& xml, PobBuildSpec& out, std::string* err)
{
	out = PobBuildSpec();
	auto fail = [&](const char* m) { if (err) *err = m; return false; };
	std::string tag;
	if (find_tag(xml, "Build", 0, tag) != std::string::npos) {
		out.className = attr(tag, "className");
		out.ascendClassName = attr(tag, "ascendClassName");
	}
	std::string treeTag;
	const size_t tree = find_tag(xml, "Tree", 0, treeTag);
	if (tree == std::string::npos) return fail("no <Tree> in build file");
	int active = atoi(attr(treeTag, "activeSpec").c_str());
	if (active < 1) active = 1;
	std::string spec, first;
	size_t p = tree;
	for (int i = 1;; i++) {
		p = find_tag(xml, "Spec", p + 1, tag);
		if (p == std::string::npos) break;
		if (i == 1) first = tag;
		if (i == active) { spec = tag; break; }
	}
	if (spec.empty()) spec = first; // POB falls back to the first spec too
	if (spec.empty()) return fail("no <Spec> in build file");
	out.treeVersion = attr(spec, "treeVersion");
	const std::string cls = attr(spec, "classId");
	out.classId = cls.empty() ? -1 : atoi(cls.c_str());
	const std::string nodes = attr(spec, "nodes");
	for (size_t a = 0; a < nodes.size();) {
		size_t b = nodes.find(',', a);
		if (b == std::string::npos) b = nodes.size();
		const int id = atoi(nodes.substr(a, b - a).c_str());
		if (id > 0) out.nodes.insert(id);
		a = b + 1;
	}
	return true;
}

bool LoadPobBuildSpec(const std::wstring& path, PobBuildSpec& out, std::string* err)
{
	std::string xml;
	if (!read_file_utf8(path, xml)) { if (err) *err = "cannot read build file"; return false; }
	return ParsePobBuildSpec(xml, out, err);
}
