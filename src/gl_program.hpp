#pragma once
#include <string>
#include <vector>
#include <cstdint>
struct GLProgram {
    unsigned prog=0;
    bool load_compute_from_file(const char* path);
    void use() const;
    void destroy();
};
std::string read_text_file(const char* path);
