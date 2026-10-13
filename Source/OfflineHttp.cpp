#define _WINHTTP_INTERNAL_
#define _WINHTTP_
#include <windows.h>
#include <winhttp.h>
#include <wintrust.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "Backend.h"

namespace {
	HMODULE httpModule;
	std::once_flag httpOnce;

	FARPROC HttpFunction(const char* name) {
		std::call_once(httpOnce, [] {
			httpModule = LoadLibraryExW(L"winhttp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		});
		return httpModule ? GetProcAddress(httpModule, name) : nullptr;
	}

#define REAL(name) reinterpret_cast<decltype(&name)>(HttpFunction(#name))

	BOOL Fail(DWORD error) {
		SetLastError(error);
		return FALSE;
	}

	enum class Kind { Session, Connection, Request };
	enum class Phase { Open, Sending, Sent, Receiving, Ready, Reading };
	struct Handle;
	using State = std::shared_ptr<Handle>;

	struct Handle {
		HINTERNET value = nullptr;
		Kind kind = Kind::Session;
		State parent;
		bool local = false;
		bool async = false;
		std::atomic<bool> closed{false};
		std::mutex mutex;
		WINHTTP_STATUS_CALLBACK callback = nullptr;
		DWORD notifications = 0;
		DWORD_PTR context = 0;
		std::wstring host, path;
		std::string body;
		DWORD total = 0;
		offline::Response response;
		std::size_t position = 0;
		Phase phase = Phase::Open;
		std::mutex queueMutex;
		std::deque<std::function<void()>> queue;
		bool working = false;
	};

	std::mutex handlesMutex;
	std::unordered_map<HINTERNET, State> handles;

	State Lookup(HINTERNET value) {
		std::lock_guard<std::mutex> lock(handlesMutex);
		const auto found = handles.find(value);
		return found == handles.end() ? nullptr : found->second;
	}

	void Remember(const State& state) {
		std::lock_guard<std::mutex> lock(handlesMutex);
		handles.emplace(state->value, state);
	}

	State Child(const State& parent, Kind kind, bool local) {
		auto state = std::make_shared<Handle>();
		state->parent = parent;
		state->kind = kind;
		state->local = local;
		if (parent) {
			std::lock_guard<std::mutex> lock(parent->mutex);
			state->async = parent->async;
			state->callback = parent->callback;
			state->notifications = parent->notifications;
			state->host = parent->host;
		}
		return state;
	}

	void Notify(const State& state, DWORD status, void* data = nullptr, DWORD bytes = 0) {
		WINHTTP_STATUS_CALLBACK callback;
		DWORD mask;
		DWORD_PTR context;
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			callback = state->callback;
			mask = state->notifications;
			context = state->context;
		}
		if (callback && (mask & status) && (!state->closed || status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING))
			callback(state->value, context, status, data, bytes);
	}

	DWORD WINAPI Work(void* parameter) {
		std::unique_ptr<State> owner(static_cast<State*>(parameter));
		auto state = *owner;
		for (;;) {
			std::function<void()> action;
			{
				std::lock_guard<std::mutex> lock(state->queueMutex);
				if (state->queue.empty()) {
					state->working = false;
					return 0;
				}
				action = std::move(state->queue.front());
				state->queue.pop_front();
			}
			try {
				action();
			} catch (...) {
				WINHTTP_ASYNC_RESULT error{API_RECEIVE_RESPONSE, ERROR_WINHTTP_INTERNAL_ERROR};
				Notify(state, WINHTTP_CALLBACK_STATUS_REQUEST_ERROR, &error, sizeof(error));
			}
		}
	}

	bool Queue(const State& state, std::function<void()> action) {
		std::lock_guard<std::mutex> lock(state->queueMutex);
		state->queue.push_back(std::move(action));
		if (!state->working) {
			auto owner = std::make_unique<State>(state);
			if (!QueueUserWorkItem(Work, owner.get(), WT_EXECUTEDEFAULT)) {
				state->queue.pop_back();
				return false;
			}
			owner.release();
			state->working = true;
		}
		return true;
	}

	BOOL Run(const State& state, std::function<void()> action) {
		if (state->async)
			return Queue(state, std::move(action)) ? TRUE : Fail(ERROR_NOT_ENOUGH_MEMORY);
		try {
			action();
			return TRUE;
		} catch (...) {
			return Fail(ERROR_WINHTTP_INTERNAL_ERROR);
		}
	}

	BOOL CopyValue(void* destination, DWORD* size, const void* source, DWORD bytes) {
		if (!size)
			return Fail(ERROR_INVALID_PARAMETER);
		if (!destination || *size < bytes) {
			*size = bytes;
			return Fail(ERROR_INSUFFICIENT_BUFFER);
		}
		memcpy(destination, source, bytes);
		*size = bytes;
		return TRUE;
	}

	BOOL CopyText(void* destination, DWORD* size, const std::wstring& value) {
		const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
		if (!CopyValue(destination, size, value.c_str(), bytes))
			return FALSE;
		*size -= sizeof(wchar_t);
		return TRUE;
	}

	bool RequestReady(const State& state) {
		return state->kind == Kind::Request && state->phase == Phase::Ready && !state->closed;
	}
}

extern "C" {
	HINTERNET WINAPI WinHttpOpen(LPCWSTR agent, DWORD access, LPCWSTR proxy, LPCWSTR bypass, DWORD flags) {
		const auto real = REAL(WinHttpOpen);
		if (!real) {
			Fail(ERROR_MOD_NOT_FOUND);
			return nullptr;
		}
		auto state = Child(nullptr, Kind::Session, false);
		state->value = real(agent, access, proxy, bypass, flags);
		if (!state->value)
			return nullptr;
		state->async = (flags & WINHTTP_FLAG_ASYNC) != 0;
		Remember(state);
		return state->value;
	}

	HINTERNET WINAPI WinHttpConnect(HINTERNET session, LPCWSTR host, INTERNET_PORT port, DWORD reserved) {
		const auto parent = Lookup(session);
		if (!parent || !host || !offline::IsServiceHost(host)) {
			const auto real = REAL(WinHttpConnect);
			const auto value = real ? real(session, host, port, reserved) : nullptr;
			if (value && parent) {
				auto state = Child(parent, Kind::Connection, false);
				state->value = value;
				Remember(state);
			}
			return value;
		}
		if (parent->kind != Kind::Session || parent->closed) {
			Fail(ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
			return nullptr;
		}
		auto state = Child(parent, Kind::Connection, true);
		state->host = host;
		state->value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!state->value)
			return nullptr;
		Remember(state);
		auto value = state->value;
		Notify(parent, WINHTTP_CALLBACK_STATUS_HANDLE_CREATED, &value, sizeof(value));
		return value;
	}

	HINTERNET WINAPI WinHttpOpenRequest(HINTERNET connection, LPCWSTR verb, LPCWSTR path, LPCWSTR version,
	    LPCWSTR referrer, LPCWSTR* accepts, DWORD flags) {
		const auto parent = Lookup(connection);
		if (!parent || !parent->local) {
			const auto real = REAL(WinHttpOpenRequest);
			const auto value = real ? real(connection, verb, path, version, referrer, accepts, flags) : nullptr;
			if (value && parent) {
				auto state = Child(parent, Kind::Request, false);
				state->value = value;
				Remember(state);
			}
			return value;
		}
		if (parent->kind != Kind::Connection || parent->closed) {
			Fail(ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
			return nullptr;
		}
		auto state = Child(parent, Kind::Request, true);
		state->path = path ? path : L"/";
		state->value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!state->value)
			return nullptr;
		Remember(state);
		auto value = state->value;
		Notify(parent, WINHTTP_CALLBACK_STATUS_HANDLE_CREATED, &value, sizeof(value));
		return value;
	}

	WINHTTP_STATUS_CALLBACK WINAPI WinHttpSetStatusCallback(
	    HINTERNET value, WINHTTP_STATUS_CALLBACK callback, DWORD flags, DWORD_PTR reserved) {
		auto state = Lookup(value);
		if (!state || !state->local) {
			const auto real = REAL(WinHttpSetStatusCallback);
			auto previous = real ? real(value, callback, flags, reserved) : WINHTTP_INVALID_STATUS_CALLBACK;
			if (previous == WINHTTP_INVALID_STATUS_CALLBACK)
				return previous;
			if (state) {
				std::lock_guard<std::mutex> lock(state->mutex);
				state->callback = callback;
				state->notifications = flags;
			}
			return previous;
		}
		std::lock_guard<std::mutex> lock(state->mutex);
		const auto previous = state->callback;
		state->callback = callback;
		state->notifications = flags;
		return previous;
	}

	BOOL WINAPI WinHttpSendRequest(HINTERNET value, LPCWSTR headers, DWORD headerLength, void* optional,
	    DWORD optionalLength, DWORD totalLength, DWORD_PTR context) {
		auto state = Lookup(value);
		if (!state || !state->local)
			return REAL(WinHttpSendRequest)(
			    value, headers, headerLength, optional, optionalLength, totalLength, context);
		if ((!optional && optionalLength) || optionalLength > 32 * 1024 * 1024 || totalLength < optionalLength)
			return Fail(ERROR_INVALID_PARAMETER);
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (state->kind != Kind::Request || state->closed ||
			    (state->phase != Phase::Open && state->phase != Phase::Ready))
				return Fail(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
			state->body.assign(optionalLength ? static_cast<const char*>(optional) : "", optionalLength);
			state->total = totalLength;
			if (context)
				state->context = context;
			state->position = 0;
			state->phase = Phase::Sending;
		}
		return Run(state, [state] {
			if (state->closed)
				return;
			{
				std::lock_guard<std::mutex> lock(state->mutex);
				state->phase = Phase::Sent;
			}
			if (state->async)
				Notify(state, WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);
		});
	}

	BOOL WINAPI WinHttpWriteData(HINTERNET value, LPCVOID buffer, DWORD bytes, LPDWORD written) {
		auto state = Lookup(value);
		if (!state || !state->local)
			return REAL(WinHttpWriteData)(value, buffer, bytes, written);
		if ((!buffer && bytes) || bytes > 32 * 1024 * 1024)
			return Fail(ERROR_INVALID_PARAMETER);
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (state->phase != Phase::Sent || state->body.size() + bytes > state->total)
				return Fail(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
			if (bytes)
				state->body.append(static_cast<const char*>(buffer), bytes);
		}
		if (written)
			*written = bytes;
		if (!state->async)
			return TRUE;
		return Run(state, [state, bytes] {
			DWORD count = bytes;
			Notify(state, WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE, &count, sizeof(count));
		});
	}

	BOOL WINAPI WinHttpReceiveResponse(HINTERNET value, LPVOID reserved) {
		auto state = Lookup(value);
		if (!state || !state->local)
			return REAL(WinHttpReceiveResponse)(value, reserved);
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (state->phase != Phase::Sent || state->body.size() != state->total)
				return Fail(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
			state->phase = Phase::Receiving;
		}
		return Run(state, [state] {
			if (state->closed)
				return;
			auto response = offline::Respond(state->path, state->body);
			{
				std::lock_guard<std::mutex> lock(state->mutex);
				state->response = std::move(response);
				state->phase = Phase::Ready;
			}
			if (state->async)
				Notify(state, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE);
		});
	}

	BOOL WINAPI WinHttpReadData(HINTERNET value, LPVOID buffer, DWORD bytes, LPDWORD read) {
		auto state = Lookup(value);
		if (!state || !state->local)
			return REAL(WinHttpReadData)(value, buffer, bytes, read);
		if ((!buffer && bytes) || (!state->async && !read))
			return Fail(ERROR_INVALID_PARAMETER);
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (!RequestReady(state))
				return Fail(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
			state->phase = Phase::Reading;
		}
		if (read)
			*read = 0;
		return Run(state, [state, buffer, bytes, read] {
			if (state->closed)
				return;
			DWORD count;
			{
				std::lock_guard<std::mutex> lock(state->mutex);
				count = static_cast<DWORD>(std::min<std::size_t>(bytes, state->response.body.size() - state->position));
				if (count)
					memcpy(buffer, state->response.body.data() + state->position, count);
				state->position += count;
				state->phase = Phase::Ready;
			}
			if (!state->async && read)
				*read = count;
			if (state->async)
				Notify(state, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, buffer, count);
		});
	}

	BOOL WINAPI WinHttpQueryDataAvailable(HINTERNET value, LPDWORD available) {
		auto state = Lookup(value);
		if (!state || !state->local)
			return REAL(WinHttpQueryDataAvailable)(value, available);
		DWORD count;
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (!RequestReady(state))
				return Fail(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
			count = static_cast<DWORD>(state->response.body.size() - state->position);
		}
		if (!state->async) {
			if (!available)
				return Fail(ERROR_INVALID_PARAMETER);
			*available = count;
			return TRUE;
		}
		if (available)
			*available = 0;
		return Run(state, [state, count] {
			DWORD bytes = count;
			Notify(state, WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, &bytes, sizeof(bytes));
		});
	}

	BOOL WINAPI WinHttpQueryHeaders(
	    HINTERNET value, DWORD level, LPCWSTR name, LPVOID buffer, LPDWORD size, LPDWORD index) {
		auto state = Lookup(value);
		if (!state || !state->local)
			return REAL(WinHttpQueryHeaders)(value, level, name, buffer, size, index);
		std::lock_guard<std::mutex> lock(state->mutex);
		if (!RequestReady(state))
			return Fail(ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
		if (index && *index != 0)
			return Fail(ERROR_WINHTTP_HEADER_NOT_FOUND);
		const auto field = level & 0x0000ffffu;
		const auto& response = state->response;
		const std::wstring type(response.type.begin(), response.type.end());
		const std::wstring status = std::to_wstring(response.status);
		const std::wstring length = std::to_wstring(response.body.size());
		const std::wstring statusText = response.status == 200 ? L"OK" : L"Not Found";
		std::wstring text;
		if (field == WINHTTP_QUERY_STATUS_CODE)
			text = status;
		else if (field == WINHTTP_QUERY_STATUS_TEXT)
			text = statusText;
		else if (field == WINHTTP_QUERY_CONTENT_LENGTH)
			text = length;
		else if (field == WINHTTP_QUERY_CONTENT_TYPE)
			text = type;
		else if (field == WINHTTP_QUERY_VERSION)
			text = L"HTTP/1.1";
		else if (field == WINHTTP_QUERY_RAW_HEADERS_CRLF || field == WINHTTP_QUERY_RAW_HEADERS) {
			text = L"HTTP/1.1 " + status + L" " + statusText + L"\r\nContent-Type: " + type + L"\r\nContent-Length: " +
			       length + L"\r\n\r\n";
			if (field == WINHTTP_QUERY_RAW_HEADERS) {
				std::wstring raw;
				for (auto ch : text) {
					if (ch == L'\r')
						continue;
					raw += ch == L'\n' ? L'\0' : ch;
				}
				text = std::move(raw);
			}
		} else if (field == WINHTTP_QUERY_CUSTOM && name) {
			if (_wcsicmp(name, L"Content-Type") == 0)
				text = type;
			else if (_wcsicmp(name, L"Content-Length") == 0)
				text = length;
			else
				return Fail(ERROR_WINHTTP_HEADER_NOT_FOUND);
		} else
			return Fail(ERROR_WINHTTP_HEADER_NOT_FOUND);
		BOOL result;
		if (level & WINHTTP_QUERY_FLAG_NUMBER) {
			if (field != WINHTTP_QUERY_STATUS_CODE && field != WINHTTP_QUERY_CONTENT_LENGTH)
				return Fail(ERROR_WINHTTP_INVALID_QUERY_REQUEST);
			const DWORD number =
			    field == WINHTTP_QUERY_STATUS_CODE ? response.status : static_cast<DWORD>(response.body.size());
			result = CopyValue(buffer, size, &number, sizeof(number));
		} else
			result = CopyText(buffer, size, text);
		if (result && index)
			++*index;
		return result;
	}

	BOOL WINAPI WinHttpAddRequestHeaders(HINTERNET value, LPCWSTR headers, DWORD length, DWORD modifiers) {
		auto state = Lookup(value);
		if (!state || !state->local)
			return REAL(WinHttpAddRequestHeaders)(value, headers, length, modifiers);
		return state->kind == Kind::Request ? TRUE : Fail(ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
	}

	BOOL WINAPI WinHttpSetTimeouts(HINTERNET value, int resolve, int connect, int send, int receive) {
		auto state = Lookup(value);
		if (!state || !state->local)
			return REAL(WinHttpSetTimeouts)(value, resolve, connect, send, receive);
		return resolve < -1 || connect < -1 || send < -1 || receive < -1 ? Fail(ERROR_INVALID_PARAMETER) : TRUE;
	}

	BOOL WINAPI WinHttpSetOption(HINTERNET value, DWORD option, LPVOID buffer, DWORD size) {
		auto state = Lookup(value);
		if (!state || !state->local) {
			const auto result = REAL(WinHttpSetOption)(value, option, buffer, size);
			if (result && state && option == WINHTTP_OPTION_CONTEXT_VALUE && buffer && size == sizeof(DWORD_PTR)) {
				std::lock_guard<std::mutex> lock(state->mutex);
				state->context = *static_cast<DWORD_PTR*>(buffer);
			}
			return result;
		}
		if (option == WINHTTP_OPTION_CONTEXT_VALUE && buffer && size == sizeof(DWORD_PTR)) {
			std::lock_guard<std::mutex> lock(state->mutex);
			state->context = *static_cast<DWORD_PTR*>(buffer);
			return TRUE;
		}
		switch (option) {
		case WINHTTP_OPTION_SECURITY_FLAGS:
		case WINHTTP_OPTION_DISABLE_FEATURE:
		case WINHTTP_OPTION_RESOLVE_TIMEOUT:
		case WINHTTP_OPTION_CONNECT_TIMEOUT:
		case WINHTTP_OPTION_SEND_TIMEOUT:
		case WINHTTP_OPTION_RECEIVE_TIMEOUT:
			return buffer && size == sizeof(DWORD) ? TRUE : Fail(ERROR_INVALID_PARAMETER);
		default:
			return Fail(ERROR_WINHTTP_INVALID_OPTION);
		}
	}

	BOOL WINAPI WinHttpQueryOption(HINTERNET value, DWORD option, LPVOID buffer, LPDWORD size) {
		auto state = Lookup(value);
		if (!state || !state->local)
			return REAL(WinHttpQueryOption)(value, option, buffer, size);
		std::lock_guard<std::mutex> lock(state->mutex);
		if (option == WINHTTP_OPTION_CONTEXT_VALUE)
			return CopyValue(buffer, size, &state->context, sizeof(state->context));
		if (option == WINHTTP_OPTION_HANDLE_TYPE) {
			const DWORD kind = state->kind == Kind::Request ? WINHTTP_HANDLE_TYPE_REQUEST : WINHTTP_HANDLE_TYPE_CONNECT;
			return CopyValue(buffer, size, &kind, sizeof(kind));
		}
		if (option == WINHTTP_OPTION_URL)
			return CopyText(buffer, size, L"https://" + state->host + state->path);
		return Fail(ERROR_WINHTTP_INVALID_OPTION);
	}

	BOOL WINAPI WinHttpCloseHandle(HINTERNET value) {
		const auto root = Lookup(value);
		if (!root)
			return REAL(WinHttpCloseHandle)(value);
		std::vector<State> removed;
		{
			std::lock_guard<std::mutex> lock(handlesMutex);
			for (auto it = handles.begin(); it != handles.end();) {
				auto parent = it->second;
				while (parent && parent != root)
					parent = parent->parent;
				if (parent) {
					it->second->closed = true;
					removed.push_back(it->second);
					it = handles.erase(it);
				} else
					++it;
			}
		}
		for (const auto& state : removed) {
			if (!state->local)
				continue;
			const auto close = [state] {
				auto handle = state->value;
				Notify(state, WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING, &handle, sizeof(handle));
				CloseHandle(state->value);
			};
			if (!Queue(state, close))
				close();
		}
		return root->local ? TRUE : REAL(WinHttpCloseHandle)(value);
	}

	BOOL WINAPI WinHttpCheckPlatform() {
		const auto real = REAL(WinHttpCheckPlatform);
		return real ? real() : FALSE;
	}

	LONG WINAPI WinVerifyTrust(HWND window, GUID* action, LPVOID data) {
		auto trust = static_cast<WINTRUST_DATA*>(data);
		if (trust && trust->cbStruct >= FIELD_OFFSET(WINTRUST_DATA, dwStateAction) &&
		    trust->dwUnionChoice == WTD_CHOICE_FILE && trust->pFile && trust->pFile->pcwszFilePath) {
			wchar_t current[32768]{}, target[32768]{};
			GetModuleFileNameW(nullptr, current, 32768);
			GetFullPathNameW(trust->pFile->pcwszFilePath, 32768, target, nullptr);
			const auto name = wcsrchr(current, L'\\');
			if (name && _wcsicmp(name + 1, L"Evolve.exe") == 0) {
				if (_wcsicmp(current, target) == 0)
					return ERROR_SUCCESS;
				const auto apiPath = std::wstring(current, name + 1) + L"steam_api64.dll";
				if (_wcsicmp(apiPath.c_str(), target) == 0)
					return ERROR_SUCCESS;
				HMODULE support = nullptr;
				GetModuleHandleExW(
				    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				    reinterpret_cast<LPCWSTR>(&WinVerifyTrust), &support);
				wchar_t supportPath[32768]{};
				if (support && GetModuleFileNameW(support, supportPath, 32768) && _wcsicmp(supportPath, target) == 0)
					return ERROR_SUCCESS;
			}
		}
		static const auto module = LoadLibraryExW(L"wintrust.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		using Verify = LONG(WINAPI*)(HWND, GUID*, LPVOID);
		static const auto real = module ? reinterpret_cast<Verify>(GetProcAddress(module, "WinVerifyTrust")) : nullptr;
		return real ? real(window, action, data) : TRUST_E_PROVIDER_UNKNOWN;
	}
}

void InstallBackendResolver();

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		offline::SetModule(module);
		InstallBackendResolver();
		SetEnvironmentVariableW(L"SteamAppId", L"273350");
		SetEnvironmentVariableW(L"SteamGameId", L"273350");
	}
	return TRUE;
}
