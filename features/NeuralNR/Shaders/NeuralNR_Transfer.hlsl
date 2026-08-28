// Inverse pass: transfer the neural edit (computed on the SDR proxy) back
// onto the HDR frame. MODEL — not the verified RenoDX UpgradeToneMap.
// Reads: original HDR, pre-NR sRGB proxy, post-NR result. Applies the
// per-pixel edit as a clamped gain ratio to the HDR frame.

Texture2D<float4>   HDRInput    : register(t0); // original scRGB/fp16 frame
Texture2D<float4>   SDRProxyIn  : register(t1); // pre-NR sRGB proxy
Texture2D<float4>   SDRProxyOut : register(t2); // post-NR result
RWTexture2D<float4> HDRResult   : register(u0); // final HDR out

static const float MAX_RATIO = 4.0; // safety clamp on the per-pixel gain

float3 SRGBDecode(float3 c)
{
    c = max(c, 0.0);
    return c <= 0.04045f ? c / 12.92f
                         : pow((c + 0.055f) / 1.055f, 2.4f);
}

[numthreads(8, 8, 1)]
void CS_TransferEditToHDR(uint3 DTid : SV_DispatchThreadID)
{
    float4   hdr    = HDRInput[DTid.xy];
    float3   linIn  = SRGBDecode(SDRProxyIn[DTid.xy].rgb);
    float3   linOut = SRGBDecode(SDRProxyOut[DTid.xy].rgb);

    float3   ratio  = clamp(linOut / max(linIn, 1e-5), 0.0, MAX_RATIO);
    HDRResult[DTid.xy] = float4(hdr.rgb * ratio, hdr.a);
}
