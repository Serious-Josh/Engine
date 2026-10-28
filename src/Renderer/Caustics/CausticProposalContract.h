#pragma once

#include "CausticResourceContracts.h"

#include <algorithm>
#include <numbers>

namespace CausticContracts
{
    constexpr double MinimumSphereProbability = 1.0 / 16.0;

    struct Direction
    {
        double x = 0;
        double y = 0;
        double z = 1;
    };

    struct WorldSphere
    {
        CausticPublication publication{};
        Direction center{};
        double radius = 0;
        bool enclosureVerified = false;
    };

    struct Proposal
    {
        Direction axis{};
        double cosineHalfAngle = -1;
        double inverseSolidAngle = 1 / (4 * std::numbers::pi);
        double sphereProbability = 0;
    };

    inline bool BuildProposal(const WorldSphere& bound, Direction light, CausticPublication current,
        double sphereProbability, Proposal& output)
    {
        if(!bound.enclosureVerified || !CausticValidPublication(current) ||
            !CausticSamePublication(bound.publication, current) || !std::isfinite(bound.radius) ||
            bound.radius <= 0 || !std::isfinite(sphereProbability) ||
            sphereProbability < MinimumSphereProbability || sphereProbability > 1)
        {
            return false;
        }

        const Direction offset{bound.center.x - light.x, bound.center.y - light.y, bound.center.z - light.z};
        const double distance = std::hypot(offset.x, offset.y, offset.z);

        if(!std::isfinite(distance) || !std::isfinite(light.x) || !std::isfinite(light.y) || !std::isfinite(light.z))
        {
            return false;
        }

        Proposal proposal;
        proposal.sphereProbability = sphereProbability;

        if(distance > bound.radius)
        {
            proposal.axis = {offset.x / distance, offset.y / distance, offset.z / distance};
            const double ratio = bound.radius / distance;
            const double sineSquared = ratio * ratio;
            const double cosine = std::sqrt(std::max(0.0, 1 - sineSquared));
            // Stable 1-cos(theta), avoiding catastrophic cancellation for small reflectors.
            const double width = sineSquared / (1 + cosine);

            if(width > 0 && 1 - width < 1 && std::isfinite(1 / (2 * std::numbers::pi * width)))
            {
                proposal.cosineHalfAngle = 1 - width;
                // Density must match the representable cap actually sampled, not ideal width.
                proposal.inverseSolidAngle = 1 / (2 * std::numbers::pi * (1 - proposal.cosineHalfAngle));
            }
            // Unrepresentably small caps safely become full-sphere proposals.
        }

        output = proposal;
        return true;
    }

    inline double CapDensity(const Proposal& proposal, Direction unitDirection)
    {
        if(proposal.cosineHalfAngle == -1)
        {
            return proposal.inverseSolidAngle;
        }

        const double cosine = proposal.axis.x * unitDirection.x + proposal.axis.y * unitDirection.y +
            proposal.axis.z * unitDirection.z;
        return cosine >= proposal.cosineHalfAngle ? proposal.inverseSolidAngle : 0;
    }

    inline double EmissionDensity(const Proposal& proposal, Direction unitDirection)
    {
        // One selected light (p_L=1) and one proposal (b_0=1). This is per steradian.
        return proposal.sphereProbability / (4 * std::numbers::pi) +
            (1 - proposal.sphereProbability) * CapDensity(proposal, unitDirection);
    }
}
