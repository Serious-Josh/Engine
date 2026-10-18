#pragma once

#include "Scene/SceneHierarchyTransforms.h"
#include "Scene/SceneDocumentAdapter.h"
#include "assets/AssetID.h"
#include <unordered_set>

// Document-only transaction payload. History owns no runtime or GPU resources.
struct SceneHierarchyEdit
{
    SceneDocument before;
    SceneDocument after;
    std::map<std::string, std::string> bindingSources;
};

namespace SceneHierarchy
{
    inline bool SameObjects(const SceneDocument& expected, const SceneDocument& current)
    {
        auto comparison = current;
        comparison.objects = expected.objects;
        return SceneDocumentsEqual(comparison, current);
    }

    inline bool Subtree(const SceneDocument& document, std::string_view root,
        std::unordered_set<std::string>& ids, std::string& error)
    {
        Forest forest;

        if(!ValidateSceneDocument(document, &error) || !Build(document.objects, forest, &error))
        {
            return false;
        }

        const auto found = std::find(forest.objectIDs.begin(), forest.objectIDs.end(), root);

        if(found == forest.objectIDs.end())
        {
            return Fail(&error, "Subtree root no longer exists.");
        }

        std::vector<size_t> pending{static_cast<size_t>(found - forest.objectIDs.begin())};
        std::unordered_set<std::string> candidate;

        while(!pending.empty())
        {
            const auto index = pending.back();
            pending.pop_back();
            candidate.insert(document.objects[index].id);
            pending.insert(pending.end(), forest.children[index].begin(), forest.children[index].end());
        }

        ids.swap(candidate);
        return true;
    }

    inline bool DeleteSubtree(const SceneDocument& before, std::string_view root,
        SceneDocument& after, std::string& error)
    {
        std::unordered_set<std::string> ids;

        if(!Subtree(before, root, ids, error))
        {
            return false;
        }

        auto candidate = before;
        std::erase_if(candidate.objects, [&](const auto& object) { return ids.contains(object.id); });

        if(!ValidateSceneDocument(candidate, &error))
        {
            return false;
        }

        after = std::move(candidate);
        return true;
    }

    inline std::string DuplicateName(std::string_view name, const SceneDocument& document)
    {
        std::string base(name);
        const auto open = base.rfind('(');

        if(open != std::string::npos && base.back() == ')' && open + 2 < base.size() &&
            std::all_of(base.begin() + open + 1, base.end() - 1, [](char value) { return value >= '0' && value <= '9'; }))
        {
            base.resize(open);

            if(!base.empty() && base.back() == ' ')
            {
                base.pop_back();
            }

            if(base.empty())
            {
                base = "Object";
            }
        }

        std::unordered_set<std::string> names;

        for(const auto& object : document.objects)
        {
            names.insert(object.name);
        }

        for(size_t suffix = 2;; ++suffix)
        {
            auto candidate = base + " (" + std::to_string(suffix) + ")";

            if(!names.contains(candidate))
            {
                return candidate;
            }
        }
    }

