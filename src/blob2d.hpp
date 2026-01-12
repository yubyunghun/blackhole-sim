#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>

struct Blob2D {
    GLuint vao=0, vbo=0, ibo=0, prog=0;
    GLint loc_uVP=-1, loc_uM=-1, loc_uColor=-1, loc_uRadius=-1, loc_uSoft=-1, loc_uGain=-1;

    void init();
    // color: RGB (0..1), radius: world-units in plane of blob, soft: edge softness, gain: brightness
    void draw(const glm::mat4& VP, const glm::mat4& M,
              const glm::vec3& color, float radius, float soft, float gain);
    void shutdown();
};
