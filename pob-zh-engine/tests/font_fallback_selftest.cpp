// Headless check of the engine's fallback font chain (engine/render/r_font_fallback.h)
// against the fonts that actually ship in dist\Fonts\.
//
// Why a dedicated target: r_font.cpp needs a renderer and a GL context, and the
// launcher's --font-*-selftest series covers the launcher's ImGui atlas -- a
// different subsystem. This runs the same FtFallbackChain code the engine
// links, with real FreeType and the real TTFs, so "Noto lacks U+62BA, FZ_ZY
// supplies it" is asserted rather than assumed.
//
// Build:  cmake --build build --config Release --target font_fallback_selftest
// Run:    build\Release\font_fallback_selftest.exe [dist\Fonts]
//         (needs freetype.dll on PATH -- dist\engine has it)
// Exit 0 = all PASS; 2 = some FAIL; 1 = could not even start.
#include "../engine/render/r_font_fallback.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static int g_checks = 0, g_failures = 0;
static void check(const char* name, bool ok, const std::string& detail = "") {
	g_checks++;
	if (!ok) g_failures++;
	printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name,
	       detail.empty() ? "" : "  (", detail.empty() ? "" : (detail + ")").c_str());
}

static bool fileExists(const std::string& p) {
	std::ifstream f(p);
	return f.good();
}

