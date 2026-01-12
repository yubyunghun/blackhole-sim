#pragma once
#include <functional>
struct GLFWwindow;
struct Engine {
    int width=0,height=0;
    GLFWwindow* win=nullptr;
    bool init(int w,int h,const char* title);
    void loop(const std::function<void(float)>& frame);
    void shutdown();
};
