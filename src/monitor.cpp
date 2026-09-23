#include "monitor.hpp"

#include <obs-module.h>
#include <obs.h>

#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>

#include <nlohmann/json.hpp>

#include "util.hpp"

namespace hexbar {

static std::atomic<bool> gInstantiated{false};

// 状态机固定参数(轮询间隔本身可由设置页「高级 / 调试」调整,见 opt*)
static constexpr int64_t kFastWindowMs = 300000;  // 对局结束后的追赶窗口(5 分钟)
static constexpr int64_t kFastTailMs = 45000;     // 看到新对局后再追一小段,等数据落全
static constexpr int64_t kDiscoverRetryMs = 5000; // 未连接时发现重试
static constexpr int64_t kHeartbeatMs = 60000;    // 状态心跳日志

Monitor &Monitor::Instance()
{
	static Monitor inst;
	return inst;
}

static std::string FmtClock(double sec)
{
	int s = (int)sec;
	if (s < 0)
		s = 0;
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
	return buf;
}

void Monitor::DbgLog(const char *fmt, ...)
{
	if (!optDebugLog.load())
		return;
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	blog(LOG_INFO, "[lol-hexbar][debug] %s", buf);
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
	blog(LOG_INFO, "[lol-hexbar] monitor start: config=%s, cached matches=%zu", util::WideToUtf8(base).c_str(),
	     data.MatchCount());

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
	// 采集节奏/调试开关即时生效(无需重启 OBS);配置以秒为单位,内部转毫秒
	optPhasePollMs.store(std::max(1, s.phasePollSec) * 1000);
	optLivePollMs.store(std::max(1, s.livePollSec) * 1000);
	optFastPollMs.store(std::max(1, s.fastPollSec) * 1000);
	optIdlePollMs.store(std::max(1, s.idlePollSec) * 1000);
	optInGamePollMs.store(std::max(1, s.inGamePollSec) * 1000);
	bool oldDbg = optDebugLog.exchange(s.debugLog);
	if (oldDbg != s.debugLog)
		blog(LOG_INFO, "[lol-hexbar] debug log %s", s.debugLog ? "enabled" : "disabled");
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
	st.phase = dPhase;
	st.inGame = dInGame;
	st.gameTimeS = dGameTimeS;
	st.gameMode = dGameMode;
	st.settlement = dSettlement;
	st.lastAttemptMs = dLastAttemptMs;
	st.lastRefreshMs = dLastRefreshMs;
	st.lastReason = dLastReason;
	st.lastIncoming = dLastIncoming;
	st.lastAdded = dLastAdded;
	st.newestGameId = dNewestGameId;
	st.failStreak = dFailStreak;
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
	consecutiveFetchFailures = 0;
	consecutivePhaseFailures = 0;
	// 重连/换号后重置对局状态并开追赶窗口:断连期间结束的对局(常见于换号第一局)
	// 会被尽快补上,而不是等到下一次对局结束。
	prevPhase.clear();
	inGame = false;
	gameTimeS = 0;
	gameMode.clear();
	gameEndPending = false;
	lastFetchAdded = 0;
	fastPollUntilMs = util::NowMs() + kFastWindowMs;
	{
		std::lock_guard<std::mutex> lock(mtx);
		data.SetCachedInstallDir(ep.installDir);
		status = "客户端已连接(" + diag + ")";
		lcuConnected = true;
	}
	blog(LOG_INFO, "[lol-hexbar] lcu connected: %s (port=%d)", diag.c_str(), ep.port);
	return true;
}

static bool IsGamingPhase(const std::string &p)
{
	return p == "InProgress" || p == "WaitingForStats" || p == "PreEndOfGame" || p == "EndOfGame" ||
	       p == "Reconnect";
}

// 解析战绩列表;返回成功解析的局数,skipped 输出因缺字段/异常被跳过的局数
static void NormalizeGames(const nlohmann::json &games, const std::string &platformId, const std::string &puuid,
			   std::vector<MatchRecord> &out, size_t &skipped)
{
	for (auto &g : games) {
		try {
			MatchRecord m;
			m.platformId = platformId;
			m.puuid = puuid;
			m.gameId = g.value("gameId", (uint64_t)0);
			if (!m.gameId) {
				++skipped;
				continue;
			}
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
			if (!mine) {
				++skipped;
				continue;
			}

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
			++skipped;
		}
	}
}

bool Monitor::FetchHistory(const std::string &puuid, std::string &err, bool *changedOut, size_t *incomingOut,
			   size_t *addedOut, uint64_t *newestOut)
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
	if (!client.GetJson(path, j, err)) {
		err = "matches: " + err;
		return false;
	}

