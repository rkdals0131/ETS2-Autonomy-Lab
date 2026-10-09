Texture2D<float> depth_m : register(t0);
StructuredBuffer<float4> beams : register(t1); // unit source-camera direction, maximum range
struct Return { float range; uint status; uint pixel; float source_depth; };
RWStructuredBuffer<Return> returns : register(u0);
cbuffer Parameters : register(b0) {
    row_major float4x4 projection;
    float4 viewport;
    uint4 dimensions; // source width, height, beam count, interpolate inverse depth
    float4 sampling; // maximum neighbor optical-depth ratio
};
[numthreads(128,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if(id.x>=dimensions.z) return;
    float4 beam=beams[id.x];
    Return r;r.range=asfloat(0x7fc00000);r.status=1;r.pixel=0xffffffff;r.source_depth=asfloat(0x7fc00000);
    float4 clip=mul(projection,float4(beam.xyz,0));
    float2 ndc=clip.xy/clip.w;
    float2 uv=viewport.xy+float2(ndc.x+1,1-ndc.y)*viewport.zw*.5;
    if(clip.w>0 && beam.z<0 && all(isfinite(uv)) && all(abs(ndc)<=1) && all(uv>=0) && all(uv<dimensions.xy)) {
        uint2 pixel=(uint2)floor(uv);
        float z=depth_m.Load(int3(pixel,0));
        if(dimensions.w) {
            float2 p=uv-.5;
            int2 i=(int2)floor(p);
            float2 f=p-i;
            // Four pixel centers must belong to this viewport. Reciprocal
            // optical depth is affine over a projected plane; metric Z is not.
            if(all(i>=0) && all(i+1<int2(dimensions.xy)) &&
               all(float2(i)+.5>=viewport.xy) && all(float2(i)+1.5<viewport.xy+viewport.zw)) {
                float4 zs=float4(depth_m.Load(int3(i,0)),depth_m.Load(int3(i+int2(1,0),0)),
                    depth_m.Load(int3(i+int2(0,1),0)),depth_m.Load(int3(i+int2(1,1),0)));
                float lo=min(min(zs.x,zs.y),min(zs.z,zs.w));
                float hi=max(max(zs.x,zs.y),max(zs.z,zs.w));
                if(all(isfinite(zs)) && lo>0 && hi<=lo*sampling.x) {
                    float4 inv=rcp(zs);
                    z=rcp(lerp(lerp(inv.x,inv.y,f.x),lerp(inv.z,inv.w,f.x),f.y));
                }
            }
        }
        float range=z/-beam.z;
        r.pixel=pixel.y*dimensions.x+pixel.x;r.source_depth=z;r.status=2;
        if(isfinite(range) && range>0) {
            r.status=range<=beam.w?0:3;
            if(r.status==0) r.range=range;
        }
    }
    returns[id.x]=r;
}
