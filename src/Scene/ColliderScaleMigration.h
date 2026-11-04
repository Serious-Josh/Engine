#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

struct SceneDocument;

// Scenes before collider scaling (JSON schema < SceneDocument::kColliderScaleSchemaVersion, cooked payload
// < CookedScene::ColliderScaleVersion) stored Physics and Trigger Collider dimensions and offsets as absolute
// meters, ignoring the object's scale. Loading such a scene converts each collider to object-local values whose
// effective shape at the object's scale (ColliderScaling.h) reproduces what was stored, so the collision geometry is
// preserved. Readers run this only for older versions and saves write the current version, so repeated load/save
// never converts twice. Character Controllers are not touched.
struct ColliderScaleMigrationReport
{
    // Reproduced bit for bit.
    std::size_t exactColliders = 0;
    // Converted to the nearest float (within one unit in the last place of a stored value); listed in exceptions.
    std::size_t nearestColliders = 0;
    // No object-local value exists (zero or nonfinite scale); kept as stored and listed in exceptions.
    std::size_t unconvertedColliders = 0;
    std::vector<std::string> exceptions;
};

ColliderScaleMigrationReport MigrateStoredColliderScale(SceneDocument& document);

// Logs the conversion (Info) and every exception (Warning) for one loaded scene.
void LogColliderScaleMigration(const SceneDocument& document, const ColliderScaleMigrationReport& report,
    std::string_view source);
