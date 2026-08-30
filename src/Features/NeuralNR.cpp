#include "Features/NeuralNR.h"
#include "Features/NeuralNR/CallerSpoof.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "Utils/FileSystem.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>
#include <imgui.h>

namespace
{
	constexpr int kFeatureDLSSNR = 18;

	using PFN_GetCapParams       = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
	using PFN_CreateFeature      = decltype(&NVSDK_NGX_D3D11_CreateFeature);
	using PFN_EvaluateFeature    = decltype(&NVSDK_NGX_D3D11_EvaluateFeature);
	using PFN_ReleaseFeature     = decltype(&NVSDK_NGX_D3D11_ReleaseFeature);

	// Standard Init signature (NVSDK_NGX_D3D11_Init): (AppId, Path, Device, FeatureCommonInfo, Version)
	using PFN_InitExt_Std = NVSDK_NGX_Result (*)(
		unsigned long long InApplicationId,
		const wchar_t* InApplicationDataPath,
		ID3D11Device* InDevice,
		const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo,
		NVSDK_NGX_Version InSDKVersion);

	// Strategy 1 signature (Snippet ABI: Version + Parameter Block)
	using PFN_SnippetInit_Strategy1 = NVSDK_NGX_Result (*)(
		unsigned long long InApplicationId,
		const wchar_t* InApplicationDataPath,
		ID3D11Device* InDevice,
		NVSDK_NGX_Version InSDKVersion,
		NVSDK_NGX_Parameter* InParameters);

	// Strategy 2 signature (Standard NGX ABI: FeatureCommonInfo + Version)
	using PFN_SnippetInit_Strategy2 = PFN_InitExt_Std;

	// Strategy 3 signature (Flipped NGX ABI: Version + FeatureCommonInfo)
	using PFN_SnippetInit_Strategy3 = NVSDK_NGX_Result (*)(
		unsigned long long InApplicationId,
		const wchar_t* InApplicationDataPath,
		ID3D11Device* InDevice,
		NVSDK_NGX_Version InSDKVersion,
		const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo);

	// ---------------------------------------------------------------------------
	// Layer 2: SEH-Guarded Callers (POD only, no C++ unwinding objects in scope)
	// ---------------------------------------------------------------------------

	NVSDK_NGX_Result CallSnippetInit_Strategy1_SEH(
		FARPROC pfn,
		unsigned long long appId,
		const wchar_t* appPath,
		ID3D11Device* dev,
		NVSDK_NGX_Version ver,
		NVSDK_NGX_Parameter* params,
		bool* outFaulted)
	{
		if (outFaulted) *outFaulted = false;
		__try
		{
			auto fn = reinterpret_cast<PFN_SnippetInit_Strategy1>(pfn);
			return fn(appId, appPath, dev, ver, params);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (outFaulted) *outFaulted = true;
			return static_cast<NVSDK_NGX_Result>(0xDEADBEEF);
		}
	}

	NVSDK_NGX_Result CallSnippetInit_Strategy2_SEH(
		FARPROC pfn,
		unsigned long long appId,
		const wchar_t* appPath,
		ID3D11Device* dev,
		const NVSDK_NGX_FeatureCommonInfo* featInfo,
		NVSDK_NGX_Version ver,
		bool* outFaulted)
	{
		if (outFaulted) *outFaulted = false;
		__try
		{
			auto fn = reinterpret_cast<PFN_SnippetInit_Strategy2>(pfn);
			return fn(appId, appPath, dev, featInfo, ver);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (outFaulted) *outFaulted = true;
			return static_cast<NVSDK_NGX_Result>(0xDEADBEEF);
		}
	}

	NVSDK_NGX_Result CallSnippetInit_Strategy3_SEH(
		FARPROC pfn,
		unsigned long long appId,
		const wchar_t* appPath,
		ID3D11Device* dev,
		NVSDK_NGX_Version ver,
		const NVSDK_NGX_FeatureCommonInfo* featInfo,
		bool* outFaulted)
	{
		if (outFaulted) *outFaulted = false;
		__try
		{
			auto fn = reinterpret_cast<PFN_SnippetInit_Strategy3>(pfn);
			return fn(appId, appPath, dev, ver, featInfo);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (outFaulted) *outFaulted = true;
			return static_cast<NVSDK_NGX_Result>(0xDEADBEEF);
		}
	}

