#include "image_tex.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

#include <webp/decode.h>

#include <GLES2/gl2.h>
#include <cstdlib>
#include <cstring>

unsigned char* DecodeImageRGBA(const unsigned char* data, int len, int* w, int* h)
{
	// GGG's atlastree-export ships webp sheets from 3.30 on; stb has no webp.
	if (len >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4)) {
		unsigned char* px = WebPDecodeRGBA(data, (size_t)len, w, h);
		if (!px) return nullptr;
		// Copied so FreeDecoded can stay a plain free() for both decoders.
		size_t n = (size_t)*w * (size_t)*h * 4;
		unsigned char* out = (unsigned char*)malloc(n);
		if (out) memcpy(out, px, n);
		WebPFree(px);
		return out;
	}
	int comp = 0;
	return stbi_load_from_memory(data, len, w, h, &comp, 4); // force RGBA
}

void FreeDecoded(unsigned char* pixels)
{
	if (pixels) stbi_image_free(pixels);
}

unsigned CreateTextureRGBA(const unsigned char* rgba, int w, int h)
{
	if (!rgba || w <= 0 || h <= 0) return 0;
	GLuint tex = 0;
	glGenTextures(1, &tex);
	if (!tex) return 0;
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	return tex;
}

void DeleteTexture(unsigned tex)
{
	if (tex) { GLuint t = (GLuint)tex; glDeleteTextures(1, &t); }
}
