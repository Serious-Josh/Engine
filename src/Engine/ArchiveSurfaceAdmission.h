#pragma once
#include "assets/ProjectMaterialPreparationService.h"
#include <functional>
class Renderer;
class FileSceneAssetCatalog;
class FileSceneRuntime;
class ArchiveSurfaceAdmission
{
  public:
    // Synchronous Engine-thread startup/diagnostic admission using tracked upload/table progress.
    // Renderer, preparation, catalog and runtime must remain alive; callbacks must not reenter
    // submission or replace those owners. Failures retain Renderer-owned GPU resources for
    // existing checked teardown; this driver adds no automatic abort or rollback.
    static void Admit(Renderer&, ProjectMaterialPreparationService&, FileSceneAssetCatalog&,
                      FileSceneRuntime&, const std::function<void()>& checkCancellation = {});
};
