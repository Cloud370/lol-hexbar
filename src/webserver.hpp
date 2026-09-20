#pragma once

#include <memory>
#include <string>

#include "gamedata.hpp"

namespace hexbar {

// 顶部战绩条面板(与全局配置独立)
struct BarPanelSettings {
	bool enabled = true;
	bool showTotal = true;
	bool showKda = true;
	int spacing = 5; // 段间距 px
	int w = 1440;    // 添加 OBS 浏览器源时的默认尺寸
	int h = 110;
};

// 侧边出装列表面板(与全局配置独立;默认值即用户调定的样式)
struct SidePanelSettings {
	bool enabled = true;
	bool showHexes = true;
	bool showItems = true;
	bool showKda = true;
	bool showNames = true;
	int iconSize = 24;
	int rows = 3;
	int scrollSec = 10;
	int scrollMs = 1500;
	int radius = 4;
	int opacity = 95;
	int hexCols = 2;
	int w = 600;
	int h = 690;
};

// 网页路线的全部配置(全局 + 两个面板 + 服务)
struct AppSettings {
	BarSettings bar; // 全局:批次间隔/局数/模式/客户端目录
	BarPanelSettings barPanel;
	SidePanelSettings sidePanel;
	int httpPort = 35712;
	bool firstRunDone = false;
};

// 仅监听 127.0.0.1 的内嵌 HTTP 服务:
//   GET  /overlay             战绩条页面(透明背景)
//   GET  /                    设置页
//   GET  /api/snapshot        展示快照 JSON
//   GET  /api/status          连接状态
//   GET/POST /api/settings    设置读写
//   POST /api/refresh         手动刷新战绩
//   POST /api/new-batch       开始新批次
//   GET  /api/scenes          OBS 场景列表
//   POST /api/add-source      在 OBS 里创建/复用浏览器源
//   GET  /asset/champ/:id.png /asset/hex/:id.png  图标缓存
class WebServer {
public:
	static WebServer &Instance();

	// configDir 为 obs_module_config_path 结果;加载 settings.json 并开始监听
	bool Start(const std::wstring &configDir);
	void Stop();

	bool RestartListener(int port, std::string &err);

	AppSettings SettingsCopy();
	void MarkFirstRunDone();
	std::string OverlayUrl();  // http://127.0.0.1:<port>/overlay
	std::string SettingsUrl(); // http://127.0.0.1:<port>/

private:
	WebServer() = default;
	struct Impl;
	std::shared_ptr<Impl> impl; // 连接线程共享持有,保证停止时序安全
};

} // namespace hexbar
