#pragma once

#include "Scene/SceneDocument.h"
#include "Scene/SceneEnvironmentResolution.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// W4: a World opened in the Editor as one transient composite document (Engine_World_Local_Level_Architecture §12.1, [C6]).
//
// The composite is the input of the existing FileSceneRuntime, staging and S0 residency path; it is never saved, and the World and
// Local Level documents it is built from are never changed. It holds, in this order:
//   - the World's own entities;
//   - for each member in World membership order, one synthetic, mesh-less level root carrying the member's placement, followed by
//     the Local Level's entities in document order, its root entities parented to the level root.
// Hierarchy composition then gives each Local entity placement(level) x local transform with the existing conventions.
//
// Runtime identity: entity ids are only unique within their own document, so every entity's runtime id is
// "<owning level asset id>/<original id>". A level root's runtime id is the bare level asset id: it contains no '/', so it can never
// equal a namespaced entity id. CompositeWorld::Find maps a runtime id back to its level and original id.
//
// Frame-wide policies (approved W4 decisions A1 and B1):
//   - Environment: the World's own values (the composite has every override enabled and no owner, so resolution gives the World's
//     environment). Each Local Level's resolved environment is kept for read-only inspection only.
//   - Sun: the first enabled World-owned Directional Light; otherwise the first enabled Local-owned one in membership order, then
//     document order; otherwise none. Every other enabled Directional Light is disabled in the composite copy (never in its source
//     document), so it cannot become an additional directional light.
//   - Main Camera: the first Main Camera in the same order keeps the designation; other Main Cameras lose it in the composite copy.
struct CompositeWorldSource
{
    SceneDocument document;
    std::filesystem::path relativePath;
    // The exact file bytes the document was read from: the save baseline and the external-modification check.
    std::string bytes;
};

struct CompositeWorld
{
    struct Level
    {
        std::string id;
        std::string name;
        std::filesystem::path relativePath;
        ScenePlacement placement;
        // The Local Level's own resolved environment (W3 rules, against this World). Inspection only: the frame uses the World's.
        EffectiveSceneEnvironment environment;
        // The groups in which that environment differs from the World's (empty when none): what the Local Level would show
        // standalone but does not in the World preview.
        std::string differencesFromWorld;
        std::size_t entityCount = 0;
        std::size_t lightCount = 0;
    };

    enum class EntityKind
    {
        World,
        Local,
        LevelRoot
    };

    struct Entity
    {
        EntityKind kind = EntityKind::Local;
        // The owning level's asset id (the World's for World entities and the Local Level's for its root).
        std::string levelID;
        // The id in the owning document; empty for a level root.
        std::string originalID;
    };

    struct LightReference
    {
        std::string levelID;
        std::string originalID;
    };

    std::string worldID;
    std::string worldName;
    std::filesystem::path worldRelativePath;
    // The frame-wide environment (A1): the World's own values.
    EffectiveSceneEnvironment worldEnvironment;
    std::size_t lightCount = 0;
    // The scene light limit that applies (64, or 65 when any level has a Directional Light).
    std::size_t lightCapacity = 0;
    // Project scenes the loader could not read, so their membership could not be checked against this World (a double claim by
    // an unreadable World cannot be ruled out). Empty when every project scene was read.
    std::vector<std::string> unverifiedScenes;
    SceneDocument document;
    std::vector<Level> levels;
    // Parallel to document.objects.
    std::vector<Entity> entities;
    std::unordered_map<std::string, std::size_t> runtimeIndex;
    std::optional<LightReference> sun;
    // Enabled Directional Lights disabled in the composite copy because another light is the sun (B1).
    std::vector<LightReference> suppressedDirectionalLights;
    std::vector<LightReference> suppressedMainCameras;

    // W5: the authoritative source documents as last read or saved (World, then each Local Level in `levels` order). Projection
    // compares against them for per-document dirty state; Save replaces a baseline only after its file was committed.
    CompositeWorldSource worldSource;
    std::vector<CompositeWorldSource> localSources;

    const Entity* Find(std::string_view runtimeID) const;
    static std::string RuntimeID(std::string_view levelID, std::string_view originalID);
};

// Validates the World and its members and builds the composite. `locals` must hold one source per World member (any order). Fails,
// leaving `composite` unchanged, on any invalid document, membership mismatch, cross-level parent reference, light capacity overflow
// or composite validation failure; nothing is skipped silently.
bool BuildCompositeWorld(const CompositeWorldSource& world, const std::vector<CompositeWorldSource>& locals, CompositeWorld& composite,
    std::string& error);

// W5: the inverse of BuildCompositeWorld. Splits an edited composite document back into the World and Local Level documents:
//   - entities go to their owning level with the runtime namespace removed (a parent that is the level root becomes "no parent");
//   - level roots become the World's member placements (their scale must be exactly 1; roots are never persisted);
//   - the composite's scene-level settings are the World's; each Local Level keeps its own scene-level settings from its baseline;
//   - fields the composite policy changed (suppressed suns' `enabled`, suppressed Main Cameras' `main`) revert to the source value.
// Project(Build(sources)) reproduces the sources. Fails on any object that does not belong to a known level.
struct CompositeWorldProjection
{
    SceneDocument world;
    // In CompositeWorld::levels order.
    std::vector<SceneDocument> locals;
};

bool ProjectCompositeDocuments(const CompositeWorld& composite, const SceneDocument& edited, CompositeWorldProjection& projection,
    std::string& error);

// W5: a candidate composite produced by an edit keeps every ownership rule: it projects (no cross-level parent, no object outside a
// level, level roots carry only a placement without scale), no level root was deleted, and the policy-suppressed sun and Main
// Camera flags were not changed.
bool ValidateCompositeEdit(const CompositeWorld& composite, const SceneDocument& candidate, std::string& error);

// Which documents differ from their baselines: index 0 is the World, index i + 1 is levels[i].
std::vector<bool> DirtyCompositeDocuments(const CompositeWorld& composite, const CompositeWorldProjection& projection);
