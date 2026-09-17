// GPU time per section of a POB frame, for the opt-in performance log
// (host/perf_log.h). Timer queries (EXT_disjoint_timer_query, which ANGLE's
// D3D11 back end exposes) are begun and ended around a section and read back
// frames later, only once the driver says the result is available: nothing
// here ever waits on the GPU, so turning the log on does not change the frame
// timing it is trying to observe.
//
// ES3 allows one TIME_ELAPSED query active at a time, so sections never nest:
// the liquid-glass pass runs inside the layer replay, and the caller switches
// layers -> glass -> layers around it (see DrawGlassPanel).
#pragma once

#include <deque>
#include <vector>

#include "../../host/perf_log.h"

class r_perfGpu_c {
public:
	// Call with the GL context current. Idempotent.
	void Init();
	bool Supported() const { return supported_; }

	void Begin(PerfLog::Gpu sec);
	void End();
	// Section currently being timed, or -1.
	int Active() const { return active_ ? (int)curSec_ : -1; }

	// Hands finished results to the recorder; never blocks.
	void Poll(PerfLog::Recorder* rec);

private:
	struct Pending { unsigned int query; PerfLog::Gpu sec; };
	bool inited_ = false;
	bool supported_ = false;
	bool active_ = false;
	unsigned int cur_ = 0;
	PerfLog::Gpu curSec_ = PerfLog::GpuLayers;
	std::vector<unsigned int> free_;
	std::deque<Pending> pending_;
	int created_ = 0;
};
