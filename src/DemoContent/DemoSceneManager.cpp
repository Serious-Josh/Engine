#include "assets/CookedScene.h"
#include "DemoSceneManager.h"

#include "DemoContent/SphereGridScene.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Core/Log.h"

#include <utility>

void DemoSceneManager::SetProjectAssets(
    const std::filesystem::path& root,
    const ProjectAssetRegistry& registry)
{
    m_projectRoot = root;
    m_registry = &registry;
}

void DemoSceneManager::ConfigureProjectAssets(IDemoScene& scene) const
{
    auto* fileScene = dynamic_cast<FileBackedDemoScene*>(&scene);

    if(fileScene && !fileScene->IsArchiveActivation() && m_registry)
    {
        fileScene->SetProjectAssets(m_projectRoot, *m_registry);
        fileScene->SetTextureStreamingEnabled(m_textureStreamingEnabled);
        fileScene->SetMeshCPUCache(m_meshCPUCache);
    }
}

bool DemoSceneManager::Initialize(
    std::string_view sceneName,
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const Shader* vertexShader,
    const Shader* pixelShader,
    float aspectRatio)
{
    m_aspectRatio = aspectRatio;
    m_hasPendingSceneChange = false;

    const DemoSceneEntry* sceneEntry = SceneRegistry::Instance().Find(sceneName);

    if (!sceneEntry)
    {
        Log::Error("Failed to find demo scene.");
        return false;
    }

    m_activeSceneEntry = sceneEntry;

    m_requestedSceneEntry = sceneEntry;

    m_activeScene = sceneEntry->create();

    if (!m_activeScene)
    {
        Log::Error("Failed to create demo scene.");
        return false;
    }

    ConfigureProjectAssets(*m_activeScene);

    if (!m_activeScene->Initialize(device, uploader, descriptorHeap, vertexShader, pixelShader))
    {
        Log::Error("Failed to initialize demo scene.");
        m_activeScene.reset();
        return false;
    }

    m_activeScene->SetAspectRatio(m_aspectRatio);

    return true;
}

void DemoSceneManager::Shutdown()
{
    CancelFileSceneRequest();
    if (m_activeScene)
    {
        m_activeScene->Shutdown();
        m_activeScene.reset();
    }

    m_activeSceneEntry = nullptr;
    m_requestedSceneEntry = nullptr;
    m_activeFileName.clear();

    m_hasPendingSceneChange = false;
}

bool DemoSceneManager::RequestScene(std::string_view sceneName)
{
    if (m_hasPendingSceneChange || HasPendingFileSceneRequest())
    {
        Log::Error("A scene change request is already pending.");
        return false;
    }

    const DemoSceneEntry* sceneEntry = SceneRegistry::Instance().Find(sceneName);

    if (!sceneEntry)
    {
        Log::Error("Requested demo scene was not found.");
        return false;
    }

    if (sceneEntry == m_activeSceneEntry)
    {
        m_hasPendingSceneChange = false;

        m_requestedSceneEntry = m_activeSceneEntry;
        return true;
    }

    // Registered file scenes share the atomic file staging/renderer commit path.
    auto requested = sceneEntry->create();

    // Anchors project-relative registered scene paths before the path is read.
    ConfigureProjectAssets(*requested);

    if (const auto* fileScene = dynamic_cast<const FileBackedDemoScene*>(requested.get()))
    {
        if (!RequestFileScene(fileScene->GetScenePath()))
        {
            return false;
        }

        m_requestedSceneEntry = sceneEntry;
        return true;
    }

    m_requestedSceneEntry = sceneEntry;
    m_hasPendingSceneChange = true;

    return true;
}

bool DemoSceneManager::HasPendingSceneChange() const
{
    return m_hasPendingSceneChange || HasPendingFileSceneRequest();
}

