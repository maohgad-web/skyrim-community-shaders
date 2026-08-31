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
#include <imgui.h>

namespace
{
	constexpr int kFeatureDLSSNR = 18;

	using PFN_CreateFeature   = NVSDK_NGX_Result (*)(ID3D11DeviceContext*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
	using PFN_EvaluateFeature = NVSDK_NGX_Result (*)(ID3D11DeviceContext*, NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, void*);

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

	// PATCH: isolated into its own function with only POD/pointer parameters
	// so __try/__except is unambiguously valid here — OnPresent itself
	// constructs std::string temporaries (in SetSubrect), and mixing __try
	// with C++ objects needing destructors in the same function is the same
	// MSVC restriction already respected throughout CallerSpoof.cpp. Same
	// rationale as CreateFeature's guard above: P may be a borrowed, foreign
	// parameter block rather than one this code fully controls.
	bool SafeEvaluateFeature(void* pfnEvaluateFeature, ID3D11DeviceContext* ctx, NVSDK_NGX_Handle* feature, NVSDK_NGX_Parameter* params, NVSDK_NGX_Result* outResult)
	{
		__try
		{
			*outResult = ((PFN_EvaluateFeature)pfnEvaluateFeature)(ctx, feature, params, nullptr);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// PATCH: isolated so __try/__except is valid here -- CompileShaders'
	// own compile lambda has std::ifstream/stringstream/string objects in
	// its scope, which can't coexist with __try in the same function under
	// MSVC (same constraint already applied throughout CallerSpoof.cpp).
	// Defense-in-depth: the null-device check in CompileShaders should
	// prevent this from ever faulting, but nothing upstream (Feature.h's
	// ForEachLoadedFeature loop has zero exception handling) catches a
	// fault here if some other unexpected state causes one.
	bool SafeCreateComputeShader(ID3DBlob* blob, ID3D11ComputeShader** out)
	{
		__try
		{
			HRESULT hr = globals::d3d::device->CreateComputeShader(
				blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out);
			return SUCCEEDED(hr) && *out;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// PATCH: closes the one remaining unguarded call in CompileShaders --
	// D3DCompile itself doesn't need the D3D device (it's a standalone
	// HLSL-to-bytecode compiler), so it wasn't a suspect under the
	// null-device hypothesis, but leaving it unguarded meant a fault here
	// specifically would still produce zero log output, undermining the
	// "CompileShaders always logs something" guarantee this diagnostic
	// relies on. Isolated for the same __try/C2712 reason as the others.
	bool SafeD3DCompile(const char* src, size_t srcSize, const char* fileName, const char* entryPoint, ID3DBlob** outBlob, ID3DBlob** outErrorBlob)
	{
		__try
		{
			return SUCCEEDED(D3DCompile(src, srcSize, fileName, nullptr,
				D3D_COMPILE_STANDARD_FILE_INCLUDE, entryPoint, "cs_5_0", 0, 0, outBlob, outErrorBlob));
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// PATCH: isolated so __try/__except is valid here, shared by both
	// candidate parameter sources CreateFeature now tries in sequence.
	bool SafeCreateFeatureCall(void* pfnCreateFeature, ID3D11DeviceContext* ctx, NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** outHandle, NVSDK_NGX_Result* outResult)
	{
		__try
		{
			*outResult = ((PFN_CreateFeature)pfnCreateFeature)(ctx, static_cast<NVSDK_NGX_Feature>(kFeatureDLSSNR), params, outHandle);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	using PFN_GetCapabilityParameters = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);

	// PATCH: isolated so __try/__except is valid here, matching every other
	// Safe* helper in this file.
	bool SafeGetCapabilityParameters(void* pfnGetCapParams, NVSDK_NGX_Parameter** outParams, NVSDK_NGX_Result* outResult)
	{
		__try
		{
			*outResult = ((PFN_GetCapabilityParameters)pfnGetCapParams)(outParams);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// PATCH: actively calls GetCapabilityParameters ourselves, resolved
	// directly from _nvngx.dll, instead of passively waiting to intercept
	// someone else's call to it. Confirmed via the per-target import scan
	// that no tracked module statically imports this name, and confirmed
	// via the log that nobody dynamically resolves it through GetProcAddress
	// either -- Streamline evidently manages its own parameters through an
	// internal layer that never touches this raw NGX entry point, so
	// passive interception was never going to catch it. The instance check
	// already confirmed our own loaded snippet is the same live core
	// instance, so calling this ourselves through that same confirmed-real
	// module should be valid. Uses GetModuleHandleW (find already-loaded),
	// never LoadLibraryW -- the same lesson from the earlier LoadDLL
	// timing investigation: the real core only becomes findable once
	// Streamline has loaded it via its own driver-store-aware mechanism.
	void TryCaptureCapabilityParametersDirectly()
	{
		auto& s = NeuralNR::GetState();
		if (s.capabilityParamsCaptured) return;

		HMODULE hCore = GetModuleHandleW(L"_nvngx.dll");
		if (!hCore) return;

		auto pfnGetCapParams = GetProcAddress(hCore, "NVSDK_NGX_D3D11_GetCapabilityParameters");
		if (!pfnGetCapParams) return;

		CSS::CallerSpoof::Install();
		NVSDK_NGX_Parameter* outParams = nullptr;
		NVSDK_NGX_Result res = static_cast<NVSDK_NGX_Result>(0xDEADBEEF);
		bool survived = SafeGetCapabilityParameters((void*)pfnGetCapParams, &outParams, &res);
		CSS::CallerSpoof::Uninstall();

		if (!survived)
		{
			logger::warn("NeuralNR: SEH caught access violation calling GetCapabilityParameters directly.");
			return;
		}

		if (!NVSDK_NGX_SUCCEED(res) || !outParams)
		{
			static uint32_t s_failLogCounter = 0;
			if (++s_failLogCounter % 300 == 1)
				logger::warn("NeuralNR: Direct GetCapabilityParameters call failed, res=0x{:X}", static_cast<uint32_t>(res));
			return;
		}

		logger::info("NeuralNR: Direct GetCapabilityParameters call succeeded: {}", (void*)outParams);
		s.capabilityParams = outParams;
		s.capabilityParamsCaptured = true;
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

bool NeuralNR::CompileShaders()
{
	auto& s = GetState();

	// PATCH: kPostPostLoad (this function's only caller, via PostPostLoad)
	// fires before the game has rendered a single frame -- globals::d3d::
	// device is only populated later, once the first real Present call is
	// intercepted. Dereferencing it here when still null is a structured
	// exception, not a C++ one, and Feature.h's ForEachLoadedFeature loop
	// has zero exception handling around its callback -- nothing upstream
	// would catch it. Return cleanly so the caller can retry later (see
	// OnPresent) once the device actually exists, instead of faulting.
	if (!globals::d3d::device)
	{
		logger::info("NeuralNR: CompileShaders deferred -- D3D device not yet available.");
		return false;
	}

	auto dir = Util::PathHelpers::GetShadersPath();

	auto compile = [&](const char* file, const char* entry, ID3D11ComputeShader** out) {
		std::ifstream f(dir / file, std::ios::binary);
		if (!f) { logger::warn("NeuralNR: missing shader {}", file); return false; }
		
		std::stringstream ss; ss << f.rdbuf();
		auto src = ss.str();
		
		ID3DBlob* blob = nullptr;
		ID3DBlob* errorBlob = nullptr;
		
		if (!SafeD3DCompile(src.c_str(), src.size(), file, entry, &blob, &errorBlob))
		{ 
			logger::warn("NeuralNR: D3DCompile failed or faulted for {}", file);
			if (errorBlob) errorBlob->Release();
			return false; 
		}
		
		bool ok = SafeCreateComputeShader(blob, out);
		if (!ok)
			logger::warn("NeuralNR: CreateComputeShader failed or faulted for {}", file);

		blob->Release();
		return ok;
	};

	return compile("NeuralNR_SDRProxy.hlsl", "CS_GenerateSDRProxy", &s.proxyCS)
		&& compile("NeuralNR_Transfer.hlsl", "CS_TransferEditToHDR", &s.transferCS);
}

bool NeuralNR::CreateFeature()
{
	auto& s = GetState();
	if (!s.pfnCreateFeature) return false;

	// PATCH: wide-net cascade across two independent candidate sources,
	// rather than assuming one is correct. s.nrParams (confirmed Feature
	// 18, or the SuperSampling wide-net fallback) may be a block already
	// specialized for a DIFFERENT feature by the time we borrow it -- the
	// guide's own Section 4 describes CreateFeature expecting the core's
	// generic GetCapabilityParameters block instead, which is now captured
	// separately as s.capabilityParams and tried second. Worth remembering
	// this project is D3D11 with a Streamline-hook architecture that
	// matches neither the guide's own Vulkan verification nor the
	// D3D12/Streamline third-party report it credits -- "success" here may
	// look different from either, so testing both sources on their own
	// terms is deliberate, not a placeholder until we pick the "right" one.
	auto attempt = [&](NVSDK_NGX_Parameter* params, const char* label) -> bool {
		if (!params) return false;

		CSS::CallerSpoof::Install();
		NVSDK_NGX_Result res = static_cast<NVSDK_NGX_Result>(0xDEADBEEF);
		bool survived = SafeCreateFeatureCall(s.pfnCreateFeature, globals::d3d::context, params, &s.nrFeature, &res);
		CSS::CallerSpoof::Uninstall();

		if (!survived)
		{
			logger::error("NeuralNR: SEH caught access violation inside CreateFeature (source={}).", label);
			return false;
		}
		if (!NVSDK_NGX_SUCCEED(res) || !s.nrFeature)
		{
			logger::error("NeuralNR: Snippet CreateFeature failed, res=0x{:X} (source={})", static_cast<uint32_t>(res), label);
			return false;
		}
		logger::info("NeuralNR: Snippet CreateFeature Success. Tensor context established. (source={})", label);
		return true;
	};

	if (attempt(s.nrParams, s.paramsAreBorrowed ? "SR-borrowed" : "confirmed-Feature18"))
		return true;

	// PATCH: third candidate -- a second, distinct-caller block, captured
	// only if a genuinely different module than the one behind s.nrParams
	// also called CreateFeature for NR or SR (see Hooked_NGXCreate). Tests
	// "what if two modules both called, and we locked onto the wrong one
	// first" directly, rather than only observing it in the log.
	if (attempt(s.nrParamsAlt, s.nrParamsAltBorrowed ? "alt-caller-SR-borrowed" : "alt-caller-confirmed-Feature18"))
		return true;

	return attempt(s.capabilityParams, "core-GetCapabilityParameters");
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

	// 1. Dynamic Interceptor Poll: continuously ensures IAT hooks are attached
	CSS::CallerSpoof::InstallActiveInterceptors();

	// PATCH: lazy retry for shader compilation deferred in PostPostLoad
	// (D3D device wasn't available that early). By the time OnPresent runs
	// at all, a real Present call has occurred, so the device is
	// guaranteed valid here -- safe to retry unconditionally until it
	// succeeds once.
	if (!s.proxyCS || !s.transferCS)
		CompileShaders();

	// PATCH: actively try capturing GetCapabilityParameters ourselves each
	// frame until it succeeds once -- see TryCaptureCapabilityParametersDirectly
	// for why passive interception was abandoned for this specific source.
	if (!s.capabilityParamsCaptured)
		TryCaptureCapabilityParametersDirectly();

	// 2. Await parameter capture from active DLSS execution (or after toggling DLSS in menu)
	if (!s.streamlineContextCaptured || !s.nrParams || !s.pfnEvaluateFeature || !s.pfnCreateFeature) 
	{
		// PATCH: was a single generic "waiting" message — now shows exactly
		// which of the four gate conditions is still unmet, readable
		// directly from this log without cross-referencing CallerSpoof's
		// separate log stream.
		static uint32_t s_waitCounter = 0;
		if (++s_waitCounter % 300 == 1)
			logger::info("NeuralNR: Waiting — streamlineContextCaptured={}, nrParams={}, pfnEvaluateFeature={}, pfnCreateFeature={}, paramsAreBorrowed={}",
				s.streamlineContextCaptured, (void*)s.nrParams, s.pfnEvaluateFeature != nullptr, s.pfnCreateFeature != nullptr, s.paramsAreBorrowed);
		return; 
	}

	// PATCH: fires once, the moment we have SOMETHING to work with,
	// regardless of whether CreateFeature ever succeeds afterward -- this
	// was previously placed after a successful EvaluateFeature call, which
	// meant it never fired at all given CreateFeature has never once
	// succeeded. Confirms whether our own directly-loaded snippet DLL
	// instance matches Streamline's own loaded copy: if they match, a
	// handle Streamline created against its copy is safe to hand to our
	// resolved function pointers (same module, same internal state either
	// way); if they differ, that's a real, separate problem worth knowing
	// about independent of which parameter source ends up working.
	static bool s_loggedInstanceCheck = false;
	if (!s_loggedInstanceCheck)
	{
		HMODULE hStreamlineCopy = GetModuleHandleW(L"nvngx_dlssnr.dll");
		logger::info("NeuralNR: Instance check — ours={}, Streamline's={}, match={}",
			(void*)s.hSnippetDLL, (void*)hStreamlineCopy, (hStreamlineCopy == s.hSnippetDLL));
		s_loggedInstanceCheck = true;
	}

	auto ctx = globals::d3d::context;
	ID3D11Texture2D* back = nullptr;
	if (FAILED(globals::d3d::swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
	D3D11_TEXTURE2D_DESC dsc{}; back->GetDesc(&dsc);
	const uint32_t w = dsc.Width, h = dsc.Height;
	if (s.w != w || s.h != h) s.needsReset = true;
	CreateResources(w, h, dsc.Format);

	// 3. 3D Scene Gate
	if (!s.mvSRV || !s.depthSRV) 
	{
		static uint32_t s_resourceWaitCounter = 0;
		if (++s_resourceWaitCounter % 300 == 1)
			logger::info("NeuralNR: Parameter context acquired! Waiting for 3D depth and motion vectors...");
		back->Release(); 
		return; 
	}

	// 4. Feature Creation on DLSS-NR snippet using captured parameters
	if (!s.nrFeature) {
		static uint32_t s_createRetry = 0;
		if (s_createRetry++ % 60 == 0) {
			logger::info("NeuralNR: Initializing Neural Rendering feature context...");
			if (!CreateFeature()) {
				logger::warn("NeuralNR: Snippet CreateFeature rejected. Retrying...");
				back->Release();
				return;
			}
		} else {
			back->Release();
			return;
		}
	}

	auto* P = s.nrParams;
	
	P->Set("DLSSNR.Width",  static_cast<unsigned int>(w));
	P->Set("DLSSNR.Height", static_cast<unsigned int>(h));
	P->Set(NVSDK_NGX_Parameter_Width,  static_cast<unsigned int>(w));
	P->Set(NVSDK_NGX_Parameter_Height, static_cast<unsigned int>(h));
	P->Set(NVSDK_NGX_Parameter_OutWidth,  static_cast<unsigned int>(w));
	P->Set(NVSDK_NGX_Parameter_OutHeight, static_cast<unsigned int>(h));

	auto SetSubrect = [&](const char* name) {
		P->Set((std::string("DLSSNR.") + name + "SubrectBaseX").c_str(), 0u);
		P->Set((std::string("DLSSNR.") + name + "SubrectBaseY").c_str(), 0u);
		P->Set((std::string("DLSSNR.") + name + "SubrectWidth").c_str(),  static_cast<unsigned int>(w));
		P->Set((std::string("DLSSNR.") + name + "SubrectHeight").c_str(), static_cast<unsigned int>(h));
	};
	SetSubrect("Color"); 
	SetSubrect("MVec");
	SetSubrect("Depth"); 
	SetSubrect("Output");

	const bool hdr = IsActuallyHDROutput(globals::d3d::swapChain);
	ctx->CopyResource(s.inputColor, back);
	if (hdr) DispatchProxy();

	P->Set("DLSSNR.Hint.Render.Preset", static_cast<unsigned int>(settings.preset));
	P->Set("DLSSNR.Enabled", 1u);
	if (s.needsReset.exchange(false)) {
		P->Set("DLSSNR.Reset", 1u);
		P->Set("Reset", 1u);
	}
	
	P->Set("DLSSNR.Style",                 settings.style);
	P->Set("DLSSNR.Intensity",             settings.intensity);
	P->Set("DLSSNR.LocalToneStrength",     settings.localTone);
	P->Set("DLSSNR.LocalStructureStrength",settings.localStructure);
	P->Set("DLSSNR.SkinStructureStrength", settings.skinStructure);
	P->Set("DLSSNR.UseAutoMask",           static_cast<unsigned int>(settings.useAutoMask));
	P->Set("DLSSNR.MVecScaleX",            settings.mvScaleX);
	P->Set("DLSSNR.MVecScaleY",            settings.mvScaleY);
	P->Set("DLSSNR.DepthInverted",         static_cast<unsigned int>(settings.depthInverted));
	P->Set("NRPaperWhiteNits",             settings.paperWhiteNits);
	P->Set("NREncodeStrength",             settings.encodeStrength);

	auto colorRes = (ID3D11Resource*)(hdr ? s.sdrProxyTex : s.inputColor);
	auto outRes   = (ID3D11Resource*)s.nrOutputTex;
	auto mvecRes  = ResourceFromView(s.mvSRV);
	auto depthRes = ResourceFromView(s.depthSRV);

	P->Set("DLSSNR.Color",  colorRes);
	P->Set("DLSSNR.Output", outRes);
	P->Set("DLSSNR.MVec",   mvecRes);
	P->Set("DLSSNR.Depth",  depthRes);

	P->Set("Color",         colorRes);
	P->Set("Output",        outRes);
	P->Set("MotionVectors", mvecRes);
	P->Set("Depth",         depthRes);
	P->Set("Jitter.Offset.X", 0.0f);
	P->Set("Jitter.Offset.Y", 0.0f);
	P->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
	P->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
	P->Set("MV.Scale.X",    settings.mvScaleX);
	P->Set("MV.Scale.Y",    settings.mvScaleY);
	P->Set("Depth.Inverted",static_cast<unsigned int>(settings.depthInverted));

	// 5. Snippet Execution
	CSS::CallerSpoof::Install();
	NVSDK_NGX_Result evalRes = static_cast<NVSDK_NGX_Result>(0xDEADBEEF);
	if (!SafeEvaluateFeature(s.pfnEvaluateFeature, ctx, s.nrFeature, P, &evalRes))
	{
		logger::error("NeuralNR: SEH caught access violation calling EvaluateFeature — likely incompatible with the borrowed parameter block.");
	}
	CSS::CallerSpoof::Uninstall();

	static bool s_loggedEval = false;
	if (NVSDK_NGX_SUCCEED(evalRes) && !s_loggedEval)
	{
		logger::info("NeuralNR: Neural Rendering frame evaluated successfully! Format=0x{:X} (paramsAreBorrowed={})", static_cast<uint32_t>(dsc.Format), s.paramsAreBorrowed);
		s_loggedEval = true;
	}
	else if (!NVSDK_NGX_SUCCEED(evalRes))
	{
		static NVSDK_NGX_Result s_lastLoggedFailure = static_cast<NVSDK_NGX_Result>(-1);
		static uint32_t s_failureLogFrameCounter = 0;
		if (evalRes != s_lastLoggedFailure || ++s_failureLogFrameCounter >= 300)
		{
			logger::warn("NeuralNR: EvaluateFeature failed, res=0x{:X} (paramsAreBorrowed={})", static_cast<uint32_t>(evalRes), s.paramsAreBorrowed);
			s_lastLoggedFailure = evalRes;
			s_failureLogFrameCounter = 0;
		}
	}

	if (hdr) { DispatchTransfer(); ctx->CopyResource(back, s.transferOut); }
	else     { ctx->CopyResource(back, s.nrOutputTex); }
	back->Release();
}

void NeuralNR::PostPostLoad()
{
	// Diagnostic — confirms whether this function is even being invoked by
	// SKSE's messaging system at all, independent of anything downstream.
	// Placed before GetState()/CompileShaders() so nothing in this
	// function's own logic can swallow it before it fires.
	logger::info("NeuralNR: PostPostLoad entered.");

	auto& s = GetState();

	// PATCH: CompileShaders() needs the D3D device, which this early in
	// the lifecycle may not exist yet (see CompileShaders' own guard).
	// Previously a failure here returned out of PostPostLoad entirely,
	// blocking the snippet load and the Detours ambush below -- neither of
	// which depends on the device at all, and the ambush specifically
	// benefits from installing as early as possible (it's what lets it
	// catch Streamline's own resolution before Streamline itself boots).
	// Attempt shader compilation, but don't let a deferred failure here
	// block anything else -- it retries lazily from OnPresent() once the
	// device exists.
	if (!CompileShaders())
		logger::info("NeuralNR: shader compile deferred or failed; will retry from OnPresent.");

	std::wstring snippetPath = Util::PathHelpers::GetFeatureShaderPath("NeuralNR") / L"nvngx_dlssnr.dll";
	if (!std::filesystem::exists(snippetPath)) {
		snippetPath = Util::PathHelpers::GetShadersPath() / L"Upscaling" / L"Streamline" / L"nvngx_dlssnr.dll";
	}
	
	s.hSnippetDLL = LoadLibraryW(snippetPath.c_str());
	if (s.hSnippetDLL) {
		s.pfnCreateFeature = GetProcAddress(s.hSnippetDLL, "NVSDK_NGX_D3D11_CreateFeature");
		s.pfnEvaluateFeature = GetProcAddress(s.hSnippetDLL, "NVSDK_NGX_D3D11_EvaluateFeature");
		logger::info("NeuralNR: Target snippet loaded. Exports resolved.");
	} else {
		// PATCH: no longer returns here -- the Detours ambush below is
		// fully independent of whether our own direct snippet load
		// succeeded, so an unrelated failure here shouldn't block it.
		logger::warn("NeuralNR: Target snippet nvngx_dlssnr.dll could not be loaded.");
	}

	CSS::CallerSpoof::InstallUpscalerHooks();
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