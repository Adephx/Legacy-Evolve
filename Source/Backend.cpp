#include "Backend.h"
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <regex>
#include <stdexcept>

namespace offline {
	namespace {
		HMODULE resourceModule;
		std::mutex identityMutex;
		std::string identity = "76561197960265729";

		std::string Field(std::string_view input, const char* name, bool quoted) {
			const std::regex pattern(std::string("\"") + name + "\"\\s*:\\s*" + (quoted ? "\"([^\"]*)\"" : "([0-9]+)"));
			std::match_results<std::string_view::const_iterator> match;
			if (!std::regex_search(input.begin(), input.end(), match, pattern))
				return {};
			return {match[1].first, match[1].second};
		}

		void Replace(std::string& input, const char* name, const std::string& value, bool quoted) {
			const std::regex pattern(std::string("(\"") + name + "\"\\s*:\\s*)" + (quoted ? "\"[^\"]*\"" : "[0-9]+"));
			std::string result;
			std::size_t position = 0;
			for (std::sregex_iterator it(input.begin(), input.end(), pattern), end; it != end; ++it) {
				result.append(input, position, static_cast<std::size_t>(it->position()) - position);
				result += (*it)[1].str();
				result += quoted ? "\"" + value + "\"" : value;
				position = static_cast<std::size_t>(it->position() + it->length());
			}
			result.append(input, position, std::string::npos);
			input = std::move(result);
		}

		std::string Asset(const char* name) {
			std::string resourceName(name);
			for (auto& ch : resourceName)
				ch = ch == '.' ? '_' : (ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch);
			const auto resource = FindResourceA(resourceModule, resourceName.c_str(), MAKEINTRESOURCEA(10));
			const auto loaded = resource ? LoadResource(resourceModule, resource) : nullptr;
			const auto data = loaded ? LockResource(loaded) : nullptr;
			if (!data)
				throw std::runtime_error("Missing embedded response");
			return {static_cast<const char*>(data), SizeofResource(resourceModule, resource)};
		}

		bool EndsWith(std::wstring_view value, std::wstring_view suffix) {
			return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
		}
	}

	void SetModule(HMODULE module) {
		resourceModule = module;
	}

	void PrepareBackendTrust() {
		static std::once_flag prepared;
		std::call_once(prepared, [] {
			wchar_t executable[32768]{};
			if (!GetModuleFileNameW(nullptr, executable, 32768))
				return;
			const std::filesystem::path path(executable);
			if (_wcsicmp(path.filename().c_str(), L"Evolve.exe"))
				return;
			try {
				const auto bundle = path.parent_path().parent_path() / L"ca-bundle.crt";
				auto certificate = RuntimeCertificatePem();
				while (!certificate.empty() && (certificate.back() == '\r' || certificate.back() == '\n'))
					certificate.pop_back();
				if (certificate.empty())
					throw std::runtime_error("The embedded backend certificate is empty.");
				certificate += "\r\n";
				const auto temporary =
				    bundle.wstring() + L".legacy-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
				const auto file = CreateFileW(
				    temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (file == INVALID_HANDLE_VALUE)
					throw std::runtime_error(
					    "Could not prepare the local certificate. Check that the game directory is writable.");
				DWORD written = 0;
				const bool complete =
				    WriteFile(file, certificate.data(), static_cast<DWORD>(certificate.size()), &written, nullptr) &&
				    written == certificate.size() && FlushFileBuffers(file);
				CloseHandle(file);
				if (!complete || !MoveFileExW(temporary.c_str(), bundle.c_str(),
				                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
					DeleteFileW(temporary.c_str());
					throw std::runtime_error("Could not create or replace ca-bundle.crt.");
				}
			} catch (const std::exception& error) {
				MessageBoxA(nullptr, error.what(), "Legacy Evolve", MB_OK | MB_ICONERROR);
			}
		});
	}

	bool IsProfileHost(std::wstring_view input) {
		std::wstring host(input);
		std::transform(host.begin(), host.end(), host.begin(), [](wchar_t c) {
			return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + L'a' - L'A') : c;
		});
		while (!host.empty() && (host.back() == L'.' || host.back() == L'/'))
			host.pop_back();
		return host == L"my.2k.com" || EndsWith(host, L".my.2k.com") || host == L"evolve2k.com" ||
		       EndsWith(host, L".evolve2k.com");
	}

	bool IsServiceHost(std::wstring_view input) {
		if (IsProfileHost(input))
			return true;
		std::wstring host(input);
		std::transform(host.begin(), host.end(), host.begin(), [](wchar_t c) {
			return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + L'a' - L'A') : c;
		});
		while (!host.empty() && (host.back() == L'.' || host.back() == L'/'))
			host.pop_back();
		return host == L"4v1game.net" || EndsWith(host, L".4v1game.net") || host == L"evolve.hydra.agoragames.com";
	}

