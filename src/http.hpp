#pragma once

#include <string>

namespace hexbar {

struct HttpResponse {
	long status = 0;
	std::string body;

	bool Ok() const { return status >= 200 && status < 300; }
};

class Http {
public:
	// 同步 GET。basicAuth 为完整 "Basic xxxx" 头值(可空)。
	// ignoreCertErrors 用于 LCU 自签名证书。
	// quiet=true 时不输出失败日志(高频探测端口 2999 等场合避免刷屏)。
	static bool Get(const std::wstring &url, const std::string &basicAuth, bool ignoreCertErrors, int timeoutMs,
			size_t maxBytes, HttpResponse &out, std::string &err, bool quiet = false);
};

} // namespace hexbar
