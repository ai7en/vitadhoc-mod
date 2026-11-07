/* fx_crtm.inc -- the CRT look's screen-size terms that do not change from
 * frame to frame, drawn once into a render target the size of the picture's
 * output (video_vita.c fx_draw) and read by pass B (fx_crt.inc):
 *   r  scan row shape x phosphor mask, / 3
 *   g  vignette term (strength * (x - 0.5)^2), x 4
 *   b  1 (the rounded corners of the Geom look are left out)
 * Same uniforms as pass B; redrawn when the picture's size, its output size
 * or the filter changes. */
float rx = U0.x;
float ry = U0.y;
float2 pp = float2(vTexcoord.x * U5.x, vTexcoord.y * U5.y - U5.z);
float2 uv = float2(pp.x / rx, pp.y / ry);
bool coarse = U2.y > 1.5f || (U2.y <= 0.5f && ry > 300.0f);
float rmin = min(U0.z / rx, U0.w / ry);
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
float vig = uv.x - 0.5f;
float mq = uv.x * U0.z * ((U3.x > 0.5f ? 0.6666f : 1.0f) * 0.5f);
if (U3.x > 1.5f)
   mq = uv.x * U0.z / mscale;
float mask = (1.0f - U3.y) + U3.y * fx_sine(mq);
return float4(row_f * mask * 0.33333334f, U3.z * vig * vig * 4.0f, 1.0f, 1.0f);
