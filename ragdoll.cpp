// Best-effort single-file ragdoll-style prototype for GTA SA 1.0 US / Plugin-SDK.
//
// This is intentionally not presented as a faithful recovery of Rockstar's missing
// Ragdoll/BoneNode subsystem. Instead, it ports the broad leaked idea into a
// compile-oriented single .cpp plugin:
// - capture a handful of ped bone keyframes,
// - temporarily take rotation ownership away from normal animation updates,
// - simulate damped angular motion per bone,
// - blend the result back into the ped's current local bone orientations.
//
// Controls:
// - F7: Toggle the prototype on/off for the player ped.
// - H : Apply an extra forward/upward impulse while active.

#include "plugin.h"
#include "common.h"
#include "RenderWare.h"
#include "CPad.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CWorld.h"
#include "CPools.h"
#include "CTimer.h"
#include "CMessages.h"
#include "ePedBones.h"
#include "RpHAnimBlendInterpFrame.h"

#include <array>
#include <cmath>

using namespace plugin;

namespace {
    constexpr unsigned TOGGLE_COOLDOWN_MS = 300;
    constexpr float    MAX_TIME_STEP = 0.033f;
    constexpr float    MAX_ANGLE_DEGREES = 55.0f;
    constexpr float    IMPULSE_SCALE = 60.0f;
    constexpr int      MESSAGE_TIME_MS = 1200;

    // This mirrors the better documented frame layout from gta-reversed.
    struct AnimBlendFrameDataEx {
        union {
            struct {
                unsigned char bf1 : 1;
                unsigned char keyFramesIgnoreNodeOrientation : 1;
                unsigned char keyFramesIgnoreNodeTranslation : 1;
                unsigned char hasVelocity : 1;
                unsigned char hasZVelocity : 1;
                unsigned char needsKeyFrameUpdate : 1;
                unsigned char isCompressed : 1;
                unsigned char isUpdatingFrame : 1;
            } bits;
            unsigned char flags;
        };

        CVector                  bonePos;
        RpHAnimBlendInterpFrame* keyFrame;
        unsigned int             boneTag;
    };
    static_assert(sizeof(AnimBlendFrameDataEx) == 0x18, "AnimBlendFrameDataEx size mismatch");

    struct BoneDef {
        unsigned int boneId;
        float        stiffness;
        float        damping;
        CVector      response;
    };

    struct BoneRuntime {
        BoneDef                  def{};
        AnimBlendFrameDataEx* frame{};
        RtQuat                   bindOrientation{};
        RwV3d                    bindTranslation{};
        CVector                  angle{};      // degrees
        CVector                  angularVel{}; // degrees / second
        bool                     active{};
    };

    struct PedRuntimeState {
        bool         dontAcceptIKLookAts{};
        bool         isLooking{};
        bool         isRestoringLook{};
        bool         isAimingGun{};
        bool         isRestoringGun{};
        bool         canPointGunAtTarget{};
        bool         updateMatricesRequired{};
        unsigned int pedIKFlags{};
        CVector      lastMoveSpeed{};
        bool         wasStanding{};
        bool         wasInAir{};
        float        airTime{};
    };

    static const std::array<BoneDef, 13> kBoneDefs{ {
        { BONE_PELVIS,          26.0f, 7.2f, { 0.08f, 0.08f, 0.02f } },
        { BONE_SPINE1,          20.0f, 6.0f, { 0.12f, 0.10f, 0.04f } },
        { BONE_UPPERTORSO,      16.0f, 5.0f, { 0.16f, 0.12f, 0.06f } },
        { BONE_HEAD1,           13.5f, 4.4f, { 0.20f, 0.16f, 0.08f } },
        { BONE_HEAD,            13.0f, 4.0f, { 0.28f, 0.20f, 0.12f } },
        { BONE_LEFTSHOULDER,    10.5f, 3.7f, { 0.18f, 0.10f, 0.22f } },
        { BONE_RIGHTSHOULDER,   10.5f, 3.7f, { 0.18f, 0.10f,-0.22f } },
        { BONE_LEFTELBOW,        9.0f, 3.2f, { 0.22f, 0.06f, 0.18f } },
        { BONE_RIGHTELBOW,       9.0f, 3.2f, { 0.22f, 0.06f,-0.18f } },
        { BONE_LEFTHIP,         14.0f, 4.6f, { 0.10f, 0.05f, 0.07f } },
        { BONE_RIGHTHIP,        14.0f, 4.6f, { 0.10f, 0.05f,-0.07f } },
        { BONE_LEFTKNEE,        11.5f, 4.0f, { 0.14f, 0.02f, 0.03f } },
        { BONE_RIGHTKNEE,       11.5f, 4.0f, { 0.14f, 0.02f,-0.03f } },
    } };