int main(int argc, char** argv) {
	std::string fontDir = argc > 1 ? argv[1] : "dist/Fonts";
	if (!fontDir.empty() && fontDir.back() != '/' && fontDir.back() != '\\') fontDir += '/';
	const std::string noto = fontDir + "NotoSansTC-Regular.ttf";
	const std::string fz = fontDir + "FZ_ZY.ttf";
	if (!fileExists(noto) || !fileExists(fz)) {
		printf("cannot start: need %s and %s\n", noto.c_str(), fz.c_str());
		return 1;
	}

	// ---- CollectFallbacks: pure rule, fake filesystem -------------------------
	{
		FtFallbackChain chain;
		std::vector<std::string> cands = {
			"D:/x/Fonts/NotoSansTC-Regular.ttf",   // user's pick (POB_ZH_FONTFILE)
			"D:/x/Fonts/notosanstc-regular.TTF",   // the named default again, different case
			"D:/x/Fonts/FZ_ZY.ttf",
			"D:/x/Fonts/CJKFallback.ttf",           // does not exist
			"Fonts/NotoSansTC-Regular.ttf",         // CFG_DATAPATH legacy entries
			"Fonts/FZ_ZY.ttf",
		};
		auto exists = [](const std::string& p) { return p.find("CJKFallback") == std::string::npos && p[0] == 'D'; };
		chain.CollectFallbacks(cands, 0, exists);
		check("collect: primary and its case-variant spelling are skipped, missing file skipped",
		      chain.paths.size() == 1 && chain.paths[0] == "D:/x/Fonts/FZ_ZY.ttf",
		      "n=" + std::to_string(chain.paths.size()));
		// Primary is FZ_ZY: the two Noto spellings collapse to one entry.
		chain.CollectFallbacks(cands, 2, exists);
		check("collect: duplicates of a fallback collapse (case-insensitive)",
		      chain.paths.size() == 1 && chain.paths[0] == cands[0],
		      "n=" + std::to_string(chain.paths.size()));
		// Nothing else exists: empty chain, no crash.
		chain.CollectFallbacks(cands, 0, [](const std::string&) { return false; });
		check("collect: nothing exists -> empty chain", chain.paths.empty());
	}

	// ---- FaceFor: real FreeType, real shipped fonts --------------------------
	FT_Library lib = nullptr;
	if (FT_Init_FreeType(&lib)) { printf("cannot start: FT_Init_FreeType failed\n"); return 1; }
	FT_Face notoFace = nullptr, fzFace = nullptr;
	if (FT_New_Face(lib, noto.c_str(), 0, &notoFace) || FT_New_Face(lib, fz.c_str(), 0, &fzFace)) {
		printf("cannot start: FT_New_Face failed\n");
		return 1;
	}
	// Ground truth for the assertions below, from the faces themselves -- if a
	// future font build changes coverage, the test says so instead of lying.
	const char32_t kHit = 0x64CA;      // 擊: both fonts
	const char32_t kOfficial = 0x62BA; // 抺: GGPK text, Noto subset lacks it, FZ_ZY has it
	const char32_t kVariant = 0x6483;  // 撃: old-dictionary typo, same shape of gap
	const char32_t kNowhere = 0x0378;  // unassigned codepoint: no font has it
	check("precondition: Noto lacks U+62BA and FZ_ZY has it",
	      FT_Get_Char_Index(notoFace, kOfficial) == 0 && FT_Get_Char_Index(fzFace, kOfficial) != 0);
	check("precondition: Noto lacks U+6483 and FZ_ZY has it",
	      FT_Get_Char_Index(notoFace, kVariant) == 0 && FT_Get_Char_Index(fzFace, kVariant) != 0);
	check("precondition: no shipped font has U+0378",
	      FT_Get_Char_Index(notoFace, kNowhere) == 0 && FT_Get_Char_Index(fzFace, kNowhere) == 0);

	{
		FtFallbackChain chain;
		chain.paths = { fz };
		int opens = 0;
		auto log = [&](const std::string&, bool ok) { if (ok) opens++; };

		FT_Face f = chain.FaceFor(lib, notoFace, kHit, log);
		check("hit in primary -> primary face, fallbacks NOT opened", f == notoFace && !chain.opened && opens == 0);

		f = chain.FaceFor(lib, notoFace, kOfficial, log);
		check("U+62BA (official GGPK text) -> comes from FZ_ZY", f != notoFace && f != nullptr && chain.opened && opens == 1
		      && FT_Get_Char_Index(f, kOfficial) != 0);
		FT_Face first = f;

		f = chain.FaceFor(lib, notoFace, kVariant, log);
		check("U+6483 -> same fallback face reused, no second open", f == first && opens == 1);

		f = chain.FaceFor(lib, notoFace, kNowhere, log);
		check("missing everywhere -> primary (.notdef box), never null", f == notoFace);

		f = chain.FaceFor(lib, notoFace, kHit, log);
		check("hit in primary after chain opened -> still primary", f == notoFace);
		chain.Close();
		check("close releases the fallback faces", chain.faces.empty());
	}
	{
		// No fallbacks configured: the old behaviour, byte for byte.
		FtFallbackChain chain;
		FT_Face f = chain.FaceFor(lib, notoFace, kOfficial);
		check("empty chain -> primary for a miss", f == notoFace && chain.opened && chain.faces.empty());
	}
	{
		// Reverse direction: FZ_ZY primary lacks U+FF62 (｢), Noto has it.
		FtFallbackChain chain;
		chain.paths = { noto };
		const char32_t bracket = 0xFF62;
		check("precondition: FZ_ZY lacks U+FF62 and Noto has it",
		      FT_Get_Char_Index(fzFace, bracket) == 0 && FT_Get_Char_Index(notoFace, bracket) != 0);
		FT_Face f = chain.FaceFor(lib, fzFace, bracket);
		check("FZ_ZY primary, U+FF62 -> comes from Noto", f != fzFace && f != nullptr && FT_Get_Char_Index(f, bracket) != 0);
		chain.Close();
	}
	{
		// A path that cannot be opened is skipped, not fatal, and reported.
		FtFallbackChain chain;
		chain.paths = { fontDir + "does-not-exist.ttf", fz };
		std::vector<std::string> failed;
		FT_Face f = chain.FaceFor(lib, notoFace, kOfficial,
		                          [&](const std::string& p, bool ok) { if (!ok) failed.push_back(p); });
		check("unopenable fallback is skipped and reported; next one still serves",
		      failed.size() == 1 && f != notoFace && FT_Get_Char_Index(f, kOfficial) != 0);
		chain.Close();
	}

	FT_Done_Face(notoFace);
	FT_Done_Face(fzFace);
	FT_Done_FreeType(lib);
	printf("%d checks, %d failed\nRESULT %s\n", g_checks, g_failures, g_failures ? "FAIL" : "PASS");
	return g_failures ? 2 : 0;
}
