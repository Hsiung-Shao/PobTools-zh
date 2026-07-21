#include "core_tex_manipulation.h"
#include "core_compress.h"

#include <filesystem>
#include <fstream>
#include <string_view>
#include <unordered_map>

#include "ui_local.h"

#include <gli/load_dds.hpp>
#include <gli/save_dds.hpp>
#include <gsl/span>
#include <zstd.h>

using namespace std::string_view_literals;

Texture_c::Texture_c()
{
}

static std::unordered_map<std::string_view, gli::format> g_formatStrToId{
	{"RGB"sv, gli::format::FORMAT_RGB8_UNORM_PACK8},
	{"RGBA"sv, gli::format::FORMAT_RGBA8_UNORM_PACK8},
	{"BC1"sv, gli::format::FORMAT_RGBA_DXT1_UNORM_BLOCK8},
	{"BC7"sv, gli::format::FORMAT_RGBA_BP_UNORM_BLOCK16},
};

static std::unordered_map<gli::format, std::string_view> g_formatIdToStr{
	{gli::format::FORMAT_RGB8_UNORM_PACK8, "RGB"sv},
	{gli::format::FORMAT_RGBA8_UNORM_PACK8, "RGBA"sv},
	{gli::format::FORMAT_RGBA_DXT1_UNORM_BLOCK8, "BC1"sv},
	{gli::format::FORMAT_RGBA_BP_UNORM_BLOCK16, "BC7"sv},
};

bool Texture_c::Allocate(gli::format format, int width, int height, int layerCount, int mipCount)
{
	auto extent = gli::texture2d_array::extent_type(width, height);
	tex = gli::texture2d_array(format, extent, layerCount, mipCount);
	return true;
}

bool Texture_c::Allocate(std::string_view format, int width, int height, int layerCount, int mipCount)
{
	gli::format formatId = gli::format::FORMAT_UNDEFINED;
	if (auto I = g_formatStrToId.find(format); I != g_formatStrToId.end())
		formatId = I->second;
	else
		return false;

	return Allocate(formatId, width, height, layerCount, mipCount);
}

bool Texture_c::Load(const char* fileName)
{
	auto path = std::filesystem::u8path(fileName);
	std::vector<char> fileData;
	{
		std::ifstream is(path, std::ios::binary);
		if (!is.is_open())
			return false;
		is.seekg(0, std::ios::end);
		fileData.resize(is.tellg());
		is.seekg(0, std::ios::beg);
		is.read(fileData.data(), fileData.size());
	}

	auto fileTex = gli::load_dds(fileData.data(), fileData.size());
	if (fileTex.faces() != 1)
		return false;

	tex = gli::texture2d_array(fileTex);
	return !tex.empty();
}



bool Texture_c::Save(const char* fileName)
{
	if (tex.empty())
		return false;

	bool wantDds = false;
	bool wantZstd = false;
	auto path = std::filesystem::u8path(fileName);
	auto subPath = path;
	if (subPath.extension() == ".zst") {
		wantZstd = true;
		subPath = subPath.replace_extension();
	}

	if (subPath.extension() == ".dds") {
		wantDds = true;
		subPath = subPath.replace_extension();
	}

	if (!wantDds)
		return false;

	std::vector<char> fileData;
	if (!gli::save_dds(tex, fileData))
		return false;

	if (wantZstd) {
		auto res = CompressZstandard(as_bytes(gsl::span(fileData)));
		if (!res.has_value())
			return false;
		fileData = std::move(res.value());
	}

	std::ofstream os(path, std::ios::binary);
	if (!os.is_open())
		return false;

	os.write(fileData.data(), fileData.size());
	return !!os;
}

TextureInfo_s Texture_c::Info() const
{
	TextureInfo_s ret = {};
	ret.formatId = tex.format();
	if (auto I = g_formatIdToStr.find(ret.formatId); I != g_formatIdToStr.end())
		ret.formatStr = I->second;
	else
		ret.formatStr = "<unknown>"sv;
	ret.width = tex.extent().x;
	ret.height = tex.extent().y;
	ret.layerCount = (int)tex.layers();
	ret.mipCount = (int)tex.levels();
	return ret;
}

bool Texture_c::IsValid() const
{
	return !tex.empty();
}

//bool Texture_c::SetLayer(Texture_c& srcTex, int targetLayer)
//{
//#if 0
//	if (srcTex.tex.extent() != tex.extent())
//		return false;
//	if (srcTex.tex.layers() != dstTex.layers())
//	tex.copy(srcTex, 0, 0, )
//#endif
//	return false;
//}
//
//bool Texture_c::CopyImage(Texture_c& srcTex, int targetX, int targetY)
//{
//	return false;
//}
//
//bool Texture_c::Transcode(gli::format format)
//{
//	return false;
//}
//
//bool Texture_c::GenerateMipmaps()
//{
//	return false;
//}

bool Texture_c::StackTextures(std::vector<Texture_c> textures)
{
	if (textures.empty())
		return false;

	const auto dstFormat = textures.front().tex.format();
	const auto dstExtent = textures.front().tex.extent();
	const auto dstLevels = textures.front().tex.levels();

	// Ensure that all the source textures are flat and compatible.
	for (auto& src : textures)
		if (src.tex.layers() != 1 ||
			src.tex.faces() != 1 ||
			src.tex.extent() != dstExtent ||
			src.tex.format() != dstFormat ||
			src.tex.levels() != dstLevels)
		{
			return false;
		}

	tex = gli::texture2d_array(dstFormat, dstExtent, textures.size(), dstLevels);
	for (size_t layerIdx = 0; layerIdx < textures.size(); ++layerIdx) {
		auto& src = textures[layerIdx].tex;
		for (size_t levelIdx = 0; levelIdx < dstLevels; ++levelIdx)
			tex.copy(src, 0, 0, levelIdx, layerIdx, 0, levelIdx);
	}

	return true;
}
