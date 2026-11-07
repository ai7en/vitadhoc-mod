/* fx_crt.inc -- bagel-CRT-mini (see fx_common.inc), pass B: the body of main(),
 * drawn at the final size from pass A's picture (fx_crta.inc), which has the
 * dither merge, the ghost and the saturation already in it, and the baked
 * screen-size terms (fx_crtm.inc, unit 1: Mask).
 *   U2  contrast | scan grid (0 auto, 1 240-class, 2 480-class) |
 *       scanline dark | scanline bright
 *   (U3 is baked into the mask: mask type, strength, vignette)
 *   U4  (z) noise on
 * No vertical roll, no warp of the picture and no rounded corners. */
float rx = U0.x;
float ry = U0.y;
float2 pp = float2(vTexcoord.x * U5.x, vTexcoord.y * U5.y - U5.z);
float rmin = min(U0.z / rx, U0.w / ry);
float sample_scale = max(rmin * U1.y, 1.0f);
float2 sp = fx_sharp(pp, sample_scale);
float3 col = fx_tap(Source, U0, U5, sp);
/* the baked terms, one texel per output pixel (the target is 960 x 544) */
float4 m = tex2D(Mask, float2(pp.x / rx * U0.z * 0.0010416667f,
                              pp.y / ry * U0.w * 0.0018382353f));
float noise_add = 0.0f;
if (U4.z >= 0.5f)
{
   float ts = frac(fmod(U1.x, 1009.0f) * 0.618034f) * 1000.0f;
   noise_add = (fx_hash(float2(pp.x / rx * U0.z + ts, pp.y / ry * U0.w + ts)) - 0.5f) * 0.08f;
}
/* scanline strength follows the picture's brightness, the vignette darkens
 * towards the sides */
float w = 0.3036f * (col.x + col.y + col.z);
float row_mul = max((1.0f - U2.z) - (U2.w - U2.z) * w - m.y * 0.25f, 0.0f) * (m.x * 3.0f);
float kk = row_mul * (U1.w * U2.x);
float off = 0.5f - 0.5f * U2.x;
col = fx_clamp01(float3(col.x * kk + off + noise_add, col.y * kk + off + noise_add,
                        col.z * kk + off + noise_add));
return float4(col.x * m.z, col.y * m.z, col.z * m.z, 1.0f);
