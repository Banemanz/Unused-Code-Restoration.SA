// Restores a basic on-foot first-person camera path hinted at by
// Partial SRC Leak/PlayerPed.cpp and Partial SRC Leak/Camera.cpp.

#include "plugin.h"
#include "common.h"
#include "CCamera.h"
#include "CMessages.h"
#include "CPad.h"
#include "CPlayerPed.h"
#include "CTimer.h"
#include "eCamMode.h"

using namespace plugin;

namespace {
class RestoredFirstPerson;
static RestoredFirstPerson* gRestoredFirstPersonInstance = nullptr;

class RestoredFirstPerson {
public:
    RestoredFirstPerson() {
        Events::gameProcessEvent += [] {
            if (gRestoredFirstPersonInstance) {
                gRestoredFirstPersonInstance->Process();
            }
        };
        gRestoredFirstPersonInstance = this;
    }

private:

    bool     m_enabled{};
    unsigned m_nextToggleTime{};

    static constexpr unsigned TOGGLE_COOLDOWN_MS = 300;
    static constexpr int CAMERA_CONTROLLER_SCRIPT = 1;

    static bool IsFirstPersonActive() {
        const auto mode = TheCamera.m_aCams[TheCamera.m_nActiveCam].m_nMode;
        return mode == MODE_1STPERSON_RUNABOUT;
    }

    static bool IsWeaponStyleFirstPersonActive() {
        const auto mode = TheCamera.m_aCams[TheCamera.m_nActiveCam].m_nMode;
        switch (mode) {
        case MODE_M16_1STPERSON:
        case MODE_M16_1STPERSON_RUNABOUT:
        case MODE_SNIPER:
        case MODE_SNIPER_RUNABOUT:
        case MODE_ROCKETLAUNCHER:
        case MODE_ROCKETLAUNCHER_RUNABOUT:
        case MODE_ROCKETLAUNCHER_HS:
        case MODE_ROCKETLAUNCHER_RUNABOUT_HS:
        case MODE_HELICANNON_1STPERSON:
        case MODE_CAMERA:
            return true;
        default:
            return false;
        }
    }

    void Process() {
        auto* player = FindPlayerPed();
        if (!player || !player->IsAlive()) {
            Disable();
            return;
        }

        if (CTimer::m_snTimeInMilliseconds >= m_nextToggleTime && KeyPressed('V')) {
            m_nextToggleTime = CTimer::m_snTimeInMilliseconds + TOGGLE_COOLDOWN_MS;
            if (m_enabled) {
                Disable();
            } else {
                Enable(player);
            }
        }

        if (!m_enabled) {
            return;
        }

        if (player->bInVehicle || player->m_pVehicle || IsWeaponStyleFirstPersonActive() || CPad::GetPad(0)->GetTarget()) {
            Disable();
            return;
        }

        // Keep the on-foot runabout first-person path alive while this restore is enabled.
        TheCamera.m_bFirstPersonBeingUsed = true;
        TheCamera.m_nFirstPersonCamLastInputTime = CTimer::m_snTimeInMilliseconds;
        TheCamera.m_bEnable1rstPersonCamCntrlsScript = true;

        if (!IsFirstPersonActive()) {
            TheCamera.TakeControl(player, MODE_1STPERSON_RUNABOUT, SWITCHTYPE_JUMPCUT, CAMERA_CONTROLLER_SCRIPT);
        }
    }

    void Enable(CPlayerPed* player) {
        if (!player || player->bInVehicle || player->m_pVehicle || IsWeaponStyleFirstPersonActive() || CPad::GetPad(0)->GetTarget()) {
            return;
        }

        m_enabled = true;
        TheCamera.Enable1rstPersonCamCntrlsScript();
        TheCamera.m_bFirstPersonBeingUsed = true;
        TheCamera.m_nFirstPersonCamLastInputTime = CTimer::m_snTimeInMilliseconds;
        // Intentionally use the runabout mode, not MODE_M16_1STPERSON / driving 1st-person variants.
        TheCamera.TakeControl(player, MODE_1STPERSON_RUNABOUT, SWITCHTYPE_JUMPCUT, CAMERA_CONTROLLER_SCRIPT);
        CMessages::AddMessageJumpQ("Restored first person on", 1000, 0, false);
    }

    void Disable() {
        if (!m_enabled && !IsFirstPersonActive()) {
            return;
        }

        m_enabled = false;
        TheCamera.m_bFirstPersonBeingUsed = false;
        TheCamera.m_bEnable1rstPersonCamCntrlsScript = false;
        TheCamera.RestoreWithJumpCut();
        CMessages::AddMessageJumpQ("Restored first person off", 1000, 0, false);
    }
};

RestoredFirstPerson gRestoredFirstPerson;
}
