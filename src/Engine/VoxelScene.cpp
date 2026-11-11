#include "Engine/VoxelScene.h"

#include "Voxel/VoxelBinary.h"
#include "Voxel/VoxelManifest.h"
#include "assets/ImportedMesh.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>

namespace VoxelScene
{
namespace
{
    // The scene's literal ranges must stay the voxel module's.
    static_assert(Voxel::MinimumBaseExponent == -7 && Voxel::MaximumBaseExponent == 4);
    static_assert(Voxel::RegionFormat::MinimumNarrowBand == 1 && Voxel::RegionFormat::MaximumNarrowBand == 32);
    static_assert(Voxel::RegionFormat::MinimumCoarseClamp == 1 && Voxel::RegionFormat::MaximumCoarseClamp == 16);

    Voxel::ObjectOrientation ToVoxel(VoxelOrientation orientation) noexcept
    {
        switch(orientation)
        {
        case VoxelOrientation::AsAuthored:
            return Voxel::ObjectOrientation::AsAuthored;
        case VoxelOrientation::Outward:
            return Voxel::ObjectOrientation::Outward;
        case VoxelOrientation::Automatic:
            break;
        }
        return Voxel::ObjectOrientation::Automatic;
    }

    bool Finite(const Matrix4& matrix) noexcept
    {
        for(const auto& row : matrix.m)
        {
            for(const float value : row)
            {
                if(!std::isfinite(value))
                {
                    return false;
                }
            }
        }
        return true;
    }

    std::string Describe(const SceneObjectDocument& object)
    {
        return "'" + object.name + "' (" + object.id + ")";
    }
}

ParticipationDecision ResolveParticipation(const SceneObjectDocument& object)
{
    const VoxelSettings settings = object.voxel.value_or(VoxelSettings{});
    ParticipationDecision decision;
    decision.orientation = ToVoxel(settings.orientation);

    const auto exclude = [&](ParticipationReason reason) {
        decision.participates = false;
        decision.reason = reason;
        decision.warning = settings.participation == VoxelParticipation::Static;
        return decision;
    };

    if(object.meshReference.empty())
    {
        decision.reason = ParticipationReason::NoMesh;
        return decision;
    }

    if(settings.participation == VoxelParticipation::Excluded)
    {
        decision.reason = ParticipationReason::Excluded;
        return decision;
    }

    // Motion wins over an explicit Static.
    if(object.characterController)
    {
        return exclude(ParticipationReason::CharacterController);
    }

    if(object.physics)
    {
        switch(object.physics->bodyType)
        {
        case Physics::BodyType::Static:
            decision.participates = true;
            decision.reason = ParticipationReason::StaticBody;
            return decision;
        case Physics::BodyType::Dynamic:
            return exclude(ParticipationReason::DynamicBody);
        case Physics::BodyType::Kinematic:
            return exclude(ParticipationReason::KinematicBody);
        }
    }

    if(settings.participation == VoxelParticipation::Static)
    {
        decision.participates = true;
        decision.reason = ParticipationReason::ExplicitStatic;
        return decision;
    }

    decision.reason = ParticipationReason::NotStatic;
    return decision;
}

const char* ParticipationReasonToString(ParticipationReason reason) noexcept
{
    switch(reason)
    {
    case ParticipationReason::StaticBody:
        return "static physics body";
    case ParticipationReason::ExplicitStatic:
        return "marked static";
    case ParticipationReason::NoMesh:
        return "no mesh";
    case ParticipationReason::Excluded:
        return "excluded";
    case ParticipationReason::DynamicBody:
        return "dynamic body";
    case ParticipationReason::KinematicBody:
        return "kinematic body";
    case ParticipationReason::CharacterController:
        return "character controller";
    case ParticipationReason::NotStatic:
        return "not static";
    }
    return "unknown";
}

Voxel::BakeSettings BakeSettingsFor(const SceneDocument& document)
{
    const VoxelBakeSettings scene = document.voxelBake.value_or(VoxelBakeSettings{});
    Voxel::BakeSettings settings;
    settings.baseExponent = static_cast<std::int8_t>(scene.baseExponent);
    settings.regionBricks = static_cast<std::uint8_t>(scene.regionBricks);
    settings.narrowBand = static_cast<std::uint8_t>(scene.narrowBand);
    settings.coarseClamp = static_cast<std::uint8_t>(scene.coarseClamp);
    return settings;
}

std::expected<CapturedScene, std::string> Capture(const SceneDocument& document, const SceneAccess& access, const Voxel::SnapshotOptions& options)
{
    if(document.voxelBake)
    {
        std::string error;
        if(!ValidateVoxelBakeSettings(*document.voxelBake, &error))
        {
            return std::unexpected("Invalid voxel bake settings: " + error);
        }
    }

    CapturedScene captured;
    captured.settings = BakeSettingsFor(document);
    captured.currentManifest = document.voxelBake ? document.voxelBake->manifestReference : std::string{};
    CaptureReport& report = captured.report;
    report.objects = document.objects.size();

    // Stable identity: participants in ascending object-ID order, whatever the document order.
    std::vector<const SceneObjectDocument*> ordered;
    for(const SceneObjectDocument& object : document.objects)
    {
        ordered.push_back(&object);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->id < b->id; });

