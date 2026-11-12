#version 450
#extension GL_GOOGLE_include_directive : require
#include "../../pathtrace/shaders/display.glsl"
#include "../../postfx/shaders/composite.glsl"
layout(binding=0,rgba16f) readonly uniform image2D upscaledColor;
layout(binding=1,r32f) readonly uniform image2D stableDepth;
// The view effects of what screens and portals show (render resolution:
// chromatic aberration in pixels of a 1920-wide output; 0 elsewhere).
layout(binding=2,r16f) readonly uniform image2D viewEffects;
// display and the nits: displayColor (pathtrace/shaders/display.glsl).
// viewEffects: 1 when the image holds this frame's (the path tracer's primary
// surface replacement pass ran).
layout(push_constant) uniform Settings { uint display; float peakNits; float paperWhiteNits; uint viewEffects; } settings;
float viewEffectAt(ivec2 pixel,ivec2 size) {
  ivec2 source=ivec2((vec2(pixel)+.5)*vec2(imageSize(viewEffects))/vec2(size));
  return imageLoad(viewEffects,clamp(source,ivec2(0),imageSize(viewEffects)-1)).r;
}
layout(location=0) out vec4 result;
void main() {
  ivec2 pixel=ivec2(gl_FragCoord.xy);
  vec3 color=imageLoad(upscaledColor,pixel).rgb;
  // remixapi_MaterialInfoViewEffectsEXT: the red channel from the right, the
  // blue from the left, where those pixels show such a view too.
  if(settings.viewEffects!=0u) {
    ivec2 size=imageSize(upscaledColor);
    float aberration=viewEffectAt(pixel,size);
    if(aberration>0) {
      int shift=max(1,int(round(aberration*float(size.x)/1920.)));
      ivec2 right=min(pixel+ivec2(shift,0),size-1),left=max(pixel-ivec2(shift,0),ivec2(0));
      if(viewEffectAt(right,size)>0)color.r=imageLoad(upscaledColor,right).r;
      if(viewEffectAt(left,size)>0)color.b=imageLoad(upscaledColor,left).b;
    }
  }
  color=postColor(color,gl_FragCoord.xy/vec2(imageSize(upscaledColor)));
  color=displayColor(color,settings.display,settings.peakNits,settings.paperWhiteNits);
  result=vec4(color,1);
  gl_FragDepth=imageLoad(stableDepth,pixel).r;
}
