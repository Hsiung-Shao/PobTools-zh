#if 0

// MemTrak Memory Tracker v3
// (c) David Gowor, 2005
//

#ifndef _MEMTRAK_H
#define _MEMTRAK_H

/** MemTrak operators **/

void* operator new(size_t size, const char *file, int line);
void* operator new[](size_t size, const char *file, int line);
void operator delete(void *ptr, const char *file, int line);
void operator delete[](void *ptr, const char *file, int line);
void operator delete(void *ptr);
void operator delete[](void *ptr);

void* _memTrak_realloc(void* ptr, size_t size, const char* file, int line);
void* _memTrak_reallocNoTrack(void* ptr, size_t size);

extern bool _memTrak_suppressReport;
extern char _memTrak_reportName[512];

void _memTrak_memReport(const char* fileName);
size_t _memTrak_getAllocSize();
int _memTrak_getAllocCount();
size_t _memTrak_getOverhead();

/** Macro hack **/

#define new new(__FILE__, __LINE__)
#define realloc(p, s) _memTrak_realloc(p, s, __FILE__, __LINE__)

#endif

#endif
