#include "Features/NeuralNR/CallerSpoof.h"
#include <windows.h>
#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

namespace CSS::CallerSpoof
{
	using GetModuleFileNameW_t = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);
	static GetModuleFileNameW_t s_origK32 = nullptr;
	static GetModuleFileNameW_t s_origKB  = nullptr;
	static HMODULE              s_ourModule = nullptr;

	static bool CallerIsNGX(void* returnAddress)
	{
		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery(returnAddress, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;

		auto base = reinterpret_cast<HMODULE>(mbi.AllocationBase);
		HMODULE hNR   = GetModuleHandleW(L"nvngx_dlssnr.dll");
		HMODULE hCore = GetModuleHandleW(L"_nvngx.dll");
		HMODULE hNGX  = GetModuleHandleW(L"nvngx.dll");
		return (hNR && base == hNR) || (hCore && base == hCore) || (hNGX && base == hNGX);
	}

	static DWORD Spoof(LPWSTR lpFilename, DWORD nSize)
	{
		const wchar_t* spoof = L"C:\\Windows\\System32\\nvngx.dll";
		const DWORD len = static_cast<DWORD>(wcslen(spoof));
		if (len >= nSize)
		{
			wcsncpy_s(lpFilename, nSize, spoof, _TRUNCATE);
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return nSize;
		}
		wcscpy_s(lpFilename, nSize, spoof);
		SetLastError(ERROR_SUCCESS);
		return len;
	}

	static bool ShouldSpoof(HMODULE hModule, LPWSTR lpFilename, DWORD nSize, void* returnAddress)
	{
		if (!lpFilename || nSize == 0) return false;
		if (hModule != nullptr && hModule != s_ourModule) return false;
		return CallerIsNGX(returnAddress);
	}

	static DWORD WINAPI HookedK32(HMODULE h, LPWSTR f, DWORD n)
	{
		void* ra = _ReturnAddress();
		return ShouldSpoof(h, f, n, ra) ? Spoof(f, n) : s_origK32(h, f, n);
	}

	static DWORD WINAPI HookedKB(HMODULE h, LPWSTR f, DWORD n)
	{
		void* ra = _ReturnAddress();
		return ShouldSpoof(h, f, n, ra) ? Spoof(f, n) : s_origKB(h, f, n);
	}

	void Install()
	{
		if (s_origK32 || s_origKB) return;

		// Resolve our own module handle for comparison
		GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
			reinterpret_cast<LPCWSTR>(&Install),
			&s_ourModule);

		// Obtain the function pointers BEFORE attaching the detours
		HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
		HMODULE hKB  = GetModuleHandleW(L"kernelbase.dll");

		if (hK32)
			s_origK32 = reinterpret_cast<GetModuleFileNameW_t>(GetProcAddress(hK32, "GetModuleFileNameW"));
		if (hKB)
			s_origKB  = reinterpret_cast<GetModuleFileNameW_t>(GetProcAddress(hKB, "GetModuleFileNameW"));

		// Hook kernel32!GetModuleFileNameW
		if (s_origK32)
		{
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(reinterpret_cast<PVOID*>(&s_origK32), reinterpret_cast<PVOID>(HookedK32));
			DetourTransactionCommit();
		}

		// Hook kernelbase!GetModuleFileNameW
		if (s_origKB)
		{
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(reinterpret_cast<PVOID*>(&s_origKB), reinterpret_cast<PVOID>(HookedKB));
			DetourTransactionCommit();
		}

		logger::info("NeuralNR: caller-spoof installed (kernel32={}, kernelbase={}).",
			s_origK32 ? "ok" : "skipped", s_origKB ? "ok" : "skipped");
	}

	void Uninstall()
	{
		if (s_origK32)
		{
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourDetach(reinterpret_cast<PVOID*>(&s_origK32), reinterpret_cast<PVOID>(HookedK32));
			DetourTransactionCommit();
			s_origK32 = nullptr;
		}
		if (s_origKB)
		{
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourDetach(reinterpret_cast<PVOID*>(&s_origKB), reinterpret_cast<PVOID>(HookedKB));
			DetourTransactionCommit();
			s_origKB = nullptr;
		}
	}
}