#include <windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include "Backend.h"

namespace {
	using AddWarning = unsigned(__fastcall*)(void*, unsigned, void*, const char*, bool);
	using MenuAction = void(__fastcall*)(void*);
	using MenuCheck = bool(__fastcall*)(void*);
	using HiddenButton = bool(__fastcall*)(void*, void**);
	AddWarning originalWarning = nullptr;
	MenuAction originalSignInComplete = nullptr;
	MenuAction originalVideo = nullptr;
	HiddenButton originalHiddenButton = nullptr;
	BYTE* gameImage = nullptr;
	bool localSessionReady = false;
	bool WriteCode(void* target, const void* bytes, std::size_t size);

	void HideMenuElement(void* menu, const char* function) {
		alignas(8) BYTE arguments[0x30]{};
		const bool visible = false;
		reinterpret_cast<MenuAction>(gameImage + 0x49d1b8)(arguments);
		reinterpret_cast<void(__fastcall*)(void*, const bool*, int)>(gameImage + 0x4acd64)(arguments, &visible, 6);
		reinterpret_cast<bool(__fastcall*)(void*, const char*, void*, bool, void*)>(gameImage + 0x1b30954)(
		    menu, function, arguments, true, nullptr);
		reinterpret_cast<MenuAction>(gameImage + 0x49d7d4)(arguments);
	}

	void CleanMainMenu(void* menu) {
		HideMenuElement(menu, "setNewsVisibility");
	}

	const char* __fastcall AchievementsAttribute(void* node, const char* name) {
		using GetAttribute = const char*(__fastcall*)(void*, const char*);
		const auto original = reinterpret_cast<GetAttribute>(gameImage + 0x417a48);
		if (name && (!strcmp(name, "display_name") || !strcmp(name, "description")) &&
		    !strcmp(original(node, "command"), "OpenProfile"))
			return !strcmp(name, "display_name") ? "ACHIEVEMENTS" : "View your earned achievements and progress.";
		return original(node, name);
	}

	bool __fastcall AchievementsLabel(void* manager, const char* label, void* output, bool english, void* info) {
		using LocalizeLabel = bool(__fastcall*)(void*, const char*, void*, bool, void*);
		const bool renamed = label && _stricmp(label, "@ui_Profile") == 0;
		if (renamed)
			label = "Achievements";
		const bool translated =
		    reinterpret_cast<LocalizeLabel>(gameImage + 0x450fa4)(manager, label, output, english, info);
		return renamed || translated;
	}

	bool __fastcall HideRetiredButton(void* menu, void** reference) {
		void* node = *reference;
		if (node) {
			const auto table = *static_cast<void***>(node);
			const auto command =
			    reinterpret_cast<const char*(__fastcall*)(void*, const char*)>(table[0xe8 / 8])(node, "command");
			if (command && (!strcmp(command, "Store") || !strcmp(command, "My2k") || !strcmp(command, "UnlinkMy2K"))) {
				reinterpret_cast<MenuAction>(table[0x20 / 8])(node);
				return true;
			}
		}
		return originalHiddenButton(menu, reference);
	}

	bool SuppressWarning(unsigned id, const char* parameter) {
		if (id == 0x7dcfe389 || id == 0x82fb7d69 || id == 0xfa014652)
			return true;
		return id == 0x1c220346 && parameter && strcmp(parameter, "@ui_menu_lobby_error_0") == 0;
	}

	unsigned __fastcall AddLocalWarning(
	    void* manager, unsigned id, void* listener, const char* parameter, bool immediate) {
		if (SuppressWarning(id, parameter))
			return 0;
		return originalWarning(manager, id, listener, parameter, immediate);
	}

	void __fastcall CompleteLocalSignIn(void* menu) {
		originalSignInComplete(menu);
		CleanMainMenu(menu);
		constexpr BYTE keepSession[] = {0x32, 0xc0, 0x90, 0x90, 0x90};
		localSessionReady = WriteCode(gameImage + 0x16c0007, keepSession, sizeof(keepSession));
	}

