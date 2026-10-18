#pragma once

#include "Scene/SceneDocument.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <unordered_map>
#include <utility>

namespace SceneHierarchy
{
    struct Policy
    {
        // Matches the existing default cooked-scene admission limits.
        size_t maxObjects = 8192;
        size_t maxParentIDBytes = 4096;
    };

    struct Forest
    {
        // Indices address this document snapshot, never runtime/render slots.
        std::vector<std::string> objectIDs;
        std::vector<std::string> parentIDs;
        std::vector<std::optional<size_t>> parents;
        std::vector<std::vector<size_t>> children;
        std::vector<size_t> roots;
        std::vector<size_t> parentBeforeChild;
        bool operator==(const Forest&) const = default;
    };

    inline bool Fail(std::string* error, std::string message)
    {
        if(error)
        {
            *error = std::move(message);
        }

        return false;
    }

    // Value-only shared seam: no assets, renderer, scene pointers or recursive traversal.
    // Build everything before replacing output, including on rejected cyclic candidates.
    inline bool Build(std::span<const SceneObjectDocument> objects, Forest& output,
        std::string* error = nullptr, const Policy& policy = {})
    {
        if(error)
        {
            error->clear();
        }

        if(objects.size() > policy.maxObjects)
        {
            return Fail(error, "Hierarchy exceeds the admitted object count.");
        }

        Forest candidate;
        candidate.parents.resize(objects.size());
        candidate.children.resize(objects.size());
        candidate.objectIDs.reserve(objects.size());
        candidate.parentIDs.reserve(objects.size());
        std::unordered_map<std::string, size_t> indices;
        indices.reserve(objects.size());

        for(size_t index = 0; index < objects.size(); ++index)
        {
            const auto& object = objects[index];

            if(object.id.empty() || !indices.emplace(object.id, index).second)
            {
                return Fail(error, "Hierarchy requires unique nonempty object IDs: " + object.id);
            }

            if(object.parentID.size() > policy.maxParentIDBytes || object.parentID.find('\0') != std::string::npos)
            {
                return Fail(error, "Invalid hierarchy parent reference for object: " + object.id);
            }

            if(!object.parentID.empty() && (object.physics || object.triggerCollider || object.characterController))
            {
                return Fail(error, "Physics, Trigger Collider and Character Controller require a root object: " + object.id);
            }

            candidate.objectIDs.push_back(object.id);
            candidate.parentIDs.push_back(object.parentID);
        }

        for(size_t index = 0; index < objects.size(); ++index)
        {
            const auto& object = objects[index];

            if(object.parentID.empty())
            {
                candidate.roots.push_back(index);
                continue;
            }

            const auto parent = indices.find(object.parentID);

            if(parent == indices.end() || parent->second == index)
            {
                return Fail(error, "Missing or self hierarchy parent for object: " + object.id);
            }

            candidate.parents[index] = parent->second;
            candidate.children[parent->second].push_back(index);
        }

        // Reverse stack insertion produces deterministic preorder without recursion.
        // Every node has at most one incoming edge. Unvisited nodes imply a cycle.
        std::vector<size_t> pending(candidate.roots.rbegin(), candidate.roots.rend());
        candidate.parentBeforeChild.reserve(objects.size());

        while(!pending.empty())
        {
            const size_t index = pending.back();
            pending.pop_back();
            candidate.parentBeforeChild.push_back(index);
            const auto& children = candidate.children[index];
            pending.insert(pending.end(), children.rbegin(), children.rend());
        }

        if(candidate.parentBeforeChild.size() != objects.size())
        {
            return Fail(error, "Hierarchy contains a parent cycle.");
        }

        output = std::move(candidate);
        return true;
    }

    inline bool RequireFlat(std::span<const SceneObjectDocument> objects, std::string* error = nullptr)
    {
        for(const auto& object : objects)
        {
            if(!object.parentID.empty())
            {
                return Fail(error, "Hierarchy relationships are internal-only until world evaluation and persistence are implemented: " + object.id);
            }
        }

        return true;
    }

    class State
    {
    public:
        bool Update(std::span<const SceneObjectDocument> objects, std::string* error = nullptr)
        {
            Forest candidate;
            Policy policy;

            if(std::all_of(objects.begin(), objects.end(),
                [](const SceneObjectDocument& object) { return object.parentID.empty(); }))
            {
                policy.maxObjects = (std::max)(policy.maxObjects, objects.size());
            }

            if(!Build(objects, candidate, error, policy))
            {
                return false;
            }

            if(m_revision != 0 && candidate == m_forest)
            {
                return true;
            }

            if(m_revision == (std::numeric_limits<std::uint64_t>::max)())
            {
                return Fail(error, "Hierarchy revision is exhausted.");
            }

            m_forest = std::move(candidate);
            ++m_revision;
            return true;
        }

        // Scene replacement leaves no admitted forest until the next commit rebuilds it.
        // The revision still advances so a cached revision from the previous scene cannot match.
        void Reset() noexcept
        {
            m_forest = {};

            if(m_revision != (std::numeric_limits<std::uint64_t>::max)())
            {
                ++m_revision;
            }
        }

        const Forest& GetForest() const noexcept
        {
            return m_forest;
        }

        std::uint64_t GetRevision() const noexcept
        {
            return m_revision;
        }

    private:
        Forest m_forest;
        std::uint64_t m_revision = 0;
    };
}
