#pragma once
#include "Feature.h"
#include "NVSDK_NGX_D3D11.h"
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
			// The gate-compliant caller (the shim) — user-provided. The gated
			// snippet entry points are resolved from here. WE DO NOT PROVIDE IT.
			HMODULE hBackend = nullptr;
			// The NGX core (_nvngx.dll / nvngx.dll) — owns the capability param block.
			HMODULE hCore = nullptr;
			NVSDK_NGX_Handle* nrFeature = nullptr;
			// The core's capability param block (NOT a fresh AllocateParameters).
			NVSDK_NGX_Parameter* nrParams = nullptr;
			void* pfnInitExt = nullptr;
			void* pfnGetCapabilityParams = nullptr;   // core export
			void* pfnPopulateParameters = nullptr;    // snippet, gated (via backend)
			void* pfnCreateFeature = nullptr;         // snippet, gated (via backend)
			void* pfnEvaluateFeature = nullptr;       // snippet, gated (via backend)
			void* pfnReleaseFeature = nullptr;        // snippet, gated (via backend)

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
