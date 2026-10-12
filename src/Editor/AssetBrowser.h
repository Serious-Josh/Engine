#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <array>
#include <string>
#include <vector>

class AssetBrowser
{
public:
    AssetBrowser();
    void Draw(bool& visible);

    struct MaterialCreationRequest
    {
        std::filesystem::path relativePath;
    };

    std::optional<MaterialCreationRequest> TakeMaterialCreationRequest();
    void OnMaterialCreationCompleted(
        const std::filesystem::path& relativePath,
        const std::string& error);

private:
    struct Entry
    {
        std::filesystem::path path;

        std::string name;

        bool directory = false;
        bool navigable = false;
    };

    struct Listing
    {
        std::vector<Entry> entries;

        std::string error;
    };

    bool CanBrowse(
        const std::filesystem::path& path,
        std::string& error) const;
    const Listing& GetListing(const std::filesystem::path& path);
    void DrawTree(
        const std::filesystem::path& path,
        const std::string& label);
    void Navigate(const std::filesystem::path& path);
    void DrawContents();
    bool CanCreateMaterial(std::string& error) const;
    void DrawMaterialCreationModal();

    std::filesystem::path m_root;
    std::filesystem::path m_directory;
    std::filesystem::path m_selection;

    std::map<std::filesystem::path, Listing> m_listings;

    std::string m_error;

    float m_panelHeight = 260.0f;

    std::optional<MaterialCreationRequest> m_materialRequest;

    bool m_materialCreationPending = false;
    bool m_openMaterialModal = false;

    std::filesystem::path m_materialDirectory;

    std::array<char, 256> m_materialName{};

    std::string m_materialError;
};
