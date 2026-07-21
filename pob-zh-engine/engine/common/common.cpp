// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Module: Common
//

#include "common.h"

// ===================
// Argument List Class
// ===================

args_c::args_c(const char* in)
{
	argc = 0;
	memset(argv, 0, sizeof(char*) * 256);

	argBuf = AllocString(in);
	char* ptr = argBuf;
	while (*ptr) {
		if (isspace(*ptr)) {
			ptr++;
		} else if (*ptr == '"') {
			argv[argc++] = ++ptr;
			while (*ptr && *ptr != '"') {
				ptr++;
			}
			if (*ptr) *(ptr++) = 0;
		} else {
			argv[argc++] = ptr++;
			while (*ptr && !isspace(*ptr)) {
				ptr++;
			}
			if (*ptr) *(ptr++) = 0;
		}
	}
}

args_c::~args_c()
{
	FreeString(argBuf);
}

const char* args_c::operator[](int i)
{
	if (i >= 0 && i < argc) {
		return argv[i];
	} else {
		return "";
	}
}

// =================
// Text Buffer Class
// =================

textBuffer_c::textBuffer_c()
{
	buf = NULL;
	Init();
}

textBuffer_c::~textBuffer_c()
{
	Free();
}

void textBuffer_c::Alloc(int sz)
{
	// Allocate, set length and position, and null-teriminate.
	Free();
	buf = new char[sz+1];
	caret = len = sz;
	buf[len] = 0;
}

void textBuffer_c::Init()
{
	// Initialise to zero length
	Alloc(0);
}

void textBuffer_c::Free()
{
	// Delete buffer memory
	delete buf;
	buf = NULL;
}

textBuffer_c &textBuffer_c::operator=(const char* r)
{
	// Reallocate buffer and copy the string
	Alloc((int)strlen(r));
	strcpy(buf, r);
	return *this;
}

void textBuffer_c::IncSize()
{
	len++;
	char* tmp = new char[len+1];
	memcpy(tmp, buf, len);
	tmp[len] = 0;
	delete buf;
	buf = tmp;	
}

void textBuffer_c::DecSize()
{
	len--;
	char* tmp = new char[len+1];
	memcpy(tmp, buf, len);
	tmp[len] = 0;
	delete buf;
	buf = tmp;
}

bool textBuffer_c::KeyEvent(int key, int type)
{
	if (type == KE_KEYUP) {
		return false;
	}

	int escLen;
	switch (key) {
	// Left/Right: move cursor
	case KEY_LEFT:
		if (caret > 0) {
			caret--;
			if (caret > 2 && (escLen = IsColorEscape(&buf[caret-1]))) {
				caret-= escLen;
			}
		}
		return true;
	case KEY_RIGHT:
		if (caret < len) {
			caret++;
			if (len - caret > 2 && (escLen = IsColorEscape(&buf[caret]))) {
				caret+= escLen;
			}
		}
		return true;
	// Home/end: seek cursor to start/end
	case KEY_HOME:
		caret = 0;
		return true;
	case KEY_END:
		caret = len;
		return true;
	// Backspace: delete character to left of cursor, shift remaining buffer
	case KEY_BACK:
		if (len && caret) {
			for (int c = caret - 1; c <= len; c++) {
				buf[c] = buf[c + 1];
			}
			if (caret > 0) caret--;
			DecSize();
		}
		return true;
	// Delete: delete character above cursor, shift remaining buffer
	case KEY_DELETE:
		if (len && caret < len) {
			for (int c = caret; c <= len; c++) {
				buf[c] = buf[c + 1];
			}
			DecSize();
		}
		return true;
	default:
		if (type == KE_CHAR && key >= 32) {
			for (int c = len; c >= caret; c--) {
				buf[c] = buf[c - 1];
			}
			buf[caret++] = key;
			IncSize();
			return true;
		}
		break;
	}
	return false;
}

// ==============
// Color Escaping
// ==============

// Color escape table
static const float* colorEscape[10] = { 
	colorBlack,		// ^0
	colorRed,		// ^1
	colorGreen,		// ^2
	colorBlue,		// ^3
	colorYellow,	// ^4
	colorPurple,	// ^5
	colorAqua,		// ^6
	colorWhite,		// ^7
	colorGray,		// ^8
	colorDarkGray	// ^9
};

