#define _ADVAPI32_
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>

namespace {
	int steamRoot, steamActive;

	HKEY RootKey() {
		return reinterpret_cast<HKEY>(&steamRoot);
	}

	HKEY ActiveKey() {
		return reinterpret_cast<HKEY>(&steamActive);
	}

	bool IsSteamKey(HKEY key) {
		return key == RootKey() || key == ActiveKey();
	}

	FARPROC RegistryFunction(const char* name) {
		static const auto module = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		return module ? GetProcAddress(module, name) : nullptr;
	}

	std::wstring LocalModule() {
		HMODULE module = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		    reinterpret_cast<LPCWSTR>(&LocalModule), &module);
		wchar_t path[32768]{};
		GetModuleFileNameW(module, path, 32768);
		return path;
	}

	std::wstring LocalFolder() {
		const auto path = LocalModule();
		return path.substr(0, path.find_last_of(L'\\') + 1);
	}

	LSTATUS LocalOpen(HKEY key, const wchar_t* name, PHKEY result) {
		if (!name || !result)
			return ERROR_INVALID_PARAMETER;
		if ((key == HKEY_CURRENT_USER || key == HKEY_LOCAL_MACHINE) &&
		    (_wcsicmp(name, L"Software\\Valve\\Steam") == 0 ||
		        _wcsicmp(name, L"Software\\Wow6432Node\\Valve\\Steam") == 0)) {
			*result = RootKey();
			return ERROR_SUCCESS;
		}
		if (((key == HKEY_CURRENT_USER || key == HKEY_LOCAL_MACHINE) &&
		        (_wcsicmp(name, L"Software\\Valve\\Steam\\ActiveProcess") == 0 ||
		            _wcsicmp(name, L"Software\\Wow6432Node\\Valve\\Steam\\ActiveProcess") == 0)) ||
		    (key == RootKey() && _wcsicmp(name, L"ActiveProcess") == 0)) {
			*result = ActiveKey();
			return ERROR_SUCCESS;
		}
		if (IsSteamKey(key) && !*name) {
			*result = key;
			return ERROR_SUCCESS;
		}
		return ERROR_FILE_NOT_FOUND;
	}

	LSTATUS CopyRegistry(void* target, DWORD* capacity, const void* value, DWORD length) {
		if (!capacity)
			return ERROR_INVALID_PARAMETER;
		const auto supplied = *capacity;
		*capacity = length;
		if (!target)
			return ERROR_SUCCESS;
		if (supplied < length)
			return ERROR_MORE_DATA;
		memcpy(target, value, length);
		return ERROR_SUCCESS;
	}

	LSTATUS LocalQuery(const wchar_t* name, DWORD* type, BYTE* value, DWORD* size, bool wide) {
		if (!name)
			return ERROR_FILE_NOT_FOUND;
		if (_wcsicmp(name, L"pid") == 0 || _wcsicmp(name, L"ActiveUser") == 0) {
			DWORD number = GetCurrentProcessId();
			if (_wcsicmp(name, L"ActiveUser") == 0) {
				wchar_t id[32]{};
				const auto ini = LocalFolder() + L"steam_settings\\configs.user.ini";
				GetPrivateProfileStringW(
				    L"user::general", L"account_steamid", L"76561197960265729", id, 32, ini.c_str());
				number = static_cast<DWORD>(_wcstoui64(id, nullptr, 10));
			}
			if (type)
				*type = REG_DWORD;
			return CopyRegistry(value, size, &number, sizeof(number));
		}
		std::wstring text;
		if (_wcsicmp(name, L"SteamClientDll64") == 0)
			text = LocalModule();
		else if (_wcsicmp(name, L"SteamPath") == 0)
			text = LocalFolder();
		else
			return ERROR_FILE_NOT_FOUND;
		if (type)
			*type = REG_SZ;
		if (wide)
			return CopyRegistry(value, size, text.c_str(), static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
		const auto length = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (!length)
			return ERROR_NO_UNICODE_TRANSLATION;
		std::string narrow(static_cast<std::size_t>(length), '\0');
		WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, narrow.data(), length, nullptr, nullptr);
		return CopyRegistry(value, size, narrow.data(), static_cast<DWORD>(narrow.size()));
	}

	std::wstring Wide(const char* value) {
		if (!value)
			return {};
		const int length = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
		if (!length)
			return {};
		std::wstring text(static_cast<std::size_t>(length), L'\0');
		MultiByteToWideChar(CP_ACP, 0, value, -1, text.data(), length);
		text.pop_back();
		return text;
	}
}

