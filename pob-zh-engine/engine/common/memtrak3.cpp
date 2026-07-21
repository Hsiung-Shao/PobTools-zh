// MemTrak Memory Tracker v3
// (c) David Gowor, 2005
//

#if 0

#include <windows.h>
#include <stdio.h>

bool _memTrak_suppressReport;
char _memTrak_reportName[512];

// =======
// Classes
// =======

#define TRAKPOW 16
#define TRAKSZ (1<<TRAKPOW)
#define TRAKMASK (TRAKSZ-1)
#define FREEDSZ 65536
#define FREEDNUM 1024
#define FREEDMASK (FREEDNUM-1)

struct _trakEntry_s {
	size_t		size;
	const char	*file;
	int			line;
};

struct _freedArray_s {
	volatile int num;
	volatile int numRead;
	unsigned int* a;
};

class _memTrak_c {
public:	
	_memTrak_c();
	~_memTrak_c();

	void*	New(size_t, const char *, int);
	void*	Realloc(void*, size_t, const char *, int);
	void	Delete(void *);

	void	MemReport(const char*);
	size_t	GetAllocSize();
	int		GetAllocCount();
	size_t	GetOverhead();

private:
	_trakEntry_s*	trak[1024];
	volatile int	trakCount;
	volatile int	numTrak;

	_freedArray_s	freed[FREEDNUM];
	volatile int	numFreedFree;
	volatile int	freedIn;
	volatile int	freedOut;

	size_t	memSize;
};

// =============
// MemTrak Class
// =============

_memTrak_c::_memTrak_c()
{
	memset(trak, 0, sizeof(trak));
	trak[0] = (_trakEntry_s*)malloc(sizeof(_trakEntry_s) * TRAKSZ);
	trakCount = 1;
	numTrak = 0;
	memset(freed, 0, sizeof(freed));
	freed[0].a = (unsigned int*)malloc(sizeof(unsigned int) * FREEDSZ);
	freed[0].numRead = FREEDSZ;
	freed[1].a = (unsigned int*)malloc(sizeof(unsigned int) * FREEDSZ);
	numFreedFree = 0;
	freedIn = 1;
	freedOut = 0;
	memSize = 0;
	_memTrak_suppressReport = false;
	strcpy_s(_memTrak_reportName, 512, "_memtrak.log");
}

_memTrak_c::~_memTrak_c()
{
	if (_memTrak_suppressReport) {
		for (int t = 0; t < trakCount; t++) {
			free(trak[t]);
		}
		for (int f = 0; f < FREEDNUM; f++) {
			free(freed[f].a);
		}
		return;
	}

	FILE *outf = NULL;

	int lknum = 0;
	size_t lksize = 0;
	for (int i = 0; i < numTrak; i++) {
		_trakEntry_s* t = &trak[i>>TRAKPOW][i&TRAKMASK];
		if (t->file) {
			lknum++;
			lksize+= t->size;
			if ( !outf ) {
				outf = fopen(_memTrak_reportName, "w");
				if ( !outf ) return;
			}
			fprintf(outf, "%s(%d): %d bytes\n", t->file, t->line, t->size);
		}
	}

	if (outf) {
		fclose(outf);
	}

	for (int t = 0; t < trakCount; t++) {
		free(trak[t]);
	}
	for (int f = 0; f < FREEDNUM; f++) {
		free(freed[f].a);
	}

	if (lknum) {
		char msg[256];
		sprintf_s(msg, 256, "%d memory leaks have been detected.\r\nTotal size: %d bytes", lknum, lksize);
		MessageBox(NULL, msg, "MemTrak3", MB_ICONERROR|MB_SYSTEMMODAL);
		char wd[260];
		GetCurrentDirectory(260, wd);
		ShellExecute(NULL, "open", "notepad", _memTrak_reportName, wd, SW_SHOW);
	}
}

void _memTrak_c::MemReport(const char* fileName)
{
	FILE* f = fopen(fileName, "w");

	int num = 0;
	size_t size = 0;
	for (int i = 0; i < numTrak; i++) {
		_trakEntry_s* t = &trak[i>>TRAKPOW][i&TRAKMASK];
		if (t->file) {
			num++;
			size+= t->size;
			fprintf(f, "%s(%d): %d bytes\n", t->file, t->line, t->size);
		}
	}

	fprintf(f, "%d bytes in %d allocations.\n", size, num);
	fclose(f);
}

size_t _memTrak_c::GetAllocSize()
{
	return memSize;
}

int _memTrak_c::GetAllocCount()
{
	return numTrak - freed[freedIn].num - freed[freedOut].num - numFreedFree * FREEDSZ;
}

size_t _memTrak_c::GetOverhead()
{
	return sizeof(_memTrak_c)									// Tracker object
		 + sizeof(_trakEntry_s) * trakCount * TRAKSZ			// Main arrays
		 + sizeof(unsigned int) * (numFreedFree + 2) * FREEDSZ	// Freed arrays
		 + sizeof(unsigned int) * GetAllocCount();				// Tracking numbers
}

