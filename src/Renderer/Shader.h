#pragma once

#include <cstddef>
#include <vector>

class Shader
{
    public:
        bool Load(const char* path);
        void Unload();

        const void* GetByteCode() const;
        size_t GetBytecodeSize() const;

    private:
        std::vector<char> m_bytecode;
};