/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/
#include <Cry_Geo.h>
#include <ConvertContext.h>
#include <IIndexedMesh.h>
#include <CGFContent.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzFramework/API/ApplicationAPI.h>
#include <AzFramework/StringFunc/StringFunc.h>
#include <AzToolsFramework/Debug/TraceContext.h>
#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <GFxFramework/MaterialIO/Material.h>
#include <SceneAPI/SceneCore/Containers/Scene.h>
#include <SceneAPI/SceneCore/Containers/SceneGraph.h>
#include <SceneAPI/SceneCore/DataTypes/Groups/IGroup.h>
#include <SceneAPI/SceneCore/DataTypes/Groups/IMeshGroup.h>
#include <SceneAPI/SceneCore/DataTypes/Rules/IRule.h>
#include <SceneAPI/SceneCore/DataTypes/Rules/IMaterialRule.h>
#include <SceneAPI/SceneCore/Containers/Views/SceneGraphChildIterator.h>
#include <SceneAPI/SceneCore/Containers/Utilities/Filters.h>
#include <SceneAPI/SceneCore/DataTypes/GraphData/IMaterialData.h>
#include <SceneAPI/SceneCore/Utilities/FileUtilities.h>
#include <SceneAPI/SceneCore/Utilities/Reporting.h>
#include <SceneAPI/SceneCore/Export/MtlMaterialExporter.h>
#include <RC/ResourceCompilerScene/Common/CommonExportContexts.h>
#include <RC/ResourceCompilerScene/Common/MaterialExporter.h>
#include <SceneAPI/SceneCore/Containers/RuleContainer.h>


namespace AZ
{
    namespace RC
    {
        namespace SceneEvents = AZ::SceneAPI::Events;
        namespace SceneDataTypes = AZ::SceneAPI::DataTypes;
        namespace SceneContainers = AZ::SceneAPI::Containers;
        namespace SceneViews = AZ::SceneAPI::Containers::Views;

        MaterialExporter::MaterialExporter(IConvertContext* convertContext)
            : CallProcessorBinder()
            , m_cachedGroup(nullptr)
            , m_convertContext(convertContext)
            , m_exportMaterial(true)
        {
            m_physMaterialNames[PHYS_GEOM_TYPE_DEFAULT_PROXY] = GFxFramework::MaterialExport::g_stringPhysicsNoDraw;

            BindToCall(&MaterialExporter::SetupMaterial);
            BindToCall(&MaterialExporter::ConfigureContainer);
            BindToCall(&MaterialExporter::ProcessNode);
            BindToCall(&MaterialExporter::PatchMesh);
            ActivateBindings();
        }

        SceneAPI::Events::ProcessingResult MaterialExporter::SetupMaterial(GroupExportContext& context)
        {
            switch (context.m_phase)
            {
            case Phase::Construction:
            {
                return HandleMaterialFileLoadingAndCreation(context);
            }
            default:
                return SceneEvents::ProcessingResult::Ignored;
            }
        }


        SceneEvents::ProcessingResult MaterialExporter::ConfigureContainer(ContainerExportContext& context)
        {
            switch (context.m_phase)
            {
            case Phase::Construction:
            {
                SceneEvents::ProcessingResult result = HandleMaterialFileLoadingAndCreation(context);
                if (result != SceneEvents::ProcessingResult::Success)
                {
                    return result;
                }
                m_cachedGroup = &(context.m_group);
                SetupGlobalMaterial(context);
                return SceneEvents::ProcessingResult::Success;
            }
            case Phase::Finalizing:
                if (!m_exportMaterial)
                {
                    Reset();
                    return SceneEvents::ProcessingResult::Ignored;
                }

                PatchSubmeshes(context);
                CreateSubMaterials(context);
                Reset();
                return SceneEvents::ProcessingResult::Success;
            default:
                return SceneEvents::ProcessingResult::Ignored;
            }
        }

        SceneEvents::ProcessingResult MaterialExporter::ProcessNode(NodeExportContext& context)
        {
            if (context.m_phase == Phase::Filling && m_exportMaterial)
            {
                AssignCommonMaterial(context);
                return SceneEvents::ProcessingResult::Success;
            }
            else
            {
                return SceneEvents::ProcessingResult::Ignored;
            }
        }
        
