#include "r_perf_gpu.h"

#include <glad/gles2.h>

#include <cstring>

// Not in the generated glad (it only carries the extensions the renderer
// uses); the values are fixed by the EXT_disjoint_timer_query spec.
#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif

namespace {
const int kMaxQueries = 256; // a frame uses a handful; results lag a few frames
}

void r_perfGpu_c::Init()
{
	if (inited_) return;
	inited_ = true;
	supported_ = false;
	GLint n = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &n);
	for (GLint i = 0; i < n; i++) {
		const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
		if (e && strcmp(e, "GL_EXT_disjoint_timer_query") == 0) {
			supported_ = true;
			break;
		}
	}
	if (supported_) {
		GLint dummy = 0;
		glGetIntegerv(GL_GPU_DISJOINT_EXT, &dummy); // clears the flag
		while (glGetError() != GL_NO_ERROR) {}
	}
}

void r_perfGpu_c::Begin(PerfLog::Gpu sec)
{
	if (!supported_ || active_) return;
	unsigned int q = 0;
	if (!free_.empty()) {
		q = free_.back();
		free_.pop_back();
	}
	else if (created_ < kMaxQueries) {
		GLuint id = 0;
		glGenQueries(1, &id);
		q = id;
		created_++;
	}
	if (!q) return; // pool exhausted: this section goes unmeasured, never blocks
	glBeginQuery(GL_TIME_ELAPSED_EXT, q);
	if (glGetError() != GL_NO_ERROR) {
		// The extension is listed but refuses the target: stop trying.
		supported_ = false;
		free_.push_back(q);
		return;
	}
	cur_ = q;
	curSec_ = sec;
	active_ = true;
}

void r_perfGpu_c::End()
{
	if (!active_) return;
	glEndQuery(GL_TIME_ELAPSED_EXT);
	pending_.push_back({ cur_, curSec_ });
	active_ = false;
}

void r_perfGpu_c::Poll(PerfLog::Recorder* rec)
{
	if (!supported_ || !rec) return;
	GLint disjoint = 0;
	glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
	if (disjoint) rec->GpuDisjoint();
	while (!pending_.empty()) {
		const Pending p = pending_.front();
		GLuint avail = 0;
		glGetQueryObjectuiv(p.query, GL_QUERY_RESULT_AVAILABLE, &avail);
		if (!avail) break; // results arrive in order: the rest are not ready either
		GLuint ns = 0;     // low 32 bits of the 64-bit result: fine below 4.29 s
		glGetQueryObjectuiv(p.query, GL_QUERY_RESULT, &ns);
		pending_.pop_front();
		free_.push_back(p.query);
		if (!disjoint) rec->AddGpu(p.sec, ns / 1.0e6);
	}
}
