#include "gl_shot.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <GLES2/gl2.h>

#include <vector>

// POBTOOLS_LAUNCHER_SHOT: the back buffer as a 32-bit top-down BMP. Called
// between RenderDrawData and SwapBuffers, so it reads the frame just drawn.
void WriteFramebufferBmp(const std::wstring& path, int w, int h)
{
	if (w <= 0 || h <= 0) return;
	std::vector<unsigned char> px((size_t)w * h * 4);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	std::vector<unsigned char> out((size_t)w * h * 4);
	for (int y = 0; y < h; y++) {
		const unsigned char* src = &px[(size_t)(h - 1 - y) * w * 4];   // GL rows are bottom-up
		unsigned char* dst = &out[(size_t)y * w * 4];
		for (int x = 0; x < w; x++) {
			dst[x * 4 + 0] = src[x * 4 + 2];
			dst[x * 4 + 1] = src[x * 4 + 1];
			dst[x * 4 + 2] = src[x * 4 + 0];
			dst[x * 4 + 3] = 255;
		}
	}
	BITMAPFILEHEADER fh{};
	BITMAPINFOHEADER ih{};
	ih.biSize = sizeof(ih);
	ih.biWidth = w;
	ih.biHeight = -h;   // top-down
	ih.biPlanes = 1;
	ih.biBitCount = 32;
	ih.biCompression = BI_RGB;
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof(fh) + sizeof(ih);
	fh.bfSize = fh.bfOffBits + (DWORD)out.size();
	HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return;
	DWORD wr = 0;
	WriteFile(f, &fh, sizeof(fh), &wr, nullptr);
	WriteFile(f, &ih, sizeof(ih), &wr, nullptr);
	WriteFile(f, out.data(), (DWORD)out.size(), &wr, nullptr);
	CloseHandle(f);
}

std::wstring ShotEnv(const wchar_t* name)
{
	wchar_t buf[MAX_PATH] = L"";
	const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
	return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring();
}
