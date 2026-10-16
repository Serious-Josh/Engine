#pragma once
#include "assets/ImportedMesh.h"
#include <filesystem>
#include <string>

class ProjectPaths;
class ProjectAssetRegistry;
enum class ScenePrimitive { Cube, Sphere };
namespace PrimitiveMeshAssets
{
const char* Name(ScenePrimitive primitive);
ImportedMesh Generate(ScenePrimitive primitive);
// Ordinary registered FBX sources, shared per project; no renderer resources are created here.
bool Ensure(const ProjectPaths&, ProjectAssetRegistry&, ScenePrimitive, std::string& reference, std::string& error);
}
