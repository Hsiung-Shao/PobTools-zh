// SimpleGraphic Engine -- PobTools addition
//
// Fallback face chain for the FreeType text path. Kept renderer-free (only
// FreeType and the standard library) so tests/font_fallback_selftest.cpp can
// exercise the exact code r_font.cpp runs, against the fonts that actually
// ship, without a GL context.
//
// Why it exists: the default Noto Sans TC is a subset build and lacks
// characters the game's own text uses ("左旋抺除之兆" -- U+62BA is what GGG
// wrote). A miss used to render .notdef -- a box -- with nothing in any log.
#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cctype>
#include <functional>
#include <string>
#include <vector>

struct FtFallbackChain {
	// Paths tried in order for a codepoint the primary face lacks. Filled by
	// CollectFallbacks; opened lazily by FaceFor so a primary that covers
	// everything never pays for loading the others.
	std::vector<std::string> paths;
	std::vector<FT_Face> faces;
	bool opened = false;

	// Same file, spelled twice: the user's pick is usually one of the named
	// defaults spelled again, and Windows paths are case-insensitive.
	static bool SameFile(const std::string& a, const std::string& b) {
		if (a.size() != b.size()) return false;
		for (size_t i = 0; i < a.size(); i++) {
			if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
		}
		return true;
	}

	// Every candidate other than the primary that exists, in the same
	// priority order, without duplicates. `exists` is injected so the rule can
	// be tested without touching the disk.
	void CollectFallbacks(const std::vector<std::string>& candidates, size_t primaryIdx,
	                      const std::function<bool(const std::string&)>& exists) {
		paths.clear();
		for (size_t j = 0; j < candidates.size(); j++) {
			if (j == primaryIdx || SameFile(candidates[j], candidates[primaryIdx])) continue;
			bool dup = false;
			for (const std::string& have : paths) {
				if (SameFile(have, candidates[j])) { dup = true; break; }
			}
			if (dup) continue;
			if (exists(candidates[j])) paths.push_back(candidates[j]);
		}
	}

	// The face to render `cp` from: the primary when it has the glyph, else the
	// first fallback that does, else the primary again so .notdef still draws
	// (a visible box beats a silent blank). `log(path, ok)` fires once per
	// fallback file, when the chain is first opened.
	FT_Face FaceFor(FT_Library library, FT_Face primary, char32_t cp,
	                const std::function<void(const std::string&, bool)>& log = nullptr) {
		if (FT_Get_Char_Index(primary, (FT_ULong)cp) != 0) return primary;
		if (!opened) {
			opened = true;
			for (const std::string& path : paths) {
				FT_Face f = nullptr;
				const bool ok = FT_New_Face(library, path.c_str(), 0, &f) == 0;
				if (ok) faces.push_back(f);
				if (log) log(path, ok);
			}
		}
		for (FT_Face f : faces) {
			if (FT_Get_Char_Index(f, (FT_ULong)cp) != 0) return f;
		}
		return primary;
	}

	void Close() {
		for (FT_Face f : faces) FT_Done_Face(f);
		faces.clear();
	}
};
