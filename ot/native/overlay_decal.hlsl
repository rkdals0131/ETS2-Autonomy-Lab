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
// The path for an exact distance per pixel (the mask's 25 cm texels stepped a thin ribbon).
cbuffer Path : register(b1) {
    float4 path_info;          // point count, half width (m), fade start, fade end (m of arc)
    float4 path_points[256];   // x, z relative to the mask origin; z: arc length from the start
};
struct Vertex { float4 position : SV_Position; };
// Dual-source output (blend: target = add + target * multiply). The mark is paint on the lit,
// tone-mapped scene: it tints what is below, so it takes the scene's light, shade and
// night instead of a flat colour laid over the image (that read as glowing, 2026-10-21),
// and a small emissive floor keeps it readable on an unlit road.
struct Paint { float4 add : SV_Target0; float4 multiply : SV_Target1; };
static const float paint_gain = 2.2;     // tint of a full-strength mark on mid grey
static const float paint_floor = 0.06;   // emissive share of the colour
// A mark across the view is foreshortened on the road: the ACC bar's 0.5 m under a lead
// 50 m ahead is about one pixel tall (2026-10-21, not seen). Every part of the path keeps
// at least this half thickness on screen (pixels at 1080 lines), up to max_half_m on the road.
static const float min_half_px = 4;
static const float max_half_m = 4;       // overlay_decal.cpp widens the bounds by it
// A 4-vertex strip over `bounds`: the command list stays fixed while the
// rectangle (the scissor of the design) changes every frame.
Vertex vs_main(uint id : SV_VertexID) {
    Vertex v;
    v.position = float4((id & 1) ? bounds.z : bounds.x, (id & 2) ? bounds.y : bounds.w, 0, 1);
    return v;
}
Paint ps_main(Vertex input) {
    float2 pixel = input.position.xy;
    float d = scene_depth.Load(int3(pixel * depth.zw, 0));
    float2 uv = pixel * target.zw;
    float4 eye = mul(inverse_projection, float4(2 * uv.x - 1, 1 - 2 * uv.y, (d - depth.x) / (depth.y - depth.x), 1));
    float3 p = mul((float3x3)eye_to_world, eye.xyz / eye.w); // world offset from the camera
    // Distance to the path and the arc length at the nearest point.
    float2 q = p.xz + camera.xz;
    float best = 1e9, arc = 0;
    float2 along = 0;   // direction of the nearest segment
    uint count = (uint)path_info.x;
    for (uint i = 1; i < count; ++i) {
        float2 a = path_points[i - 1].xy, ab = path_points[i].xy - a;
        float l2 = dot(ab, ab);
        float t = l2 > 0 ? saturate(dot(q - a, ab) / l2) : 0;
        float dd = length(q - a - t * ab);
        if (dd < best) { best = dd; arc = path_points[i - 1].z + t * sqrt(l2); along = l2 > 0 ? ab * rsqrt(l2) : 0; }
    }
    // Derivatives before any discard: the quad still has all four lanes here. The edge is
    // one pixel wide at every distance (fwidth of the metric distance).
    float3 n = cross(ddx(p), ddy(p));
    float edge = max(fwidth(best), 0.01);
    // Road metres per pixel across the nearest segment, from the view ray on the flat ground
    // plane: smooth over depth edges, where fwidth(best) jumps to the object behind.
    bool flat = camera.w != 0 && p.y < -1e-3 * length(p);
    float2 g = camera.xz + p.xz * (flat ? -camera.y / p.y : 0);
    float2 gx = ddx(g), gy = ddy(g), across = float2(-along.y, along.x);
    float per_pixel = abs(dot(across, gx)) + abs(dot(across, gy));
    float half_width = flat ? clamp(min_half_px * target.y / 1080 * per_pixel, path_info.y, max_half_m) : path_info.y;
    // Outside this frame's exterior range: interior (dashboard, pillars, mirrors) or sky.
    if (!(d >= depth.x && d <= depth.y) || !(eye.w != 0)) discard;
    float nl = length(n);
    if (!(nl > 0) || abs(n.y) < road.z * nl) discard;          // walls, car sides, housings
    float horizontal = length(p.xz);
    if (camera.w != 0 && abs(p.y + camera.y) > road.x + horizontal * road.y) discard; // off the road height
    float coverage = 1 - smoothstep(half_width - edge, half_width + edge, best);
    // A darker rim (12 cm, at most 40 % of the half width; widened with it) outlines the
    // mark on bright road.
    float rim = min(0.12, 0.4 * path_info.y) * half_width / path_info.y;
    float inner = 1 - smoothstep(half_width - rim - edge, half_width - rim + edge, best);
    float fade = 1 - smoothstep(path_info.z, path_info.w, arc);
    float alpha = color.a * coverage * fade;   // strength of the paint
    if (alpha <= 0) discard;
    float3 tint = lerp(color.rgb * 0.35, color.rgb, inner);
    Paint paint;
    paint.add = float4(alpha * paint_floor * tint, 0);
    paint.multiply = float4(1 - alpha + alpha * paint_gain * tint, 1);
    return paint;
}
