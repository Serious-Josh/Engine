#include "Editor.h"
#include "Project/ProjectPaths.h"
#include "assets/ProjectAssetRegistry.h"
void Editor::ProcessMaterialLocateRequest(const ProjectPaths& paths,const ProjectAssetRegistry& registry)
{
    const auto reference=m_inspectorPanel.TakeMaterialLocateRequest();
    if(!reference) return;
    const auto id=AssetID::ParseReference(*reference);
    std::string error;
    if(!id) error="This assigned material has no project asset to locate.";
    else if(m_assetBrowser.RevealMaterial(*id,paths,registry,error))
    {
        m_workspace.AssetBrowserVisible()=true;
        m_inspectorPanel.SetMaterialLocateStatus({});
        return;
    }
    m_inspectorPanel.SetMaterialLocateStatus("Cannot locate material: "+error);
}
