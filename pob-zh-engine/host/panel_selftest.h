// Headless check that an IToolPanel behaves as a launcher tab: opened with
// --panel-selftest, report at <exeDir>PobTools\, 0 = pass.
//
// Needs a desktop (it creates a hidden window and a GL context) but no
// interaction, and never takes focus.
#pragma once

#include <string>

int RunPanelSelfTest(const std::wstring& exeDir);
