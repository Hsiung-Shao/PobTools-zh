// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Common header file, included by all modules
//

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#if _WIN32
#include <process.h>
#include <direct.h>
#include <io.h>
#else
#include <strings.h>
#include <unistd.h>
#endif
#include <math.h>
#include <errno.h>
#include <stdbool.h>

#ifdef _DEBUG
#include "common/memtrak3.h"
#endif

#include <string>
#include <string_view>
#include <vector>

// =======
// Classes
// =======

enum st_e { S, T };
enum xyzw_e { X, Y, Z, W };
enum abcd_e { A, B, C, D, E, F, G, H, I, J };
enum rtp_e { RHO, THETA, PHI };
enum ypr_e { YAW, PITCH, ROLL }; 

typedef unsigned char byte;
typedef signed char sbyte;
typedef unsigned short word;
typedef unsigned int dword;

typedef float	plane_t[4];	// Plane
typedef float	vec2_t[2];	// Vector X,Y
typedef float	vec3_t[3];	// Vector X,Y,Z
typedef float	vec4_t[4];	// Vector X,Y,Z,W
typedef float	col3_t[3];	// Colour R,G,B
typedef float	col4_t[4];	// Colour R,G,B,A
typedef byte	bcol_t[4];	// Colour R,G,B,A
typedef float	tc_t[2];	// Texture coordinates S,T
typedef float	sph_t[3];	// Spherical point rho, theta, phi
typedef float	scp_t[2];	// Screen point X,Y

struct ref_s { vec3_t pos, ang; };

class args_c {
public:
	int argc;
	char* argv[256];

	args_c(const char* in);
	~args_c();

	const char* operator[](int i);

private:
	char* argBuf;
};

class textBuffer_c {
public:
	textBuffer_c();
	~textBuffer_c();

	char*	buf;		// Buffer
	int		len;		// Buffer text length
	int		caret;		// Cursor position

	void	Init();		// Initialise the buffer
	void	Free();		// Free the buffer
	bool	KeyEvent(int key, int type);// Act on a keypress

	textBuffer_c &operator=(const char* r);

private:
	void	Alloc(int sz);// Allocate the buffer
	void	IncSize();	// Increment buffer size
	void	DecSize();	// Decrement buffer size
};

// =========
// Constants
// =========

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static const char axchr[] = {'X', 'Y', 'Z', 'W'};

static const vec2_t origin2 = {0, 0};
static const vec3_t origin3 = {0, 0, 0};
static const vec4_t origin4 = {0, 0, 0, 1};

static const col4_t colorBlack	= {0.0f, 0.0f, 0.0f, 1.0f};
static const col4_t colorRed	= {1.0f, 0.0f, 0.0f, 1.0f};
static const col4_t colorGreen	= {0.0f, 1.0f, 0.0f, 1.0f};
static const col4_t colorBlue	= {0.0f, 0.0f, 1.0f, 1.0f};
static const col4_t colorYellow	= {1.0f, 1.0f, 0.0f, 1.0f};
static const col4_t colorPurple	= {1.0f, 0.0f, 1.0f, 1.0f};
static const col4_t colorAqua	= {0.0f, 1.0f, 1.0f, 1.0f};
static const col4_t colorWhite	= {1.0f, 1.0f, 1.0f, 1.0f};
static const col4_t colorGray	= {0.7f, 0.7f, 0.7f, 1.0f};
static const col4_t colorDarkGray={0.4f, 0.4f, 0.4f, 1.0f};

// =========
// Templates
// =========

template <class T>
void _trealloc(T* &ptr, size_t count, const char* file, int line)
{
#ifdef _MEMTRAK_H
	ptr = (T*)_memTrak_realloc(ptr, count * sizeof(T), file, line);
#else
	ptr = (T*)realloc(ptr, count * sizeof(T));
#endif
}
#define trealloc(ptr, count) _trealloc(ptr, count, __FILE__, __LINE__)