    inline bool DuplicateSubtree(const SceneDocument& before, std::string_view root,
        SceneDocument& after, std::string& duplicateRoot, std::string& error,
        std::map<std::string, std::string>* bindingSources = nullptr)
    {
        std::unordered_set<std::string> ids;

        if(!Subtree(before, root, ids, error))
        {
            return false;
        }

        std::unordered_set<std::string> used;
        std::unordered_map<std::string, std::string> remap;

        for(const auto& object : before.objects)
        {
            used.insert(object.id);
        }

        for(const auto& object : before.objects)
        {
            if(!ids.contains(object.id))
            {
                continue;
            }

            std::string fresh;

            do
            {
                const auto generated = AssetID::Generate();

                if(!generated)
                {
                    return Fail(&error, "Cannot generate subtree duplicate identity.");
                }

                fresh = generated->ToString();
            }
            while(!used.insert(fresh).second);

            remap.emplace(object.id, std::move(fresh));
        }

        auto candidate = before;
        size_t insertion = 0;

        for(size_t index = 0; index < before.objects.size(); ++index)
        {
            if(ids.contains(before.objects[index].id))
            {
                insertion = index + 1;
            }
        }

        std::vector<SceneObjectDocument> copies;
        auto naming = before;
        std::map<std::string, std::string> donors;

        for(const auto& object : before.objects)
        {
            if(!ids.contains(object.id))
            {
                continue;
            }

            auto copy = object;
            copy.id = remap.at(object.id);
            copy.name = DuplicateName(copy.name, naming);
            naming.objects.push_back(copy);
            donors.emplace(copy.id, object.id);

            if(const auto parent = remap.find(copy.parentID); parent != remap.end())
            {
                copy.parentID = parent->second;
            }

            copies.push_back(std::move(copy));
        }

        candidate.objects.insert(candidate.objects.begin() + insertion, copies.begin(), copies.end());

        if(!ValidateSceneDocument(candidate, &error))
        {
            return false;
        }

        auto selected = remap.at(std::string(root));
        after = std::move(candidate);
        duplicateRoot = std::move(selected);

        if(bindingSources)
        {
            bindingSources->swap(donors);
        }
        return true;
    }

    // beforeSibling is a stable destination sibling ID; empty appends to that sibling list.
    inline bool Reparent(const SceneDocument& before, std::string_view root, std::string_view parent,
        bool keepWorld, std::string_view beforeSibling, SceneDocument& after, std::string& error)
    {
        std::unordered_set<std::string> subtree;

        if(!Subtree(before, root, subtree, error))
        {
            return false;
        }

        if(subtree.contains(std::string(parent)))
        {
            return Fail(&error, "Cannot parent an object to itself or its descendant.");
        }

        Forest forest;
        Build(before.objects, forest, &error);
        std::vector<Matrix4> worlds(before.objects.size());
        std::unordered_map<std::string, size_t> indices;

        for(const auto index : forest.parentBeforeChild)
        {
            indices.emplace(before.objects[index].id, index);
            worlds[index] = before.objects[index].transform.GetModelMatrix();

            if(forest.parents[index] && !TryMultiplyAffine(worlds[*forest.parents[index]], worlds[index], worlds[index]))
            {
                return Fail(&error, "Cannot evaluate authored world transform.");
            }
        }

        if(!parent.empty() && !indices.contains(std::string(parent)))
        {
            return Fail(&error, "Destination parent no longer exists.");
        }

        const auto sourceIndex = indices.at(std::string(root));

        if(before.objects[sourceIndex].parentID == parent)
        {
            const auto& siblings = forest.parents[sourceIndex] ? forest.children[*forest.parents[sourceIndex]] : forest.roots;
            const auto position = std::find(siblings.begin(), siblings.end(), sourceIndex);
            const auto next = position + 1;
            const std::string_view nextID = next == siblings.end() ? std::string_view{} : before.objects[*next].id;

            if(beforeSibling == root || beforeSibling == nextID)
            {
                after = before;
                return true;
            }
        }

        auto candidate = before;
        auto object = candidate.objects[sourceIndex];
        object.parentID = parent;

        if(keepWorld && before.objects[sourceIndex].parentID != parent)
        {
            const Matrix4 parentWorld = parent.empty() ? Matrix4::Identity() : worlds[indices.at(std::string(parent))];

            if(!TryConvertWorldToLocal(parentWorld, worlds[sourceIndex], object.transform, &error))
            {
                return false;
            }
        }

        candidate.objects.erase(candidate.objects.begin() + sourceIndex);
        auto insertion = candidate.objects.end();

        if(!beforeSibling.empty())
        {
            insertion = std::find_if(candidate.objects.begin(), candidate.objects.end(),
                [&](const auto& entry) { return entry.id == beforeSibling && entry.parentID == parent; });

            if(insertion == candidate.objects.end())
            {
                return Fail(&error, "Destination sibling is missing or belongs to another parent.");
            }
        }

        candidate.objects.insert(insertion, std::move(object));

        if(!ValidateSceneDocument(candidate, &error))
        {
            return false;
        }

        after = std::move(candidate);
        return true;
    }
}
