#include "webserver.hpp"

// WinSock 必须先于可能引入 windows.h 的头
#include <winsock2.h>
#include <ws2tcpip.h>

#include <obs-module.h>
#include <obs.h>
#include <obs-frontend-api.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "monitor.hpp"
#include "util.hpp"
#include "webfiles.hpp"

namespace hexbar {

namespace {

constexpr int kMaxHeaderBytes = 32 * 1024;
constexpr int kMaxBodyBytes = 1024 * 1024;
constexpr int kMaxActiveConns = 32;

struct Request {
	std::string method;
	std::string path; // 不含 query
	std::string query;
	int httpMinor = 1;
	std::map<std::string, std::string> headers; // 小写键
	std::string body;
	bool keepAlive = true;
};

struct Response {
	int status = 200;
	const char *reason = "OK";
	std::string contentType = "application/json; charset=utf-8";
	std::string cacheControl;
	std::string body;

	void SetHtml(std::string s)
	{
		contentType = "text/html; charset=utf-8";
		cacheControl = "no-store"; // 页面文件可热改,浏览器源/预览刷新时取最新
		body = std::move(s);
	}
};

std::string Trim(std::string s)
{
	auto notSpace = [](unsigned char c) {
		return !std::isspace(c);
	};
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
	s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
	return s;
}

// 国服大区名(与 LeagueAkari platform-names 同源的公开映射)
static const std::map<std::string, std::string> &PlatformNames()
{
	static const std::map<std::string, std::string> m = {
		{"HN1", "艾欧尼亚"},   {"HN2", "祖安"},         {"HN3", "诺克萨斯"},   {"HN4", "班德尔城"},
		{"HN5", "皮尔特洛夫"}, {"HN6", "战争学院"},     {"HN7", "巨神峰"},     {"HN8", "雷瑟守备"},
		{"HN9", "裁决之地"},   {"HN10", "黑色玫瑰"},    {"HN11", "暗影岛"},    {"HN12", "钢铁烈阳"},
		{"HN13", "水晶之痕"},  {"HN14", "均衡教派"},    {"HN15", "影流"},      {"HN16", "守望之海"},
		{"HN17", "征服之海"},  {"HN18", "卡拉曼达"},    {"HN19", "皮城警备"},  {"BGP1", "男爵领域"},
		{"BGP2", "峡谷之巅"},  {"WT1", "比尔吉沃特"},   {"WT2", "德玛西亚"},   {"WT3", "弗雷尔卓德"},
		{"WT4", "无畏先锋"},   {"WT5", "恕瑞玛"},       {"WT6", "扭曲丛林"},   {"WT7", "巨龙之巢"},
		{"EDU1", "教育网"},    {"PBE", "体验服"},       {"NJ100", "联盟一区"}, {"GZ100", "联盟二区"},
		{"CQ100", "联盟三区"}, {"TJ100", "联盟四区"},   {"TJ101", "联盟五区"}, {"NA", "北美"},
		{"EUW1", "西欧"},      {"EUN1", "北欧 & 东欧"}, {"KR", "韩国"},        {"JP1", "日本"},
		{"BR1", "巴西"},       {"LA1", "拉丁美洲南"},   {"LA2", "拉丁美洲北"}, {"RU1", "俄罗斯"},
		{"TR1", "土耳其"},     {"OC1", "大洋洲"},       {"TW1", "台湾"},       {"TW2", "台湾"},
		{"SG1", "新加坡"},     {"SG2", "新加坡"},       {"TH1", "泰国"},       {"TH2", "泰国"},
		{"VN1", "越南"},       {"VN2", "越南"},         {"PH1", "菲律宾"},     {"PH2", "菲律宾"},
	};
	return m;
}

static std::string RegionDisplayName(const std::string &platformId)
{
	auto &m = PlatformNames();
	auto it = m.find(platformId);
	return it != m.end() ? it->second : platformId;
}

} // namespace

struct WebServer::Impl : std::enable_shared_from_this<Impl> {
	std::wstring cfgDir;
	std::wstring settingsFile;

	std::recursive_mutex mtx; // 保护 app
	AppSettings app;

	std::mutex restartMtx; // 序列化监听器重启
	std::thread acceptThread;
	std::atomic<bool> running{false};
	std::atomic<int> listenPort{0};
	std::atomic<uintptr_t> listenSock{0}; // Stop/Restart 关闭以解除 accept 阻塞

	std::mutex connMtx;
	std::condition_variable connCv;
	int activeConns = 0;
	std::vector<SOCKET> liveSocks; // Stop 时逐个 shutdown 以解除阻塞

	std::thread retryThread; // 绑定失败后的自愈重试(直播中不能重启 OBS)
	std::mutex retryMtx;
	std::condition_variable retryCv; // Stop 用以立刻唤醒重试等待
	std::atomic<bool> retryAlive{false};

