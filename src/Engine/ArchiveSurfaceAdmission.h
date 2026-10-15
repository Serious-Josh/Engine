#pragma once
#include "assets/ProjectMaterialPreparationService.h"
#include <functional>
class Renderer;
class FileSceneAssetCatalog;
class FileSceneRuntime;
class ArchiveSurfaceAdmission
{
public:
    // Startup admission driver shared with diagnostics; uses existing tracked frame/upload machinery.
    static void Admit(Renderer&,ProjectMaterialPreparationService&,FileSceneAssetCatalog&,FileSceneRuntime&, const std::function<void()>& checkCancellation = {});
};
