#include "assets/ProjectAssetRegistry.h"

#include <algorithm>

bool ProjectAssetRegistry::Register(const AssetRecord& record)
{
    if(!ValidateAssetRecord(record)){
        return false;
    }

    for(const AssetRecord& existing : m_records){
        if(existing.id == record.id ||
           existing.relativePath == record.relativePath){
            return false;
           }
    }

    m_records.push_back(record);
    return true;
}

const AssetRecord* ProjectAssetRegistry::Find(const AssetID& id) const noexcept
{
    const auto it = std::find_if(m_records.begin(), m_records.end(), [&id](const AssetRecord& record)
    {
       return record.id == id; 
    });

    return it == m_records.end() ? nullptr : &*it;
}

std::vector<AssetRecord> ProjectAssetRegistry::GetAssets() const
{
    return m_records;
}

std::vector<AssetRecord> ProjectAssetRegistry::GetAssets(AssetType type) const
{
    std::vector<AssetRecord> result;

    for(const AssetRecord& record : m_records){
        if(record.type == type){
            result.push_back(record);
        }
    }

    return result;
}

bool ProjectAssetRegistry::UpdatePath(const AssetID& id, const std::filesystem::path& newRelativePath)
{
    auto it = std::find_if(m_records.begin(), m_records.end(), [&id](const AssetRecord& record)
    {
        return record.id == id;
    });

    if(it == m_records.end()){
        return false;
    }

    AssetRecord updated = *it;
    updated.relativePath = newRelativePath;

    if(!ValidateAssetRecord(updated)){
        return false;
    }

    for(const AssetRecord& existing : m_records){
        if(existing.id != id && existing.relativePath == newRelativePath){
            return false;
        }
    }

    it->relativePath = newRelativePath;
    return true;
}

bool ProjectAssetRegistry::Unregister(const AssetID& id)
{
    const auto oldSize = m_records.size();

    std::erase_if(m_records, [&id](const AssetRecord& record)
    {
       return record.id == id; 
    });

    return m_records.size() != oldSize;
}

void ProjectAssetRegistry::Clear() noexcept
{
    m_records.clear();
}