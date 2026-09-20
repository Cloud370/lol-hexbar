#include <obs-module.h>
#include <obs-frontend-api.h>

#include <windows.h>
#include <shellapi.h>

#include "monitor.hpp"
#include "util.hpp"
#include "webserver.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("lol-hexbar", "zh-CN")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "海克斯大乱斗近期战绩条(本地 HTTP 服务 + OBS 浏览器源)";
}

namespace hexbar {

static std::wstring ModuleConfigDir()
{
	std::wstring base;
	char *cfg = obs_module_config_path(nullptr);
	if (cfg) {
		base = util::Utf8ToWide(cfg);
		bfree(cfg);
	}
	return base;
}

static void OpenSettingsPage(void *)
{
	std::wstring url = util::Utf8ToWide(WebServer::Instance().SettingsUrl());
	ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void StartPlugin()
{
	Monitor &mon = Monitor::Instance();
	mon.Start();

	WebServer &ws = WebServer::Instance();
	std::wstring cfgDir = ModuleConfigDir();
	ws.Start(cfgDir.empty() ? L"." : cfgDir);

	AppSettings st = ws.SettingsCopy();
	mon.ApplySettings(st.bar);

	// OBS 工具菜单入口,方便随时打开设置页
	obs_frontend_add_tools_menu_item("海克斯大乱斗战绩条 · 设置", OpenSettingsPage, nullptr);

	// 首次运行:自动打开设置页,便于拿到 overlay 地址
	if (!st.firstRunDone) {
		OpenSettingsPage(nullptr);
		ws.MarkFirstRunDone();
	}

	blog(LOG_INFO, "[lol-hexbar] started, overlay at %s", ws.OverlayUrl().c_str());
}

void ShutdownPlugin()
{
	// 先停 HTTP(不再有新请求触及 Monitor),再停采集线程
	WebServer::Instance().Stop();
	Monitor::Instance().Shutdown();
}

} // namespace hexbar

bool obs_module_load(void)
{
	hexbar::StartPlugin();
	blog(LOG_INFO, "[lol-hexbar] loaded (hex brawl match bar, html route)");
	return true;
}

void obs_module_unload(void)
{
	hexbar::ShutdownPlugin();
	blog(LOG_INFO, "[lol-hexbar] unloaded");
}
