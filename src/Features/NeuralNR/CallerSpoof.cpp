#include "Features/NeuralNR/CallerSpoof.h"
#include "Utils/FileSystem.h"
#include "Globals.h"
#include <windows.h>
#include <filesystem>

namespace CSS::CallerSpoof
{
	using GetModuleFileNameW_t = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);
	static GetModuleFileNameW_t s_origK32 = nullptr;

	static DWORD WINAPI HookedK32(HMODULE hModule, LPWSTR lpFilename, DWORD nSize)
	{
		// Critical: Only spoof queries targeting the host executable (NULL module).
		// NGX uses NULL to validate the host application signature.
		if (hModule == nullptr && lpFilename && nSize > 0)
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

		// Forward all other module lookups normally
		if (s_origK32) return s_origK32(hModule, lpFilename, nSize);
		return 0;
	}

	static void PatchModuleIATAny(HMODULE hTargetModule, const char* targetFunction)
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
							if (!s_origK32) s_origK32 = (GetModuleFileNameW_t)firstThunk->u1.Function;
							firstThunk->u1.Function = (uintptr_t)HookedK32;
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

	void Install()
	{
		HMODULE hCore = GetModuleHandleW(L"_nvngx.dll");
		HMODULE hNGX  = GetModuleHandleW(L"nvngx.dll");
		
		// Preemptively load the NR payload so we can intercept its validation handshake
		HMODULE hNR = GetModuleHandleW(L"nvngx_dlssnr.dll");
		if (!hNR) 
		{
			auto path = Util::PathHelpers::GetFeatureShaderPath("NeuralNR") / L"nvngx_dlssnr.dll";
			if (!std::filesystem::exists(path)) {
				path = Util::PathHelpers::GetShadersPath() / L"Upscaling" / L"Streamline" / L"nvngx_dlssnr.dll";
			}
			hNR = LoadLibraryW(path.c_str());
		}

		if (hCore) PatchModuleIATAny(hCore, "GetModuleFileNameW");
		if (hNGX)  PatchModuleIATAny(hNGX,  "GetModuleFileNameW");
		if (hNR)   PatchModuleIATAny(hNR,   "GetModuleFileNameW");

		logger::info("NeuralNR: IAT caller-spoof installed (hCore={}, hNGX={}, hNR={}).",
			hCore != nullptr, hNGX != nullptr, hNR != nullptr);
	}

	void Uninstall()
	{
		// IAT uninstallation is inherently handled on process exit.
	}
}