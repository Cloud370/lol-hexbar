#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

namespace hexbar::util {

std::wstring Utf8ToWide(std::string_view utf8)
{
	if (utf8.empty())
		return std::wstring();
	int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), (int)utf8.size(), nullptr, 0);
	if (need <= 0) {
		std::wstring fallback;
		fallback.reserve(utf8.size());
		for (char c : utf8)
			fallback.push_back((wchar_t)(unsigned char)c);
		return fallback;
	}
	std::wstring out((size_t)need, L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), (int)utf8.size(), out.data(), need);
	return out;
}

std::string WideToUtf8(std::wstring_view wide)
{
	if (wide.empty())
		return std::string();
	int need = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), (int)wide.size(), nullptr, 0,
				       nullptr, nullptr);
	if (need <= 0) {
		std::string fallback;
		fallback.reserve(wide.size());
		for (wchar_t c : wide)
			fallback.push_back((char)(c & 0xff));
		return fallback;
	}
	std::string out((size_t)need, '\0');
	WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), (int)wide.size(), out.data(), need, nullptr,
			    nullptr);
	return out;
}

static const char B64_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string B64Encode(std::string_view bytes)
{
	std::string out;
	out.reserve((bytes.size() + 2) / 3 * 4);
	size_t i = 0;
	while (i + 3 <= bytes.size()) {
		uint32_t v = (uint8_t)bytes[i] << 16 | (uint8_t)bytes[i + 1] << 8 | (uint8_t)bytes[i + 2];
		out.push_back(B64_CHARS[(v >> 18) & 63]);
		out.push_back(B64_CHARS[(v >> 12) & 63]);
		out.push_back(B64_CHARS[(v >> 6) & 63]);
		out.push_back(B64_CHARS[v & 63]);
		i += 3;
	}
	size_t rem = bytes.size() - i;
	if (rem == 1) {
		uint32_t v = (uint8_t)bytes[i] << 16;
		out.push_back(B64_CHARS[(v >> 18) & 63]);
		out.push_back(B64_CHARS[(v >> 12) & 63]);
		out.append("==");
	} else if (rem == 2) {
		uint32_t v = (uint8_t)bytes[i] << 16 | (uint8_t)bytes[i + 1] << 8;
		out.push_back(B64_CHARS[(v >> 18) & 63]);
		out.push_back(B64_CHARS[(v >> 12) & 63]);
		out.push_back(B64_CHARS[(v >> 6) & 63]);
		out.push_back('=');
	}
	return out;
}

std::string Trim(std::string_view s)
{
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
		++b;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
		--e;
	return std::string(s.substr(b, e - b));
}

std::vector<std::string> Split(std::string_view s, char sep)
{
	std::vector<std::string> out;
	size_t pos = 0;
	while (true) {
		size_t next = s.find(sep, pos);
		if (next == std::string_view::npos) {
			out.emplace_back(s.substr(pos));
			break;
		}
		out.emplace_back(s.substr(pos, next - pos));
		pos = next + 1;
	}
	return out;
}

bool ReadFileBytes(const std::wstring &path, std::string &out, size_t maxBytes)
{
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f)
		return false;
	std::streamoff size = f.tellg();
	if (size < 0)
		return false;
	f.seekg(0);
	out.resize((size_t)size);
	if (size > 0)
		f.read(out.data(), size);
	return (bool)f || size == 0;
}

bool WriteFileBytesAtomic(const std::wstring &path, const void *data, size_t len)
{
	namespace fs = std::filesystem;
	std::wstring tmp = path + L".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f)
			return false;
		if (len > 0)
			f.write((const char *)data, (std::streamsize)len);
		if (!f)
			return false;
	}
	std::error_code ec;
	fs::rename(tmp, path, ec);
	if (ec) {
		// 目标被占用时的兜底:直接写原路径
		std::ofstream f(path, std::ios::binary | std::ios::trunc);
		if (!f)
			return false;
		if (len > 0)
			f.write((const char *)data, (std::streamsize)len);
		DeleteFileW(tmp.c_str());
	}
	return true;
}

int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::system_clock::now().time_since_epoch())
		.count();
}

bool RegexSearchRemoved()
{
	return true;
}

std::string ExtractFlagValue(std::string_view text, std::string_view key)
{
	size_t pos = text.find(key);
	if (pos == std::string_view::npos)
		return {};
	pos += key.size();
	size_t end = pos;
	while (end < text.size()) {
		char c = text[end];
		bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
			  c == '-';
		if (!ok)
			break;
		++end;
	}
	return std::string(text.substr(pos, end - pos));
}

std::string ExtractDigitsAfter(std::string_view text, std::string_view key)
{
	size_t pos = text.find(key);
	if (pos == std::string_view::npos)
		return {};
	pos += key.size();
	size_t end = pos;
	while (end < text.size() && text[end] >= '0' && text[end] <= '9')
		++end;
	return std::string(text.substr(pos, end - pos));
}

} // namespace hexbar::util
