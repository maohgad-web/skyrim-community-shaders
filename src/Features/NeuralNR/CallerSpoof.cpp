#include "Features/NeuralNR/CallerSpoof.h"
#include "Features/NeuralNR.h"
#include "Utils/FileSystem.h"
#include "Globals.h"
#include <windows.h>
#include <vector>
#include <thread>

namespace CSS::CallerSpoof
{
	enum slFeature : uint32_t { kFeatureDLSS_NR = 1004 }; 
	
	struct slPreferences {
		bool showConsole;
		int logLevel;
		void* pathsToPlugins;
		uint32_t numPathsToPlugins;
		const slFeature* featuresToLoad;
		uint32_t numFeaturesToLoad;
		uint32_t renderAPI;
	};

	typedef int (*slInit_t)(const slPreferences*, void*, void*);
	typedef int (*slIsFeatureSupported_t)(slFeature, const void*);
	
	static slInit_t s_orig_slInit = nullptr;
	static slIsFeatureSupported_t s_orig_slIsFeatureSupported = nullptr;
	static std::vector<slFeature> s_modifiedFeatures;

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

	// --- The Direct Parameter Steal ---
	static NVSDK_NGX_Result Hooked_NGXCreate(ID3D11DeviceContext* ctx, NVSDK_NGX_Feature feat, NVSDK_NGX_Parameter* param, NVSDK_NGX_Handle** handle)
	{
		auto& s = NeuralNR::GetState();
		
		// Target SuperSampling (Feature 1) to steal Streamline's globally validated NGX_Parameter block
		if (!s.streamlineContextCaptured && param && feat == NVSDK_NGX_Feature_SuperSampling)
		{
			logger::info("NeuralNR [Streamline]: Pipeline intercepted! Captured globally validated NGX_Parameter block from DLSS-SR (Feature 1).");
			s.nrParams = param;
			s.streamlineContextCaptured = true;
		}

		if (s_orig_NGXCreate) return s_orig_NGXCreate(ctx, feat, param, handle);
		return static_cast<NVSDK_NGX_Result>(0xBAD00007);
	}

	static NVSDK_NGX_Result Hooked_NGXEvaluate(ID3D11DeviceContext* ctx, NVSDK_NGX_Handle* feat, NVSDK_NGX_Parameter* param, void* info)
	{
		if (s_orig_NGXEvaluate) return s_orig_NGXEvaluate(ctx, feat, param, info);
		return static_cast<NVSDK_NGX_Result>(0xBAD00007);
	}

	int Hooked_slInit(const slPreferences* pref, void* app, void* device)
	{
		logger::info("NeuralNR [Streamline]: Intercepted slInit. Modifying plugin array...");
		
		slPreferences modPref = *pref;
		s_modifiedFeatures.clear();
		
		bool nrFound = false;
		if (pref->featuresToLoad && pref->numFeaturesToLoad > 0) {
			for (uint32_t i = 0; i < pref->numFeaturesToLoad; ++i) {
				s_modifiedFeatures.push_back(pref->featuresToLoad[i]);
				if (pref->featuresToLoad[i] == kFeatureDLSS_NR) nrFound = true;
			}
		}
		
		if (!nrFound) {
			logger::info("NeuralNR [Streamline]: Dynamically injecting Feature 1004 (Neural Rendering) into sl::Preferences.");
			s_modifiedFeatures.push_back(kFeatureDLSS_NR);
			modPref.featuresToLoad = s_modifiedFeatures.data();
			modPref.numFeaturesToLoad = static_cast<uint32_t>(s_modifiedFeatures.size());
		}
		
		return s_orig_slInit(&modPref, app, device);
	}

	int Hooked_slIsFeatureSupported(slFeature feature, const void* pArch)
	{
		if (feature == kFeatureDLSS_NR) return 0; 
		return s_orig_slIsFeatureSupported(feature, pArch);
	}

	// --- Aggressive GetProcAddress Hijack ---
	static FARPROC WINAPI Hooked_GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
	{
		if (lpProcName && ((ULONG_PTR)lpProcName > 0xFFFF))
		{
			if (_stricmp(lpProcName, "slInit") == 0) {
				if (!s_orig_slInit) s_orig_slInit = (slInit_t)(s_origGetProcAddress ? s_origGetProcAddress(hModule, lpProcName) : ::GetProcAddress(hModule, lpProcName));
				return (FARPROC)Hooked_slInit;
			}
			if (_stricmp(lpProcName, "slIsFeatureSupported") == 0) {
				if (!s_orig_slIsFeatureSupported) s_orig_slIsFeatureSupported = (slIsFeatureSupported_t)(s_origGetProcAddress ? s_origGetProcAddress(hModule, lpProcName) : ::GetProcAddress(hModule, lpProcName));
				return (FARPROC)Hooked_slIsFeatureSupported;
			}
			if (_stricmp(lpProcName, "NVSDK_NGX_D3D11_CreateFeature") == 0) {
				if (!s_orig_NGXCreate) s_orig_NGXCreate = (PFN_CreateFeature)(s_origGetProcAddress ? s_origGetProcAddress(hModule, lpProcName) : ::GetProcAddress(hModule, lpProcName));
				return (FARPROC)Hooked_NGXCreate;
			}
			if (_stricmp(lpProcName, "NVSDK_NGX_D3D11_EvaluateFeature") == 0) {
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

	static void StreamlineHookThread()
	{
		HMODULE hUpscaler = nullptr;
		while (!(hUpscaler = GetModuleHandleW(L"SkyrimUpscaler.dll")) && !(hUpscaler = GetModuleHandleW(L"FSR2.dll"))) {
			Sleep(10);
		}
		PatchModuleIATAny(hUpscaler, "GetProcAddress", (void*)Hooked_GetProcAddress, (void**)&s_origGetProcAddress);
		logger::info("NeuralNR [Diag]: Successfully attached GetProcAddress hijack to Upscaler plugin.");

		HMODULE hInterposer = nullptr;
		while (!(hInterposer = GetModuleHandleW(L"sl.interposer.dll"))) {
			Sleep(10);
		}
		PatchModuleIATAny(hInterposer, "GetProcAddress", (void*)Hooked_GetProcAddress, (void**)&s_origGetProcAddress);
		logger::info("NeuralNR [Diag]: Successfully attached GetProcAddress hijack to sl.interposer.dll.");
		
		HMODULE hDLSS = nullptr;
		while (!(hDLSS = GetModuleHandleW(L"sl.dlss.dll"))) {
			Sleep(10);
		}
		PatchModuleIATAny(hDLSS, "GetProcAddress", (void*)Hooked_GetProcAddress, (void**)&s_origGetProcAddress);
		logger::info("NeuralNR [Diag]: Successfully attached GetProcAddress hijack to sl.dlss.dll.");
	}

	void InstallStreamlineHooks()
	{
		// Spawn aggressive background polling thread immediately
		std::thread(StreamlineHookThread).detach();
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

	// Execute background poll at DLL load time
	static struct AutoInit { AutoInit() { InstallStreamlineHooks(); } } s_autoInit;
}