template <class T>
T endianSwap16(T a)
{
	byte* b = (byte*)&a;
	byte o[2] = {b[1], b[0]};
	return *(T*)o;
}
template <class T>
T endianSwap16(byte* b)
{
	byte o[2] = {b[1], b[0]};
	return *(T*)o;
}
template <class T>
T endianSwap32(T a)
{
	byte* b = (byte*)&a;
	byte o[4] = {b[3], b[2], b[1], b[0]};
	return *(T*)o;
}
template <class T>
T endianSwap32(byte* b)
{
	byte o[4] = {b[3], b[2], b[1], b[0]};
	return *(T*)o;
}
template <class T>
T endianSwap64(T a)
{
	byte* b = (byte*)&a;
	byte o[8] = {b[7], b[6], b[5], b[4], b[3], b[2], b[1], b[0]};
	return *(T*)o;
}
template <class T>
T endianSwap64(byte* b)
{
	byte o[8] = {b[7], b[6], b[5], b[4], b[3], b[2], b[1], b[0]};
	return *(T*)o;
}

template<class T>
float RAD(T a)
{
	return (float)a / 180.0f * M_PI;
}
template<class T>
float DEG(T a)
{
	return (float)a / M_PI * 180.0f;
}

template<class T>
float SINE(T d, int t)
{
	float ang = ((t*(4*(int)d)/1000)%(4*360)) / 4.0f;
	return sin(RAD(ang));
}

template<class A, class B>
bool PointInBox(A* pt, B* tl, B* br)
{
	return (pt[X]>=tl[X]&&pt[X]<=br[X]) && (pt[Y]>=tl[Y]&&pt[Y]<=br[Y]);
}

template<class A, class T>	
void Vector2Copy(A* in, T* out)				
{	
	out[X] = (T)(in[X]);		
	out[Y] = (T)(in[Y]);			
}
template<class A, class B, class T>
void Vector2Add(A* v0, B* v1, T* out)		
{	
	out[X] = (T)(v0[X] + v1[X]);
	out[Y] = (T)(v0[Y] + v1[Y]);	
}
template<class A, class B, class T>
void Vector2Subtract(A* v0, B* v1, T* out)	
{	
	out[X] = (T)(v0[X] - v1[X]);
	out[Y] = (T)(v0[Y] - v1[Y]);	
}
template<class A, class B, class T>
void Vector2Multiply(A* v0, B* v1, T* out)	
{	
	out[X] = (T)(v0[X] * v1[X]);
	out[Y] = (T)(v0[Y] * v1[Y]);	
}
template<class A, class B, class T>
void Vector2Scale(A* in, B sc, T* out)
{
	out[X] = (T)(in[X] * sc);
	out[Y] = (T)(in[Y] * sc);
}

template<class A, class T>	
void VectorCopy(A* in, T* out)				
{	
	out[X] = (T)(in[X]);		
	out[Y] = (T)(in[Y]);			
	out[Z] = (T)(in[Z]);			
}
template<class A, class B, class T>
void VectorAdd(A* v0, B* v1, T* out)		
{	
	out[X] = (T)(v0[X] + v1[X]);
	out[Y] = (T)(v0[Y] + v1[Y]);	
	out[Z] = (T)(v0[Z] + v1[Z]);	
}
template<class A, class B, class T>
void VectorSubtract(A* v0, B* v1, T* out)	
{	
	out[X] = (T)(v0[X] - v1[X]);
	out[Y] = (T)(v0[Y] - v1[Y]);	
	out[Z] = (T)(v0[Z] - v1[Z]);	
}
template<class A, class B, class T>
void VectorMultiply(A* v0, B* v1, T* out)	
{	
	out[X] = (T)(v0[X] * v1[X]);
	out[Y] = (T)(v0[Y] * v1[Y]);	
	out[Z] = (T)(v0[Z] * v1[Z]);	
}
template<class A, class B, class T>
void VectorScale(A* in, B sc, T* out)
{
	out[X] = (T)(in[X] * sc);
	out[Y] = (T)(in[Y] * sc);
	out[Z] = (T)(in[Z] * sc);
}
template<class A, class B, class C, class T>
void VectorMA(A* v0, B sc, C* v1, T* out)
{
	out[X] = (T)(v0[X] + v1[X] * sc);
	out[Y] = (T)(v0[Y] + v1[Y] * sc);
	out[Z] = (T)(v0[Z] + v1[Z] * sc);
}

