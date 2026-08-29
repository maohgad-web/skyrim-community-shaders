#pragma once
#include "Feature.h"

// PATCH: "NVSDK_NGX_D3D11.h" never existed as a real NVIDIA-shipped file — NVIDIA
// splits D3D11 support across these three real headers instead, gated behind
// <d3d11.h> having already been included (nvsdk_ngx.h checks for __d3d11_h__).
// Pull these three from your extern/Streamline-DX12 submodule's ngx-sdk/include
// folder and drop them here as Features/ngx/*.h.
#include <d3d11.h>
#include "ngx/nvsdk_ngx.h"
#include "ngx/nvsdk_ngx_helpers.h"
#include "ngx/nvsdk_ngx_params.h"

#include <atomic>
#include <string>

namespace CSS
{
	struct NeuralNR : public Feature
	{
		// Pure virtuals — both return std::string, no const (matches Feature base).
		std::string GetName() override       { return "Neural NR"; }
		std::string GetShortName() override  { return "NeuralNR"; }

		void PostPostLoad() override;
		void DrawSettings() override;
		void LoadSettings(json& o_json) override;
		void SaveSettings(json& o_json) override;
		void Reset() override;

		static void OnPresent();
		static bool IsEnabled() { return GetState().initialized; }

	private:
		struct Settings
		{
			float intensity = 1.0f, style = 0.0f;
			float localTone = 0.5f, localStructure = 0.5f, skinStructure = 0.5f;
			int   useAutoMask = 1, depthInverted = 0, preset = 0;
			float mvScaleX = 1.0f, mvScaleY = 1.0f;
			float paperWhiteNits = 203.0f, encodeStrength = 1.0f;
		} settings;

		struct State
		{
			bool initialized = false;
			std::atomic<bool> needsReset{true};
			HMODULE hDLL = nullptr;
			NVSDK_NGX_Handle* nrFeature = nullptr;
			NVSDK_NGX_Parameter* nrParams = nullptr;
			void* pfnInitExt = nullptr;
			void* pfnAllocateParameters = nullptr;
			void* pfnCreateFeature = nullptr;
			void* pfnEvaluateFeature = nullptr;
			void* pfnDestroyParameters = nullptr;

			ID3D11Texture2D* inputColor = nullptr;
			ID3D11Texture2D* sdrProxyTex = nullptr;
			ID3D11Texture2D* nrOutputTex = nullptr;
			ID3D11Texture2D* transferOut = nullptr;
			ID3D11Texture2D* mvTex = nullptr;
			ID3D11ShaderResourceView* inputSRV = nullptr;
			ID3D11ShaderResourceView* sdrProxySRV = nullptr;
			ID3D11UnorderedAccessView* sdrProxyUAV = nullptr;
			ID3D11ShaderResourceView* nrOutputSRV = nullptr;
			ID3D11UnorderedAccessView* nrOutputUAV = nullptr;
			ID3D11UnorderedAccessView* transferOutUAV = nullptr;
			ID3D11ShaderResourceView* mvSRV = nullptr;
			ID3D11ShaderResourceView* depthSRV = nullptr;
			ID3D11ComputeShader* proxyCS = nullptr;
			ID3D11ComputeShader* transferCS = nullptr;
			ID3D11Buffer* tuningCB = nullptr;
			uint32_t w = 0, h = 0;
			DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
		};

		static State& GetState() { static State s; return s; }

		void LoadDLL();
		bool CheckGate();
		bool CreateFeature();
		bool CompileShaders();
		void CreateResources(uint32_t w, uint32_t h, DXGI_FORMAT fmt);
		void ReleaseResources();
		void ReleaseFeature();
		void DispatchProxy();
		void DispatchTransfer();
		ID3D11Resource* ResourceFromView(ID3D11View* view) const;
	};
}