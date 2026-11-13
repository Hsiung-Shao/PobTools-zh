// Offline characterization checks for the two data updaters (AtlasUpdater, PassiveTreeUpdater).
//
// Safety net for refactoring them: every scenario runs the real updater on its real worker thread against a
// sandbox exeDir under %TEMP% and a fake network (HttpSetTestSource), so nothing touches the real Data\ or the
// internet. Report: <exeDir>atlas_update_selftest.txt / pt_update_selftest.txt; exit 0 = all PASS.
#pragma once

#include <string>

// "pob-zh.exe --atlas-update-selftest"
int RunAtlasUpdateSelfTest(const std::wstring& exeDir);
// "pob-zh.exe --pt-update-selftest"
int RunPassiveUpdateSelfTest(const std::wstring& exeDir);
