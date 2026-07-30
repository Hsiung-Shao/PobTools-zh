// PobTools item-icon manager.
//
// Fetches item icons from web.poecdn.com on demand. Downloads run on a worker
// thread with an on-disk cache (PobTools\cache\icons); GL textures are created
// on the main (GL) thread in Pump(). Everything degrades gracefully: no index,
// no data file or no network just means no icons (names still show).
//
// Art paths are the real key -- that is what the URL and the cache filename are
// built from, and two different items can legitimately share one (the atlas
// planner has two Cartography scarabs on the same art, which then share a single
// texture). The name-based calls are a thin adapter over the path ones for
// callers that only know an English item name; they need the bundled
// Data/icon_paths.json index (generated from the RePoE dataset). Callers that
// already hold an art path -- e.g. the atlas planner reading
// Data/scarabs_poe1.json -- use RequestPath/TextureByPath and need no index.
#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

class IconManager {
public:
	void Init(const std::wstring& exeDir);
	void Shutdown();                              // GL thread: joins worker, deletes textures
	void Pump();                                  // GL thread, each frame: upload finished downloads

	// By art path ("Art/2DItems/..." without extension) -- no index needed.
	void RequestPath(const std::string& artPath);
	unsigned TextureByPath(const std::string& artPath);

	// By English item name; resolved through Data/icon_paths.json.
	void Request(const std::string& name);        // main thread: queue a download if not done yet
	unsigned Texture(const std::string& name);    // main thread: GLuint (0 if not ready / no icon)

	// True when the name -> art-path index loaded; irrelevant to the path calls.
	bool available() const { return !paths_.empty(); }

private:
	void workerLoop();

	std::unordered_map<std::string, std::string> paths_; // name -> art path (immutable after Init)
	std::wstring cacheDir_;

	std::thread worker_;
	std::atomic<bool> stop_{ false };

	// Everything below is keyed by ART PATH, not by item name.
	std::mutex reqMx_;
	std::condition_variable reqCv_;
	std::deque<std::string> reqQ_;

	struct Done { std::string art; int w = 0, h = 0; unsigned char* rgba = nullptr; };
	std::mutex doneMx_;
	std::deque<Done> doneQ_;

	std::unordered_set<std::string> requested_;          // main-thread only
	std::unordered_map<std::string, unsigned> tex_;      // main-thread only (0 = none)
};
