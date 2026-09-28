#include "Shader.h"

#include <fstream>
#include <iostream>

bool Shader::Load(const char* path)
{
    std::cout
    << "Shader this = " << this
    << ", vector = " << &m_bytecode
    << '\n';


    std::ifstream file(path, std::ios::binary | std::ios::ate);

    if(!file){
        return false;
    }


    const std::streamsize size = file.tellg();

    if(size <= 0){
        return false;
    }
    std::cout
    << "Shader size = " << size
    << ", vector size = " << m_bytecode.size()
    << ", vector capacity = " << m_bytecode.capacity()
    << '\n';


    
    m_bytecode.resize(static_cast<size_t>(size));


    file.seekg(0, std::ios::beg);

    if(!file.read(m_bytecode.data(), size)){
        m_bytecode.clear();
        return false;
    }


    return true;
}

void Shader::Unload()
{
    m_bytecode.clear();
}

const void* Shader::GetByteCode() const
{
    return m_bytecode.data();
}

size_t Shader::GetBytecodeSize() const
{
    return m_bytecode.size();
}