	bool IsActuallyHDROutput(IDXGISwapChain* swapChain)
	{
		bool isHDR = false;
		IDXGIOutput* output = nullptr;
		if (swapChain && SUCCEEDED(swapChain->GetContainingOutput(&output)) && output)
		{
			IDXGIOutput6* output6 = nullptr;
			if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6))))
			{
				DXGI_OUTPUT_DESC1 desc1{};
				if (SUCCEEDED(output6->GetDesc1(&desc1)))
					isHDR = (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
				output6->Release();
			}
			output->Release();
		}
		return isHDR;
	}
}

ID3D11Resource* NeuralNR::ResourceFromView(ID3D11View* view) const
{
	if (!view) return nullptr;
	ID3D11Resource* res = nullptr;
	view->GetResource(&res);
	if (res) res->Release();
	return res;
}

void NeuralNR::LoadDLL()
{
	auto& s = GetState();

	s.hDLL = GetModuleHandleW(L"_nvngx.dll");
	if (!s.hDLL) s.hDLL = GetModuleHandleW(L"nvngx.dll");
	if (!s.hDLL) s.hDLL = LoadLibraryW(L"nvngx.dll");
	if (!s.hDLL) s.hDLL = LoadLibraryW(L"_nvngx.dll");

	if (!s.hDLL)
	{
		logger::warn("NeuralNR: Could not find the core NGX library (nvngx.dll or _nvngx.dll) — Streamline/DLSS may not have initialized it yet.");
	}
	else
	{
		// Layer 1: Confirmed safe plain Init symbol as the primary core target
		s.pfnInitExt                 = GetProcAddress(s.hDLL, "NVSDK_NGX_D3D11_Init");
		if (!s.pfnInitExt) s.pfnInitExt = GetProcAddress(s.hDLL, "NVSDK_NGX_D3D11_Init_Ext");
		s.pfnGetCapabilityParameters = GetProcAddress(s.hDLL, "NVSDK_NGX_D3D11_GetCapabilityParameters");
	}

	std::wstring snippetPath = Util::PathHelpers::GetShadersPath() / L"Upscaling" / L"Streamline" / L"nvngx_dlssnr.dll";
	s.hSnippetDLL = LoadLibraryW(snippetPath.c_str());

	if (s.hSnippetDLL)
	{
		s.pfnPopulateParams   = GetProcAddress(s.hSnippetDLL, "NVSDK_NGX_D3D11_PopulateParameters_Impl");
		s.pfnCreateFeature    = GetProcAddress(s.hSnippetDLL, "NVSDK_NGX_D3D11_CreateFeature");
		s.pfnEvaluateFeature  = GetProcAddress(s.hSnippetDLL, "NVSDK_NGX_D3D11_EvaluateFeature");
		s.pfnReleaseFeature   = GetProcAddress(s.hSnippetDLL, "NVSDK_NGX_D3D11_ReleaseFeature");
	}
	else
	{
		logger::warn("NeuralNR: Could not load snippet nvngx_dlssnr.dll directly.");
	}
}

bool NeuralNR::CheckGate()
{
	auto dev = globals::d3d::device;
	IDXGIDevice* dxgiDev = nullptr;
	if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) return false;
	IDXGIAdapter* adp = nullptr;
	bool ok = SUCCEEDED(dxgiDev->GetAdapter(&adp));
	dxgiDev->Release();
	if (!ok || !adp) return false;
	IDXGIAdapter1* adp1 = nullptr;
	ok = SUCCEEDED(adp->QueryInterface(IID_PPV_ARGS(&adp1)));
	adp->Release();
	if (!ok || !adp1) return false;
	DXGI_ADAPTER_DESC1 d{};
	adp1->GetDesc1(&d);
	adp1->Release();
	if (d.VendorId != 0x10DE) return false;
	if (globals::features::upscaling.GetUpscaleMethod() != Upscaling::UpscaleMethod::kDLSS) return false;
	return true;
}

