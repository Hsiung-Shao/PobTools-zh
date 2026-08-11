#pragma once

#include <string>

// Headless regression for POB's generated item names ("<title>, <base type>").
// See item_name_selftest.cpp for what it defends.
int RunItemNameSelftest();

// --tr "<english>": print what the ENGINE returns for one string, through the
// real pipeline. Exists because re-implementing the lookup rules in a script to
// "check" a dictionary change reproduces whatever the script author misread --
// a Python model of this pipeline once omitted the glossary layer and
// attributed a dozen working names to the wrong path. Ask the engine instead.
int RunTranslateProbe(const std::wstring& text);
