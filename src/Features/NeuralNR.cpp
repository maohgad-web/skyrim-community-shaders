#include "Features/NeuralNR.h"
#include "Features/NeuralNR/CallerSpoof.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "Utils/FileSystem.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
	// PATCH: the feature isn't in NVIDIA's real NVSDK_NGX_Feature enum (it's
	// unreleased/reserved), so there's no NVSDK_NGX_Feature_Reserved_DLSSNR to
	// reference — that name never existed in a real header either. Confirmed
	// value: the programming guide states it directly ("Create with feature id
	// 18") and independently via disassembly (mov edx, 0x12 = 18 before the
	// CreateFeature call).
	constexpr int kFeatureDLSSNR = 18;

	// PATCH: these five now come from the real vendor headers (Features/ngx/*.h)
	// via decltype, instead of hand-typed guesses. This pulls whatever signature
	// NVIDIA actually declared, so a mismatch becomes a compile error instead of
	// silent stack corruption at runtime (the guide's own warning on this).
	//
	// IMPORTANT: if any of these five lines fail to compile, that's diagnostic,
	// not a dead end — it means the real header declares that export under a
	// different name than the string LoadDLL() looks up below (e.g. NVIDIA
	// sometimes ships a "_C" suffixed callback variant alongside the plain one).
	// Open the header, search for "D3D11" + the operation name, and use whatever
	// name is actually declared there in both this decltype and the matching
	// GetProcAddress string in LoadDLL().
	//
	// NOTE: NVSDK_NGX_D3D11_Init_Ext only exists in nvsdk_ngx.h under
	// #if defined(NGX_SNIPPET_BUILD) (the NGX Core <-> Snippet signature).
	// We build against the NGX SDK <-> Core side, so the plain
	// NVSDK_NGX_D3D11_Init is the correct export.
	using PFN_InitExt            = decltype(&NVSDK_NGX_D3D11_Init);
	using PFN_AllocParams        = decltype(&NVSDK_NGX_D3D11_AllocateParameters);
	using PFN_CreateFeature      = decltype(&NVSDK_NGX_D3D11_CreateFeature);
	using PFN_EvaluateFeature    = decltype(&NVSDK_NGX_D3D11_EvaluateFeature);
	using PFN_DestroyParams      = decltype(&NVSDK_NGX_D3D11_ReleaseFeature);
}

// NOTE: intentionally NOT wrapped in namespace CSS — NeuralNR is a global-scope
// type (see NeuralNR.h / Globals.h forward decl), matching every other feature.
// The body below keeps its original one-tab indentation from when it was wrapped;
// it is NOT inside a namespace. CallerSpoof stays CSS:: and is qualified below.
ID3D11Resource* NeuralNR::ResourceFromView(ID3D11View* view) const
{
	if (!view) return nullptr;
	ID3D11Resource* res = nullptr;
	view->GetResource(&res);
	if (res) res->Release(); // NGX holds its own lifetime; drop the +1
	return res;
}

void NeuralNR::LoadDLL()
{
	auto& s = GetState();
	const auto path = Util::PathHelpers::GetFeatureShaderPath("NeuralNR") / L"nvngx_dlssnr.dll";
	s.hDLL = LoadLibraryW(path.c_str());
	if (!s.hDLL) return;
	s.pfnInitExt            = GetProcAddress(s.hDLL, "NVSDK_NGX_D3D11_Init");
	s.pfnAllocateParameters = GetProcAddress(s.hDLL, "NVSDK_NGX_D3D11_AllocateParameters");
	s.pfnCreateFeature      = GetProcAddress(s.hDLL, "NVSDK_NGX_D3D11_CreateFeature");
	s.pfnEvaluateFeature    = GetProcAddress(s.hDLL, "NVSDK_NGX_D3D11_EvaluateFeature");
	s.pfnDestroyParameters  = GetProcAddress(s.hDLL, "NVSDK_NGX_D3D11_ReleaseFeature");
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
	if (d.VendorId != 0x10DE) return false; // NVIDIA
	// CALIBRATE: RTX 50 (Blackwell) device-ID range.
	if (!(d.DeviceId >= 0x2B00 && d.DeviceId <= 0x2BFF)) return false;
	// DLSS must be active so the NGX runtime is already alive in-process.
	if (globals::features::upscaling.GetUpscaleMethod() != Upscaling::UpscaleMethod::kDLSS) return false;
	// PATCH: D3D11_DRIVER_METADATA / D3D11_FEATURE_DRIVER_METADATA aren't real
	// D3D11 API symbols (confirmed against Microsoft's documented D3D11_FEATURE
	// enum — no such entry exists). A real driver-version query would need
	// NVAPI, a dependency this project doesn't have. Dropping this is safe:
	// NGX's own Init call already enforces its minimum driver version and
	// fails with FAIL_OutOfDate on its own if the driver's too old.
	return true;
}

