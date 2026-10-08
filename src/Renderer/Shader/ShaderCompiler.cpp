#include "Core/Log.h"
#include "ShaderCompiler.h"

#include <filesystem>
#include <string>

bool ShaderCompiler::Initialize()
{
    HRESULT result = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&m_utils));

    if(FAILED(result)){
        return false;
    }

    result = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_compiler));

    if(FAILED(result)){
        m_utils->Release();
        m_utils = nullptr;

        return false;
    }

    return true;

}

void ShaderCompiler::Shutdown()
{
    if(m_compiler){
        m_compiler->Release();
        m_compiler = nullptr;
    }

    if(m_utils){
        m_utils->Release();
        m_utils = nullptr;
    }
}

bool ShaderCompiler::Compile(const ShaderCompileDescription& description, ShaderCompileResult& output)
{
    output.bytecode.clear();

    if(!m_utils || !m_compiler){
        return false;
    }

    if(!description.path || !description.entryPoint){
        return false;
    }

    const wchar_t* profile = GetShaderProfile(description.stage);

    if(!profile){
        return false;
    }

    IDxcBlobEncoding* source = nullptr;

    HRESULT result = m_utils->LoadFile(description.path, nullptr, &source);

    if(FAILED(result)){
        return false;
    }

    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr = source->GetBufferPointer();
    sourceBuffer.Size = source->GetBufferSize();
    sourceBuffer.Encoding = DXC_CP_UTF8;

    IDxcIncludeHandler* includeHandler = nullptr;

    result = m_utils->CreateDefaultIncludeHandler(&includeHandler);

    if(FAILED(result)){
        source->Release();
        return false;
    }

    std::filesystem::path shaderPath = description.path;
    std::wstring shaderDirectory = shaderPath.parent_path().wstring();

    const wchar_t* arguments[] =
    {
        description.path,
        L"-E", description.entryPoint,
        L"-T", profile,
        L"-HV", L"2021",
        L"-I", shaderDirectory.c_str()
    };

    IDxcResult* compileResult = nullptr;

    result = m_compiler->Compile(&sourceBuffer, arguments, static_cast<UINT32>(sizeof(arguments) / sizeof(arguments[0])), includeHandler, IID_PPV_ARGS(&compileResult));

    includeHandler->Release();

    if(FAILED(result)){
        source->Release();
        return false;
    }

    HRESULT compileStatus = S_OK;
    result = compileResult->GetStatus(&compileStatus);

    if(FAILED(result)){
        Log::Error("Failed to retrieve DXC shader compilation status (GetStatus). HRESULT: " + std::to_string(result));
        compileResult->Release();
        source->Release();
        return false;
    }

    IDxcBlobUtf8* errors = nullptr;

    result = compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);

    if(SUCCEEDED(result) && errors && errors->GetStringLength() > 0){
        Log::Error(errors->GetStringPointer());
    }

    if(errors){
        errors->Release();
    }

    if(FAILED(compileStatus)){
        compileResult->Release();
        source->Release();
        return false;
    }

    IDxcBlob* object = nullptr;

    result = compileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);

    if(FAILED(result)){
        compileResult->Release();
        source->Release();
        return false;
    }

    const char* bytecodeBegin = static_cast<const char*>(object->GetBufferPointer());
    const char* bytecodeEnd = bytecodeBegin + object->GetBufferSize();

    output.bytecode.assign(bytecodeBegin, bytecodeEnd);

    object->Release();

    compileResult->Release();
    source->Release();

    return true;
}


bool ShaderCompiler::CompileLibrary(
    const ShaderLibraryCompileDescription& description,
    ShaderLibraryCompileResult& output)
{
    output.bytecode.clear();

    if(!m_utils || !m_compiler){
        return false;
    }

    if(!description.path){
        return false;
    }

    IDxcBlobEncoding* source = nullptr;

    HRESULT result = m_utils->LoadFile(
        description.path,
        nullptr,
        &source);

    if(FAILED(result)){
        return false;
    }

    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr = source->GetBufferPointer();
    sourceBuffer.Size = source->GetBufferSize();
    sourceBuffer.Encoding = DXC_CP_UTF8;

    IDxcIncludeHandler* includeHandler = nullptr;

    result = m_utils->CreateDefaultIncludeHandler(&includeHandler);

    if(FAILED(result)){
        source->Release();
        return false;
    }

    std::filesystem::path shaderPath = description.path;
    std::wstring shaderDirectory = shaderPath.parent_path().wstring();

    const wchar_t* arguments[] =
    {
        description.path,
        L"-T", L"lib_6_6",
        L"-HV", L"2021",
        L"-I", shaderDirectory.c_str()
    };

    IDxcResult* compileResult = nullptr;

    result = m_compiler->Compile(
        &sourceBuffer,
        arguments,
        static_cast<UINT32>(
            sizeof(arguments) / sizeof(arguments[0])),
        includeHandler,
        IID_PPV_ARGS(&compileResult));

    includeHandler->Release();

    if(FAILED(result)){
        source->Release();
        return false;
    }

    HRESULT compileStatus = S_OK;
    result = compileResult->GetStatus(&compileStatus);

    if(FAILED(result)){
        Log::Error("Failed to retrieve DXC shader-library compilation status (GetStatus). HRESULT: " + std::to_string(result));
        compileResult->Release();
        source->Release();
        return false;
    }

    IDxcBlobUtf8* errors = nullptr;

    result = compileResult->GetOutput(
        DXC_OUT_ERRORS,
        IID_PPV_ARGS(&errors),
        nullptr);

    if(SUCCEEDED(result) &&
       errors &&
       errors->GetStringLength() > 0)
    {
        Log::Error(errors->GetStringPointer());
    }

    if(errors){
        errors->Release();
    }

    if(FAILED(compileStatus)){
        compileResult->Release();
        source->Release();
        return false;
    }

    IDxcBlob* object = nullptr;

    result = compileResult->GetOutput(
        DXC_OUT_OBJECT,
        IID_PPV_ARGS(&object),
        nullptr);

    if(FAILED(result)){
        compileResult->Release();
        source->Release();
        return false;
    }

    const char* bytecodeBegin =
        static_cast<const char*>(object->GetBufferPointer());

    const char* bytecodeEnd =
        bytecodeBegin + object->GetBufferSize();

    output.bytecode.assign(
        bytecodeBegin,
        bytecodeEnd);

    object->Release();
    compileResult->Release();
    source->Release();

    return true;
}

const wchar_t* ShaderCompiler::GetShaderProfile(ShaderStage stage) const
{
    switch(stage){
        case ShaderStage::Vertex:
            return L"vs_6_6";
        case ShaderStage::Pixel:
            return L"ps_6_6";
        case ShaderStage::Compute:
            return L"cs_6_6";
    }

    return nullptr;
}