    static float ToRadians(float degrees) {
        return degrees * 0.01745329251994329577f;
    }

    static CVector ClampVec3(const CVector& v, float minValue, float maxValue) {
        return {
            std::clamp(v.x, minValue, maxValue),
            std::clamp(v.y, minValue, maxValue),
            std::clamp(v.z, minValue, maxValue)
        };
    }

    static void EulerDegreesToQuat(const CVector& anglesDeg, RtQuat& outQuat) {
        const CVector halfRadAngles = {
            ToRadians(anglesDeg.x) * 0.5f,
            ToRadians(anglesDeg.y) * 0.5f,
            ToRadians(anglesDeg.z) * 0.5f
        };

        const float cr = std::cos(halfRadAngles.x);
        const float sr = std::sin(halfRadAngles.x);
        const float cp = std::cos(halfRadAngles.y);
        const float sp = std::sin(halfRadAngles.y);
        const float cy = std::cos(halfRadAngles.z);
        const float sy = std::sin(halfRadAngles.z);

        outQuat.real = cr * cp * cy + sr * sp * sy;
        outQuat.imag.x = sr * cp * cy - cr * sp * sy;
        outQuat.imag.y = cr * sp * cy + sr * cp * sy;
        outQuat.imag.z = cr * cp * sy - sr * sp * cy;
    }

    static RtQuat MulQuat(const RtQuat& a, const RtQuat& b) {
        RtQuat result{};
        RtQuat q1 = a;
        RtQuat q2 = b;
        RtQuatMultiply(&result, &q1, &q2);
        return result;
    }

    static RtQuat NormalizeQuat(const RtQuat& q) {
        RtQuat out = q;
        const float lenSq = out.imag.x * out.imag.x + out.imag.y * out.imag.y + out.imag.z * out.imag.z + out.real * out.real;
        if (lenSq <= 0.000001f) {
            out.imag.x = 0.0f;
            out.imag.y = 0.0f;
            out.imag.z = 0.0f;
            out.real = 1.0f;
            return out;
        }

        const float invLen = 1.0f / std::sqrt(lenSq);
        out.imag.x *= invLen;
        out.imag.y *= invLen;
        out.imag.z *= invLen;
        out.real *= invLen;
        return out;
    }

    static RtQuat BlendQuatShortestPath(const RtQuat& from, const RtQuat& to, float t) {
        RtQuat dst = to;
        const float dot = from.imag.x * dst.imag.x + from.imag.y * dst.imag.y + from.imag.z * dst.imag.z + from.real * dst.real;

        if (dot < 0.0f) {
            dst.imag.x = -dst.imag.x;
            dst.imag.y = -dst.imag.y;
            dst.imag.z = -dst.imag.z;
            dst.real = -dst.real;
        }

        RtQuat blended{};
        blended.imag.x = from.imag.x + (dst.imag.x - from.imag.x) * t;
        blended.imag.y = from.imag.y + (dst.imag.y - from.imag.y) * t;
        blended.imag.z = from.imag.z + (dst.imag.z - from.imag.z) * t;
        blended.real = from.real + (dst.real - from.real) * t;
        return NormalizeQuat(blended);
    }

    static AnimBlendFrameDataEx* FindBoneFrame(CPed* ped, unsigned int boneId) {
        if (!ped || !ped->m_pRwObject) {
            return nullptr;
        }

        auto* frame = RpAnimBlendClumpFindBone(reinterpret_cast<RpClump*>(ped->m_pRwObject), boneId);
        return reinterpret_cast<AnimBlendFrameDataEx*>(frame);
    }

    struct ActiveRagdoll {
        bool                                         enabled{};
        bool                                         manual{};
        CPed* ped{};
        unsigned                                     startedAt{};
        PedRuntimeState                              saved{};
        std::array<BoneRuntime, kBoneDefs.size()>    bones{};
    };

    class RestoredRagdollSingleCpp {
    public:
        RestoredRagdollSingleCpp() {
            Events::gameProcessEvent += [] {
                if (ms_instance) {
                    ms_instance->Process();
                }
                };
            ms_instance = this;
        }

    private:
        static RestoredRagdollSingleCpp* ms_instance;

        static constexpr size_t   MAX_RAGDOLLS = 8;
        static constexpr float    AUTO_RADIUS = 35.0f;
        static constexpr float    KEEP_RADIUS = 55.0f;
        static constexpr unsigned MIN_ACTIVE_MS = 1200;