    std::vector<Voxel::MeshInput> inputs;
    std::vector<std::string> problems;
    // An identical mesh under an identical world matrix is the same geometry: captured once.
    std::map<std::pair<const ImportedMesh*, std::array<std::uint32_t, 16>>, std::string> seen;

    for(const SceneObjectDocument* object : ordered)
    {
        const ParticipationDecision decision = ResolveParticipation(*object);
        ++report.byReason[decision.reason];

        if(decision.warning)
        {
            report.warnings.push_back(Describe(*object) + " is marked voxel Static but is a " + ParticipationReasonToString(decision.reason) +
                "; it is not baked.");
        }

        if(!decision.participates)
        {
            continue;
        }

        const std::optional<Matrix4> world = access.world ? access.world(*object) : std::nullopt;
        const ImportedMesh* mesh = access.mesh ? access.mesh(*object) : nullptr;

        if(!world)
        {
            problems.push_back(Describe(*object) + " has no world transform");
            continue;
        }

        if(mesh == nullptr)
        {
            problems.push_back(Describe(*object) + " has no loaded mesh geometry");
            continue;
        }

        if(!Finite(*world))
        {
            problems.push_back(Describe(*object) + " has a non-finite world transform");
            continue;
        }

        std::array<std::uint32_t, 16> bits{};
        for(std::size_t index = 0; index < 16; ++index)
        {
            bits[index] = std::bit_cast<std::uint32_t>(world->m[index / 4][index % 4]);
        }

        if(const auto [found, isNew] = seen.try_emplace({mesh, bits}, object->id); !isNew)
        {
            ++report.duplicateInstances;
            report.warnings.push_back(Describe(*object) + " repeats the geometry and transform of object " + found->second + "; captured once.");
            continue;
        }

        if(mesh->indices.size() % 3 != 0)
        {
            problems.push_back(Describe(*object) + " has an index count that is not a multiple of three");
            continue;
        }

        const auto& m = world->m;
        const double determinant = double(m[0][0]) * (double(m[1][1]) * m[2][2] - double(m[1][2]) * m[2][1]) -
            double(m[0][1]) * (double(m[1][0]) * m[2][2] - double(m[1][2]) * m[2][0]) +
            double(m[0][2]) * (double(m[1][0]) * m[2][1] - double(m[1][1]) * m[2][0]);
        const bool mirrored = determinant < 0.0;
        report.mirrored += mirrored;

        Voxel::MeshInput input;
        input.orientation = decision.orientation;
        input.positions.reserve(mesh->vertices.size());
        bool finite = true;

        for(const Vertex& vertex : mesh->vertices)
        {
            const double p[3] = {vertex.position[0], vertex.position[1], vertex.position[2]};
            Voxel::Point transformed;
            for(std::size_t row = 0; row < 3; ++row)
            {
                transformed[row] = double(m[row][0]) * p[0] + double(m[row][1]) * p[1] + double(m[row][2]) * p[2] + double(m[row][3]);
                finite = finite && std::isfinite(transformed[row]);
            }
            input.positions.push_back(transformed);
        }

        if(!finite)
        {
            problems.push_back(Describe(*object) + " has non-finite vertex positions in world space");
            continue;
        }

        bool indicesValid = true;
        input.triangles.reserve(mesh->indices.size() / 3);
        for(std::size_t index = 0; index + 2 < mesh->indices.size(); index += 3)
        {
            std::array<std::uint32_t, 3> triangle{mesh->indices[index], mesh->indices[index + 1], mesh->indices[index + 2]};
            indicesValid = indicesValid && triangle[0] < mesh->vertices.size() && triangle[1] < mesh->vertices.size() && triangle[2] < mesh->vertices.size();
            // A mirroring world matrix reverses winding; reversing it back keeps outward meshes outward.
            if(mirrored)
            {
                std::swap(triangle[1], triangle[2]);
            }
            input.triangles.push_back(triangle);
        }

        if(!indicesValid)
        {
            problems.push_back(Describe(*object) + " has a triangle index out of range");
            continue;
        }

        ++report.participating;
        report.triangles += input.triangles.size();
        inputs.push_back(std::move(input));
    }

