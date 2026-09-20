#include "monitor.hpp"

#include <obs-module.h>
#include <obs.h>

#include <objbase.h>

#include <chrono>
#include <condition_variable>

#include <nlohmann/json.hpp>

#include "util.hpp"

namespace hexbar {

static std::atomic<bool> gInstantiated{false};

Monitor &Monitor::Instance()
{
	static Monitor inst;
	return inst;
}

void Monitor::Start()
{
	if (running.exchange(true))
		return;
	char *cfg = obs_module_config_path(nullptr);
	std::wstring base;
	if (cfg) {
		base = util::Utf8ToWide(cfg);
		bfree(cfg);
	}
	if (base.empty())
		base = L".";
	dataFile = base + L"\\matches.json";
	assets.Init(base + L"\\cache");

	// 已缓存的客户端目录作为发现线索
	std::wstring cachedDir;
	{
		std::lock_guard<std::mutex> lock(mtx);
		data.Load(dataFile);
		cachedDir = settings.clientDir.empty() ? data.CachedInstallDir() : settings.clientDir;
		if (cachedDir.empty())
			cachedDir = data.CachedInstallDir();
	}

	worker = std::thread([this] { Run(); });
}

void Monitor::Shutdown()
{
	if (!running.exchange(false))
		return;
	if (worker.joinable())
		worker.join();
	{
		std::lock_guard<std::mutex> lock(mtx);
		data.Save(dataFile);
	}
}

void Monitor::ApplySettings(const BarSettings &s)
{
	bool recompute = false;
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (s.maxGames != settings.maxGames || s.modeFilter != settings.modeFilter ||
		    s.batchGapMin != settings.batchGapMin || s.clientDir != settings.clientDir) {
			recompute = true;
		}
		settings = s;
	}
	if (recompute)
		RecomputeAndBump();
}

void Monitor::NotifyDisplayChanged()
{
	RecomputeAndBump();
}

Snapshot Monitor::SnapshotCopy()
{
	std::lock_guard<std::mutex> lock(mtx);
	Snapshot s = data.ComputeSnapshot(settings);
	s.epoch = epoch.load();
	return s;
}

void Monitor::RequestNewBatch()
{
	data.ManualNewBatch(util::NowMs());
	RecomputeAndBump();
	{
		std::lock_guard<std::mutex> lock(mtx);
		status = "已开始新批次,等待下一局结算";
	}
}

Monitor::StatusInfo Monitor::StatusSnapshot()
{
	StatusInfo st;
	std::lock_guard<std::mutex> lock(mtx);
	st.status = status;
	st.account = accountDisplay;
	st.lcuConnected = lcuConnected;
	st.matchCount = data.MatchCount();
	st.augmentCount = assets.AugmentCount();
	return st;
}

void Monitor::RecomputeAndBump()
{
	uint64_t e = epoch.load() + 1;
	epoch.store(e);
	SaveIfDirty();
}

void Monitor::SaveIfDirty()
{
	std::lock_guard<std::mutex> lock(mtx);
	if (data.Dirty())
		data.Save(dataFile);
}

bool Monitor::DiscoverAndConnect()
{
	std::wstring overrideDir;
	{
		std::lock_guard<std::mutex> lock(mtx);
		overrideDir = settings.clientDir.empty() ? data.CachedInstallDir() : settings.clientDir;
	}

	LcuEndpoint ep;
	std::string diag;
	if (!DiscoverLcu(overrideDir, ep, diag)) {
		std::lock_guard<std::mutex> lock(mtx);
		status = "未发现英雄联盟客户端(" + diag + "),持续重试中";
		lcuConnected = false;
		return false;
	}

	endpoint = ep;
	haveEndpoint = true;
	currentPuuid.clear();
	{
		std::lock_guard<std::mutex> lock(mtx);
		data.SetCachedInstallDir(ep.installDir);
		status = "客户端已连接(" + diag + ")";
		lcuConnected = true;
	}
	return true;
}

static bool IsGamingPhase(const std::string &p)
{
	return p == "InProgress" || p == "WaitingForStats" || p == "PreEndOfGame" || p == "EndOfGame" ||
	       p == "Reconnect";
}

