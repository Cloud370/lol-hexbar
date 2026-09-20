#pragma once

#include <string>

namespace hexbar::webfiles {

// 启动时调用:确定页面文件的磁盘位置。
// 优先级:环境变量 LOLHEXBAR_WEB_DIR(开发热改)→ 插件安装 data 目录(obs_module_file)
// → 把内嵌资源解压到 <configDir>\web(裸 DLL 分发也能用)。
void Init(const std::wstring &configDir);

// 读取页面内容("overlay.html" / "settings.html"),每次从磁盘现读以支持热改;
// 磁盘全部失败时返回内嵌资源字节。失败返回 false。
bool Read(const char *name, std::string &out);

} // namespace hexbar::webfiles
