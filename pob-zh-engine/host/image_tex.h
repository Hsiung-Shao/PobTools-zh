// PNG/JPG decode + GL texture helpers for the host (stb_image, already vendored
// in dep/stb). Decoding is thread-safe (pure CPU); CreateTextureRGBA / DeleteTexture
// must run on the thread that owns the GL context.
#pragma once

// Decode an image buffer to RGBA8. Returns a buffer to free with FreeDecoded, or
// nullptr on failure. Safe to call off the GL thread.
unsigned char* DecodeImageRGBA(const unsigned char* data, int len, int* w, int* h);
void FreeDecoded(unsigned char* pixels);

// Upload RGBA8 pixels as a GL texture (GL thread only). Returns a GLuint (0 on failure).
unsigned CreateTextureRGBA(const unsigned char* rgba, int w, int h);
void DeleteTexture(unsigned tex);
