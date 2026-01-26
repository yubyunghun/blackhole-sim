#version 330 core
in vec2 vUV;
out vec4 o;
uniform sampler2D uTex;
void main(){
    o = texture(uTex, vUV);
}
