#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace offline {
	struct Response {
		unsigned status = 404;
		std::string type = "application/json";
		std::string body = "{}";
	};

	void SetModule(HMODULE module);
	void PrepareBackendTrust();
	std::string RuntimeCertificatePem();
	bool IsProfileHost(std::wstring_view host);
	bool IsServiceHost(std::wstring_view host);
	Response Respond(std::wstring_view path, std::string_view body);
}
