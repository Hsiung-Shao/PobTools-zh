#pragma once

#include <filesystem>
#include <optional>

#include <gli/texture2d_array.hpp>

struct TextureInfo_s
{
	gli::format formatId = gli::format::FORMAT_UNDEFINED;
	std::string formatStr;
	int width{}, height{};
	int layerCount{};
	int mipCount{};
};

struct Texture_c
{
	Texture_c();

	bool Allocate(gli::format format, int width, int height, int layerCount, int mipCount);
	bool Allocate(std::string_view format, int width, int height, int layerCount, int mipCount);
	bool Load(const char* fileName);
	bool Save(const char* fileName);
	TextureInfo_s Info() const;
	bool IsValid() const;

	//bool SetLayer(Texture_c& srcTex, int targetLayer);
	//bool CopyImage(Texture_c& srcTex, int targetX, int targetY);
	//bool Transcode(gli::format format);
	//bool GenerateMipmaps();

	bool StackTextures(std::vector<Texture_c> textures);


private:
	gli::texture2d_array tex;
};