	bool StartListener(int port, std::string &err);
	void SpawnRetry();
	void RetryLoop();
	void AcceptLoop(int port, std::promise<int> bound);
	void ClientLoop(SOCKET s);
	void Dispatch(const Request &req, Response &res);

	void LoadSettings();
	bool SaveSettings();
	nlohmann::json SettingsJson();
	void HandleSettingsPost(const Request &req, Response &res);
	void HandleSnapshot(Response &res);
	void HandleStatus(Response &res);
	void HandleScenes(Response &res);
	void HandleAddSource(const Request &req, Response &res);
	void HandleAsset(const Request &req, Response &res);
};

// ---------------------------------------------------------------- 设置持久化

static AppSettings JsonToSettings(const nlohmann::json &j)
{
	AppSettings s;
	s.bar.batchGapMin = std::clamp(j.value("batchGapMin", 100000), 1, 100000);
	s.bar.maxGames = std::clamp(j.value("maxGames", 20), 1, 100);
	int mode = j.value("modeFilter", 0);
	s.bar.modeFilter = (mode == 1) ? 1 : 0;
	s.bar.clientDir = util::Utf8ToWide(j.value("clientDir", ""));
	s.httpPort = std::clamp(j.value("httpPort", 35712), 1024, 65535);
	s.firstRunDone = j.value("firstRunDone", false);

	if (j.contains("bar") && j["bar"].is_object()) {
		auto &b = j["bar"];
		s.barPanel.enabled = b.value("enabled", true);
		s.barPanel.showTotal = b.value("showTotal", true);
		s.barPanel.showKda = b.value("showKda", true);
		s.barPanel.spacing = std::clamp(b.value("spacing", 5), 0, 40);
		s.barPanel.w = std::clamp(b.value("w", 1440), 50, 32768);
		s.barPanel.h = std::clamp(b.value("h", 110), 50, 16384);
	}
	if (j.contains("side") && j["side"].is_object()) {
		auto &d = j["side"];
		s.sidePanel.enabled = d.value("enabled", true);
		s.sidePanel.showHexes = d.value("showHexes", true);
		s.sidePanel.showItems = d.value("showItems", true);
		s.sidePanel.showKda = d.value("showKda", true);
		s.sidePanel.showNames = d.value("showNames", true);
		s.sidePanel.iconSize = std::clamp(d.value("iconSize", 24), 8, 96);
		s.sidePanel.rows = std::clamp(d.value("rows", 3), 1, 100);
		s.sidePanel.scrollSec = std::clamp(d.value("scrollSec", 10), 0, 3600);
		s.sidePanel.scrollMs = std::clamp(d.value("scrollMs", 1500), 0, 5000);
		s.sidePanel.radius = std::clamp(d.value("radius", 4), 0, 40);
		s.sidePanel.opacity = std::clamp(d.value("opacity", 95), 10, 100);
		s.sidePanel.hexCols = std::clamp(d.value("hexCols", 2), 1, 6);
		s.sidePanel.w = std::clamp(d.value("w", 600), 50, 32768);
		s.sidePanel.h = std::clamp(d.value("h", 690), 50, 32768);
	}
	return s;
}

nlohmann::json WebServer::Impl::SettingsJson()
{
	std::lock_guard<std::recursive_mutex> lock(mtx);
	nlohmann::json j;
	j["batchGapMin"] = app.bar.batchGapMin;
	j["maxGames"] = app.bar.maxGames;
	j["modeFilter"] = app.bar.modeFilter;
	j["clientDir"] = util::WideToUtf8(app.bar.clientDir);
	j["httpPort"] = app.httpPort;
	j["bar"] = {
		{"enabled", app.barPanel.enabled},
		{"showTotal", app.barPanel.showTotal},
		{"showKda", app.barPanel.showKda},
		{"spacing", app.barPanel.spacing},
		{"w", app.barPanel.w},
		{"h", app.barPanel.h},
	};
	j["side"] = {
		{"enabled", app.sidePanel.enabled},
		{"showHexes", app.sidePanel.showHexes},
		{"showItems", app.sidePanel.showItems},
		{"showKda", app.sidePanel.showKda},
		{"iconSize", app.sidePanel.iconSize},
		{"rows", app.sidePanel.rows},
		{"scrollSec", app.sidePanel.scrollSec},
		{"scrollMs", app.sidePanel.scrollMs},
		{"radius", app.sidePanel.radius},
		{"opacity", app.sidePanel.opacity},
		{"hexCols", app.sidePanel.hexCols},
		{"showNames", app.sidePanel.showNames},
		{"w", app.sidePanel.w},
		{"h", app.sidePanel.h},
	};
	return j;
}

void WebServer::Impl::LoadSettings()
{
	std::lock_guard<std::recursive_mutex> lock(mtx);
	std::string content;
	if (util::ReadFileBytes(settingsFile, content)) {
		try {
			app = JsonToSettings(nlohmann::json::parse(content));
		} catch (...) {
			// 损坏则用默认
		}
	}
}

bool WebServer::Impl::SaveSettings()
{
	std::string out;
	{
		std::lock_guard<std::recursive_mutex> lock(mtx);
		nlohmann::json j = SettingsJson();
		j["firstRunDone"] = app.firstRunDone;
		out = j.dump(1);
	}
	return util::WriteFileBytesAtomic(settingsFile, out.data(), out.size());
}

// ---------------------------------------------------------------- 生命周期

WebServer &WebServer::Instance()
{
	static WebServer inst;
	return inst;
}

bool WebServer::Start(const std::wstring &configDir)
{
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;

	auto p = std::make_shared<Impl>();
	p->cfgDir = configDir;
	p->settingsFile = configDir + L"\\settings.json";
	// 页面文件来源就绪(开发目录 / 安装 data 目录 / 解压内嵌资源)
	webfiles::Init(configDir);
	p->LoadSettings();
	p->running = true;
	impl = p;

	std::string err;
	int port;
	{
		std::lock_guard<std::recursive_mutex> lock(p->mtx);
		port = p->app.httpPort;
	}
	if (!p->StartListener(port, err)) {
		blog(LOG_WARNING, "[lol-hexbar] http listen on 127.0.0.1:%d failed: %s", port, err.c_str());
		// 服务起不来不影响数据采集,保持 running 以便 Stop 清理;
		// 端口常被双开 OBS / 残留进程占用,直播中无法重启 OBS,交给自愈重试
		p->SpawnRetry();
	}
	return true;
}

void WebServer::Stop()
{
	if (!impl)
		return;
	auto p = impl;
	impl.reset();

	p->running = false;
	p->retryCv.notify_all(); // 唤醒自愈重试,避免 Stop 等满一轮间隔
	if (p->retryThread.joinable())
		p->retryThread.join();
	{
		// 与自愈重试/手动重启串行,防止关闭期间又被重新绑定
		std::lock_guard<std::mutex> lock(p->restartMtx);
		{
			std::lock_guard<std::mutex> lc(p->connMtx);
			for (SOCKET s : p->liveSocks)
				::shutdown(s, SD_BOTH);
		}
		uintptr_t ls = p->listenSock.exchange(0);
		if (ls)
			closesocket((SOCKET)ls); // 解除 accept 阻塞
		if (p->acceptThread.joinable())
			p->acceptThread.join();
	}
	{
		std::unique_lock<std::mutex> lock(p->connMtx);
		p->connCv.wait_for(lock, std::chrono::seconds(3), [&] { return p->activeConns == 0; });
	}
	WSACleanup();
}

bool WebServer::RestartListener(int port, std::string &err)
{
	if (!impl)
		return false;
	auto p = impl;
	std::lock_guard<std::mutex> rl(p->restartMtx);

	// 关闭监听套接字使 accept 线程退出后重新绑定
	uintptr_t ls = p->listenSock.exchange(0);
	if (ls)
		closesocket((SOCKET)ls);
	if (p->acceptThread.joinable())
		p->acceptThread.join();
	if (!p->StartListener(port, err)) {
		// 新端口起不来时保持 running,由自愈线程接管重试
		p->SpawnRetry();
		return false;
	}
	return true;
}

// ---------------------------------------------------------------- 绑定失败自愈

void WebServer::Impl::SpawnRetry()
{
	std::lock_guard<std::mutex> lock(retryMtx);
	if (retryAlive.exchange(true))
		return; // 自愈线程已在运行
	if (retryThread.joinable())
		retryThread.join(); // 上一轮已结束,收尸后复用
	retryThread = std::thread([self = shared_from_this()]() { self->RetryLoop(); });
}

// 每 5 秒重试绑定当前设置端口,成功或服务停止才退出。
// 典型场景:启动时端口被双开的 OBS / 残留进程占用,对方退出后服务自动恢复,
// 全程无需重启 OBS(直播中不可行)。
void WebServer::Impl::RetryLoop()
{
	int nth = 0;
	while (running.load() && listenPort.load() == 0) {
		{
			std::unique_lock<std::mutex> lock(retryMtx);
			retryCv.wait_for(lock, std::chrono::seconds(5), [&] { return !running.load(); });
		}
		if (!running.load() || listenPort.load() != 0)
			break;
		int port;
		{
			std::lock_guard<std::recursive_mutex> lock(mtx);
			port = app.httpPort;
		}
		std::string err;
		{
			std::lock_guard<std::mutex> rl(restartMtx);
			if (!running.load() || listenPort.load() != 0)
				break;
			if (StartListener(port, err)) {
				blog(LOG_INFO, "[lol-hexbar] http server self-healed on 127.0.0.1:%d", port);
				break;
			}
		}
		if (++nth % 12 == 1) // 首次与每分钟各一条,不刷爆 OBS 日志
			blog(LOG_WARNING, "[lol-hexbar] http listen retry on 127.0.0.1:%d failed: %s", port,
			     err.c_str());
	}
	std::lock_guard<std::mutex> lock(retryMtx);
	retryAlive = false;
}

AppSettings WebServer::SettingsCopy()
{
	if (!impl)
		return AppSettings{};
	std::lock_guard<std::recursive_mutex> lock(impl->mtx);
	return impl->app;
}

void WebServer::MarkFirstRunDone()
{
	if (!impl)
		return;
	auto p = impl;
	{
		std::lock_guard<std::recursive_mutex> lock(p->mtx);
		p->app.firstRunDone = true;
	}
	p->SaveSettings();
}

std::string WebServer::OverlayUrl()
{
	int port = 35712;
	if (impl) {
		std::lock_guard<std::recursive_mutex> lock(impl->mtx);
		port = impl->app.httpPort;
	}
	return "http://127.0.0.1:" + std::to_string(port) + "/overlay";
}

std::string WebServer::SettingsUrl()
{
	int port = 35712;
	if (impl) {
		std::lock_guard<std::recursive_mutex> lock(impl->mtx);
		port = impl->app.httpPort;
	}
	return "http://127.0.0.1:" + std::to_string(port) + "/";
}

// ---------------------------------------------------------------- 监听与连接

bool WebServer::Impl::StartListener(int port, std::string &err)
{
	// promise 值 = 0 成功,否则为 AcceptLoop 线程内的 WSAGetLastError
	// (错误码必须当场取:WSA 错误是每线程独立的)
	std::promise<int> bound;
	std::future<int> fut = bound.get_future();
	acceptThread = std::thread([this, port, &bound]() mutable { AcceptLoop(port, std::move(bound)); });
	int bindErr = fut.get();
	if (bindErr != 0) {
		if (acceptThread.joinable())
			acceptThread.join();
		err = "bind 127.0.0.1:" + std::to_string(port) + " 失败,WSAError=" + std::to_string(bindErr) +
		      "(10048=端口被占用)";
		listenPort = 0;
	} else {
		listenPort = port;
	}
	return bindErr == 0;
}

void WebServer::Impl::AcceptLoop(int port, std::promise<int> bound)
{
	SOCKET lsock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (lsock == INVALID_SOCKET) {
		bound.set_value(WSAGetLastError());
		return;
	}
	// 独占绑定:Windows 上 SO_REUSEADDR 允许强行绑到他人占用的端口,
	// 结果"绑定成功但连接被对方收走",页面打不开且日志无错——客户端口
	// 无法访问的一种来源。EXCLUSIVEADDRUSE 让冲突变成显式 bind 失败,
	// 由自愈重试接管。代价:快速重绑同端口可能撞 TIME_WAIT,同样交给重试。
	BOOL exclusive = TRUE;
	setsockopt(lsock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u_short)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(lsock, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR || listen(lsock, 16) == SOCKET_ERROR) {
		int e = WSAGetLastError();
		closesocket(lsock);
		bound.set_value(e);
		return;
	}
	bound.set_value(0);
	listenSock = (uintptr_t)lsock;
	blog(LOG_INFO, "[lol-hexbar] overlay http server listening on 127.0.0.1:%d", port);

	auto self = shared_from_this(); // 由 WebServer::impl 共享,防止停止期间悬空
	while (running.load()) {
		sockaddr_in peer{};
		int plen = sizeof(peer);
		SOCKET c = accept(lsock, (sockaddr *)&peer, &plen);
		if (c == INVALID_SOCKET) {
			// 监听套接字被 Stop/Restart 关闭才退出;其余(资源紧张等)
			// 视为瞬时错误,稍等重试,不能让服务无声死掉
			if (!running.load() || listenSock.load() == 0)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}
		{
			std::lock_guard<std::mutex> lock(connMtx);
			if (activeConns >= kMaxActiveConns) {
				closesocket(c);
				continue;
			}
			++activeConns;
			liveSocks.push_back(c);
		}
		std::thread([self, c]() { self->ClientLoop(c); }).detach();
	}
	// 仅当未被 Stop/Restart 抢先关闭时才清理标记(对方已置 0)
	uintptr_t mine = (uintptr_t)lsock;
	listenSock.compare_exchange_strong(mine, 0);
	closesocket(lsock);
	listenPort = 0;
}

void WebServer::Impl::ClientLoop(SOCKET s)
{
	DWORD tmo = 10000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof(tmo));
	int nodelay = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));

	std::string buf;
	while (running.load()) {
		// 读取完整请求头
		size_t headEnd;
		while ((headEnd = buf.find("\r\n\r\n")) == std::string::npos) {
			if (buf.size() > kMaxHeaderBytes) {
				closesocket(s);
				goto done;
			}
			char tmp[8192];
			int n = recv(s, tmp, sizeof(tmp), 0);
			if (n <= 0)
				goto done;
			buf.append(tmp, (size_t)n);
		}

		{
			Request req;
			std::string head = buf.substr(0, headEnd);

			// 请求行
			size_t lineEnd = head.find("\r\n");
			std::string reqLine = head.substr(0, lineEnd == std::string::npos ? head.size() : lineEnd);
			{
				size_t sp1 = reqLine.find(' ');
				size_t sp2 = reqLine.rfind(' ');
				if (sp1 == std::string::npos || sp2 == sp1) {
					closesocket(s);
					goto done;
				}
				req.method = reqLine.substr(0, sp1);
				std::string target = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);
				std::string ver = reqLine.substr(sp2 + 1);
				req.httpMinor = (ver == "HTTP/1.0") ? 0 : 1;
				size_t q = target.find('?');
				if (q != std::string::npos) {
					req.path = target.substr(0, q);
					req.query = target.substr(q + 1);
				} else {
					req.path = target;
				}
			}

			// 头字段
			size_t pos = (lineEnd == std::string::npos) ? head.size() : lineEnd + 2;
			while (pos < head.size()) {
				size_t e = head.find("\r\n", pos);
				if (e == std::string::npos)
					e = head.size();
				std::string line = head.substr(pos, e - pos);
				pos = e + 2;
				size_t col = line.find(':');
				if (col == std::string::npos)
					continue;
				std::string k = line.substr(0, col);
				std::transform(k.begin(), k.end(), k.begin(), ::tolower);
				req.headers[k] = Trim(line.substr(col + 1));
			}

			size_t contentLen = 0;
			auto it = req.headers.find("content-length");
			if (it != req.headers.end())
				contentLen = (size_t)strtoull(it->second.c_str(), nullptr, 10);
			if (contentLen > (size_t)kMaxBodyBytes) {
				closesocket(s);
				goto done;
			}

			size_t need = headEnd + 4 + contentLen;
			while (buf.size() < need) {
				char tmp[8192];
				int n = recv(s, tmp, sizeof(tmp), 0);
				if (n <= 0) {
					closesocket(s);
					goto done;
				}
				buf.append(tmp, (size_t)n);
			}
			req.body = buf.substr(headEnd + 4, contentLen);
			buf.erase(0, need); // 可能的流水线数据留给下一轮

			auto conn = req.headers.find("connection");
			std::string connVal = conn == req.headers.end() ? "" : conn->second;
			std::transform(connVal.begin(), connVal.end(), connVal.begin(), ::tolower);
			req.keepAlive = (req.httpMinor == 1) ? (connVal != "close") : (connVal == "keep-alive");

			Response res;
			try {
				Dispatch(req, res);
			} catch (...) {
				// 本线程 detached,异常逃逸会 std::terminate 直接带走 OBS;
				// 单个请求处理失败只影响该请求
				res = Response{};
				res.status = 500;
				res.reason = "Internal Server Error";
				res.body = "{\"error\":\"internal error\"}";
			}

			std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + res.reason + "\r\n";
			out += "Content-Type: " + res.contentType + "\r\n";
			out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
			out += "Connection: " + std::string(req.keepAlive ? "keep-alive" : "close") + "\r\n";
			if (!res.cacheControl.empty())
				out += "Cache-Control: " + res.cacheControl + "\r\n";
			out += "\r\n";
			out += res.body;

			const char *p = out.data();
			size_t left = out.size();
			bool sendFail = false;
			while (left > 0) {
				int n = send(s, p, (int)left, 0);
				if (n <= 0) {
					sendFail = true;
					break;
				}
				p += n;
				left -= (size_t)n;
			}
			if (sendFail || !req.keepAlive) {
				closesocket(s);
				goto done;
			}
		}
	}
	closesocket(s);
