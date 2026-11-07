/* fx_lcda.inc -- the LCD look, pass A (see fx_common.inc): what works in the
 * picture's own pixels, drawn once per frame at the picture's size into a
 * render target: the fake ghost, the saturation and, for the paper
 * background, the shade the dark pixels leave on it (in the alpha).
 *   U3  (y) background on
 *   U4  shading depth | shading distance (px at 640x480) | fake ghost on */
float rx = U0.x;
float ry = U0.y;
float2 pp = float2(vTexcoord.x * U5.x, vTexcoord.y * U5.y - U5.z);
float3 col = fx_tap(Source, U0, U5, pp);
if (U4.z >= 0.5f)
{
   /* fake ghosting: a spatial echo, not a previous frame */
   float ex = 5.0f / 640.0f * rx;
   col = col * (0.75f * 0.875f) +
         fx_tap(Source, U0, U5, float2(pp.x - ex, pp.y)) * (0.25f * 0.875f) +
         fx_tap(Source, U0, U5, float2(pp.x - 2.0f * ex, pp.y)) * 0.125f;
}
col = fx_clamp01(fx_saturate(col, U1.z));
float sh = 1.0f;
if (U3.y >= 0.5f)
{
   float3 src = fx_tap(Source, U0, U5,
                       float2(pp.x - U4.y / 640.0f * rx,
                              pp.y - U4.y / 480.0f * ry));
   float dark = clamp(1.0f - fx_luma(src), 0.0f, 1.0f);
   sh = 1.0f - 0.65f * dark * U4.x;
}
return float4(col.x, col.y, col.z, sh);
