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
    AnimBlendFrameDataEx*    frame{};
    RtQuat                   bindOrientation{};
    RwV3d                    bindTranslation{};
    CVector                  angle{};      // degrees
    CVector                  angularVel{}; // degrees / second
    bool                     active{};
};

static const std::array<BoneDef, 8> kBoneDefs{{
    { BONE_SPINE1,          18.0f, 5.5f, { 0.12f, 0.10f, 0.04f } },
    { BONE_UPPERTORSO,      15.0f, 5.0f, { 0.16f, 0.12f, 0.06f } },
    { BONE_HEAD,            13.5f, 4.0f, { 0.28f, 0.20f, 0.12f } },
    { BONE_LEFTSHOULDER,    10.5f, 3.7f, { 0.18f, 0.10f, 0.22f } },
    { BONE_RIGHTSHOULDER,   10.5f, 3.7f, { 0.18f, 0.10f,-0.22f } },
    { BONE_LEFTELBOW,        9.0f, 3.2f, { 0.22f, 0.06f, 0.18f } },
    { BONE_RIGHTELBOW,       9.0f, 3.2f, { 0.22f, 0.06f,-0.18f } },
    { BONE_HEAD1,           12.5f, 4.5f, { 0.20f, 0.16f, 0.08f } },
}};

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

    outQuat.real   = cr * cp * cy + sr * sp * sy;
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
    out.real   *= invLen;
    return out;
}

static RtQuat BlendQuatShortestPath(const RtQuat& from, const RtQuat& to, float t) {
    RtQuat dst = to;
    const float dot = from.imag.x * dst.imag.x + from.imag.y * dst.imag.y + from.imag.z * dst.imag.z + from.real * dst.real;

    if (dot < 0.0f) {
        dst.imag.x = -dst.imag.x;
        dst.imag.y = -dst.imag.y;
        dst.imag.z = -dst.imag.z;
        dst.real   = -dst.real;
    }

    RtQuat blended{};
    blended.imag.x = from.imag.x + (dst.imag.x - from.imag.x) * t;
    blended.imag.y = from.imag.y + (dst.imag.y - from.imag.y) * t;
    blended.imag.z = from.imag.z + (dst.imag.z - from.imag.z) * t;
    blended.real   = from.real   + (dst.real   - from.real)   * t;
    return NormalizeQuat(blended);
}

static AnimBlendFrameDataEx* FindBoneFrame(CPed* ped, unsigned int boneId) {
    if (!ped || !ped->m_pRwObject) {
        return nullptr;
    }

    auto* frame = RpAnimBlendClumpFindBone(reinterpret_cast<RpClump*>(ped->m_pRwObject), boneId);
    return reinterpret_cast<AnimBlendFrameDataEx*>(frame);
}

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

    bool                      m_enabled{};
    unsigned                  m_nextToggleTime{};
    CPlayerPed*               m_ped{};
    std::array<BoneRuntime, kBoneDefs.size()> m_bones{};

private:
    static bool IsPedUsable(CPlayerPed* ped) {
        return ped && ped->IsAlive() && ped->m_pRwObject && !ped->bInVehicle && !ped->m_pVehicle;
    }

    void Process() {
        auto* player = FindPlayerPed();

        if (CTimer::m_snTimeInMilliseconds >= m_nextToggleTime && KeyPressed(VK_F7)) {
            m_nextToggleTime = CTimer::m_snTimeInMilliseconds + TOGGLE_COOLDOWN_MS;
            if (m_enabled) {
                Disable();
            } else if (IsPedUsable(player)) {
                Enable(player);
            }
        }

        if (!m_enabled) {
            return;
        }

        if (!IsPedUsable(player) || player != m_ped) {
            Disable();
            return;
        }

        float dt = CTimer::ms_fTimeStep / 50.0f;
        dt = std::clamp(dt, 0.0f, MAX_TIME_STEP);

        UpdatePhysics(dt);
        ApplyToKeyframes(dt);
    }

    void Enable(CPlayerPed* ped) {
        if (!IsPedUsable(ped)) {
            return;
        }

        m_ped = ped;
        m_enabled = CaptureBones();

        if (!m_enabled) {
            m_ped = nullptr;
            CMessages::AddMessageJumpQ("Ragdoll setup failed", MESSAGE_TIME_MS, 0, false);
            return;
        }

        const CVector launchImpulse = SamplePedImpulse() + CVector{ 0.30f, 0.0f, 0.65f };
        ApplyImpulse(launchImpulse);
        CMessages::AddMessageJumpQ("Ragdoll prototype on", MESSAGE_TIME_MS, 0, false);
    }

    void Disable() {
        RestoreCapturedBones();
        m_enabled = false;
        m_ped = nullptr;
        CMessages::AddMessageJumpQ("Ragdoll prototype off", MESSAGE_TIME_MS, 0, false);
    }

    bool CaptureBones() {
        bool foundAny = false;

        for (size_t i = 0; i < kBoneDefs.size(); ++i) {
            auto& bone = m_bones[i];
            bone = {};
            bone.def = kBoneDefs[i];
            bone.frame = FindBoneFrame(m_ped, bone.def.boneId);

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

    void RestoreCapturedBones() {
        for (auto& bone : m_bones) {
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

    CVector SamplePedImpulse() const {
        if (!m_ped) {
            return {};
        }

        const CVector moveSpeed = m_ped->m_vecMoveSpeed;
        CVector impulse = moveSpeed * IMPULSE_SCALE;

        if (KeyPressed('H')) {
            const CVector forward = m_ped->GetForward();
            impulse += forward * 16.0f;
            impulse.z += 10.0f;
        }

        return impulse;
    }

    void ApplyImpulse(const CVector& impulse) {
        for (auto& bone : m_bones) {
            if (!bone.active) {
                continue;
            }

            bone.angularVel.x += impulse.y * bone.def.response.x - impulse.z * bone.def.response.y;
            bone.angularVel.y += impulse.x * bone.def.response.y;
            bone.angularVel.z += impulse.x * bone.def.response.z;
        }
    }

    void UpdatePhysics(float dt) {
        const CVector impulse = SamplePedImpulse();
        if (impulse.MagnitudeSqr() > 0.0001f) {
            ApplyImpulse(impulse * dt);
        }

        for (auto& bone : m_bones) {
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

    void ApplyToKeyframes(float dt) {
        const float blend = std::clamp(dt * 12.0f, 0.08f, 0.35f);

        for (auto& bone : m_bones) {
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