done: {
	std::lock_guard<std::mutex> lock(connMtx);
	--activeConns;
	auto it = std::find(liveSocks.begin(), liveSocks.end(), s);
	if (it != liveSocks.end())
		liveSocks.erase(it);
}
	connCv.notify_all();
}

// ---------------------------------------------------------------- 路由

void WebServer::Impl::Dispatch(const Request &req, Response &res)
{
	bool isGet = req.method == "GET";
	bool isPost = req.method == "POST";

	// 页面内容每次从磁盘现读(data/web 或解压副本),支持不重编热改
	auto HtmlFile = [](const char *name, Response &res) {
		std::string html;
		if (webfiles::Read(name, html)) {
			res.SetHtml(std::move(html));
			return true;
		}
		res.status = 500;
		res.reason = "Internal Server Error";
		res.body = "{\"error\":\"page file missing: " + std::string(name) + "\"}";
		return false;
	};

	if (isGet && req.path == "/") {
		HtmlFile("settings.html", res);
	} else if (isGet && (req.path == "/overlay" || req.path == "/bar" || req.path == "/side")) {
		HtmlFile("overlay.html", res);
	} else if (isGet && req.path == "/api/snapshot") {
		HandleSnapshot(res);
	} else if (isGet && req.path == "/api/status") {
		HandleStatus(res);
	} else if (isGet && req.path == "/api/settings") {
		res.body = SettingsJson().dump();
	} else if (isPost && req.path == "/api/settings") {
		HandleSettingsPost(req, res);
	} else if (isPost && req.path == "/api/refresh") {
		Monitor::Instance().RequestRefresh();
		res.body = "{\"ok\":true}";
	} else if (isPost && req.path == "/api/new-batch") {
		Monitor::Instance().RequestNewBatch();
		res.body = "{\"ok\":true}";
	} else if (isGet && req.path == "/api/scenes") {
		HandleScenes(res);
	} else if (isPost && req.path == "/api/add-source") {
		HandleAddSource(req, res);
	} else if (isGet && req.path.rfind("/asset/", 0) == 0) {
		HandleAsset(req, res);
	} else {
		res.status = 404;
		res.reason = "Not Found";
		res.body = "{\"error\":\"not found\"}";
	}

	if (req.path.rfind("/api/", 0) == 0)
		res.cacheControl = "no-store";
}

