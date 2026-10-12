#include "DemoSceneManager.h"

#include "DemoContent/SphereGridScene.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Core/Log.h"

#include <utility>

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
    if (m_hasPendingSceneChange || !m_requestedFilePath.empty())
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

    m_requestedSceneEntry = sceneEntry;
    m_hasPendingSceneChange = true;

    return true;
}

bool DemoSceneManager::HasPendingSceneChange() const
{
    return m_hasPendingSceneChange || !m_requestedFilePath.empty();
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
    if (!m_requestedFilePath.empty())
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
    if (m_hasPendingSceneChange || !m_requestedFilePath.empty())
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
    m_requestedSceneEntry = nullptr;

    if (errorMessage)
    {
        errorMessage->clear();
    }

    return true;
}

bool DemoSceneManager::StageRequestedFileScene(
    ID3D12Device* device,
    ResourceUploader& uploader,
    DescriptorHeap& descriptorHeap,
    const Shader* vertexShader,
    const Shader* pixelShader,
    std::string* errorMessage)
{
    if (m_requestedFilePath.empty() || m_hasPendingSceneChange || m_stagedFileScene)
    {
        return FailFileRequest(errorMessage, "An unstaged file scene request is required.");
    }

    std::unique_ptr<FileBackedDemoScene> candidate;

    try
    {
        candidate = std::make_unique<FileBackedDemoScene>(m_requestedFilePath);

        if (!candidate->Initialize(device, uploader, descriptorHeap, vertexShader, pixelShader))
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
    if (!m_stagedFileScene || m_requestedFilePath.empty() || m_hasPendingSceneChange)
    {
        return FailFileRequest(errorMessage, "No staged file scene is available to commit.");
    }

    if (m_activeScene)
    {
        m_activeScene->Shutdown();
    }

    m_activeScene = std::move(m_stagedFileScene);
    m_activeFileName = std::move(m_stagedFileName);
    m_activeSceneEntry = nullptr;
    m_requestedSceneEntry = nullptr;
    m_requestedFilePath.clear();

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
}
