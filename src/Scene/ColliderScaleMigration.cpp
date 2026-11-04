#include "Scene/ColliderScaleMigration.h"

#include "Core/Log.h"
#include "Physics/ColliderScaling.h"
#include "Scene/SceneDocument.h"

namespace
{
template <typename Component>
void MigrateCollider(ColliderScaleMigrationReport& report, const SceneObjectDocument& object, Component& collider,
    const char* componentName)
{
    std::string detail;
    const auto outcome = Physics::LocalizeStoredCollider(collider, object.transform.scale, detail);
    const std::string subject = "'" + object.name + "' (" + object.id + ") " + componentName + ": ";

    switch(outcome)
    {
    case Physics::ColliderMigrationOutcome::Exact:
        ++report.exactColliders;
        break;
    case Physics::ColliderMigrationOutcome::Nearest:
        ++report.nearestColliders;
        report.exceptions.push_back(subject + detail + ".");
        break;
    case Physics::ColliderMigrationOutcome::Unconvertible:
        ++report.unconvertedColliders;
        report.exceptions.push_back(subject + detail + "; kept as stored, so its size now follows the object's scale.");
        break;
    }
}
}

ColliderScaleMigrationReport MigrateStoredColliderScale(SceneDocument& document)
{
    ColliderScaleMigrationReport report;

    for(SceneObjectDocument& object : document.objects)
    {
        if(object.physics)
        {
            MigrateCollider(report, object, *object.physics, "Physics collider");
        }

        if(object.triggerCollider)
        {
            MigrateCollider(report, object, *object.triggerCollider, "Trigger Collider");
        }
    }

    return report;
}

void LogColliderScaleMigration(const SceneDocument& document, const ColliderScaleMigrationReport& report,
    std::string_view source)
{
    const std::size_t total = report.exactColliders + report.nearestColliders + report.unconvertedColliders;

    if(total == 0)
    {
        return;
    }

    Log::Info("Scene '" + document.name + "' (" + std::string(source) + "): collider dimensions converted to "
        "object-local values; " + std::to_string(report.exactColliders) + " exact, " +
        std::to_string(report.nearestColliders) + " nearest float, " + std::to_string(report.unconvertedColliders) +
        " kept as stored." + (source.starts_with("schema") ? " Save to persist." : ""));

    for(const std::string& exception : report.exceptions)
    {
        Log::Warning("Scene '" + document.name + "' collider scale migration: " + exception);
    }
}