        unsigned m_nextToggleTime{};
        std::array<ActiveRagdoll, MAX_RAGDOLLS> m_ragdolls{};

    private:
        static bool CanRagdollPed(CPed* ped) {
            return ped && ped->m_pRwObject && !ped->bInVehicle && !ped->m_pVehicle && !ped->bDontRender;
        }

        static bool ShouldAutoActivate(CPed* ped) {
            return ped && (!ped->IsAlive() || ped->bIsInTheAir || ped->bKnockedUpIntoAir || ped->bIsLanding || ped->bFallenDown || ped->bIsDyingStuck);
        }

        static float DistanceToPlayer(const CPed* ped, const CPed* player) {
            if (!ped || !player) {
                return 99999.0f;
            }
            return DistanceBetweenPoints(ped->GetPosition(), player->GetPosition());
        }

        ActiveRagdoll* FindRagdoll(CPed* ped) {
            for (auto& ragdoll : m_ragdolls) {
                if (ragdoll.enabled && ragdoll.ped == ped) {
                    return &ragdoll;
                }
            }
            return nullptr;
        }

        ActiveRagdoll* AllocateRagdoll() {
            for (auto& ragdoll : m_ragdolls) {
                if (!ragdoll.enabled) {
                    return &ragdoll;
                }
            }
            return nullptr;
        }

        void Process() {
            auto* player = FindPlayerPed();

            if (CTimer::m_snTimeInMilliseconds >= m_nextToggleTime && KeyPressed(VK_F7) && player) {
                m_nextToggleTime = CTimer::m_snTimeInMilliseconds + TOGGLE_COOLDOWN_MS;
                if (auto* existing = FindRagdoll(player)) {
                    Disable(*existing, true);
                }
                else {
                    if (auto* slot = AllocateRagdoll()) {
                        Enable(*slot, player, true);
                    }
                }
            }

            AutoActivateNearbyPeds(player);

            for (auto& ragdoll : m_ragdolls) {
                if (!ragdoll.enabled) {
                    continue;
                }
                UpdateOne(ragdoll, player);
            }
        }

        void AutoActivateNearbyPeds(CPed* player) {
            if (!CPools::ms_pPedPool || !player) {
                return;
            }

            for (int i = 0; i < CPools::ms_pPedPool->m_nSize; ++i) {
                auto* ped = CPools::ms_pPedPool->GetAt(i);
                if (!ped || FindRagdoll(ped) || !CanRagdollPed(ped)) {
                    continue;
                }

                if (DistanceToPlayer(ped, player) > AUTO_RADIUS) {
                    continue;
                }

                if (!ShouldAutoActivate(ped)) {
                    continue;
                }

                if (auto* slot = AllocateRagdoll()) {
                    Enable(*slot, ped, ped == player);
                }
                else {
                    return;
                }
            }
        }

        void UpdateOne(ActiveRagdoll& ragdoll, CPed* player) {
            if (!CanRagdollPed(ragdoll.ped)) {
                Disable(ragdoll, ragdoll.manual);
                return;
            }

            if (!ragdoll.manual && player && DistanceToPlayer(ragdoll.ped, player) > KEEP_RADIUS) {
                Disable(ragdoll, false);
                return;
            }

            const bool recovered = ragdoll.ped->IsAlive() && ragdoll.ped->bIsStanding && !ragdoll.ped->bIsInTheAir && !ragdoll.ped->bKnockedUpIntoAir;
            if (!ragdoll.manual && recovered && CTimer::m_snTimeInMilliseconds - ragdoll.startedAt >= MIN_ACTIVE_MS) {
                Disable(ragdoll, false);
                return;
            }

            float dt = CTimer::ms_fTimeStep / 50.0f;
            dt = std::clamp(dt, 0.0f, MAX_TIME_STEP);

            ragdoll.ped->bUpdateMatricesRequired = true;
            UpdatePhysics(ragdoll, dt);
            ApplyToKeyframes(ragdoll, dt);
        }