    if(!problems.empty())
    {
        std::string message = "Cannot capture voxel geometry:";
        for(const std::string& problem : problems)
        {
            message += "\n  " + problem + ".";
        }
        return std::unexpected(message);
    }

    auto snapshot = Voxel::GeometrySnapshot::Build(inputs, options);
    if(!snapshot)
    {
        return std::unexpected("Cannot build the voxel geometry snapshot: " + snapshot.error());
    }

    captured.snapshot = std::make_shared<const Voxel::GeometrySnapshot>(std::move(*snapshot));
    return captured;
}

// ---------------------------------------------------------------------------------------------------------------------------

SceneBakeJob::SceneBakeJob(CapturedScene scene, std::shared_ptr<Voxel::AssetStore> store, BakeJobOptions options, BackgroundTaskManager* tasks) :
    m_scene(std::move(scene)),
    m_store(std::move(store)),
    m_options(options)
{
    if(tasks != nullptr)
    {
        m_task = tasks->Begin({"Voxel bake", true, InvalidBackgroundTaskId});
        m_task.Start();
        m_task.SetStatus("Preparing geometry");
    }

    m_thread = std::thread([this] { Run(); });
}

SceneBakeJob::~SceneBakeJob()
{
    m_cancel = true;

    if(m_thread.joinable())
    {
        m_thread.join();
    }

    // Never published: whatever was staged is discarded.
    if(!m_consumed && m_prepared && m_prepared->has_value())
    {
        Voxel::AbandonWorldBake(*m_store, **m_prepared);
    }

    if(!m_consumed)
    {
        m_task.MarkCancelled("Voxel bake abandoned.");
    }
}

void SceneBakeJob::Run()
{
    const Voxel::BakeAcceleration acceleration(*m_scene.snapshot);
    Voxel::WorldBakeOptions options;
    options.settings = m_scene.settings;
    options.threads = m_options.threads;
    options.memoryBudget = m_options.memoryBudget;
    options.maximumRegions = m_options.maximumRegions;
    options.reuse = m_options.reuse;
    options.cancel = &m_cancel;
    options.regionsTotal = &m_total;
    options.regionsDone = &m_done;
    m_prepared = Voxel::PrepareWorldBake(*m_scene.snapshot, acceleration, *m_store, options);
    m_finished = true;
}

void SceneBakeJob::Cancel() noexcept
{
    m_cancel = true;
}

bool SceneBakeJob::IsFinished() const noexcept
{
    return m_finished.load();
}