bool NeuralNR::CompileShaders()
{
	auto& s = GetState();
	auto dir = Util::PathHelpers::GetShadersPath();

	auto compile = [&](const char* file, const char* entry, ID3D11ComputeShader** out) {
		std::ifstream f(dir / file, std::ios::binary);
		if (!f) { logger::warn("NeuralNR: missing shader {}", file); return false; }
		
		std::stringstream ss; ss << f.rdbuf();
		auto src = ss.str();
		
		ID3DBlob* blob = nullptr;
		ID3DBlob* errorBlob = nullptr;
		
		if (FAILED(D3DCompile(src.c_str(), src.size(), file, nullptr,
			D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, "cs_5_0", 0, 0, &blob, &errorBlob)))
		{ 
			if (errorBlob) {
				logger::warn("NeuralNR: D3DCompile failed for {}\nCompiler Output:\n{}", file, static_cast<const char*>(errorBlob->GetBufferPointer()));
				errorBlob->Release();
			} else {
				logger::warn("NeuralNR: D3DCompile failed for {} (No error blob generated)", file); 
			}
			return false; 
		}
		
		auto hr = globals::d3d::device->CreateComputeShader(
			blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out);
			
		blob->Release();
		return SUCCEEDED(hr) && *out;
	};

	return compile("NeuralNR_SDRProxy.hlsl", "CS_GenerateSDRProxy", &s.proxyCS)
		&& compile("NeuralNR_Transfer.hlsl", "CS_TransferEditToHDR", &s.transferCS);
}

bool NeuralNR::CreateFeature()
{
	auto& s = GetState();
	if (!s.pfnCreateFeature || !s.nrParams) return false;

	CSS::CallerSpoof::Install();
	
	NVSDK_NGX_Result res = ((PFN_CreateFeature)s.pfnCreateFeature)(
		globals::d3d::context, static_cast<NVSDK_NGX_Feature>(kFeatureDLSSNR), s.nrParams, &s.nrFeature);
		
	CSS::CallerSpoof::Uninstall();

	if (!NVSDK_NGX_SUCCEED(res) || !s.nrFeature)
	{
		logger::error("NeuralNR: CreateFeature failed, res=0x{:X}", static_cast<uint32_t>(res));
		return false;
	}
	logger::info("NeuralNR: CreateFeature Success.");
	return true;
}

