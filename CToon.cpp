// CToon-inspired single-file example for GTA SA 1.0 US / Plugin-SDK.
//
// Important note:
// - The preserved leak shows Rockstar's original `CToon` relied on the RenderWare
//   `rptoon` plugin (`RpToonAtomicEnable`, `RpToonGeoSetPaint`, etc.).
// - This repo does not expose a buildable, in-tree `rptoon` SDK surface for a
//   standalone Plugin-SDK example project.
// - So this file intentionally implements an honest, compileable approximation:
//   permanent material/geometry posterization + flatter surface properties,
//   applied across visible entities, vehicles, peds, objects, and LOD entities.
//
// Controls:
// - F7: Toggle the effect on/off.

#include "plugin.h"
#include "common.h"
#include "CMessages.h"
#include "CPad.h"
#include "CTimer.h"
#include "CRenderer.h"
#include "CPed.h"
#include "CVehicle.h"
#include "CObject.h"
#include "RenderWare.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

using namespace plugin;

namespace {
constexpr unsigned TOGGLE_COOLDOWN_MS = 300;
constexpr int      MESSAGE_TIME_MS = 1500;
constexpr int      POSTERIZE_STEPS = 4;
constexpr float    MATERIAL_AMBIENT = 1.0f;
constexpr float    MATERIAL_DIFFUSE = 0.35f;
constexpr float    MATERIAL_SPECULAR = 0.0f;

struct SavedMaterialState {
    RwRGBA              color{};
    RwSurfaceProperties surfaceProps{};
};

struct SavedGeometryState {
    RwUInt32                              flags{};
    std::vector<std::vector<RwTexCoords>> texCoordSets{};
};

static unsigned char QuantizeChannel(unsigned char channel) {
    const float normalized = static_cast<float>(channel) / 255.0f;
    const float stepped = static_cast<float>(static_cast<int>(normalized * static_cast<float>(POSTERIZE_STEPS - 1) + 0.5f))
                        / static_cast<float>(POSTERIZE_STEPS - 1);
    const float biased = 0.10f + stepped * 0.90f;
    return static_cast<unsigned char>(std::clamp(biased, 0.0f, 1.0f) * 255.0f);
}

static float ProcessTexCoord(float value, int opt) {
    return opt == 1 ? (value * 1.0f) : (value * 0.4f);
}

class CToonInspiredSingleCpp {
public:
    CToonInspiredSingleCpp() {
        Events::gameProcessEvent += [] {
            Instance().ProcessToggle();
        };

        Events::drawingEvent += [] {
            Instance().ProcessVisibleRendererLists();
        };

        Events::pedRenderEvent.before += [](CPed* ped) {
            Instance().BeginDynamicLightingOverride();
            Instance().ProcessRwObject(ped ? ped->m_pRwObject : nullptr);
        };
        Events::pedRenderEvent.after += [](CPed*) {
            Instance().EndDynamicLightingOverride();
        };

        Events::vehicleRenderEvent.before += [](CVehicle* vehicle) {
            Instance().BeginDynamicLightingOverride();
            Instance().ProcessRwObject(vehicle ? vehicle->m_pRwObject : nullptr);
        };
        Events::vehicleRenderEvent.after += [](CVehicle*) {
            Instance().EndDynamicLightingOverride();
        };

        Events::objectRenderEvent.before += [](CObject* object) {
            Instance().BeginDynamicLightingOverride();
            Instance().ProcessRwObject(object ? object->m_pRwObject : nullptr);
        };
        Events::objectRenderEvent.after += [](CObject*) {
            Instance().EndDynamicLightingOverride();
        };
    }

    static CToonInspiredSingleCpp& Instance() {
        static CToonInspiredSingleCpp instance;
        return instance;
    }

private:
    bool                                             m_enabled{ true };
    unsigned                                         m_nextToggleTime{};
    int                                              m_lightingOverrideDepth{};
    std::unordered_map<RpMaterial*, SavedMaterialState> m_materials{};
    std::unordered_map<RpGeometry*, SavedGeometryState> m_geometries{};

    void ProcessToggle() {
        if (CTimer::m_snTimeInMilliseconds < m_nextToggleTime) {
            return;
        }

        if (!KeyPressed(VK_F7)) {
            return;
        }

        m_nextToggleTime = CTimer::m_snTimeInMilliseconds + TOGGLE_COOLDOWN_MS;
        m_enabled = !m_enabled;

        if (!m_enabled) {
            RestoreAll();
        }

        CMessages::AddMessageJumpQ(
            m_enabled ? "CToon-inspired on" : "CToon-inspired off",
            MESSAGE_TIME_MS,
            0,
            false
        );
    }

    void ProcessVisibleRendererLists() {
        if (!m_enabled) {
            return;
        }

        for (unsigned int i = 0; i < CRenderer::ms_nNoOfVisibleEntities; ++i) {
            ProcessEntity(CRenderer::ms_aVisibleEntityPtrs[i]);
        }

        for (unsigned int i = 0; i < CRenderer::ms_nNoOfVisibleLods; ++i) {
            ProcessEntity(CRenderer::ms_aVisibleLodPtrs[i]);
        }

        for (unsigned int i = 0; i < CRenderer::ms_nNoOfVisibleSuperLods; ++i) {
            ProcessEntity(CRenderer::ms_aVisibleSuperLodPtrs[i]);
        }
    }

    void BeginDynamicLightingOverride() {
        if (!m_enabled) {
            return;
        }

        if (m_lightingOverrideDepth++ == 0) {
            DeActivateDirectional();
            SetFullAmbient();
        }
    }

