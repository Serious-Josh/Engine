#pragma once

#include "assets/AcousticMaterial.h"
#include "assets/ImportedMesh.h"
#include "Audio/AudioSystem.h"
#include "Scene/AudioComponents.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// M4: from Engine's meshes and authored acoustic settings to the audio module's plain, immutable geometry. Main thread.
namespace AcousticGeometry
{
// The stable identity of one submesh within its mesh: its position and its name together. A reimport that reorders or renames
// submeshes therefore leaves the assignment unmatched (and reported), never silently on the wrong surface.
std::string SubmeshKey(const MeshSubmesh& submesh, std::size_t index);

struct BuildReport
{
    // Why this mesh contributes nothing; empty on success.
    std::string error;
    // Readable notes: submeshes falling back to the default material, assignments matching no submesh.
    std::vector<std::string> warnings;
};

using MaterialResolver = std::function<std::optional<AcousticMaterialData>(const std::string& reference, std::string* error)>;

// Engine-space geometry with one material per submesh. Null, with report.error, for missing, empty or invalid data. Degenerate
// triangles are dropped; they can't occlude anything.
std::shared_ptr<const Audio::AcousticMeshData> Build(const ImportedMesh& mesh, const AcousticSettings& settings,
    const MaterialResolver& resolveMaterial, BuildReport& report);
}
