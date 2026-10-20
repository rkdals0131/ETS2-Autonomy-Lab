// HUD trajectory decal (docs/24 §5). Drawn into main0/composition_out just before
// blit_to_target; reads the scene depth and a BEV mask, never writes depth.
Texture2D<float> scene_depth : register(t0);
Texture2D<float2> path_mask : register(t1);
SamplerState mask_sampler : register(s0);
cbuffer Overlay : register(b0) {
    row_major float4x4 inverse_projection; // corrected clip -> eye of the exterior main pass
    row_major float4x4 eye_to_world;       // rotation only; world axes are SCS (Y up)
    float4 camera;   // camera relative to the mask origin (x, height above ground_y, z); w: ground known
    float4 depth;    // exterior viewport depth near, far; depth texels per target pixel (x, y)
    float4 mask_u;   // uv.x = dot(xy, world xz relative to the mask origin) + z
    float4 mask_v;
    float4 road;     // z tolerance (m), slope tolerance (tan), minimum |normal.y|
    float4 color;
    float4 target;   // width, height, 1/width, 1/height
    float4 bounds;   // NDC rectangle x0, y0, x1, y1 covering the projected path
};
struct Vertex { float4 position : SV_Position; };
// A 4-vertex strip over `bounds`: the command list stays fixed while the
// rectangle (the scissor of the design) changes every frame.
Vertex vs_main(uint id : SV_VertexID) {
    Vertex v;
    v.position = float4((id & 1) ? bounds.z : bounds.x, (id & 2) ? bounds.y : bounds.w, 0, 1);
    return v;
}
float4 ps_main(Vertex input) : SV_Target {
    float2 pixel = input.position.xy;
    float d = scene_depth.Load(int3(pixel * depth.zw, 0));
    float2 uv = pixel * target.zw;
    float4 eye = mul(inverse_projection, float4(2 * uv.x - 1, 1 - 2 * uv.y, (d - depth.x) / (depth.y - depth.x), 1));
    float3 p = mul((float3x3)eye_to_world, eye.xyz / eye.w); // world offset from the camera
    // Derivatives before any discard: the quad still has all four lanes here.
    float3 n = cross(ddx(p), ddy(p));
    // Outside this frame's exterior range: interior (dashboard, pillars, mirrors) or sky.
    if (!(d >= depth.x && d <= depth.y) || !(eye.w != 0)) discard;
    float nl = length(n);
    if (!(nl > 0) || abs(n.y) < road.z * nl) discard;          // walls, car sides, housings
    float horizontal = length(p.xz);
    if (camera.w != 0 && abs(p.y + camera.y) > road.x + horizontal * road.y) discard; // off the road height
    float2 q = p.xz + camera.xz;
    float2 m = path_mask.SampleLevel(mask_sampler, float2(dot(mask_u.xy, q) + mask_u.z, dot(mask_v.xy, q) + mask_v.z), 0);
    float alpha = color.a * m.x;
    if (alpha <= 0) discard;
    return float4(color.rgb, alpha);
}
