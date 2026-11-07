/* fx_crta.inc -- the CRT look, pass A (see fx_common.inc): everything that
 * works in the picture's own pixels, drawn once per frame at the picture's
 * size into a render target: the dither merge / dot crawl, the RF ghost and
 * the saturation.  Pass B (fx_crt.inc) reads the result.
 *   U2  grid (U2.y: 0 auto, 1 240-class, 2 480-class)
 *   U4  dither merge (0 off, 1 on, 2 dot crawl) | RF ghost (0 off, 1 static,
 *       2 motion)
 * Taps sit on the pixel centres, so the picture is read unfiltered. */
float rx = U0.x;
float ry = U0.y;
float2 pp = float2(vTexcoord.x * U5.x, vTexcoord.y * U5.y - U5.z);
bool coarse = U2.y > 1.5f || (U2.y <= 0.5f && ry > 300.0f);
float2 sp = pp;
float3 col;
float half_px = 0.0f;
if (U4.x < 0.5f)
   col = fx_tap(Source, U0, U5, sp);
else
{
   /* horizontal blur that merges fine dithering; dot crawl leans the
    * weights by row group on a slow cycle */
   float crawl = U4.x > 1.5f ? 0.05f * fx_sine(U1.x * 0.031830989f) : 0.0f;
   if (coarse)
   {
      float lean = frac(pp.y * 0.125f) < 0.5f ? crawl : -crawl;
      float3 c1 = fx_tap(Source, U0, U5, sp);
      float3 c0 = fx_tap(Source, U0, U5, float2(sp.x - 2.0f, sp.y));
      float3 c2 = fx_tap(Source, U0, U5, float2(sp.x + 2.0f, sp.y));
      col = c1 + (c0 * (0.25f + lean) + c2 * (0.25f - lean) - c1 * 0.5f);
      half_px = 1.0f;
   }
   else
   {
      float lean = frac(pp.y * 0.25f) < 0.5f ? crawl : -crawl;
      float shift = 2.0f * lean;
      col = (fx_tap(Source, U0, U5, float2(sp.x - 0.5f - shift, sp.y)) +
             fx_tap(Source, U0, U5, float2(sp.x + 0.5f - shift, sp.y))) * 0.5f;
      half_px = 0.5f;
   }
}
if (U4.y >= 0.5f)
{
   float gl = 5.0f;
   float gs = 0.5f;
   if (U4.y > 1.5f)
   {
      /* motion: now and then the echo drifts closer or further and back */
      float cycle_id = floor(U1.x / 2400.0f);
      float t = U1.x - cycle_id * 2400.0f;
      float slot_len = (2400.0f - 270.0f) / 2.0f;
      float slot = min(floor(t / slot_len), 1.0f);
      float drift_id = cycle_id * 2.0f + slot;
      float start = fx_hash(float2(drift_id, 7.7f)) * (slot_len - 600.0f);
      float p = clamp((t - slot * slot_len - start) / 600.0f, 0.0f, 1.0f);
      float hh = 4.0f * p * (1.0f - p);
      float s = hh * (0.775f + 0.225f * hh);
      float bump = s * s;
      float r = fx_hash(float2(drift_id, 9.1f));
      float drift = r < 0.25f ? lerp1(-3.5f, -3.0f, r / 0.25f)
                              : lerp1(6.0f, 10.0f, (r - 0.25f) / 0.75f);
      gl = 5.0f + drift * bump;
      gs = 0.5f * (1.0f + 0.3f * bump);
   }
   col = fx_echoes(Source, U0, U5, col, pp, half_px, gl, gs);
}
col = fx_clamp01(fx_saturate(col, U1.z));
return float4(col.x, col.y, col.z, 1.0f);