bool DemoSceneManager::ApplyPendingSceneChange(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const Shader* vertexShader,
    const Shader* pixelShader)
{
    // File candidates require the explicit staging/commit boundary. The existing
    // registry switching sequence must never consume a file request implicitly.
    if (HasPendingFileSceneRequest())
    {
        Log::Error("File scene activation requires explicit candidate commit.");
        return false;
    }

    if (!m_hasPendingSceneChange)
    {
        return true;
    }

    if (!m_requestedSceneEntry)
    {
        Log::Error("Requested demo scene entry is invalid.");

        return false;
    }

    if (m_activeScene)
    {
        m_activeScene->Shutdown();
        m_activeScene.reset();
        m_activeSceneEntry = nullptr;
    }

    std::unique_ptr<IDemoScene> newScene = m_requestedSceneEntry->create();

    if (!newScene)
    {

        Log::Error("Failed to create requested demo scene.");
        return false;
    }

    ConfigureProjectAssets(*newScene);

    if (!newScene->Initialize(device, uploader, descriptorHeap, vertexShader, pixelShader))
    {
        Log::Error("Failed to initialize requested demo scene.");

        newScene->Shutdown();
        return false;
    }

    newScene->SetAspectRatio(m_aspectRatio);

    m_activeFileName.clear();
    m_activeScene = std::move(newScene);
    m_activeSceneEntry = m_requestedSceneEntry;

    m_hasPendingSceneChange = false;

    return true;
}

void DemoSceneManager::SetAspectRatio(float aspectRatio)
{
    m_aspectRatio = aspectRatio;

    if (m_activeScene)
    {
        m_activeScene->SetAspectRatio(aspectRatio);
    }

    if (m_stagedFileScene)
    {
        m_stagedFileScene->SetAspectRatio(aspectRatio);
    }
}

IDemoScene& DemoSceneManager::GetActiveScene()
{
    return *m_activeScene;
}

const IDemoScene& DemoSceneManager::GetActiveScene() const
{
    return *m_activeScene;
}

IDemoScene* DemoSceneManager::TryGetActiveScene() noexcept
{
    return m_activeScene.get();
}

const IDemoScene* DemoSceneManager::TryGetActiveScene() const noexcept
{
    return m_activeScene.get();
}

SphereGridScene* DemoSceneManager::GetActiveSphereGridScene()
{
    return dynamic_cast<SphereGridScene*>(m_activeScene.get());
}

std::span<const DemoSceneEntry> DemoSceneManager::GetAvailableScenes() const
{
    return SceneRegistry::Instance().GetScenes();
}

std::string_view DemoSceneManager::GetActiveSceneName() const
{
    if (!m_activeSceneEntry)
    {
        return m_activeFileName;
    }

    return m_activeSceneEntry->name;
}

namespace
{
    bool FailFileRequest(
        std::string* errorMessage,
        const std::string& message)
    {
        if (errorMessage)
        {
            *errorMessage = message;
        }

        return false;
    }
}

bool DemoSceneManager::RequestFileScene(
    const std::filesystem::path& path,
    std::string* errorMessage)
{
    if (m_hasPendingSceneChange || HasPendingFileSceneRequest())
    {
        return FailFileRequest(errorMessage, "A scene change request is already pending.");
    }

    if (path.empty() || !path.is_absolute() || path.extension() != ".scene")
    {
        return FailFileRequest(errorMessage, "An absolute .scene file path is required.");
    }

    std::error_code error;

    if (!std::filesystem::is_regular_file(path, error))
    {
        return FailFileRequest(errorMessage, "Scene path is not an accessible regular file: " +
            path.string() + (error ? " (" + error.message() + ")" : ""));
    }

    m_requestedFilePath = path;
    m_requestedMetadataOnly = true;
    m_requestedSceneEntry = nullptr;

    if (errorMessage)
    {
        errorMessage->clear();
    }

    return true;
}

bool DemoSceneManager::RequestArchiveScene(std::shared_ptr<const CookedScene::ArchiveScene> source, std::string* error)
{
    if(m_hasPendingSceneChange || HasPendingFileSceneRequest() || m_stagedFileScene || !source || !source->ValidateReferences(error))
        return FailFileRequest(error,"Archive activation requires a validated source and no pending scene switch.");
    m_requestedArchive = std::move(source); m_requestedMetadataOnly = true; m_requestedSceneEntry = nullptr;
    return true;
}

bool DemoSceneManager::RequestMetadataOnlyFileScene(const std::filesystem::path& path, std::string* errorMessage)
{
    return RequestFileScene(path, errorMessage);
}

