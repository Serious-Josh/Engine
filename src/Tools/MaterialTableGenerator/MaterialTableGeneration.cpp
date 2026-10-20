#include "Tools/MaterialTableGenerator/MaterialTableGeneration.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

namespace MaterialTables
{
    namespace
    {
        constexpr double Pi = 3.14159265358979323846;

        struct Vector
        {
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
        };

        Vector Normalize(const Vector& value)
        {
            const double length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
            return {value.x / length, value.y / length, value.z / length};
        }

        Vector Cross(const Vector& left, const Vector& right)
        {
            return {left.y * right.z - left.z * right.y, left.z * right.x - left.x * right.z,
                left.x * right.y - left.y * right.x};
        }

        double Dot(const Vector& left, const Vector& right)
        {
            return left.x * right.x + left.y * right.y + left.z * right.z;
        }

        // Neumaier-compensated sum; deterministic for a fixed order of additions.
        struct CompensatedSum
        {
            double sum = 0.0;
            double compensation = 0.0;

            void Add(double value)
            {
                const double total = sum + value;

                if(std::abs(sum) >= std::abs(value))
                {
                    compensation += (sum - total) + value;
                }
                else
                {
                    compensation += (value - total) + sum;
                }

                sum = total;
            }

            double Value() const
            {
                return sum + compensation;
            }
        };

        // Heitz 2018 visible-normal sample for the view in the local frame (normal +Z), isotropic alpha.
        // The same mapping as PathTraceSampleVisibleNormal, evaluated in double.
        Vector SampleVisibleNormal(const Vector& view, double alpha, double u1, double u2)
        {
            const Vector stretched = Normalize({alpha * view.x, alpha * view.y, view.z});
            const double lengthSquared = stretched.x * stretched.x + stretched.y * stretched.y;
            const Vector axis1 = lengthSquared > 0.0 ?
                Vector{-stretched.y / std::sqrt(lengthSquared), stretched.x / std::sqrt(lengthSquared), 0.0} :
                Vector{1.0, 0.0, 0.0};
            const Vector axis2 = Cross(stretched, axis1);

            const double radius = std::sqrt(u1);
            const double angle = 2.0 * Pi * u2;
            const double t1 = radius * std::cos(angle);
            double t2 = radius * std::sin(angle);
            const double blend = 0.5 * (1.0 + stretched.z);
            t2 = (1.0 - blend) * std::sqrt(std::max(1.0 - t1 * t1, 0.0)) + blend * t2;

            const double height = std::sqrt(std::max(1.0 - t1 * t1 - t2 * t2, 0.0));
            const Vector stretchedNormal{t1 * axis1.x + t2 * axis2.x + height * stretched.x,
                t1 * axis1.y + t2 * axis2.y + height * stretched.y, t1 * axis1.z + t2 * axis2.z + height * stretched.z};
            return Normalize({alpha * stretchedNormal.x, alpha * stretchedNormal.y, std::max(stretchedNormal.z, 0.0)});
        }
    }

    double SmithLambda(double alphaSquared, double cosine)
    {
        const double cosineSquared = cosine * cosine;
        const double t = alphaSquared * std::max(1.0 - cosineSquared, 0.0) / std::max(cosineSquared, 1.0e-300);
        return t / (2.0 * (1.0 + std::sqrt(1.0 + t)));
    }

    std::array<double, 2> IntegrateMissingAndBias(double cosine, double roughness, std::uint32_t latticeSize)
    {
        const double clampedRoughness = std::max(roughness, MinimumRoughness);
        const double alpha = clampedRoughness * clampedRoughness;
        const double alphaSquared = alpha * alpha;
        const Vector view{std::sqrt(std::max(1.0 - cosine * cosine, 0.0)), 0.0, cosine};
        const double lambdaView = SmithLambda(alphaSquared, cosine);
        CompensatedSum missing;
        CompensatedSum bias;

        // The visible-normal radius coordinate u1 is warped as u1 = 1 - (1 - s)^4, Jacobian 4 (1 - s)^3, so the
        // GGX tail at u1 -> 1 (grazing microfacets, where reflections leave the hemisphere) is resolved.
        for(std::uint32_t row = 0; row < latticeSize; ++row)
        {
            const double s = (row + 0.5) / latticeSize;
            const double complement = 1.0 - s;
            const double u1 = 1.0 - complement * complement * complement * complement;
            const double jacobian = 4.0 * complement * complement * complement;

            for(std::uint32_t column = 0; column < latticeSize; ++column)
            {
                const double u2 = (column + 0.5) / latticeSize;
                const Vector half = SampleVisibleNormal(view, alpha, u1, u2);
                const double viewDotHalf = Dot(view, half);
                const double lightZ = 2.0 * viewDotHalf * half.z - view.z;

                if(lightZ <= 0.0)
                {
                    missing.Add(jacobian);
                    continue;
                }

                const double lambdaLight = SmithLambda(alphaSquared, lightZ);
                const double denominator = 1.0 + lambdaView + lambdaLight;
                const double fresnelWeight = std::pow(1.0 - std::clamp(viewDotHalf, 0.0, 1.0), 5.0);
                missing.Add(jacobian * lambdaLight / denominator);
                bias.Add(jacobian * fresnelWeight * (1.0 + lambdaView) / denominator);
            }
        }

        const double count = static_cast<double>(latticeSize) * latticeSize;
        return {missing.Value() / count, bias.Value() / count};
    }

