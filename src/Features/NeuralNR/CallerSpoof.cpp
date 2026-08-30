#include "Features/NeuralNR/CallerSpoof.h"
#include "Features/NeuralNR.h"
#include "Utils/FileSystem.h"
#include "Globals.h"
#include <windows.h>
#include <thread>
#include <vector>

namespace CSS::CallerSpoof
{
	using PFN_CreateFeature = NVSDK_NGX_Result (*)(ID3D11DeviceContext*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
	static PFN_CreateFeature s_orig_NGXCreate = nullptr;

	using PFN_EvaluateFeature = NVSDK_NGX_Result (*)(ID3D11DeviceContext*, NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, void*);
	static PFN_EvaluateFeature s_orig_NGXEvaluate = nullptr;

	using GetProcAddress_t = FARPROC(WINAPI*)(HMODULE, LPCSTR);
	static GetProcAddress_t s_origGetProcAddress = nullptr;

	using GetModuleFileNameW_t = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);
	static GetModuleFileNameW_t s_origK32 = nullptr;
	static HMODULE s_ourModule = nullptr;

	static DWORD WINAPI HookedK32(HMODULE hModule, LPWSTR lpFilename, DWORD nSize)
	{
		if (hModule == s_ourModule && lpFilename && nSize > 0)
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

		if (s_origK32) return s_origK32(hModule, lpFilename, nSize);
		return 0;
	}

	// --- SEH-Guarded Diagnostic Interceptors ---
	static NVSDK_NGX_Result Hooked_NGXCreate(ID3D11DeviceContext* ctx, NVSDK_NGX_Feature feat, NVSDK_NGX_Parameter* param, NVSDK_NGX_Handle** handle)
	{
		auto& s = NeuralNR::GetState();
		
		logger::info("NeuralNR [Diag]: Intercepted Streamline CreateFeature! Target FeatureID: {}", static_cast<uint32_t>(feat));

		if (!s.streamlineContextCaptured && param)
		{
			__try {
				logger::info("NeuralNR [Diag]: Stealing validated NGX_Parameter block from active CreateFeature event: {}", (void*)param);
				s.nrParams = param;
				s.streamlineContextCaptured = true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				logger::warn("NeuralNR [Diag]: SEH caught access violation during CreateFeature parameter steal.");
			}
		}

		if (s_orig_NGXCreate) {
			__try {
				return s_orig_NGXCreate(ctx, feat, param, handle);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				logger::warn("NeuralNR [Diag]: SEH caught crash inside original Streamline CreateFeature!");
				return static_cast<NVSDK_NGX_Result>(0xDEADBEEF);
			}
		}
		return static_cast<NVSDK_NGX_Result>(0xBAD00007);
	}

	static NVSDK_NGX_Result Hooked_NGXEvaluate(ID3D11DeviceContext* ctx, NVSDK_NGX_Handle* feat, NVSDK_NGX_Parameter* param, void* info)
	{
		auto& s = NeuralNR::GetState();
		
		static uint32_t logCounter = 0;
		if (logCounter++ % 300 == 0) {
			logger::info("NeuralNR [Diag]: Streamline EvaluateFeature is running mid-game. Handle={}, ParamBlock={}", (void*)feat, (void*)param);
		}

		if (!s.streamlineContextCaptured && param)
		{
			__try {
				logger::info("NeuralNR [Diag]: Stealing validated NGX_Parameter block from active EvaluateFeature event: {}", (void*)param);
				s.nrParams = param;
				s.streamlineContextCaptured = true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				logger::warn("NeuralNR [Diag]: SEH caught access violation during EvaluateFeature parameter steal.");
			}
		}

		if (s_orig_NGXEvaluate) {
			__try {
				return s_orig_NGXEvaluate(ctx, feat, param, info);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				logger::warn("NeuralNR [Diag]: SEH caught crash inside original Streamline EvaluateFeature!");
				return static_cast<NVSDK_NGX_Result>(0xDEADBEEF);
			}
		}
		return static_cast<NVSDK_NGX_Result>(0xBAD00007);
	}

	static FARPROC WINAPI Hooked_GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
	{
		if (lpProcName && ((ULONG_PTR)lpProcName > 0xFFFF))
		{
			if (_stricmp(lpProcName, "NVSDK_NGX_D3D11_CreateFeature") == 0)
			{
				if (!s_orig_NGXCreate) s_orig_NGXCreate = (PFN_CreateFeature)(s_origGetProcAddress ? s_origGetProcAddress(hModule, lpProcName) : ::GetProcAddress(hModule, lpProcName));
				return (FARPROC)Hooked_NGXCreate;
			}
			if (_stricmp(lpProcName, "NVSDK_NGX_D3D11_EvaluateFeature") == 0)
			{
				if (!s_orig_NGXEvaluate) s_orig_NGXEvaluate = (PFN_EvaluateFeature)(s_origGetProcAddress ? s_origGetProcAddress(hModule, lpProcName) : ::GetProcAddress(hModule, lpProcName));
				return (FARPROC)Hooked_NGXEvaluate;
			}
		}
		
		if (s_origGetProcAddress) return s_origGetProcAddress(hModule, lpProcName);
		return ::GetProcAddress(hModule, lpProcName);
	}

