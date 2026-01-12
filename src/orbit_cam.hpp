#pragma once
#include <glm/glm.hpp>
struct GLFWwindow;

struct OrbitCam {
    float yaw=-0.9f, pitch=-0.35f, radius=2.6f;
    glm::vec3 target{0.0f,0.0f,0.0f};
    double lastX=0.0, lastY=0.0;
    bool dragging=false;

    void handle(GLFWwindow* win, float dt);
    glm::mat4 view() const;
    glm::mat4 proj(float aspect) const;
    glm::vec3 eye() const;
};