static void NormalizeGames(const nlohmann::json &games, const std::string &platformId, const std::string &puuid,
			   std::vector<MatchRecord> &out)
{
	for (auto &g : games) {
		try {
			MatchRecord m;
			m.platformId = platformId;
			m.puuid = puuid;
			m.gameId = g.value("gameId", (uint64_t)0);
			if (!m.gameId)
				continue;
			m.key = platformId + ":" + puuid + ":" + std::to_string(m.gameId);
			m.durationS = g.value("gameDuration", 0);
			int64_t creation = g.value("gameCreation", (int64_t)0);
			m.startMs = creation;
			m.endMs = m.startMs + (int64_t)m.durationS * 1000;
			m.gameMode = g.value("gameMode", "");
			m.queueId = g.value("queueId", 0);
			m.matchedGame = g.value("gameType", "") == "MATCHED_GAME";

			// 找到本人参与者(国服仅返回本人)
			const nlohmann::json *mine = nullptr;
			if (g.contains("participants") && g["participants"].is_array() && !g["participants"].empty()) {
				int myPid = -1;
				if (g.contains("participantIdentities") && g["participantIdentities"].is_array()) {
					for (auto &id : g["participantIdentities"]) {
						if (id.contains("player") && id["player"].value("puuid", "") == puuid) {
							myPid = id.value("participantId", -1);
							break;
						}
					}
				}
				for (auto &p : g["participants"]) {
					int pid = p.value("participantId", -1);
					if (myPid == -1 || pid == myPid) {
						mine = &p;
						if (myPid != -1)
							break;
					}
				}
			}
			if (!mine)
				continue;

			m.championId = mine->value("championId", 0);
			auto &st = (*mine)["stats"];
			m.win = st.value("win", false);
			m.remake = st.value("gameEndedInEarlySurrender", false);
			m.kills = st.value("kills", 0);
			m.deaths = st.value("deaths", 0);
			m.assists = st.value("assists", 0);
			for (int i = 0; i < HEXBAR_MAX_HEXES; ++i) {
				std::string f = "playerAugment" + std::to_string(i + 1);
				int32_t v = st.value(f, 0);
				if (v > 0)
					m.hexes[m.hexCount++] = v;
			}
			// item0..item5(不含 item6 饰品栏),跳过空位
			for (int i = 0; i < HEXBAR_MAX_ITEMS; ++i) {
				int32_t v = st.value("item" + std::to_string(i), 0);
				if (v > 0)
					m.items[m.itemCount++] = v;
			}
			out.push_back(std::move(m));
		} catch (...) {
			// 单局解析失败跳过,不影响其它对局
		}
	}
}

bool Monitor::FetchHistory(const std::string &puuid, std::string &err, bool *changedOut)
{
	LcuClient client(endpoint);
	int endIdx = 24;
	{
		std::lock_guard<std::mutex> lock(mtx);
		endIdx = std::clamp(settings.maxGames + 4, 20, 100);
	}
	std::string path = "/lol-match-history/v1/products/lol/" + puuid +
			   "/matches?begIndex=0&endIndex=" + std::to_string(endIdx - 1);
	nlohmann::json j;
	if (!client.GetJson(path, j, err))
		return false;

	std::string platformId = "unknown";
	if (j.contains("platformId") && j["platformId"].is_string())
		platformId = j["platformId"].get<std::string>();

	std::vector<MatchRecord> incoming;
	if (j.contains("games") && j["games"].contains("games") && j["games"]["games"].is_array())
		NormalizeGames(j["games"]["games"], platformId, puuid, incoming);

	data.MergeMatches(incoming, changedOut);
	return true;
}

// 补齐快照需要的图标;返回本次是否新拉到此前缺失的图标(页面需重渲染才能显示)
bool Monitor::EnsureAssetsForSnapshot()
{
	bool fetchedAny = false;
	Snapshot snap = SnapshotCopy();
	LcuClient client(endpoint);
	for (auto &e : snap.games) {
		std::string err;
		if (assets.ChampionIconPath(e.m.championId).empty() &&
		    assets.EnsureChampionIcon(e.m.championId, haveEndpoint ? &client : nullptr, err))
			fetchedAny = true;
		for (int i = 0; i < e.m.hexCount; ++i)
			if (assets.HexIconPath(e.m.hexes[i]).empty() && assets.EnsureHexIcon(e.m.hexes[i], err))
				fetchedAny = true;
		for (int i = 0; i < e.m.itemCount; ++i)
			if (assets.ItemIconPath(e.m.items[i]).empty() &&
			    assets.EnsureItemIcon(e.m.items[i], haveEndpoint ? &client : nullptr, err))
				fetchedAny = true;
	}
	return fetchedAny;
}

