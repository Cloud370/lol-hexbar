#include "lcu.hpp"

#include <windows.h>
#include <objbase.h>
#include <wbemcli.h>

#include <algorithm>
#include <comdef.h>
#include <filesystem>
#include <functional>
#include <sstream>

#include "http.hpp"
#include "util.hpp"

#include <obs.h>

namespace hexbar {

namespace fs = std::filesystem;

// ---------------------------------------------------------------- 候选目录

static void AddIfHasLeagueClient(std::vector<std::wstring> &out, const std::wstring &dir)
{
	if (dir.empty())
		return;
	std::error_code ec;
	if (fs::exists(dir + L"\\LeagueClient.exe", ec))
		out.push_back(dir);
}

std::vector<std::wstring> CandidateInstallDirs(const std::wstring &overrideDir)
{
	std::vector<std::wstring> out;

	if (!overrideDir.empty()) {
		// 用户给的可能是安装根、或直接是 LeagueClient 目录
		std::error_code ec;
		if (fs::exists(overrideDir + L"\\LeagueClient.exe", ec)) {
			out.push_back(overrideDir);
		} else {
			std::wstring sub = overrideDir + (overrideDir.back() == L'\\' ? L"" : L"\\") + L"LeagueClient";
			if (fs::exists(sub + L"\\LeagueClient.exe", ec))
				out.push_back(sub);
		}
	}

	// 注册表:国服 WeGame 安装位置
	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Tencent\\LOL", 0, KEY_READ, &key) ==
	    ERROR_SUCCESS) {
		wchar_t buf[MAX_PATH] = {};
		DWORD len = MAX_PATH, type = 0;
		if (RegQueryValueExW(key, L"InstallPath", nullptr, &type, (LPBYTE)buf, &len) == ERROR_SUCCESS &&
		    type == REG_SZ) {
			std::wstring install(buf);
			while (!install.empty() && (install.back() == L'\\' || install.back() == L'/'))
				install.pop_back();
			AddIfHasLeagueClient(out, install + L"\\LeagueClient");
		}
		RegCloseKey(key);
	}

	// 盘符扫描固定常见布局
	wchar_t drives[256] = {};
	DWORD n = GetLogicalDriveStringsW(256, drives);
	const wchar_t *candidates[] = {L"WeGameApps\\英雄联盟\\LeagueClient", L"WeGame\\games\\英雄联盟\\LeagueClient",
				       L"Riot Games\\League of Legends\\LeagueClient",
				       L"Program Files\\Riot Games\\League of Legends\\LeagueClient"};
	for (DWORD i = 0; i + 3 < n + 1 && drives[i]; i += 4) {
		if (GetDriveTypeW(drives + i) != DRIVE_FIXED)
			continue;
		std::wstring root(drives + i);
		for (auto rel : candidates)
			AddIfHasLeagueClient(out, root + rel);
	}

	// 去重保持顺序
	std::vector<std::wstring> uniq;
	for (auto &d : out)
		if (std::find(uniq.begin(), uniq.end(), d) == uniq.end())
			uniq.push_back(d);
	return uniq;
}

// ---------------------------------------------------------------- lockfile

static bool TryLockfile(const std::wstring &leagueClientDir, LcuEndpoint &out)
{
	std::wstring lock = leagueClientDir + L"\\lockfile";
	std::string content;
	if (!util::ReadFileBytes(lock, content, 1024) || content.empty())
		return false;
	// 格式:LeagueClient:pid:port:token:protocol(以 NUL 结尾或纯文本)
	content.push_back('\n');
	auto parts = util::Split(content.substr(0, content.find('\n')), ':');
	if (parts.size() < 5)
		return false;
	try {
		out.port = std::stoi(parts[2]);
	} catch (...) {
		return false;
	}
	out.token = parts[3];
	out.installDir = leagueClientDir;
	return out.Valid();
}

// ---------------------------------------------------------------- 进程命令行(WMI)

