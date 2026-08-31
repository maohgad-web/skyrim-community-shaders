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
        // PATCH: set by CallerSpoof.cpp's Hooked_NGXCreate specifically when
        // the SuperSampling (Feature 1) fallback fires instead of confirmed
        // Feature 18 -- lets every downstream log line here state which
        // source actually supplied nrParams, instead of needing to
        // cross-reference CallerSpoof's separate log stream by timestamp.
        bool paramsAreBorrowed = false;
        
        HMODULE hSnippetDLL = nullptr;
        
        NVSDK_NGX_Handle* nrFeature = nullptr;
        NVSDK_NGX_Parameter* nrParams = nullptr;
        // PATCH: a SEPARATE candidate source for CreateFeature, captured
        // from the core's own GetCapabilityParameters call -- a generic,
        // feature-agnostic block, distinct from nrParams (which may be
        // SuperSampling's own already-specialized block when borrowed).
        // Never overwrites nrParams; CreateFeature tries both in sequence.
        NVSDK_NGX_Parameter* capabilityParams = nullptr;
        bool capabilityParamsCaptured = false;
        // PATCH: second, independent candidate -- captured only when a
        // DIFFERENT module than the one that supplied nrParams calls
        // CreateFeature for NR or SR. Directly tests "what if two modules
        // both call, and we locked onto the wrong one first" instead of
        // just observing the second caller in the log without ever trying
        // its data. Never overwrites nrParams.
        NVSDK_NGX_Parameter* nrParamsAlt = nullptr;
        bool nrParamsAltBorrowed = false;
        std::string nrParamsCallerModule;
        
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