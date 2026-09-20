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

// WeGame/全球服常见安装目录候选(存在性由调用方检查)
std::vector<std::wstring> CandidateInstallDirs(const std::wstring &overrideDir);

} // namespace hexbar
