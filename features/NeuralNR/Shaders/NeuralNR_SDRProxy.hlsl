// Forward pass: fold the finished HDR frame to an sRGB SDR proxy.
// Required before NVSDK_NGX_D3D11_EvaluateFeature — the model expects an
// sRGB-encoded signal or it produces "mottled dark clouds".

Texture2D<float4>   HDRInput       : register(t0);
RWTexture2D<float4> SDRProxyOutput : register(u0);

cbuffer NRTuning : register(b0)
{
    float NRPaperWhiteNits;   // 203.0f — paper white for the SDR fold
    float NREncodeStrength;   // 1.0f
    float _pad0;
    float _pad1;
};

float3 SRGBEncode(float3 c)
{
    return c < 0.0031308f ? 12.92f * c
                          : 1.055f * pow(abs(c), 1.0f / 2.4f) - 0.055f;
}

[numthreads(8, 8, 1)]
void CS_GenerateSDRProxy(uint3 DTid : SV_DispatchThreadID)
{
    float4 hdr = HDRInput[DTid.xy];
    float3 sdr = hdr.rgb * (80.0f / max(NRPaperWhiteNits, 1.0f));
    sdr = SRGBEncode(sdr) * NREncodeStrength;
    SDRProxyOutput[DTid.xy] = float4(sdr, hdr.a);
}
