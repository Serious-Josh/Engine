#pragma once

#include <string_view>

class Mesh;
struct Material;

class SceneAssetResolver
{
public:
    virtual ~SceneAssetResolver() = default;

    virtual Mesh* ResolveMesh(
        std::string_view reference) = 0;

    virtual Material* ResolveMaterial(
        std::string_view reference) = 0;
};