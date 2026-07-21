// PobTools item-icon manager.
//
// Uses a bundled name -> art-path index (Data/icon_paths.json, generated from the
// RePoE dataset) to fetch item icons from web.poecdn.com on demand. Downloads run
// on a worker thread with an on-disk cache; GL textures are created on the main
// (GL) thread in Pump(). Everything degrades gracefully: no index or no network
// just means no icons (Chinese names still show).
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
	void Request(const std::string& name);        // main thread: queue a download if not done yet
	unsigned Texture(const std::string& name);    // main thread: GLuint (0 if not ready / no icon)
	bool available() const { return !paths_.empty(); }

private:
	void workerLoop();

	std::unordered_map<std::string, std::string> paths_; // name -> art path (immutable after Init)
	std::wstring cacheDir_;

	std::thread worker_;
	std::atomic<bool> stop_{ false };

	std::mutex reqMx_;
	std::condition_variable reqCv_;
	std::deque<std::string> reqQ_;

	struct Done { std::string name; int w = 0, h = 0; unsigned char* rgba = nullptr; };
	std::mutex doneMx_;
	std::deque<Done> doneQ_;

	std::unordered_set<std::string> requested_;          // main-thread only
	std::unordered_map<std::string, unsigned> tex_;      // main-thread only (0 = none)
};