int IsColorEscape(const char* str)
{
	if (str[0] != '^') {
		return 0;
	}
	if (isdigit(str[1])) {
		return 2;
	}
	else if (str[1] == 'x' || str[1] == 'X') {
		for (int c = 0; c < 6; c++) {
			if (!isxdigit(str[c + 2])) {
				return 0;
			}
		}
		return 8;
	}
	return 0;
}

int IsColorEscape(std::u32string_view str)
{
	if (str.size() < 2 || str[0] != '^') {
		return 0;
	}

	auto discrim = str[1];

	// Check for indexed colour escape like ^7.
	// Avoid using isdigit as we only accept arabic numerals.
	if (discrim >= U'0' && discrim <= U'9') {
		return 2;
	}

	// Check for direct colour escape like ^x123ABC.
	if (str.size() >= 8 && (discrim == 'x' || discrim == 'X')) {
		for (int c = 0; c < 6; c++) {
			auto ch = str[c + 2];
			bool const isHexDigit = (ch >= U'0' && ch <= U'9') || (ch >= U'A' && ch <= U'F') || (ch >= U'a' && ch <= U'f');
			if (!isHexDigit) {
				return 0;
			}
		}
		return 8;
	}

	// Fallthrough indicates no recognized colour code.
	return 0;
}

void ReadColorEscape(const char* str, col3_t out)
{
	int len = IsColorEscape(str);
	switch (len) {
	case 2:
		VectorCopy(colorEscape[str[1] - '0'], out);
		break;
	case 8:
	{
		int xr, xg, xb;
		sscanf(str + 2, "%2x%2x%2x", &xr, &xg, &xb);
		out[0] = xr / 255.0f;
		out[1] = xg / 255.0f;
		out[2] = xb / 255.0f;
	}
	break;
	}
}

std::u32string_view ReadColorEscape(std::u32string_view str, col3_t out)
{
	int len = IsColorEscape(str);
	switch (len) {
	case 2:
		VectorCopy(colorEscape[str[1] - U'0'], out);
		break;
	case 8:
		{
			int xr, xg, xb;
			char buf[7]{};
			for (size_t i = 0; i < 6; ++i) {
				buf[i] = (char)str[i + 2];
			}
			sscanf(buf, "%2x%2x%2x", &xr, &xg, &xb);
			out[0] = xr / 255.0f;
			out[1] = xg / 255.0f;
			out[2] = xb / 255.0f;
		}
		break;
	}
	return str.substr(len);
}

// ================
// String Functions
// ================

// MemTrak hack
#ifdef _MEMTRAK_H
#undef new
#else
#define new(f,l) new
#endif

char* _AllocString(const char* str, const char* file, int line)
{
	if (str == NULL) {
		return NULL;
	}

	size_t aslen = strlen(str) + 1;
	char* al = new(file, line) char[aslen];
	strcpy(al, str);
	return al;
}

char* _AllocStringLen(size_t len, const char* file, int line)
{
	char* al = new(file, line) char[len+1];
	al[len] = 0;
	return al;
}

void FreeString(const char* str)
{
	if (str) delete[] str;
}

dword StringHash(const char* str, int mask)
{
	size_t len = strlen(str);
	dword hash = 0;
	for (size_t i = 0; i < len; i++) {
		hash+= (str[i] * 4999) ^ (((dword)i + 17) * 2003);
	}
	return hash & mask;
}

dword StringHash(std::string_view str, int mask)
{
	size_t len = str.length();
	dword hash = 0;
	for (size_t i = 0; i < len; i++) {
		hash += (str[i] * 4999) ^ (((dword)i + 17) * 2003);
	}
	return hash & mask;
}

#ifdef _WIN32
#include <Windows.h>

static wchar_t* WidenCodepageString(const char* str, UINT codepage)
{
	if (!str) {
		return nullptr;
	}
	// Early-out if empty, avoids ambigious error return from MBTWC.
	if (!*str) {
		wchar_t* wstr = new wchar_t[1];
		*wstr = L'\0';
		return wstr;
	}
	DWORD cb = (DWORD)strlen(str);
	int cch = MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, str, cb, nullptr, 0);
	if (cch == 0) {
		// Invalid string or other error.
		return nullptr;
	}
	wchar_t* wstr = new wchar_t[cch + 1]; // sized MBTWC doesn't include terminator.
	MultiByteToWideChar(codepage, 0, str, cb, wstr, cch);
	wstr[cch] = '\0';
	return wstr;
}

