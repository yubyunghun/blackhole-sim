#version 330 core
in vec2 vUV; out vec4 o;
uniform sampler2D uTex; uniform float uThreshold;
void main(){
    vec3 c = texture(uTex, vUV).rgb;
    float l = max(c.r, max(c.g, c.b));
    // soft threshold: keep only the part of the brightness above uThreshold
    o = vec4(c * (max(l - uThreshold, 0.0) / max(l, 1e-4)), 1.0);
}
