// Optional GitHub API authentication, shared by the launcher and CLI workers.
#pragma once

#include <string>

namespace GitHubToken {

// Trims pasted whitespace and rejects characters that cannot occur in a PAT.
// Empty is valid and means anonymous requests. Never includes input in errors.
bool Normalize(const std::string& input, std::string& token);

// <exeDir>PobTools\\github_token.bin, encrypted for the current Windows user.
// Save is atomic; empty deletes the saved credential. Failure keeps the old one.
std::string Load(const std::wstring& exeDir, std::string* err = nullptr);
bool Save(const std::wstring& exeDir, const std::string& input, std::string* err = nullptr);

// Thread-safe: settings changes affect the next request, even on an existing client.
bool Set(const std::string& input);
bool IsApiHost(const std::wstring& host);
std::wstring RequestHeaders(const std::wstring& host);

// Only call for api.github.com. Raw response bodies are never returned or logged.
std::string ApiError(int status, const std::string& remaining,
                     const std::string& retryAfter, const std::string& body);
extern const char* const ErrorGlyphSeed;

} // namespace GitHubToken
