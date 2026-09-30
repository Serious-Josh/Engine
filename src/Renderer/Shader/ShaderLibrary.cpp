#include "ShaderLibrary.h"

bool ShaderLibrary::Initialize(const std::filesystem::path& shaderRoot)
{
    m_shaderRoot = shaderRoot;

    if(!m_compiler.Initialize()){
        return false;
    }

    return true;
}

void ShaderLibrary::Shutdown()
{
    for(std::unique_ptr<Shader>& shader : m_shaders){
        shader->Unload();
    }

    m_shaders.clear();

    m_compiler.Shutdown();
}

Shader* ShaderLibrary::Load(const ShaderDescription& description)
{
    if(!description.path || !description.entryPoint){
        return nullptr;
    }

    const std::filesystem::path shaderPath = m_shaderRoot / description.path;

    const std::wstring shaderPathString = shaderPath.wstring();
    const std::wstring entryPointString = std::filesystem::path(description.entryPoint).wstring();

    ShaderCompileDescription compileDescription{};
    compileDescription.path = shaderPathString.c_str();
    compileDescription.entryPoint = entryPointString.c_str();
    compileDescription.stage = description.stage;

    ShaderCompileResult compileResult{};

    if(!m_compiler.Compile(compileDescription, compileResult)){
        return nullptr;
    }

    std::unique_ptr<Shader> shader = std::make_unique<Shader>();

    if(!shader->Initialize(compileResult.bytecode.data(), compileResult.bytecode.size())){
        return nullptr;
    }

    Shader* shaderPointer = shader.get();

    m_shaders.push_back(std::move(shader));

    return shaderPointer;
}