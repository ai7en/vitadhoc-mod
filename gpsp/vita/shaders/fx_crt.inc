/* fx_crt.inc -- bagel-CRT-mini (see fx_common.inc), pass B: the body of main(),
 * drawn at the final size from pass A's picture (fx_crta.inc), which has the
 * dither merge, the ghost and the saturation already in it.
 *   U2  contrast | scan grid (0 auto, 1 240-class, 2 480-class) |
 *       scanline dark | scanline bright
 *   U3  mask type (0 aperture, 1 fine, 2 locked to the picture's pixels) |
 *       mask strength | vignette | border (0 geom, 1 corner, 2 flat)
 *   U4  (z) noise on
 * No vertical roll, and the picture is never warped: the Geom border only
 * borrows the curved outline for its rounded corners. */
float rx = U0.x;
float ry = U0.y;
float2 pp = float2(vTexcoord.x * U5.x, vTexcoord.y * U5.y - U5.z);
float2 uv = float2(pp.x / rx, pp.y / ry);
bool coarse = U2.y > 1.5f || (U2.y <= 0.5f && ry > 300.0f);
float rmin = min(U0.z / rx, U0.w / ry);
float sample_scale = max(rmin * U1.y, 1.0f);
float grid_mul = coarse ? 2.0f : 1.0f;
float mscale = max(floor(rmin * grid_mul + 0.501f), 1.0f);
float row_f = 1.0f;
if (mscale * grid_mul >= 2.5f)
{
   /* rows follow a sine; the 2x coarse grid alternates */
   float ra = mscale < 2.5f ? 0.9219821f : 0.9428090f;
   float rb = mscale < 2.5f ? 0.3872325f : 0.4714045f;
   float ph = uv.y * U0.w / mscale -
              (coarse ? 0.25f * mscale - 0.5f * floor(0.5f * mscale) : 0.25f);
   row_f = ra + rb * fx_sine(ph);
}
float2 sp = fx_sharp(pp, sample_scale);
float3 col = fx_tap(Source, U0, U5, sp);
float noise_add = 0.0f;
if (U4.z >= 0.5f)
{
   float ts = frac(fmod(U1.x, 1009.0f) * 0.618034f) * 1000.0f;
   noise_add = (fx_hash(float2(uv.x * U0.z + ts, uv.y * U0.w + ts)) - 0.5f) * 0.08f;
}
/* scanline strength follows the picture's brightness, the vignette darkens
 * towards the sides */
float w = 0.3036f * (col.x + col.y + col.z);
float vig = uv.x - 0.5f;
float row_mul = max((1.0f - U2.z) - (U2.w - U2.z) * w - U3.z * vig * vig, 0.0f) * row_f;
float mq = uv.x * U0.z * ((U3.x > 0.5f ? 0.6666f : 1.0f) * 0.5f);
if (U3.x > 1.5f)
   mq = uv.x * U0.z / mscale;
float mask = (1.0f - U3.y) + U3.y * fx_sine(mq);
float kk = row_mul * mask * (U1.w * U2.x);
float off = 0.5f - 0.5f * U2.x;
col = fx_clamp01(float3(col.x * kk + off + noise_add, col.y * kk + off + noise_add,
                        col.z * kk + off + noise_add));
/* border */
float bm = 1.0f;
{
   float hx = abs(uv.x - 0.5f);
   float hy = abs(uv.y - 0.5f);
   if (max(hx, hy) >= 0.43f)
   {
      float radius = 0.04f;
      float trans = 90.0f;
      if (U3.w < 0.5f)
      {
         float ox = hx * (1.0f + 4.0f * hy * hy * 0.031f);
         float oy = hy * (1.0f + 4.0f * hx * hx * 0.041f);
         hx = ox;
         hy = oy;
      }
      else if (U3.w < 1.5f)
      {
         radius = 0.055f;
         trans = 60.0f;
      }
      else
      {
         radius = 0.025f;
         trans = 60.0f;
      }
      float qx = max(hx - (0.5f - radius), 0.0f);
      float qy = max(hy - (0.5f - radius), 0.0f);
      float m = clamp((radius - sqrt(qx * qx + qy * qy)) * trans, 0.0f, 1.0f);
      bm = U3.w > 1.5f ? lerp1(0.75f, 1.0f, m) : m;
   }
}
return float4(col.x * bm, col.y * bm, col.z * bm, 1.0f);
