#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>

struct Grid3D {
    GLuint vao=0, vbo=0, ibo=0, prog=0;
    // uniforms
    GLint loc_uVP=-1, loc_uM=-1, loc_uColor=-1;
    GLint loc_uBH=-1, loc_uSingR=-1, loc_uWarpK=-1, loc_uTime=-1;

    // runtime params
    glm::vec3 bhPos{0.0f, 0.0f, 0.0f}; // black hole center in world
    float singR = 0.35f;               // “event horizon” radius used for warp falloff
    float warpK = 0.35f;               // lensing strength

    void init();
    void set_warp(const glm::vec3& bh, float r, float k);
    void draw(const glm::mat4& VP, const glm::vec3& color);
    void shutdown();
};