std::pair<std::size_t, std::size_t> SceneBakeJob::Poll()
{
    const std::size_t total = m_total.load();
    const std::size_t done = (std::min)(m_done.load(), total);

    if(m_task.IsCancelRequested())
    {
        m_cancel = true;
    }

    if(total > 0)
    {
        m_task.SetProgress(done, total);
        m_task.SetStatus("Baking regions");
    }

    return {done, total};
}

std::expected<BakeOutcome, Voxel::WorldBakeError> SceneBakeJob::Finish()
{
    if(m_consumed)
    {
        return std::unexpected(Voxel::WorldBakeError{false, "This voxel bake was already finished."});
    }

    if(m_thread.joinable())
    {
        m_thread.join();
    }

    m_consumed = true;

    if(!m_prepared->has_value())
    {
        const Voxel::WorldBakeError error = m_prepared->error();
        error.cancelled ? m_task.MarkCancelled(error.message) : m_task.Fail(error.message);
        return std::unexpected(error);
    }

    // A cancel that arrived after preparation still publishes nothing.
    if(m_cancel.load())
    {
        Voxel::AbandonWorldBake(*m_store, **m_prepared);
        const Voxel::WorldBakeError error{true, "The voxel bake was cancelled; nothing was published."};
        m_task.MarkCancelled(error.message);
        return std::unexpected(error);
    }

    const std::uint64_t contentHash = (*m_prepared)->result.contentHash;
    auto published = Voxel::PublishWorldBake(*m_store, std::move(**m_prepared));

    if(!published)
    {
        m_task.Fail(published.error().message);
        return std::unexpected(published.error());
    }

    BakeOutcome outcome;
    outcome.manifestReference = published->manifestReference;
    outcome.settings = m_scene.settings;
    outcome.statistics = published->statistics;
    outcome.contentHash = contentHash;
    m_task.Complete("Voxel bake published: " + std::to_string(outcome.statistics.populated) + " populated regions.");
    return outcome;
}

VoxelBakeSettings BakeSettingsWithManifest(const Voxel::BakeSettings& settings, const std::string& manifest)
{
    VoxelBakeSettings scene;
    scene.baseExponent = settings.baseExponent;
    scene.regionBricks = settings.regionBricks;
    scene.narrowBand = settings.narrowBand;
    scene.coarseClamp = settings.coarseClamp;
    scene.manifestReference = manifest;
    return scene;
}

void ApplyBakeResult(SceneDocument& document, const BakeOutcome& outcome)
{
    document.voxelBake = BakeSettingsWithManifest(outcome.settings, outcome.manifestReference);
}

const char* BakeStateToString(BakeState state) noexcept
{
    switch(state)
    {
    case BakeState::NotBaked:
        return "Not baked";
    case BakeState::UpToDate:
        return "Up to date";
    case BakeState::Stale:
        return "Out of date";
    case BakeState::SettingsChanged:
        return "Settings changed";
    case BakeState::Missing:
        return "Missing";
    case BakeState::Corrupt:
        return "Corrupt";
    }
    return "Unknown";
}