void NeuralNR::CreateResources(uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
	auto& s = GetState();
	const bool sizeChanged = (s.w != w || s.h != h || s.fmt != fmt);

	auto dev = globals::d3d::device;
	const auto hdrFmt = (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? DXGI_FORMAT_R16G16B16A16_FLOAT : fmt;
	const auto srgb   = DXGI_FORMAT_R8G8B8A8_UNORM;

	auto mkTex = [&](ID3D11Texture2D** out, DXGI_FORMAT f, UINT bind) {
		D3D11_TEXTURE2D_DESC td{};
		td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = f; td.SampleDesc = {1,0}; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind;
		return SUCCEEDED(dev->CreateTexture2D(&td, nullptr, out));
	};
	auto mkSRV = [&](ID3D11Texture2D* t, DXGI_FORMAT f, ID3D11ShaderResourceView** out) {
		D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
		sd.Format = f; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
		return SUCCEEDED(dev->CreateShaderResourceView(t, &sd, out));
	};
	auto mkUAV = [&](ID3D11Texture2D* t, DXGI_FORMAT f, ID3D11UnorderedAccessView** out) {
		D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
		ud.Format = f; ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D; ud.Texture2D.MipSlice = 0;
		return SUCCEEDED(dev->CreateUnorderedAccessView(t, &ud, out));
	};

	auto tryCreateMotionVectorTexture = [&]() {
		if (s.mvSRV) return; 
		if (auto* mv = globals::features::upscaling.motionVectorCopyTexture)
		{
			auto* raw = static_cast<ID3D11Texture2D*>(mv->resource.get());
			if (raw)
			{
				D3D11_TEXTURE2D_DESC desc{};
				raw->GetDesc(&desc);
				mkSRV(raw, desc.Format, &s.mvSRV);
			}
		}
	};

	auto tryCreateDepthTexture = [&]() {
		if (s.depthSRV) return;
		auto* depthSRV = globals::game::renderer->GetDepthStencilData()
			.depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
		if (depthSRV)
		{
			s.depthSRV = depthSRV;
			s.depthSRV->AddRef();
			logger::info("NeuralNR: Native Depth SRV successfully acquired!");
		}
	};

	if (!sizeChanged)
	{
		tryCreateMotionVectorTexture();
		tryCreateDepthTexture();
		return;
	}

	ReleaseResources();
	s.w = w; s.h = h; s.fmt = fmt;

	mkTex(&s.inputColor,  hdrFmt, D3D11_BIND_SHADER_RESOURCE);
	mkSRV(s.inputColor,  hdrFmt, &s.inputSRV);
	mkTex(&s.sdrProxyTex, srgb, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
	mkSRV(s.sdrProxyTex, srgb, &s.sdrProxySRV);
	mkUAV(s.sdrProxyTex, srgb, &s.sdrProxyUAV);
	mkTex(&s.nrOutputTex, srgb, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
	mkSRV(s.nrOutputTex, srgb, &s.nrOutputSRV);
	mkUAV(s.nrOutputTex, srgb, &s.nrOutputUAV);
	mkTex(&s.transferOut, hdrFmt, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
	mkUAV(s.transferOut, hdrFmt, &s.transferOutUAV);

	tryCreateMotionVectorTexture();
	tryCreateDepthTexture();

	struct Tuning { float paperWhite, encode, _0, _1; };
	Tuning t{ settings.paperWhiteNits, settings.encodeStrength, 0.f, 0.f };
	D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(Tuning); bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &s.tuningCB)))
		globals::d3d::context->UpdateSubresource(s.tuningCB, 0, nullptr, &t, 0, 0);
}

void NeuralNR::ReleaseResources()
{
	auto& s = GetState();
	auto rel = [](auto** p) { if (*p) { (*p)->Release(); *p = nullptr; } };
	rel(&s.inputSRV); rel(&s.sdrProxySRV); rel(&s.nrOutputSRV); rel(&s.mvSRV); rel(&s.depthSRV);
	rel(&s.sdrProxyUAV); rel(&s.nrOutputUAV); rel(&s.transferOutUAV);
	rel(&s.inputColor); rel(&s.sdrProxyTex); rel(&s.nrOutputTex); rel(&s.transferOut); rel(&s.mvTex);
	rel(&s.proxyCS); rel(&s.transferCS); rel(&s.tuningCB);
}

void NeuralNR::ReleaseFeature()
{
	auto& s = GetState();
	if (s.pfnReleaseFeature && s.nrFeature)
		((PFN_ReleaseFeature)s.pfnReleaseFeature)(s.nrFeature);
		
	s.nrParams = nullptr;
	s.nrFeature = nullptr;
}

void NeuralNR::DispatchProxy()
{
	auto& s = GetState(); auto ctx = globals::d3d::context;
	ctx->CSSetShader(s.proxyCS, nullptr, 0);
	ctx->CSSetShaderResources(0, 1, &s.inputSRV);
	ctx->CSSetUnorderedAccessViews(0, 1, &s.sdrProxyUAV, nullptr);
	ctx->CSSetConstantBuffers(0, 1, &s.tuningCB);
	ctx->Dispatch((s.w + 7) / 8, (s.h + 7) / 8, 1);
	ID3D11ShaderResourceView* n0 = nullptr; ID3D11UnorderedAccessView* n1 = nullptr;
	ctx->CSSetShaderResources(0, 1, &n0);
	ctx->CSSetUnorderedAccessViews(0, 1, &n1, nullptr);
}

void NeuralNR::DispatchTransfer()
{
	auto& s = GetState(); auto ctx = globals::d3d::context;
	ctx->CSSetShader(s.transferCS, nullptr, 0);
	ID3D11ShaderResourceView* srcs[3] = { s.inputSRV, s.sdrProxySRV, s.nrOutputSRV };
	ctx->CSSetShaderResources(0, 3, srcs);
	ctx->CSSetUnorderedAccessViews(0, 1, &s.transferOutUAV, nullptr);
	ctx->Dispatch((s.w + 7) / 8, (s.h + 7) / 8, 1);
	ID3D11ShaderResourceView* n0 = nullptr; ID3D11UnorderedAccessView* n1 = nullptr;
	ctx->CSSetShaderResources(0, 3, &n0);
	ctx->CSSetUnorderedAccessViews(0, 1, &n1, nullptr);
}

void NeuralNR::OnPresent()
{
	if (!settings.enabled) return;

	auto& s = GetState();

	if (!s.initialized)
	{
		static uint32_t s_retryFrameCounter = 0;
		static uint32_t s_setupAttempts = 0;
		constexpr uint32_t kSetupRetryIntervalFrames = 120;
		constexpr uint32_t kMaxSetupAttempts = 300;

		const bool firstAttempt = (s_setupAttempts == 0);
		const bool dueForRetry  = (++s_retryFrameCounter >= kSetupRetryIntervalFrames) && (s_setupAttempts < kMaxSetupAttempts);
		if (firstAttempt || dueForRetry)
		{
			s_retryFrameCounter = 0;
			++s_setupAttempts;
			PostPostLoad();
		}
	}

	if (!s.pfnEvaluateFeature || !s.nrParams) 
	{
		return;
	}

	auto ctx = globals::d3d::context;
	ID3D11Texture2D* back = nullptr;
	if (FAILED(globals::d3d::swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
	D3D11_TEXTURE2D_DESC dsc{}; back->GetDesc(&dsc);
	const uint32_t w = dsc.Width, h = dsc.Height;
	if (s.w != w || s.h != h) s.needsReset = true;
	CreateResources(w, h, dsc.Format);

	auto* P = s.nrParams;
	P->Set("DLSSNR.Width",  (int)w);
	P->Set("DLSSNR.Height", (int)h);
	P->Set(NVSDK_NGX_Parameter_Width,  (int)w);
	P->Set(NVSDK_NGX_Parameter_Height, (int)h);
	P->Set(NVSDK_NGX_Parameter_OutWidth,  (int)w);
	P->Set(NVSDK_NGX_Parameter_OutHeight, (int)h);
	
	auto SetSubrect = [&](const char* name) {
		P->Set((std::string(name) + "SubrectBaseX").c_str(), 0);
		P->Set((std::string(name) + "SubrectBaseY").c_str(), 0);
		P->Set((std::string(name) + "SubrectWidth").c_str(),  (int)w);
		P->Set((std::string(name) + "SubrectHeight").c_str(), (int)h);
	};
	SetSubrect("DLSSNR.Color"); 
	SetSubrect("DLSSNR.MVec");
	SetSubrect("DLSSNR.Depth"); 
	SetSubrect("DLSSNR.Output");

	if (!s.initialized)
	{
		static uint32_t s_createRetry = 0;
		static int s_strategy = 1;
		static bool s_snippetInitialized = false;

		if (s_createRetry++ % 60 == 0) 
		{
			if (s_strategy > 4) {
				logger::error("NeuralNR: All 4 initialization strategies exhausted. Feature permanently disabled.");
				s.initialized = true; 
				back->Release();
				return;
			}

			if (!s_snippetInitialized) 
			{
				logger::info("NeuralNR: --- Executing Snippet Init Strategy {} ---", s_strategy);
				
				static std::wstring appPath = Util::PathHelpers::GetFeatureShaderPath("NeuralNR").wstring();
				static std::wstring dllSearchPath = appPath;
				if (!std::filesystem::exists(Util::PathHelpers::GetFeatureShaderPath("NeuralNR") / L"nvngx_dlssnr.dll")) {
					dllSearchPath = (Util::PathHelpers::GetShadersPath() / L"Upscaling" / L"Streamline").wstring();
				}
				static const wchar_t* searchPaths[] = { dllSearchPath.c_str() };
				static NVSDK_NGX_FeatureCommonInfo featureInfo{};
				featureInfo.PathListInfo.Path = searchPaths;
				featureInfo.PathListInfo.Length = 1;

				auto pfnInitExt = GetProcAddress(s.hSnippetDLL, "NVSDK_NGX_D3D11_Init_Ext");
				if (!pfnInitExt && s_strategy != 4) {
					pfnInitExt = GetProcAddress(s.hSnippetDLL, "NVSDK_NGX_D3D11_Init");
				}

				NVSDK_NGX_Result snippetInitRes = static_cast<NVSDK_NGX_Result>(-1);
				bool faulted = false;

				CSS::CallerSpoof::Install();

				if (s_strategy == 1 && pfnInitExt) {
					logger::info("NeuralNR: Strategy 1 -> Snippet ABI + Dev/Avail Capability Injection (SEH Guarded)");
					s.nrParams->Set("DLSSNR.Available", 1);
					s.nrParams->Set("SuperSampling.Available", 1);
					s.nrParams->Set("NVSDK_NGX_Parameter_DevOverride", 1);
					
					snippetInitRes = CallSnippetInit_Strategy1_SEH(
						reinterpret_cast<FARPROC>(pfnInitExt),
						231313132ULL,
						appPath.c_str(),
						globals::d3d::device,
						NVSDK_NGX_Version_API,
						s.nrParams,
						&faulted);
				}
				else if (s_strategy == 2 && pfnInitExt) {
					logger::info("NeuralNR: Strategy 2 -> Standard ABI (FeatureCommonInfo + Version) (SEH Guarded)");
					snippetInitRes = CallSnippetInit_Strategy2_SEH(
						reinterpret_cast<FARPROC>(pfnInitExt),
						231313132ULL,
						appPath.c_str(),
						globals::d3d::device,
						&featureInfo,
						NVSDK_NGX_Version_API,
						&faulted);
				}
				else if (s_strategy == 3 && pfnInitExt) {
					logger::info("NeuralNR: Strategy 3 -> Flipped ABI (Version + FeatureCommonInfo) (SEH Guarded)");
					snippetInitRes = CallSnippetInit_Strategy3_SEH(
						reinterpret_cast<FARPROC>(pfnInitExt),
						231313132ULL,
						appPath.c_str(),
						globals::d3d::device,
						NVSDK_NGX_Version_API,
						&featureInfo,
						&faulted);
				}
				else if (s_strategy == 4) {
					logger::info("NeuralNR: Strategy 4 -> Bypass Snippet Init, force Populated parameters (Zero-Risk Fallback)");
					snippetInitRes = static_cast<NVSDK_NGX_Result>(1); // NVSDK_NGX_Result_Success
				}

				CSS::CallerSpoof::Uninstall();

				if (faulted) {
					logger::error("NeuralNR: Strategy {} triggered an Access Violation (SEH caught & recovered safely).", s_strategy);
				}

				if (NVSDK_NGX_SUCCEED(snippetInitRes) && !faulted) {
					logger::info("NeuralNR: Strategy {} Snippet Handshake Succeeded!", s_strategy);
					s_snippetInitialized = true;
					
					if (s.pfnPopulateParams) {
						using PFN_Populate = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter*);
						((PFN_Populate)s.pfnPopulateParams)(s.nrParams);
					}
				} else {
					logger::warn("NeuralNR: Strategy {} Snippet Handshake Failed (0x{:X}, Faulted={})", 
						s_strategy, static_cast<uint32_t>(snippetInitRes), faulted);
					s_strategy++;
					back->Release();
					return;
				}
			}

			if (s_snippetInitialized && !s.nrFeature) {
				if (CreateFeature()) {
					s.initialized = true;
					logger::info("NeuralNR: Initialization fully stabilized on Strategy {}.", s_strategy);
				} else {
					logger::warn("NeuralNR: CreateFeature rejected Strategy {}. Resetting and advancing matrix...", s_strategy);
					s_snippetInitialized = false; 
					s_strategy++;
					back->Release();
					return;
				}
			}
		}
		else
		{
			back->Release();
			return; 
		}
	}

	if (!s.inputColor || !s.nrOutputTex || !s.mvSRV || !s.depthSRV) 
	{
		static uint32_t s_resourceWaitCounter = 0;
		if (++s_resourceWaitCounter % 300 == 1)
		{
			logger::info("NeuralNR: Feature created! Waiting for active scene render targets (Depth={}, MVec={})...",
				s.depthSRV != nullptr, s.mvSRV != nullptr);
		}
		back->Release(); 
		return; 
	}

	const bool hdr = IsActuallyHDROutput(globals::d3d::swapChain);

	ctx->CopyResource(s.inputColor, back);
	if (hdr) DispatchProxy();

	P->Set("DLSSNR.Hint.Render.Preset", settings.preset);
	P->Set("DLSSNR.Enabled", 1);
	if (s.needsReset.exchange(false)) P->Set("DLSSNR.Reset", 1);
	P->Set("DLSSNR.Style",                 settings.style);
	P->Set("DLSSNR.Intensity",             settings.intensity);
	P->Set("DLSSNR.LocalToneStrength",     settings.localTone);
	P->Set("DLSSNR.LocalStructureStrength",settings.localStructure);
	P->Set("DLSSNR.SkinStructureStrength", settings.skinStructure);
	P->Set("DLSSNR.UseAutoMask",           settings.useAutoMask);
	P->Set("DLSSNR.MVecScaleX",            settings.mvScaleX);
	P->Set("DLSSNR.MVecScaleY",            settings.mvScaleY);
	P->Set("DLSSNR.DepthInverted",         settings.depthInverted);
	P->Set("NRPaperWhiteNits",             settings.paperWhiteNits);
	P->Set("NREncodeStrength",             settings.encodeStrength);

	P->Set("DLSSNR.Color",  (ID3D11Resource*)(hdr ? s.sdrProxyTex : s.inputColor));
	P->Set("DLSSNR.Output", (ID3D11Resource*)s.nrOutputTex);
	P->Set("DLSSNR.MVec",   ResourceFromView(s.mvSRV));
	P->Set("DLSSNR.Depth",  ResourceFromView(s.depthSRV));

	NVSDK_NGX_Result evalRes = ((PFN_EvaluateFeature)s.pfnEvaluateFeature)(
		ctx, s.nrFeature, P, nullptr);

	static bool s_loggedEval = false;
	if (NVSDK_NGX_SUCCEED(evalRes) && !s_loggedEval)
	{
		logger::info("NeuralNR: First frame evaluated successfully! Format=0x{:X}", static_cast<uint32_t>(dsc.Format));
		s_loggedEval = true;
	}
	else if (!NVSDK_NGX_SUCCEED(evalRes))
	{
		static NVSDK_NGX_Result s_lastLoggedFailure = static_cast<NVSDK_NGX_Result>(-1);
		static uint32_t s_failureLogFrameCounter = 0;
		constexpr uint32_t kFailureLogIntervalFrames = 300; 

		const bool isNewFailureCode = (evalRes != s_lastLoggedFailure);
		++s_failureLogFrameCounter;
		if (isNewFailureCode || s_failureLogFrameCounter >= kFailureLogIntervalFrames)
		{
			logger::warn("NeuralNR: EvaluateFeature failed, res=0x{:X}", static_cast<uint32_t>(evalRes));
			s_lastLoggedFailure = evalRes;
			s_failureLogFrameCounter = 0;
		}
		back->Release();
		return;
	}

	if (hdr) { DispatchTransfer(); ctx->CopyResource(back, s.transferOut); }
	else     { ctx->CopyResource(back, s.nrOutputTex); }
	back->Release();
}

void NeuralNR::PostPostLoad()
{
	auto& s = GetState();
	LoadDLL();
	
	if (!s.hSnippetDLL || !s.pfnCreateFeature || !s.pfnEvaluateFeature)
	{ logger::info("NeuralNR: Snippet DLL or required exports not found — disabled"); return; }
	
	if (!CheckGate())
	{ logger::info("NeuralNR: gate failed — disabled"); return; }
	
	if (!CompileShaders())
	{ logger::info("NeuralNR: shader compile failed"); return; }

	static std::wstring appPath = Util::PathHelpers::GetFeatureShaderPath("NeuralNR").wstring();
	static std::wstring dllSearchPath = appPath;

	if (!std::filesystem::exists(Util::PathHelpers::GetFeatureShaderPath("NeuralNR") / L"nvngx_dlssnr.dll"))
	{
		dllSearchPath = (Util::PathHelpers::GetShadersPath() / L"Upscaling" / L"Streamline").wstring();
	}

	static const wchar_t* searchPaths[] = { dllSearchPath.c_str() };
	
	static NVSDK_NGX_FeatureCommonInfo featureInfo{};
	featureInfo.PathListInfo.Path = searchPaths;
	featureInfo.PathListInfo.Length = 1;

	// Layer 1: Core Init uses confirmed plain Init signature (AppId, Path, Device, FeatureCommonInfo, Version)
	if (s.pfnInitExt)
	{
		NVSDK_NGX_Result initRes = ((PFN_InitExt_Std)s.pfnInitExt)(
			231313132ULL, 
			appPath.c_str(), 
			globals::d3d::device, 
			&featureInfo,
			NVSDK_NGX_Version_API
		);

		if (!NVSDK_NGX_SUCCEED(initRes))
		{ 
			logger::warn("NeuralNR: Core Init failed with result code: 0x{:X}. Proceeding to allocate parameters via Streamline context...", static_cast<uint32_t>(initRes)); 
		}
	}

	if (!s.pfnGetCapabilityParameters)
	{ logger::info("NeuralNR: GetCapabilityParameters export missing from core"); return; }

	NVSDK_NGX_Result capRes = ((PFN_GetCapParams)s.pfnGetCapabilityParameters)(&s.nrParams);

	if (!NVSDK_NGX_SUCCEED(capRes) || !s.nrParams)
	{ logger::info("NeuralNR: GetCapabilityParameters failed"); return; }

	logger::info("NeuralNR: Core context acquired. Snippet initialization deferred to active render cycle.");
}

void NeuralNR::Reset() { GetState().needsReset = true; }

void NeuralNR::DrawSettings()
{
	ImGui::Checkbox("Enable DLSS Neural NR", &settings.enabled);

	if (settings.enabled)
	{
		ImGui::Separator();
		ImGui::SliderFloat("Intensity", &settings.intensity, 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat("Local Tone Strength", &settings.localTone, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Local Structure", &settings.localStructure, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Skin Structure", &settings.skinStructure, 0.0f, 1.0f, "%.2f");
		ImGui::Checkbox("Auto Mask", reinterpret_cast<bool*>(&settings.useAutoMask));
	}
}

void NeuralNR::LoadSettings(json& o_json)
{
	if (o_json.contains("NeuralNR") && !o_json["NeuralNR"].is_null())
	{
		auto& j = o_json["NeuralNR"];
		settings.enabled = j.value("enabled", true);
		settings.intensity = j.value("intensity", 1.0f);
		settings.localTone = j.value("localTone", 0.5f);
		settings.localStructure = j.value("localStructure", 0.5f);
		settings.skinStructure = j.value("skinStructure", 0.5f);
		settings.useAutoMask = j.value("useAutoMask", true);
	}
}

void NeuralNR::SaveSettings(json& o_json)
{
	o_json["NeuralNR"]["enabled"] = settings.enabled;
	o_json["NeuralNR"]["intensity"] = settings.intensity;
	o_json["NeuralNR"]["localTone"] = settings.localTone;
	o_json["NeuralNR"]["localStructure"] = settings.localStructure;
	o_json["NeuralNR"]["skinStructure"] = settings.skinStructure;
	o_json["NeuralNR"]["useAutoMask"] = settings.useAutoMask;
}