extern "C" {
	LSTATUS WINAPI RegOpenKeyExW(HKEY key, LPCWSTR name, DWORD options, REGSAM access, PHKEY result) {
		const auto local = LocalOpen(key, name, result);
		if (local == ERROR_SUCCESS || IsSteamKey(key))
			return local;
		const auto real = reinterpret_cast<decltype(&RegOpenKeyExW)>(RegistryFunction("RegOpenKeyExW"));
		return real ? real(key, name, options, access, result) : ERROR_PROC_NOT_FOUND;
	}

	LSTATUS WINAPI RegOpenKeyExA(HKEY key, LPCSTR name, DWORD options, REGSAM access, PHKEY result) {
		const auto text = Wide(name);
		const auto local = LocalOpen(key, name ? text.c_str() : nullptr, result);
		if (local == ERROR_SUCCESS || IsSteamKey(key))
			return local;
		const auto real = reinterpret_cast<decltype(&RegOpenKeyExA)>(RegistryFunction("RegOpenKeyExA"));
		return real ? real(key, name, options, access, result) : ERROR_PROC_NOT_FOUND;
	}

	LSTATUS WINAPI RegQueryValueExW(
	    HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE value, LPDWORD size) {
		if (IsSteamKey(key))
			return LocalQuery(name, type, value, size, true);
		const auto real = reinterpret_cast<decltype(&RegQueryValueExW)>(RegistryFunction("RegQueryValueExW"));
		return real ? real(key, name, reserved, type, value, size) : ERROR_PROC_NOT_FOUND;
	}

	LSTATUS WINAPI RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE value, LPDWORD size) {
		if (IsSteamKey(key)) {
			const auto text = Wide(name);
			return LocalQuery(name ? text.c_str() : nullptr, type, value, size, false);
		}
		const auto real = reinterpret_cast<decltype(&RegQueryValueExA)>(RegistryFunction("RegQueryValueExA"));
		return real ? real(key, name, reserved, type, value, size) : ERROR_PROC_NOT_FOUND;
	}

	LSTATUS WINAPI RegCloseKey(HKEY key) {
		if (IsSteamKey(key))
			return ERROR_SUCCESS;
		const auto real = reinterpret_cast<decltype(&RegCloseKey)>(RegistryFunction("RegCloseKey"));
		return real ? real(key) : ERROR_PROC_NOT_FOUND;
	}

	HMODULE SteamRuntime() {
		static const auto module = LoadLibraryExW((LocalFolder() + L"steam_api64.dll").c_str(), nullptr,
		    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
		return module;
	}

	void* __cdecl CreateInterface(const char* version, int* result) {
		const auto steam = SteamRuntime();
		using Create = void*(__cdecl*)(const char*);
		using Init = bool(__cdecl*)();
		const auto create =
		    steam ? reinterpret_cast<Create>(GetProcAddress(steam, "SteamInternal_CreateInterface")) : nullptr;
		const auto init = steam ? reinterpret_cast<Init>(GetProcAddress(steam, "SteamAPI_Init")) : nullptr;
		const bool compatible = steam && GetProcAddress(steam, "SteamAPI_ISteamClient_GetISteamUser");
		static const bool initialized = compatible && init && init();
		auto value = create && initialized && version ? create(version) : nullptr;
		if (result)
			*result = value ? 0 : 1;
		return value;
	}

	bool __cdecl Steam_BGetCallback(int pipe, void* message) {
		const auto steam = SteamRuntime();
		using Init = void(__cdecl*)();
		using Frame = void(__cdecl*)(int);
		using Next = bool(__cdecl*)(int, void*);
		const auto init = reinterpret_cast<Init>(GetProcAddress(steam, "SteamAPI_ManualDispatch_Init"));
		const auto frame = reinterpret_cast<Frame>(GetProcAddress(steam, "SteamAPI_ManualDispatch_RunFrame"));
		const auto next = reinterpret_cast<Next>(GetProcAddress(steam, "SteamAPI_ManualDispatch_GetNextCallback"));
		if (!init || !frame || !next)
			return false;
		init();
		frame(pipe);
		return next(pipe, message);
	}

	void __cdecl Steam_FreeLastCallback(int pipe) {
		using Free = void(__cdecl*)(int);
		const auto free =
		    reinterpret_cast<Free>(GetProcAddress(SteamRuntime(), "SteamAPI_ManualDispatch_FreeLastCallback"));
		if (free)
			free(pipe);
	}

	bool __cdecl Steam_GetAPICallResult(
	    int pipe, std::uint64_t call, void* data, int size, int expected, bool* failed) {
		using Result = bool(__cdecl*)(int, std::uint64_t, void*, int, int, bool*);
		const auto result =
		    reinterpret_cast<Result>(GetProcAddress(SteamRuntime(), "SteamAPI_ManualDispatch_GetAPICallResult"));
		return result && result(pipe, call, data, size, expected, failed);
	}
}
