// Reintroduces the partially commented-out mobile-phone path visible in Partial SRC Leak/Ped.cpp
// as a single Plugin-SDK example plugin for GTA SA 1.0 US.

#include "plugin.h"
#include "common.h"
#include "CAnimManager.h"
#include "CMessages.h"
#include "CPad.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CStreaming.h"
#include "CTimer.h"
#include "eAnimations.h"
#include "eModelID.h"
#include "eWeaponType.h"

using namespace plugin;

namespace {
class RestoredMobilePhone;
static RestoredMobilePhone* g_mobilePhoneInstance = nullptr;

class RestoredMobilePhone {
public:
    RestoredMobilePhone() {
        g_mobilePhoneInstance = this;
        Events::gameProcessEvent += [] {
            if (g_mobilePhoneInstance) {
                g_mobilePhoneInstance->Process();
            }
        };
    }

private:
    enum class State {
        Inactive,
        Entering,
        Talking,
        Leaving
    };


    State       m_state{ State::Inactive };
    CPlayerPed* m_ped{};
    eWeaponType m_savedWeapon{ WEAPONTYPE_UNARMED };
    bool        m_phoneVisible{};
    unsigned    m_nextToggleTime{};

    static constexpr unsigned TOGGLE_COOLDOWN_MS = 300;
    static constexpr float PHONE_APPEAR_TIME = 0.85f;
    static constexpr float PHONE_DISAPPEAR_TIME = 0.5f;

    void Process() {
        auto* player = FindPlayerPed();
        if (!player || !player->IsAlive()) {
            ForceCleanup();
            return;
        }

        if (CTimer::m_snTimeInMilliseconds >= m_nextToggleTime && KeyPressed('M')) {
            m_nextToggleTime = CTimer::m_snTimeInMilliseconds + TOGGLE_COOLDOWN_MS;
            if (m_state == State::Inactive) {
                StartCall(player);
            } else {
                StopCall();
            }
        }

        if (m_state == State::Inactive) {
            return;
        }

        if (player != m_ped || !m_ped->m_pRwObject || m_ped->m_pVehicle || m_ped->bInVehicle) {
            ForceCleanup();
            return;
        }

        auto* clump = reinterpret_cast<RpClump*>(m_ped->m_pRwObject);
        auto* phoneIn = RpAnimBlendClumpGetAssociation(clump, ANIM_DEFAULT_PHONE_IN);
        auto* phoneOut = RpAnimBlendClumpGetAssociation(clump, ANIM_DEFAULT_PHONE_OUT);

        if (!m_phoneVisible && phoneIn && phoneIn->m_fCurrentTime >= PHONE_APPEAR_TIME) {
            m_ped->AddWeaponModel(MODEL_CELLPHONE);
            m_phoneVisible = true;
        }

        if (m_phoneVisible && phoneOut && phoneOut->m_fCurrentTime >= PHONE_DISAPPEAR_TIME) {
            HidePhoneAndRestoreWeapon();
        }
    }

    void StartCall(CPlayerPed* player) {
        if (!player || !player->m_pRwObject || player->m_pVehicle || player->bInVehicle) {
            return;
        }

        CStreaming::RequestModel(MODEL_CELLPHONE, GAME_REQUIRED);
        CStreaming::LoadAllRequestedModels(false);

        const eWeaponType currentWeaponType = player->GetWeapon()->m_eWeaponType;

        m_ped = player;
        m_savedWeapon = currentWeaponType;
        m_phoneVisible = false;
        m_state = State::Entering;

        player->RemoveWeaponAnims(currentWeaponType, -4.0f);
        player->RemoveWeaponModel(-1);
        player->SetCurrentWeapon(WEAPONTYPE_UNARMED);

        auto* assoc = CAnimManager::BlendAnimation(reinterpret_cast<RpClump*>(player->m_pRwObject), ANIM_GROUP_DEFAULT, ANIM_DEFAULT_PHONE_IN, 4.0f);
        if (assoc) {
            assoc->SetFinishCallback(OnPhoneInFinished, player);
        }

        CMessages::AddMessageJumpQ("Restored mobile phone on", 1000, 0, false);
    }

    void StopCall() {
        if (!m_ped || !m_ped->m_pRwObject) {
            ForceCleanup();
            return;
        }

        auto* clump = reinterpret_cast<RpClump*>(m_ped->m_pRwObject);
        auto* talk = RpAnimBlendClumpGetAssociation(clump, ANIM_DEFAULT_PHONE_TALK);
        auto* assoc = CAnimManager::BlendAnimation(clump, ANIM_GROUP_DEFAULT, ANIM_DEFAULT_PHONE_OUT, 8.0f);
        m_state = State::Leaving;

        if (talk) {
            talk->SetBlendTo(0.0f, -8.0f);
        }

        if (assoc) {
            assoc->SetFinishCallback(OnPhoneOutFinished, m_ped);
        } else {
            ForceCleanup();
        }

        CMessages::AddMessageJumpQ("Restored mobile phone off", 1000, 0, false);
    }

    void OnPhoneInFinishedImpl(CPlayerPed* player) {
        if (m_state != State::Entering || player != m_ped || !player || !player->m_pRwObject) {
            return;
        }

        CAnimManager::BlendAnimation(reinterpret_cast<RpClump*>(player->m_pRwObject), ANIM_GROUP_DEFAULT, ANIM_DEFAULT_PHONE_TALK, 4.0f);
        m_state = State::Talking;
    }

    void OnPhoneOutFinishedImpl(CPlayerPed* player) {
        if (player != m_ped) {
            return;
        }
        ForceCleanup();
    }

    void ForceCleanup() {
        if (m_ped && m_ped->m_pRwObject) {
            HidePhoneAndRestoreWeapon();
        }

        m_ped = nullptr;
        m_savedWeapon = WEAPONTYPE_UNARMED;
        m_phoneVisible = false;
        m_state = State::Inactive;
    }

    void HidePhoneAndRestoreWeapon() {
        if (!m_ped) {
            return;
        }

        if (m_phoneVisible) {
            m_ped->RemoveWeaponModel(MODEL_CELLPHONE);
            m_phoneVisible = false;
        }

        m_ped->SetCurrentWeapon(m_savedWeapon);
        m_savedWeapon = WEAPONTYPE_UNARMED;
    }

    static void OnPhoneInFinished(CAnimBlendAssociation*, void* data) {
        if (g_mobilePhoneInstance) {
            g_mobilePhoneInstance->OnPhoneInFinishedImpl(static_cast<CPlayerPed*>(data));
        }
    }

    static void OnPhoneOutFinished(CAnimBlendAssociation*, void* data) {
        if (g_mobilePhoneInstance) {
            g_mobilePhoneInstance->OnPhoneOutFinishedImpl(static_cast<CPlayerPed*>(data));
        }
    }
};

static RestoredMobilePhone gRestoredMobilePhone;
}