    void EndDynamicLightingOverride() {
        if (m_lightingOverrideDepth <= 0) {
            m_lightingOverrideDepth = 0;
            return;
        }

        if (--m_lightingOverrideDepth == 0) {
            ReSetAmbientAndDirectionalColours();
        }
    }

    void ProcessEntity(CEntity* entity) {
        if (!entity) {
            return;
        }

        ProcessRwObject(entity->m_pRwObject);
    }

    void ProcessRwObject(RwObject* rwObject) {
        if (!m_enabled || !rwObject) {
            return;
        }

        if (rwObject->type == rpCLUMP) {
            RpClumpForAllAtomics(reinterpret_cast<RpClump*>(rwObject), PatchAtomicCB, reinterpret_cast<void*>(1));
        } else if (rwObject->type == rpATOMIC) {
            PatchAtomic(reinterpret_cast<RpAtomic*>(rwObject), 0);
        }
    }

    static RpAtomic* PatchAtomicCB(RpAtomic* atomic, void* data) {
        Instance().PatchAtomic(atomic, static_cast<int>(reinterpret_cast<intptr_t>(data)));
        return atomic;
    }

    void PatchAtomic(RpAtomic* atomic, int mappingOpt) {
        if (!atomic) {
            return;
        }

        RpGeometry* geometry = RpAtomicGetGeometry(atomic);
        if (!geometry) {
            return;
        }

        auto [geometryIt, insertedGeometry] = m_geometries.try_emplace(geometry, SavedGeometryState{ geometry->flags, {} });
        if (insertedGeometry) {
            SaveAndPatchGeometryTexCoords(geometryIt->first, geometryIt->second, mappingOpt);
            geometry->flags |= rpGEOMETRYMODULATEMATERIALCOLOR;
        }

        RpGeometryForAllMaterials(geometry, PatchMaterialCB, this);
    }

    void SaveAndPatchGeometryTexCoords(RpGeometry* geometry, SavedGeometryState& savedState, int opt) {
        if (!geometry) {
            return;
        }

        const int geometryFlags = RpGeometryGetFlags(geometry);
        if ((geometryFlags & rpGEOMETRYTEXTURED) == 0) {
            return;
        }

        const int numTexCoordSets = RpGeometryGetNumTexCoordSets(geometry);
        const int numVertices = RpGeometryGetNumVertices(geometry);

        if (numTexCoordSets <= 0 || numVertices <= 0) {
            return;
        }

        savedState.texCoordSets.resize(numTexCoordSets);
        RpGeometryLock(geometry, rpGEOMETRYLOCKTEXCOORDSALL);

        for (int setIndex = 0; setIndex < numTexCoordSets; ++setIndex) {
            auto* texCoords = RpGeometryGetVertexTexCoords(geometry, RwTextureCoordinateIndex(setIndex + rwTEXTURECOORDINATEINDEX0));
            if (!texCoords) {
                continue;
            }

            auto& savedCoords = savedState.texCoordSets[setIndex];
            savedCoords.assign(texCoords, texCoords + numVertices);

            for (int vertexIndex = 0; vertexIndex < numVertices; ++vertexIndex) {
                texCoords[vertexIndex].u = ProcessTexCoord(texCoords[vertexIndex].u, opt);
                texCoords[vertexIndex].v = ProcessTexCoord(texCoords[vertexIndex].v, opt);
            }
        }

        RpGeometryUnlock(geometry);
    }

    static RpMaterial* PatchMaterialCB(RpMaterial* material, void* data) {
        auto* self = static_cast<CToonInspiredSingleCpp*>(data);
        if (self) {
            self->PatchMaterial(material);
        }
        return material;
    }

    void PatchMaterial(RpMaterial* material) {
        if (!material) {
            return;
        }

        auto [materialIt, insertedMaterial] = m_materials.try_emplace(material, SavedMaterialState{ material->color, material->surfaceProps });
        if (!insertedMaterial) {
            return;
        }

        material->color.red   = QuantizeChannel(material->color.red);
        material->color.green = QuantizeChannel(material->color.green);
        material->color.blue  = QuantizeChannel(material->color.blue);
        material->surfaceProps.ambient  = MATERIAL_AMBIENT;
        material->surfaceProps.diffuse  = MATERIAL_DIFFUSE;
        material->surfaceProps.specular = MATERIAL_SPECULAR;
    }

    void RestoreAll() {
        if (m_lightingOverrideDepth > 0) {
            m_lightingOverrideDepth = 0;
            ReSetAmbientAndDirectionalColours();
        }

        for (auto& [material, saved] : m_materials) {
            if (!material) {
                continue;
            }

            material->color = saved.color;
            material->surfaceProps = saved.surfaceProps;
        }

        for (auto& [geometry, saved] : m_geometries) {
            if (!geometry) {
                continue;
            }

            if (!saved.texCoordSets.empty()) {
                const int numTexCoordSets = static_cast<int>(saved.texCoordSets.size());
                RpGeometryLock(geometry, rpGEOMETRYLOCKTEXCOORDSALL);

                for (int setIndex = 0; setIndex < numTexCoordSets; ++setIndex) {
                    auto* texCoords = RpGeometryGetVertexTexCoords(geometry, RwTextureCoordinateIndex(setIndex + rwTEXTURECOORDINATEINDEX0));
                    const auto& savedCoords = saved.texCoordSets[setIndex];
                    if (!texCoords || savedCoords.empty()) {
                        continue;
                    }

                    std::copy(savedCoords.begin(), savedCoords.end(), texCoords);
                }

                RpGeometryUnlock(geometry);
            }

            geometry->flags = saved.flags;
        }

        m_materials.clear();
        m_geometries.clear();
    }
};

static CToonInspiredSingleCpp& g_cToonInspiredSingleCpp = CToonInspiredSingleCpp::Instance();
} // namespace
