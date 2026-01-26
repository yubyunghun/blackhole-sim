#version 330 core
in vec2 vUV; out vec4 o;
uniform sampler2D uTex; uniform vec2 uStep;   // one texel along the blur axis
void main(){
    const float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec3 s = texture(uTex, vUV).rgb * w[0];
    for (int i = 1; i < 5; ++i){
        s += texture(uTex, vUV + uStep * float(i)).rgb * w[i];
        s += texture(uTex, vUV - uStep * float(i)).rgb * w[i];
    }
    o = vec4(s, 1.0);
}
