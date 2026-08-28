#include "Features/NeuralNR.h"
#include "Features/NeuralNR/CallerSpoof.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "Logger.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
	// CALIBRATE: match the real NVSDK_NGX_D3D11.h signatures.
	using PFN_InitExt         = HRESULT(WINAPI*)(const char*, const wchar_t*, ID3D11Device*, int*, void*);
	using PFN_AllocParams     = NVSDK_NGX_Parameter*(WINAPI*)(NVSDK_NGX_Parameter**);
	using PFN_CreateFeature   = NVSDK_NGX_Result(WINAPI*)(ID3D11DeviceContext*, int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
	using PFN_EvaluateFeature = NVSDK_NGX_Result(WINAPI*)(ID3D11DeviceContext*, NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, void*);
	using PFN_DestroyParams   = void(WINAPI*)(NVSDK_NGX_Parameter**);
}

namespace CSS
{
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
		// CALIBRATE: confirm the deployed data-dir helper.
		const auto path = Globals::DataDir + L"\\Shaders\\NeuralNR\\nvngx_dlssnr.dll";
		s.hDLL = LoadLibraryW(path.c_str());
		if (!s.hDLL) return;
		s.pfnInitExt            = GetProcAddress(s.hDLL, "NVSDK_NGX_D3D11_Init_Ext");
		s.pfnAllocateParameters = GetProcAddress(s.hDLL, "NVSDK_NGX_D3D11_AllocateParameters");
		s.pfnCreateFeature      = GetProcAddress(s.hDLL, "NVSDK_NGX_D3D11_CreateFeature");
		s.pfnEvaluateFeature    = GetProcAddress(s.hDLL, "NVSDK_NGX_D3D11_EvaluateFeature");
		s.pfnDestroyParameters  = GetProcAddress(s.hDLL, "NVSDK_NGX_D3D11_FreeParameters");
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
		if (globals::features::upscaling.GetUpscaleMethod() != UpscaleMethod::kDLSS) return false;
		D3D11_DRIVER_METADATA meta{};
		if (FAILED(dev->CheckFeatureSupport(D3D11_FEATURE_DRIVER_METADATA, &meta, sizeof(meta)))) return false;
		// CALIBRATE: verify bit layout against the value your 616 driver logs.
		const auto major = (meta.DriverVersion >> 16) & 0xFFFF;
		log::info("NeuralNR: driver raw=0x{:08X} major={}", meta.DriverVersion, major);
		return major >= 616;
	}

	bool NeuralNR::CompileShaders()
	{
		auto& s = GetState();
		auto dir = Globals::DataDir + L"\\Shaders\\";
		auto compile = [&](const std::wstring& file, const char* entry, ID3D11ComputeShader** out) {
			std::ifstream f(dir + file, std::ios::binary);
			if (!f) { log::warn("NeuralNR: missing shader {}", file); return false; }
			std::stringstream ss; ss << f.rdbuf();
			auto src = ss.str();
			ID3DBlob* blob = nullptr;
			if (FAILED(D3DCompile(src.c_str(), src.size(), file.c_str(), nullptr,
				D3D_COMPILE_STANDARD_DEBUG_INCLUDES, entry, "cs_6_0", 0, 0, &blob, nullptr)))
			{ log::warn("NeuralNR: D3DCompile failed for {}", file); return false; }
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
		CallerSpoof::Install();
		NVSDK_NGX_Result res = ((PFN_CreateFeature)s.pfnCreateFeature)(
			globals::d3d::context, NVSDK_NGX_Feature_Reserved_DLSSNR, s.nrParams, &s.nrFeature);
		CallerSpoof::Uninstall();
		if (!NVSDK_NGX_SUCCEED(res) || !s.nrFeature)
		{
			log::error("NeuralNR: CreateFeature failed, res=0x{:X}", static_cast<uint32_t>(res));
			return false;
		}
		log::info("NeuralNR: CreateFeature Success.");
		return true;
	}

	void NeuralNR::CreateResources(uint32_t w, uint32_t h, DXGI_FORMAT fmt)
	{
		auto& s = GetState();
		if (s.w == w && s.h == h && s.fmt == fmt) return;
		ReleaseResources();
		s.w = w; s.h = h; s.fmt = fmt;
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

		// MVs — CALIBRATE: confirm the wrapper member for the raw resource.
		if (auto* mv = globals::features::upscaling.motionVectorCopyTexture.get())
		{
			auto* raw = static_cast<ID3D11Texture2D*>(mv->resource.get());
			if (raw)
			{
				mkTex(&s.mvTex, DXGI_FORMAT_R8G8_FLOAT, D3D11_BIND_SHADER_RESOURCE);
				mkSRV(s.mvTex, DXGI_FORMAT_R8G8_FLOAT, &s.mvSRV);
			}
		}

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
					log::info("NeuralNR: Skyrim Depth Format: 0x{:X}", static_cast<uint32_t>(dd.Format));
					D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
					sd.Format = DXGI_FORMAT_R32_FLOAT; // CALIBRATE: override for typeless depth
					sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
					if (FAILED(dev->CreateShaderResourceView(dtex, &sd, &s.depthSRV)))
						log::warn("NeuralNR: depth SRV (R32_FLOAT override) failed");
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
		if (!s.inputColor || !s.nrOutputTex) { back->Release(); return; }
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
		{ log::warn("NeuralNR: EvaluateFeature failed, res=0x{:X}", static_cast<uint32_t>(evalRes)); back->Release(); return; }

		if (hdr) { DispatchTransfer(); ctx->CopyResource(back, s.transferOut); }
		else     { ctx->CopyResource(back, s.nrOutputTex); }
		back->Release();
	}

	void NeuralNR::PostPostLoad()
	{
		auto& s = GetState();
		LoadDLL();
		if (!s.hDLL || !s.pfnCreateFeature || !s.pfnEvaluateFeature)
		{ log::info("NeuralNR: DLL/exports not found — disabled"); return; }
		if (!CheckGate())
		{ log::info("NeuralNR: gate failed — disabled"); return; }
		if (!CompileShaders())
		{ log::info("NeuralNR: shader compile failed"); return; }
		int version = 0;
		// CALIBRATE: confirm Init_Ext signature + return type.
		if (FAILED(((PFN_InitExt)s.pfnInitExt)("css-neuralnr", L"", globals::d3d::device, &version, nullptr)))
		{ log::info("NeuralNR: Init_Ext failed"); return; }
		if (!s.pfnAllocateParameters)
		{ log::info("NeuralNR: AllocateParameters export missing"); return; }
		s.nrParams = ((PFN_AllocParams)s.pfnAllocateParameters)(&s.nrParams);
		if (!s.nrParams)
		{ log::info("NeuralNR: AllocateParameters failed"); return; }
		log::info("NeuralNR: ready to create feature (NGX v{})", version);
	}

	void NeuralNR::Reset() { GetState().needsReset = true; }

	void NeuralNR::DrawSettings()  { /* CALIBRATE: wire CSS ImGui sliders to settings */ }
	void NeuralNR::LoadSettings(json&)  { /* CALIBRATE: read the "NeuralNR" section */ }
	void NeuralNR::SaveSettings(json&)  { /* CALIBRATE: write the "NeuralNR" section */ }
}