	std::string platformId = "unknown";
	if (j.contains("platformId") && j["platformId"].is_string())
		platformId = j["platformId"].get<std::string>();

	std::vector<MatchRecord> incoming;
	size_t total = 0, skipped = 0;
	if (j.contains("games") && j["games"].contains("games") && j["games"]["games"].is_array()) {
		total = j["games"]["games"].size();
		NormalizeGames(j["games"]["games"], platformId, puuid, incoming, skipped);
	} else {
		err = "matches: unexpected payload (games.games missing)";
		return false;
	}

	uint64_t newest = 0;
	for (auto &m : incoming)
		newest = std::max(newest, m.gameId);

	bool changed = false;
	size_t added = 0;
	data.MergeMatches(incoming, &changed, &added);
	if (changedOut)
		*changedOut = changed;
	if (incomingOut)
		*incomingOut = incoming.size();
	if (addedOut)
		*addedOut = added;
	if (newestOut)
		*newestOut = newest;
	if (skipped > 0)
		blog(LOG_INFO, "[lol-hexbar] history: %zu games, parsed %zu, skipped %zu", total, incoming.size(),
		     skipped);
	DbgLog("history(%s): total=%zu parsed=%zu skipped=%zu added=%zu", puuid.substr(0, 8).c_str(), total,
	       incoming.size(), skipped, added);
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

void Monitor::OnFetchFailure(const std::string &msg)
{
	++consecutiveFetchFailures;
	int streak = consecutiveFetchFailures;
	{
		std::lock_guard<std::mutex> lock(mtx);
		dFailStreak = streak;
		lcuConnected = streak < 2;
		status = msg + ",连续失败 " + std::to_string(streak) + " 次";
	}
	if (streak >= 3) {
		blog(LOG_WARNING, "[lol-hexbar] %s — 连续 3 次失败,丢弃端点重新发现客户端", msg.c_str());
		haveEndpoint = false;
		consecutiveFetchFailures = 0;
	}
}

void Monitor::FetchAll(bool manual, const char *reason)
{
	if (!haveEndpoint)
		return;

	{
		std::lock_guard<std::mutex> lock(mtx);
		dLastAttemptMs = util::NowMs();
		dLastReason = reason;
	}
	DbgLog("fetch start (%s)", reason);

	LcuClient client(endpoint);
	nlohmann::json summoner;
	std::string err;
	if (!client.GetJson("/lol-summoner/v1/current-summoner", summoner, err)) {
		OnFetchFailure("读取账号失败:" + err);
		return;
	}
	std::string puuid;
	std::string display;
	std::string tag;
	try {
		puuid = summoner.value("puuid", "");
		display = summoner.value("gameName", summoner.value("displayName", ""));
		tag = summoner.value("tagLine", "");
	} catch (const std::exception &e) {
		OnFetchFailure(std::string("账号信息解析失败:") + e.what());
		return;
	}
	if (puuid.empty()) {
		OnFetchFailure("账号信息缺少 puuid");
		return;
	}
	if (!tag.empty())
		display += "#" + tag;

	// 同一客户端内换号(不重启):显式重置状态并开追赶窗口,确保新账号第一局尽快上屏
	if (!currentPuuid.empty() && currentPuuid != puuid) {
		blog(LOG_INFO, "[lol-hexbar] account switched (%s -> %s): reset state, fast catch-up",
		     currentPuuid.substr(0, 8).c_str(), puuid.substr(0, 8).c_str());
		prevPhase.clear();
		inGame = false;
		gameTimeS = 0;
		gameEndPending = false;
		lastFetchAdded = 0;
		fastPollUntilMs = util::NowMs() + kFastWindowMs;
	}
	currentPuuid = puuid;

	bool dataChanged = false;
	size_t incoming = 0, added = 0;
	uint64_t newest = 0;
	if (!FetchHistory(puuid, err, &dataChanged, &incoming, &added, &newest)) {
		OnFetchFailure("拉取战绩失败:" + err);
		return;
	}
	consecutiveFetchFailures = 0;
	lastFetchAdded = added;

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

	{
		std::lock_guard<std::mutex> lock(mtx);
		accountDisplay = display;
		dLastReason = reason;
		dLastIncoming = incoming;
		dLastAdded = added;
		dFailStreak = 0;
		lcuConnected = true;
		if (newest)
			dNewestGameId = newest;
		if (dataChanged) {
			dLastRefreshMs = util::NowMs();
			status = std::string("已更新(") + reason + "):新增 " + std::to_string(added) + " 局 / 收到 " +
				 std::to_string(incoming) + " 局";
		} else if (manual) {
			status = "已连接 " + display + " · 手动刷新完成,无新增";
		} else {
			status = "已连接 " + display + " · 暂无新对局";
		}
	}
	if (dataChanged) {
		blog(LOG_INFO, "[lol-hexbar] history updated (%s): +%zu (incoming %zu), newest gameId=%llu", reason,
		     added, incoming, (unsigned long long)newest);
	}
}

void Monitor::MarkGameEnded(int64_t now, const char *source)
{
	if (!gameEndPending) {
		blog(LOG_INFO, "[lol-hexbar] game ended via %s (liveTime=%.0fs mode=%s) — fast-polling match history",
		     source, gameTimeS, gameMode.c_str());
		gameEndPending = true;
	}
	if (now + kFastWindowMs > fastPollUntilMs)
		fastPollUntilMs = now + kFastWindowMs;
	inGame = false;
	gameTimeS = 0;
	{
		std::lock_guard<std::mutex> lock(mtx);
		dInGame = false;
		dGameTimeS = 0;
		dSettlement = true;
		status = "对局已结束,正在等待战绩写入(高频拉取中)…";
	}
}

void Monitor::Tick(int64_t now)
{
	if (refreshRequested.exchange(false)) {
		if (!haveEndpoint)
			DiscoverAndConnect();
		if (haveEndpoint)
			FetchAll(true, "手动");
		lastFetchMs = now;
	}

	if (!haveEndpoint) {
		if (now - lastDiscoverMs > kDiscoverRetryMs) {
			lastDiscoverMs = now;
			DiscoverAndConnect();
			if (haveEndpoint) {
				FetchAll(false, "连接后首次");
				lastFetchMs = util::NowMs();
			}
		}
		return;
	}

	// ---- 1) gameflow 阶段轮询(2s):状态机 + 兜底触发 ----
	if (now - lastPhasePollMs > optPhasePollMs.load()) {
		lastPhasePollMs = now;
		LcuClient client(endpoint);
		nlohmann::json j;
		std::string err;
		if (client.GetJson("/lol-gameflow/v1/gameflow-phase", j, err) && j.is_string()) {
			consecutivePhaseFailures = 0;
			std::string phase = j.get<std::string>();
			bool wasGaming = IsGamingPhase(prevPhase);
			bool isGaming = IsGamingPhase(phase);
			if (phase != prevPhase)
				blog(LOG_INFO, "[lol-hexbar] phase: %s -> %s", prevPhase.c_str(), phase.c_str());
			DbgLog("phase poll: %s", phase.c_str());
			prevPhase = phase;
			// 进入结算屏 = 对局已结束、战绩即将写入:开追赶窗口
			if (phase == "WaitingForStats" || phase == "PreEndOfGame" || phase == "EndOfGame") {
				if (now + kFastWindowMs > fastPollUntilMs)
					fastPollUntilMs = now + kFastWindowMs;
			}
			if (phase == "InProgress") {
				fastPollUntilMs = 0; // 新对局开始,窗口自然结束
				gameEndPending = false;
			}
			if (wasGaming && !isGaming) {
				// 离开对局(含直接回大厅):立即拉一次;实时接口不可用时靠这里兜底
				MarkGameEnded(now, "gameflow");
				FetchAll(false, "离开对局");
				lastFetchMs = util::NowMs();
			}
			std::lock_guard<std::mutex> lock(mtx);
			lcuConnected = true;
			dPhase = prevPhase;
		} else {
			if (++consecutivePhaseFailures >= 3 && now - lastFetchMs > 15000) {
				blog(LOG_WARNING, "[lol-hexbar] gameflow-phase 连续失败(%s),重新发现客户端",
				     err.c_str());
				haveEndpoint = false;
				consecutivePhaseFailures = 0;
				std::lock_guard<std::mutex> lock(mtx);
				lcuConnected = false;
				dPhase.clear();
				dInGame = false;
				status = "客户端连接中断,正在重新发现…";
				return;
			}
		}
	}

	// ---- 2) 游戏内实时接口(1s,仅对局中):在游戏进程退出的瞬间捕捉"对局结束" ----
	bool phaseGaming = IsGamingPhase(prevPhase);
	if (phaseGaming && now - lastLivePollMs > optLivePollMs.load()) {
		lastLivePollMs = now;
		LiveGameInfo li = QueryLiveGame(1200);
		if (li.ok) {
			if (!inGame)
				blog(LOG_INFO, "[lol-hexbar] live game detected: mode=%s gameTime=%.0fs",
				     li.gameMode.c_str(), li.gameTimeS);
			inGame = true;
			gameTimeS = li.gameTimeS;
			gameMode = li.gameMode;
			{
				std::lock_guard<std::mutex> lock(mtx);
				dInGame = true;
				dGameTimeS = li.gameTimeS;
				dGameMode = li.gameMode;
				dSettlement = false;
				status = "对局中 · 游戏内 " + FmtClock(li.gameTimeS);
			}
		} else if (inGame) {
			// 游戏进程已退出 → 对局结束,立刻请求战绩
			blog(LOG_INFO, "[lol-hexbar] live API gone (%s) after %.0fs — game ended", li.err.c_str(),
			     gameTimeS);
			MarkGameEnded(now, "live-api");
			FetchAll(false, "对局结束");
			lastFetchMs = util::NowMs();
		}
	} else if (!phaseGaming && inGame) {
		MarkGameEnded(now, "phase-left");
	}

	// ---- 3) 战绩拉取节奏:结算追赶 2s / 对局中 5min 兜底 / 空闲 10s ----
	int64_t fetchGap;
	const char *reason;
	if (now < fastPollUntilMs) {
		fetchGap = optFastPollMs.load();
		reason = "结算追赶";
	} else if (phaseGaming) {
		fetchGap = optInGamePollMs.load();
		reason = "对局中兜底";
	} else {
		fetchGap = optIdlePollMs.load();
		reason = "空闲轮询";
	}
	{
		std::lock_guard<std::mutex> lock(mtx);
		dSettlement = now < fastPollUntilMs;
	}
	if (now - lastFetchMs > fetchGap) {
		lastFetchMs = now;
		FetchAll(false, reason);
	}

	// 追赶窗口内一旦看到新对局,再追一小段等数据落全,随后收窄窗口避免无谓轮询
	if (gameEndPending && lastFetchAdded > 0) {
		int64_t tail = util::NowMs() + kFastTailMs;
		if (tail < fastPollUntilMs)
			fastPollUntilMs = tail;
		gameEndPending = false;
		lastFetchAdded = 0;
	}

	// ---- 4) 海克斯/装备元数据 6 小时刷新 ----
	if (now - lastAugmentRefreshMs > 6 * 3600 * 1000LL) {
		lastAugmentRefreshMs = now;
		std::string err;
		assets.RefreshAugments(err);
		LcuClient client(endpoint);
		assets.RefreshItems(&client, err);
	}

	// ---- 5) 低频心跳:一眼看清当前状态 ----
	if (now - lastHeartbeatMs > kHeartbeatMs) {
		lastHeartbeatMs = now;
		DbgLog("heartbeat: phase=%s inGame=%d gameTime=%.0f endpoint=%d settlement=%d matches=%zu",
		       prevPhase.c_str(), inGame ? 1 : 0, gameTimeS, haveEndpoint ? 1 : 0,
		       now < fastPollUntilMs ? 1 : 0, data.MatchCount());
	}
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
		// 工作线程绝不允许因异常退出:任何单次异常都吞掉并记录,下一轮继续
		try {
			Tick(now);
		} catch (const std::exception &e) {
			blog(LOG_WARNING, "[lol-hexbar] tick exception: %s", e.what());
		} catch (...) {
			blog(LOG_WARNING, "[lol-hexbar] tick exception (unknown)");
		}

		// 退出前/定期落盘
		if (data.Dirty() && now - lastSaveMs > 3000) {
			lastSaveMs = now;
			std::lock_guard<std::mutex> lock(mtx);
			data.Save(dataFile);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(300));
	}

	CoUninitialize();
	blog(LOG_INFO, "[lol-hexbar] monitor thread stopped");
}

} // namespace hexbar