bool NeuralNR::CompileShaders()
{
	auto& s = GetState();
	auto dir = Util::PathHelpers::GetShadersPath();
	auto compile = [&](const std::wstring& file, const char* entry, ID3D11ComputeShader** out) {
		std::ifstream f(dir / file, std::ios::binary);
		if (!f) { logger::warn("NeuralNR: missing shader {}", file); return false; }
		std::stringstream ss; ss << f.rdbuf();
		auto src = ss.str();
		ID3DBlob* blob = nullptr;
		if (FAILED(D3DCompile(src.c_str(), src.size(), file.c_str(), nullptr,
			D3D_COMPILE_STANDARD_DEBUG_INCLUDES, entry, "cs_6_0", 0, 0, &blob, nullptr)))
		{ logger::warn("NeuralNR: D3DCompile failed for {}", file); return false; }
		auto hr = globals::d3d::device->CreateComputeShader(
			blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out);
		blob->Release();
		return SUCCEEDED(hr) && *out;
	};
	return compile(L"NeuralNR_SDRProxy.hlsl", "CS_GenerateSDRProxy", &s.proxyCS)
		&& compile(L"NeuralNR_Transfer.hlsl", "CS_TransferEditToHDR", &s.transferCS);
}

bool NeuralNR::CreateFeature()
{
	auto& s = GetState();
	// No-op seam. Your own implementation (if any) drops in here.
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

	// Motion vectors come from Upscaling and may not exist yet the first time
	// this runs (e.g. an early menu frame before Upscaling has produced
	// motionVectorCopyTexture). Factored out so it can be retried on later
	// frames without redoing the rest of resource creation.
	auto tryCreateMotionVectorTexture = [&]() {
		if (s.mvTex) return; // already have it
		if (auto* mv = globals::features::upscaling.motionVectorCopyTexture.get())
		{
			auto* raw = static_cast<ID3D11Texture2D*>(mv->resource.get());
			if (raw)
			{
				mkTex(&s.mvTex, DXGI_FORMAT_R8G8_FLOAT, D3D11_BIND_SHADER_RESOURCE);
				mkSRV(s.mvTex, DXGI_FORMAT_R8G8_FLOAT, &s.mvSRV);
			}
		}
	};

	if (!sizeChanged)
	{
		// Resolution unchanged — everything else already exists. Still retry
		// the motion vector texture if it wasn't available on first creation.
		tryCreateMotionVectorTexture();
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

	// MVs — retry logic above; this is the first attempt.
	tryCreateMotionVectorTexture();

	// Depth SRV — CALIBRATE: format override for typeless depth.
	auto* depthSRV = globals::game::renderer->GetDepthStencilData()
		.depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].textureView.get();
	if (depthSRV)
	{
		ID3D11Resource* dres = nullptr;
		depthSRV->GetResource(&dres);
		if (dres)
		{
			ID3D11Texture2D* dtex = nullptr;
			if (SUCCEEDED(dres->QueryInterface(IID_PPV_ARGS(&dtex))))
			{
				D3D11_TEXTURE2D_DESC dd{}; dtex->GetDesc(&dd);
				logger::info("NeuralNR: Skyrim Depth Format: 0x{:X}", static_cast<uint32_t>(dd.Format));
				D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
				sd.Format = DXGI_FORMAT_R32_FLOAT; // CALIBRATE: override for typeless depth
				sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
				if (FAILED(dev->CreateShaderResourceView(dtex, &sd, &s.depthSRV)))
					logger::warn("NeuralNR: depth SRV (R32_FLOAT override) failed");
				dtex->Release();
			}
			dres->Release();
		}
	}

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
	if (s.pfnDestroyParameters && s.nrParams)
		((PFN_DestroyParams)s.pfnDestroyParameters)(&s.nrParams);
	s.nrParams = nullptr;
	s.nrFeature = nullptr; // CALIBRATE: call DestroyFeature if the header exposes one
}

void NeuralNR::DispatchProxy()
{
	auto& s = GetState(); auto ctx = globals::d3d::context;
	ctx->CSSetShader(s.proxyCS, nullptr, 0);
	ctx->CSSetShaderResources(0, 1, &s.inputSRV);
	ctx->CSSetUnorderedAccessViews(0, 1, &s.sdrProxyUAV);
	ctx->CSSetConstantBuffers(0, 1, &s.tuningCB);
	ctx->Dispatch(s.w / 8, s.h / 8, 1);
	ID3D11ShaderResourceView* n0 = nullptr; ID3D11UnorderedAccessView* n1 = nullptr;
	ctx->CSSetShaderResources(0, 1, &n0);
	ctx->CSSetUnorderedAccessViews(0, 1, &n1);
}

