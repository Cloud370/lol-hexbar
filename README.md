# LoL 海克斯大乱斗战绩条(OBS 插件 + HTML Overlay)

一个原生 OBS Studio 插件:为英雄联盟**海克斯大乱斗**(国服 WeGame / 全球服)直播展示**本次直播批次**的近期战绩条。插件内嵌一个只监听 `127.0.0.1` 的本地 HTTP 服务,OBS 用**浏览器源**加载页面展示——英雄头像、胜负、海克斯选择(带品质边框)与 K/D/A,顶部战绩条 + 侧边出装列表两个独立面板,对局结束自动更新。

基于 [obsproject/obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)(GPL-2.0)构建,仅支持 Windows。

## 快速上手

1. 下载安装器 `lol-hexbar-<版本>-windows-x64-Installer.exe` 并运行:它会**自动检测 OBS Studio 安装目录**(读取 OBS 官方安装程序留在注册表的信息),默认直接装进 OBS 目录(`obs-plugins\64bit` + `data\obs-plugins\`),一路下一步即可;未检测到 OBS 时装到 OBS 官方支持的 `C:\ProgramData\obs-studio\plugins\`(每插件目录)。升级 = 重新运行安装器覆盖;可在系统"应用列表"卸载
2. 启动 OBS(插件随 OBS 自动运行;首次运行会自动打开设置页,也可从 OBS 菜单 **工具 → 海克斯大乱斗战绩条 · 设置** 打开)
3. 启动英雄联盟客户端(插件会持续重试发现,先后顺序不限)
4. 在设置页"OBS 浏览器源"面板选好场景,点 **添加 / 更新浏览器源**(同名源只同步地址与尺寸,不影响你在 OBS 里调好的位置缩放);也可手动添加浏览器源,URL 填 `http://127.0.0.1:35712/overlay`
5. 在 OBS 里把该源摆到游戏画面顶部,按需整体缩放

不想用安装器时,也可手动部署:把 zip 里的 `lol-hexbar\`(含 `bin\64bit\lol-hexbar.dll` 与 `data\`)整个目录复制到 `C:\ProgramData\obs-studio\plugins\` 下,或把 dll 复制到 OBS 目录的 `obs-plugins\64bit\`、`data\` 内容并入 OBS 的 `data\obs-plugins\lol-hexbar\`;只复制单个 dll 也能用——插件会把内嵌页面自动解压到自己的配置目录。

两个面板相互独立、可分别开关与添加:/bar 顶部战绩条(总胜负 + 每局头像/KDA,不含海克斯装备,建议放游戏画面顶部);/side 侧边出装列表(每局一行:头像+胜负+KDA、海克斯芯片带中文名与品质描边、装备按背包槽位顺序,大图标,超出可见行数自动滚动);/overlay 两面板合一。位置与缩放由你在 OBS 里自由调整,无数据时显示半透明占位(可先摆放定位)。

## 功能

- 自动发现并连接本机英雄联盟客户端(国服走客户端日志解析,全球服走 lockfile/进程命令行),只读接口
- **战绩后台自动刷新**:非对局中约每 15 秒拉取一次,对局结束跳变立即触发;数据没变化时不重渲染(不打断侧栏滚动动画),全程无需手动点"刷新"
- 换账号、断线重连自动补齐去重;**重开局不展示且不计胜负**;装备按 item0..5 背包槽位顺序展示
- 侧边列表海克斯图标为 LeagueAkari 式品质框:白银/黄金/棱彩各配色描边加深色底,棱彩为 135° 渐变边
- 批次规则:上一局结束到下一局开始间隔 ≤ 设定值(默认 60 分钟,可设 1~100000,所见即所得)算同一批,可跨账号;可随时"开始新批次"(手动边界不会被历史回填覆盖)
- 顶部条:默认最近 20 局,每局头像 + K/D/A;条首总胜负
- 侧边列表:每局的海克斯(中文名+品质)与装备(槽位顺序),图标大小/可见行数/滚动间隔可调
- 设置页:`http://127.0.0.1:35712/`(端口可改),含实时预览(可切"无数据占位")、连接状态、账号、缓存局数、地址复制;配置分**全局 / 顶部条面板 / 侧边列表面板**三块独立组织,每个面板可独立开关并一键添加自己的浏览器源
- 海克斯:gtimg 元数据(中文名/品质/图标);装备:LCU 本地 items.json(含国服模式专属装备,兜底 gtimg 图标);英雄头像:客户端本地资产(兜底 CommunityDragon);全部磁盘缓存
- 数据本地持久化(`%APPDATA%\obs-studio\plugin_config\lol-hexbar\`,便携模式在 OBS 目录 `config/` 下),重启先恢复缓存再刷新;服务暂不可达时页面保留最后画面

## HTTP 接口(均仅本机)

| 路径 | 说明 |
| --- | --- |
| `GET /bar` `GET /side` `GET /overlay` | 顶部条 / 侧边列表 / 两面板合一(透明背景,OBS 浏览器源加载) |
| `GET /` | 设置页 |
| `GET /api/snapshot` | 展示快照 JSON(2.5s 轮询) |
| `GET /api/status` / `GET,POST /api/settings` | 状态 / 设置读写 |
| `POST /api/refresh` / `POST /api/new-batch` | 手动刷新 / 开始新批次 |
| `GET /api/scenes` / `POST /api/add-source` | OBS 场景列表 / 一键添加浏览器源 |
| `GET /asset/champ/:id.png` `/asset/hex/:id.png` `/asset/item/:id.png` | 图标缓存 |

LCU 凭据只留在插件进程内,不进入页面与 URL。

## 构建

```bash
cmake --preset windows-x64-vs2026   # 或 windows-x64
cmake --build build_x64_vs2026 --config RelWithDebInfo
```

首次 configure 会自动下载 OBS 31.1.1 源码与 obs-deps(见 `buildspec.json`)。需要 VS2022/2026 与 CMake ≥ 3.28。

### 页面(HTML/CSS/JS)开发

两个页面是外置静态文件:[`data/web/overlay.html`](data/web/overlay.html)(战绩画面)与 [`data/web/settings.html`](data/web/settings.html)(设置页)。同时它们被内嵌进 dll 作为兜底:安装/部署带 `data\` 目录时优先读磁盘文件,只复制裸 dll 时启动自动解压到插件配置目录。

改页面不用重编:设环境变量 `LOLHEXBAR_WEB_DIR` 指向仓库的 `data\web`(如 `set LOLHEXBAR_WEB_DIR=<仓库路径>\data\web`),重启 OBS 后服务直接读源文件,保存即生效,浏览器源/预览里刷新即可看到。

## 发布

推送形如 `1.2.3` 的 tag(同时把 `buildspec.json` 的 `version` 改为相同版本),GitHub Actions 自动产出 Windows x64 产物(draft release):zip 手动包 + NSIS 安装器(`…-Installer.exe`,自动检测 OBS 目录安装,见 [installer.nsi](installer.nsi))。

## 已实测环境

- Windows 11(10.0.26200)、OBS Studio 31.0.1 / 31.1.1、国服客户端 16.18(WeGame,HN1,locale zh_CN)
- 数据链路(客户端发现、战绩、海克斯、图标)与 HTML 展示、OBS 浏览器源一键添加均实机验证;其它区服理论兼容(走 lockfile/命令行发现)但未实测

## 许可

GPL-2.0(与 OBS 及官方插件模板一致)。第三方:`nlohmann/json`(MIT)。
