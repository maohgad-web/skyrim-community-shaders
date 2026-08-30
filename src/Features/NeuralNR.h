#pragma once

#include <d3d11.h>
#include "ngx/nvsdk_ngx.h"
#include "ngx/nvsdk_ngx_helpers.h"
#include "ngx/nvsdk_ngx_params.h"

#include "Feature.h"
#include <atomic>
#include <string>

struct NeuralNR : public Feature
{
    std::string GetName() override       { return "Neural NR"; }
    std::string GetShortName() override  { return "NeuralNR"; }

    void PostPostLoad() override;
    void DrawSettings() override;
    void LoadSettings(json& o_json) override;
    void SaveSettings(json& o_json) override;
    void Reset() override;

    void OnPresent();
    bool IsEnabled() { return settings.enabled; }

    struct State
    {
        std::atomic<bool> needsReset{true};
        bool streamlineContextCaptured = false; 
        
        HMODULE hSnippetDLL = nullptr;
        
        NVSDK_NGX_Handle* nrFeature = nullptr;
        NVSDK_NGX_Parameter* nrParams = nullptr;
        
        void* pfnCreateFeature = nullptr;
        void* pfnEvaluateFeature = nullptr;

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

private:
    struct Settings
    {
        bool  enabled = true;
        float intensity = 1.0f, style = 0.0f;
        float localTone = 0.5f, localStructure = 0.5f, skinStructure = 0.5f;
        int   useAutoMask = 1, depthInverted = 0, preset = 0;
        float mvScaleX = 1.0f, mvScaleY = 1.0f;
        float paperWhiteNits = 203.0f, encodeStrength = 1.0f;
    } settings;

    bool CreateFeature();
    bool CompileShaders();
    void CreateResources(uint32_t w, uint32_t h, DXGI_FORMAT fmt);
    void ReleaseResources();
    void DispatchProxy();
    void DispatchTransfer();
    ID3D11Resource* ResourceFromView(ID3D11View* view) const;
};