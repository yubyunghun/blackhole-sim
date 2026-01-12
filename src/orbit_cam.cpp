#include "orbit_cam.hpp"
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
using namespace glm;

glm::vec3 OrbitCam::eye() const {
    float cp = cosf(pitch), sp = sinf(pitch);
    float cy = cosf(yaw),   sy = sinf(yaw);
    vec3 dir(cp*cy, sp, cp*sy);
    return target - radius * dir;
}

glm::mat4 OrbitCam::view() const {
    return lookAt(eye(), target, vec3(0,1,0));
}

glm::mat4 OrbitCam::proj(float aspect) const {
    return perspective(radians(55.0f), aspect, 0.05f, 100.0f);
}

void OrbitCam::handle(GLFWwindow* win, float dt){
    double x,y; glfwGetCursorPos(win,&x,&y);
    int mb = glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_LEFT);
    if(mb==GLFW_PRESS && !dragging){ dragging=true; lastX=x; lastY=y; }
    if(mb==GLFW_RELEASE) dragging=false;
    if(dragging){
        float dx=float(x-lastX), dy=float(y-lastY);
        lastX=x; lastY=y;
        yaw   -= dx*0.005f;
        pitch -= dy*0.005f;
        pitch = std::clamp(pitch, -1.4f, 1.4f);
    }
    if(glfwGetKey(win, GLFW_KEY_W)==GLFW_PRESS) target += vec3(0,0,-1)*0.8f*dt;
    if(glfwGetKey(win, GLFW_KEY_S)==GLFW_PRESS) target += vec3(0,0, 1)*0.8f*dt;
    if(glfwGetKey(win, GLFW_KEY_A)==GLFW_PRESS) target += vec3(-1,0,0)*0.8f*dt;
    if(glfwGetKey(win, GLFW_KEY_D)==GLFW_PRESS) target += vec3( 1,0,0)*0.8f*dt;
    if(glfwGetKey(win, GLFW_KEY_Q)==GLFW_PRESS) radius += 1.2f*dt;
    if(glfwGetKey(win, GLFW_KEY_E)==GLFW_PRESS) radius -= 1.2f*dt;
    radius = std::clamp(radius, 0.4f, 10.0f);
}