void Monitor::FetchAll(bool manual)
{
	if (!haveEndpoint)
		return;

	LcuClient client(endpoint);
	nlohmann::json summoner;
	std::string err;
	if (!client.GetJson("/lol-summoner/v1/current-summoner", summoner, err)) {
		std::lock_guard<std::mutex> lock(mtx);
		status = "读取账号失败:" + err;
		lcuConnected = false;
		haveEndpoint = false;
		return;
	}
	std::string puuid = summoner.value("puuid", "");
	if (puuid.empty()) {
		std::lock_guard<std::mutex> lock(mtx);
		status = "账号信息缺少 puuid";
		return;
	}
	std::string display = summoner.value("gameName", summoner.value("displayName", ""));
	std::string tag = summoner.value("tagLine", "");
	if (!tag.empty())
		display += "#" + tag;

	bool puuidChanged = currentPuuid != puuid;
	currentPuuid = puuid;
	(void)puuidChanged;

	bool dataChanged = false;
	if (!FetchHistory(puuid, err, &dataChanged)) {
		std::lock_guard<std::mutex> lock(mtx);
		status = "拉取战绩失败:" + err + (manual ? "(已请求手动刷新)" : ",稍后重试");
		lcuConnected = false;
		haveEndpoint = false;
		return;
	}

	// 装备元数据缺失时补一次(LCU 在线才能拉)
	if (!assets.ItemsValid()) {
		std::string ierr;
		assets.RefreshItems(&client, ierr);
	}

	bool assetsChanged = EnsureAssetsForSnapshot();
	// 数据有变或补到此前缺失的图标才 bump epoch(页面轮询方重渲染);
	// 没变只落盘,不打扰侧栏滚动动画
	if (dataChanged || assetsChanged)
		RecomputeAndBump();
	else
		SaveIfDirty();

	std::lock_guard<std::mutex> lock(mtx);
	accountDisplay = display;
	status = "已连接 " + display + " · 战绩已更新";
	lcuConnected = true;
}

void Monitor::Run()
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	{
		std::string err;
		assets.RefreshAugments(err); // 失败时用磁盘缓存
		RecomputeAndBump();
	}

	int64_t lastSaveMs = 0;
	while (running.load()) {
		int64_t now = util::NowMs();

		if (refreshRequested.exchange(false)) {
			if (!haveEndpoint)
				DiscoverAndConnect();
			if (haveEndpoint)
				FetchAll(true);
			lastFetchMs = now;
		}

		if (!haveEndpoint) {
			if (now - lastDiscoverMs > 5000) {
				lastDiscoverMs = now;
				DiscoverAndConnect();
				if (haveEndpoint) {
					FetchAll(false);
					lastFetchMs = util::NowMs();
				}
			}
		} else {
			// 阶段轮询:检测"游戏结束"跳变并触发拉取
			if (now - lastPhasePollMs > 2000) {
				lastPhasePollMs = now;
				LcuClient client(endpoint);
				nlohmann::json j;
				std::string err;
				if (client.GetJson("/lol-gameflow/v1/gameflow-phase", j, err) && j.is_string()) {
					std::string phase = j.get<std::string>();
					bool wasGaming = IsGamingPhase(prevPhase);
					bool isGaming = IsGamingPhase(phase);
					prevPhase = phase;
					if (wasGaming && !isGaming) {
						// 结算可能延迟:立即拉一次,失败由重连机制退避重试
						FetchAll(false);
						lastFetchMs = util::NowMs();
					}
					std::lock_guard<std::mutex> lock(mtx);
					lcuConnected = true;
				} else {
					// 接口失败:先退避重试,连续失败再重新发现
					if (now - lastFetchMs > 15000) {
						haveEndpoint = false;
						std::lock_guard<std::mutex> lock(mtx);
						lcuConnected = false;
						status = "客户端连接中断:" + err;
					}
				}
			}

			// 战绩自动刷新:非对局中每 15 秒拉一次(结算延迟、换账号都自动补上,
			// 数据没变不会触发页面重渲染);对局中历史不会变化,退避到 5 分钟兜底
			int64_t fetchGap = IsGamingPhase(prevPhase) ? 300000LL : 15000LL;
			if (now - lastFetchMs > fetchGap) {
				lastFetchMs = now;
				FetchAll(false);
			}

			// 海克斯/装备元数据 6 小时刷新
			if (now - lastAugmentRefreshMs > 6 * 3600 * 1000LL) {
				lastAugmentRefreshMs = now;
				std::string err;
				assets.RefreshAugments(err);
				LcuClient client(endpoint);
				assets.RefreshItems(&client, err);
			}
		}

		// 退出前/定期落盘
		if (data.Dirty() && now - lastSaveMs > 3000) {
			lastSaveMs = now;
			std::lock_guard<std::mutex> lock(mtx);
			data.Save(dataFile);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}

	CoUninitialize();
}

} // namespace hexbar