template<class A, class T>	
void Vector4Copy(A* in, T* out)				
{	
	out[X] = (T)(in[X]);		
	out[Y] = (T)(in[Y]);			
	out[Z] = (T)(in[Z]);		
	out[W] = (T)(in[W]);			
}
template<class A, class B, class T>
void Vector4Add(A* v0, B* v1, T* out)		
{	
	out[X] = (T)(v0[X] + v1[X]);
	out[Y] = (T)(v0[Y] + v1[Y]);	
	out[Z] = (T)(v0[Z] + v1[Z]);	
	out[W] = (T)(v0[W] + v1[W]);	
}
template<class A, class B, class T>
void Vector4Subtract(A* v0, B* v1, T* out)	
{	
	out[X] = (T)(v0[X] - v1[X]);
	out[Y] = (T)(v0[Y] - v1[Y]);	
	out[Z] = (T)(v0[Z] - v1[Z]);
	out[W] = (T)(v0[W] - v1[W]);	
}
template<class A, class B, class T>
void Vector4Multiply(A* v0, B* v1, T* out)	
{	
	out[X] = (T)(v0[X] * v1[X]);
	out[Y] = (T)(v0[Y] * v1[Y]);	
	out[Z] = (T)(v0[Z] * v1[Z]);	
	out[W] = (T)(v0[W] * v1[W]);	
}
template<class A, class B, class T>
void Vector4Scale(A* in, B sc, T* out)
{
	out[X] = (T)(in[X] * sc);
	out[Y] = (T)(in[Y] * sc);
	out[Z] = (T)(in[Z] * sc);
	out[W] = (T)(in[W] * sc);
}

template<class A, class B>
float DotProduct2(A* v0, B* v1)
{
	return v0[X] * v1[X] + v0[Y] * v1[Y];
}
template<class A>
float Vector2Length(A* v)
{
	return (float)sqrt(v[X] * v[X] + v[Y] * v[Y]);
}

template<class A, class B>
float DotProduct(A* v0, B* v1)
{
	return v0[X] * v1[X] + v0[Y] * v1[Y] + v0[Z] * v1[Z];
}
template<class A>
float VectorLength(A* v)
{
	return (float)sqrt(v[X] * v[X] + v[Y] * v[Y] + v[Z] * v[Z]);
}

template<class A, class B>
void CrossProduct(A* v0, A* v1, B* o)
{
	o[X] = (B)(v0[Y]*v1[Z] - v0[Z]*v1[Y]);
	o[Y] = (B)(v0[Z]*v1[X] - v0[X]*v1[Z]);
	o[Z] = (B)(v0[X]*v1[Y] - v0[Y]*v1[X]);
}

template<class A, class T>
void PlaneCopy(A* in, T* out)
{
	out[X] = (T)(in[X]);		
	out[Y] = (T)(in[Y]);			
	out[Z] = (T)(in[Z]);		
	out[D] = (T)(in[D]);			
}

template<class A>
void PlaneNormalise(A* p)
{
	float ivlen = 1.0f / VectorLength(p);
	p[X]*= ivlen;
	p[Y]*= ivlen;
	p[Z]*= ivlen;
	p[D]*= ivlen;
}
template<class A>
void VectorNormalise(A* v)
{
	float ivlen = 1.0f / VectorLength(v);
	v[X]*= ivlen;
	v[Y]*= ivlen;
	v[Z]*= ivlen;
}

