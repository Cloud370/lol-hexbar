#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hexbar {

struct LcuEndpoint {
	int port = 0;
	std::string token;
	std::wstring installDir; // 指向 …\LeagueClient(仅用于缓存/展示)
	bool Valid() const { return port > 0 && !token.empty(); }
};

// 依次尝试:lockfile → 进程命令行(WMI)→ 客户端日志解析(国服主路径)。
// overrideDir 为用户在属性面板填写的客户端目录(可为安装根或 LeagueClient 目录)。
bool DiscoverLcu(const std::wstring &overrideDir, LcuEndpoint &out, std::string &diag);

// 用 gameflow-phase 验证端点可用性
bool ValidateLcuEndpoint(const LcuEndpoint &ep);

class LcuClient {
public:
	explicit LcuClient(const LcuEndpoint &ep) : ep(ep) {}

	bool GetJson(const std::string &pathUtf8, nlohmann::json &out, std::string &err) const;
	bool GetBinary(const std::string &pathUtf8, std::string &outBytes, std::string &err) const;

	const LcuEndpoint &endpoint() const { return ep; }

private:
	LcuEndpoint ep;
};

// 游戏内实时数据接口(Live Client Data API):
//   https://127.0.0.1:2999/liveclientdata/gamestats
// 仅在对局进行中存在(由游戏进程提供,随游戏退出立即消失),无需认证、自签名证书。
// 轮询它可获得对局是否进行中与游戏内时长,从而在游戏退出的瞬间触发战绩拉取。
struct LiveGameInfo {
	bool ok = false;
	std::string gameMode;
	double gameTimeS = 0;
	std::string err;
};

// timeoutMs 为接收超时;失败时 err 说明原因(端口未监听时通常是连接被拒)。
LiveGameInfo QueryLiveGame(int timeoutMs = 1200);

// WeGame/全球服常见安装目录候选(存在性由调用方检查)
std::vector<std::wstring> CandidateInstallDirs(const std::wstring &overrideDir);

} // namespace hexbar
