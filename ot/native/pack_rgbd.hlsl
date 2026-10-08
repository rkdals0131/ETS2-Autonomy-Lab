// The inputs are private GPU copies: packing never rebinds game render targets.
Texture2D<float4> source : register(t0);
Texture2D<float4> attributes : register(t1);
Texture2D<uint4> material : register(t2);
cbuffer Parameters : register(b0) {
    float4 controls; // x: common linear RGB gain
    float4 viewport; // x, y, width, height
    float4 depth_range; // min, max
    row_major float4x4 inverse_projection;
};
#ifdef PACK_DEPTH
RWTexture2D<float> result : register(u0);
#else
RWTexture2D<float4> result : register(u0);
#endif
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    result.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    int3 pixel = int3(id.xy, 0);
#ifdef PACK_DEPTH
    float depth = source.Load(pixel).x;
    float z = attributes.Load(pixel).w;
    uint4 flags = material.Load(pixel);
    uint bits = ((flags.w >> 13) & 7) | ((flags.z & 3) << 3);
    float2 uv = (float2(id.xy) + .5 - viewport.xy) / viewport.zw;
    bool valid = isfinite(z) && z != 0 && !(bits & 16) &&
        isfinite(depth) && depth >= depth_range.x && depth <= depth_range.y &&
        all(uv >= 0) && all(uv < 1);
    if (controls.y != 0) {
        float z_ndc = (depth-depth_range.x)/(depth_range.y-depth_range.x);
        float4 eye = mul(inverse_projection, float4(2*uv.x-1, 1-2*uv.y, z_ndc, 1));
        depth = -eye.z/eye.w;
        valid = valid && isfinite(depth) && depth > 0 && eye.w != 0;
    }
    result[id.xy] = valid ? depth : asfloat(0x7fc00000);
#else
    float3 rgb = source.Load(pixel).rgb;
    rgb = float3(isfinite(rgb.x) ? max(rgb.x, 0) : 0,
                 isfinite(rgb.y) ? max(rgb.y, 0) : 0,
                 isfinite(rgb.z) ? max(rgb.z, 0) : 0) * controls.x;
    rgb = saturate(1 - rcp(1 + rgb));
    float3 srgb = float3(rgb.x <= .0031308 ? 12.92*rgb.x : 1.055*pow(rgb.x, 1.0/2.4)-.055,
                        rgb.y <= .0031308 ? 12.92*rgb.y : 1.055*pow(rgb.y, 1.0/2.4)-.055,
                        rgb.z <= .0031308 ? 12.92*rgb.z : 1.055*pow(rgb.z, 1.0/2.4)-.055);
    result[id.xy] = float4(saturate(srgb), 1);
#endif
}
