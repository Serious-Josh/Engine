#include "Shader.h"

bool Shader::Initialize(
    const void* bytecode,
    size_t size)
{
    if (!bytecode || size == 0)
    {
        return false;
    }

    const char* bytecodeBegin = static_cast<const char*>(bytecode);

    const char* bytecodeEnd = bytecodeBegin + size;

    m_bytecode.assign(bytecodeBegin, bytecodeEnd);

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
