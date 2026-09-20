#include "webfiles.hpp"

#include <windows.h>

#include <obs-module.h>

#include <filesystem>
#include <map>
#include <mutex>

#include "util.hpp"

namespace hexbar::webfiles {

namespace {

struct Entry {
	std::string embedded;  // 内嵌资源字节(最终兜底)
	std::wstring diskPath; // 首选磁盘路径(安装布局文件或解压出的副本)
};

std::mutex g_mtx;
std::wstring g_devDir; // LOLHEXBAR_WEB_DIR 指向的源码 data/web(开发热改)
std::map<std::string, Entry> g_entries;

// 读取本 dll 内嵌的 RCDATA 资源(资源名即文件名,见 web-resources.rc)
bool LoadEmbedded(const char *name, std::string &out)
{
	HMODULE mod = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCWSTR)&LoadEmbedded, &mod))
		return false;
	std::wstring resName = util::Utf8ToWide(name);
	HRSRC r = FindResourceW(mod, resName.c_str(), RT_RCDATA);
	if (!r)
		return false;
	HGLOBAL h = LoadResource(mod, r);
	if (!h)
		return false;
	const char *p = (const char *)LockResource(h);
	DWORD n = SizeofResource(mod, r);
	if (!p || !n)
		return false;
	out.assign(p, (size_t)n);
	return true;
}

// 与内嵌不一致时覆盖写:升级自动更新;内容相同不动(保留对手改副本的尊重)
void ExtractIfChanged(const std::wstring &path, const std::string &bytes)
{
	std::string cur;
	if (util::ReadFileBytes(path, cur) && cur == bytes)
		return;
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
	util::WriteFileBytesAtomic(path, bytes.data(), bytes.size());
}

} // namespace

void Init(const std::wstring &configDir)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	g_entries.clear();

	wchar_t env[1024] = {};
	GetEnvironmentVariableW(L"LOLHEXBAR_WEB_DIR", env, 1024);
	if (env[0])
		g_devDir = env;

	static const char *kFiles[] = {"overlay.html", "settings.html"};
	for (const char *name : kFiles) {
		Entry e;
		LoadEmbedded(name, e.embedded);

		// 安装器/开发 rundir 布局:插件 data 目录自带 web 文件
		std::wstring installed;
		char *p = obs_module_file((std::string("web/") + name).c_str());
		if (p && *p) {
			installed = util::Utf8ToWide(p);
			bfree(p);
		}

		// 裸 DLL 分发(没有 data 目录):把内嵌资源解压到配置目录
		std::wstring extracted = configDir + L"\\web\\" + util::Utf8ToWide(name);
		if (installed.empty() && !e.embedded.empty())
			ExtractIfChanged(extracted, e.embedded);
		e.diskPath = installed.empty() ? extracted : installed;

		g_entries[name] = std::move(e);
	}
}

bool Read(const char *name, std::string &out)
{
	std::wstring dev;
	{
		std::lock_guard<std::mutex> lock(g_mtx);
		auto it = g_entries.find(name);
		if (it == g_entries.end())
			return false;
		dev = g_devDir;
		// 开发目录:改完文件浏览器里刷新即可,无需重编重启
		if (!dev.empty() && util::ReadFileBytes(dev + L"\\" + util::Utf8ToWide(name), out))
			return true;
		// 常规:安装布局文件,或解压出的副本(允许手改热调)
		if (util::ReadFileBytes(it->second.diskPath, out))
			return true;
		// 兜底:内存中的内嵌资源(磁盘全不可用时页面仍可用)
		if (!it->second.embedded.empty()) {
			out = it->second.embedded;
			return true;
		}
	}
	return false;
}

} // namespace hexbar::webfiles
