#include "ui_icons_data.h"
#include "ui_icons.h"

#include <imgui.h>

#include <cmath>
#include <cstring>

namespace {
#include "data/icons_lucide.inc"

const ImWchar kRanges[] = { (ImWchar)PobIcon::kRangeFirst, (ImWchar)PobIcon::kRangeLast, 0 };
} // namespace

namespace PobIcon {

const unsigned char* FontData() { return kLucideTtf; }
unsigned FontSize() { return kLucideTtfSize; }
const unsigned short* GlyphRanges() { return kRanges; }

float HheaAscentRatio(const std::vector<unsigned char>& d)
{
	auto u16 = [&](size_t o) { return (unsigned)(d[o] << 8 | d[o + 1]); };
	auto u32 = [&](size_t o) { return (unsigned)(d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]); };
	if (d.size() < 12) return 0.8f;
	const unsigned n = u16(4);
	for (unsigned i = 0; i < n; i++) {
		const size_t rec = 12 + (size_t)i * 16;
		if (rec + 16 > d.size()) break;
		if (memcmp(&d[rec], "hhea", 4) != 0) continue;
		const size_t off = u32(rec + 8);
		if (off + 8 > d.size()) break;
		const int asc = (short)u16(off + 4), desc = (short)u16(off + 6);
		if (asc <= 0 || asc - desc <= 0) break;
		return (float)asc / (float)(asc - desc);
	}
	return 0.8f;
}

void MergeInto(ImFontAtlas* atlas, float px, const ImFontConfig& mergeCfg, float primaryAscent)
{
	ImFontConfig ci = mergeCfg;
	ci.MergeMode = true;
	ci.FontDataOwnedByAtlas = false;
	ci.GlyphMinAdvanceX = px;
	// The icon's top is at (ascent - 0.96 px + offset) below the line top;
	// offset = px - ascent puts its 4%..96% box at 4%..96% of the line.
	ci.GlyphOffset.y = std::floor(px - std::floor(px * primaryAscent + 0.5f) + 0.5f);
	atlas->AddFontFromMemoryTTF((void*)kLucideTtf, (int)kLucideTtfSize, px, &ci, kRanges);
}

bool FaceHasIcons(const ImFont* face)
{
	return face && face->FindGlyphNoFallback((ImWchar)kProbe) != nullptr;
}

} // namespace PobIcon
