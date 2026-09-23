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

	// 供 HTTP 服务读取的聚合状态(含诊断字段,便于定位"为什么不刷新")
	struct StatusInfo {
		std::string status;
		std::string account; // utf-8,如 玩家名#TAG
		bool lcuConnected = false;
		size_t matchCount = 0;
		size_t augmentCount = 0;

		// ---- 诊断 ----
		std::string phase;         // 当前 gameflow 阶段
		bool inGame = false;       // 游戏内实时接口可用(= 真正在对局中)
		double gameTimeS = 0;      // 游戏内时长(秒)
		std::string gameMode;      // 游戏内模式
		bool settlement = false;   // 结算追赶窗口是否打开
		int64_t lastAttemptMs = 0; // 上次尝试拉取战绩的时刻
		int64_t lastRefreshMs = 0; // 上次真正拉到数据变化的时刻
		std::string lastReason;    // 上次拉取原因
		size_t lastIncoming = 0;   // 上次接口返回的对局条数
		size_t lastAdded = 0;      // 上次新增对局数
		uint64_t newestGameId = 0; // 已知最新对局 ID
		int failStreak = 0;        // 连续拉取失败次数
	};
	StatusInfo StatusSnapshot();

	// 渲染线程只读访问(内部自持锁;路径查询仅做文件系统 stat)
	AssetStore &Assets() { return assets; }

private:
	Monitor() = default;
	void Run();
	void Tick(int64_t now);

	bool DiscoverAndConnect();
	void FetchAll(bool manual, const char *reason);
	bool FetchHistory(const std::string &puuid, std::string &err, bool *changedOut, size_t *incomingOut,
			  size_t *addedOut, uint64_t *newestOut);
	void RecomputeAndBump();
	void SaveIfDirty();
	bool EnsureAssetsForSnapshot();
	void MarkGameEnded(int64_t now, const char *source);
	void OnFetchFailure(const std::string &msg);
	void DbgLog(const char *fmt, ...); // 仅在开启 debugLog 时输出

	std::thread worker;
	std::atomic<bool> running{false};
	std::atomic<bool> refreshRequested{false};
	std::atomic<uint64_t> epoch{1};

	// 采集节奏/调试开关:由 ApplySettings 从设置页写入,工作线程无锁读取
	std::atomic<int> optPhasePollMs{2000};
	std::atomic<int> optLivePollMs{1000};
	std::atomic<int> optFastPollMs{2000};
	std::atomic<int> optIdlePollMs{10000};
	std::atomic<int> optInGamePollMs{300000};
	std::atomic<bool> optDebugLog{false};

	std::mutex mtx; // 保护 data/assets/settings/status/诊断字段
	GameData data;
	AssetStore assets;
	BarSettings settings;
	std::wstring dataFile;
	std::string status = "初始化…";
	std::string accountDisplay; // 如 玩家名#TAG
	bool lcuConnected = false;

	// 诊断字段(受 mtx 保护)
	std::string dPhase;
	bool dInGame = false;
	double dGameTimeS = 0;
	std::string dGameMode;
	bool dSettlement = false;
	int64_t dLastAttemptMs = 0;
	int64_t dLastRefreshMs = 0;
	std::string dLastReason;
	size_t dLastIncoming = 0;
	size_t dLastAdded = 0;
	uint64_t dNewestGameId = 0;
	int dFailStreak = 0;

	// 仅工作线程访问
	LcuEndpoint endpoint;
	bool haveEndpoint = false;
	std::string currentPuuid;
	std::string prevPhase;
	bool inGame = false;
	double gameTimeS = 0;
	std::string gameMode;
	bool gameEndPending = false; // 对局已结束但战绩列表尚未出现新对局
	int64_t lastPhasePollMs = 0;
	int64_t lastDiscoverMs = 0;
	int64_t lastFetchMs = 0;
	int64_t lastAugmentRefreshMs = 0;
	int64_t lastLivePollMs = 0;
	int64_t lastHeartbeatMs = 0;
	int64_t fastPollUntilMs = 0; // 结算追赶窗口截止时刻
	size_t lastFetchAdded = 0;   // 上次拉取新增局数(用于收窄追赶窗口)
	int consecutiveFetchFailures = 0;
	int consecutivePhaseFailures = 0;
};

} // namespace hexbar
