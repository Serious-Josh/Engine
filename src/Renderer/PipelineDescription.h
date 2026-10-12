#pragma once

#include <d3d12.h>

enum class RootParameterType
{
    Constants,
    ConstantBuffer,
    ShaderResource,
    DescriptorTable
};

enum class DescriptorRangeType
{
    ShaderResource,
    UnorderedAccess
};

struct RootParameterDescription
{
    RootParameterType type = RootParameterType::Constants;

    unsigned int shaderRegister = 0;
    unsigned int constantCount = 0;

    D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL;

    DescriptorRangeType descriptorRangeType = DescriptorRangeType::ShaderResource;

    unsigned int descriptorCount = 1;
};