void WebServer::Impl::HandleSnapshot(Response &res)
{
	Monitor &mon = Monitor::Instance();
	Snapshot snap = mon.SnapshotCopy();
	AppSettings st;
	{
		std::lock_guard<std::recursive_mutex> lock(mtx);
		st = app;
	}

	nlohmann::json j;
	j["epoch"] = snap.epoch;
	j["wins"] = snap.wins;
	j["losses"] = snap.losses;
	j["remakes"] = snap.remakes;
	// 当前大区名:取最新一局的 platformId 映射
	if (!snap.games.empty())
		j["region"] = RegionDisplayName(snap.games.back().m.platformId);
	// 面板配置随快照下发,展示设置保存后页面在 2.5s 内自动应用
	j["bar"] = {
		{"enabled", st.barPanel.enabled},
		{"showTotal", st.barPanel.showTotal},
		{"showKda", st.barPanel.showKda},
		{"spacing", st.barPanel.spacing},
	};
	j["side"] = {
		{"enabled", st.sidePanel.enabled},     {"showHexes", st.sidePanel.showHexes},
		{"showItems", st.sidePanel.showItems}, {"showKda", st.sidePanel.showKda},
		{"iconSize", st.sidePanel.iconSize},   {"rows", st.sidePanel.rows},
		{"scrollSec", st.sidePanel.scrollSec}, {"scrollMs", st.sidePanel.scrollMs},
		{"radius", st.sidePanel.radius},       {"opacity", st.sidePanel.opacity},
		{"hexCols", st.sidePanel.hexCols},     {"showNames", st.sidePanel.showNames},
	};

	auto &games = j["games"] = nlohmann::json::array();
	for (auto &e : snap.games) {
		nlohmann::json g;
		g["champ"] = e.m.championId;
		g["t"] = e.m.startMs; // 开始时间,页面显示相对时间用
		g["win"] = e.m.win;
		g["remake"] = e.m.remake;
		g["k"] = e.m.kills;
		g["d"] = e.m.deaths;
		g["a"] = e.m.assists;
		auto &hx = g["hexes"] = nlohmann::json::array();
		for (int i = 0; i < e.m.hexCount; ++i) {
			nlohmann::json h;
			h["id"] = e.m.hexes[i];
			AugmentMeta am;
			if (mon.Assets().AugmentCopy(e.m.hexes[i], am)) {
				h["n"] = am.nameCn;
				h["lv"] = am.level;
			}
			hx.push_back(std::move(h));
		}
		auto &it = g["items"] = nlohmann::json::array();
		for (int i = 0; i < e.m.itemCount; ++i) {
			nlohmann::json o;
			o["id"] = e.m.items[i];
			ItemMeta im;
			if (mon.Assets().ItemCopy(e.m.items[i], im))
				o["n"] = im.nameCn;
			it.push_back(std::move(o));
		}
		games.push_back(std::move(g));
	}
	res.body = j.dump();
}