        void Enable(ActiveRagdoll& ragdoll, CPed* ped, bool manual) {
            if (!CanRagdollPed(ped)) {
                return;
            }

            ragdoll = {};
            ragdoll.ped = ped;
            ragdoll.manual = manual;
            ragdoll.startedAt = CTimer::m_snTimeInMilliseconds;
            ragdoll.enabled = CaptureBones(ragdoll);

            if (!ragdoll.enabled) {
                ragdoll = {};
                if (manual) {
                    CMessages::AddMessageJumpQ("Ragdoll setup failed", MESSAGE_TIME_MS, 0, false);
                }
                return;
            }

            SuppressPedIK(ragdoll);
            ragdoll.saved.lastMoveSpeed = ped->m_vecMoveSpeed;
            ragdoll.saved.wasStanding = ped->bIsStanding;
            ragdoll.saved.wasInAir = ped->bIsInTheAir || ped->bKnockedUpIntoAir;
            ragdoll.saved.airTime = 0.0f;

            const CVector launchImpulse = SamplePedImpulse(ragdoll) + CVector{ 0.30f, 0.0f, 0.65f };
            ApplyImpulse(ragdoll, launchImpulse);

            if (manual) {
                CMessages::AddMessageJumpQ("Ragdoll prototype on", MESSAGE_TIME_MS, 0, false);
            }
        }

        void Disable(ActiveRagdoll& ragdoll, bool showMessage) {
            RestoreCapturedBones(ragdoll);
            RestorePedIK(ragdoll);
            ragdoll = {};
            if (showMessage) {
                CMessages::AddMessageJumpQ("Ragdoll prototype off", MESSAGE_TIME_MS, 0, false);
            }
        }

        void SuppressPedIK(ActiveRagdoll& ragdoll) {
            auto* ped = ragdoll.ped;
            if (!ped || !ped->m_pIntelligence) {
                return;
            }

            ragdoll.saved.dontAcceptIKLookAts = ped->bDontAcceptIKLookAts;
            ragdoll.saved.isLooking = ped->bIsLooking;
            ragdoll.saved.isRestoringLook = ped->bIsRestoringLook;
            ragdoll.saved.isAimingGun = ped->bIsAimingGun;
            ragdoll.saved.isRestoringGun = ped->bIsRestoringGun;
            ragdoll.saved.canPointGunAtTarget = ped->bCanPointGunAtTarget;
            ragdoll.saved.updateMatricesRequired = ped->bUpdateMatricesRequired;
            ragdoll.saved.pedIKFlags = ped->m_pedIK.m_nFlags;

            if (auto* task = ped->m_pIntelligence->m_TaskMgr.GetTaskSecondary(TASK_SECONDARY_IK)) {
                task->MakeAbortable(ped, ABORT_PRIORITY_IMMEDIATE, nullptr);
            }

            ped->bDontAcceptIKLookAts = true;
            ped->bIsLooking = false;
            ped->bIsRestoringLook = false;
            ped->bIsAimingGun = false;
            ped->bIsRestoringGun = false;
            ped->bCanPointGunAtTarget = false;
            ped->bUpdateMatricesRequired = true;
            ped->m_pedIK.m_nFlags = 0;
        }

        void RestorePedIK(ActiveRagdoll& ragdoll) {
            auto* ped = ragdoll.ped;
            if (!ped) {
                return;
            }

            ped->bDontAcceptIKLookAts = ragdoll.saved.dontAcceptIKLookAts;
            ped->bIsLooking = ragdoll.saved.isLooking;
            ped->bIsRestoringLook = ragdoll.saved.isRestoringLook;
            ped->bIsAimingGun = ragdoll.saved.isAimingGun;
            ped->bIsRestoringGun = ragdoll.saved.isRestoringGun;
            ped->bCanPointGunAtTarget = ragdoll.saved.canPointGunAtTarget;
            ped->bUpdateMatricesRequired = ragdoll.saved.updateMatricesRequired;
            ped->m_pedIK.m_nFlags = ragdoll.saved.pedIKFlags;
        }

        bool CaptureBones(ActiveRagdoll& ragdoll) {
            bool foundAny = false;

            for (size_t i = 0; i < kBoneDefs.size(); ++i) {
                auto& bone = ragdoll.bones[i];
                bone = {};
                bone.def = kBoneDefs[i];
                bone.frame = FindBoneFrame(ragdoll.ped, bone.def.boneId);

                if (!bone.frame || !bone.frame->keyFrame) {
                    continue;
                }

                bone.bindOrientation = bone.frame->keyFrame->orientation;
                bone.bindTranslation = bone.frame->keyFrame->translation;
                bone.frame->bits.keyFramesIgnoreNodeOrientation = true;
                bone.frame->bits.keyFramesIgnoreNodeTranslation = true;
                bone.active = true;
                foundAny = true;
            }

            return foundAny;
        }

