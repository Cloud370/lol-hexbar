#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "assets.hpp"
#include "gamedata.hpp"
#include "lcu.hpp"

namespace hexbar {

class Monitor {
public:
	static Monitor &Instance();

	void Start();
	void Shutdown();

	void ApplySettings(const BarSettings &s);
	Snapshot SnapshotCopy();
	uint64_t CurrentEpoch() const { return epoch.load(); }

	// 展示类配置变化后调用:让页面轮询立刻看到新样式
	void NotifyDisplayChanged();

	void RequestRefresh() { refreshRequested = true; }
	void RequestNewBatch();

	// 供 HTTP 服务读取的聚合状态
	struct StatusInfo {
		std::string status;
		std::string account; // utf-8,如 玩家名#TAG
		bool lcuConnected = false;
		size_t matchCount = 0;
		size_t augmentCount = 0;
	};
	StatusInfo StatusSnapshot();

	// 渲染线程只读访问(内部自持锁;路径查询仅做文件系统 stat)
	AssetStore &Assets() { return assets; }

private:
	Monitor() = default;
	void Run();

	bool DiscoverAndConnect();
	void FetchAll(bool manual);
	bool FetchHistory(const std::string &puuid, std::string &err, bool *changedOut = nullptr);
	void RecomputeAndBump();
	void SaveIfDirty();
	bool EnsureAssetsForSnapshot();

	std::thread worker;
	std::atomic<bool> running{false};
	std::atomic<bool> refreshRequested{false};
	std::atomic<uint64_t> epoch{1};

	std::mutex mtx; // 保护 data/assets/settings/status
	GameData data;
	AssetStore assets;
	BarSettings settings;
	std::wstring dataFile;
	std::string status = "初始化…";
	std::string accountDisplay; // 如 玩家名#TAG
	bool lcuConnected = false;

	// 仅工作线程访问
	LcuEndpoint endpoint;
	bool haveEndpoint = false;
	std::string currentPuuid;
	std::string prevPhase;
	int64_t lastPhasePollMs = 0;
	int64_t lastDiscoverMs = 0;
	int64_t lastFetchMs = 0;
	int64_t lastAugmentRefreshMs = 0;
	int64_t fastPollUntilMs = 0; // 结算追赶窗口截止时刻:期间每 5 秒拉战绩
};

} // namespace hexbar
