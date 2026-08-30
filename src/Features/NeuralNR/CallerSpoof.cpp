#include "Features/NeuralNR/CallerSpoof.h"
#include "Features/NeuralNR.h"
#include "Utils/FileSystem.h"
#include "Globals.h"
#include <windows.h>
#include <vector>

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

	// Active Interceptor: Evaluates every frame passing through Streamline / DLSS-SR
	static NVSDK_NGX_Result Hooked_NGXEvaluate(ID3D11DeviceContext* ctx, NVSDK_NGX_Handle* feat, NVSDK_NGX_Parameter* param, void* info)
	{
		auto& s = NeuralNR::GetState();
		
		static uint32_t s_evalPassCounter = 0;
		if (++s_evalPassCounter % 300 == 1)
		{
			logger::info("NeuralNR [Diag]: Active EvaluateFeature intercepted! Handle={}, ParamBlock={}", 
				(void*)feat, (void*)param);
		}

		if (!s.streamlineContextCaptured && param)
		{
			logger::info("NeuralNR [Streamline]: DLSS-SR frame intercepted! Captured validated NGX_Parameter block at {}", (void*)param);
			s.nrParams = param;
			s.streamlineContextCaptured = true;
		}

		if (s_orig_NGXEvaluate) {
			return s_orig_NGXEvaluate(ctx, feat, param, info);
		}
		return NVSDK_NGX_Result_Fail;
	}

	// Active Interceptor: Traps any feature creation events
	static NVSDK_NGX_Result Hooked_NGXCreate(ID3D11DeviceContext* ctx, NVSDK_NGX_Feature feat, NVSDK_NGX_Parameter* param, NVSDK_NGX_Handle** handle)
	{
		logger::info("NeuralNR [Diag]: Active CreateFeature intercepted! FeatureID={}, ParamBlock={}", 
			static_cast<uint32_t>(feat), (void*)param);

		auto& s = NeuralNR::GetState();
		if (!s.streamlineContextCaptured && param)
		{
			logger::info("NeuralNR [Streamline]: Captured NGX_Parameter block from CreateFeature event.");
			s.nrParams = param;
			s.streamlineContextCaptured = true;
		}

		if (s_orig_NGXCreate) return s_orig_NGXCreate(ctx, feat, param, handle);
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
		if (feature == kFeatureDLSS_NR) {
			logger::info("NeuralNR [Streamline]: slIsFeatureSupported(1004) queried -> Returning supported.");
			return 0; 
		}
		return s_orig_slIsFeatureSupported(feature, pArch);
	}

	static bool PatchModuleIATAny(HMODULE hTargetModule, const char* targetFunction, void* hookFunc, void** origFunc)
	{
		if (!hTargetModule) return false;

		PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)hTargetModule;
		if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return false;

		PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)((BYTE*)hTargetModule + dosHeader->e_lfanew);
		if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return false;

		DWORD importDirRVA = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
		if (!importDirRVA) return false;

		PIMAGE_IMPORT_DESCRIPTOR importDesc = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hTargetModule + importDirRVA);
		bool patched = false;

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
							patched = true;
						}
					}
				}
				originalFirstThunk++;
				firstThunk++;
			}
			importDesc++;
		}
		return patched;
	}

	void InstallUpscalerHooks()
	{
		HMODULE hUpscaler = GetModuleHandleW(L"SkyrimUpscaler.dll"); 
		if (!hUpscaler) hUpscaler = GetModuleHandleW(L"FSR2.dll");
		if (!hUpscaler) hUpscaler = GetModuleHandleW(NULL); 
		
		PatchModuleIATAny(hUpscaler, "slInit", (void*)Hooked_slInit, (void**)&s_orig_slInit);
		PatchModuleIATAny(hUpscaler, "slIsFeatureSupported", (void*)Hooked_slIsFeatureSupported, (void**)&s_orig_slIsFeatureSupported);
	}

	bool InstallActiveInterceptors()
	{
		static bool s_interposerHooked = false;
		static bool s_dlssHooked = false;
		static bool s_coreHooked = false;

		HMODULE hInterposer = GetModuleHandleW(L"sl.interposer.dll");
		HMODULE hDLSS       = GetModuleHandleW(L"sl.dlss.dll");
		HMODULE hCore       = GetModuleHandleW(L"_nvngx.dll");

		if (hInterposer && !s_interposerHooked) {
			bool p1 = PatchModuleIATAny(hInterposer, "NVSDK_NGX_D3D11_EvaluateFeature", (void*)Hooked_NGXEvaluate, (void**)&s_orig_NGXEvaluate);
			bool p2 = PatchModuleIATAny(hInterposer, "NVSDK_NGX_D3D11_CreateFeature",   (void*)Hooked_NGXCreate,   (void**)&s_orig_NGXCreate);
			logger::info("NeuralNR [Diag]: Hooked sl.interposer.dll (Eval={}, Create={})", p1, p2);
			s_interposerHooked = true;
		}

		if (hDLSS && !s_dlssHooked) {
			bool p1 = PatchModuleIATAny(hDLSS, "NVSDK_NGX_D3D11_EvaluateFeature", (void*)Hooked_NGXEvaluate, (void**)&s_orig_NGXEvaluate);
			bool p2 = PatchModuleIATAny(hDLSS, "NVSDK_NGX_D3D11_CreateFeature",   (void*)Hooked_NGXCreate,   (void**)&s_orig_NGXCreate);
			logger::info("NeuralNR [Diag]: Hooked sl.dlss.dll (Eval={}, Create={})", p1, p2);
			s_dlssHooked = true;
		}

		if (hCore && !s_coreHooked) {
			bool p1 = PatchModuleIATAny(hCore, "NVSDK_NGX_D3D11_EvaluateFeature", (void*)Hooked_NGXEvaluate, (void**)&s_orig_NGXEvaluate);
			logger::info("NeuralNR [Diag]: Hooked _nvngx.dll (Eval={})", p1);
			s_coreHooked = true;
		}

		return (s_interposerHooked || s_dlssHooked || s_coreHooked);
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