void WebServer::Impl::HandleStatus(Response &res)
{
	Monitor::StatusInfo st = Monitor::Instance().StatusSnapshot();
	int port;
	{
		std::lock_guard<std::recursive_mutex> lock(mtx);
		port = app.httpPort;
	}
	nlohmann::json j;
	j["connected"] = st.lcuConnected;
	j["status"] = st.status;
	j["account"] = st.account;
	j["matches"] = st.matchCount;
	j["augments"] = st.augmentCount;
	j["port"] = port;
	j["listening"] = listenPort.load() != 0; // 端口被占用自愈期间为 false
	j["overlayUrl"] = "http://127.0.0.1:" + std::to_string(port) + "/overlay";
	res.body = j.dump();
}

void WebServer::Impl::HandleSettingsPost(const Request &req, Response &res)
{
	nlohmann::json body;
	try {
		body = nlohmann::json::parse(req.body);
	} catch (...) {
		res.status = 400;
		res.reason = "Bad Request";
		res.body = "{\"ok\":false,\"error\":\"无效的 JSON\"}";
		return;
	}

	int oldPort;
	bool firstRun;
	{
		std::lock_guard<std::recursive_mutex> lock(mtx);
		oldPort = app.httpPort;
		firstRun = app.firstRunDone;
	}

	// 合并式保存:仅覆盖请求里出现的字段,缺省保持现值(防止空/半截请求重置全部配置)
	nlohmann::json cur = SettingsJson();
	for (auto it = body.begin(); it != body.end(); ++it) {
		if (it.key() == "bar" || it.key() == "side") {
			if (it.value().is_object()) {
				for (auto sub = it.value().begin(); sub != it.value().end(); ++sub)
					cur[it.key()][sub.key()] = sub.value();
			}
		} else if (!it.value().is_null()) {
			cur[it.key()] = it.value();
		}
	}
	AppSettings next = JsonToSettings(cur);
	next.firstRunDone = firstRun;
	int newPort = next.httpPort;

	{
		std::lock_guard<std::recursive_mutex> lock(mtx);
		app = next;
	}
	Monitor::Instance().ApplySettings(next.bar); // 批次/局数/模式变化 → 重算
	Monitor::Instance().NotifyDisplayChanged();  // 面板配置变化 → 页面刷新
	SaveSettings();

	// 端口与当前监听不一致时重启监听(含从未绑定成功的情况)
	if (newPort != listenPort.load()) {
		std::string err;
		if (!WebServer::Instance().RestartListener(newPort, err)) {
			// 回退端口并立即回绑旧端口;仍失败(如 TIME_WAIT 未散)则由自愈重试接管
			{
				std::lock_guard<std::recursive_mutex> lock(mtx);
				app.httpPort = oldPort;
			}
			SaveSettings();
			std::string rebindErr;
			WebServer::Instance().RestartListener(oldPort, rebindErr);
			res.status = 409;
			res.reason = "Conflict";
			res.body =
				"{\"ok\":false,\"error\":\"" + err + "\",\"settings\":" + SettingsJson().dump() + "}";
			return;
		}
	}
	bool portChanged = (newPort != oldPort);

	nlohmann::json j;
	j["ok"] = true;
	j["portChanged"] = portChanged;
	j["settings"] = SettingsJson();
	res.body = j.dump();
}

