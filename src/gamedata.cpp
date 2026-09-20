#include "gamedata.hpp"

#include <algorithm>
#include <map>

#include <nlohmann/json.hpp>

#include "util.hpp"

namespace hexbar {

static const char *kSchemaVersion = "1";

void GameData::Load(const std::wstring &file)
{
	std::lock_guard<std::mutex> lock(mtx);
	matches.clear();
	manualPins.clear();
	manualBoundaryMs = 0;

	std::string content;
	if (!util::ReadFileBytes(file, content))
		return;
	try {
		auto j = nlohmann::json::parse(content);
		if (j.value("schema", "") != kSchemaVersion)
			return;
		cachedInstallDir = util::Utf8ToWide(j.value("installDir", ""));
		manualBoundaryMs = j.value("manualBoundaryMs", (int64_t)0);
		// 旧版本只有一个 pinnedStarts 列表(自动/手动混存);新版仅保留手动边界语义
		for (auto p : j.value("pinnedStarts", std::vector<int64_t>{}))
			manualPins.push_back(p);
		for (auto &g : j.value("matches", std::vector<nlohmann::json>{})) {
			MatchRecord m;
			m.key = g.value("key", "");
			m.platformId = g.value("platformId", "");
			m.puuid = g.value("puuid", "");
			m.gameId = g.value("gameId", (uint64_t)0);
			m.startMs = g.value("startMs", (int64_t)0);
			m.endMs = g.value("endMs", (int64_t)0);
			m.durationS = g.value("durationS", 0);
			m.championId = g.value("championId", 0);
			m.win = g.value("win", false);
			m.remake = g.value("remake", false);
			m.kills = g.value("kills", 0);
			m.deaths = g.value("deaths", 0);
			m.assists = g.value("assists", 0);
			auto hexArr = g.value("hexes", std::vector<int32_t>{});
			m.hexCount = 0;
			for (size_t i = 0; i < hexArr.size() && i < HEXBAR_MAX_HEXES; ++i)
				m.hexes[m.hexCount++] = hexArr[i];
			auto itemArr = g.value("items", std::vector<int32_t>{});
			m.itemCount = 0;
			for (size_t i = 0; i < itemArr.size() && i < HEXBAR_MAX_ITEMS; ++i)
				m.items[m.itemCount++] = itemArr[i];
			m.gameMode = g.value("gameMode", "");
			m.queueId = g.value("queueId", 0);
			m.matchedGame = g.value("matchedGame", false);
			if (!m.key.empty() && m.gameId)
				matches.push_back(std::move(m));
		}
	} catch (...) {
		// 缓存损坏时静默丢弃,重新采集
	}
	dirty = false;
}

bool GameData::Save(const std::wstring &file)
{
	std::lock_guard<std::mutex> lock(mtx);
	nlohmann::json j;
	j["schema"] = kSchemaVersion;
	j["installDir"] = util::WideToUtf8(cachedInstallDir);
	j["manualBoundaryMs"] = manualBoundaryMs;
	j["pinnedStarts"] = manualPins;
	std::vector<nlohmann::json> arr;
	arr.reserve(matches.size());
	for (auto &m : matches) {
		nlohmann::json g;
		g["key"] = m.key;
		g["platformId"] = m.platformId;
		g["puuid"] = m.puuid;
		g["gameId"] = m.gameId;
		g["startMs"] = m.startMs;
		g["endMs"] = m.endMs;
		g["durationS"] = m.durationS;
		g["championId"] = m.championId;
		g["win"] = m.win;
		g["remake"] = m.remake;
		g["kills"] = m.kills;
		g["deaths"] = m.deaths;
		g["assists"] = m.assists;
		std::vector<int32_t> hexArr;
		for (int i = 0; i < m.hexCount; ++i)
			hexArr.push_back(m.hexes[i]);
		g["hexes"] = hexArr;
		std::vector<int32_t> itemArr;
		for (int i = 0; i < m.itemCount; ++i)
			itemArr.push_back(m.items[i]);
		g["items"] = itemArr;
		g["gameMode"] = m.gameMode;
		g["queueId"] = m.queueId;
		g["matchedGame"] = m.matchedGame;
		arr.push_back(std::move(g));
	}
	j["matches"] = arr;
	std::string out = j.dump(1);
	if (util::WriteFileBytesAtomic(file, out.data(), out.size())) {
		dirty = false;
		return true;
	}
	return false;
}

bool GameData::MergeMatches(std::vector<MatchRecord> &incoming, bool *changedOut)
{
	std::lock_guard<std::mutex> lock(mtx);
	std::map<std::string, size_t> index;
	for (size_t i = 0; i < matches.size(); ++i)
		index[matches[i].key] = i;

	bool changed = false;
	for (auto &m : incoming) {
		if (m.key.empty() || !m.gameId)
			continue;
		auto it = index.find(m.key);
		if (it == index.end()) {
			index[m.key] = matches.size();
			matches.push_back(m);
			changed = true;
		} else if (!matches[it->second].SameContent(m)) {
			matches[it->second] = m;
			changed = true;
		}
	}
	if (changed)
		dirty = true;
	if (changedOut)
		*changedOut = changed;
	return changed;
}

static bool ModeSelected(const MatchRecord &m, int modeFilter)
{
	if (modeFilter == 0)
		return m.gameMode.rfind("KIWI", 0) == 0; // KIWI / KIWI_JADE
	return m.matchedGame;
}

Snapshot GameData::ComputeSnapshot(const BarSettings &s)
{
	std::lock_guard<std::mutex> lock(mtx);

	int gapMs = std::max(1, s.batchGapMin) * 60LL * 1000LL;

	std::vector<const MatchRecord *> sel;
	for (auto &m : matches)
		if (ModeSelected(m, s.modeFilter))
			sel.push_back(&m);
	std::sort(sel.begin(), sel.end(), [](const MatchRecord *a, const MatchRecord *b) {
		return a->startMs != b->startMs ? a->startMs < b->startMs : a->gameId < b->gameId;
	});

	// 分批:相邻空档超阈值、或越过手动"开始新批次"边界时切分。
	// 间隔修改立即生效(便于调试与合并历史);当前批次的稳定由手动边界保证。
	auto crossesPin = [&](int64_t prevStart, int64_t curStart) {
		for (auto p : manualPins)
			if (prevStart < p && p <= curStart)
				return true;
		return false;
	};

	std::vector<std::pair<size_t, size_t>> batches; // [begin,end) 下标区间
	for (size_t i = 0; i < sel.size();) {
		size_t j = i + 1;
		while (j < sel.size()) {
			int64_t gap = sel[j]->startMs - sel[j - 1]->endMs;
			if (gap > gapMs || crossesPin(sel[j - 1]->startMs, sel[j]->startMs))
				break;
			++j;
		}
		batches.emplace_back(i, j);
		i = j;
	}

	// 确定当前批次:默认最后一批;手动边界晚于最后一批起点时,当前批次为空
	size_t cur = batches.size();
	if (!batches.empty()) {
		cur = batches.size() - 1;
		if (manualBoundaryMs > 0 && sel[batches.back().first]->startMs < manualBoundaryMs)
			cur = batches.size(); // 新批次尚无对局 → 空展示
	}

	Snapshot snap;
	if (cur < batches.size()) {
		auto [b, e] = batches[cur];
		int64_t batchEnd = 0;
		int64_t batchStart = sel[b]->startMs;
		for (size_t k = b; k < e; ++k)
			batchEnd = std::max(batchEnd, sel[k]->endMs);
		(void)batchEnd;
		(void)batchStart;

		int wins = 0, losses = 0, remakes = 0;
		for (size_t k = b; k < e; ++k) {
			if (sel[k]->remake)
				++remakes;
			else if (sel[k]->win)
				++wins;
			else
				++losses;
		}
		snap.wins = wins;
		snap.losses = losses;
		snap.remakes = remakes;

		// 展示列表不含重开局(重开也不计入胜负);截断与详细位只作用于有效对局
		std::vector<const MatchRecord *> disp;
		for (size_t k = b; k < e; ++k) {
			if (!sel[k]->remake)
				disp.push_back(sel[k]);
		}
		size_t count = std::min<size_t>(disp.size(), (size_t)std::max(1, s.maxGames));
		size_t begin = disp.size() - count;
		for (size_t k = begin; k < disp.size(); ++k) {
			Snapshot::Entry entry;
			entry.m = *disp[k];
			snap.games.push_back(std::move(entry));
		}
	}
	return snap;
}

void GameData::ManualNewBatch(int64_t nowMs)
{
	std::lock_guard<std::mutex> lock(mtx);
	manualBoundaryMs = nowMs;
	manualPins.push_back(nowMs);
	dirty = true;
}

void GameData::Prune(size_t keepMax)
{
	std::lock_guard<std::mutex> lock(mtx);
	if (matches.size() <= keepMax)
		return;
	std::sort(matches.begin(), matches.end(),
		  [](const MatchRecord &a, const MatchRecord &b) { return a.startMs < b.startMs; });
	matches.erase(matches.begin(), matches.end() - (std::ptrdiff_t)keepMax);
	dirty = true;
}

size_t GameData::MatchCount() const
{
	std::lock_guard<std::mutex> lock(mtx);
	return matches.size();
}

std::wstring GameData::CachedInstallDir() const
{
	std::lock_guard<std::mutex> lock(mtx);
	return cachedInstallDir;
}

void GameData::SetCachedInstallDir(const std::wstring &dir)
{
	std::lock_guard<std::mutex> lock(mtx);
	if (cachedInstallDir != dir) {
		cachedInstallDir = dir;
		dirty = true;
	}
}

} // namespace hexbar