	bool __fastcall OpenLocalMenu(void* object) {
		auto menu = static_cast<BYTE*>(object);
		if (localSessionReady)
			menu[0x86da] = 0;
		const bool opened = reinterpret_cast<MenuCheck>(gameImage + 0x16bfeb4)(menu);
		if (opened && !localSessionReady && menu[0x86da] && !menu[0x86dc] && !menu[0x86dd]) {
			const auto game = *reinterpret_cast<BYTE**>(gameImage + 0x344e8b8);
			reinterpret_cast<void(__fastcall*)(bool)>(gameImage + 0x1b412d0)(false);
			menu[0x879a] = 1;
			if (*reinterpret_cast<int*>(game + 0x150) == 15)
				reinterpret_cast<MenuAction>(gameImage + 0x17fb648)(game);
			else
				reinterpret_cast<MenuAction>(gameImage + 0x16bc2f0)(menu);
		}
		if (opened)
			CleanMainMenu(menu);
		return opened;
	}

	bool __fastcall OpenAccolades(void* menu) {
		const bool opened = reinterpret_cast<MenuCheck>(gameImage + 0x1687fc0)(menu);
		if (opened) {
			reinterpret_cast<void(__fastcall*)(int)>(gameImage + 0x1685d90)(1);
			HideMenuElement(menu, "setTabMenuVisible");
		}
		return opened;
	}

	void __fastcall AccoladesEvent(void* listener, void* event) {
		const auto table = *static_cast<void***>(event);
		const bool expanding =
		    reinterpret_cast<bool(__fastcall*)(void*, const char*)>(table[0x30 / 8])(event, "open_comm_menu");
		reinterpret_cast<void(__fastcall*)(void*, void*)>(gameImage + 0x16893cc)(listener, event);
		if (expanding) {
			reinterpret_cast<void(__fastcall*)(int)>(gameImage + 0x1685d90)(1);
			HideMenuElement(static_cast<BYTE*>(listener) - 8, "setTabMenuVisible");
		}
	}

	void __fastcall SkipVideo(void* video) {
		if (static_cast<BYTE*>(video)[0x8a]) {
			originalVideo(video);
			return;
		}
		reinterpret_cast<void(__fastcall*)(void*, bool)>(gameImage + 0x170ebec)(video, true);
	}

	void __fastcall UpdateClientSync(BYTE* state, float elapsed) {
		static bool rankedReady = false;
		const bool wasClosing = state[0x10] != 0;
		if (!wasClosing)
			rankedReady = false;
		reinterpret_cast<void(__fastcall*)(BYTE*, float)>(gameImage + 0x14ab5c8)(state, elapsed);
		const auto actor = reinterpret_cast<BYTE*(__fastcall*)()>(gameImage + 0x19fed88)();
		const auto data = actor ? *reinterpret_cast<BYTE**>(actor + 0x2090) : nullptr;
		if (!data)
			return;
		using Request = void(__fastcall*)(void*, bool);
		if (!wasClosing && state[0x10] && !data[0xc8])
			reinterpret_cast<Request>(gameImage + 0x15e6604)(data, true);
		if (*reinterpret_cast<int*>(state + 0x14) == 2 && !rankedReady) {
			rankedReady = true;
			if (!data[0xca])
				reinterpret_cast<Request>(gameImage + 0x15e65a0)(data, true);
		}
	}

	void AbsoluteJump(BYTE* output, const void* address) {
		const BYTE instruction[] = {0xff, 0x25, 0, 0, 0, 0};
		memcpy(output, instruction, sizeof(instruction));
		memcpy(output + 6, &address, sizeof(address));
	}