        SceneEvents::ProcessingResult MaterialExporter::PatchMesh(MeshNodeExportContext& context)
        {
            if (context.m_phase == Phase::Filling && m_exportMaterial)
            {
                return PatchMaterials(context);
            }
            else
            {
                return SceneEvents::ProcessingResult::Ignored;
            }
        }

        SceneEvents::ProcessingResult MaterialExporter::HandleMaterialFileLoadingAndCreation(GroupExportContext& context)
        {
            AZ_TraceContext("Material Group", context.m_group.GetName());

            if (!context.m_group.GetRuleContainerConst().FindFirstByType<SceneDataTypes::IMaterialRule>())
            {
                m_exportMaterial = false;
                AZ_TracePrintf(AZ::SceneAPI::Utilities::LogWindow, "Skipping material processing due to material rule not being present.");
                return SceneEvents::ProcessingResult::Ignored;
            }

            if (!LoadMaterialFile(context))
            {
                m_exportMaterial = false;
                AZ_TracePrintf(AZ::SceneAPI::Utilities::ErrorWindow, "Unable to read MTL file for processing meshes.");
                return SceneEvents::ProcessingResult::Failure;
            }
            return SceneEvents::ProcessingResult::Success;
        }

        bool MaterialExporter::LoadMaterialFile(GroupExportContext& context)
        {
            // Get path of material in source path
            AZStd::string rootPath(static_cast<ConvertContext*>(m_convertContext)->GetSourcePath().c_str());
            AzFramework::StringFunc::Path::StripFullName(rootPath);
            AZStd::string filePath =
                AZ::SceneAPI::Utilities::FileUtilities::CreateOutputFileName(context.m_group.GetName(),
                rootPath, GFxFramework::MaterialExport::g_mtlExtension);
            AZ_TraceContext("Material file path", filePath);

            m_materialGroup = AZStd::make_shared<GFxFramework::MaterialGroup>();
            bool fileRead = m_materialGroup->ReadMtlFile(filePath.c_str());
            if (!fileRead)
            {
                AZ_TracePrintf(AZ::SceneAPI::Utilities::LogWindow, "Unable to load material file, creating default one.");
                AZStd::string fileName = context.m_group.GetName() + GFxFramework::MaterialExport::g_mtlExtension;
                const SceneDataTypes::ISceneNodeGroup* sceneNodeGroup = azrtti_cast<const SceneDataTypes::ISceneNodeGroup*>(&context.m_group);
                if (sceneNodeGroup)
                {
                    const char* folderPath = nullptr;
                    AZStd::string texturePath;
                    EBUS_EVENT_RESULT(folderPath, AzToolsFramework::AssetSystemRequestBus, GetAbsoluteDevGameFolderPath);
                    if (!folderPath)
                    {
                        AZ_TracePrintf(SceneAPI::Utilities::WarningWindow, "Unable to get determine game folder. Texture path may be invalid.");
                    }
                    else
                    {
                        texturePath = folderPath;
                        EBUS_EVENT(AzFramework::ApplicationRequests::Bus, NormalizePath, texturePath);
                    }
                    AZ_TraceContext("Texture path", texturePath);

                    SceneAPI::Export::MtlMaterialExporter exporter;
                    SceneAPI::Export::MtlMaterialExporter::SaveMaterialResult result =
                        exporter.SaveMaterialGroup(*sceneNodeGroup, context.m_scene, texturePath);
                    // Update material path to the target path.
                    filePath = SceneAPI::Utilities::FileUtilities::CreateOutputFileName(
                        context.m_group.GetName(), context.m_outputDirectory, GFxFramework::MaterialExport::g_mtlExtension);

                    switch(result)
                    {
                    case SceneAPI::Export::MtlMaterialExporter::SaveMaterialResult::Success:
                        if (exporter.WriteToFile(filePath.c_str(), context.m_scene))
                        {
                            fileRead = m_materialGroup->ReadMtlFile(filePath.c_str());
                        }
                        break;
                    case SceneAPI::Export::MtlMaterialExporter::SaveMaterialResult::Failure:
                        AZ_TracePrintf(SceneAPI::Utilities::ErrorWindow, "Failed to created default material.");
                        break;
                    case SceneAPI::Export::MtlMaterialExporter::SaveMaterialResult::Skipped:
                        AZ_TracePrintf(SceneAPI::Utilities::LogWindow, "Skipping creation of default material.");
                        break;
                    default:
                        AZ_TraceContext("Unknown result", static_cast<int>(result));
                        AZ_TracePrintf(SceneAPI::Utilities::ErrorWindow, "Unknown material exporter result.");
                        break;
                    }
                }
            }

            if (!fileRead)
            {
                m_materialGroup.reset();
                return false;
            }
            
            return true;
        }

