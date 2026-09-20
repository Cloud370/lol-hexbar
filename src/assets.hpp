#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "lcu.hpp"

namespace hexbar {

struct AugmentMeta {
	int32_t id = 0;
	std::string nameCn;
	std::string level; // kSilver / kGold / kPrismatic
	std::string smallIconUrl;
};

struct ItemMeta {
	int32_t id = 0;
	std::string nameCn;
	std::string iconPath; // LCU 本地资源路径
};

class AssetStore {
public:
	void Init(const std::wstring &cacheDir);

	// 从 gtimg 刷新海克斯元数据(失败保留旧缓存),返回是否有可用数据
	bool RefreshAugments(std::string &err);

	// 装备元数据:LCU /lol-game-data/assets/v1/items.json(lcu 为空或失败时仅用磁盘缓存)
	bool RefreshItems(const LcuClient *lcu, std::string &err);
	bool ItemsValid() const;

	const AugmentMeta *Augment(int32_t id);

	// 线程安全拷贝(渲染/HTTP 线程用,避免持有内部指针)
	bool AugmentCopy(int32_t id, AugmentMeta &out);
	bool ItemCopy(int32_t id, ItemMeta &out);

	// 仅返回缓存路径(不联网);不存在返回空
	std::wstring ChampionIconPath(int32_t championId) const;
	std::wstring HexIconPath(int32_t augId) const;
	std::wstring ItemIconPath(int32_t itemId) const;

	// 下载缺失资源(仅在监控线程调用)
	bool EnsureChampionIcon(int32_t championId, const LcuClient *lcu, std::string &err);
	bool EnsureHexIcon(int32_t augId, std::string &err);
	bool EnsureItemIcon(int32_t itemId, const LcuClient *lcu, std::string &err);

	size_t AugmentCount();

private:
	std::wstring champPath(int32_t id) const;
	std::wstring hexPath(int32_t id) const;
	std::wstring itemPath(int32_t id) const;
	bool DownloadTo(const std::string &urlUtf8, const std::wstring &dest, bool ignoreCert, std::string &err);

	mutable std::mutex mtx;
	std::wstring dir;
	std::wstring augmentsFile;
	std::wstring itemsFile;
	std::unordered_map<int32_t, AugmentMeta> augs;
	std::unordered_map<int32_t, ItemMeta> itemsMap;
	bool augmentsValid = false;
	bool itemsValid = false;
};

} // namespace hexbar
