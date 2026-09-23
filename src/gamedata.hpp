#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace hexbar {

constexpr int HEXBAR_MAX_HEXES = 6;
constexpr int HEXBAR_MAX_ITEMS = 6;

struct MatchRecord {
	std::string key; // platformId:puuid:gameId
	std::string platformId;
	std::string puuid;
	uint64_t gameId = 0;
	int64_t startMs = 0;
	int64_t endMs = 0;
	int32_t durationS = 0;
	int32_t championId = 0;
	bool win = false;
	bool remake = false;
	int32_t kills = 0;
	int32_t deaths = 0;
	int32_t assists = 0;
	int32_t hexes[HEXBAR_MAX_HEXES] = {};
	int32_t hexCount = 0;
	int32_t items[HEXBAR_MAX_ITEMS] = {}; // item0..item5,不含饰品栏
	int32_t itemCount = 0;
	std::string gameMode;
	int32_t queueId = 0;
	bool matchedGame = false;

	bool SameContent(const MatchRecord &o) const
	{
		return key == o.key && platformId == o.platformId && puuid == o.puuid && gameId == o.gameId &&
		       startMs == o.startMs && endMs == o.endMs && durationS == o.durationS &&
		       championId == o.championId && win == o.win && remake == o.remake && kills == o.kills &&
		       deaths == o.deaths && assists == o.assists && hexCount == o.hexCount &&
		       itemCount == o.itemCount && gameMode == o.gameMode && queueId == o.queueId &&
		       matchedGame == o.matchedGame && memcmp(hexes, o.hexes, sizeof(hexes)) == 0 &&
		       memcmp(items, o.items, sizeof(items)) == 0;
	}
};

// 全局采集/批次配置;面板各自的展示配置在 webserver.hpp 的 AppSettings 里,互相独立
struct BarSettings {
	int batchGapMin = 100000;
	int maxGames = 20;
	int modeFilter = 0; // 0=仅海克斯大乱斗 1=全部
	std::wstring clientDir;

	// 采集/刷新节奏(设置页「高级 / 调试」可调,**统一以秒为单位**;改动即时生效、无需重启)
	int phasePollSec = 2;    // gameflow 阶段轮询
	int livePollSec = 1;     // 对局中游戏内实时接口轮询
	int fastPollSec = 2;     // 对局结束后的结算追赶轮询
	int idlePollSec = 10;    // 空闲时战绩轮询
	int inGamePollSec = 300; // 对局中战绩兜底(对局里历史不会变,退避)
	bool debugLog = false;   // 输出逐次拉取/心跳等详细日志
};

struct Snapshot {
	struct Entry {
		MatchRecord m;
	};
	std::vector<Entry> games; // 时间升序,左旧右新,已截断且不含重开
	int wins = 0;
	int losses = 0;
	int remakes = 0;
	uint64_t epoch = 0;
};

class GameData {
public:
	void Load(const std::wstring &file);
	bool Save(const std::wstring &file);
	bool Dirty() const { return dirty; }

	// 合并一批(同一账号)战绩,返回是否有变化;changedOut 输出是否变化,
	// addedOut 输出本次新增(此前不存在的 key)局数,便于日志/诊断
	bool MergeMatches(std::vector<MatchRecord> &incoming, bool *changedOut = nullptr, size_t *addedOut = nullptr);

	// 按当前设置计算展示快照;内部同时维护批次固定点
	Snapshot ComputeSnapshot(const BarSettings &s);

	void ManualNewBatch(int64_t nowMs);
	void Prune(size_t keepMax);

	size_t MatchCount() const;

	std::wstring CachedInstallDir() const;
	void SetCachedInstallDir(const std::wstring &dir);

private:
	mutable std::mutex mtx;
	std::vector<MatchRecord> matches;
	std::vector<int64_t> manualPins; // 手动"开始新批次"边界,永久生效
	int64_t manualBoundaryMs = 0;
	mutable std::wstring cachedInstallDir;
	bool dirty = false;
};

} // namespace hexbar