	Response Respond(std::wstring_view inputPath, std::string_view request) {
		const auto query = inputPath.find(L'?');
		auto path = inputPath.substr(0, query);
		while (!path.empty() && path.front() == L'/')
			path.remove_prefix(1);
		while (!path.empty() && path.back() == L'/')
			path.remove_suffix(1);
		const char* asset = nullptr;
		bool json = true;

		struct Route {
			std::wstring_view path;
			const char* asset;
		};

		static constexpr Route routes[] = {{L"doorman/1", "doorman.json"},
		    {L"telemetry/1", "telemetryconfiggenerate.json"}, {L"sessions/1", "sessions.json"},
		    {L"news/1", "news.json"}, {L"storage/1", "iteminsert.json"}, {L"sso/1", "applogon.json"},
		    {L"stats/1", "stats.json"}, {L"content/1", "stringsget.json"}, {L"auth.php", "SteamLogin.json"},
		    {L"requestserver.php", "requestserver.json"},
		    {L"Production/252019.252019.0/build_config_signed.json", "build_config_signed0.json"},
		    {L"Production/252019.252019/micro_patch_version_signed.json", "micro_patch_version_signed.json"},
		    {L"Production/252019.252019/micro_patch_252019.252019_31.pak", "micro_patch_252019.252019_31.pak"},
		    {L"Production/252019.252019.31/build_config_signed.json", "build_config_signed_31.json"}};
		for (const auto& route : routes) {
			if (path == route.path ||
			    (EndsWith(path, route.path) && path[path.size() - route.path.size() - 1] == L'/')) {
				asset = route.asset;
				json = route.path.substr(0, 11) != L"Production/";
				break;
			}
		}
		if (path == L"entitlements/1") {
			const auto action = Field(request, "action", true);
			if (action == "steamApi.user.checkAppOwnership")
				asset = "checkAppOwnership.json";
			else if (action == "entitlementDefs.getFirstPartyMapping")
				asset = "getFirstPartyMapping.json";
			else if (action == "grants.find")
				asset = "grantsfind.json";
		}
		if (!asset)
			return {200, "application/json", R"({"message":"OK"})"};
		Response response{200, json ? "application/json; charset=utf-8" : "application/octet-stream", Asset(asset)};
		if (!json)
			return response;

		auto suppliedId = Field(request, "firstPartyPlayerId", true);
		if (suppliedId.empty())
			suppliedId = Field(request, "steamid", true);
		std::string steamId;
		{
			std::lock_guard<std::mutex> lock(identityMutex);
			if (suppliedId.size() == 17 && suppliedId.find_first_not_of("0123456789") == std::string::npos)
				identity = suppliedId;
			steamId = identity;
		}
		const auto context = Field(request, "appContext", false);
		if (!context.empty())
			Replace(response.body, "appContext", context, false);
		const auto now = std::to_string(std::time(nullptr));
		Replace(response.body, "serverTime", now, false);
		Replace(response.body, "createdOn", now, false);
		Replace(response.body, "ownersteamid", steamId, true);
		return response;
	}
}
