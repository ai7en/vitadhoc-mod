/* fx_lcdm.inc -- the LCD look's screen-size terms that do not change from
 * frame to frame, drawn once into a render target the size of the picture's
 * output (video_vita.c fx_draw) and read by pass B (fx_lcd.inc): the grid's
 * three factors times the border fade.  Same uniforms as pass B; redrawn
 * when the picture's size, its output size or the filter changes.
 *   U2  grid mode (0 auto, 1 240-class, 2 480-class) | grid (0 off, 1 RGB,
 *       2 mono) | border fade on
 *   U3  border depth */
float rx = U0.x;
float ry = U0.y;
float2 pp = float2(vTexcoord.x * U5.x, vTexcoord.y * U5.y - U5.z);
float2 uv = float2(pp.x / rx, pp.y / ry);
float2 frag = float2(uv.x * U0.z, uv.y * U0.w);
bool coarse = U2.y > 1.5f || (U2.y <= 0.5f && ry > 300.0f);
float rmin = min(U0.z / rx, U0.w / ry);
float mscale = max(floor((coarse ? 2.0f * rmin : rmin) + 0.501f), 2.0f);
float3 grid = float3(1.0f, 1.0f, 1.0f);
float gx = frag.x / mscale;
float gy = frag.y / mscale;
if (U2.z > 1.5f)
{
   /* mono (lcd1x): one factor, a dark gap between pixels */
   float yf = 16.0f / 17.0f - 1.0f / 17.0f * fx_sine(gy + 0.25f);
   float xf = 0.8f - 0.2f * fx_sine(gx + 0.25f);
   float m = yf * xf;
   grid = float3(m, m, m);
}
else if (U2.z > 0.5f)
{
   /* RGB (lcd3x): sub-pixel phases +90, -30 and -150 degrees */
   float yf = 16.0f / 17.0f + 1.0f / 17.0f * fx_sine(gy);
   float u = frac(gx) - 0.5f;
   float sx = -fx_sinh(u);
   float cx = -fx_sinh(0.25f - abs(u));
   grid = float3(yf * (4.0f + cx) / 5.0f,
                 yf * (4.0f + 0.8660254f * sx - 0.5f * cx) / 5.0f,
                 yf * (4.0f - 0.8660254f * sx - 0.5f * cx) / 5.0f);
}
float kb = 1.0f;
if (U2.w >= 0.5f)
{
   /* rounded fade at the edges */
   float hx = abs(uv.x - 0.5f);
   float hy = abs(uv.y - 0.5f);
   if (max(hx, hy) >= 0.475f)
   {
      float qx = max(hx - 0.475f, 0.0f);
      float qy = max(hy - 0.475f, 0.0f);
      float c = clamp((0.025f - sqrt(qx * qx + qy * qy)) * 60.0f, 0.0f, 1.0f);
      kb = lerp1(1.0f - U3.x, 1.0f, c);
   }
}
return float4(grid.x * kb, grid.y * kb, grid.z * kb, 1.0f);
