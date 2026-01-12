#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include "engine.hpp"

bool Engine::init(int w,int h,const char* title){
    width=w; height=h;
    if(!glfwInit()) return false;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
    win=glfwCreateWindow(width,height,title,nullptr,nullptr);
    if(!win){ glfwTerminate(); return false; }
    glfwMakeContextCurrent(win);
    if(!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return false;
    glfwSwapInterval(1);
    return true;
}
void Engine::loop(const std::function<void(float)>& frame){
    double last=glfwGetTime();
    while(!glfwWindowShouldClose(win)){
        double now=glfwGetTime();
        float dt=float(now-last); last=now;
        frame(dt);
        glfwSwapBuffers(win);
        glfwPollEvents();
    }
}
void Engine::shutdown(){
    if(win){ glfwDestroyWindow(win); win=nullptr; }
    glfwTerminate();
}