void WebServer::Impl::HandleAsset(const Request &req, Response &res)
{
	// /asset/champ/236.png、/asset/hex/1373.png、/asset/item/3153.png
	bool champ = req.path.rfind("/asset/champ/", 0) == 0;
	bool hex = req.path.rfind("/asset/hex/", 0) == 0;
	bool item = req.path.rfind("/asset/item/", 0) == 0;
	if (!champ && !hex && !item) {
		res.status = 404;
		res.reason = "Not Found";
		return;
	}
	size_t prefix = champ ? strlen("/asset/champ/") : (hex ? strlen("/asset/hex/") : strlen("/asset/item/"));
	std::string num = req.path.substr(prefix);
	if (num.size() >= 4 && num.compare(num.size() - 4, 4, ".png") == 0)
		num = num.substr(0, num.size() - 4);
	if (num.empty() || num.size() > 8 || num.find_first_not_of("0123456789") != std::string::npos) {
		res.status = 404;
		res.reason = "Not Found";
		return;
	}
	int id = atoi(num.c_str());
	if (id <= 0 || (champ && id > 2000) || ((hex || item) && id > 1000000)) {
		res.status = 404;
		res.reason = "Not Found";
		return;
	}

	AssetStore &assets = Monitor::Instance().Assets();
	std::wstring file = champ ? assets.ChampionIconPath(id)
				  : (hex ? assets.HexIconPath(id) : assets.ItemIconPath(id));
	std::string bytes;
	if (file.empty() || !util::ReadFileBytes(file, bytes, 8 * 1024 * 1024)) {
		res.status = 404;
		res.reason = "Not Found";
		return;
	}
	res.contentType = "image/png";
	res.cacheControl = "max-age=3600";
	res.body = std::move(bytes);
}