        void MaterialExporter::SetupGlobalMaterial(ContainerExportContext& context)
        {
            AZ_Assert(m_cachedGroup == &context.m_group, "ContainerExportContext doesn't belong to chain of previously called MeshGroupExportContext.");

            CMaterialCGF* rootMaterial = context.m_container.GetCommonMaterial();
            if (!rootMaterial)
            {
                rootMaterial = new CMaterialCGF();
                rootMaterial->nPhysicalizeType = PHYS_GEOM_TYPE_NONE;
                azstrcpy(rootMaterial->name, sizeof(rootMaterial->name), context.m_group.GetName().c_str());
                context.m_container.SetCommonMaterial(rootMaterial);
            }
        }

        void MaterialExporter::AssignCommonMaterial(NodeExportContext& context)
        {
            AZ_Assert(m_cachedGroup == &context.m_group, "MeshNodeExportContext doesn't belong to chain of previously called MeshGroupExportContext.");

            CMaterialCGF* rootMaterial = context.m_container.GetCommonMaterial();
            AZ_Assert(rootMaterial, "Previously assigned root material has been deleted.");
            context.m_node.pMaterial = rootMaterial;
        }

        SceneAPI::Events::ProcessingResult MaterialExporter::PatchMaterials(MeshNodeExportContext& context)
        {
            AZ_Assert(m_cachedGroup == &context.m_group, "MeshNodeExportContext doesn't belong to chain of previously called MeshGroupExportContext.");

            AZStd::vector<size_t> relocationTable;
            SceneEvents::ProcessingResult result = BuildRelocationTable(relocationTable, context);

            if (result == SceneEvents::ProcessingResult::Failure)
            {
                AZ_TracePrintf(SceneAPI::Utilities::ErrorWindow, "Material mapping has encountered an error and mesh generation has failed. If this FBX file was previously processed using the legacy FBX importer there may be a material mismatch. Please either move the FBX file from the source directory or delete the existing outputs and reimport.");
                return result;
            }

            if (relocationTable.empty())
            {
                // If the relocationTable is empty no materials were assigned to any of the
                //      selected meshes. In this case simply leave the subsets as assigned
                //      so users can later manually add materials if needed.
                return SceneEvents::ProcessingResult::Ignored;
            }

            if (context.m_container.GetExportInfo()->bMergeAllNodes)
            {
                // Due to a bug which cases subsets to not merge correctly (see PatchSubmeshes for more details) use the global
                //      table so far to patch the subset index in the face info instead. This way they will be assigned to the
                //      eventual global subset stored in the first mesh.
                int faceCount = context.m_mesh.GetFaceCount();
                for (int i = 0; i < faceCount; ++i)
                {
                    context.m_mesh.m_pFaces[i].nSubset = relocationTable[context.m_mesh.m_pFaces[i].nSubset];
                }
            }
            else
            {
                for (SMeshSubset& subset : context.m_mesh.m_subsets)
                {
                    subset.nMatID = relocationTable[subset.nMatID];
                }
            }

            return SceneEvents::ProcessingResult::Success;
        }

