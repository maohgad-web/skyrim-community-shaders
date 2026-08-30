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

	using PFN_EvaluateFeature = NVSDK_NGX_Result (*)(ID3D11DeviceContext*, NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, void*);
	static PFN_EvaluateFeature s_orig_NGXEvaluate = nullptr;

	static NVSDK_NGX_Result Hooked_NGXEvaluate(ID3D11DeviceContext* ctx, NVSDK_NGX_Handle* feat, NVSDK_NGX_Parameter* param, void* info)
	{
		auto& s = NeuralNR::GetState();
		
		// Steal ONLY the parameter block, NOT the handle (which belongs to DLSS-SR).
		if (!s.streamlineContextCaptured && param)
		{
			logger::info("NeuralNR [Streamline]: DLSS pipeline intercepted! Stealing globally validated NGX_Parameter block.");
			s.nrParams = param;
			s.streamlineContextCaptured = true;
		}

		return s_orig_NGXEvaluate(ctx, feat, param, info);
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

	void InstallStreamlineHooks()
	{
		HMODULE hUpscaler = GetModuleHandleW(L"SkyrimUpscaler.dll"); 
		if (!hUpscaler) hUpscaler = GetModuleHandleW(L"FSR2.dll");
		if (!hUpscaler) hUpscaler = GetModuleHandleW(NULL); 
		
		PatchModuleIATAny(hUpscaler, "slInit", (void*)Hooked_slInit, (void**)&s_orig_slInit);
		PatchModuleIATAny(hUpscaler, "slIsFeatureSupported", (void*)Hooked_slIsFeatureSupported, (void**)&s_orig_slIsFeatureSupported);

		// Hook the active interposer pipeline to catch the DLSS-SR evaluation command
		HMODULE hInterposer = GetModuleHandleW(L"sl.interposer.dll");
		if (!hInterposer) hInterposer = GetModuleHandleW(L"sl.dlss.dll");

		if (hInterposer) {
			PatchModuleIATAny(hInterposer, "NVSDK_NGX_D3D11_EvaluateFeature", (void*)Hooked_NGXEvaluate, (void**)&s_orig_NGXEvaluate);
			logger::info("NeuralNR: Streamline IAT hooks installed successfully. Ready to intercept DLSS execution.");
		} else {
			logger::warn("NeuralNR: Streamline plugins not found in memory. IAT payload steal deferred.");
		}
	}

	void Install() {}
	void Uninstall() {}
}