// ---------------------------------------------------------------- OBS 集成

static std::mutex g_obsMtx; // 序列化所有 obs 前端操作(HTTP 线程调用)

static bool FindSceneItem(obs_scene_t *scene, const char *name)
{
	struct Ctx {
		const char *name;
		bool found;
	} ctx{name, false};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *p) {
			auto *c = (Ctx *)p;
			obs_source_t *src = obs_sceneitem_get_source(item);
			if (src) {
				const char *n = obs_source_get_name(src);
				if (n && strcmp(n, c->name) == 0)
					c->found = true;
			}
			return !c->found;
		},
		&ctx);
	return ctx.found;
}

void WebServer::Impl::HandleScenes(Response &res)
{
	std::lock_guard<std::mutex> g(g_obsMtx);
	nlohmann::json j;
	auto &arr = j["scenes"] = nlohmann::json::array();

	obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; i++) {
		const char *nm = obs_source_get_name(list.sources.array[i]);
		if (nm)
			arr.push_back(nm);
	}
	obs_frontend_source_list_free(&list);

	j["current"] = "";
	obs_source_t *cur = obs_frontend_get_current_scene();
	if (cur) {
		const char *nm = obs_source_get_name(cur);
		if (nm)
			j["current"] = nm;
		obs_source_release(cur);
	}
	res.body = j.dump();
}

