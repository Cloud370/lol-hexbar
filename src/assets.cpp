#include "assets.hpp"

#include <filesystem>

#include <nlohmann/json.hpp>

#include "http.hpp"
#include "util.hpp"

namespace hexbar {

namespace fs = std::filesystem;

static constexpr const char *GTIMG_AUGMENTS_URL = "https://game.gtimg.cn/images/lol/act/img/js/kiwi/kiwi_augments.json";
static constexpr const char *CD_CHAMP_ICON_URL =
	"https://raw.communitydragon.org/latest/plugins/rcp-be-lol-game-data/global/default/v1/champion-icons/";
static constexpr const char *GTIMG_ITEM_ICON_URL = "https://game.gtimg.cn/images/lol/act/img/item/"; // + <id>.png
static constexpr const char *LCU_ITEMS_PATH = "/lol-game-data/assets/v1/items.json";

void AssetStore::Init(const std::wstring &cacheDir)
{
	std::lock_guard<std::mutex> lock(mtx);
	dir = cacheDir;
	std::error_code ec;
	fs::create_directories(dir, ec);
	augmentsFile = dir + L"\\kiwi_augments.json";
	itemsFile = dir + L"\\items.json";

	// 启动即加载缓存
	std::string content;
	if (util::ReadFileBytes(augmentsFile, content)) {
		try {
			auto j = nlohmann::json::parse(content);
			for (auto &a : j) {
				AugmentMeta m;
				m.id = a.value("augmentID", 0);
				m.nameCn = a.value("name_cn", "");
				m.level = a.value("level", "");
				m.smallIconUrl = a.value("small_Icon", "");
				if (m.id)
					augs[m.id] = m;
			}
			augmentsValid = !augs.empty();
		} catch (...) {
		}
	}
	content.clear();
	if (util::ReadFileBytes(itemsFile, content)) {
		try {
			auto j = nlohmann::json::parse(content);
			for (auto &e : j) {
				ItemMeta m;
				m.id = e.value("id", 0);
				m.nameCn = e.value("name", "");
				m.iconPath = e.value("iconPath", "");
				if (m.id)
					itemsMap[m.id] = m;
			}
			itemsValid = !itemsMap.empty();
		} catch (...) {
		}
	}
}

bool AssetStore::RefreshAugments(std::string &err)
{
	HttpResponse resp;
	if (!Http::Get(util::Utf8ToWide(GTIMG_AUGMENTS_URL), "", false, 10000, 8 * 1024 * 1024, resp, err))
		return false;
	if (!resp.Ok()) {
		err = "HTTP " + std::to_string(resp.status);
		return false;
	}
	try {
		auto j = nlohmann::json::parse(resp.body);
		if (!j.is_array() || j.empty()) {
			err = "unexpected augment payload";
			return false;
		}
		std::unordered_map<int32_t, AugmentMeta> fresh;
		for (auto &a : j) {
			AugmentMeta m;
			m.id = a.value("augmentID", 0);
			m.nameCn = a.value("name_cn", "");
			m.level = a.value("level", "");
			m.smallIconUrl = a.value("small_Icon", "");
			if (m.id)
				fresh[m.id] = m;
		}
		{
			std::lock_guard<std::mutex> lock(mtx);
			augs = std::move(fresh);
			augmentsValid = true;
		}
		util::WriteFileBytesAtomic(augmentsFile, resp.body.data(), resp.body.size());
		return true;
	} catch (const std::exception &e) {
		err = std::string("parse: ") + e.what();
		return false;
	}
}

const AugmentMeta *AssetStore::Augment(int32_t id)
{
	std::lock_guard<std::mutex> lock(mtx);
	auto it = augs.find(id);
	return it == augs.end() ? nullptr : &it->second;
}

bool AssetStore::AugmentCopy(int32_t id, AugmentMeta &out)
{
	std::lock_guard<std::mutex> lock(mtx);
	auto it = augs.find(id);
	if (it == augs.end())
		return false;
	out = it->second;
	return true;
}

bool AssetStore::RefreshItems(const LcuClient *lcu, std::string &err)
{
	if (!lcu) {
		std::lock_guard<std::mutex> lock(mtx);
		return itemsValid;
	}
	nlohmann::json j;
	std::string ferr;
	if (!lcu->GetJson(LCU_ITEMS_PATH, j, ferr)) {
		err = "items.json: " + ferr;
		std::lock_guard<std::mutex> lock(mtx);
		return itemsValid; // 失败保留缓存
	}
	if (!j.is_array() || j.empty()) {
		err = "items.json: unexpected payload";
		return false;
	}
	std::unordered_map<int32_t, ItemMeta> fresh;
	for (auto &e : j) {
		ItemMeta m;
		m.id = e.value("id", 0);
		m.nameCn = e.value("name", "");
		m.iconPath = e.value("iconPath", "");
		if (m.id)
			fresh[m.id] = m;
	}
	{
		std::lock_guard<std::mutex> lock(mtx);
		itemsMap = std::move(fresh);
		itemsValid = true;
	}
	std::string raw = j.dump();
	util::WriteFileBytesAtomic(itemsFile, raw.data(), raw.size());
	return true;
}

bool AssetStore::ItemsValid() const
{
	std::lock_guard<std::mutex> lock(mtx);
	return itemsValid;
}

bool AssetStore::ItemCopy(int32_t id, ItemMeta &out)
{
	std::lock_guard<std::mutex> lock(mtx);
	auto it = itemsMap.find(id);
	if (it == itemsMap.end())
		return false;
	out = it->second;
	return true;
}

size_t AssetStore::AugmentCount()
{
	std::lock_guard<std::mutex> lock(mtx);
	return augs.size();
}

std::wstring AssetStore::champPath(int32_t id) const
{
	return dir + L"\\champ_" + std::to_wstring(id) + L".png";
}

std::wstring AssetStore::hexPath(int32_t id) const
{
	return dir + L"\\hex_" + std::to_wstring(id) + L".png";
}

std::wstring AssetStore::itemPath(int32_t id) const
{
	return dir + L"\\item_" + std::to_wstring(id) + L".png";
}

std::wstring AssetStore::ChampionIconPath(int32_t championId) const
{
	std::error_code ec;
	std::wstring p = champPath(championId);
	return fs::exists(p, ec) ? p : std::wstring();
}

std::wstring AssetStore::HexIconPath(int32_t augId) const
{
	std::error_code ec;
	std::wstring p = hexPath(augId);
	return fs::exists(p, ec) ? p : std::wstring();
}

std::wstring AssetStore::ItemIconPath(int32_t itemId) const
{
	std::error_code ec;
	std::wstring p = itemPath(itemId);
	return fs::exists(p, ec) ? p : std::wstring();
}

bool AssetStore::DownloadTo(const std::string &urlUtf8, const std::wstring &dest, bool ignoreCert, std::string &err)
{
	HttpResponse resp;
	if (!Http::Get(util::Utf8ToWide(urlUtf8), "", ignoreCert, 10000, 4 * 1024 * 1024, resp, err))
		return false;
	if (!resp.Ok()) {
		err = "HTTP " + std::to_string(resp.status);
		return false;
	}
	// PNG 魔数校验,防止把错误页当图标存下来
	if (resp.body.size() < 8 || (uint8_t)resp.body[0] != 0x89 || resp.body[1] != 'P') {
		err = "not a PNG payload";
		return false;
	}
	return util::WriteFileBytesAtomic(dest, resp.body.data(), resp.body.size());
}

bool AssetStore::EnsureChampionIcon(int32_t championId, const LcuClient *lcu, std::string &err)
{
	std::error_code ec;
	std::wstring dest = champPath(championId);
	if (fs::exists(dest, ec) && fs::file_size(dest, ec) > 0)
		return true;
	if (lcu) {
		std::string bytes;
		std::string path = "/lol-game-data/assets/v1/champion-icons/" + std::to_string(championId) + ".png";
		if (lcu->GetBinary(path, bytes, err)) {
			if (bytes.size() >= 8 && (uint8_t)bytes[0] == 0x89 && bytes[1] == 'P')
				return util::WriteFileBytesAtomic(dest, bytes.data(), bytes.size());
			err = "champion icon payload not PNG";
		}
	}
	// 兜底:CommunityDragon
	std::string url = std::string(CD_CHAMP_ICON_URL) + std::to_string(championId) + ".png";
	return DownloadTo(url, dest, false, err);
}

bool AssetStore::EnsureHexIcon(int32_t augId, std::string &err)
{
	std::error_code ec;
	std::wstring dest = hexPath(augId);
	if (fs::exists(dest, ec) && fs::file_size(dest, ec) > 0)
		return true;
	const AugmentMeta *meta = Augment(augId);
	if (!meta || meta->smallIconUrl.empty()) {
		err = "no augment meta for " + std::to_string(augId);
		return false;
	}
	return DownloadTo(meta->smallIconUrl, dest, false, err);
}

bool AssetStore::EnsureItemIcon(int32_t itemId, const LcuClient *lcu, std::string &err)
{
	std::error_code ec;
	std::wstring dest = itemPath(itemId);
	if (fs::exists(dest, ec) && fs::file_size(dest, ec) > 0)
		return true;

	// 首选 LCU 本地资源(含国服/模式专属装备),兜底 gtimg 静态图标
	ItemMeta meta;
	if (lcu && ItemCopy(itemId, meta) && !meta.iconPath.empty()) {
		std::string bytes;
		std::string ferr;
		if (lcu->GetBinary(meta.iconPath, bytes, ferr)) {
			if (bytes.size() >= 8 && (uint8_t)bytes[0] == 0x89 && bytes[1] == 'P')
				return util::WriteFileBytesAtomic(dest, bytes.data(), bytes.size());
		}
	}
	std::string url = std::string(GTIMG_ITEM_ICON_URL) + std::to_string(itemId) + ".png";
	return DownloadTo(url, dest, false, err);
}

} // namespace hexbar