    std::array<double, 2> IntegrateAverage(const MaterialTableSet& tables, double roughness)
    {
        constexpr std::uint32_t steps = 1u << 20;
        CompensatedSum missing;
        CompensatedSum bias;

        // Midpoint rule in the table coordinate u; mu = u (Linear) or u^2 (Squared), dmu = mu'(u) du.
        const bool squared = tables.header.cosineMapping == static_cast<std::uint32_t>(CosineMapping::Squared);

        for(std::uint32_t step = 0; step < steps; ++step)
        {
            const double coordinate = (step + 0.5) / steps;
            const double cosine = squared ? coordinate * coordinate : coordinate;
            const double jacobian = squared ? 2.0 * coordinate : 1.0;
            const auto value = SampleDirectional(tables, cosine, roughness);
            const double weight = 2.0 * cosine * jacobian / steps;
            missing.Add(value[0] * weight);
            bias.Add(value[1] * weight);
        }

        return {missing.Value(), bias.Value()};
    }

    MaterialTableSet GenerateMaterialTables(const GenerationSettings& settings)
    {
        MaterialTableSet tables;
        tables.header.cosineMapping = static_cast<std::uint32_t>(settings.cosineMapping);
        tables.header.directionalWidth = settings.tableSize;
        tables.header.directionalHeight = settings.tableSize;
        tables.header.averageWidth = settings.tableSize;
        tables.header.minimumRoughness = static_cast<float>(MinimumRoughness);
        tables.header.latticeSize = settings.latticeSize;
        tables.header.generatorVersion = GeneratorVersion;
        tables.directional.assign(static_cast<size_t>(settings.tableSize) * settings.tableSize * 2, 0.0f);
        tables.average.assign(static_cast<size_t>(settings.tableSize) * 2, 0.0f);

        // Rows are claimed dynamically, but each texel's value depends only on its own fixed-order sum.
        std::atomic<std::uint32_t> nextRow{0};
        const auto worker = [&]()
        {
            for(std::uint32_t row = nextRow++; row < settings.tableSize; row = nextRow++)
            {
                const double roughness = (row + 0.5) / settings.tableSize;

                for(std::uint32_t column = 0; column < settings.tableSize; ++column)
                {
                    const double cosine = CosineAtColumn(tables.header, column);
                    const auto value = IntegrateMissingAndBias(cosine, roughness, settings.latticeSize);
                    const size_t index = (static_cast<size_t>(row) * settings.tableSize + column) * 2;
                    tables.directional[index] = static_cast<float>(value[0]);
                    tables.directional[index + 1] = static_cast<float>(value[1]);
                }
            }
        };

        const unsigned int threadCount = settings.threadCount != 0 ? settings.threadCount :
            std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> threads;

        for(unsigned int thread = 0; thread < threadCount; ++thread)
        {
            threads.emplace_back(worker);
        }

        for(std::thread& thread : threads)
        {
            thread.join();
        }

        for(std::uint32_t row = 0; row < settings.tableSize; ++row)
        {
            const auto value = IntegrateAverage(tables, (row + 0.5) / settings.tableSize);
            tables.average[static_cast<size_t>(row) * 2] = static_cast<float>(value[0]);
            tables.average[static_cast<size_t>(row) * 2 + 1] = static_cast<float>(value[1]);
        }

        return tables;
    }
}