void WebServer::Impl::HandleAddSource(const Request &req, Response &res)
{
	nlohmann::json body;
	try {
		body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
	} catch (...) {
		res.status = 400;
		res.reason = "Bad Request";
		res.body = "{\"ok\":false,\"error\":\"无效的 JSON(请检查编码)\"}";
		return;
	}
	std::string sceneName = body.value("scene", "");
	std::string panel = body.value("panel", "bar");

	AppSettings st;
	{
		std::lock_guard<std::recursive_mutex> lock(mtx);
		st = app;
	}
	std::string inputName, url;
	int w, h;
	if (panel == "side") {
		inputName = "海克斯战绩条·侧栏";
		url = "http://127.0.0.1:" + std::to_string(st.httpPort) + "/side";
		w = st.sidePanel.w;
		h = st.sidePanel.h;
	} else if (panel == "bar") {
		inputName = "海克斯战绩条·顶部";
		url = "http://127.0.0.1:" + std::to_string(st.httpPort) + "/bar";
		w = st.barPanel.w;
		h = st.barPanel.h;
	} else {
		res.status = 400;
		res.reason = "Bad Request";
		res.body = "{\"ok\":false,\"error\":\"panel 必须是 bar 或 side\"}";
		return;
	}

	nlohmann::json out;
	std::lock_guard<std::mutex> g(g_obsMtx);

	do { // 一次性错误出口
		if (!obs_get_latest_input_type_id("browser_source")) {
			out["error"] = "OBS 未启用浏览器源组件(obs-browser)";
			break;
		}

		obs_source_t *scene = nullptr;
		if (!sceneName.empty()) {
			scene = obs_get_source_by_name(sceneName.c_str());
			if (!scene || !obs_scene_from_source(scene)) {
				out["error"] = "找不到场景:" + sceneName;
				if (scene)
					obs_source_release(scene);
				break;
			}
		} else {
			scene = obs_frontend_get_current_scene();
			if (!scene) {
				out["error"] = "无法获取当前场景,请先选择场景";
				break;
			}
		}

		obs_source_t *input = obs_get_source_by_name(inputName.c_str());
		if (input) {
			if (strcmp(obs_source_get_unversioned_id(input), "browser_source") != 0) {
				out["error"] = "已存在同名非浏览器源,请改名后重试";
				obs_source_release(input);
				obs_source_release(scene);
				break;
			}
			// 复用:仅同步地址与尺寸(场景项的位置/缩放不受影响)
			obs_data_t *s = obs_data_create();
			obs_data_set_string(s, "url", url.c_str());
			obs_data_set_int(s, "width", w);
			obs_data_set_int(s, "height", h);
			obs_source_update(input, s);
			obs_data_release(s);
		} else {
			obs_data_t *s = obs_data_create();
			obs_data_set_string(s, "url", url.c_str());
			obs_data_set_int(s, "width", w);
			obs_data_set_int(s, "height", h);
			input = obs_source_create("browser_source", inputName.c_str(), s, nullptr);
			obs_data_release(s);
			if (!input) {
				out["error"] = "创建浏览器源失败";
				obs_source_release(scene);
				break;
			}
		}

		obs_scene_t *sc = obs_scene_from_source(scene);
		bool existed = FindSceneItem(sc, inputName.c_str());
		if (!existed) {
			obs_sceneitem_t *item = obs_scene_add(sc, input);
			if (item)
				obs_sceneitem_set_visible(item, true);
		}

		out["message"] = std::string(existed ? "场景已包含「" : "已把「") + inputName +
				 (existed ? "」加入场景(已同步地址与尺寸):" : "」浏览器源加入场景:") +
				 obs_source_get_name(scene);
		obs_source_release(input);
		obs_source_release(scene);
	} while (false);

	if (out.contains("error")) {
		res.status = 409;
		res.reason = "Conflict";
		out["ok"] = false;
	} else {
		out["ok"] = true;
	}
	res.body = out.dump();
}

} // namespace hexbar
