#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hexbar::util {

std::wstring Utf8ToWide(std::string_view utf8);
std::string WideToUtf8(std::wstring_view wide);

std::string B64Encode(std::string_view bytes);

std::string Trim(std::string_view s);
std::vector<std::string> Split(std::string_view s, char sep);

bool ReadFileBytes(const std::wstring &path, std::string &out, size_t maxBytes = 32 * 1024 * 1024);
bool WriteFileBytesAtomic(const std::wstring &path, const void *data, size_t len);

int64_t NowMs();

// 在 text 中查找 "key<sep>值" 形式的参数,值取随后连续的 [A-Za-z0-9_-] 或数字,返回空串表示未找到
std::string ExtractFlagValue(std::string_view text, std::string_view key);
std::string ExtractDigitsAfter(std::string_view text, std::string_view key);

} // namespace hexbar::util