static bool TryProcessCommandLine(LcuEndpoint &out, std::string &diag)
{
	bool found = false;
	IWbemLocator *locator = nullptr;
	IWbemServices *services = nullptr;
	IEnumWbemClassObject *enumerator = nullptr;

	HRESULT hr =
		CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void **)&locator);
	if (FAILED(hr)) {
		diag = "WMI locator failed";
		return false;
	}
	hr = locator->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
	if (SUCCEEDED(hr)) {
		CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
				  RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
		hr = services->ExecQuery(
			_bstr_t(L"WQL"),
			_bstr_t(L"SELECT CommandLine FROM Win32_Process WHERE Name='LeagueClientUx.exe'"),
			WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator);
		if (SUCCEEDED(hr)) {
			IWbemClassObject *obj = nullptr;
			ULONG returned = 0;
			while (enumerator->Next(WBEM_INFINITE, 1, &obj, &returned) == S_OK && returned == 1) {
				_variant_t cmd;
				if (obj->Get(L"CommandLine", 0, &cmd, nullptr, nullptr) == S_OK && cmd.vt == VT_BSTR &&
				    cmd.bstrVal) {
					std::string cmdline = util::WideToUtf8(cmd.bstrVal);
					std::string token = util::ExtractFlagValue(cmdline, "--remoting-auth-token=");
					std::string port = util::ExtractDigitsAfter(cmdline, "--app-port=");
					if (!token.empty() && !port.empty()) {
						try {
							out.port = std::stoi(port);
							out.token = token;
							found = true;
						} catch (...) {
						}
					}
				}
				obj->Release();
				if (found)
					break;
			}
		}
	}
	if (enumerator)
		enumerator->Release();
	if (services)
		services->Release();
	if (locator)
		locator->Release();
	if (!found)
		diag = "no readable LeagueClientUx command line";
	return found;
}

// ---------------------------------------------------------------- 客户端日志解析(国服主路径)

static bool TryClientLog(const std::wstring &leagueClientDir, LcuEndpoint &out)
{
	std::error_code ec;
	fs::path dir(leagueClientDir);
	if (!fs::exists(dir, ec))
		return false;

	// 取最新的 *_LeagueClientUx.log 或 debug.log
	fs::path best;
	fs::file_time_type bestTime = fs::file_time_type::min();
	for (auto &entry : fs::directory_iterator(dir, ec)) {
		if (ec)
			break;
		if (!entry.is_regular_file(ec))
			continue;
		auto name = entry.path().filename().wstring();
		bool isUxLog = name.size() > 20 && name.rfind(L"_LeagueClientUx.log") != std::wstring::npos;
		bool isDebug = _wcsicmp(name.c_str(), L"debug.log") == 0;
		if (!isUxLog && !isDebug)
			continue;
		auto t = entry.last_write_time(ec);
		if (ec)
			continue;
		if (t > bestTime) {
			bestTime = t;
			best = entry.path();
		}
	}
	if (best.empty())
		return false;

	std::string content;
	// Ux 日志头部就带完整参数;debug.log 可能很大,只读前 256KB
	if (!util::ReadFileBytes(best.wstring(), content, 256 * 1024))
		return false;
	if (content.size() > 128 * 1024)
		content.resize(128 * 1024);

	std::string token = util::ExtractFlagValue(content, "--remoting-auth-token=");
	std::string port = util::ExtractDigitsAfter(content, "--app-port=");
	if (token.empty() || port.empty())
		return false;
	try {
		out.port = std::stoi(port);
	} catch (...) {
		return false;
	}
	out.token = token;
	out.installDir = leagueClientDir;
	return out.Valid();
}

// ---------------------------------------------------------------- 验证与 REST

bool ValidateLcuEndpoint(const LcuEndpoint &ep)
{
	std::wstring url = L"https://127.0.0.1:" + std::to_wstring(ep.port) + L"/lol-gameflow/v1/gameflow-phase";
	std::string auth = "Basic " + util::B64Encode("riot:" + ep.token);
	HttpResponse resp;
	std::string err;
	if (!Http::Get(url, auth, true, 4000, 64 * 1024, resp, err))
		return false;
	return resp.Ok();
}

bool LcuClient::GetJson(const std::string &pathUtf8, nlohmann::json &out, std::string &err) const
{
	HttpResponse resp;
	if (!GetBinary(pathUtf8, resp.body, err))
		return false;
	try {
		out = nlohmann::json::parse(resp.body);
	} catch (const std::exception &e) {
		err = std::string("json parse: ") + e.what();
		return false;
	}
	return true;
}

