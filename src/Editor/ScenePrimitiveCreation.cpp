#include "ScenePrimitiveCreation.h"
#include "SceneObjectDuplication.h"
#include "assets/AssetID.h"
#include <algorithm>
#include <stdexcept>

namespace
{
// Shared by primitive and empty creation: the existing duplicate-name convention and a
// fresh UUID not used by any object in the document.
void AssignNewObjectIdentity(const SceneDocument& document, SceneObjectDocument& object, const char* objectKind)
{
    const bool nameTaken = std::any_of(document.objects.begin(), document.objects.end(),
        [&](const SceneObjectDocument& existing)
        {
            return existing.name == object.name;
        });

    if(nameTaken)
    {
        object.name = UniqueDuplicateObjectName(object.name, document);
    }

    constexpr unsigned int maximumAttempts = 8;

    for(unsigned int attempt = 0; attempt < maximumAttempts && object.id.empty(); ++attempt)
    {
        const auto id = AssetID::Generate();

        if(!id)
        {
            continue;
        }

        const bool idTaken = std::any_of(document.objects.begin(), document.objects.end(),
            [&](const SceneObjectDocument& existing)
            {
                return existing.id == id->ToString();
            });

        if(!idTaken)
        {
            object.id = id->ToString();
        }
    }

    if(object.id.empty())
    {
        throw std::runtime_error(std::string("Cannot allocate a fresh ") + objectKind + " object identity.");
    }
}
}

SceneObjectDocument PreparePrimitiveObject(const SceneDocument& document, ScenePrimitive primitive,
    const std::string& meshReference)
{
    const auto meshID=AssetID::ParseReference(meshReference);
    if(!meshID || meshID->ToReference()!=meshReference) throw std::invalid_argument("Primitive requires a canonical mesh AssetID.");
    SceneObjectDocument object;
    object.name=PrimitiveMeshAssets::Name(primitive);
    AssignNewObjectIdentity(document, object, "primitive");
    object.meshReference=meshReference;
    object.materialReference="builtin:default";
    // SceneObjectDocument defaults provide identity transform and absent physics.
    return object;
}

SceneObjectDocument PrepareEmptyObject(const SceneDocument& document)
{
    SceneObjectDocument object;
    object.name = "Empty Object";
    AssignNewObjectIdentity(document, object, "empty");

    // Defaults already give an identity transform, empty mesh/material references, no
    // slot bindings and no physics; nothing else is assigned.
    return object;
}
