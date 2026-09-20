#include "http.hpp"

#include <obs.h>
#include <windows.h>
#include <winhttp.h>

#include "util.hpp"

#pragma comment(lib, "winhttp.lib")

namespace hexbar {

static void CALLBACK CertIgnoreCallback(HINTERNET hRequest, DWORD_PTR, DWORD internetStatus, LPVOID, DWORD)
{
	if (internetStatus == WINHTTP_CALLBACK_STATUS_SENDING_REQUEST) {
		DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
			      SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
		WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
	}
}

bool Http::Get(const std::wstring &url, const std::string &basicAuth, bool ignoreCertErrors, int timeoutMs,
	       size_t maxBytes, HttpResponse &out, std::string &err)
{
	out = {};
	URL_COMPONENTS uc{};
	uc.dwStructSize = sizeof(uc);
	wchar_t host[256] = {};
	wchar_t path[2048] = {};
	uc.lpszHostName = host;
	uc.dwHostNameLength = 255;
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = 2047;
	if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
		err = "WinHttpCrackUrl failed";
		return false;
	}

	HINTERNET session = WinHttpOpen(L"lol-hexbar/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
					WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) {
		err = "WinHttpOpen failed";
		return false;
	}
	WinHttpSetTimeouts(session, 3000, 3000, timeoutMs, timeoutMs);

	HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
	if (!connect) {
		err = "WinHttpConnect failed";
		WinHttpCloseHandle(session);
		return false;
	}

	DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
	HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
					       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
	if (!request) {
		err = "WinHttpOpenRequest failed";
		WinHttpCloseHandle(connect);
		WinHttpCloseHandle(session);
		return false;
	}

	if (ignoreCertErrors) {
		// 实测(Win10/11 + schannel):证书错误发生在 SENDING_REQUEST 回调之前,
		// 必须在发送前预设忽略标志,回调仅作兜底
		DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
			      SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
		WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
		WinHttpSetStatusCallback(request, CertIgnoreCallback, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0);
	}

	if (!basicAuth.empty()) {
		std::wstring header = L"Authorization: " + util::Utf8ToWide(basicAuth);
		WinHttpAddRequestHeaders(request, header.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
	}

	BOOL sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
	bool ok = sent && WinHttpReceiveResponse(request, nullptr);
	if (!ok) {
		DWORD e = GetLastError();
		err = "HTTP request failed (" + std::to_string(e) + ")";
		blog(LOG_INFO, "[lol-hexbar] http fail: %ls -> %s", url.c_str(), err.c_str());
		WinHttpCloseHandle(request);
		WinHttpCloseHandle(connect);
		WinHttpCloseHandle(session);
		return false;
	}

	DWORD status = 0, size = sizeof(status);
	WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size,
			    nullptr);
	out.status = (long)status;

	for (;;) {
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(request, &available))
			break;
		if (available == 0)
			break;
		size_t cur = out.body.size();
		if (cur + available > maxBytes) {
			err = "response too large";
			WinHttpCloseHandle(request);
			WinHttpCloseHandle(connect);
			WinHttpCloseHandle(session);
			return false;
		}
		out.body.resize(cur + available);
		DWORD readBytes = 0;
		if (!WinHttpReadData(request, out.body.data() + cur, available, &readBytes))
			break;
		out.body.resize(cur + readBytes);
		if (readBytes == 0)
			break;
	}

	WinHttpCloseHandle(request);
	WinHttpCloseHandle(connect);
	WinHttpCloseHandle(session);
	return true;
}

} // namespace hexbar
