/* fx_lcd.inc -- bagel-LCD-mini (see fx_common.inc), pass B: the body of main(),
 * drawn at the final size from pass A's picture (fx_lcda.inc: ghost, saturation
 * and the paper shade in the alpha) and the baked grid (fx_lcdm.inc, unit 1:
 * Mask).
 *   U2  contrast
 *   U3  (y) background on | (z) background mix | (w) background tint
 *       (0 white, 1 warm, 2 gray, 3 green) */
float rx = U0.x;
float ry = U0.y;
float2 pp = float2(vTexcoord.x * U5.x, vTexcoord.y * U5.y - U5.z);
float2 uv = float2(pp.x / rx, pp.y / ry);
float rmin = min(U0.z / rx, U0.w / ry);
float sample_scale = max(max(rmin, 1.0f) * U1.y, 1.0f);
float4 a = fx_tap4(Source, U0, U5, fx_sharp(pp, sample_scale));
/* the baked grid, one texel per output pixel (the target is 960 x 544) */
float4 m = tex2D(Mask, float2(pp.x / rx * U0.z * 0.0010416667f,
                              pp.y / ry * U0.w * 0.0018382353f));
float k = U1.w * U2.x;
float off = 0.5f - 0.5f * U2.x;
float3 col = float3(a.x * m.x * k + off, a.y * m.y * k + off,
                    a.z * m.z * k + off);
if (U3.y >= 0.5f)
{
   /* LCD background: clear pixels let a paper backing show through, dark
    * ones leave a shade on it */
   float2 frag = float2(uv.x * U0.z, uv.y * U0.w);
   float t = U3.w;
   float3 paper = float3(
      0.86f + t * (-0.35f / 3.0f + t * (-0.04f + t * 0.1f / 6.0f)),
      0.86f + t * (-0.74f / 3.0f + t * (0.055f + t * 0.01f / 6.0f)),
      0.86f + t * (-1.8f / 3.0f + t * (0.385f + t * -0.45f / 6.0f)));
   float grain = (fx_hash(float2(floor(frag.x * 0.5f), floor(frag.y * 0.5f))) -
                  0.5f) * 0.04f;
   paper = float3((paper.x + grain) * a.w, (paper.y + grain) * a.w,
                  (paper.z + grain) * a.w);
   col = lerp(col, paper, U3.z * smoothstep(0.25f, 0.85f, fx_luma(col)));
}
return float4(col.x, col.y, col.z, 1.0f);