	bool WriteCode(void* target, const void* bytes, std::size_t size) {
		DWORD old = 0;
		if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &old))
			return false;
		memcpy(target, bytes, size);
		FlushInstructionCache(GetCurrentProcess(), target, size);
		VirtualProtect(target, size, old, &old);
		return true;
	}

	template <std::size_t N, typename Function>
	bool Detour(BYTE* target, const BYTE (&expected)[N], Function replacement, Function& original) {
		static_assert(N >= 14);
		if (original)
			return true;
		if (memcmp(target, expected, N) != 0)
			return false;
		auto trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr, N + 14, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		if (!trampoline)
			return false;
		memcpy(trampoline, expected, N);
		AbsoluteJump(trampoline + N, target + N);
		DWORD old = 0;
		if (!VirtualProtect(trampoline, N + 14, PAGE_EXECUTE_READ, &old)) {
			VirtualFree(trampoline, 0, MEM_RELEASE);
			return false;
		}
		FlushInstructionCache(GetCurrentProcess(), trampoline, N + 14);
		BYTE jump[N];
		memset(jump, 0x90, N);
		AbsoluteJump(jump, reinterpret_cast<const void*>(replacement));
		original = reinterpret_cast<Function>(trampoline);
		if (WriteCode(target, jump, N))
			return true;
		original = nullptr;
		VirtualFree(trampoline, 0, MEM_RELEASE);
		return false;
	}

	void InstallGameChanges() {
		static std::atomic<bool> finished{false};
		if (finished.load(std::memory_order_acquire))
			return;
		static std::mutex lock;
		std::lock_guard<std::mutex> guard(lock);
		if (finished.load(std::memory_order_relaxed))
			return;
		const auto image = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
		const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
		const auto pe = reinterpret_cast<IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
		if (pe->FileHeader.TimeDateStamp != 0x568dda73 || pe->OptionalHeader.SizeOfImage != 0x4111200) {
			finished.store(true, std::memory_order_release);
			return;
		}
		constexpr BYTE warning[] = {
		    0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20};
		constexpr BYTE signIn[] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xf9};
		constexpr BYTE video[] = {
		    0x40, 0x55, 0x53, 0x56, 0x57, 0x48, 0x8b, 0xec, 0x48, 0x83, 0xec, 0x68, 0x48, 0x8b, 0xf9};
		constexpr BYTE multiplayer[] = {0x84, 0xc0, 0x75, 0x3f, 0x48, 0x85, 0xdb};
		constexpr BYTE signInCheck[] = {0xe8, 0xd8, 0xac, 0xff, 0xff};
		constexpr BYTE legalQueue[] = {0x84, 0xc0, 0x75, 0x56};
		constexpr BYTE tutorialCheck[] = {0x84, 0xc0, 0x74, 0x52};
		constexpr BYTE regionProbe[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30};
		constexpr BYTE startupMovie[] = {0xe8, 0x3b, 0x7b, 0xfe, 0xff};
		constexpr BYTE hiddenButton[] = {
		    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20};
		constexpr BYTE newsVisibility[] = {0xc6, 0x45, 0x20, 0x01};
		constexpr BYTE defaultNews[] = {0xe8, 0x2e, 0xd9, 0x46, 0x00};
		constexpr BYTE tabNavigation[] = {0xe8, 0xc9, 0x74, 0x4a, 0x00};
		const auto attributeSlot = reinterpret_cast<void**>(image + 0x21b3118);
		const auto labelSlot = reinterpret_cast<void**>(image + 0x21bd2a8);
		const auto profileEventSlot = reinterpret_cast<void**>(image + 0x23af5c8);
		const auto profileSlot = reinterpret_cast<void**>(image + 0x23af518);
		const auto menuSlot = reinterpret_cast<void**>(image + 0x23b5928);
		const auto syncSlot = reinterpret_cast<void**>(image + 0x236bcd0);
		if (memcmp(image + 0x1248d1c, warning, sizeof(warning)) || memcmp(image + 0x16c08a0, signIn, sizeof(signIn)) ||
		    memcmp(image + 0x170f4ec, video, sizeof(video)) ||
		    memcmp(image + 0x16bac0c, multiplayer, sizeof(multiplayer)) ||
		    memcmp(image + 0x16c0007, signInCheck, sizeof(signInCheck)) ||
		    memcmp(image + 0x117a9e0, startupMovie, sizeof(startupMovie)) ||
		    memcmp(image + 0x16f4692, legalQueue, sizeof(legalQueue)) ||
		    memcmp(image + 0x16c0a16, tutorialCheck, sizeof(tutorialCheck)) ||
		    memcmp(image + 0x124695c, regionProbe, sizeof(regionProbe)) ||
		    memcmp(image + 0x16beef4, hiddenButton, sizeof(hiddenButton)) ||
		    memcmp(image + 0x1689486, tabNavigation, sizeof(tabNavigation)) || *attributeSlot != image + 0x417a48 ||
		    *labelSlot != image + 0x450fa4 || *profileEventSlot != image + 0x16893cc ||
		    *profileSlot != image + 0x1687fc0 || *syncSlot != image + 0x14ab5c8 ||
		    memcmp(image + 0x16c39b3, newsVisibility, sizeof(newsVisibility)) ||
		    memcmp(image + 0x16c3021, defaultNews, sizeof(defaultNews)) || *menuSlot != image + 0x16bfeb4)
			return;
		HMODULE pinned = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
		        reinterpret_cast<LPCWSTR>(&InstallGameChanges), &pinned))
			return;
		gameImage = image;
		if (!Detour(image + 0x1248d1c, warning, &AddLocalWarning, originalWarning) ||
		    !Detour(image + 0x16c08a0, signIn, &CompleteLocalSignIn, originalSignInComplete) ||
		    !Detour(image + 0x170f4ec, video, &SkipVideo, originalVideo) ||
		    !Detour(image + 0x16beef4, hiddenButton, &HideRetiredButton, originalHiddenButton))
			return;
		constexpr BYTE skipCall[] = {0x90, 0x90, 0x90, 0x90, 0x90};
		const BYTE hidden = 0;
		if (!WriteCode(image + 0x117a9e0, skipCall, sizeof(skipCall)) ||
		    !WriteCode(image + 0x16c3021, skipCall, sizeof(skipCall)) || !WriteCode(image + 0x16c39b6, &hidden, 1))
			return;
		const BYTE allow = 0xeb;
		if (!WriteCode(image + 0x16bac0e, &allow, 1) || !WriteCode(image + 0x16f4694, &allow, 1))
			return;
		constexpr BYTE skipTutorial[] = {0xeb, 0x08};
		if (!WriteCode(image + 0x16c0a16, skipTutorial, sizeof(skipTutorial)))
			return;
		constexpr BYTE skipRegions[] = {0xe9, 0x7b, 0, 0, 0, 0x90};
		if (!WriteCode(image + 0x124695c, skipRegions, sizeof(skipRegions)))
			return;
		const auto openProfile = reinterpret_cast<void*>(&OpenAccolades);
		const auto profileEvent = reinterpret_cast<void*>(&AccoladesEvent);
		const auto attribute = reinterpret_cast<void*>(&AchievementsAttribute);
		const auto label = reinterpret_cast<void*>(&AchievementsLabel);
		if (!WriteCode(profileSlot, &openProfile, sizeof(openProfile)) ||
		    !WriteCode(profileEventSlot, &profileEvent, sizeof(profileEvent)) ||
		    !WriteCode(attributeSlot, &attribute, sizeof(attribute)) || !WriteCode(labelSlot, &label, sizeof(label)) ||
		    !WriteCode(image + 0x1689486, skipCall, sizeof(skipCall)))
			return;
		const auto openMenu = reinterpret_cast<void*>(&OpenLocalMenu);
		const auto updateSync = reinterpret_cast<void*>(&UpdateClientSync);
		if (!WriteCode(menuSlot, &openMenu, sizeof(openMenu)) || !WriteCode(syncSlot, &updateSync, sizeof(updateSync)))
			return;
		finished.store(true, std::memory_order_release);
	}
}

extern "C" HANDLE WINAPI EvolveCreateFileW(LPCWSTR path, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES security,
    DWORD disposition, DWORD attributes, HANDLE templateFile) {
	offline::PrepareBackendTrust();
	InstallGameChanges();
	return CreateFileW(path, access, sharing, security, disposition, attributes, templateFile);
}

extern "C" HANDLE WINAPI EvolveCreateFileA(LPCSTR path, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES security,
    DWORD disposition, DWORD attributes, HANDLE templateFile) {
	offline::PrepareBackendTrust();
	InstallGameChanges();
	return CreateFileA(path, access, sharing, security, disposition, attributes, templateFile);
}
