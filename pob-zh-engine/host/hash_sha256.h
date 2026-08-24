// SHA-256 over an in-memory buffer via Windows CNG (BCrypt).
// Used by the app updater to verify downloaded release assets against the
// sha256 digest reported by the GitHub API before anything touches disk, and by
// sig_verify.cpp as the message hash under the release signature.
#pragma once

#include <string>

// Fills the 32-byte digest. Returns false on any BCrypt failure (caller should
// treat the download as unverified).
bool Sha256Raw(const void* data, size_t size, unsigned char out[32]);

// Fills *hexLower with the 64-char lowercase hex digest. Returns false on any
// BCrypt failure (caller should treat the download as unverified).
bool Sha256Hex(const void* data, size_t size, std::string* hexLower);
