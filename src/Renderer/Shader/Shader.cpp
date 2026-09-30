#include "Shader.h"

#include <fstream>

bool Shader::Initialize(const void* bytecode, size_t size)
{
    if(!bytecode || size == 0){
        return false;
    }

    const char* bytecodeBegin = static_cast<const char*>(bytecode);
    const char* bytecodeEnd = bytecodeBegin + size;

    m_bytecode.assign(bytecodeBegin, bytecodeEnd);

    return true;
}

bool Shader::Load(const char* path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);

    if(!file){
        return false;
    }


    const std::streamsize size = file.tellg();

    if(size <= 0){
        return false;
    }
    
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