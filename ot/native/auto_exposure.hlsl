Texture2D<float4> source : register(t0);
RWStructuredBuffer<float4> exposure : register(u0);
cbuffer Parameters : register(b0) { float elapsed; float3 unused; };
groupshared float luminance[256];
[numthreads(16,16,1)]
void main(uint3 tid : SV_GroupThreadID, uint index : SV_GroupIndex) {
    uint width,height;source.GetDimensions(width,height);
    float sum=0;
    // 1,024 spatially distributed samples; no full-size luminance buffer.
    for(uint n=0;n<4;++n) {
        uint2 p=min(uint2((float2(tid.x,tid.y+n*16)+.5)/float2(16,64)*float2(width,height)),uint2(width-1,height-1));
        float3 rgb=source.Load(int3(p,0)).rgb;
        rgb=float3(isfinite(rgb.x)?max(rgb.x,0):0,isfinite(rgb.y)?max(rgb.y,0):0,isfinite(rgb.z)?max(rgb.z,0):0);
        sum+=log2(max(dot(rgb,float3(.2126,.7152,.0722)),1e-6));
    }
    luminance[index]=sum;GroupMemoryBarrierWithGroupSync();
    for(uint stride=128;stride>0;stride>>=1) {
        if(index<stride) luminance[index]+=luminance[index+stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if(index==0) {
        float mean=exp2(luminance[0]/1024);
        float target=clamp(.18/mean,.001,10000);
        float4 prior=exposure[0];
        float alpha=prior.w==0?1:1-exp(-elapsed/(target>prior.x?.7:.3));
        float gain=exp2(lerp(log2(max(prior.x,.001)),log2(target),alpha));
        exposure[0]=float4(gain,mean,target,1);
    }
}