#ifdef _DEBUG
bool DemoSceneManager::RequestSynchronousFileSceneDiagnostic(const std::filesystem::path& path, std::string* errorMessage)
{
    if (!RequestFileScene(path, errorMessage))
    {
        return false;
    }

    m_requestedMetadataOnly = false;
    return true;
}
#endif

bool DemoSceneManager::StageRequestedFileScene(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const Shader* vertexShader,
    const Shader* pixelShader,
    std::string* errorMessage)
{
    if (!HasPendingFileSceneRequest() || m_hasPendingSceneChange || m_stagedFileScene)
    {
        return FailFileRequest(errorMessage, "An unstaged file scene request is required.");
    }

    std::unique_ptr<FileBackedDemoScene> candidate;

    try
    {
        candidate = m_requestedArchive ? std::make_unique<FileBackedDemoScene>(m_requestedArchive) : std::make_unique<FileBackedDemoScene>(m_requestedFilePath);

        ConfigureProjectAssets(*candidate);

        bool initialized = false;

#ifdef _DEBUG
        if (!m_requestedMetadataOnly)
        {
            initialized = candidate->InitializeSynchronousDiagnostic(device, uploader, descriptorHeap, vertexShader, pixelShader);
        }
        else
#endif
        {
            initialized = candidate->Initialize(device, uploader, descriptorHeap, vertexShader, pixelShader);
        }

        if(!initialized)
        {
            candidate->Shutdown();
            return FailFileRequest(errorMessage, "Failed to initialize file scene candidate: " +
                m_requestedFilePath.string() + ". See initialization diagnostics.");
        }

        candidate->SetAspectRatio(m_aspectRatio);
        m_stagedFileName = candidate->GetDisplayName();
        m_stagedFileScene = std::move(candidate);
    }
    catch (const std::exception& exception)
    {
        if (candidate)
        {
            candidate->Shutdown();
        }

        return FailFileRequest(errorMessage, exception.what());
    }

    if (errorMessage)
    {
        errorMessage->clear();
    }

    return true;
}

IDemoScene* DemoSceneManager::GetStagedFileScene() noexcept
{
    return m_stagedFileScene.get();
}

const IDemoScene* DemoSceneManager::GetStagedFileScene() const noexcept
{
    return m_stagedFileScene.get();
}

std::string_view DemoSceneManager::GetStagedFileSceneName() const noexcept
{
    return m_stagedFileName;
}

const std::filesystem::path& DemoSceneManager::GetRequestedFileScenePath() const noexcept
{
    return m_requestedFilePath;
}

bool DemoSceneManager::CommitStagedFileScene(std::string* errorMessage)
{
    if (!m_stagedFileScene || !HasPendingFileSceneRequest() || m_hasPendingSceneChange)
    {
        return FailFileRequest(errorMessage, "No staged file scene is available to commit.");
    }

    if (m_activeScene)
    {
        m_activeScene->Shutdown();
    }

    m_activeScene = std::move(m_stagedFileScene);
    m_activeFileName = std::move(m_stagedFileName);
    m_activeSceneEntry = m_requestedSceneEntry;
    m_requestedSceneEntry = nullptr;
    m_requestedFilePath.clear();
    m_requestedArchive.reset();
    m_requestedMetadataOnly = false;

    if (errorMessage)
    {
        errorMessage->clear();
    }

    return true;
}

void DemoSceneManager::CancelFileSceneRequest()
{
    if (m_stagedFileScene)
    {
        m_stagedFileScene->Shutdown();
        m_stagedFileScene.reset();
    }

    m_stagedFileName.clear();
    m_requestedFilePath.clear();
    m_requestedArchive.reset();
    m_requestedMetadataOnly = false;
}

bool DemoSceneManager::InitializeArchive(std::shared_ptr<const CookedScene::ArchiveScene> source, ID3D12Device* device,
    ResourceUploader& uploader, DescriptorHeap& heap, const Shader* vertex, const Shader* pixel, float aspect)
{
    m_aspectRatio=aspect; m_registry=nullptr; m_projectRoot.clear(); m_hasPendingSceneChange=false;
    m_activeScene=std::make_unique<FileBackedDemoScene>(std::move(source));
    if(!m_activeScene->Initialize(device,uploader,heap,vertex,pixel)) return false;
    m_activeScene->SetAspectRatio(aspect); return true;
}