	static void PatchModuleIATAny(HMODULE hTargetModule, const char* targetFunction, void* hookFunc, void** origFunc)
	{
		if (!hTargetModule) return;

		PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)hTargetModule;
		if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return;

		PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)((BYTE*)hTargetModule + dosHeader->e_lfanew);
		if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return;

		DWORD importDirRVA = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
		if (!importDirRVA) return;

		PIMAGE_IMPORT_DESCRIPTOR importDesc = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hTargetModule + importDirRVA);

		while (importDesc->Name)
		{
			PIMAGE_THUNK_DATA originalFirstThunk = (PIMAGE_THUNK_DATA)((BYTE*)hTargetModule + importDesc->OriginalFirstThunk);
			PIMAGE_THUNK_DATA firstThunk = (PIMAGE_THUNK_DATA)((BYTE*)hTargetModule + importDesc->FirstThunk);

			while (originalFirstThunk->u1.AddressOfData)
			{
				if (!(originalFirstThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG))
				{
					PIMAGE_IMPORT_BY_NAME importByName = (PIMAGE_IMPORT_BY_NAME)((BYTE*)hTargetModule + originalFirstThunk->u1.AddressOfData);
					if (_stricmp((const char*)importByName->Name, targetFunction) == 0)
					{
						DWORD oldProtect;
						if (VirtualProtect(&firstThunk->u1.Function, sizeof(uintptr_t), PAGE_READWRITE, &oldProtect))
						{
							if (origFunc && !*origFunc) *origFunc = (void*)firstThunk->u1.Function;
							firstThunk->u1.Function = (uintptr_t)hookFunc;
							VirtualProtect(&firstThunk->u1.Function, sizeof(uintptr_t), oldProtect, &oldProtect);
						}
					}
				}
				originalFirstThunk++;
				firstThunk++;
			}
			importDesc++;
		}
	}

	// PATCH: the header (CallerSpoof.h) declares InstallUpscalerHooks() and
	// InstallActiveInterceptors() — this file previously defined neither,
	// only a differently-named InstallStreamlineHooks(), which meant both
	// declared functions were unresolved-external at link time (NeuralNR.cpp
	// calls both). Restructured so both real names exist, sharing one
	// idempotent core so calling it repeatedly (every frame, from OnPresent)
	// is cheap once everything's already hooked.
	//
	// NOTE — thread-safety worth knowing about: this shared state
	// (s_hookedStatus / s_origGetProcAddress) is now touched both from the
	// background polling thread (InstallUpscalerHooks) and from the main
	// render thread (InstallActiveInterceptors, called every frame from
	// OnPresent) with no locking between them. In practice the window is
	// narrow — each entry only transitions its status once — but it's a
	// genuine, unaddressed data race, not just a style nitpick. Flagging it
	// rather than silently leaving it, since it's outside what was asked
	// (compile errors) but directly relevant to this exact restructure.
	static const wchar_t* s_targetModules[] = {
		L"sl.interposer.dll",
		L"sl.common.dll",
		L"sl.dlss.dll",
		L"sl.dlss_g.dll",
		L"sl.dlss_nr.dll",
		L"sl.reflex.dll",
		L"sl.nis.dll",
		L"sl.pcl.dll",
		L"nvngx.dll",
		L"_nvngx.dll"
	};
	static std::vector<bool> s_hookedStatus(std::size(s_targetModules), false);

	bool InstallActiveInterceptors()
	{
		bool allHooked = true;
		for (size_t i = 0; i < std::size(s_targetModules); ++i)
		{
			if (!s_hookedStatus[i])
			{
				HMODULE hMod = GetModuleHandleW(s_targetModules[i]);
				if (hMod)
				{
					PatchModuleIATAny(hMod, "GetProcAddress", (void*)Hooked_GetProcAddress, (void**)&s_origGetProcAddress);

					char logName[64];
					size_t converted = 0;
					wcstombs_s(&converted, logName, sizeof(logName), s_targetModules[i], _TRUNCATE);

					logger::info("NeuralNR [Diag]: Successfully attached GetProcAddress hijack to {}.", logName);
					s_hookedStatus[i] = true;
				}
				else
				{
					allHooked = false;
				}
			}
		}
		return allHooked;
	}

	void InstallUpscalerHooks()
	{
		std::thread([]() {
			while (!InstallActiveInterceptors())
				Sleep(200);
			logger::info("NeuralNR [Diag]: All Streamline and NGX modules actively intercepted.");
		}).detach();
	}

	void Install()
	{
		GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&Install),
			&s_ourModule);

		HMODULE hCore = GetModuleHandleW(L"_nvngx.dll");
		HMODULE hNGX  = GetModuleHandleW(L"nvngx.dll");
		HMODULE hNR   = GetModuleHandleW(L"nvngx_dlssnr.dll");

		if (hCore) PatchModuleIATAny(hCore, "GetModuleFileNameW", (void*)HookedK32, (void**)&s_origK32);
		if (hNGX)  PatchModuleIATAny(hNGX,  "GetModuleFileNameW", (void*)HookedK32, (void**)&s_origK32);
		if (hNR)   PatchModuleIATAny(hNR,   "GetModuleFileNameW", (void*)HookedK32, (void**)&s_origK32);
	}

	void Uninstall() {}
}