bool LcuClient::GetBinary(const std::string &pathUtf8, std::string &outBytes, std::string &err) const
{
	std::wstring url = L"https://127.0.0.1:" + std::to_wstring(ep.port) + util::Utf8ToWide(pathUtf8);
	std::string auth = "Basic " + util::B64Encode("riot:" + ep.token);
	HttpResponse resp;
	if (!Http::Get(url, auth, true, 6000, 16 * 1024 * 1024, resp, err))
		return false;
	if (!resp.Ok()) {
		err = "HTTP " + std::to_string(resp.status);
		return false;
	}
	outBytes = std::move(resp.body);
	return true;
}

LiveGameInfo QueryLiveGame(int timeoutMs)
{
	LiveGameInfo info;
	HttpResponse resp;
	// 官方 Live Client Data API:HTTPS + 自签名证书,无 Basic 认证。
	// quiet=true:对局外该端口通常未监听,避免每次探测都往 OBS 日志刷失败行。
	if (!Http::Get(L"https://127.0.0.1:2999/liveclientdata/gamestats", "", true, timeoutMs, 512 * 1024, resp,
		       info.err, true))
		return info;
	if (!resp.Ok()) {
		info.err = "HTTP " + std::to_string(resp.status);
		return info;
	}
	try {
		auto j = nlohmann::json::parse(resp.body);
		info.gameMode = j.value("gameMode", "");
		if (j.contains("gameTime") && j["gameTime"].is_number())
			info.gameTimeS = j["gameTime"].get<double>();
		info.ok = true;
	} catch (const std::exception &e) {
		info.err = std::string("json parse: ") + e.what();
	}
	return info;
}

bool DiscoverLcu(const std::wstring &overrideDir, LcuEndpoint &out, std::string &diag)
{
	std::ostringstream d;

	auto dirs = CandidateInstallDirs(overrideDir);
	blog(LOG_INFO, "[lol-hexbar] discovery: %zu candidate dir(s)", dirs.size());
	for (auto &dir : dirs)
		blog(LOG_INFO, "[lol-hexbar]   candidate: %s", util::WideToUtf8(dir).c_str());

	for (auto &dir : dirs) {
		LcuEndpoint ep;
		if (TryLockfile(dir, ep)) {
			blog(LOG_INFO, "[lol-hexbar] lockfile hit: %s port=%d", util::WideToUtf8(dir).c_str(), ep.port);
			if (ValidateLcuEndpoint(ep)) {
				out = ep;
				diag = "lockfile@" + util::WideToUtf8(dir);
				return true;
			}
			blog(LOG_INFO, "[lol-hexbar] lockfile endpoint invalid");
		}
	}

	{
		LcuEndpoint ep;
		std::string wmiDiag;
		if (TryProcessCommandLine(ep, wmiDiag)) {
			blog(LOG_INFO, "[lol-hexbar] process cmdline hit: port=%d", ep.port);
			if (ValidateLcuEndpoint(ep)) {
				out = ep;
				diag = "process-cmdline";
				return true;
			}
			blog(LOG_INFO, "[lol-hexbar] cmdline endpoint invalid");
		} else {
			blog(LOG_INFO, "[lol-hexbar] process cmdline unavailable: %s", wmiDiag.c_str());
		}
	}

	for (auto &dir : dirs) {
		LcuEndpoint ep;
		if (TryClientLog(dir, ep)) {
			blog(LOG_INFO, "[lol-hexbar] client log hit: %s port=%d token=%s...",
			     util::WideToUtf8(dir).c_str(), ep.port, ep.token.substr(0, 4).c_str());
			if (ValidateLcuEndpoint(ep)) {
				out = ep;
				diag = "client-log@" + util::WideToUtf8(dir);
				return true;
			}
			blog(LOG_INFO, "[lol-hexbar] client log endpoint invalid (port=%d)", ep.port);
		} else {
			blog(LOG_INFO, "[lol-hexbar] client log miss: %s", util::WideToUtf8(dir).c_str());
		}
	}

	diag = "client not found (lockfile/cmdline/log all failed)";
	return false;
}

} // namespace hexbar