inline void* _memTrak_c::New(size_t size, const char *file, int line)
{
	char* ptr = (char*)malloc(size + 4);
	if ( !ptr ) {
		return NULL;
	}

	unsigned int i;
	int id = -1;
	if (numFreedFree) {
		int out = freedOut;
		id = InterlockedDecrement((LONG*)&freed[out].num);
		if (id >= 0) {
			i = freed[out].a[id];
			InterlockedIncrement((LONG*)&freed[out].numRead);
		} else if (id == -1) {
			InterlockedDecrement((LONG*)&numFreedFree);
			int nextOut = (freedOut + 1) & FREEDMASK;
			freed[nextOut].num = FREEDSZ;
			freed[nextOut].numRead = 0;
			int oldOut = freedOut;
			freedOut = nextOut;
			while (freed[oldOut].numRead < FREEDSZ);
			free(freed[oldOut].a);
			freed[oldOut].a = NULL;
		}
	}
	if (id < 0) {
		i = InterlockedIncrement((LONG*)&numTrak);
		if (i == trakCount<<TRAKPOW) {
			trak[trakCount] = (_trakEntry_s*)malloc(sizeof(_trakEntry_s) * TRAKSZ);
			trakCount++;
		} else {
			while ((int)i > trakCount<<TRAKPOW) Sleep(0);
		}
		i--;
	}
	InterlockedExchangeAdd(&memSize, size);

	trak[i>>TRAKPOW][i&TRAKMASK].size = size;
	trak[i>>TRAKPOW][i&TRAKMASK].file = file;
	trak[i>>TRAKPOW][i&TRAKMASK].line = line;
	
	*(unsigned int*)ptr = i;
	return ptr + 4;
}

inline void* _memTrak_c::Realloc(void* ptr, size_t size, const char* file, int line)
{
	if ( !ptr ) {
		return New(size, file, line);
	}
	if ( !size ) {
		Delete(ptr);
		return NULL;
	}

	char* realptr = (char*)ptr - 4;
	unsigned int i = *(unsigned int*)realptr;

	InterlockedExchangeSubtract(&memSize, trak[i>>TRAKPOW][i&TRAKMASK].size);
	InterlockedExchangeAdd(&memSize, size);
	trak[i>>TRAKPOW][i&TRAKMASK].size = size;
	trak[i>>TRAKPOW][i&TRAKMASK].file = file;
	trak[i>>TRAKPOW][i&TRAKMASK].line = line;

	return (char*)realloc(realptr, size + 4) + 4;
}

inline void _memTrak_c::Delete(void *ptr)
{
	if (ptr) {
		char* realptr = (char*)ptr - 4;
		int i = *(unsigned int*)realptr;

		trak[i>>TRAKPOW][i&TRAKMASK].file = NULL;
		InterlockedExchangeSubtract(&memSize, trak[i>>TRAKPOW][i&TRAKMASK].size);

again:
		int in = freedIn;
		int id = InterlockedIncrement((LONG*)&freed[in].num);
		if (in != freedIn) {
			goto again;
		}
		if (id <= FREEDSZ) {
			freed[in].a[id - 1] = i;
			InterlockedIncrement((LONG*)&freed[in].numRead);
			if (id == FREEDSZ) {
				while (freed[freedIn].numRead < FREEDSZ);
				freed[freedIn].num = FREEDSZ;
				int newIn = (freedIn + 1) & FREEDMASK;
				freed[newIn].a = (unsigned int*)malloc(sizeof(unsigned int) * FREEDSZ);
				freed[newIn].num = 0;
				freed[newIn].numRead = 0;
				freedIn = newIn;
				InterlockedIncrement((LONG*)&numFreedFree);
			}
		} else {
			while (in == freedIn) Sleep(0);
			goto again;
		}

		free(realptr);
	}
}

// =========
// Operators
// =========

static _memTrak_c _memTrak;

void* operator new(size_t size, const char *file, int line)
{
	return _memTrak.New(size, file, line);
}

void* operator new[](size_t size, const char *file, int line)
{
	return _memTrak.New(size, file, line);
}

void operator delete(void *ptr, const char *file, int line)
{
	_memTrak.Delete(ptr);
}

void operator delete[](void *ptr, const char *file, int line)
{
	_memTrak.Delete(ptr);
}

void operator delete(void *ptr)
{
	_memTrak.Delete(ptr);
}

void operator delete[](void *ptr)
{
	_memTrak.Delete(ptr);
}

void* _memTrak_realloc(void* ptr, size_t size, const char* file, int line)
{
	return _memTrak.Realloc(ptr, size, file, line);
}

void* _memTrak_reallocNoTrack(void* ptr, size_t size)
{
	return realloc(ptr, size);
}

void _memTrak_memReport(const char* fileName)
{
	_memTrak.MemReport(fileName);
}

size_t _memTrak_getAllocSize()
{
	return _memTrak.GetAllocSize();
}

int _memTrak_getAllocCount()
{
	return _memTrak.GetAllocCount();
}

size_t _memTrak_getOverhead()
{
	return _memTrak.GetOverhead();
}

#endif