void NeuralNR::DispatchTransfer()
{
	auto& s = GetState(); auto ctx = globals::d3d::context;
	ctx->CSSetShader(s.transferCS, nullptr, 0);
	ID3D11ShaderResourceView* srcs[3] = { s.inputSRV, s.sdrProxySRV, s.nrOutputSRV };
	ctx->CSSetShaderResources(0, 3, srcs);
	ctx->CSSetUnorderedAccessViews(0, 1, &s.transferOutUAV);
	ctx->Dispatch(s.w / 8, s.h / 8, 1);
	ID3D11ShaderResourceView* n0 = nullptr; ID3D11UnorderedAccessView* n1 = nullptr;
	ctx->CSSetShaderResources(0, 3, &n0);
	ctx->CSSetUnorderedAccessViews(0, 1, &n1);
}

void NeuralNR::OnPresent()
{
	auto& s = GetState();
	if (!s.initialized)
	{
		if (!s.nrFeature && CreateFeature()) s.initialized = true;
		if (!s.initialized) return;
	}

	auto ctx = globals::d3d::context;
	ID3D11Texture2D* back = nullptr;
	if (FAILED(globals::d3d::swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
	D3D11_TEXTURE2D_DESC dsc{}; back->GetDesc(&dsc);
	const uint32_t w = dsc.Width, h = dsc.Height;
	if (s.w != w || s.h != h) s.needsReset = true;
	CreateResources(w, h, dsc.Format);
	// Bail out this frame (and retry next frame via CreateResources) rather
	// than handing NGX a null MVec resource, which is one of the four
	// resources this feature requires unconditionally.
	if (!s.inputColor || !s.nrOutputTex || !s.mvTex) { back->Release(); return; }
	const bool hdr = (dsc.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
		|| (dsc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);

	ctx->CopyResource(s.inputColor, back);
	if (hdr) DispatchProxy();

	auto* P = s.nrParams;
	P->Set(NVSDK_NGX_Parameter_Width,  (int)w);
	P->Set(NVSDK_NGX_Parameter_Height, (int)h);
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
	P->Set("DLSSNR.MVec",   (ID3D11Resource*)s.mvTex);
	P->Set("DLSSNR.Depth",  ResourceFromView(s.depthSRV));

	auto SetSubrect = [&](const char* name) {
		P->Set((std::string(name) + "SubrectBaseX").c_str(), 0);
		P->Set((std::string(name) + "SubrectBaseY").c_str(), 0);
		P->Set((std::string(name) + "SubrectWidth").c_str(),  (int)w);
		P->Set((std::string(name) + "SubrectHeight").c_str(), (int)h);
	};
	SetSubrect("DLSSNR.Color"); SetSubrect("DLSSNR.MVec");
	SetSubrect("DLSSNR.Depth"); SetSubrect("DLSSNR.Output");

	NVSDK_NGX_Result evalRes = ((PFN_EvaluateFeature)s.pfnEvaluateFeature)(
		ctx, s.nrFeature, P, nullptr);
	if (!NVSDK_NGX_SUCCEED(evalRes))
	{ logger::warn("NeuralNR: EvaluateFeature failed, res=0x{:X}", static_cast<uint32_t>(evalRes)); back->Release(); return; }

	if (hdr) { DispatchTransfer(); ctx->CopyResource(back, s.transferOut); }
	else     { ctx->CopyResource(back, s.nrOutputTex); }
	back->Release();
}

void NeuralNR::PostPostLoad()
{
	auto& s = GetState();
	LoadDLL();
	if (!s.hDLL || !s.pfnCreateFeature || !s.pfnEvaluateFeature)
	{ logger::info("NeuralNR: DLL/exports not found — disabled"); return; }
	if (!CheckGate())
	{ logger::info("NeuralNR: gate failed — disabled"); return; }
	if (!CompileShaders())
	{ logger::info("NeuralNR: shader compile failed"); return; }
	
	if (!NVSDK_NGX_SUCCEED(((PFN_InitExt)s.pfnInitExt)(0x1337ULL, L"", globals::d3d::device, nullptr, NVSDK_NGX_Version_API)))
	{ logger::info("NeuralNR: Init_Ext failed"); return; }
	
	if (!s.pfnAllocateParameters)
	{ logger::info("NeuralNR: AllocateParameters export missing"); return; }
	s.nrParams = ((PFN_AllocParams)s.pfnAllocateParameters)(&s.nrParams);
	if (!s.nrParams)
	{ logger::info("NeuralNR: AllocateParameters failed"); return; }
	logger::info("NeuralNR: ready to create feature");
}

void NeuralNR::Reset() { GetState().needsReset = true; }

void NeuralNR::DrawSettings()  { /* CALIBRATE: wire CSS ImGui sliders to settings */ }
void NeuralNR::LoadSettings(json&)  { /* CALIBRATE: read the "NeuralNR" section */ }
void NeuralNR::SaveSettings(json&)  { /* CALIBRATE: write the "NeuralNR" section */ }