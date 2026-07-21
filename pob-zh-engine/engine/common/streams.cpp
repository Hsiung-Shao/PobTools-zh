// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Module: Streams
//

#include "common.h"

// ============
// Memory Input
// ============

memInputStream_c::memInputStream_c()
{
	mem = NULL;
	memLen = memPos = 0;
}

memInputStream_c::memInputStream_c(ioStream_c* in)
{
	mem = NULL;
	MemInput(in);
}

memInputStream_c::memInputStream_c(byte* useMem, size_t useMemSize)
{
	mem = NULL;
	MemUse(useMem, useMemSize);
}

memInputStream_c::~memInputStream_c()
{
	MemFree();
}

size_t memInputStream_c::GetLen()
{
	return memLen;
}

size_t memInputStream_c::GetPos()
{
	return memPos;
}

bool memInputStream_c::Seek(size_t pos, int mode)
{
	size_t newPos;
	switch (mode) {
	case SEEK_SET:
		newPos = pos;
		break;
	case SEEK_CUR:
		newPos = memPos + pos;
		break;
	case SEEK_END:
		newPos = memLen - pos;
		break;
	default:
		return true;
	}
	if (newPos >= 0 && newPos < memLen) {
		memPos = newPos;
		return false;
	} else {
		return true;
	}
}

bool memInputStream_c::Read(void* out, size_t len)
{
	if (len > memLen - memPos) {
		return true;
	}
	memcpy(out, mem + memPos, len);
	memPos+= len;
	return false;
}

bool memInputStream_c::MemInput(ioStream_c* in)
{
	MemFree();
	memLen = in->GetLen();
	if (memLen == 0) {
		return true;
	}
	mem = new byte[in->GetLen()];
	if (in->Seek(0, SEEK_SET) || in->Read(mem, memLen)) {
		MemFree();
		return true;
	}
	return false;
}

void memInputStream_c::MemCopy(byte* in, size_t len)
{
	MemFree();
	if (len) {
		mem = new byte[len];
		memLen = len;
		memcpy(mem, in, len);
	}
}

void memInputStream_c::MemUse(byte* in, size_t len)
{
	MemFree();
	mem = in;
	memLen = len;
}

void memInputStream_c::MemFree()
{
	delete mem;
	mem = NULL;
	memLen = memPos = 0;
}

// =============
// Memory Output
// =============

memOutputStream_c::memOutputStream_c(size_t initSize)
{
	memLen = 0;
	memSize = initSize;
	memPos = 0;
	mem = new byte[memSize];
}

memOutputStream_c::~memOutputStream_c()
{
	delete mem;
}

size_t memOutputStream_c::GetLen()
{
	return memLen;
}

size_t memOutputStream_c::GetPos()
{
	return memPos;
}

bool memOutputStream_c::Seek(size_t pos, int mode)
{
	size_t newPos;
	switch (mode) {
	case SEEK_SET:
		newPos = pos;
		break;
	case SEEK_CUR:
		newPos = memPos + pos;
		break;
	case SEEK_END:
		newPos = memSize - pos;
		break;
	default:
		return true;
	}
	if (newPos >= 0) {
		memPos = newPos;
		return false;
	} else {
		return true;
	}
}

bool memOutputStream_c::Write(const void* in, size_t len)
{
	while (len > memSize - memPos) {
		memSize<<= 1;
		trealloc(mem, memSize);
	}
	memcpy(mem + memPos, in, len);
	memPos+= len;
	if (memPos > memLen) {
		memLen = memPos;
	}
	return false;
}

byte* memOutputStream_c::MemGet()
{
	return mem;
}

bool memOutputStream_c::MemOutput(ioStream_c* out)
{
	return out->Write(mem, memLen);
}

void memOutputStream_c::MemReset()
{
	memLen = memPos = 0;
}

// ===============
// Stdio File Base
// ===============

fileStreamBase_c::fileStreamBase_c()
{
	file = NULL;
}

fileStreamBase_c::~fileStreamBase_c()
{
	FileClose();
}

size_t fileStreamBase_c::GetLen()
{
	if (!file) {
		return 0;
	}
	auto pos = ftell(file);
	fseek(file, 0, SEEK_END);
	auto size = ftell(file);
	fseek(file, pos, SEEK_SET);
	return size;
}

size_t fileStreamBase_c::GetPos()
{
	return file? ftell(file) : 0;
}

bool fileStreamBase_c::Seek(size_t pos, int mode)
{
	if ( !file ) {
		return true;
	}
	return fseek(file, (long)pos, mode) != 0;
}

void fileStreamBase_c::FileClose()
{
	if (file) {
		fclose(file);
		file = NULL;
	}
}

// ================
// Stdio File Input
// ================

bool fileInputStream_c::Read(void* out, size_t len)
{
	if ( !file ) {
		return true;
	}
	return fread(out, len, 1, file) < 1;
}

bool fileInputStream_c::FileOpen(std::filesystem::path const& fileName, bool binary)
{
	FileClose();
#ifdef _WIN32
	file = _wfopen(fileName.c_str(), binary ? L"rb" : L"r");
#else
	file = fopen(fileName.c_str(), binary ? "rb" : "r");
#endif
	if ( !file ) {
		return true;
	}
	return false;
}

// =================
// Stdio File Output
// =================

bool fileOutputStream_c::Write(const void* in, size_t len)
{
	if ( !file ) {
		return true;
	}
	return fwrite(in, len, 1, file) < 1;
}

bool fileOutputStream_c::FileOpen(std::filesystem::path const& fileName, bool binary)
{
	FileClose();
#ifdef _WIN32
	file = _wfopen(fileName.c_str(), binary ? L"wb" : L"w");
#else
	file = fopen(fileName.c_str(), binary ? "wb" : "w");
#endif
	if ( !file ) {
		return true;
	}
	return false;
}

void fileOutputStream_c::FilePrintf(const char* fmt, ...)
{
	if (file) {
		va_list va;
		va_start(va, fmt);
		vfprintf(file, fmt, va);
		va_end(va);
	}
}

void fileOutputStream_c::FileFlush()
{
	if (file) {
		fflush(file);
	}
}