template <class A>
void FindPlane(A* p, A* a, A* b, A* c)
{
	A* v0[3], v1[3];
	VectorSubtract(b, a, v0);
	VectorSubtract(c, a, v1);
	CrossProduct(v0, v1, p);
	p[D] = -DotProduct(a, p);
	PlaneNormalise(p);
}
template<class A, class B>
float DistanceToPlane(A* p, B* v)
{
	return p[X] * v[X] + p[Y] * v[Y] + p[Z] * v[Z] + p[D];
}
template<class A>
void NearestPointOnPlane(A* p, A* a, A* o)
{
	A k = DistanceToPlane(p, a);
	VectorMA(a, -k, p, o);
}
template<class A>
void LinePointOnPlane(A* p, A* a, A* b, A* o)
{
	A* v;
	VectorSubtract(b, a, v);
	A k = DistanceToPlane(p, a) / DotProduct(p, v);
	VectorMA(a, -k, v, o);
}

template<class A, class B>
void VectorSphere(A* v, B* s)
{
	s[RHO] = VectorLength(v);
	s[THETA] = (float)atan2(v[X], v[Y]);
	s[PHI] = (float)atan2(Vector2Length(v), v[Z]);
}
template<class A, class B>
void PlaneSphere(A* v, B* s)
{
	s[RHO] = 1;
	s[THETA] = (B)atan2(v[X], v[Y]);
	s[PHI] = (B)atan2(Vector2Length(v), v[Z]);
}
template<class A, class B>
void SphereVector(A* s, B* v)
{
	B sR = s[RHO] * (B)sin(s[PHI]);
	v[X] = sR * (B)cos(s[THETA]);
	v[Y] = sR * (B)sin(s[THETA]);
	v[Z] = s[RHO] * (B)cos(s[PHI]);
}

template <class T> 
void LL_Link(T &f, T &l, T i) 
{
	i->prev = l;
	i->next = NULL;
	if (l) l->next = i;
	if (!f) f = i;
	l = i;
}
template <class T>
T LL_Next(T &i)
{
	T n;
	n = i;
	i = i->next;
	return n;
}
template <class T>
T LL_Prev(T &i)
{
	T n;
	n = i;
	i = i->prev;
	return n;
}
template <class T>
void LL_Unlink(T &f, T &l, T i) 
{
	if (i == f) f = i->next;
	if (i == l) l = i->prev;
	if (i->prev) i->prev->next = i->next;
	if (i->next) i->next->prev = i->prev;
}

template <class T>
T bita(T i, T bit)
{
	return (i & (1 << bit)) >> bit;
}
template <class T>
T bitr(T i, T start, T len)
{
	return (i & (((1<<len)-1) << start)) >> start;
}

template <class T>
T clamp(T &v, T l, T u)
{
	if (v < l) v = l;
	else if (v > u) v = u;
	return v;
}

// ================
// Common Functions
// ================

int		IsColorEscape(char const* str);
int		IsColorEscape(std::u32string_view str);
void	ReadColorEscape(char const* str, col3_t out);
std::u32string_view ReadColorEscape(std::u32string_view str, col3_t out);

char*	_AllocString(const char* str, const char* file, int line);
#define AllocString(s) _AllocString(s, __FILE__, __LINE__)
char*	_AllocStringLen(size_t len, const char* file, int line);
#define AllocStringLen(s) _AllocStringLen(s, __FILE__, __LINE__)
void	FreeString(const char* str);
dword	StringHash(const char* str, int mask);
dword	StringHash(std::string_view str, int mask);

struct IndexedUTF32String {
	std::u32string text;
	std::vector<size_t> sourceCodeUnitOffsets;
};

IndexedUTF32String IndexUTF8ToUTF32(std::string_view str);

#ifdef _WIN32
wchar_t* WidenANSIString(const char* str);
wchar_t* WidenOEMString(const char* str);
wchar_t* WidenUTF8String(const char* str);
void FreeWideString(wchar_t* str);

char* NarrowANSIString(const wchar_t* str);
char* NarrowOEMString(const wchar_t* str);
char* NarrowUTF8String(const wchar_t* str);
#endif

#ifndef _WIN32
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _chdir chdir
#endif

// =======
// Headers
// =======

#include "config.h"

#include "common/keylist.h"
#include "common/streams.h"
#include "common/console.h"
