#include "Features/NeuralNR/CallerSpoof.h"
#include "Features/NeuralNR.h"
#include "Utils/FileSystem.h"
#include "Globals.h"
#include <windows.h>
#include <intrin.h>
#include <thread>
#include <vector>
#include <atomic>
#include <string>
#include <detours/detours.h>

#pragma intrinsic(_ReturnAddress)

namespace CSS::CallerSpoof
{
	// PATCH: resolves a return address to the short filename of the module
	// that owns it — the same technique the NGX snippet's own caller-
	// identity check uses internally (per the guide's Section 2). Lets
	// every hook below report WHICH module's code actually made the call,
	// not just which modules we successfully patched. That's the direct
	// answer to "does the data come from _nvngx.dll or sl.dlss.dll" --
	// PatchModuleIATAny only tells us which modules import these names,
	// this tells us which one's import slot actually got exercised.
	static std::string GetCallingModuleShortName(void* returnAddress)
	{
		HMODULE hCallingModule = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
				reinterpret_cast<LPCWSTR>(returnAddress), &hCallingModule) || !hCallingModule)
			return "<unresolved>";

		wchar_t path[MAX_PATH]{};
		if (!GetModuleFileNameW(hCallingModule, path, MAX_PATH))
			return "<unresolved>";

		const wchar_t* lastBackslash = wcsrchr(path, L'\\');
		const wchar_t* lastSlash = wcsrchr(path, L'/');
		if (lastSlash && (!lastBackslash || lastSlash > lastBackslash)) lastBackslash = lastSlash;
		const wchar_t* fileName = lastBackslash ? lastBackslash + 1 : path;

		char narrow[MAX_PATH]{};
		size_t converted = 0;
		if (wcstombs_s(&converted, narrow, sizeof(narrow), fileName, _TRUNCATE) != 0)
			return "<unresolved>";
		return std::string(narrow);
	}

	using PFN_CreateFeature = NVSDK_NGX_Result (*)(ID3D11DeviceContext*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
	static PFN_CreateFeature s_orig_NGXCreate = nullptr;

	using PFN_EvaluateFeature = NVSDK_NGX_Result (*)(ID3D11DeviceContext*, NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, void*);
	static PFN_EvaluateFeature s_orig_NGXEvaluate = nullptr;

	// PATCH: new candidate parameter source. The guide's Section 4
	// describes the CORE's generic GetCapabilityParameters block as what
	// CreateFeature actually expects -- distinct from a feature-specific
	// block already captured inside another feature's own CreateFeature
	// call (which is what the SuperSampling fallback borrows). Capturing
	// this separately lets NeuralNR's own CreateFeature try both as
	// independent candidates rather than assuming one is correct.
	using PFN_GetCapabilityParameters = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
	static PFN_GetCapabilityParameters s_orig_NGXGetCapParams = nullptr;

	// PATCH: slOnPluginLoad's real signature is unconfirmed here — the guide
	// credits it to a third-party report on a different game under D3D12,
	// not this D3D11 environment. Treated as generically/safely as possible:
	// a single opaque context pointer, matching the guide's own description
	// ("captures that object"). Deliberately NOT dereferencing its contents
	// yet — only logging the raw pointer value — until DumpModuleImports
	// confirms this name (or something like it) actually exists here, and
	// ideally until its real layout is confirmed rather than guessed.
	using PFN_slOnPluginLoad = void* (*)(void*);
	static PFN_slOnPluginLoad s_orig_slOnPluginLoad = nullptr;

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
	// PATCH: feature ID confirmed via kFeatureDLSSNR (18) rather than assumed
	// from timing alone — the ID is already present in this call's own
	// signature (feat), just previously unused for the steal decision.
	constexpr int kFeatureDLSSNR = 18;

	static NVSDK_NGX_Result Hooked_NGXCreate(ID3D11DeviceContext* ctx, NVSDK_NGX_Feature feat, NVSDK_NGX_Parameter* param, NVSDK_NGX_Handle** handle)
	{
		auto& s = NeuralNR::GetState();
		std::string callerModule = GetCallingModuleShortName(_ReturnAddress());

		logger::info("NeuralNR [Diag]: Intercepted CreateFeature from {}! Target FeatureID: {}", callerModule, static_cast<uint32_t>(feat));

		const bool isNR = (static_cast<int>(feat) == kFeatureDLSSNR);
		const bool isSR = (feat == NVSDK_NGX_Feature_SuperSampling);

		if (!s.streamlineContextCaptured && param && isNR)
		{
			__try {
				logger::info("NeuralNR [Diag]: Confirmed Feature 18 (Neural Rendering) via {} — stealing NGX_Parameter block: {}", callerModule, (void*)param);
				s.nrParams = param;
				s.nrParamsCallerModule = callerModule;
				s.streamlineContextCaptured = true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				logger::warn("NeuralNR [Diag]: SEH caught access violation during CreateFeature parameter steal.");
			}
		}
		// PATCH: wide-net fallback, tried alongside the confirmed-Feature-18
		// path above rather than replacing it. Feature 18 has not been
		// observed even once across every test this session — consistent
		// with the guide's own Section 2 (dlss_nr_0 absent from Streamline's
		// OTA manifest, plugin self-disables without the separate patch-
		// and-inject step, which nothing here does). Rather than keep
		// waiting on an event that may structurally never occur, this
		// borrows SR's own live, already-validated block and lets our own
		// directly-resolved CreateFeature attempt feature 18 against it —
		// a genuinely different hypothesis (untested block completeness,
		// per the guide's section 4 warning that a freshly allocated block
		// "lacks the snippet and preset callbacks the feature expects").
		// Logged distinctly so it's always clear which path actually
		// supplied the block if this fires.
		else if (!s.streamlineContextCaptured && param && isSR)
		{
			__try {
				logger::info("NeuralNR [Diag]: Feature 18 not observed — borrowing live Feature 1 (SuperSampling) NGX_Parameter block via {} as a wide-net fallback: {}", callerModule, (void*)param);
				s.nrParams = param;
				s.nrParamsCallerModule = callerModule;
				s.streamlineContextCaptured = true;
				// PATCH: marks this as the borrowed path so NeuralNR.cpp's own
				// diagnostic logs can state which source supplied nrParams,
				// without needing to cross-reference this separate log stream.
				s.paramsAreBorrowed = true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				logger::warn("NeuralNR [Diag]: SEH caught access violation during SuperSampling parameter borrow.");
			}
		}
		// PATCH: second, independent candidate. If a primary block was
		// already captured, but THIS call is for the same feature category
		// (NR or SR) from a genuinely DIFFERENT module than the one that
		// supplied the primary, capture it too -- directly tests "what if
		// two modules both call this, and we locked onto the wrong one
		// first" instead of only ever observing the second caller in the
		// log without trying its data. Never overwrites nrParams; captured
		// once, tried by CreateFeature only if the primary fails.
		else if (s.streamlineContextCaptured && param && (isNR || isSR) && !s.nrParamsAlt && callerModule != s.nrParamsCallerModule)
		{
			__try {
				logger::info("NeuralNR [Diag]: Second distinct caller observed ({}, feature {}) — capturing as an alternate candidate: {}", callerModule, static_cast<uint32_t>(feat), (void*)param);
				s.nrParamsAlt = param;
				s.nrParamsAltBorrowed = isSR;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				logger::warn("NeuralNR [Diag]: SEH caught access violation during alternate parameter capture.");
			}
		}

		NVSDK_NGX_Result createRes = static_cast<NVSDK_NGX_Result>(0xBAD00007);
		if (s_orig_NGXCreate) {
			__try {
				createRes = s_orig_NGXCreate(ctx, feat, param, handle);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				logger::warn("NeuralNR [Diag]: SEH caught crash inside original Streamline CreateFeature!");
				return static_cast<NVSDK_NGX_Result>(0xDEADBEEF);
			}
		}

		// PATCH: record the real handle NGX returned specifically for feature
		// 18, so Hooked_NGXEvaluate — which never receives a feature ID, only
		// an opaque handle — can confirm by exact pointer match instead of
		// guessing which evaluate call belongs to NR.
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
		std::string callerModule = GetCallingModuleShortName(_ReturnAddress());

		static uint32_t logCounter = 0;
		if (logCounter++ % 300 == 0) {
			logger::info("NeuralNR [Diag]: EvaluateFeature running mid-game from {}. Handle={}, ParamBlock={}", callerModule, (void*)feat, (void*)param);
		}

		// PATCH: EvaluateFeature never receives a feature ID — only this
		// opaque handle. The only way to know it's genuinely NR is to check
		// it against the handle Hooked_NGXCreate already confirmed was
		// returned for feature 18. Without s.nrFeature set yet, there's
		// nothing trustworthy to steal here — it could be SR's or FG's
		// handle just as easily. Falls back to the old first-caller-wins
		// behavior only if Create was never seen at all (e.g. NR's feature
		// was already created before these hooks installed).
		const bool isConfirmedNR = s.nrFeature && (feat == s.nrFeature);
		const bool noCreateSeenYet = !s.nrFeature;

		if (!s.streamlineContextCaptured && param && (isConfirmedNR || noCreateSeenYet))
		{
			__try {
				logger::info("NeuralNR [Diag]: Stealing NGX_Parameter block from EvaluateFeature via {} (confirmed={}): {}", callerModule, isConfirmedNR, (void*)param);
				s.nrParams = param;
				if (isConfirmedNR) s.nrFeature = feat;
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

	static NVSDK_NGX_Result Hooked_NGXGetCapabilityParameters(NVSDK_NGX_Parameter** outParams)
	{
		std::string callerModule = GetCallingModuleShortName(_ReturnAddress());
		NVSDK_NGX_Result res = static_cast<NVSDK_NGX_Result>(0xBAD00007);
		if (s_orig_NGXGetCapParams) {
			__try {
				res = s_orig_NGXGetCapParams(outParams);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				logger::warn("NeuralNR [Diag]: SEH caught crash inside original GetCapabilityParameters (caller={})!", callerModule);
				return static_cast<NVSDK_NGX_Result>(0xDEADBEEF);
			}
		}

		auto& s = NeuralNR::GetState();
		if (NVSDK_NGX_SUCCEED(res) && outParams && *outParams && !s.capabilityParamsCaptured)
		{
			__try {
				logger::info("NeuralNR [Diag]: Captured core GetCapabilityParameters block via {}: {}", callerModule, (void*)*outParams);
				s.capabilityParams = *outParams;
				s.capabilityParamsCaptured = true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				logger::warn("NeuralNR [Diag]: SEH caught access violation during GetCapabilityParameters capture.");
			}
		}

		return res;
	}

	static void* WINAPI Hooked_slOnPluginLoad(void* pluginContext)
	{
		// Log the call, the caller, and the raw pointer first, before
		// touching anything -- confirms the hook fired at all regardless
		// of what happens next.
		std::string callerModule = GetCallingModuleShortName(_ReturnAddress());
		logger::info("NeuralNR [Diag]: slOnPluginLoad intercepted from {}! context={}", callerModule, pluginContext);

		if (s_orig_slOnPluginLoad) {
			__try {
				return s_orig_slOnPluginLoad(pluginContext);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				logger::warn("NeuralNR [Diag]: SEH caught crash forwarding to original slOnPluginLoad — signature guess was likely wrong.");
				return pluginContext; // best-effort passthrough rather than losing the value entirely
			}
		}
		return pluginContext;
	}

	static FARPROC WINAPI Hooked_GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
	{
		if (lpProcName && ((ULONG_PTR)lpProcName > 0xFFFF))
		{
			// PATCH: log EVERY distinct symbol name looked up on our hooked
			// modules, not just the ones we react to. This is the actual
			// "stop guessing" step — rather than trying one name at a time
			// across separate builds, this dumps every real dynamic lookup
			// Streamline's D3D11 plugins make in a single test run. Dedup'd
			// by name so a hot lookup path doesn't flood the log.
			{
				static std::vector<std::string> s_loggedNames;
				bool alreadyLogged = false;
				for (const auto& n : s_loggedNames) {
					if (_stricmp(n.c_str(), lpProcName) == 0) { alreadyLogged = true; break; }
				}
				if (!alreadyLogged) {
					s_loggedNames.emplace_back(lpProcName);
					logger::info("NeuralNR [Dump]: GetProcAddress lookup for: {}", lpProcName);
				}
			}

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
			if (_stricmp(lpProcName, "slOnPluginLoad") == 0)
			{
				if (!s_orig_slOnPluginLoad) s_orig_slOnPluginLoad = (PFN_slOnPluginLoad)(s_origGetProcAddress ? s_origGetProcAddress(hModule, lpProcName) : ::GetProcAddress(hModule, lpProcName));
				return (FARPROC)Hooked_slOnPluginLoad;
			}
			if (_stricmp(lpProcName, "NVSDK_NGX_D3D11_GetCapabilityParameters") == 0)
			{
				if (!s_orig_NGXGetCapParams) s_orig_NGXGetCapParams = (PFN_GetCapabilityParameters)(s_origGetProcAddress ? s_origGetProcAddress(hModule, lpProcName) : ::GetProcAddress(hModule, lpProcName));
				return (FARPROC)Hooked_NGXGetCapabilityParameters;
			}
		}
		
		if (s_origGetProcAddress) return s_origGetProcAddress(hModule, lpProcName);
		return ::GetProcAddress(hModule, lpProcName);
	}

	// PATCH: now returns whether a matching import was actually found and
	// patched, instead of silently returning nothing either way. Previously
	// the "Successfully attached..." log fired unconditionally regardless
	// of whether any of the four PatchModuleIATAny calls actually found
	// anything -- meaning a module could report "success" while genuinely
	// importing none of these names, which is exactly what made it
	// impossible to tell which of _nvngx.dll / sl.dlss.dll / etc. actually
	// carries these imports versus which ones we just happened to iterate.
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
		bool foundAny = false;

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
							foundAny = true;
						}
					}
				}
				originalFirstThunk++;
				firstThunk++;
			}
			importDesc++;
		}
		return foundAny;
	}

	// PATCH: safe, READ-ONLY diagnostic — no patching, no calling, just
	// walking and logging every import table entry a module actually has.
	// This directly answers the ordinal question from last message (are
	// NVSDK_NGX_D3D11_CreateFeature/EvaluateFeature genuinely absent by
	// name, or ordinal-only?) and gives full visibility into what THIS
	// specific D3D11 build's Streamline plugins actually call, instead of
	// continuing to guess names one at a time from a Vulkan/D3D12 report.
	// Ordinal-only entries are logged as "ordinal N" since there's no name
	// to print, but they ARE logged (PatchModuleIATAny above still skips
	// them for patching purposes — this is purely informational).
	static void DumpModuleImports(HMODULE hTargetModule, const char* moduleNameForLog)
	{
		if (!hTargetModule) return;

		PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)hTargetModule;
		if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return;

		PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)((BYTE*)hTargetModule + dosHeader->e_lfanew);
		if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return;

		DWORD importDirRVA = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
		if (!importDirRVA)
		{
			logger::info("NeuralNR [Dump]: {} has no import table.", moduleNameForLog);
			return;
		}

		PIMAGE_IMPORT_DESCRIPTOR importDesc = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hTargetModule + importDirRVA);

		while (importDesc->Name)
		{
			const char* dllName = (const char*)((BYTE*)hTargetModule + importDesc->Name);
			PIMAGE_THUNK_DATA originalFirstThunk = (PIMAGE_THUNK_DATA)((BYTE*)hTargetModule + importDesc->OriginalFirstThunk);

			while (originalFirstThunk->u1.AddressOfData)
			{
				if (originalFirstThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
				{
					logger::info("NeuralNR [Dump]: {} imports from {}: ordinal {}",
						moduleNameForLog, dllName, static_cast<uint32_t>(IMAGE_ORDINAL(originalFirstThunk->u1.Ordinal)));
				}
				else
				{
					PIMAGE_IMPORT_BY_NAME importByName = (PIMAGE_IMPORT_BY_NAME)((BYTE*)hTargetModule + originalFirstThunk->u1.AddressOfData);
					logger::info("NeuralNR [Dump]: {} imports from {}: {}",
						moduleNameForLog, dllName, (const char*)importByName->Name);
				}
				originalFirstThunk++;
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
	// PATCH: std::vector<bool> is a well-known special case that bit-packs
	// several booleans into shared storage words — writes to different
	// indices from different threads can be a genuine data race if they
	// land in the same word, unlike a normal vector<T>. With only 10
	// entries here, several very plausibly share a word. This is touched
	// from both the background polling thread (InstallUpscalerHooks) and
	// the main render thread (InstallActiveInterceptors, every frame from
	// OnPresent) — real atomics give each entry independent storage.
	// Worth fixing on correctness grounds regardless of whether it's
	// confirmed as the cause of any specific observed crash.
	static std::atomic<bool> s_hookedStatus[std::size(s_targetModules)] = {};

	// Extracted into its own function so the patching logic lives in one
	// place, called from InstallActiveInterceptors below.
	static void PatchTargetModuleAndMarkHooked(HMODULE hMod, size_t idx)
	{
		char logName[64];
		size_t converted = 0;
		wcstombs_s(&converted, logName, sizeof(logName), s_targetModules[idx], _TRUNCATE);

		// PATCH: each result captured and logged individually now that
		// PatchModuleIATAny reports whether it actually found a match --
		// this is what tells us which specific module (_nvngx.dll,
		// sl.dlss.dll, etc.) genuinely imports each of these names, rather
		// than a single blanket "success" line that fired regardless.
		bool hasGetProcAddress = PatchModuleIATAny(hMod, "GetProcAddress", (void*)Hooked_GetProcAddress, (void**)&s_origGetProcAddress);
		bool hasCreateFeature  = PatchModuleIATAny(hMod, "NVSDK_NGX_D3D11_CreateFeature", (void*)Hooked_NGXCreate, (void**)&s_orig_NGXCreate);
		bool hasEvaluateFeature = PatchModuleIATAny(hMod, "NVSDK_NGX_D3D11_EvaluateFeature", (void*)Hooked_NGXEvaluate, (void**)&s_orig_NGXEvaluate);
		bool hasSlOnPluginLoad = PatchModuleIATAny(hMod, "slOnPluginLoad", (void*)Hooked_slOnPluginLoad, (void**)&s_orig_slOnPluginLoad);
		bool hasGetCapParams   = PatchModuleIATAny(hMod, "NVSDK_NGX_D3D11_GetCapabilityParameters", (void*)Hooked_NGXGetCapabilityParameters, (void**)&s_orig_NGXGetCapParams);

		DumpModuleImports(hMod, logName);

		logger::info("NeuralNR [Diag]: Scanned {} -- GetProcAddress={}, CreateFeature={}, EvaluateFeature={}, GetCapabilityParameters={}, slOnPluginLoad={} (true = import found and patched).",
			logName, hasGetProcAddress, hasCreateFeature, hasEvaluateFeature, hasGetCapParams, hasSlOnPluginLoad);
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
				if (hMod)
				{
					PatchTargetModuleAndMarkHooked(hMod, i);
				}
				else
				{
					allHooked = false;
				}
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

	// --- Instant-hook on module load, via Detours, as a lower-latency
	// alternative to polling. Confirmed available and correctly wired for
	// this exact target via CMakeLists.txt's find_path(DETOURS_INCLUDE_DIRS
	// "detours/detours.h") + target_include_directories/target_link_libraries
	// on ${PROJECT_NAME} — the earlier compile failure was solely the wrong
	// include path (detours.h instead of detours/detours.h), not a missing
	// or unwired dependency. Deliberately scoped to LoadLibrary* only — NOT
	// GetProcAddress, which is a much hotter, much more heavily-targeted
	// function in a crowded modding ecosystem (SKSE plugins, ENB, ReShade,
	// possibly other Community Shaders features) where a second, unrelated
	// inline hook on the same function's prologue bytes is a real collision
	// risk. LoadLibrary* is called far less often, making that collision
	// risk much smaller, and directly closes the actual gap observed in
	// testing: modules load, then sit undetected until the next 200ms poll
	// tick — sometimes several seconds late. This patches a matching module
	// the instant it loads instead. InstallActiveInterceptors() keeps
	// running in parallel regardless, as the deliberate fallback: if this
	// hook fails to install, or a target module was already loaded before
	// this installed at all, the poll still eventually catches it.
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
		// SEH-guarded: runs inside a hooked WinAPI entry point, on whatever
		// thread happened to call LoadLibrary — same uncertain-context
		// caution as every other hook in this file.
		__try
		{
			size_t idx = 0;
			if (hLoaded && FindTargetModuleIndex(fileName, &idx) && !s_hookedStatus[idx].load())
			{
				logger::info("NeuralNR [Diag]: Instant-hook caught a target module load via LoadLibrary — patching immediately instead of waiting for the next poll.");
				PatchTargetModuleAndMarkHooked(hLoaded, idx);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			logger::warn("NeuralNR [Diag]: SEH caught access violation in the LoadLibrary instant-patch path.");
		}
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
			logger::info("NeuralNR [Diag]: LoadLibrary instant-hook installed successfully — modules will be patched the moment they load instead of on the next poll tick.");
		}
		else
		{
			logger::warn("NeuralNR [Diag]: LoadLibrary instant-hook failed to install (err={}) — falling back to poll-only detection via InstallActiveInterceptors.", err);
		}
	}

	void InstallUpscalerHooks()
	{
		// Install the low-latency instant-hook first (synchronous, cheap).
		// The poll-based background thread still starts regardless right
		// after — deliberate belt-and-suspenders: whichever mechanism catches
		// a given module first wins, and if the Detours hook fails to
		// install for any reason, the poll thread alone still eventually gets
		// there, exactly as it did before this change.
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