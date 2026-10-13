#include <windows.h>
#include <cstring>
#include <cwchar>

namespace {
	HMODULE OwnModule() {
		HMODULE module = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		    reinterpret_cast<LPCWSTR>(&OwnModule), &module);
		return module;
	}

	bool IsClient(const wchar_t* path) {
		if (!path)
			return false;
		const auto slash = wcsrchr(path, L'\\');
		const auto name = slash ? slash + 1 : path;
		return _wcsicmp(name, L"steamclient64.dll") == 0 || _wcsicmp(name, L"steamclient.dll") == 0;
	}
}

extern "C" {
	HMODULE WINAPI EvolveLoadLibraryW(LPCWSTR path) {
		if (IsClient(path)) {
			wchar_t own[32768]{};
			if (!GetModuleFileNameW(OwnModule(), own, 32768))
				return nullptr;
			return LoadLibraryW(own);
		}
		return LoadLibraryW(path);
	}

	HMODULE WINAPI EvolveLoadLibraryA(LPCSTR path) {
		if (path) {
			const auto slash = strrchr(path, '\\');
			const auto name = slash ? slash + 1 : path;
			if (_stricmp(name, "steamclient64.dll") == 0 || _stricmp(name, "steamclient.dll") == 0)
				return EvolveLoadLibraryW(L"steamclient64.dll");
		}
		return LoadLibraryA(path);
	}

	HMODULE WINAPI EvolveLoadLibraryExW(LPCWSTR path, HANDLE file, DWORD flags) {
		if (IsClient(path))
			return EvolveLoadLibraryW(L"steamclient64.dll");
		return LoadLibraryExW(path, file, flags);
	}

	HMODULE WINAPI EvolveLoadLibraryExA(LPCSTR path, HANDLE file, DWORD flags) {
		if (path) {
			const auto slash = strrchr(path, '\\');
			const auto name = slash ? slash + 1 : path;
			if (_stricmp(name, "steamclient64.dll") == 0 || _stricmp(name, "steamclient.dll") == 0)
				return EvolveLoadLibraryW(L"steamclient64.dll");
		}
		return LoadLibraryExA(path, file, flags);
	}

	FARPROC WINAPI EvolveGetProcAddress(HMODULE module, LPCSTR name) {
		if (reinterpret_cast<ULONG_PTR>(name) <= 65535)
			return GetProcAddress(module, name);
		const bool system = module == OwnModule() || module == GetModuleHandleW(L"kernel32.dll") ||
		                    module == GetModuleHandleW(L"kernelbase.dll") ||
		                    module == GetModuleHandleW(L"advapi32.dll") || module == GetModuleHandleW(L"winhttp.dll") ||
		                    module == GetModuleHandleW(L"wintrust.dll") || module == GetModuleHandleW(L"ws2_32.dll");
		if (!system)
			return GetProcAddress(module, name);
		if (strcmp(name, "GetProcAddress") == 0 || strcmp(name, "LoadLibraryA") == 0 ||
		    strcmp(name, "LoadLibraryW") == 0 || strcmp(name, "LoadLibraryExA") == 0 ||
		    strcmp(name, "LoadLibraryExW") == 0 || strcmp(name, "CreateFileA") == 0 ||
		    strcmp(name, "CreateFileW") == 0 || strcmp(name, "getaddrinfo") == 0 || strncmp(name, "WinHttp", 7) == 0 ||
		    strcmp(name, "WinVerifyTrust") == 0 || strcmp(name, "RegOpenKeyExA") == 0 ||
		    strcmp(name, "RegOpenKeyExW") == 0 || strcmp(name, "RegQueryValueExA") == 0 ||
		    strcmp(name, "RegQueryValueExW") == 0 || strcmp(name, "RegCloseKey") == 0) {
			const auto local = GetProcAddress(OwnModule(), name);
			if (local)
				return local;
		}
		return GetProcAddress(module, name);
	}
}