        void MaterialExporter::PatchSubmeshes(ContainerExportContext& context)
        {
            // Due to a bug in the merging process of the Compiler it will always take the number of subsets of the first mesh
            //      it finds. This causes files with more materials than the first model to not merge properly and ultimately cause
            //      the entire export to fail. (See CGFNodeMerger::MergeNodes for more details.) The work-around for now is to fill
            //      the first mesh up with placeholder subsets and adjust the subset indices in the face info.
            AZ_Assert(m_cachedGroup == &context.m_group, "ContainerExportContext doesn't belong to chain of previously called MeshGroupExportContext.");

            if (context.m_container.GetExportInfo()->bMergeAllNodes)
            {
                CMesh* firstMesh = nullptr;
                int nodeCount = context.m_container.GetNodeCount();
                for (int i = 0; i < nodeCount; ++i)
                {
                    CNodeCGF* node = context.m_container.GetNode(i);
                    if (node->pMesh && !node->bPhysicsProxy && node->type == CNodeCGF::NODE_MESH)
                    {
                        firstMesh = node->pMesh;
                        break;
                    }
                }

                if (firstMesh)
                {
                    int subsetCount = firstMesh->GetSubSetCount();
                    size_t materialCount = m_materialGroup->GetMaterialCount();

                    for (int i = 0; i < subsetCount; ++i)
                    {
                        AZ_Assert(firstMesh->m_subsets[i].nMatID == i, "Materials addition order broken. (%i vs. %i)", firstMesh->m_subsets[i].nMatID, i);
                    }

                    for (size_t i = subsetCount; i < materialCount; ++i)
                    {
                        SMeshSubset meshSubset;
                        meshSubset.nMatID = i;
                        firstMesh->m_subsets.push_back(meshSubset);
                    }
                }
            }
        }

        SceneAPI::Events::ProcessingResult MaterialExporter::BuildRelocationTable(AZStd::vector<size_t>& table, MeshNodeExportContext& context)
        {
            SceneEvents::ProcessingResultCombiner result;

            auto physicalizeType = context.m_physicalizeType;
            if (physicalizeType == PHYS_GEOM_TYPE_DEFAULT_PROXY)
            {
                table.push_back(m_materialGroup->FindMaterialIndex(GFxFramework::MaterialExport::g_stringPhysicsNoDraw));
            }
            else
            {
                const SceneContainers::SceneGraph& graph = context.m_scene.GetGraph();

                auto view = SceneViews::MakeSceneGraphChildView<SceneViews::AcceptEndPointsOnly>(
                    graph, context.m_nodeIndex, graph.GetContentStorage().begin(), true);
                for (auto it = view.begin(); it != view.end(); ++it)
                {
                    if ((*it) && (*it)->RTTI_IsTypeOf(SceneDataTypes::IMaterialData::TYPEINFO_Uuid()))
                    {
                        AZStd::string nodeName = graph.GetNodeName(graph.ConvertToNodeIndex(it.GetHierarchyIterator())).GetName();
                        size_t index = m_materialGroup->FindMaterialIndex(nodeName);
                        
                        if (index == GFxFramework::MaterialExport::g_materialNotFound)
                        {
                            AZ_TracePrintf(SceneAPI::Utilities::ErrorWindow, "Unable to find material named %s in mtl file while building FBX to Lumberyard material index table.", nodeName.c_str());
                            result += SceneEvents::ProcessingResult::Failure;
                        }
                        table.push_back(index);
                    }
                }
            }
            return result.GetResult();
        }

        void MaterialExporter::CreateSubMaterials(ContainerExportContext& context)
        {
            AZ_Assert(m_cachedGroup == &context.m_group, "MeshNodeExportContext doesn't belong to chain of previously called MeshGroupExportContext.");

            CMaterialCGF* rootMaterial = context.m_container.GetCommonMaterial();
            if (!rootMaterial)
            {
                AZ_Assert(rootMaterial, "Previously assigned root material has been deleted.");
                return;
            }

            // Create sub-materials stored in root material. Sub-materials will be used to assign physical types
            // to subsets stored in meshes when mesh gets compiled later on.
            rootMaterial->subMaterials.resize(m_materialGroup->GetMaterialCount(), nullptr);

            for (size_t i = 0; i < m_materialGroup->GetMaterialCount(); ++i)
            {
                CMaterialCGF* materialCGF = new CMaterialCGF();
                AZStd::shared_ptr<const GFxFramework::IMaterial> material = m_materialGroup->GetMaterial(i);
                if (material)
                {
                    azstrncpy(materialCGF->name, sizeof(materialCGF->name), material->GetName().c_str(), sizeof(materialCGF->name));
                    materialCGF->nPhysicalizeType = material->IsPhysicalMaterial() ? PHYS_GEOM_TYPE_DEFAULT_PROXY : PHYS_GEOM_TYPE_NONE;
                    rootMaterial->subMaterials[i] = materialCGF;
                }
            }
        }

        void MaterialExporter::Reset()
        {
            m_materialGroup = nullptr;
            m_exportMaterial = true;
        }

    } // namespace RC
} // namespace AZ