        void RestoreCapturedBones(ActiveRagdoll& ragdoll) {
            for (auto& bone : ragdoll.bones) {
                if (!bone.active || !bone.frame || !bone.frame->keyFrame) {
                    bone = {};
                    continue;
                }

                bone.frame->keyFrame->orientation = bone.bindOrientation;
                bone.frame->keyFrame->translation = bone.bindTranslation;
                bone.frame->bits.keyFramesIgnoreNodeOrientation = false;
                bone.frame->bits.keyFramesIgnoreNodeTranslation = false;
                bone = {};
            }
        }

        CVector SamplePedImpulse(ActiveRagdoll& ragdoll) {
            if (!ragdoll.ped) {
                return {};
            }

            const CVector moveSpeed = ragdoll.ped->m_vecMoveSpeed;
            const CVector moveDelta = moveSpeed - ragdoll.saved.lastMoveSpeed;
            ragdoll.saved.lastMoveSpeed = moveSpeed;

            CVector impulse = moveSpeed * IMPULSE_SCALE + moveDelta * (IMPULSE_SCALE * 140.0f);

            if (ragdoll.manual && KeyPressed('H')) {
                const CVector forward = ragdoll.ped->GetForward();
                impulse += forward * 16.0f;
                impulse.z += 10.0f;
            }

            return impulse;
        }

        void ApplyImpulse(ActiveRagdoll& ragdoll, const CVector& impulse) {
            for (auto& bone : ragdoll.bones) {
                if (!bone.active) {
                    continue;
                }

                bone.angularVel.x += impulse.y * bone.def.response.x - impulse.z * bone.def.response.y;
                bone.angularVel.y += impulse.x * bone.def.response.y;
                bone.angularVel.z += impulse.x * bone.def.response.z;
            }
        }

        void UpdatePhysics(ActiveRagdoll& ragdoll, float dt) {
            const CVector prevMoveSpeed = ragdoll.saved.lastMoveSpeed;
            CVector impulse = SamplePedImpulse(ragdoll);
            auto* ped = ragdoll.ped;

            const bool inAir = ped->bIsInTheAir || ped->bKnockedUpIntoAir || !ped->bIsStanding;
            if (inAir) {
                ragdoll.saved.airTime += dt;
                impulse.z -= (8.0f + ragdoll.saved.airTime * 18.0f);
            }

            if (!ragdoll.saved.wasStanding && ped->bIsStanding) {
                const float landingShock = std::max(0.0f, -prevMoveSpeed.z) * 28.0f + ragdoll.saved.airTime * 20.0f;
                const CVector forward = ped->GetForward();
                impulse += forward * landingShock;
                impulse.z += landingShock * 0.35f;
                ragdoll.saved.airTime = 0.0f;
            }

            if (!ragdoll.saved.wasInAir && (ped->bIsInTheAir || ped->bKnockedUpIntoAir)) {
                impulse.z += 7.5f;
            }

            ragdoll.saved.wasStanding = ped->bIsStanding;
            ragdoll.saved.wasInAir = ped->bIsInTheAir || ped->bKnockedUpIntoAir;

            if (impulse.MagnitudeSqr() > 0.0001f) {
                ApplyImpulse(ragdoll, impulse * dt);
            }

            for (auto& bone : ragdoll.bones) {
                if (!bone.active) {
                    continue;
                }

                const CVector springAccel = bone.angle * (-bone.def.stiffness);
                const CVector dampAccel = bone.angularVel * (-bone.def.damping);
                bone.angularVel += (springAccel + dampAccel) * dt;
                bone.angle += bone.angularVel * dt;
                bone.angle = ClampVec3(bone.angle, -MAX_ANGLE_DEGREES, MAX_ANGLE_DEGREES);
            }
        }

        void ApplyToKeyframes(ActiveRagdoll& ragdoll, float dt) {
            const float blend = std::clamp(dt * 12.0f, 0.08f, 0.35f);

            for (auto& bone : ragdoll.bones) {
                if (!bone.active || !bone.frame || !bone.frame->keyFrame) {
                    continue;
                }

                RtQuat offsetQuat{};
                EulerDegreesToQuat(bone.angle, offsetQuat);

                RtQuat target = MulQuat(bone.bindOrientation, offsetQuat);
                RtQuat current = bone.frame->keyFrame->orientation;

                bone.frame->keyFrame->orientation = BlendQuatShortestPath(current, target, blend);
                bone.frame->keyFrame->translation = bone.bindTranslation;
            }
        }
    };

    RestoredRagdollSingleCpp* RestoredRagdollSingleCpp::ms_instance = nullptr;
    static RestoredRagdollSingleCpp g_restoredRagdollSingleCpp;
}
