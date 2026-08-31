#include "Features/NeuralNR/CallerSpoof.h"
#include "Features/NeuralNR.h"
#include "Utils/FileSystem.h"
#include "Globals.h"
#include <windows.h>
#include <thread>
#include <vector>
#include <atomic>
#include <string>
#include <detours/detours.h>

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

	constexpr int kFeatureDLSSNR = 18;

	static NVSDK_NGX_Result Hooked_NGXCreate(ID3D11DeviceContext* ctx, NVSDK_NGX_Feature feat, NVSDK_NGX_Parameter* param, NVSDK_NGX_Handle** handle)
	{
		auto& s = NeuralNR::GetState();
		logger::info("NeuralNR [Diag]: Intercepted Streamline CreateFeature! Target FeatureID: {}", static_cast<uint32_t>(feat));

		const bool isNR = (static_cast<int>(feat) == kFeatureDLSSNR);

		if (!s.streamlineContextCaptured && param && isNR)
		{
			__try {
				logger::info("NeuralNR [Streamline]: Confirmed Feature 18 (Neural Rendering) — stealing NGX_Parameter block: {}", (void*)param);
				s.nrParams = param;
				s.streamlineContextCaptured = true;
				s.paramsAreBorrowed = false;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		NVSDK_NGX_Result createRes = static_cast<NVSDK_NGX_Result>(0xBAD00007);
		if (s_orig_NGXCreate) {
			__try {
				createRes = s_orig_NGXCreate(ctx, feat, param, handle);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return static_cast<NVSDK_NGX_Result>(0xDEADBEEF);
			}
		}

		if (isNR && handle && *handle)
		{
			s.nrFeature = *handle;
			logger::info("NeuralNR [Diag]: Recorded confirmed NR feature handle: {}", (void*)s.nrFeature);
		}

		return createRes;
	}

	static NVSDK_NGX_Result Hooked_NGXEvaluate(ID3D11DeviceContext* ctx, NVSDK_NGX_Handle* feat, NVSDK_NGX_Parameter* param, void* info)
	{
		auto& s = NeuralNR::GetState();
		const bool isConfirmedNR = s.nrFeature && (feat == s.nrFeature);
		const bool noCreateSeenYet = !s.nrFeature;

		if (!s.streamlineContextCaptured && param && (isConfirmedNR || noCreateSeenYet))
		{
			__try {
				logger::info("NeuralNR [Streamline]: Stealing NGX_Parameter block from EvaluateFeature (confirmed={}): {}", isConfirmedNR, (void*)param);
				s.nrParams = param;
				if (isConfirmedNR) s.nrFeature = feat;
				s.streamlineContextCaptured = true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		if (isConfirmedNR)
		{
			static uint32_t s_suppressLog = 0;
			if (s_suppressLog++ % 300 == 0) {
				logger::info("NeuralNR [Streamline]: Suppressing native Streamline EvaluateFeature for NR to prevent D3D11 race condition.");
			}
			return NVSDK_NGX_Result_Success;
		}

		if (s_orig_NGXEvaluate) {
			__try {
				return s_orig_NGXEvaluate(ctx, feat, param, info);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return static_cast<NVSDK_NGX_Result>(0xDEADBEEF);
			}
		}
		return static_cast<NVSDK_NGX_Result>(0xBAD00007);
	}

	// --- Safe Post-Boot Interceptor ---
	namespace sl::param {
		struct IParameters {
			virtual int set_bool(const char* key, bool value) = 0;
			virtual int set_ull(const char* key, unsigned long long value) = 0;
			virtual int set_flt(const char* key, float value) = 0;
			virtual int set_dbl(const char* key, double value) = 0;
			virtual int set_u32(const char* key, uint32_t value) = 0;
			virtual int set_i32(const char* key, int32_t value) = 0;
			virtual int set_ptr(const char* key, void* value) = 0;
			virtual int set_str(const char* key, const char* value) = 0;

			virtual int get_bool(const char* key, bool* value) const = 0;
			virtual int get_ull(const char* key, unsigned long long* value) const = 0;
			virtual int get_flt(const char* key, float* value) const = 0;
			virtual int get_dbl(const char* key, double* value) const = 0;
			virtual int get_u32(const char* key, uint32_t* value) const = 0;
			virtual int get_i32(const char* key, int32_t* value) const = 0;
			virtual int get_ptr(const char* key, void** value) const = 0;
			virtual int get_str(const char* key, const char** value) const = 0;
		};
	}

	using slOnPluginLoad_t = bool (*)(sl::param::IParameters*);
	static slOnPluginLoad_t s_orig_slOnPluginLoad_nr = nullptr;

	static bool Hooked_slOnPluginLoad_NR(sl::param::IParameters* params)
	{
		logger::info("NeuralNR [Streamline]: slOnPluginLoad executing for sl.dlss_nr.dll! Params: {}", (void*)params);
		auto& s = NeuralNR::GetState();

		if (params) {
			void* pContext = nullptr;
			if (params->get_ptr("sl.param.global.ngxContext", &pContext) == 0 && pContext) {
				logger::info("NeuralNR [Streamline]: Probed global NGX Context: {}", pContext);
			}
			
			void* pBlock = nullptr;
			if (params->get_ptr("sl.param.dlss_nr.ngxParameters", &pBlock) == 0 && pBlock) {
				logger::info("NeuralNR [Streamline]: Extracted native NR NGX Parameter block: {}", pBlock);
				s.nrParams = static_cast<NVSDK_NGX_Parameter*>(pBlock);
				s.streamlineContextCaptured = true;
				s.paramsAreBorrowed = false;
			}
		}

		if (s_orig_slOnPluginLoad_nr) {
			__try {
				return s_orig_slOnPluginLoad_nr(params);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
		return true; 
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
	static std::atomic<bool> s_hookedStatus[std::size(s_targetModules)] = {};

	static void PatchTargetModuleAndMarkHooked(HMODULE hMod, size_t idx)
	{
		PatchModuleIATAny(hMod, "GetProcAddress", (void*)Hooked_GetProcAddress, (void**)&s_origGetProcAddress);
		PatchModuleIATAny(hMod, "NVSDK_NGX_D3D11_CreateFeature", (void*)Hooked_NGXCreate, (void**)&s_orig_NGXCreate);
		PatchModuleIATAny(hMod, "NVSDK_NGX_D3D11_EvaluateFeature", (void*)Hooked_NGXEvaluate, (void**)&s_orig_NGXEvaluate);
		s_hookedStatus[idx].store(true);
	}

	bool InstallActiveInterceptors()
	{
		bool allHooked = true;
		for (size_t i = 0; i < std::size(s_targetModules); ++i)
		{
			if (!s_hookedStatus[i].load())
			{
				HMODULE hMod = GetModuleHandleW(s_targetModules[i]);
				if (hMod) { PatchTargetModuleAndMarkHooked(hMod, i); }
				else { allHooked = false; }
			}
		}
		return allHooked;
	}

	static bool FindTargetModuleIndex(const wchar_t* moduleFileName, size_t* outIndex)
	{
		for (size_t i = 0; i < std::size(s_targetModules); ++i)
		{
			if (_wcsicmp(moduleFileName, s_targetModules[i]) == 0)
			{
				if (outIndex) *outIndex = i;
				return true;
			}
		}
		return false;
	}

	static const wchar_t* StripToFileNameW(const wchar_t* path)
	{
		const wchar_t* lastBackslash = wcsrchr(path, L'\\');
		const wchar_t* lastSlash = wcsrchr(path, L'/');
		if (lastSlash && (!lastBackslash || lastSlash > lastBackslash)) lastBackslash = lastSlash;
		return lastBackslash ? lastBackslash + 1 : path;
	}

	using PFN_LoadLibraryW   = HMODULE(WINAPI*)(LPCWSTR);
	using PFN_LoadLibraryA   = HMODULE(WINAPI*)(LPCSTR);
	using PFN_LoadLibraryExW = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
	using PFN_LoadLibraryExA = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);

	static PFN_LoadLibraryW   s_origLoadLibraryW   = LoadLibraryW;
	static PFN_LoadLibraryA   s_origLoadLibraryA   = LoadLibraryA;
	static PFN_LoadLibraryExW s_origLoadLibraryExW = LoadLibraryExW;
	static PFN_LoadLibraryExA s_origLoadLibraryExA = LoadLibraryExA;

	static void TryInstantPatch(HMODULE hLoaded, const wchar_t* fileName)
	{
		__try
		{
			size_t idx = 0;
			if (hLoaded && FindTargetModuleIndex(fileName, &idx) && !s_hookedStatus[idx].load())
			{
				char logName[64];
				size_t converted = 0;
				wcstombs_s(&converted, logName, sizeof(logName), fileName, _TRUNCATE);
				logger::info("NeuralNR [Diag]: Instant-hook caught target module load: {}", logName);

				PatchTargetModuleAndMarkHooked(hLoaded, idx);

				if (_wcsicmp(fileName, L"sl.dlss_nr.dll") == 0) {
					void* pOnPluginLoad = (void*)GetProcAddress(hLoaded, "slOnPluginLoad");
					if (pOnPluginLoad) {
						DetourTransactionBegin();
						DetourUpdateThread(GetCurrentThread());
						s_orig_slOnPluginLoad_nr = (slOnPluginLoad_t)pOnPluginLoad;
						DetourAttach(&(PVOID&)s_orig_slOnPluginLoad_nr, (PVOID)Hooked_slOnPluginLoad_NR);
						DetourTransactionCommit();
						logger::info("NeuralNR [Streamline]: Detoured slOnPluginLoad on sl.dlss_nr.dll");
					}
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	static HMODULE WINAPI Hooked_LoadLibraryW(LPCWSTR lpLibFileName)
	{
		HMODULE result = nullptr;
		__try { result = s_origLoadLibraryW(lpLibFileName); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
		if (result && lpLibFileName) TryInstantPatch(result, StripToFileNameW(lpLibFileName));
		return result;
	}

	static HMODULE WINAPI Hooked_LoadLibraryA(LPCSTR lpLibFileName)
	{
		HMODULE result = nullptr;
		__try { result = s_origLoadLibraryA(lpLibFileName); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
		if (result && lpLibFileName)
		{
			wchar_t wide[MAX_PATH]{};
			size_t converted = 0;
			if (mbstowcs_s(&converted, wide, MAX_PATH, lpLibFileName, _TRUNCATE) == 0)
				TryInstantPatch(result, StripToFileNameW(wide));
		}
		return result;
	}

	static HMODULE WINAPI Hooked_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
	{
		HMODULE result = nullptr;
		__try { result = s_origLoadLibraryExW(lpLibFileName, hFile, dwFlags); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
		if (result && lpLibFileName) TryInstantPatch(result, StripToFileNameW(lpLibFileName));
		return result;
	}

	static HMODULE WINAPI Hooked_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
	{
		HMODULE result = nullptr;
		__try { result = s_origLoadLibraryExA(lpLibFileName, hFile, dwFlags); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
		if (result && lpLibFileName)
		{
			wchar_t wide[MAX_PATH]{};
			size_t converted = 0;
			if (mbstowcs_s(&converted, wide, MAX_PATH, lpLibFileName, _TRUNCATE) == 0)
				TryInstantPatch(result, StripToFileNameW(wide));
		}
		return result;
	}

	static bool s_loadLibraryHooksInstalled = false;

	static void InstallLoadLibraryInstantHook()
	{
		if (s_loadLibraryHooksInstalled) return;

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(&(PVOID&)s_origLoadLibraryW, Hooked_LoadLibraryW);
		DetourAttach(&(PVOID&)s_origLoadLibraryA, Hooked_LoadLibraryA);
		DetourAttach(&(PVOID&)s_origLoadLibraryExW, Hooked_LoadLibraryExW);
		DetourAttach(&(PVOID&)s_origLoadLibraryExA, Hooked_LoadLibraryExA);
		LONG err = DetourTransactionCommit();

		if (err == NO_ERROR)
		{
			s_loadLibraryHooksInstalled = true;
			logger::info("NeuralNR [Diag]: LoadLibrary instant-hook installed successfully.");
		}
	}

	void InstallUpscalerHooks()
	{
		InstallLoadLibraryInstantHook();

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