BakeStatus EvaluateBakeStatus(const CapturedScene& scene, const Voxel::AssetStore& store, const std::atomic<bool>* cancel)
{
    BakeStatus status;
    status.manifestReference = scene.currentManifest;
    const auto cancelled = [cancel] { return cancel != nullptr && cancel->load(); };

    if(scene.currentManifest.empty())
    {
        status.detail = "The scene has no published voxel bake.";
        return status;
    }

    const auto manifestBytes = store.Read(scene.currentManifest);
    if(!manifestBytes)
    {
        status.state = BakeState::Missing;
        status.detail = manifestBytes.error();
        return status;
    }

    Voxel::ManifestData manifest;
    if(const Voxel::DecodeResult decoded = Voxel::DecodeManifest(*manifestBytes, manifest); !decoded)
    {
        status.state = BakeState::Corrupt;
        status.detail = "The manifest does not decode: " + decoded.message;
        return status;
    }

    // Every region the manifest references must read and decode as itself; the content hash follows from their bytes.
    std::vector<std::uint64_t> assetHashes;
    for(const Voxel::ManifestEntry& entry : manifest.entries)
    {
        if(cancelled())
        {
            status.detail = "Cancelled.";
            return status;
        }

        if(entry.kind != Voxel::RegionKind::Populated)
        {
            ++status.uniformInside;
            assetHashes.push_back(0);
            continue;
        }

        ++status.populated;
        const auto bytes = store.Read(entry.reference);
        if(!bytes)
        {
            status.state = BakeState::Missing;
            status.detail = bytes.error();
            return status;
        }

        Voxel::RegionData data;
        const Voxel::DecodeResult decoded = Voxel::DecodeRegion(*bytes, data);
        if(!decoded || data.key != entry.key || data.inputHash != entry.inputHash || bytes->size() != entry.byteSize)
        {
            status.state = BakeState::Corrupt;
            status.detail = "Region asset " + entry.reference + (decoded ? " records a different region." : " does not decode: " + decoded.message);
            return status;
        }

        assetHashes.push_back(Voxel::Binary::Fnv1a(*bytes));
    }

    status.contentHash = Voxel::ManifestContentHash(manifest, assetHashes);

    if(manifest.settingsHash != Voxel::HashBakeSettings(scene.settings) || manifest.baseExponent != scene.settings.baseExponent ||
        manifest.regionBricks != scene.settings.regionBricks || manifest.narrowBand != scene.settings.narrowBand ||
        manifest.coarseClamp != scene.settings.coarseClamp)
    {
        status.state = BakeState::SettingsChanged;
        status.detail = "The scene's voxel settings differ from the published bake's.";
        return status;
    }

    // Compare with what a bake of the captured scene would plan: the same regions with the same inputs, or how many differ.
    const Voxel::BakeAcceleration acceleration(*scene.snapshot);
    const auto plan = Voxel::PlanWorld(*scene.snapshot, acceleration, scene.settings, std::size_t{1} << 20);
    if(!plan)
    {
        status.state = BakeState::Stale;
        status.detail = "The scene cannot be planned: " + plan.error();
        return status;
    }

    std::map<Voxel::RegionKey, std::uint64_t> published;
    for(const Voxel::ManifestEntry& entry : manifest.entries)
    {
        published[entry.key] = entry.inputHash;
    }

    // Planned regions absent from the manifest were uniformly Outside; their earlier inputs are only known through the
    // participation hash, which covers every planned region's key and input hash.
    std::size_t changed = 0;
    std::map<Voxel::RegionKey, bool> planned;
    for(const Voxel::RegionPlan& region : plan->regions)
    {
        planned[region.key] = true;
        const auto found = published.find(region.key);
        changed += found != published.end() && found->second != region.inputHash;
    }
    for(const auto& [key, hash] : published)
    {
        changed += !planned.contains(key);
    }

    if(changed == 0 && Voxel::PlanParticipationHash(*plan) == manifest.participationHash)
    {
        status.state = BakeState::UpToDate;
        status.detail = "The published bake matches the scene.";
        return status;
    }

    status.state = BakeState::Stale;
    status.changedRegions = changed;
    status.detail = changed > 0 ? std::to_string(changed) + " baked region(s) would change." : "Geometry changed in regions without baked data.";
    return status;
}

BakeStatusCheck::BakeStatusCheck(CapturedScene scene, std::shared_ptr<const Voxel::AssetStore> store) :
    m_scene(std::move(scene)),
    m_store(std::move(store))
{
    m_thread = std::thread([this] {
        m_status = EvaluateBakeStatus(m_scene, *m_store, &m_cancel);
        m_finished = true;
    });
}

BakeStatusCheck::~BakeStatusCheck()
{
    m_cancel = true;
    if(m_thread.joinable())
    {
        m_thread.join();
    }
}

bool BakeStatusCheck::IsFinished() const noexcept
{
    return m_finished.load();
}

BakeStatus BakeStatusCheck::Take()
{
    if(m_thread.joinable())
    {
        m_thread.join();
    }
    return m_status;
}
}
