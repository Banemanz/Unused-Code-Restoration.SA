// Plugin-SDK-compatible "CToon-inspired" fallback for GTA SA 1.0 US.
//
// This does NOT restore Rockstar's missing RpToon/rptoon pipeline from the leak.
// Instead, it recreates part of the intended look in a single .cpp by:
// - flattening lighting with full ambient + no directional light,
// - posterizing material colours into a few bands,
// - applying the effect through normal Plugin-SDK render events.
//
// Toggle:
// - F7: Enable / disable the effect.

#include "plugin.h"
#include "common.h"
#include "CMessages.h"
#include "CPad.h"
#include "CTimer.h"
#include "CPed.h"
#include "CVehicle.h"
#include "CObject.h"
#include "RenderWare.h"

#include <algorithm>
#include <vector>

using namespace plugin;

namespace {

    constexpr unsigned TOGGLE_COOLDOWN_MS = 300;
    constexpr int      MESSAGE_TIME_MS = 1200;
    constexpr int      POSTERIZE_STEPS = 4;

    struct SavedMaterialState {
        RpMaterial* material{};
        RwRGBA               color{};
        RwSurfaceProperties  surfaceProps{};
    };

    struct SavedGeometryState {
        RpGeometry* geometry{};
        RwUInt32    flags{};
    };

    struct RenderPatchContext {
        std::vector<SavedMaterialState> savedMaterials{};
        std::vector<SavedGeometryState> savedGeometries{};
    };

    static unsigned char QuantizeChannel(unsigned char channel) {
        if (POSTERIZE_STEPS <= 1) {
            return channel;
        }

        const float normalized = static_cast<float>(channel) / 255.0f;
        const float stepped = static_cast<float>(static_cast<int>(normalized * static_cast<float>(POSTERIZE_STEPS - 1) + 0.5f))
            / static_cast<float>(POSTERIZE_STEPS - 1);
        const float biased = 0.12f + stepped * 0.88f;
        return static_cast<unsigned char>(std::clamp(biased, 0.0f, 1.0f) * 255.0f);
    }

    static RpMaterial* PosterizeMaterialCB(RpMaterial* material, void* data) {
        if (!material || !data) {
            return material;
        }

        auto& ctx = *static_cast<RenderPatchContext*>(data);

        ctx.savedMaterials.push_back({
            material,
            material->color,
            material->surfaceProps
            });

        material->color.red = QuantizeChannel(material->color.red);
        material->color.green = QuantizeChannel(material->color.green);
        material->color.blue = QuantizeChannel(material->color.blue);

        material->surfaceProps.ambient = 1.0f;
        material->surfaceProps.diffuse = 0.0f;
        material->surfaceProps.specular = 0.0f;

        return material;
    }

    static RpAtomic* PosterizeAtomicCB(RpAtomic* atomic, void* data) {
        if (!atomic || !atomic->geometry || !data) {
            return atomic;
        }

        auto& ctx = *static_cast<RenderPatchContext*>(data);

        ctx.savedGeometries.push_back({
            atomic->geometry,
            atomic->geometry->flags
            });

        atomic->geometry->flags |= rpGEOMETRYMODULATEMATERIALCOLOR;
        RpGeometryForAllMaterials(atomic->geometry, PosterizeMaterialCB, data);
        return atomic;
    }

    static void ApplyToRwObject(RwObject* rwObject, RenderPatchContext& ctx) {
        if (!rwObject) {
            return;
        }

        if (rwObject->type == rpCLUMP) {
            RpClumpForAllAtomics(reinterpret_cast<RpClump*>(rwObject), PosterizeAtomicCB, &ctx);
        }
        else if (rwObject->type == rpATOMIC) {
            PosterizeAtomicCB(reinterpret_cast<RpAtomic*>(rwObject), &ctx);
        }
    }

    static void RestorePatchedState(RenderPatchContext& ctx) {
        for (auto it = ctx.savedMaterials.rbegin(); it != ctx.savedMaterials.rend(); ++it) {
            if (!it->material) {
                continue;
            }

            it->material->color = it->color;
            it->material->surfaceProps = it->surfaceProps;
        }

        for (auto it = ctx.savedGeometries.rbegin(); it != ctx.savedGeometries.rend(); ++it) {
            if (!it->geometry) {
                continue;
            }

            it->geometry->flags = it->flags;
        }

        ctx.savedMaterials.clear();
        ctx.savedGeometries.clear();
    }

    class CToonInspiredSingleCpp {
    public:
        CToonInspiredSingleCpp() {
            Events::gameProcessEvent += [] {
                Instance().ProcessToggle();
                };

            Events::pedRenderEvent.before += [](CPed* ped) {
                Instance().BeforeRender(ped ? ped->m_pRwObject : nullptr);
                };
            Events::pedRenderEvent.after += [](CPed*) {
                Instance().AfterRender();
                };

            Events::vehicleRenderEvent.before += [](CVehicle* vehicle) {
                Instance().BeforeRender(vehicle ? vehicle->m_pRwObject : nullptr);
                };
            Events::vehicleRenderEvent.after += [](CVehicle*) {
                Instance().AfterRender();
                };

            Events::objectRenderEvent.before += [](CObject* object) {
                Instance().BeforeRender(object ? object->m_pRwObject : nullptr);
                };
            Events::objectRenderEvent.after += [](CObject*) {
                Instance().AfterRender();
                };
        }

        static CToonInspiredSingleCpp& Instance() {
            static CToonInspiredSingleCpp pluginInstance;
            return pluginInstance;
        }

    private:
        bool               m_enabled{ true };
        bool               m_renderPatched{};
        unsigned           m_nextToggleTime{};
        RenderPatchContext m_renderCtx{};

        void ProcessToggle() {
            if (CTimer::m_snTimeInMilliseconds < m_nextToggleTime) {
                return;
            }

            if (!KeyPressed(VK_F7)) {
                return;
            }

            m_nextToggleTime = CTimer::m_snTimeInMilliseconds + TOGGLE_COOLDOWN_MS;
            m_enabled = !m_enabled;

            CMessages::AddMessageJumpQ(
                m_enabled ? "CToon-inspired fallback on" : "CToon-inspired fallback off",
                MESSAGE_TIME_MS,
                0,
                false
            );
        }

        void BeforeRender(RwObject* rwObject) {
            if (!m_enabled || m_renderPatched || !rwObject) {
                return;
            }

            ApplyToRwObject(rwObject, m_renderCtx);

            if (m_renderCtx.savedMaterials.empty() && m_renderCtx.savedGeometries.empty()) {
                return;
            }

            DeActivateDirectional();
            SetFullAmbient();
            m_renderPatched = true;
        }

        void AfterRender() {
            if (!m_renderPatched) {
                return;
            }

            RestorePatchedState(m_renderCtx);
            ReSetAmbientAndDirectionalColours();
            m_renderPatched = false;
        }
    };

    static CToonInspiredSingleCpp& gCToonInspiredSingleCpp = CToonInspiredSingleCpp::Instance();

} // namespace
