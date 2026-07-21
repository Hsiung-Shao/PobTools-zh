// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Streams Header
//

#include <filesystem>
#include <string>
#include <string_view>

// ==========
// Base Class
// ==========

class ioStream_c {
public:
	virtual size_t GetLen() = 0;
	virtual size_t GetPos() = 0;
	virtual bool Seek(size_t pos, int mode) = 0;
	virtual bool Read(void* out, size_t len)
	{
		return true;
	}
	template <class T>
	bool TRead(T &out)
	{
		return Read(&out, sizeof(T));
	}
	virtual bool Write(const void* in, size_t len)
	{
		return true;
	}
	template <class T>
	bool TWrite(T &in)
	{
		return Write(&in, sizeof(T));
	}
};

// =====================
// Memory Buffer Streams
// =====================

class memInputStream_c: public ioStream_c {
public:
	memInputStream_c();
	memInputStream_c(ioStream_c* in);
	memInputStream_c(byte* useMem, size_t useMemSize);
	~memInputStream_c();
	size_t GetLen();
	size_t GetPos();
	bool Seek(size_t pos, int mode);
	bool Read(void* out, size_t len);
	bool MemInput(ioStream_c* in);
	void MemCopy(byte* in, size_t len);
	void MemUse(byte* in, size_t len);
	void MemFree();
private:
	byte* mem;
	size_t memLen;
	size_t memPos;
};

class memOutputStream_c: public ioStream_c {
public:
	memOutputStream_c(size_t initSize = 16);
	~memOutputStream_c();
	size_t GetLen();
	size_t GetPos();
	bool Seek(size_t pos, int mode);
	bool Write(const void* in, size_t len);
	byte* MemGet();
	bool MemOutput(ioStream_c* out);
	void MemReset();
private:
	byte* mem;
	size_t memLen;
	size_t memPos;
	size_t memSize;
};

// ==================
// Stdio File Streams
// ==================

class fileStreamBase_c: public ioStream_c {
public:
	fileStreamBase_c();
	~fileStreamBase_c();
	size_t GetLen();
	size_t GetPos();
	bool Seek(size_t pos, int mode);
	void FileClose();
protected:
	FILE* file;
};

class fileInputStream_c: public fileStreamBase_c {
public:
	bool Read(void* out, size_t len);

	// Force compile error on narrow strings to favour `std::filesystem::path`.
	// These are unfortunately necessary as the path constructor is eager to
	// interpret narrow strings as the ACP codepage. Prefer using
	// `std::filesystem::u8path` (C++17) or `std::u8string` (since C++20) in
	// the calls to these functions.
	bool FileOpen(char const*, bool) = delete;
	bool FileOpen(std::string const&, bool) = delete;
	bool FileOpen(std::string_view*, bool) = delete;

	bool FileOpen(std::filesystem::path const& fileName, bool binary);
};

class fileOutputStream_c: public fileStreamBase_c {
public:
	bool Write(const void* in, size_t len);

	// Force compile error like in `fileInputStream_c` on non-path types.
	bool FileOpen(char const*, bool) = delete;
	bool FileOpen(std::string const&, bool) = delete;
	bool FileOpen(std::string_view*, bool) = delete;

	bool FileOpen(std::filesystem::path const& fileName, bool binary);
	void FilePrintf(const char* fmt, ...);
	void FileFlush();
};

