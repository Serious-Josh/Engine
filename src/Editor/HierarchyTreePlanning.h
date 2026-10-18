#pragma once

#include "HierarchyDragPayload.h"
#include "Scene/SceneHierarchySnapshot.h"
#include "Scene/SceneHierarchy.h"
#include <unordered_set>

struct HierarchyTreeRow
{
    size_t documentIndex = 0;
    size_t depth = 0;
};

// Derived UI data only. Mutation and transform validation remain H.5's responsibility.
inline bool BuildHierarchyTree(const SceneHierarchySnapshot& snapshot, SceneHierarchy::Forest& forest,
    std::string& error)
{
    std::vector<SceneObjectDocument> objects;
    objects.reserve(snapshot.entries.size());

    for(size_t index = 0; index < snapshot.entries.size(); ++index)
    {
        const auto& entry = snapshot.entries[index];
        SceneObjectDocument object;
        object.id = entry.objectID.empty() ? "procedural:" + std::to_string(index) : entry.objectID;
        object.parentID = entry.parentID;
        objects.push_back(std::move(object));
    }

    SceneHierarchy::Policy policy;

    if(std::all_of(objects.begin(), objects.end(), [](const auto& object) { return object.parentID.empty(); }))
    {
        policy.maxObjects = (std::max)(policy.maxObjects, objects.size());
    }

    return SceneHierarchy::Build(objects, forest, &error, policy);
}

inline std::vector<HierarchyTreeRow> VisibleHierarchyRows(const SceneHierarchy::Forest& forest,
    const std::unordered_set<std::string>& collapsed)
{
    std::vector<HierarchyTreeRow> rows;
    std::vector<HierarchyTreeRow> pending;

    for(auto root = forest.roots.rbegin(); root != forest.roots.rend(); ++root)
    {
        pending.push_back({*root, 0});
    }

    while(!pending.empty())
    {
        const auto row = pending.back();
        pending.pop_back();
        rows.push_back(row);

        if(collapsed.contains(forest.objectIDs[row.documentIndex]))
        {
            continue;
        }

        const auto& children = forest.children[row.documentIndex];

        for(auto child = children.rbegin(); child != children.rend(); ++child)
        {
            pending.push_back({*child, row.depth + 1});
        }
    }

    return rows;
}

// Translate visual zones into H.5's parent/before-sibling arguments. Never edits a document.
inline bool ResolveHierarchyDrop(const SceneHierarchySnapshot& snapshot, const HierarchyDropRequest& request,
    std::string& parentID, std::string& beforeSibling, std::string& error)
{
    const auto find = [&](const std::string& id)
    {
        return std::find_if(snapshot.entries.begin(), snapshot.entries.end(),
            [&](const auto& entry) { return entry.objectID == id; });
    };

    if(request.sourceID.empty() || find(request.sourceID) == snapshot.entries.end())
    {
        error = "Drag source no longer exists.";
        return false;
    }

    parentID.clear();
    beforeSibling.clear();

    if(request.zone == HierarchyDropZone::Root)
    {
        return true;
    }

    const auto target = find(request.targetID);

    if(target == snapshot.entries.end() || request.targetID == request.sourceID)
    {
        error = "Choose a different existing target.";
        return false;
    }

    if(request.zone == HierarchyDropZone::Parent)
    {
        parentID = target->objectID;
    }
    else
    {
        parentID = target->parentID;

        if(request.zone == HierarchyDropZone::Before)
        {
            beforeSibling = target->objectID;
        }
        else
        {
            for(auto next = target + 1; next != snapshot.entries.end(); ++next)
            {
                if(next->parentID == parentID && next->objectID != request.sourceID)
                {
                    beforeSibling = next->objectID;
                    break;
                }
            }
        }
    }

    return true;
}
