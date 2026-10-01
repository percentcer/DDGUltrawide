cbuffer CB : register(b0)
{
    float4 dstRect;
    float4 srcRect;
    float4 clampRect;
};
struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
VSOut VSMain(uint id : SV_VertexID)
{
    float2 t = float2(id & 1, id >> 1);
    float2 p = lerp(dstRect.xy, dstRect.zw, t);
    VSOut o;
    o.pos = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);
    o.uv = lerp(srcRect.xy, srcRect.zw, t);
    return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 PSMain(VSOut i) : SV_Target
{
    float2 uv = clamp(i.uv, clampRect.xy, clampRect.zw);
    return float4(tex.Sample(smp, uv).rgb, 1);
}

// Perspective warp: each window pixel shows the flat canvas (t0) where its
// camera ray lands. Within the corners (the center wall, seen straight on) that's
// the same point; past them, the ray meets a side cabinet's face, turned toward
// the camera, at a point that sits further out on the flat drawing. There, the
// side screens' pictures are drawn again, straight from the game's frame (t1)
// at its full resolution rather than the canvas's smaller copy, with the
// acrylic's reflections (t2) over them.
cbuffer Warp : register(b1)
{
    float4 wOut;     // window width, height; canvas width; canvas offset
    float4 wGeo;     // center picture's middle (window px); pixels per inch; corner (inches)
    float4 wCam;     // sin, cos of the faces' turn; camera distance (inches)
    float4 wPic[2];  // the side screens' pictures in the canvas (px)
    float4 wSrc[2];  // ...and in the game's frame (uv)
    float4 wTexel;   // a texel of the game's frame (uv)
    float4 wPanel[3];
};
Texture2D frame : register(t1);
Texture2D reflections : register(t2);
float3 SrgbToLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow(abs((c + 0.055) / 1.055), 2.4); }
float3 LinearToSrgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(abs(c), 1 / 2.4) - 0.055; }
float4 PSWarp(VSOut i) : SV_Target
{
    float2 f = (i.pos.xy - wGeo.xy) / wGeo.z;          // inches from the center picture's middle
    float ax = abs(f.x);
    if (ax > wGeo.w)
    {
        float sa = wCam.x, ca = wCam.y, Z = wCam.z, c0 = wGeo.w;
        float t = (sa * c0 + ca * Z) / (sa * ax + ca * Z);    // along the camera ray, to the face
        float along = ca * (t * ax - c0) + sa * Z * (1 - t);  // along the face from the corner
        f = float2(sign(f.x) * (c0 + along), t * f.y);
    }
    float2 px = f * wGeo.z + wGeo.xy + float2(wOut.w, 0);
    float2 cuv = px / float2(wOut.z, wOut.y);
    float3 c = tex.SampleLevel(smp, cuv, 0).rgb;
    [unroll] for (int s = 0; s < 2; ++s)
    {
        float2 q = (px - wPic[s].xy) / max(wPic[s].zw - wPic[s].xy, 1);
        float2 uv = clamp(lerp(wSrc[s].xy, wSrc[s].zw, q), wSrc[s].xy + wTexel.xy, wSrc[s].zw - wTexel.xy);
        float3 p = frame.Sample(smp, uv).rgb;          // (sampled everywhere: its mip level follows the warp)
        if (wPic[s].z > wPic[s].x && all(q >= 0) && all(q <= 1))
            c = LinearToSrgb(saturate(SrgbToLinear(p) + reflections.SampleLevel(smp, cuv, 0).rgb));
    }
    if (px.x < 0 || px.x > wOut.z) c = 0;              // past the canvas: left dark
    return float4(c, 1);
}

// With perspective, the touch panel: tilted back with the hood's front, a quad
// on screen. Each pixel's place on it (u, v) from the projective map (wPanel),
// then the panel's picture (srcRect) there, blended in by how much of the
// pixel it covers (so its slanted edges don't stair-step).
float4 PSPanel(VSOut i) : SV_Target
{
    float3 p = float3(i.pos.xy, 1);
    float3 q = float3(dot(wPanel[0].xyz, p), dot(wPanel[1].xyz, p), dot(wPanel[2].xyz, p));
    float2 uv = q.xy / q.z;
    float2 edge = min(uv, 1 - uv) / max(fwidth(uv), 1e-6);        // to the nearest edges, in pixels
    float cover = saturate(min(edge.x, edge.y) + 0.5);
    float3 c = tex.Sample(smp, clamp(lerp(srcRect.xy, srcRect.zw, saturate(uv)), clampRect.xy, clampRect.zw)).rgb;
    clip(cover - 1e-3);
    return float4(c, cover);
}
