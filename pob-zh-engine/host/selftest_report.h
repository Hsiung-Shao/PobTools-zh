// Shared selftest report: one "PASS  " / "FAIL  " line per check, "RESULT PASS" / "RESULT FAIL" last.
// Include after the .cpp's own WIN32_LEAN_AND_MEAN / <windows.h> (win_util.h pulls in windows.h).
#pragma once

#include "win_util.h"
#include <cstdio>
#include <string>

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

	// Appends the RESULT line, echoes the report to stdout, writes it to path; returns the exit code.
	int finish(const std::wstring& path)
	{
		text += failures == 0 ? "\nRESULT PASS\n" : "\nRESULT FAIL\n";
		printf("%s", text.c_str());
		write_file_utf8(path, text);
		return failures == 0 ? 0 : 1;
	}
};
