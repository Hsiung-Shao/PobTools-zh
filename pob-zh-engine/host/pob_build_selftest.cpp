// --pob-build-selftest: PobBuildSpec 的自檢(只依 pob_build_spec.h 的規格寫成,固定 XML 字串 + 暫存檔)。
#include "pob_build_spec.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "win_util.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>

namespace {

void pbs_log(FILE* f, const char* fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	vprintf(fmt, ap); va_end(ap);
	if (f) { va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); }
}

// 兩個 Spec:第 1 個是舊的(3_29),第 2 個是作用中的(3_30);classId 與 ascendClassId 刻意不同值,
// 才能抓到「classId 讀成 ascendClassId」或「className 讀錯屬性」的 bug。
std::string make_xml(const char* treeOpenTag)
{
	return std::string(
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<PathOfBuilding>\n"
		"\t<Build level=\"95\" className=\"Marauder\" ascendClassName=\"Juggernaut\" targetVersion=\"3_0\">\n"
		"\t\t<PlayerStat stat=\"Life\" value=\"5000\"/>\n"
		"\t</Build>\n\t") + treeOpenTag + "\n" +
		"\t\t<Spec title=\"Old\" treeVersion=\"3_29\" classId=\"0\" ascendClassId=\"0\" nodes=\"11,22,33\">\n"
		"\t\t\t<Sockets/>\n"
		"\t\t</Spec>\n"
		"\t\t<Spec title=\"Main\" treeVersion=\"3_30\" classId=\"1\" ascendClassId=\"3\" nodes=\"100,200,300,400\" masteryEffects=\"{1,2}\">\n"
		"\t\t\t<Sockets><Socket nodeId=\"2491\" itemId=\"5\"/></Sockets>\n"
		"\t\t</Spec>\n"
		"\t</Tree>\n"
		"</PathOfBuilding>\n";
}

bool ends_with(const std::wstring& s, const std::wstring& suffix)
{
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

int RunPobBuildSelfTest(const std::wstring& exeDir)
{
	AllocConsole();
	FILE* dummy; freopen_s(&dummy, "CONOUT$", "w", stdout);
	FILE* rep = nullptr;
	_wfopen_s(&rep, (exeDir + L"pob_build_selftest.txt").c_str(), L"w");

	int pass = 0, fail = 0;
	auto check = [&](bool ok, const char* name, const std::string& detail = "") {
		pbs_log(rep, "[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name,
		        detail.empty() ? "" : "  ", detail.c_str());
		ok ? pass++ : fail++;
	};

	// T1 作用中 Spec 是 activeSpec="2":節點、職業、版本取第 2 個 Spec,不是第 1 個也不是兩個的聯集
	{
		PobBuildSpec s;
		std::string err;
		const bool ok = ParsePobBuildSpec(make_xml("<Tree activeSpec=\"2\">"), s, &err);
		check(ok, "T1a activeSpec=2 解析成功", err);
		check(s.nodes == std::set<int>({ 100, 200, 300, 400 }),
		      "T1b 節點取自第 2 個 Spec(不含第 1 個的 11,22,33)", std::to_string(s.nodes.size()) + " 個");
		check(s.classId == 1, "T1c classId 取自 Spec 的 classId(不是 ascendClassId=3)", std::to_string(s.classId));
		check(s.treeVersion == "3_30", "T1d treeVersion 取自第 2 個 Spec", s.treeVersion);
		check(s.className == "Marauder" && s.ascendClassName == "Juggernaut",
		      "T1e className / ascendClassName 取自 <Build>", s.className + "/" + s.ascendClassName);
	}

	// T1 沒寫 activeSpec 時預設第 1 個 Spec
	{
		PobBuildSpec s;
		std::string err;
		const bool ok = ParsePobBuildSpec(make_xml("<Tree>"), s, &err);
		check(ok && s.nodes == std::set<int>({ 11, 22, 33 }) && s.classId == 0 && s.treeVersion == "3_29",
		      "T1f 未寫 activeSpec 時用第 1 個 Spec", err);
	}

	// T1 經檔案讀取 (LoadPobBuildSpec) 與字串解析結果一致
	std::wstring tmp;
	{
		wchar_t buf[MAX_PATH];
		GetTempPathW(MAX_PATH, buf);
		tmp = std::wstring(buf) + L"pob_build_selftest_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
		std::error_code ec;
		std::filesystem::remove_all(tmp, ec);
		std::filesystem::create_directories(tmp + L"sub", ec);
		write_file_utf8(tmp + L"a.xml", make_xml("<Tree activeSpec=\"2\">"));
		write_file_utf8(tmp + L"sub\\b.xml", make_xml("<Tree>"));
		write_file_utf8(tmp + L"c.txt", "not a build");

		PobBuildSpec s;
		std::string err;
		const bool ok = LoadPobBuildSpec(tmp + L"a.xml", s, &err);
		check(ok && s.nodes == std::set<int>({ 100, 200, 300, 400 }) && s.ascendClassName == "Juggernaut",
		      "T1g 從檔案讀入與字串解析結果一致", err);

		// 列舉:遞迴進子資料夾、只收 .xml、已排序
		std::vector<std::wstring> list = ListPobBuilds(tmp);
		bool hasA = false, hasB = false, hasTxt = false;
		for (const std::wstring& p : list) {
			if (ends_with(p, L"a.xml")) hasA = true;
			if (ends_with(p, L"b.xml")) hasB = true;
			if (ends_with(p, L"c.txt")) hasTxt = true;
		}
		check(list.size() == 2 && hasA && hasB && !hasTxt,
		      "T1h ListPobBuilds 遞迴收 .xml 且不收 .txt", std::to_string(list.size()) + " 筆");
		check(std::is_sorted(list.begin(), list.end()), "T1i ListPobBuilds 結果已排序");
		check(ListPobBuilds(tmp + L"no_such_dir").empty(), "T1j 資料夾不存在時回空清單");
	}

	// T2 壞檔:缺 <Spec>、空字串、非 XML 都回 false 且 err 非空,不崩潰
	{
		const char* bad[] = {
			"",
			"this is not xml at all",
			"<PathOfBuilding><Build className=\"Witch\"/><Tree activeSpec=\"1\"></Tree></PathOfBuilding>",
		};
		const char* names[] = { "T2a 空字串回失敗且 err 非空", "T2b 非 XML 回失敗且 err 非空", "T2c 缺 <Spec> 回失敗且 err 非空" };
		for (int i = 0; i < 3; i++) {
			PobBuildSpec s;
			std::string err;
			const bool ok = ParsePobBuildSpec(bad[i], s, &err);
			check(!ok && !err.empty(), names[i], err);
		}
		PobBuildSpec s;
		check(!ParsePobBuildSpec("", s, nullptr), "T2d err 傳 nullptr 時壞檔仍回失敗、不崩潰");
		std::string err;
		check(!LoadPobBuildSpec(tmp + L"missing.xml", s, &err) && !err.empty(),
		      "T2e 檔案不存在回失敗且 err 非空", err);
	}

	std::error_code ec;
	std::filesystem::remove_all(tmp, ec);

	pbs_log(rep, "\n%d passed, %d failed\n", pass, fail);
	pbs_log(rep, fail == 0 ? "RESULT PASS\n" : "RESULT FAIL\n");
	if (rep) fclose(rep);
	return fail == 0 ? 0 : 1;
}