wchar_t* WidenANSIString(const char* str)
{
	return WidenCodepageString(str, CP_ACP);
}

wchar_t* WidenOEMString(const char* str)
{
	return WidenCodepageString(str, CP_OEMCP);
}

wchar_t* WidenUTF8String(const char* str)
{
	return WidenCodepageString(str, CP_UTF8);
}

char* NarrowCodepageString(const wchar_t* str, UINT codepage)
{
	if (!str) {
		return nullptr;
	}
	if (!*str) {
		char* nstr = new char[1];
		*nstr = '\0';
		return nstr;
	}
	DWORD cch = (DWORD)wcslen(str);
	int cb = WideCharToMultiByte(codepage, 0, str, cch, nullptr, 0, nullptr, nullptr);
	if (cb == 0) {
		// Invalid string or other error.
		return nullptr;
	}
	char* nstr = new char[cb + 1];
	WideCharToMultiByte(codepage, 0, str, cch, nstr, cb, nullptr, nullptr);
	nstr[cb] = '\0';
	return nstr;
}

void FreeWideString(wchar_t* str)
{
	if (str) {
		delete[] str;
	}
}

char* NarrowANSIString(const wchar_t* str)
{
	return NarrowCodepageString(str, CP_ACP);
}

char* NarrowOEMString(const wchar_t* str)
{
	return NarrowCodepageString(str, CP_OEMCP);
}

char* NarrowUTF8String(const wchar_t* str)
{
	return NarrowCodepageString(str, CP_UTF8);
}

IndexedUTF32String IndexUTF8ToUTF32(std::string_view input)
{
	IndexedUTF32String ret{};

	size_t byteCount = input.size();
	auto& offsets = ret.sourceCodeUnitOffsets;
	offsets.reserve(byteCount); // conservative reservation
	std::vector<char32_t> codepoints;

	auto bytes = (uint8_t const*)input.data();
	for (size_t byteIdx = 0; byteIdx < byteCount;) {
		uint8_t const* b = bytes + byteIdx;
		size_t left = byteCount - byteIdx;
		offsets.push_back(byteIdx);

		char32_t codepoint{};
		if (*b >> 7 == 0b0) { // 0xxx'xxxx
			codepoint = *b;
			byteIdx += 1;
		}
		else if (left >= 2 &&
			b[0] >> 5 == 0b110 &&
			b[1] >> 6 == 0b10)
		{
			auto p0 = (uint32_t)b[0] & 0b1'1111;
			auto p1 = (uint32_t)b[1] & 0b11'1111;
			codepoint = p0 << 6 | p1;
			byteIdx += 2;
		}
		else if (left >= 3 &&
			b[0] >> 4 == 0b1110 &&
			b[1] >> 6 == 0b10 &&
			b[2] >> 6 == 0b10)
		{
			auto p0 = (uint32_t)b[0] & 0b1111;
			auto p1 = (uint32_t)b[1] & 0b11'1111;
			auto p2 = (uint32_t)b[2] & 0b11'1111;
			codepoint = p0 << 12 | p1 << 6 | p2;
			byteIdx += 3;
		}
		else if (left >= 4 &&
			b[0] >> 3 == 0b11110 &&
			b[1] >> 6 == 0b10 &&
			b[2] >> 6 == 0b10 &&
			b[3] >> 6 == 0b10)
		{
			auto p0 = (uint32_t)b[0] & 0b111;
			auto p1 = (uint32_t)b[1] & 0b11'1111;
			auto p2 = (uint32_t)b[2] & 0b11'1111;
			auto p3 = (uint32_t)b[3] & 0b11'1111;
			codepoint = p0 << 18 | p1 << 12 | p2 << 6 | p3;
			byteIdx += 4;
		}
		else {
			codepoint = 0xFFFDu;
			byteIdx += 1;
		}
		codepoints.push_back(codepoint);
	}

	ret.text = std::u32string(codepoints.begin(), codepoints.end());
	return ret;
}

#endif
