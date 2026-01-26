#version 330 core
in vec2 vUV; out vec4 o;
uniform sampler2D uHDR, uBloomA, uBloomB;
uniform float uBloom;      // bloom strength (0 = off)
uniform int   uTonemap;    // 1 = ACES tone map, 0 = plain clamp (reference look)
uniform float uExposure;
vec3 aces(vec3 x){ return clamp((x*(2.51*x + 0.03)) / (x*(2.43*x + 0.59) + 0.14), 0.0, 1.0); }
void main(){
    vec4 h = texture(uHDR, vUV);
    vec3 b = texture(uBloomA, vUV).rgb * 0.6 + texture(uBloomB, vUV).rgb * 0.9;
    vec3 c = h.rgb + b * uBloom;
    if (uTonemap == 1) c = aces(c * uExposure);
    o = vec4(c, h.a);        // premultiplied: glow (alpha 0) adds over the grid
}
