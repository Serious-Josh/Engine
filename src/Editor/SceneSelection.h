#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace SceneSelection
{
    // Persistent IDs for file scenes. Procedural indices are valid only within
    // the owning editor's unchanged scene/structure revision.
    struct Identity
    {
        std::string objectID;
        size_t proceduralIndex = (std::numeric_limits<size_t>::max)();
        bool operator==(const Identity&) const = default;
    };

    inline void Select(std::vector<Identity>& ordered, std::optional<Identity> identity, bool toggle)
    {
        if(!toggle) ordered.clear();
        if(!identity) return;
        const auto found = std::find(ordered.begin(), ordered.end(), *identity);
        if(found != ordered.end()) ordered.erase(found);
        else ordered.push_back(std::move(*identity));
    }
}
