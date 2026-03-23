/*
 * GTA San Andreas - PS2 Multiplayer Revival
 * Single-file plugin-sdk implementation
 *
 * Ported from the partial PS2 source leak (netgames.cpp / gamenet.cpp).
 * Replaces PS2 Network_PS2:: with Winsock2 UDP so it runs on PC.
 *
 * Build: link ws2_32.lib; compile under plugin-sdk (GTA SA 1.0 US).
 *
 * Controls (in-game):
 *   F1        - Host game (start server on default port)
 *   F2        - Join game (connect to IP in config; default 127.0.0.1)
 *   F3        - Open / close chat
 *   F4        - Disconnect / return to single-player
 *   F5-F10    - Select game mode (DM / TDM / CTF / Domination / Cash / RatRace)
 *   Enter     - Send chat message (inside ImGui chat)
 *   Escape    - Close chat window
 *
 * Config file: SA_root\MultiplayerMod.ini
 *   [Network]   Port=7777   ServerIP=127.0.0.1
 *   [Player]    Name=Player  R=255  G=128  B=0
 */

 // ============================================================
 //  INCLUDES
 // ============================================================

 // Winsock2 MUST be included before windows.h (which plugin.h pulls in via injector)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <d3d9.h>
#pragma comment(lib, "ws2_32.lib")

#include "plugin.h"

#include "CWorld.h"
#include "CTimer.h"
#include "CPad.h"
#include "CFont.h"
#include "CRadar.h"
#include "CPickups.h"
#include "CCoronas.h"
#include "CGeneral.h"
#include "CHud.h"
#include "CPlayerInfo.h"
#include "CPlayerPed.h"
#include "CPed.h"
#include "CVehicle.h"
#include "CEntity.h"
#include "ePedType.h"
#include "ePedState.h"
#include "eWeaponType.h"
#include "eModelID.h"
#include "CPickup.h"
#include "CRGBA.h"
#include "RenderWare.h"
#include "CStreaming.h"
#include "CWeapon.h"
#include "CWeaponInfo.h"
#include "eWeaponFire.h"
#include "CAnimManager.h"
#include "eAnimations.h"
#include "CTaskSimpleDuckToggle.h"
#include "CMenuManager.h"
#include "common.h"

#include "imgui.h"
#include "imgui_impl_dx9.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <cstdarg>
#include <algorithm>
#include <array>

using namespace plugin;

// ============================================================
//  FORWARD DECLARATIONS
// ============================================================

static void NetUpdate();
static void NetDraw();
static void SwitchToSinglePlayer();
static bool IsSlotDead(int slot);
static void DrawImGui();
static bool InitImGui();
static void NetBroadcast(const void* data, int size);
static void NetSendToServer(const void* data, int size);
static void FindPlayerMarkerColour(int slot, int* R, int* G, int* B);
static bool IsArmedMoveWeapon(eWeaponType weaponType);

extern "C" IMAGE_DOS_HEADER __ImageBase;

// ============================================================
//  CONSTANTS
// ============================================================

#define MAX_NETPLAYERS      8
#define NET_MAX_NAME_SIZE   24
#define NET_DEFAULT_PORT    7777
#define RECV_BUF_SIZE       2048

// Network status (mirrors the original NETSTAT_ constants)
enum eNetStatus {
    NETSTAT_SINGLEPLAYER = 0,
    NETSTAT_SERVER = 1,
    NETSTAT_CLIENTSTARTINGUP = 2,
    NETSTAT_CLIENTRUNNING = 3,
};

// Game modes (mirrors original GAMETYPE_ constants)
enum eGameType {
    GAMETYPE_DEATHMATCH = 0,
    GAMETYPE_DEATHMATCH_NOBLIPS = 1,
    GAMETYPE_TEAMDEATHMATCH = 2,
    GAMETYPE_TEAMDEATHMATCH_NOBLIPS = 3,
    GAMETYPE_STASHTHECASH = 4,
    GAMETYPE_CAPTURETHEFLAG = 5,
    GAMETYPE_RATRACE = 6,
    GAMETYPE_DOMINATION = 7,
};

// Object types for AddNetworkObject
#define NOBJ_CASHGENERATOR    0
#define NOBJ_STASHLOCATION    1
#define NOBJ_SPAWNPOINT       2
#define NOBJ_CTFBASE          3

#define NUM_TEAMS           2
#define DOMINATIONBASES     3

#define MAX_START_POINTS        8
#define MAX_NUM_CASH_GENERATORS 20
#define MAX_NUM_STASH_LOCATIONS 20
#define MAX_NUM_CTF_BASES       20
#define MAX_NUM_CASH            20
#define MAX_NUM_AMMO            20
#define MAX_NUM_WEAPONS         20
#define MAX_NUM_STORED_PICKUPS  64

// Sync interval (ms)
#define PED_SYNC_INTERVAL_FAST    33
#define PED_SYNC_INTERVAL_IDLE    66
// Timeout (ms)
#define NET_TIMEOUT_MS       6000
#define COOP_MAX_PLAYERS     2
#define COOP_RESPAWN_DELAY_MS 2000
#define COOP_CATCHUP_DISTANCE 140.0f
#define COOP_CATCHUP_INTERVAL_MS 2000
#define COOP_MELEE_RANGE     3.2f
#define COOP_MELEE_VERTICAL_RANGE 1.8f
#define COOP_MELEE_DAMAGE    18.0f
#define COOP_MELEE_COOLDOWN_MS 450

// ============================================================
//  PACKET TYPES  (mirrors MSG_* from original Netmessages)
// ============================================================

enum eNetMsg : unsigned char {
    MSG_SYNC = 0,    // Initial handshake / join
    MSG_PERIODICSYNC = 1,    // Clock sync bounce
    MSG_UPDATEPED = 2,    // Ped position/state
    MSG_PLAYERNAMES = 3,    // Name list broadcast from server
    MSG_TEXT = 4,    // Chat / notification text
    MSG_TEAMPOINTS = 5,    // Team score update
    MSG_STASHTHECASH = 6,    // Stash-the-cash game state
    MSG_CAPTURETHEFLAG = 7,    // CTF flag positions
    MSG_RATRACE = 8,    // Rat-race checkpoint
    MSG_DOMINATIONUPDATE = 9,    // Domination base ownership
    MSG_PLAYERQUIT = 10,   // Client graceful quit
    MSG_RESPAWN = 11,   // Server orders client to respawn
    MSG_MELEEATTACK = 12,   // Client asks server to validate a melee hit
    MSG_DAMAGE = 13,   // Server applies authoritative damage
    MSG_WEAPONATTACK = 14,   // Client asks server to validate a weapon shot
    MSG_ANIMEVENT = 15,   // Explicit one-shot animation event
};

// ============================================================
//  PACKET STRUCTS
// ============================================================

#pragma pack(push, 1)

struct CMsgGeneric {
    eNetMsg Message;
};

// Sent by client to join; bounced back by server with slot/pos info
struct CMsgSync {
    eNetMsg  Message;           // MSG_SYNC
    unsigned int LocalTimeOnSend;
    unsigned int ServerTimeOnBounce;// Filled by server
    int      PlayerNumberOfClient;  // Filled by server (-1 if unknown)
    char     PlayerName[NET_MAX_NAME_SIZE];
    unsigned char R, G, B;
    float    PedCoorsX, PedCoorsY, PedCoorsZ;
};

// Simple clock sync packet
struct CMsgPeriodicSync {
    eNetMsg  Message;           // MSG_PERIODICSYNC
    unsigned int LocalTimeOnSend;
    unsigned int ServerTimeOnBounce;
    int      PlayerNumberOfClient;
};

// Ped position + health update (~10 times/sec)
struct CMsgUpdatePed {
    eNetMsg  Message;           // MSG_UPDATEPED
    int      PlayerSlot;        // which net-player
    float    PosX, PosY, PosZ;
    float    MoveSpeedX, MoveSpeedY, MoveSpeedZ;
    float    Heading;
    float    Health;
    unsigned short ModelIndex;
    unsigned short InputBits;
    short    AnalogLeftRight;
    short    AnalogUpDown;
    unsigned short WeaponAmmoInClip;
    unsigned short WeaponAmmo;
    unsigned char CurrentWeapon;
    unsigned char WeaponState;
    unsigned char FightingStyle;
    unsigned char Ducking;
    unsigned char MoveState;
    unsigned char PedStateFlags; // bit0=inVehicle bit1=dead
};

// Names of all connected players (server→all)
struct CMsgPlayerNames {
    eNetMsg  Message;           // MSG_PLAYERNAMES
    // Packed: for each slot 0..MAX_NETPLAYERS-1, one byte active + name
    unsigned char Active[MAX_NETPLAYERS];
    char     Names[MAX_NETPLAYERS][NET_MAX_NAME_SIZE];
    unsigned char R[MAX_NETPLAYERS], G[MAX_NETPLAYERS], B[MAX_NETPLAYERS];
    signed char Team[MAX_NETPLAYERS];
    int      Points[MAX_NETPLAYERS];
};

// Chat or notification
struct CMsgText {
    eNetMsg  Message;           // MSG_TEXT
    char     String[128];
    unsigned char ColourR, ColourG, ColourB;
};

// Team scores
struct CMsgTeamPoints {
    eNetMsg  Message;           // MSG_TEAMPOINTS
    int      TeamPoints[NUM_TEAMS];
};

// Stash-the-cash state (server→all clients)
struct CMsgStashTheCashUpdate {
    eNetMsg  Message;           // MSG_STASHTHECASH
    float    CashCoorsX, CashCoorsY, CashCoorsZ;
    int      CashCarPlayerSlot; // -1 if free
    int      CurrentStashLocations[NUM_TEAMS];
};

// CTF flag positions (server→all clients)
struct CMsgCaptureTheFlagUpdate {
    eNetMsg  Message;           // MSG_CAPTURETHEFLAG
    float    FlagCoordinates[NUM_TEAMS * 3];
    int      CurrentStashLocations[NUM_TEAMS];
};

// Rat-race checkpoint (server→all clients)
struct CMsgRatRaceUpdate {
    eNetMsg  Message;           // MSG_RATRACE
    float    PickupCoorsX, PickupCoorsY, PickupCoorsZ;
};

// Domination base ownership (server→all clients)
struct CMsgDominationUpdate {
    eNetMsg  Message;           // MSG_DOMINATIONUPDATE
    int      DominationBases[DOMINATIONBASES];
    int      TeamDominatingBases[DOMINATIONBASES];
};

// Client polite quit
struct CMsgPlayerQuit {
    eNetMsg  Message;           // MSG_PLAYERQUIT
    int      PlayerSlot;
};

// Server orders a respawn
struct CMsgRespawn {
    eNetMsg  Message;           // MSG_RESPAWN
    int      PlayerSlot;
    float    PosX, PosY, PosZ;
};

struct CMsgMeleeAttack {
    eNetMsg  Message;           // MSG_MELEEATTACK
    int      TargetSlot;
};

struct CMsgDamage {
    eNetMsg  Message;           // MSG_DAMAGE
    int      TargetSlot;
    int      AttackerSlot;
    float    NewHealth;
    unsigned char WeaponType;
    unsigned char Killed;
};

struct CMsgWeaponAttack {
    eNetMsg  Message;           // MSG_WEAPONATTACK
    int      TargetSlot;
    unsigned char WeaponType;
};

struct CMsgAnimEvent {
    eNetMsg  Message;           // MSG_ANIMEVENT
    int      PlayerSlot;
    unsigned char EventType;
    unsigned char WeaponType;
};

#pragma pack(pop)

// ============================================================
//  NET-PLAYER SLOT  (parallel player info - plugin-sdk's
//  CPlayerInfo doesn't carry Name/bInUse/Team/Points/etc.)
// ============================================================

struct NetPlayer {
    bool          bActive;
    bool          bReportedDead;
    bool          bHandshakeComplete;
    bool          bHasPedSync;
    bool          bVisualsReady;
    char          Name[NET_MAX_NAME_SIZE];
    unsigned char R, G, B;
    signed int    Team;          // -1 = no team
    int           Points;
    int           Points2;       // Secondary score (rat race)
    int           RadarBlip;
    unsigned int  LastMsgTime;   // ms, for timeout detection
    unsigned int  LastEventTime; // ms, for respawn timer
    sockaddr_in   SockAddr;      // (server only) client's UDP address
    CPed* pPed;          // managed ped pointer (NULL if no ped)
    CVector       TargetPos;
    CVector       TargetMoveSpeed;
    float         TargetHeading;
    float         TargetHealth;
    unsigned short ModelIndex;
    unsigned short InputBits;
    short         AnalogLeftRight;
    short         AnalogUpDown;
    short         WeaponModelId;
    unsigned short AnimGroup;
    unsigned short WeaponAmmoInClip;
    unsigned char CurrentWeapon;
    unsigned char WeaponState;
    unsigned char FightingStyle;
    unsigned char Ducking;
    unsigned int  WeaponAmmo;
    unsigned char MoveState;
    short         LastAppliedAnimGroup;
    short         LastAppliedAnimId;
    short         ForcedAnimGroup;
    short         ForcedAnimId;
    unsigned char PendingAnimEvent;
    unsigned char PendingAnimWeapon;
    unsigned int  ForcedAnimUntil;
    std::array<unsigned char, 18> AnimState;

    void Init() {
        bActive = false;
        bReportedDead = false;
        bHandshakeComplete = false;
        bHasPedSync = false;
        bVisualsReady = false;
        Name[0] = '\0';
        R = G = B = 200;
        Team = -1;
        Points = 0;
        Points2 = 0;
        RadarBlip = 0;
        LastMsgTime = 0;
        LastEventTime = 0;
        memset(&SockAddr, 0, sizeof(SockAddr));
        pPed = nullptr;
        TargetPos = CVector(0.0f, 0.0f, 0.0f);
        TargetMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
        TargetHeading = 0.0f;
        TargetHealth = 100.0f;
        ModelIndex = MODEL_MALE01;
        InputBits = 0;
        AnalogLeftRight = 0;
        AnalogUpDown = 0;
        WeaponModelId = -1;
        AnimGroup = 0;
        WeaponAmmoInClip = 0;
        CurrentWeapon = WEAPONTYPE_UNARMED;
        WeaponState = WEAPONSTATE_READY;
        FightingStyle = 4;
        Ducking = 0;
        WeaponAmmo = 0;
        MoveState = PEDMOVE_STILL;
        LastAppliedAnimGroup = -1;
        LastAppliedAnimId = -1;
        ForcedAnimGroup = -1;
        ForcedAnimId = -1;
        PendingAnimEvent = 0;
        PendingAnimWeapon = WEAPONTYPE_UNARMED;
        ForcedAnimUntil = 0;
        AnimState.fill(0);
    }
};

static NetPlayer gNetPlayers[MAX_NETPLAYERS];

// Which slot is OUR local player (client or server-host)
static int gLocalSlot = 0;

// ============================================================
//  GLOBAL NETWORKING STATE
// ============================================================

static int             gNetStatus = NETSTAT_SINGLEPLAYER;
static SOCKET          gSock = INVALID_SOCKET;
static sockaddr_in     gServerAddr;     // (client only) server address
static bool            gWSAStarted = false;
static unsigned char   gRecvBuf[RECV_BUF_SIZE];

// Sync counters (mirrors SyncsDone / TimeDiffWithServer)
static int             gSyncsDone = 0;
static long long       gTimeDiffMS = 0;

static unsigned int    gLastSyncSend = 0;
static unsigned int    gLastPedSend = 0;
static unsigned int    gLastCoopCatchup = 0;
static unsigned int    gLastRemoteDebugLog[MAX_NETPLAYERS] = {};
static CMsgUpdatePed   gLastServerSentPed[MAX_NETPLAYERS] = {};
static unsigned int    gLastServerSentPedTime[MAX_NETPLAYERS] = {};
static CMsgUpdatePed   gLastClientSentPed = {};
static unsigned int    gLastClientSentPedTime = 0;
static unsigned int    gLastMeleeAttemptTime[MAX_NETPLAYERS] = {};
static unsigned int    gLastWeaponAttackTime[MAX_NETPLAYERS] = {};
static unsigned int    gLastObservedLocalAmmo = 0;
static unsigned char   gLastObservedLocalWeapon = WEAPONTYPE_UNARMED;
static unsigned char   gLastObservedLocalWeaponState = WEAPONSTATE_READY;
static unsigned short  gLastObservedLocalInputBits = 0;
static bool            gLastObservedLocalAirborne = false;

// Tracks the last time any UDP packet arrived from the server (client only).
// Kept separate from LastMsgTime (which is per-slot) so we can detect server
// silence even if the local slot's data was never updated.
static unsigned int    gLastServerMsgTime = 0;

static bool            gCoopMode = true;

// ============================================================
//  GLOBAL GAME STATE  (mirrors CNetGames static members)
// ============================================================

static int    gGameType = GAMETYPE_DEATHMATCH;
static int    gGameState = 0;
static int    gTeamPoints[NUM_TEAMS] = { 0, 0 };

static int    gNumStartPoints = 0;
static int    gNumCashGenerators = 0;
static int    gNumStashLocations = 0;
static int    gNumCTFBases = 0;
static int    gNumStoredPickups = 0;

static float  gNetStartCoors[MAX_START_POINTS][3];
static float  gCashGenerators[MAX_NUM_CASH_GENERATORS * 3];
static float  gStashLocations[MAX_NUM_STASH_LOCATIONS * 3];
static float  gCTFBases[MAX_NUM_CTF_BASES * 3];

static int    gCurrentCashGenerator = 0;
static int    gCurrentStashLocs[NUM_TEAMS] = { 0, 0 };
static float  gCashCoorsX = 0.0f;
static float  gCashCoorsY = 0.0f;
static float  gCashCoorsZ = 0.0f;
static int    gCashCarrierSlot = -1;   // which player slot has the cash (-1 = free)

static int    gPlayerCarryingFlag[NUM_TEAMS] = { -1, -1 };
static CVector gFlagCoors[NUM_TEAMS];
static int    gFlagBlip[NUM_TEAMS] = { 0, 0 };

static int    gDomBases[DOMINATIONBASES] = { 0, 0, 0 };
static int    gDomTeam[DOMINATIONBASES] = { -1, -1, -1 };
static int    gDomBlip[DOMINATIONBASES] = { 0, 0, 0 };
static unsigned int gLastDomCheck = 0;

static CVector gRatPickupCoors;

// Persistent blip handles
static int    gTargetBlip = 0;
static int    gBase0Blip = 0;
static int    gBase1Blip = 0;

// Team colours: team 0 = orange, team 1 = blue
static const unsigned int kTeamColours[NUM_TEAMS * 3] = { 255, 128, 0,  0, 100, 255 };
static const unsigned int kBaseColours[NUM_TEAMS * 3] = { 200, 100, 0,  0,  80, 200 };

// ============================================================
//  CONFIG  (read from MultiplayerMod.ini)
// ============================================================

static char  gConfigServerIP[64] = "127.0.0.1";
static int   gConfigPort = NET_DEFAULT_PORT;
static char  gLocalPlayerName[NET_MAX_NAME_SIZE] = "Player";
static unsigned char gLocalR = 255;
static unsigned char gLocalG = 128;
static unsigned char gLocalB = 0;
static float gChatWindowXNorm = 0.02f;
static float gChatWindowYNorm = 0.62f;
static float gChatWindowWNorm = 0.30f;
static float gChatWindowHNorm = 0.24f;
static bool  gChatWindowRectLoaded = false;
static bool  gChatConfigDirty = false;

static char  gPluginDir[MAX_PATH] = {};
static char  gPluginIniPath[MAX_PATH] = {};
static char  gPluginLogPath[MAX_PATH] = {};
static bool  gVerboseNetLog = false;
static bool  gUseReplayVisualSync = false;

static void InitPluginPaths()
{
    if (gPluginDir[0] != '\0')
        return;

    char pluginPath[MAX_PATH] = {};
    if (!GetModuleFileNameA((HMODULE)&__ImageBase, pluginPath, MAX_PATH))
        return;

    char* lastSlash = strrchr(pluginPath, '\\');
    if (!lastSlash)
        return;

    *(lastSlash + 1) = '\0';
    strncpy_s(gPluginDir, pluginPath, _TRUNCATE);

    strcpy_s(gPluginIniPath, gPluginDir);
    strcat_s(gPluginIniPath, "MultiplayerMod.ini");

    strcpy_s(gPluginLogPath, gPluginDir);
    strcat_s(gPluginLogPath, "PS2MultiRev.log");
}

static void LogMessageV(const char* fmt, va_list args)
{
    InitPluginPaths();
    if (gPluginLogPath[0] == '\0')
        return;

    FILE* file = nullptr;
    if (fopen_s(&file, gPluginLogPath, "a") != 0 || !file)
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(file, "[%04u-%02u-%02u %02u:%02u:%02u] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    vfprintf(file, fmt, args);
    fputc('\n', file);
    fclose(file);
}

static void LogMessage(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogMessageV(fmt, args);
    va_end(args);
}

static void LogVerbose(const char* fmt, ...)
{
    if (!gVerboseNetLog)
        return;

    va_list args;
    va_start(args, fmt);
    LogMessageV(fmt, args);
    va_end(args);
}

static void BindLocalPlayerToSlot(int slot)
{
    if (slot < 0 || slot >= MAX_NETPLAYERS)
        return;

    CPlayerPed* localPed = FindPlayerPed();
    for (int i = 0; i < MAX_NETPLAYERS; i++) {
        if (gNetPlayers[i].pPed == localPed)
            gNetPlayers[i].pPed = nullptr;
    }

    gNetPlayers[slot].pPed = localPed;
}

static void LoadConfig()
{
    InitPluginPaths();
    if (gPluginIniPath[0] == '\0')
        return;

    GetPrivateProfileStringA("Network", "ServerIP", "127.0.0.1",
        gConfigServerIP, sizeof(gConfigServerIP), gPluginIniPath);
    gConfigPort = GetPrivateProfileIntA("Network", "Port", NET_DEFAULT_PORT, gPluginIniPath);

    GetPrivateProfileStringA("Player", "Name", "Player",
        gLocalPlayerName, sizeof(gLocalPlayerName), gPluginIniPath);
    gLocalR = (unsigned char)GetPrivateProfileIntA("Player", "R", 255, gPluginIniPath);
    gLocalG = (unsigned char)GetPrivateProfileIntA("Player", "G", 128, gPluginIniPath);
    gLocalB = (unsigned char)GetPrivateProfileIntA("Player", "B", 0, gPluginIniPath);
    gVerboseNetLog = GetPrivateProfileIntA("Debug", "VerboseNetLog", 0, gPluginIniPath) != 0;
    gUseReplayVisualSync = GetPrivateProfileIntA("Debug", "ReplayVisualSync", 0, gPluginIniPath) != 0;

    char chatBuf[64] = {};
    GetPrivateProfileStringA("Chat", "PosXNorm", "0.02", chatBuf, sizeof(chatBuf), gPluginIniPath);
    gChatWindowXNorm = (float)atof(chatBuf);
    GetPrivateProfileStringA("Chat", "PosYNorm", "0.62", chatBuf, sizeof(chatBuf), gPluginIniPath);
    gChatWindowYNorm = (float)atof(chatBuf);
    GetPrivateProfileStringA("Chat", "WidthNorm", "0.30", chatBuf, sizeof(chatBuf), gPluginIniPath);
    gChatWindowWNorm = (float)atof(chatBuf);
    GetPrivateProfileStringA("Chat", "HeightNorm", "0.24", chatBuf, sizeof(chatBuf), gPluginIniPath);
    gChatWindowHNorm = (float)atof(chatBuf);
    gChatWindowXNorm = std::clamp(gChatWindowXNorm, 0.0f, 0.95f);
    gChatWindowYNorm = std::clamp(gChatWindowYNorm, 0.0f, 0.95f);
    gChatWindowWNorm = std::clamp(gChatWindowWNorm, 0.18f, 0.90f);
    gChatWindowHNorm = std::clamp(gChatWindowHNorm, 0.18f, 0.80f);
    gChatWindowRectLoaded = true;

    LogMessage("Config loaded from %s | ServerIP=%s Port=%d Name=%s RGB=(%u,%u,%u)",
        gPluginIniPath, gConfigServerIP, gConfigPort, gLocalPlayerName, gLocalR, gLocalG, gLocalB);
}

static void SaveConfig()
{
    InitPluginPaths();
    if (gPluginIniPath[0] == '\0')
        return;

    WritePrivateProfileStringA("Network", "ServerIP", gConfigServerIP, gPluginIniPath);
    char buf[32];
    sprintf_s(buf, "%d", gConfigPort);
    WritePrivateProfileStringA("Network", "Port", buf, gPluginIniPath);
    WritePrivateProfileStringA("Player", "Name", gLocalPlayerName, gPluginIniPath);
    sprintf_s(buf, "%d", gLocalR); WritePrivateProfileStringA("Player", "R", buf, gPluginIniPath);
    sprintf_s(buf, "%d", gLocalG); WritePrivateProfileStringA("Player", "G", buf, gPluginIniPath);
    sprintf_s(buf, "%d", gLocalB); WritePrivateProfileStringA("Player", "B", buf, gPluginIniPath);
    WritePrivateProfileStringA("Debug", "VerboseNetLog", gVerboseNetLog ? "1" : "0", gPluginIniPath);
    WritePrivateProfileStringA("Debug", "ReplayVisualSync", gUseReplayVisualSync ? "1" : "0", gPluginIniPath);
    sprintf_s(buf, "%.4f", gChatWindowXNorm); WritePrivateProfileStringA("Chat", "PosXNorm", buf, gPluginIniPath);
    sprintf_s(buf, "%.4f", gChatWindowYNorm); WritePrivateProfileStringA("Chat", "PosYNorm", buf, gPluginIniPath);
    sprintf_s(buf, "%.4f", gChatWindowWNorm); WritePrivateProfileStringA("Chat", "WidthNorm", buf, gPluginIniPath);
    sprintf_s(buf, "%.4f", gChatWindowHNorm); WritePrivateProfileStringA("Chat", "HeightNorm", buf, gPluginIniPath);
    gChatConfigDirty = false;

    LogMessage("Config saved to %s", gPluginIniPath);
}

// ============================================================
//  CHAT SYSTEM
// ============================================================

#define CHAT_MAX_LINES      10
#define CHAT_STRING_LEN     100
#define CHAT_DISPLAY_MS     7000

struct ChatLine {
    char     text[CHAT_STRING_LEN];
    unsigned int timestamp;
    unsigned char R, G, B;
};

static ChatLine  gChatHistory[CHAT_MAX_LINES];
static int       gChatHead = 0;           // ring-buffer head
static char      gChatInput[CHAT_STRING_LEN] = {};
static bool      gChatTyping = false;
static bool      gChatWindowOpen = false;
static bool      gChatFocusInput = false;
static bool      gImGuiInitialized = false;
static bool      gChatMouseVisible = false;
static bool      gChatWindowNeedsPlacement = false;
static unsigned int gLastImGuiFrameMs = 0;

static void Chat_AddLine(const char* text, unsigned char R = 200, unsigned char G = 200, unsigned char B = 200)
{
    ChatLine& cl = gChatHistory[gChatHead % CHAT_MAX_LINES];
    strncpy_s(cl.text, text, CHAT_STRING_LEN - 1);
    cl.timestamp = CTimer::m_snTimeInMilliseconds;
    cl.R = R; cl.G = G; cl.B = B;
    gChatHead++;
    LogMessage("Chat [%u,%u,%u]: %s", R, G, B, text);
}

static void CloseChatWindow()
{
    gChatWindowOpen = false;
    gChatTyping = false;
    gChatFocusInput = false;
    gChatInput[0] = '\0';
    FrontEndMenuManager.m_bShowMouse = false;
    gChatMouseVisible = false;
    if (gChatConfigDirty)
        SaveConfig();
}

static void OpenChatWindow()
{
    if (gNetStatus == NETSTAT_SINGLEPLAYER)
        return;
    if (!InitImGui()) {
        LogMessage("OpenChatWindow aborted: ImGui/device not ready");
        return;
    }
    gChatWindowOpen = true;
    gChatTyping = true;
    gChatFocusInput = true;
    gChatWindowNeedsPlacement = true;
    gChatInput[0] = '\0';
    FrontEndMenuManager.m_bShowMouse = true;
    gChatMouseVisible = true;
}

static void SendChatText(const char* input)
{
    if (!input || !input[0] || gNetStatus == NETSTAT_SINGLEPLAYER)
        return;

    char full[CHAT_STRING_LEN];
    sprintf_s(full, "%s: %s", gLocalPlayerName, input);
    int R, G, B;
    FindPlayerMarkerColour(gLocalSlot, &R, &G, &B);

    CMsgText msg = {};
    msg.Message = MSG_TEXT;
    strncpy_s(msg.String, full, sizeof(msg.String) - 1);
    msg.ColourR = (unsigned char)R;
    msg.ColourG = (unsigned char)G;
    msg.ColourB = (unsigned char)B;

    Chat_AddLine(full, (unsigned char)R, (unsigned char)G, (unsigned char)B);
    if (gNetStatus == NETSTAT_SERVER)
        NetBroadcast(&msg, sizeof(msg));
    else
        NetSendToServer(&msg, sizeof(msg));
}

static bool InitImGui()
{
    if (gImGuiInitialized)
        return true;

    auto* device = reinterpret_cast<IDirect3DDevice9*>(GetD3DDevice());
    if (!device)
        return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    if (!ImGui_ImplDX9_Init(device)) {
        ImGui::DestroyContext();
        return false;
    }

    gLastImGuiFrameMs = CTimer::m_snTimeInMilliseconds;
    gImGuiInitialized = true;
    LogMessage("ImGui initialized");
    return true;
}

static void ShutdownImGui()
{
    if (!gImGuiInitialized)
        return;
    ImGui_ImplDX9_Shutdown();
    ImGui::DestroyContext();
    gImGuiInitialized = false;
    LogMessage("ImGui shutdown");
}

static void AddImGuiChatCharacter(unsigned char ch)
{
    if (!gImGuiInitialized || ch == 0)
        return;
    ImGui::GetIO().AddInputCharacter(ch);
}

static char TranslateChatVirtualKey(int vk, bool shiftDown, bool capsLockOn)
{
    if (vk >= 'A' && vk <= 'Z') {
        bool uppercase = shiftDown ^ capsLockOn;
        return (char)(uppercase ? vk : (vk - 'A' + 'a'));
    }

    if (vk >= '0' && vk <= '9') {
        static const char kShiftedDigits[] = { ')', '!', '@', '#', '$', '%', '^', '&', '*', '(' };
        return shiftDown ? kShiftedDigits[vk - '0'] : (char)vk;
    }

    switch (vk) {
    case VK_SPACE: return ' ';
    case VK_OEM_MINUS: return shiftDown ? '_' : '-';
    case VK_OEM_PLUS: return shiftDown ? '+' : '=';
    case VK_OEM_4: return shiftDown ? '{' : '[';
    case VK_OEM_6: return shiftDown ? '}' : ']';
    case VK_OEM_5: return shiftDown ? '|' : '\\';
    case VK_OEM_1: return shiftDown ? ':' : ';';
    case VK_OEM_7: return shiftDown ? '"' : '\'';
    case VK_OEM_COMMA: return shiftDown ? '<' : ',';
    case VK_OEM_PERIOD: return shiftDown ? '>' : '.';
    case VK_OEM_2: return shiftDown ? '?' : '/';
    case VK_OEM_3: return shiftDown ? '~' : '`';
    default: return 0;
    }
}

static void SyncImGuiKeyboardInput()
{
    if (!gImGuiInitialized)
        return;

    ImGuiIO& io = ImGui::GetIO();
    const CKeyboardState& newKeys = CPad::NewKeyState;
    const CKeyboardState& oldKeys = CPad::OldKeyState;

    io.AddKeyEvent(ImGuiKey_Enter, newKeys.enter || newKeys.extenter);
    io.AddKeyEvent(ImGuiKey_Escape, newKeys.esc != 0);
    io.AddKeyEvent(ImGuiKey_Backspace, newKeys.back != 0);
    io.AddKeyEvent(ImGuiKey_LeftArrow, newKeys.left != 0);
    io.AddKeyEvent(ImGuiKey_RightArrow, newKeys.right != 0);
    io.AddKeyEvent(ImGuiKey_UpArrow, newKeys.up != 0);
    io.AddKeyEvent(ImGuiKey_DownArrow, newKeys.down != 0);

    const bool shiftDown = newKeys.shift != 0 || newKeys.lshift != 0 || newKeys.rshift != 0;
    const bool capsLockOn = newKeys.capslock != 0;
    for (int vk = 0; vk < 256; ++vk) {
        if (newKeys.standardKeys[vk] && !oldKeys.standardKeys[vk]) {
            char translated = TranslateChatVirtualKey(vk, shiftDown, capsLockOn);
            if (translated)
                AddImGuiChatCharacter((unsigned char)translated);
        }
    }
}

static void SyncImGuiMouseInput()
{
    if (!gImGuiInitialized)
        return;

    ImGuiIO& io = ImGui::GetIO();
    HWND hwnd = (HWND)RsGlobal.ps->window;
    POINT cursorPos = {};
    if (hwnd && GetCursorPos(&cursorPos) && ScreenToClient(hwnd, &cursorPos))
        io.AddMousePosEvent((float)cursorPos.x, (float)cursorPos.y);
    else
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);

    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);

    float wheelDelta = 0.0f;
    if (CPad::NewMouseControllerState.wheelUp)
        wheelDelta += 1.0f;
    if (CPad::NewMouseControllerState.wheelDown)
        wheelDelta -= 1.0f;
    if (wheelDelta != 0.0f)
        io.AddMouseWheelEvent(0.0f, wheelDelta);
}

static bool BeginImGuiFrame()
{
    if (!InitImGui())
        return false;

    if (!ImGui::GetCurrentContext()) {
        ShutdownImGui();
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    unsigned int now = CTimer::m_snTimeInMilliseconds;
    unsigned int dtMs = (gLastImGuiFrameMs > 0 && now > gLastImGuiFrameMs) ? (now - gLastImGuiFrameMs) : 16;
    gLastImGuiFrameMs = now;
    const float displayWidth = (float)RsGlobal.maximumWidth;
    const float displayHeight = (float)RsGlobal.maximumHeight;
    io.DisplaySize = ImVec2(displayWidth, displayHeight);
    io.DeltaTime = std::max(0.001f, dtMs / 1000.0f);
    const float uiScale = std::clamp(std::min(displayWidth / 1920.0f, displayHeight / 1080.0f), 1.0f, 2.4f);
    io.FontGlobalScale = uiScale;
    io.MouseDrawCursor = gChatWindowOpen;

    SyncImGuiKeyboardInput();
    SyncImGuiMouseInput();
    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();
    return true;
}

static void DrawChatWindow()
{
    if (!gChatWindowOpen)
        return;

    const float displayWidth = (float)RsGlobal.maximumWidth;
    const float displayHeight = (float)RsGlobal.maximumHeight;
    const ImVec2 windowPos(
        std::clamp(gChatWindowXNorm * displayWidth, 0.0f, std::max(0.0f, displayWidth - 120.0f)),
        std::clamp(gChatWindowYNorm * displayHeight, 0.0f, std::max(0.0f, displayHeight - 120.0f)));
    const ImVec2 windowSize(
        std::clamp(gChatWindowWNorm * displayWidth, 380.0f, displayWidth * 0.92f),
        std::clamp(gChatWindowHNorm * displayHeight, 240.0f, displayHeight * 0.82f));

    if (gChatWindowNeedsPlacement) {
        ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
        gChatWindowNeedsPlacement = false;
    }

    bool open = gChatWindowOpen;
    if (ImGui::Begin("PS2MultiRev Chat", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("F3: chat  F4: disconnect");
        ImGui::Separator();

        if (ImGui::BeginChild("ChatHistory", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() - 8.0f), true)) {
            const int lineCount = std::min(gChatHead, CHAT_MAX_LINES);
            for (int i = 0; i < lineCount; ++i) {
                const int idx = (gChatHead - lineCount + i + CHAT_MAX_LINES * 2) % CHAT_MAX_LINES;
                const ChatLine& cl = gChatHistory[idx];
                if (!cl.text[0])
                    continue;
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(cl.R, cl.G, cl.B, 255));
                ImGui::TextWrapped("%s", cl.text);
                ImGui::PopStyleColor();
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        if (gChatFocusInput) {
            ImGui::SetKeyboardFocusHere();
            gChatFocusInput = false;
        }

        ImGui::SetNextItemWidth(-70.0f);
        const bool sendByEnter = ImGui::InputText("##ChatInput", gChatInput, IM_ARRAYSIZE(gChatInput), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        const bool sendByButton = ImGui::Button("Send");
        if ((sendByEnter || sendByButton) && gChatInput[0]) {
            SendChatText(gChatInput);
            gChatInput[0] = '\0';
            gChatFocusInput = true;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            open = false;

        const ImVec2 savedPos = ImGui::GetWindowPos();
        const ImVec2 savedSize = ImGui::GetWindowSize();
        const float nextXNorm = std::clamp(savedPos.x / displayWidth, 0.0f, 0.95f);
        const float nextYNorm = std::clamp(savedPos.y / displayHeight, 0.0f, 0.95f);
        const float nextWNorm = std::clamp(savedSize.x / displayWidth, 0.18f, 0.90f);
        const float nextHNorm = std::clamp(savedSize.y / displayHeight, 0.18f, 0.80f);
        if (!gChatWindowRectLoaded ||
            fabsf(nextXNorm - gChatWindowXNorm) > 0.0005f ||
            fabsf(nextYNorm - gChatWindowYNorm) > 0.0005f ||
            fabsf(nextWNorm - gChatWindowWNorm) > 0.0005f ||
            fabsf(nextHNorm - gChatWindowHNorm) > 0.0005f) {
            gChatWindowXNorm = nextXNorm;
            gChatWindowYNorm = nextYNorm;
            gChatWindowWNorm = nextWNorm;
            gChatWindowHNorm = nextHNorm;
            gChatWindowRectLoaded = true;
            gChatConfigDirty = true;
        }
    }
    ImGui::End();

    gChatWindowOpen = open;
    gChatTyping = gChatWindowOpen;
    if (!gChatWindowOpen)
        CloseChatWindow();
}

// ============================================================
//  HELPER: quick AsciiToGxtChar-free font printing
// ============================================================

static void FontPrint(float x, float y, const char* txt,
    unsigned char R, unsigned char G, unsigned char B, unsigned char A = 255,
    float scaleX = 0.5f, float scaleY = 0.5f)
{
    CFont::SetProportional(true);
    CFont::SetBackground(false, false);
    CFont::SetScale(scaleX, scaleY);
    CFont::SetFontStyle(FONT_SUBTITLES);
    CFont::SetOrientation(ALIGN_LEFT);
    CFont::SetWrapx((float)RsGlobal.maximumWidth);
    CFont::SetColor(CRGBA(R, G, B, A));
    CFont::PrintString(x, y, txt);
}

// ============================================================
//  WINSOCK2 UDP LAYER
// ============================================================

static bool NetInit()
{
    if (gWSAStarted) return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    gWSAStarted = true;
    return true;
}

static void NetShutdown()
{
    if (gSock != INVALID_SOCKET) { closesocket(gSock); gSock = INVALID_SOCKET; }
    if (gWSAStarted) { WSACleanup(); gWSAStarted = false; }
}

// Open a non-blocking UDP socket, optionally bound to localPort
static bool NetOpenSocket(unsigned short localPort)
{
    if (!NetInit()) return false;
    if (gSock != INVALID_SOCKET) { closesocket(gSock); gSock = INVALID_SOCKET; }

    gSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (gSock == INVALID_SOCKET) return false;

    // Non-blocking
    u_long nonblocking = 1;
    if (ioctlsocket(gSock, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        closesocket(gSock);
        gSock = INVALID_SOCKET;
        return false;
    }

    // Bind
    sockaddr_in bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port = htons(localPort);
    if (bind(gSock, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        closesocket(gSock);
        gSock = INVALID_SOCKET;
        return false;
    }
    return true;
}

// Send raw bytes to a target address
static void NetSendTo(const void* data, int size, const sockaddr_in& addr)
{
    if (gSock == INVALID_SOCKET) return;
    sendto(gSock, (const char*)data, size, 0,
        (const sockaddr*)&addr, sizeof(sockaddr_in));
}

// Send to all active remote player sockets (server only)
static void NetBroadcast(const void* data, int size)
{
    for (int i = 0; i < MAX_NETPLAYERS; i++) {
        if (i == gLocalSlot) continue;
        if (gNetPlayers[i].bActive && gNetPlayers[i].SockAddr.sin_port != 0)
            NetSendTo(data, size, gNetPlayers[i].SockAddr);
    }
}

// Send to server (client only)
static void NetSendToServer(const void* data, int size)
{
    NetSendTo(data, size, gServerAddr);
}

// Poll the socket; fills gRecvBuf. Returns size received, 0 if nothing, -1 on error.
// fromAddr is filled with the sender address.
static int NetRecv(sockaddr_in& fromAddr)
{
    if (gSock == INVALID_SOCKET) return -1;
    int fromLen = sizeof(fromAddr);
    int r = recvfrom(gSock, (char*)gRecvBuf, RECV_BUF_SIZE, 0,
        (sockaddr*)&fromAddr, &fromLen);
    if (r == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return 0;
        return -1;
    }
    return r;
}

// ============================================================
//  PLAYER MANAGEMENT HELPERS
// ============================================================

// Find a slot matching a sockaddr_in (server: find existing client)
static int FindSlotBySockAddr(const sockaddr_in& addr)
{
    for (int i = 1; i < MAX_NETPLAYERS; i++) {  // slot 0 = server/local
        if (gNetPlayers[i].bActive &&
            gNetPlayers[i].SockAddr.sin_addr.s_addr == addr.sin_addr.s_addr &&
            gNetPlayers[i].SockAddr.sin_port == addr.sin_port)
            return i;
    }
    return -1;
}

// Allocate a fresh client slot (server)
static int AllocClientSlot(const sockaddr_in& addr)
{
    for (int i = 1; i < MAX_NETPLAYERS; i++) {
        if (!gNetPlayers[i].bActive) {
            gNetPlayers[i].Init();
            gNetPlayers[i].bActive = true;
            gNetPlayers[i].SockAddr = addr;
            gNetPlayers[i].LastMsgTime = CTimer::m_snTimeInMilliseconds;
            return i;
        }
    }
    return -1;
}

static int CountActivePlayers()
{
    int count = 0;
    for (int i = 0; i < MAX_NETPLAYERS; i++) {
        if (gNetPlayers[i].bActive)
            count++;
    }
    return count;
}

static int GetOtherActiveSlot(int slot)
{
    for (int i = 0; i < MAX_NETPLAYERS; i++) {
        if (i != slot && gNetPlayers[i].bActive)
            return i;
    }
    return -1;
}

static bool IsPedDead(const CPed* ped)
{
    if (!ped)
        return true;
    return ped->m_fHealth <= 0.0f ||
        ped->m_ePedState == PEDSTATE_DEAD ||
        ped->m_ePedState == PEDSTATE_DIE;
}

static unsigned char GetLocalMoveState(const CPed* ped)
{
    if (!ped)
        return PEDMOVE_STILL;

    if (ped->m_nMoveState >= PEDMOVE_STILL && ped->m_nMoveState <= PEDMOVE_SPRINT)
        return (unsigned char)ped->m_nMoveState;

    const float speed = ped->m_vecMoveSpeed.Magnitude();
    if (speed < 0.01f) return PEDMOVE_STILL;
    if (speed < 0.05f) return PEDMOVE_WALK;
    if (speed < 0.08f) return PEDMOVE_JOG;
    if (speed < 0.12f) return PEDMOVE_RUN;
    return PEDMOVE_SPRINT;
}

enum eNetInputBits : unsigned short {
    NETINPUT_TARGET = 1 << 0,
    NETINPUT_FIRE = 1 << 1,
    NETINPUT_JUMP = 1 << 2,
    NETINPUT_SPRINT = 1 << 3,
    NETINPUT_DUCK = 1 << 4,
    NETINPUT_MELEE = 1 << 5,
    NETINPUT_HANDBRAKE = 1 << 6,
    NETINPUT_HORN = 1 << 7,
};

enum eNetAnimEvent : unsigned char {
    NETANIM_NONE = 0,
    NETANIM_MELEE_SWING = 1,
    NETANIM_JUMP_LAUNCH = 2,
    NETANIM_JUMP_LAND = 3,
    NETANIM_HIT_REACT = 4,
    NETANIM_FIRE = 5,
};

static unsigned short CollectLocalInputBits(const CPed* ped)
{
    CPad* pad = CPad::GetPad(0);
    if (!ped || !pad)
        return 0;

    unsigned short bits = 0;
    if (pad->GetTarget()) bits |= NETINPUT_TARGET;
    if (pad->GetJump()) bits |= NETINPUT_JUMP;
    if (pad->GetSprint()) bits |= NETINPUT_SPRINT;
    if (pad->GetDuck()) bits |= NETINPUT_DUCK;
    if (pad->GetMeleeAttack()) bits |= NETINPUT_MELEE;
    if (pad->GetHandBrake()) bits |= NETINPUT_HANDBRAKE;
    if (pad->GetHorn()) bits |= NETINPUT_HORN;

    if (ped->bInVehicle) {
        if (pad->GetCarGunFired())
            bits |= NETINPUT_FIRE;
    } else {
        CWeapon* weapon = const_cast<CPed*>(ped)->GetWeapon();
        if (weapon && weapon->m_nState == WEAPONSTATE_FIRING)
            bits |= NETINPUT_FIRE;
    }

    return bits;
}

static unsigned int GetDesiredPedSyncInterval(const CPed* ped)
{
    if (!ped)
        return PED_SYNC_INTERVAL_IDLE;

    const float speed = ped->m_vecMoveSpeed.Magnitude();
    const bool moving = speed > 0.01f || ped->m_nMoveState >= PEDMOVE_WALK || ped->bInVehicle;
    return moving ? PED_SYNC_INTERVAL_FAST : PED_SYNC_INTERVAL_IDLE;
}

static bool UpdatePedNeedsSend(const CMsgUpdatePed& current, const CMsgUpdatePed& previous, unsigned int now, unsigned int lastSendTime)
{
    static const unsigned int kKeepaliveMs = 200;
    if (lastSendTime == 0 || now - lastSendTime >= kKeepaliveMs)
        return true;

    auto absf = [](float v) { return v >= 0.0f ? v : -v; };
    if (absf(current.PosX - previous.PosX) > 0.02f) return true;
    if (absf(current.PosY - previous.PosY) > 0.02f) return true;
    if (absf(current.PosZ - previous.PosZ) > 0.03f) return true;
    if (absf(current.MoveSpeedX - previous.MoveSpeedX) > 0.004f) return true;
    if (absf(current.MoveSpeedY - previous.MoveSpeedY) > 0.004f) return true;
    if (absf(current.MoveSpeedZ - previous.MoveSpeedZ) > 0.006f) return true;
    if (absf(current.Heading - previous.Heading) > 0.02f) return true;
    if (absf(current.Health - previous.Health) > 0.5f) return true;
    if (current.ModelIndex != previous.ModelIndex) return true;
    if (current.InputBits != previous.InputBits) return true;
    if (current.AnalogLeftRight != previous.AnalogLeftRight) return true;
    if (current.AnalogUpDown != previous.AnalogUpDown) return true;
    if (current.WeaponAmmoInClip != previous.WeaponAmmoInClip) return true;
    if (current.WeaponAmmo != previous.WeaponAmmo) return true;
    if (current.CurrentWeapon != previous.CurrentWeapon) return true;
    if (current.WeaponState != previous.WeaponState) return true;
    if (current.FightingStyle != previous.FightingStyle) return true;
    if (current.Ducking != previous.Ducking) return true;
    if (current.MoveState != previous.MoveState) return true;
    if (current.PedStateFlags != previous.PedStateFlags) return true;
    return false;
}

static CPed* GetSlotPed(int slot)
{
    if (slot < 0 || slot >= MAX_NETPLAYERS || !gNetPlayers[slot].bActive)
        return nullptr;

    if (slot == gLocalSlot) {
        if (gNetStatus == NETSTAT_SERVER || gNetStatus == NETSTAT_CLIENTRUNNING)
            return FindPlayerPed();
    }

    return gNetPlayers[slot].pPed;
}

static CVector GetSlotAuthoritativePosition(int slot)
{
    const CPed* ped = GetSlotPed(slot);
    if (ped)
        return ped->GetPosition();
    if (slot >= 0 && slot < MAX_NETPLAYERS)
        return gNetPlayers[slot].TargetPos;
    return CVector(0.0f, 0.0f, 0.0f);
}

static float GetSlotAuthoritativeHealth(int slot)
{
    const CPed* ped = GetSlotPed(slot);
    if (ped)
        return ped->m_fHealth;
    if (slot >= 0 && slot < MAX_NETPLAYERS)
        return gNetPlayers[slot].TargetHealth;
    return 0.0f;
}

static bool IsPedUsingMelee(CPed* ped)
{
    if (!ped || ped->bInVehicle)
        return false;

    CWeapon* weapon = ped->GetWeapon();
    if (!weapon)
        return true;

    return weapon->m_eWeaponType == WEAPONTYPE_UNARMED || weapon->IsTypeMelee();
}

static unsigned char GetPedCombatWeaponType(CPed* ped)
{
    if (!ped)
        return WEAPONTYPE_UNARMED;

    CWeapon* weapon = ped->GetWeapon();
    if (!weapon)
        return WEAPONTYPE_UNARMED;

    return (unsigned char)weapon->m_eWeaponType;
}

static CWeaponInfo* GetPedCombatWeaponInfo(CPed* ped, eWeaponType weaponType)
{
    if (!ped)
        return CWeaponInfo::GetWeaponInfo(weaponType, WEAPSKILL_STD);
    return CWeaponInfo::GetWeaponInfo(weaponType, (unsigned char)ped->GetWeaponSkill(weaponType));
}

static bool CanAttemptCoopMeleeHit(int attackerSlot, int targetSlot)
{
    if (!gCoopMode || gNetStatus == NETSTAT_SINGLEPLAYER)
        return false;
    if (attackerSlot < 0 || attackerSlot >= MAX_NETPLAYERS || targetSlot < 0 || targetSlot >= MAX_NETPLAYERS)
        return false;
    if (attackerSlot == targetSlot || !gNetPlayers[attackerSlot].bActive || !gNetPlayers[targetSlot].bActive)
        return false;
    if (IsSlotDead(attackerSlot) || IsSlotDead(targetSlot))
        return false;

    CPed* attackerPed = GetSlotPed(attackerSlot);
    if (!IsPedUsingMelee(attackerPed))
        return false;

    const CVector attackerPos = GetSlotAuthoritativePosition(attackerSlot);
    const CVector targetPos = GetSlotAuthoritativePosition(targetSlot);
    const CVector diff = targetPos - attackerPos;
    if (std::fabs(diff.z) > COOP_MELEE_VERTICAL_RANGE)
        return false;

    CVector flatDiff = diff;
    flatDiff.z = 0.0f;
    return flatDiff.Magnitude() <= COOP_MELEE_RANGE;
}

static bool CanAttemptCoopWeaponHit(int attackerSlot, int targetSlot, eWeaponType weaponType, float maxRangeOverride = 0.0f)
{
    if (!gCoopMode || gNetStatus == NETSTAT_SINGLEPLAYER)
        return false;
    if (attackerSlot < 0 || attackerSlot >= MAX_NETPLAYERS || targetSlot < 0 || targetSlot >= MAX_NETPLAYERS)
        return false;
    if (attackerSlot == targetSlot || !gNetPlayers[attackerSlot].bActive || !gNetPlayers[targetSlot].bActive)
        return false;
    if (IsSlotDead(attackerSlot) || IsSlotDead(targetSlot))
        return false;

    CPed* attackerPed = GetSlotPed(attackerSlot);
    if (!attackerPed)
        return false;

    const CVector attackerPos = GetSlotAuthoritativePosition(attackerSlot);
    const CVector targetPos = GetSlotAuthoritativePosition(targetSlot);
    CVector toTarget = targetPos - attackerPos;
    const float distance = toTarget.Magnitude();

    float maxRange = maxRangeOverride;
    if (maxRange <= 0.0f) {
        if (CWeaponInfo* info = GetPedCombatWeaponInfo(attackerPed, weaponType))
            maxRange = info->m_fWeaponRange;
    }
    if (maxRange <= 0.0f)
        maxRange = 25.0f;
    if (distance > maxRange + 1.0f)
        return false;

    toTarget.Normalise();
    CVector forward = attackerPed->GetForward();
    forward.Normalise();
    if (DotProduct(forward, toTarget) < 0.35f)
        return false;

    CColPoint colPoint;
    CEntity* hitEntity = nullptr;
    const CVector origin = attackerPos + CVector(0.0f, 0.0f, 0.7f);
    const CVector target = targetPos + CVector(0.0f, 0.0f, 0.7f);
    if (CWorld::ProcessLineOfSight(origin, target, colPoint, hitEntity, true, true, false, true, true, false, true, true))
        return false;

    return true;
}

static bool IsWeaponContinuousFire(eWeaponType weaponType)
{
    return weaponType == WEAPONTYPE_FTHROWER ||
        weaponType == WEAPONTYPE_SPRAYCAN ||
        weaponType == WEAPONTYPE_EXTINGUISHER;
}

static bool IsSafeRemoteWeaponSyncType(eWeaponType weaponType)
{
    switch (weaponType) {
    case WEAPONTYPE_UNARMED:
    case WEAPONTYPE_BRASSKNUCKLE:
    case WEAPONTYPE_GOLFCLUB:
    case WEAPONTYPE_NIGHTSTICK:
    case WEAPONTYPE_KNIFE:
    case WEAPONTYPE_BASEBALLBAT:
    case WEAPONTYPE_SHOVEL:
    case WEAPONTYPE_POOLCUE:
    case WEAPONTYPE_KATANA:
    case WEAPONTYPE_CHAINSAW:
    case WEAPONTYPE_DILDO1:
    case WEAPONTYPE_DILDO2:
    case WEAPONTYPE_VIBE1:
    case WEAPONTYPE_VIBE2:
    case WEAPONTYPE_FLOWERS:
    case WEAPONTYPE_CANE:
    case WEAPONTYPE_PISTOL:
    case WEAPONTYPE_PISTOL_SILENCED:
    case WEAPONTYPE_DESERT_EAGLE:
    case WEAPONTYPE_SHOTGUN:
    case WEAPONTYPE_SAWNOFF:
    case WEAPONTYPE_SPAS12:
    case WEAPONTYPE_MICRO_UZI:
    case WEAPONTYPE_MP5:
    case WEAPONTYPE_AK47:
    case WEAPONTYPE_M4:
    case WEAPONTYPE_TEC9:
    case WEAPONTYPE_COUNTRYRIFLE:
    case WEAPONTYPE_SNIPERRIFLE:
        return true;
    default:
        return false;
    }
}

static unsigned int GetWeaponAttackCooldownMs(eWeaponType weaponType, CWeaponInfo* weaponInfo)
{
    if (IsWeaponContinuousFire(weaponType))
        return 250;
    if (weaponType == WEAPONTYPE_SHOTGUN || weaponType == WEAPONTYPE_SAWNOFF || weaponType == WEAPONTYPE_SPAS12)
        return 700;
    if (weaponType == WEAPONTYPE_SNIPERRIFLE || weaponType == WEAPONTYPE_COUNTRYRIFLE || weaponType == WEAPONTYPE_RLAUNCHER ||
        weaponType == WEAPONTYPE_RLAUNCHER_HS || weaponType == WEAPONTYPE_GRENADE || weaponType == WEAPONTYPE_TEARGAS ||
        weaponType == WEAPONTYPE_MOLOTOV || weaponType == WEAPONTYPE_SATCHEL_CHARGE)
        return 900;
    if (weaponInfo && weaponInfo->m_nWeaponFire == WEAPON_FIRE_AREA_EFFECT)
        return 350;
    return 120;
}

struct NetAnimState {
    short AnimId;
    unsigned char Time;
    unsigned char Speed;
    unsigned char GroupId1;
    unsigned char GroupId2;
};

static NetAnimState MakeNetAnimState(short animId, float time, float speed, unsigned char groupId)
{
    NetAnimState ret = {};
    ret.AnimId = animId;
    ret.Time = (unsigned char)(std::clamp(time, 0.0f, 4.0f) * 63.75f);
    ret.Speed = (unsigned char)(std::clamp(speed, 0.0f, 3.0f) * 85.0f);
    ret.GroupId1 = groupId;
    return ret;
}

static NetAnimState MakeNetAnimBlendState(short animId, float time, float speed, unsigned char groupId, float blend)
{
    NetAnimState ret = {};
    ret.AnimId = animId;
    ret.Time = (unsigned char)(std::clamp(time, 0.0f, 4.0f) * 63.75f);
    ret.Speed = (unsigned char)(std::clamp(speed, 0.0f, 3.0f) * 85.0f);
    ret.GroupId1 = (unsigned char)(std::clamp(blend, 0.0f, 2.0f) * 127.5f);
    ret.GroupId2 = groupId;
    return ret;
}

static void StoreLocalPedVisualState(CPed* ped, CMsgUpdatePed& msg)
{
    (void)ped;
    (void)msg;
}

static void ApplyRemotePedAnimation(CPed* ped, NetPlayer& player)
{
    if (!ped || !ped->m_pRwClump)
        return;

    const float kAnimTimeScale = 63.75f;
    const float kAnimSpeedScale = 85.0f;
    const float kAnimBlendScale = 127.5f;
    const NetAnimState* states = reinterpret_cast<const NetAnimState*>(player.AnimState.data());

    if (player.AnimGroup != 0 && ped->m_nAnimGroup != player.AnimGroup)
        ped->m_nAnimGroup = player.AnimGroup;

    CAnimBlendAssociation* anim = nullptr;
    if (states[0].AnimId > 3) {
        anim = CAnimManager::BlendAnimation(ped->m_pRwClump, states[0].GroupId1, states[0].AnimId, 100.0f);
    } else {
        anim = CAnimManager::BlendAnimation(ped->m_pRwClump, ped->m_nAnimGroup, states[0].AnimId, 100.0f);
    }
    if (anim) {
        anim->SetCurrentTime(states[0].Time / kAnimTimeScale);
        anim->m_fSpeed = states[0].Speed / kAnimSpeedScale;
        anim->SetBlend(1.0f, 1.0f);
        anim->m_nCallbackType = (eAnimBlendCallbackType)0;
    }

    if (states[1].GroupId1 && states[1].AnimId) {
        anim = CAnimManager::BlendAnimation(
            ped->m_pRwClump,
            states[1].AnimId > 3 ? states[1].GroupId2 : ped->m_nAnimGroup,
            states[1].AnimId,
            100.0f
        );
        if (anim) {
            anim->SetCurrentTime(states[1].Time / kAnimTimeScale);
            anim->m_fSpeed = states[1].Speed / kAnimSpeedScale;
            anim->SetBlend(states[1].GroupId1 / kAnimBlendScale, 1.0f);
            anim->m_nCallbackType = (eAnimBlendCallbackType)0;
        }
    }

    RpAnimBlendClumpRemoveAssociations(ped->m_pRwClump, ANIMATION_PARTIAL);
    if (states[2].GroupId1 && states[2].AnimId && states[2].AnimId != 3) {
        anim = CAnimManager::BlendAnimation(ped->m_pRwClump, states[2].GroupId2, states[2].AnimId, 1000.0f);
        if (anim) {
            anim->SetCurrentTime(states[2].Time / kAnimTimeScale);
            anim->m_fSpeed = states[2].Speed / kAnimSpeedScale;
            anim->SetBlend(states[2].GroupId1 / kAnimBlendScale, 0.0f);
            anim->m_nCallbackType = (eAnimBlendCallbackType)0;
        }
    }
}

static void ApplyRemotePedWeaponModel(CPed* ped, int weaponModelId)
{
    if (!ped)
        return;

    ped->RemoveWeaponModel(-1);
    if (weaponModelId < 0)
        return;

    if (CStreaming::ms_aInfoForModel[weaponModelId].m_nLoadState == 1) {
        ped->AddWeaponModel(weaponModelId);
    } else {
        CStreaming::RequestModel((unsigned int)weaponModelId, 2);
    }
}

static unsigned short SanitizeRemotePedModel(unsigned short modelIndex)
{
    // Remote players are represented by generic CPed instances, not player peds.
    // CJ / MODEL_NULL / CSPLAY are unsafe here and can crash on SetModelIndex.
    if (modelIndex == MODEL_NULL || modelIndex == MODEL_CSPLAY)
        return MODEL_MALE01;
    return modelIndex;
}

static CVector AdjustSpawnPositionAboveGround(const CVector& pos)
{
    bool foundGround = false;
    CEntity* groundEntity = nullptr;
    float groundZ = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 4.0f, &foundGround, &groundEntity);

    CVector adjusted = pos;
    if (foundGround && groundZ > -100.0f)
        adjusted.z = std::max(adjusted.z, groundZ + 1.2f);
    else
        adjusted.z += 1.2f;

    return adjusted;
}

static void EnsureRemotePedModel(CPed* ped, unsigned short modelIndex)
{
    modelIndex = SanitizeRemotePedModel(modelIndex);
    if (!ped || ped->m_nModelIndex == modelIndex)
        return;

    CStreaming::RequestModel(modelIndex, 2);
    CStreaming::LoadAllRequestedModels(false);
    ped->SetModelIndex(modelIndex);
}

static void EnsureRemotePedWeapon(CPed* ped, unsigned char weaponType, unsigned int ammo, unsigned short ammoInClip, unsigned char weaponState)
{
    if (!ped)
        return;

    const eWeaponType desiredWeapon = (eWeaponType)weaponType;
    const CWeapon* currentWeapon = ped->GetWeapon();
    if (desiredWeapon == WEAPONTYPE_UNARMED) {
        if (currentWeapon->m_eWeaponType != WEAPONTYPE_UNARMED) {
            ped->ClearWeapons();
            if (ped->m_nWeaponModelId > 0)
                ped->RemoveWeaponModel(ped->m_nWeaponModelId);
            ped->m_nWeaponModelId = -1;
        }
        return;
    }

    if (currentWeapon->m_eWeaponType != desiredWeapon) {
        if (ped->m_nWeaponModelId > 0)
            ped->RemoveWeaponModel(ped->m_nWeaponModelId);
        ped->ClearWeapons();
        ped->GiveWeapon(desiredWeapon, std::max(1u, ammo), false);
        ped->SetCurrentWeapon(desiredWeapon);
    } else {
        ped->SetAmmo(desiredWeapon, std::max(1u, ammo));
        ped->SetCurrentWeapon(desiredWeapon);
    }

    CWeapon* appliedSlotWeapon = ped->GetWeapon();
    if (appliedSlotWeapon) {
        appliedSlotWeapon->m_nAmmoInClip = std::min<unsigned int>(std::max<unsigned int>(1u, ammoInClip), appliedSlotWeapon->m_nAmmoTotal);
        appliedSlotWeapon->m_nState = (eWeaponState)weaponState;
    }

    CWeaponInfo* weaponInfo = CWeaponInfo::GetWeaponInfo(desiredWeapon);
    if (weaponInfo && weaponInfo->m_nModelId > 0 && ped->m_nWeaponModelId != weaponInfo->m_nModelId) {
        CStreaming::RequestModel(weaponInfo->m_nModelId, 2);
        CStreaming::LoadAllRequestedModels(false);
        ped->AddWeaponModel(weaponInfo->m_nModelId);
        ped->m_nWeaponModelId = weaponInfo->m_nModelId;
    }

    const CWeapon* appliedWeapon = ped->GetWeapon();
    LogVerbose("EnsureRemotePedWeapon result ped=%p desired=%u current=%u ammo=%u clip=%u state=%u modelId=%d",
        ped, (unsigned int)desiredWeapon, (unsigned int)appliedWeapon->m_eWeaponType,
        appliedWeapon->m_nAmmoTotal, appliedWeapon->m_nAmmoInClip, appliedWeapon->m_nState, ped->m_nWeaponModelId);
}

static void ApplyRemotePedDucking(CPed* ped, NetPlayer& player)
{
    if (!ped)
        return;

    const bool shouldDuck = player.Ducking != 0;
    if (ped->bIsDucking == shouldDuck)
        return;

    CTaskSimpleDuckToggle task(shouldDuck ? 1 : 0);
    task.ProcessPed(ped);
    LogVerbose("UpdateRemotePlayerVisual step=duck ped=%p duck=%u", ped, player.Ducking);
}

static bool ResolveRemoteAnimEvent(unsigned char eventType, unsigned char weaponType, int& animGroup, int& animId, unsigned int& holdMs)
{
    animGroup = ANIM_GROUP_DEFAULT;
    holdMs = 300;

    switch (eventType) {
    case NETANIM_MELEE_SWING:
        animId = ANIM_DEFAULT_FIGHTSHF;
        holdMs = 280;
        return true;
    case NETANIM_JUMP_LAUNCH:
        animId = ANIM_DEFAULT_JUMP_LAUNCH;
        holdMs = 420;
        return true;
    case NETANIM_JUMP_LAND:
        animId = ANIM_DEFAULT_JUMP_LAND;
        holdMs = 260;
        return true;
    case NETANIM_HIT_REACT:
        animId = ANIM_DEFAULT_HIT_FRONT;
        holdMs = 420;
        return true;
    case NETANIM_FIRE:
        if (IsArmedMoveWeapon((eWeaponType)weaponType)) {
            animId = ANIM_DEFAULT_GUN_STAND;
            holdMs = 160;
            return true;
        }
        return false;
    default:
        return false;
    }
}

static void QueueRemoteAnimEvent(int slot, unsigned char eventType, unsigned char weaponType)
{
    if (slot < 0 || slot >= MAX_NETPLAYERS)
        return;
    gNetPlayers[slot].PendingAnimEvent = eventType;
    gNetPlayers[slot].PendingAnimWeapon = weaponType;
}

static bool IsArmedMoveWeapon(eWeaponType weaponType)
{
    switch (weaponType) {
    case WEAPONTYPE_PISTOL:
    case WEAPONTYPE_PISTOL_SILENCED:
    case WEAPONTYPE_DESERT_EAGLE:
    case WEAPONTYPE_SHOTGUN:
    case WEAPONTYPE_SAWNOFF:
    case WEAPONTYPE_SPAS12:
    case WEAPONTYPE_MICRO_UZI:
    case WEAPONTYPE_MP5:
    case WEAPONTYPE_AK47:
    case WEAPONTYPE_M4:
    case WEAPONTYPE_TEC9:
    case WEAPONTYPE_COUNTRYRIFLE:
    case WEAPONTYPE_SNIPERRIFLE:
        return true;
    default:
        return false;
    }
}

static bool ChooseRemoteMoveAnimation(const NetPlayer& player, int& animGroup, int& animId)
{
    if (player.bReportedDead)
        return false;

    if (player.ForcedAnimUntil > CTimer::m_snTimeInMilliseconds &&
        player.ForcedAnimGroup >= 0 && player.ForcedAnimId >= 0) {
        animGroup = player.ForcedAnimGroup;
        animId = player.ForcedAnimId;
        return true;
    }

    const bool isTargeting = (player.InputBits & NETINPUT_TARGET) != 0;
    const bool isFiring = (player.InputBits & NETINPUT_FIRE) != 0;
    const bool isDucking = (player.InputBits & NETINPUT_DUCK) != 0;
    const bool isMelee = (player.InputBits & NETINPUT_MELEE) != 0;
    const bool armedMove = IsArmedMoveWeapon((eWeaponType)player.CurrentWeapon);
    const bool movingForward = player.AnalogUpDown > 24;
    const bool movingBackward = player.AnalogUpDown < -24;
    const bool movingLeft = player.AnalogLeftRight < -24;
    const bool movingRight = player.AnalogLeftRight > 24;

    if (isDucking && armedMove) {
        animGroup = ANIM_GROUP_DEFAULT;
        if (movingForward) {
            animId = ANIM_DEFAULT_GUNCROUCHFWD;
        } else if (movingBackward) {
            animId = ANIM_DEFAULT_GUNCROUCHBWD;
        } else if (movingLeft) {
            animId = ANIM_DEFAULT_CROUCH_ROLL_L;
        } else if (movingRight) {
            animId = ANIM_DEFAULT_CROUCH_ROLL_R;
        } else {
            animId = ANIM_DEFAULT_WEAPON_CROUCH;
        }
        return true;
    }

    if (armedMove && (isTargeting || isFiring)) {
        animGroup = ANIM_GROUP_DEFAULT;
        if (movingForward) {
            animId = ANIM_DEFAULT_GUNMOVE_FWD;
        } else if (movingBackward) {
            animId = ANIM_DEFAULT_GUNMOVE_BWD;
        } else if (movingLeft) {
            animId = ANIM_DEFAULT_GUNMOVE_L;
        } else if (movingRight) {
            animId = ANIM_DEFAULT_GUNMOVE_R;
        } else if (isFiring) {
            animId = ANIM_DEFAULT_GUN_STAND;
        } else {
            animId = ANIM_DEFAULT_GUN_2_IDLE;
        }
        return true;
    }

    if (!armedMove && (isMelee || isTargeting)) {
        animGroup = ANIM_GROUP_DEFAULT;
        if (isMelee) {
            if (movingLeft) {
                animId = ANIM_DEFAULT_FIGHTSH_LEFT;
            } else if (movingRight) {
                animId = ANIM_DEFAULT_FIGHTSH_RIGHT;
            } else if (movingBackward) {
                animId = ANIM_DEFAULT_FIGHTSH_BWD;
            } else {
                animId = ANIM_DEFAULT_FIGHTSHF;
            }
        } else {
            animId = ANIM_DEFAULT_FIGHT2IDLE;
        }
        return true;
    }

    int baseGroup = ANIM_GROUP_PLAYER;
    if (player.AnimGroup == ANIM_GROUP_FAT) {
        baseGroup = ANIM_GROUP_FAT;
    } else if (player.AnimGroup == ANIM_GROUP_MUSCULAR) {
        baseGroup = ANIM_GROUP_MUSCULAR;
    }

    if (armedMove) {
        if (baseGroup == ANIM_GROUP_FAT)
            animGroup = ANIM_GROUP_PLAYER2ARMEDF;
        else if (baseGroup == ANIM_GROUP_MUSCULAR)
            animGroup = ANIM_GROUP_PLAYER2ARMEDM;
        else
            animGroup = ANIM_GROUP_PLAYER2ARMED;
    } else {
        animGroup = baseGroup;
    }

    switch (player.MoveState) {
    case PEDMOVE_STILL:
        animId = armedMove ? ANIM_PLAYER2ARMED_IDLE_ARMED : ANIM_PLAYER_IDLE_STANCE;
        return true;
    case PEDMOVE_WALK:
        animId = armedMove ? ANIM_PLAYER2ARMED_WALK_ARMED : ANIM_PLAYER_WALK_PLAYER;
        return true;
    case PEDMOVE_RUN:
    case PEDMOVE_SPRINT:
        animId = armedMove ? ANIM_PLAYER2ARMED_RUN_ARMED_0 : ANIM_PLAYER_RUN_PLAYER;
        return true;
    default:
        animId = armedMove ? ANIM_PLAYER2ARMED_IDLE_ARMED : ANIM_PLAYER_IDLE_STANCE;
        return true;
    }
}

static void ApplyRemoteMoveAnimation(CPed* ped, NetPlayer& player)
{
    if (!ped || !ped->m_pRwClump || ped->bInVehicle)
        return;

    int animGroup = ANIM_GROUP_PLAYER;
    int animId = ANIM_PLAYER_IDLE_STANCE;
    if (!ChooseRemoteMoveAnimation(player, animGroup, animId))
        return;

    if (player.LastAppliedAnimGroup == animGroup && player.LastAppliedAnimId == animId) {
        LogVerbose("UpdateRemotePlayerVisual step=blendAnimCached ped=%p group=%d anim=%d", ped, animGroup, animId);
        return;
    }

    const auto assoc = CAnimManager::BlendAnimation(ped->m_pRwClump, animGroup, animId, 4.0f);
    if (assoc) {
        assoc->SetBlend(1.0f, 4.0f);
        assoc->m_nCallbackType = (eAnimBlendCallbackType)0;
        ped->m_nAnimGroup = animGroup;
        player.LastAppliedAnimGroup = (short)animGroup;
        player.LastAppliedAnimId = (short)animId;
        LogVerbose("UpdateRemotePlayerVisual step=blendAnim ped=%p group=%d anim=%d move=%u weapon=%u",
            ped, animGroup, animId, player.MoveState, player.CurrentWeapon);
    } else {
        LogVerbose("UpdateRemotePlayerVisual step=blendAnimFailed ped=%p group=%d anim=%d", ped, animGroup, animId);
    }
}

static void ApplyPendingRemoteAnimEvent(CPed* ped, NetPlayer& player)
{
    if (!ped || !ped->m_pRwClump || player.PendingAnimEvent == NETANIM_NONE)
        return;

    int animGroup = ANIM_GROUP_DEFAULT;
    int animId = ANIM_DEFAULT_IDLE_STANCE;
    unsigned int holdMs = 0;
    if (!ResolveRemoteAnimEvent(player.PendingAnimEvent, player.PendingAnimWeapon, animGroup, animId, holdMs)) {
        LogVerbose("UpdateRemotePlayerVisual step=animEventSkipped ped=%p event=%u weapon=%u",
            ped, player.PendingAnimEvent, player.PendingAnimWeapon);
        player.PendingAnimEvent = NETANIM_NONE;
        player.PendingAnimWeapon = WEAPONTYPE_UNARMED;
        return;
    }

    const auto assoc = CAnimManager::BlendAnimation(ped->m_pRwClump, animGroup, animId, 8.0f);
    if (assoc) {
        assoc->SetBlend(1.0f, 8.0f);
        assoc->m_nCallbackType = (eAnimBlendCallbackType)0;
        player.ForcedAnimGroup = (short)animGroup;
        player.ForcedAnimId = (short)animId;
        player.ForcedAnimUntil = CTimer::m_snTimeInMilliseconds + holdMs;
        player.LastAppliedAnimGroup = (short)animGroup;
        player.LastAppliedAnimId = (short)animId;
        LogVerbose("UpdateRemotePlayerVisual step=animEvent ped=%p event=%u group=%d anim=%d hold=%u",
            ped, player.PendingAnimEvent, animGroup, animId, holdMs);
    } else {
        LogVerbose("UpdateRemotePlayerVisual step=animEventFailed ped=%p event=%u group=%d anim=%d",
            ped, player.PendingAnimEvent, animGroup, animId);
    }

    player.PendingAnimEvent = NETANIM_NONE;
    player.PendingAnimWeapon = WEAPONTYPE_UNARMED;
}

static float GetVisiblePedHealth(const NetPlayer& player)
{
    if (gCoopMode && player.bReportedDead)
        return 1.0f;
    return player.bReportedDead ? 0.0f : std::max(1.0f, player.TargetHealth);
}

static void UpdateRemotePlayerVisual(CPed* ped, NetPlayer& player)
{
    if (!ped)
        return;

    const unsigned int pedAgeMs = CTimer::m_snTimeInMilliseconds - player.LastEventTime;
    static const float kOnFootCorrectionMultiplier = 0.06f;
    static const float kInaccuracyFactorMultiplier = 5.0f;

    LogVerbose("UpdateRemotePlayerVisual begin ped=%p model=%u weap=%u ammo=%u move=%u pos=(%.2f,%.2f,%.2f)",
        ped, player.ModelIndex, player.CurrentWeapon, player.WeaponAmmo, player.MoveState,
        player.TargetPos.x, player.TargetPos.y, player.TargetPos.z);

    LogVerbose("UpdateRemotePlayerVisual step=model ped=%p", ped);
    EnsureRemotePedModel(ped, player.ModelIndex);

    const CVector currentPos = ped->GetPosition();
    const CVector delta = player.TargetPos - currentPos;
    LogVerbose("UpdateRemotePlayerVisual step=position ped=%p delta=%.3f", ped, delta.Magnitude());
    if (delta.x > 2.5f || delta.x < -2.5f ||
        delta.y > 2.5f || delta.y < -2.5f ||
        delta.z > 1.0f || delta.z < -1.0f) {
        ped->Teleport(player.TargetPos, false);
    } else {
        CVector correctedSpeed = player.TargetMoveSpeed;
        float correctionMultiplier = kOnFootCorrectionMultiplier;
        if (!ped->bIsStanding) {
            correctionMultiplier *= 0.25f;
        }

        if (delta.x > 0.0001f || delta.x < -0.0001f) {
            correctedSpeed.x += delta.x * kInaccuracyFactorMultiplier * correctionMultiplier;
        }
        if (delta.y > 0.0001f || delta.y < -0.0001f) {
            correctedSpeed.y += delta.y * kInaccuracyFactorMultiplier * correctionMultiplier;
        }
        if (delta.z > 0.0001f || delta.z < -0.0001f) {
            correctedSpeed.z += delta.z * kInaccuracyFactorMultiplier * 0.025f;
        }

        ped->m_vecMoveSpeed = correctedSpeed;
        ped->ApplyMoveSpeed();
        LogVerbose("UpdateRemotePlayerVisual step=positionCorrection ped=%p baseSpeed=(%.3f,%.3f,%.3f) corrected=(%.3f,%.3f,%.3f)",
            ped,
            player.TargetMoveSpeed.x, player.TargetMoveSpeed.y, player.TargetMoveSpeed.z,
            correctedSpeed.x, correctedSpeed.y, correctedSpeed.z);
    }

    LogVerbose("UpdateRemotePlayerVisual step=rotation ped=%p", ped);
    ped->SetHeading(player.TargetHeading);
    ped->m_fCurrentRotation = player.TargetHeading;
    ped->m_fAimingRotation = player.TargetHeading;
    ped->UpdateRwFrame();
    ped->UpdateRpHAnim();
    LogVerbose("UpdateRemotePlayerVisual step=health ped=%p dead=%d health=%.2f", ped, player.bReportedDead ? 1 : 0, player.TargetHealth);
    ped->m_fHealth = GetVisiblePedHealth(player);
    ped->m_nFightingStyle = (char)player.FightingStyle;
    ped->m_nAllowedAttackMoves |= 15u;
    ApplyRemotePedDucking(ped, player);
    ApplyRemoteMoveAnimation(ped, player);

    if (pedAgeMs > 1500 && !ped->bInVehicle && IsSafeRemoteWeaponSyncType((eWeaponType)player.CurrentWeapon)) {
        EnsureRemotePedWeapon(ped, player.CurrentWeapon, std::max(1u, player.WeaponAmmo), player.WeaponAmmoInClip, player.WeaponState);
        LogVerbose("UpdateRemotePlayerVisual step=weapon ped=%p weapon=%u ammo=%u clip=%u state=%u inputBits=0x%X move=%u",
            ped, player.CurrentWeapon, player.WeaponAmmo, player.WeaponAmmoInClip, player.WeaponState, player.InputBits, player.MoveState);
    } else {
        LogVerbose("UpdateRemotePlayerVisual step=replayDisabled ped=%p weap=%u weaponModel=%d animGroup=%u age=%u",
            ped, player.CurrentWeapon, player.WeaponModelId, player.AnimGroup, pedAgeMs);
    }

    ApplyPendingRemoteAnimEvent(ped, player);

    LogVerbose("UpdateRemotePlayerVisual end ped=%p", ped);
}

static bool IsSlotDead(int slot)
{
    if (slot < 0 || slot >= MAX_NETPLAYERS || !gNetPlayers[slot].bActive)
        return false;

    if (gCoopMode && gNetPlayers[slot].bReportedDead)
        return true;

    if (slot == gLocalSlot)
        return IsPedDead(FindPlayerPed());

    return gNetPlayers[slot].bReportedDead;
}

static CVector GetSlotPosition(int slot)
{
    if (slot >= 0 && slot < MAX_NETPLAYERS) {
        if (gNetStatus == NETSTAT_SERVER && slot == gLocalSlot) {
            CPlayerPed* localPed = FindPlayerPed();
            if (localPed)
                return localPed->GetPosition();
        }
        if (gNetPlayers[slot].pPed)
            return gNetPlayers[slot].pPed->GetPosition();
    }
    return CVector(1034.9f, -940.0f, 15.0f);
}

static CVector BuildCoopOffsetPosition(const CVector& anchor)
{
    return AdjustSpawnPositionAboveGround(CVector(anchor.x + 2.0f, anchor.y + 2.0f, anchor.z + 0.5f));
}

// ============================================================
//  GAME HELPERS
// ============================================================

// Return true when the current game mode is a team game
static bool TeamGameGoingOn()
{
    switch (gGameType) {
    case GAMETYPE_TEAMDEATHMATCH:
    case GAMETYPE_TEAMDEATHMATCH_NOBLIPS:
    case GAMETYPE_STASHTHECASH:
    case GAMETYPE_CAPTURETHEFLAG:
    case GAMETYPE_DOMINATION:
        return true;
    }
    return false;
}

// Assign team to a player
static int FindTeamForNewPlayer(int slot)
{
    if (!TeamGameGoingOn()) return -1;
    if (gNetPlayers[slot].Team >= 0) return gNetPlayers[slot].Team;

    int count[2] = { 0, 0 };
    for (int i = 0; i < MAX_NETPLAYERS; i++) {
        if (i == slot || !gNetPlayers[i].bActive) continue;
        int t = gNetPlayers[i].Team;
        if (t >= 0 && t < 2) count[t]++;
    }
    return (count[0] <= count[1]) ? 0 : 1;
}

// Blend player colour with team colour
static void FindPlayerMarkerColour(int slot, int* R, int* G, int* B)
{
    NetPlayer& p = gNetPlayers[slot];
    if (p.Team < 0) {
        *R = p.R; *G = p.G; *B = p.B;
    }
    else {
        *R = (int)((2 * p.R + 3 * kTeamColours[p.Team * 3]) / 5);
        *G = (int)((2 * p.G + 3 * kTeamColours[p.Team * 3 + 1]) / 5);
        *B = (int)((2 * p.B + 3 * kTeamColours[p.Team * 3 + 2]) / 5);
    }
}

// Pick a spawn point as far as possible from other players
static CVector SelectSafeStartPoint(int team)
{
    if (gCoopMode) {
        int partnerSlot = GetOtherActiveSlot(-1);
        if (partnerSlot >= 0 && !IsSlotDead(partnerSlot))
            return BuildCoopOffsetPosition(GetSlotPosition(partnerSlot));
    }

    // CTF: spawn near own base
    if (gGameType == GAMETYPE_CAPTURETHEFLAG && team >= 0 && gNumCTFBases > 0) {
        int b = gCurrentStashLocs[team] < gNumCTFBases ? gCurrentStashLocs[team] : 0;
        return AdjustSpawnPositionAboveGround(CVector(
            gCTFBases[b * 3] + (float)((int)(CGeneral::GetRandomNumber() & 7) - 3),
            gCTFBases[b * 3 + 1] + (float)((int)(CGeneral::GetRandomNumber() & 7) - 3),
            gCTFBases[b * 3 + 2] + 1.0f));
    }

    if (gNumStartPoints == 0) {
        // Fallback: original PS2 hard-coded start
        return AdjustSpawnPositionAboveGround(CVector(1034.9f, -940.0f, 15.0f));
    }

    float bestDist = 0.0f;
    int   bestIdx = 0;
    for (int p = 0; p < gNumStartPoints; p++) {
        float closest = 10000.0f;
        for (int s = 0; s < MAX_NETPLAYERS; s++) {
            if (!gNetPlayers[s].bActive || !gNetPlayers[s].pPed) continue;
            float d = (gNetPlayers[s].pPed->GetPosition() -
                CVector(gNetStartCoors[p][0], gNetStartCoors[p][1], gNetStartCoors[p][2])).Magnitude();
            if (d < closest) closest = d;
        }
        if (closest > bestDist) { bestDist = closest; bestIdx = p; }
    }
    return AdjustSpawnPositionAboveGround(CVector(gNetStartCoors[bestIdx][0], gNetStartCoors[bestIdx][1], gNetStartCoors[bestIdx][2]));
}

// Store a network object of any type
static void AddNetworkObject(float X, float Y, float Z, int Type)
{
    switch (Type) {
    case NOBJ_CASHGENERATOR:
        if (gNumCashGenerators < MAX_NUM_CASH_GENERATORS) {
            gCashGenerators[gNumCashGenerators * 3] = X;
            gCashGenerators[gNumCashGenerators * 3 + 1] = Y;
            gCashGenerators[gNumCashGenerators * 3 + 2] = Z;
            gNumCashGenerators++;
        }
        break;
    case NOBJ_STASHLOCATION:
        if (gNumStashLocations < MAX_NUM_STASH_LOCATIONS) {
            gStashLocations[gNumStashLocations * 3] = X;
            gStashLocations[gNumStashLocations * 3 + 1] = Y;
            gStashLocations[gNumStashLocations * 3 + 2] = Z;
            gNumStashLocations++;
        }
        break;
    case NOBJ_SPAWNPOINT:
        if (gNumStartPoints < MAX_START_POINTS) {
            gNetStartCoors[gNumStartPoints][0] = X;
            gNetStartCoors[gNumStartPoints][1] = Y;
            gNetStartCoors[gNumStartPoints][2] = Z;
            gNumStartPoints++;
        }
        break;
    case NOBJ_CTFBASE:
        if (gNumCTFBases < MAX_NUM_CTF_BASES) {
            gCTFBases[gNumCTFBases * 3] = X;
            gCTFBases[gNumCTFBases * 3 + 1] = Y;
            gCTFBases[gNumCTFBases * 3 + 2] = Z;
            gNumCTFBases++;
        }
        break;
    default:
        // Weapon/pickup pickups stored as-is (not yet used in this revision)
        break;
    }
}

// ============================================================
//  PED MANAGEMENT  (create / destroy remote player peds)
// ============================================================

// Create a ped for a remote player and add it to the world
static void CreateRemotePlayerPed(int slot, const CVector& pos)
{
    if (gNetPlayers[slot].pPed) return; // already exists

    // The model must be loaded before instantiating and setting it on the ped.
    // Without this, SetModelIndex fails to create an RwObject/matrix, causing a nullptr crash.
    unsigned short modelIndex = SanitizeRemotePedModel(gNetPlayers[slot].ModelIndex ? gNetPlayers[slot].ModelIndex : MODEL_MALE01);
    LogMessage("CreateRemotePlayerPed slot=%d begin model=%u rawPos=(%.2f,%.2f,%.2f)",
        slot, modelIndex, pos.x, pos.y, pos.z);
    CStreaming::RequestModel(modelIndex, 2);
    CStreaming::LoadAllRequestedModels(false);

    // Keep remote entities as generic mission peds until the sync path is stable.
    // Player-oriented ped types have been crashing immediately after creation.
    CPed* pPed = new CPed(PED_TYPE_CIVMALE);
    LogMessage("CreateRemotePlayerPed slot=%d constructed ped=%p", slot, pPed);
    pPed->SetModelIndex(modelIndex);
    LogMessage("CreateRemotePlayerPed slot=%d SetModelIndex(%u) ok", slot, modelIndex);
    pPed->SetCharCreatedBy(2); // 2 = PED_MISSION (keeps SA from auto-deleting it)
    pPed->bBulletProof = true;
    pPed->bCollisionProof = true;
    pPed->bMeleeProof = true;
    pPed->bInvulnerable = true;
    pPed->bCollidable = false;
    pPed->bCanBeCollidedWith = false;
    pPed->bDisableCollisionForce = true;
    pPed->bInfiniteMass = true;
    const CVector spawnPos = AdjustSpawnPositionAboveGround(pos);
    gNetPlayers[slot].TargetPos = spawnPos;
    gNetPlayers[slot].TargetHeading = 0.0f;
    gNetPlayers[slot].TargetHealth = 100.0f;
    gNetPlayers[slot].LastEventTime = CTimer::m_snTimeInMilliseconds;
    pPed->SetPosn(spawnPos); // Safely sets placement matrix
    LogMessage("CreateRemotePlayerPed slot=%d SetPosn(%.2f,%.2f,%.2f) ok", slot, spawnPos.x, spawnPos.y, spawnPos.z);
    CWorld::Add(pPed);
    LogMessage("CreateRemotePlayerPed slot=%d CWorld::Add ok", slot);
    gNetPlayers[slot].pPed = pPed;
    gNetPlayers[slot].bVisualsReady = false;
    LogMessage("CreateRemotePlayerPed slot=%d model=%u pos=(%.2f,%.2f,%.2f)",
        slot, modelIndex, spawnPos.x, spawnPos.y, spawnPos.z);
    LogMessage("CreateRemotePlayerPed slot=%d deferred visual apply until first sync packet", slot);
}

// Remove a remote player ped from the world.
// Using CWorld::Remove + delete mirrors how the original PS2 code cleaned up
// peds it explicitly newed.  If the ped is currently targeted by the game's AI
// systems (task manager etc.), calling CWorld::Remove first un-registers it so
// stale pointers held by the world/sector lists are cleared before the memory
// is freed.
static void RemoveRemotePlayerPed(int slot)
{
    if (!gNetPlayers[slot].pPed) return;
    CWorld::Remove(gNetPlayers[slot].pPed);
    delete gNetPlayers[slot].pPed;
    gNetPlayers[slot].pPed = nullptr;
}

// Clear radar blip for a slot
static void ClearSlotBlip(int slot)
{
    if (gNetPlayers[slot].RadarBlip) {
        CRadar::ClearBlip(gNetPlayers[slot].RadarBlip);
        gNetPlayers[slot].RadarBlip = 0;
    }
}

// Full removal of a player slot
static void DisconnectPlayer(int slot)
{
    ClearSlotBlip(slot);
    RemoveRemotePlayerPed(slot);
    gNetPlayers[slot].Init();
}

// ============================================================
//  BROADCAST HELPERS
// ============================================================

// Broadcast a text message to all clients AND print locally
static void BroadcastText(const char* txt,
    unsigned char R = 200, unsigned char G = 200, unsigned char B = 200)
{
    Chat_AddLine(txt, R, G, B);
    CMsgText msg;
    msg.Message = MSG_TEXT;
    strncpy_s(msg.String, txt, sizeof(msg.String) - 1);
    msg.ColourR = R; msg.ColourG = G; msg.ColourB = B;
    if (gNetStatus == NETSTAT_SERVER)
        NetBroadcast(&msg, sizeof(msg));
}

// ============================================================
//  SERVER-SIDE GAME LOGIC  (ported from netgames.cpp)
// ============================================================

// Init the server-side game objects at start
static void Server_InitGameObjects()
{
    // Hard-coded spawn/weapon pickups from original gamenet.cpp
    AddNetworkObject(1503.0f, -1662.0f, 14.0f, NOBJ_SPAWNPOINT);
    AddNetworkObject(1479.0f, -1706.5f, 14.0f, NOBJ_SPAWNPOINT);
    AddNetworkObject(1448.0f, -1662.5f, 14.0f, NOBJ_SPAWNPOINT);
    AddNetworkObject(1478.0f, -1619.0f, 14.0f, NOBJ_SPAWNPOINT);

    // Default weapon pickups (from Init_ServerAtBeginOfGame in netgames.cpp)
    CPickups::GenerateNewOne(CVector(1442.0f, -1603.0f, 14.0f),
        (unsigned int)MODEL_BRASSKNUCKLE, PICKUP_ON_STREET, 0, 0, false, nullptr);
    CPickups::GenerateNewOne(CVector(1520.0f, -1602.0f, 14.0f),
        (unsigned int)MODEL_MOLOTOV, PICKUP_ON_STREET, 0, 0, false, nullptr);
    CPickups::GenerateNewOne(CVector(1511.0f, -1675.0f, 14.0f),
        (unsigned int)MODEL_COLT45, PICKUP_ON_STREET, 0, 0, false, nullptr);
    CPickups::GenerateNewOne(CVector(1522.0f, -1720.0f, 14.0f),
        (unsigned int)MODEL_CHROMEGUN, PICKUP_ON_STREET, 0, 0, false, nullptr);
    CPickups::GenerateNewOne(CVector(1480.0f, -1750.0f, 15.5f),
        (unsigned int)MODEL_SNIPER, PICKUP_ON_STREET, 0, 0, false, nullptr);

    // Health / armour
    CPickups::GenerateNewOne(CVector(1446.0f, -1660.7f, 10.2f),
        (unsigned int)MODEL_HEALTH, PICKUP_ON_STREET, 0, 0, false, nullptr);
    CPickups::GenerateNewOne(CVector(1505.0f, -1650.0f, 14.0f),
        (unsigned int)MODEL_HEALTH, PICKUP_ON_STREET, 0, 0, false, nullptr);
    CPickups::GenerateNewOne(CVector(1498.0f, -1671.0f, 14.0f),
        (unsigned int)MODEL_BODYARMOUR, PICKUP_ON_STREET, 0, 0, false, nullptr);

    // CTF bases (example positions near GS airport area)
    AddNetworkObject(1488.0f, -680.0f, 11.7f, NOBJ_STASHLOCATION);
    AddNetworkObject(1375.5f, -989.5f, 11.8f, NOBJ_STASHLOCATION);
    AddNetworkObject(1204.0f, -795.5f, 14.6f, NOBJ_STASHLOCATION);

    AddNetworkObject(1503.0f, -1662.0f, 14.0f, NOBJ_CTFBASE);
    AddNetworkObject(1375.5f, -989.5f, 11.8f, NOBJ_CTFBASE);

    AddNetworkObject(1053.0f, -947.0f, 15.0f, NOBJ_CASHGENERATOR);
    AddNetworkObject(903.7f, -817.0f, 14.7f, NOBJ_CASHGENERATOR);

    gNumStartPoints = gNumStartPoints > 0 ? gNumStartPoints : 4;
}

// Reset all scores
static void ResetScores()
{
    for (int i = 0; i < MAX_NETPLAYERS; i++) {
        gNetPlayers[i].Points = 0;
        gNetPlayers[i].Points2 = 0;
    }
    gTeamPoints[0] = gTeamPoints[1] = 0;
    gGameState = 0;
    gCashCarrierSlot = -1;
    gPlayerCarryingFlag[0] = gPlayerCarryingFlag[1] = -1;
}

// Award kill points (server only)
static void RegisterPlayerKill(int killedSlot, int killerSlot)
{
    if (gCoopMode) {
        char msg[128];
        if (killerSlot >= 0 && killerSlot != killedSlot)
            sprintf_s(msg, "%s revived after being downed", gNetPlayers[killedSlot].Name);
        else
            sprintf_s(msg, "%s will respawn nearby", gNetPlayers[killedSlot].Name);
        BroadcastText(msg, 200, 200, 200);
        return;
    }

    switch (gGameType) {
    case GAMETYPE_DEATHMATCH:
    case GAMETYPE_DEATHMATCH_NOBLIPS:
    case GAMETYPE_TEAMDEATHMATCH:
    case GAMETYPE_TEAMDEATHMATCH_NOBLIPS:
    {
        if (killerSlot < 0) break;
        if (killerSlot == killedSlot) {
            if (gNetPlayers[killerSlot].Points > 0)
                gNetPlayers[killerSlot].Points--;
            if (TeamGameGoingOn() && gNetPlayers[killerSlot].Team >= 0)
                if (gTeamPoints[gNetPlayers[killerSlot].Team] > 0)
                    gTeamPoints[gNetPlayers[killerSlot].Team]--;
        }
        else {
            bool sameTeam = TeamGameGoingOn() &&
                gNetPlayers[killerSlot].Team >= 0 &&
                gNetPlayers[killedSlot].Team >= 0 &&
                gNetPlayers[killerSlot].Team == gNetPlayers[killedSlot].Team;
            if (!sameTeam) {
                gNetPlayers[killerSlot].Points++;
                if (TeamGameGoingOn() && gNetPlayers[killerSlot].Team >= 0)
                    gTeamPoints[gNetPlayers[killerSlot].Team]++;
            }
        }
        // Compose kill message
        char msg[128];
        if (killerSlot == killedSlot) {
            sprintf_s(msg, "%s committed suicide", gNetPlayers[killedSlot].Name);
            BroadcastText(msg, 200, 200, 200);
        }
        else if (killerSlot >= 0) {
            int R, G, B;
            FindPlayerMarkerColour(killerSlot, &R, &G, &B);
            sprintf_s(msg, "%s killed %s",
                gNetPlayers[killerSlot].Name, gNetPlayers[killedSlot].Name);
            BroadcastText(msg, (unsigned char)R, (unsigned char)G, (unsigned char)B);
        }
        else {
            sprintf_s(msg, "%s died", gNetPlayers[killedSlot].Name);
            BroadcastText(msg, 200, 200, 200);
        }
    }
    break;
    default:
        break;
    }
}

// Broadcast team points to all clients
static void BroadcastTeamPoints()
{
    CMsgTeamPoints msg;
    msg.Message = MSG_TEAMPOINTS;
    msg.TeamPoints[0] = gTeamPoints[0];
    msg.TeamPoints[1] = gTeamPoints[1];
    NetBroadcast(&msg, sizeof(msg));
}

static void SendDamageMessageToClient(int targetSlot, float newHealth, int attackerSlot, unsigned char weaponType)
{
    if (targetSlot < 0 || targetSlot >= MAX_NETPLAYERS)
        return;
    if (!gNetPlayers[targetSlot].bActive || gNetPlayers[targetSlot].SockAddr.sin_port == 0)
        return;

    CMsgDamage msg;
    msg.Message = MSG_DAMAGE;
    msg.TargetSlot = targetSlot;
    msg.AttackerSlot = attackerSlot;
    msg.NewHealth = newHealth;
    msg.WeaponType = weaponType;
    msg.Killed = (unsigned char)(newHealth <= 0.0f ? 1 : 0);
    NetSendTo(&msg, sizeof(msg), gNetPlayers[targetSlot].SockAddr);
}

static void BroadcastAnimEvent(int slot, unsigned char eventType, unsigned char weaponType)
{
    if (slot < 0 || slot >= MAX_NETPLAYERS)
        return;

    CMsgAnimEvent msg = {};
    msg.Message = MSG_ANIMEVENT;
    msg.PlayerSlot = slot;
    msg.EventType = eventType;
    msg.WeaponType = weaponType;

    if (gNetStatus == NETSTAT_SERVER)
        NetBroadcast(&msg, sizeof(msg));
    else if (gNetStatus == NETSTAT_CLIENTRUNNING)
        NetSendToServer(&msg, sizeof(msg));
}

static void TriggerLocalAnimEvent(unsigned char eventType, unsigned char weaponType)
{
    if (gNetStatus == NETSTAT_SINGLEPLAYER)
        return;
    BroadcastAnimEvent(gLocalSlot, eventType, weaponType);
}

static void ProcessLocalAnimEvents()
{
    if (gNetStatus == NETSTAT_SINGLEPLAYER)
        return;

    CPlayerPed* localPed = FindPlayerPed();
    if (!localPed || IsSlotDead(gLocalSlot))
        return;

    CWeapon* weapon = localPed->GetWeapon();
    const unsigned char currentWeapon = weapon ? (unsigned char)weapon->m_eWeaponType : WEAPONTYPE_UNARMED;
    const unsigned char currentWeaponState = weapon ? (unsigned char)weapon->m_nState : WEAPONSTATE_READY;
    const unsigned short inputBits = CollectLocalInputBits(localPed);
    const bool airborne = !localPed->bIsStanding && !localPed->bInVehicle;

    if ((inputBits & NETINPUT_MELEE) && !(gLastObservedLocalInputBits & NETINPUT_MELEE))
        TriggerLocalAnimEvent(NETANIM_MELEE_SWING, currentWeapon);
    if (currentWeaponState == WEAPONSTATE_FIRING && gLastObservedLocalWeaponState != WEAPONSTATE_FIRING)
        TriggerLocalAnimEvent(NETANIM_FIRE, currentWeapon);
    if ((inputBits & NETINPUT_JUMP) && !(gLastObservedLocalInputBits & NETINPUT_JUMP))
        TriggerLocalAnimEvent(NETANIM_JUMP_LAUNCH, currentWeapon);
    if (gLastObservedLocalAirborne && !airborne)
        TriggerLocalAnimEvent(NETANIM_JUMP_LAND, currentWeapon);

    gLastObservedLocalWeapon = currentWeapon;
    gLastObservedLocalWeaponState = currentWeaponState;
    gLastObservedLocalInputBits = inputBits;
    gLastObservedLocalAirborne = airborne;
}

static void ApplyServerAuthoritativeDamage(int targetSlot, int attackerSlot, float damage, unsigned char weaponType)
{
    if (targetSlot < 0 || targetSlot >= MAX_NETPLAYERS || !gNetPlayers[targetSlot].bActive)
        return;
    if (damage <= 0.0f || IsSlotDead(targetSlot))
        return;

    CPed* targetPed = GetSlotPed(targetSlot);
    float currentHealth = targetPed ? targetPed->m_fHealth : gNetPlayers[targetSlot].TargetHealth;
    const float newHealth = std::max(0.0f, currentHealth - damage);
    const float appliedHealth = (gCoopMode && newHealth <= 0.0f) ? 1.0f : newHealth;

    if (targetPed)
        targetPed->m_fHealth = appliedHealth;

    gNetPlayers[targetSlot].TargetHealth = newHealth;
    gNetPlayers[targetSlot].bReportedDead = newHealth <= 0.0f;
    QueueRemoteAnimEvent(targetSlot, NETANIM_HIT_REACT, weaponType);
    BroadcastAnimEvent(targetSlot, NETANIM_HIT_REACT, weaponType);

    if (targetSlot != gLocalSlot)
        SendDamageMessageToClient(targetSlot, newHealth, attackerSlot, weaponType);

    LogMessage("Coop damage attacker=%d target=%d weapon=%u amount=%.1f newHealth=%.1f",
        attackerSlot, targetSlot, weaponType, damage, newHealth);
}

static void TryServerProcessMeleeHit(int attackerSlot, int targetSlot)
{
    if (!CanAttemptCoopMeleeHit(attackerSlot, targetSlot))
        return;

    const unsigned int now = CTimer::m_snTimeInMilliseconds;
    if (now - gLastMeleeAttemptTime[attackerSlot] < COOP_MELEE_COOLDOWN_MS)
        return;

    gLastMeleeAttemptTime[attackerSlot] = now;
    ApplyServerAuthoritativeDamage(targetSlot, attackerSlot, COOP_MELEE_DAMAGE, GetPedCombatWeaponType(GetSlotPed(attackerSlot)));
}

static void TryServerProcessWeaponHit(int attackerSlot, int targetSlot, eWeaponType weaponType)
{
    CPed* attackerPed = GetSlotPed(attackerSlot);
    if (!attackerPed)
        return;

    CWeaponInfo* weaponInfo = GetPedCombatWeaponInfo(attackerPed, weaponType);
    if (!weaponInfo)
        return;

    const unsigned int now = CTimer::m_snTimeInMilliseconds;
    const unsigned int cooldownMs = GetWeaponAttackCooldownMs(weaponType, weaponInfo);
    if (now - gLastWeaponAttackTime[attackerSlot] < cooldownMs)
        return;

    const eWeaponFire fireType = (eWeaponFire)weaponInfo->m_nWeaponFire;
    if (fireType == WEAPON_FIRE_MELEE) {
        TryServerProcessMeleeHit(attackerSlot, targetSlot);
        return;
    }

    if (!CanAttemptCoopWeaponHit(attackerSlot, targetSlot, weaponType, weaponInfo->m_fWeaponRange))
        return;

    gLastWeaponAttackTime[attackerSlot] = now;

    float damage = (float)weaponInfo->m_nDamage;
    if (fireType == WEAPON_FIRE_PROJECTILE || fireType == WEAPON_FIRE_AREA_EFFECT)
        damage = std::max(damage, 35.0f);
    else if (weaponType == WEAPONTYPE_SHOTGUN || weaponType == WEAPONTYPE_SAWNOFF || weaponType == WEAPONTYPE_SPAS12)
        damage *= 1.5f;

    ApplyServerAuthoritativeDamage(targetSlot, attackerSlot, damage, (unsigned char)weaponType);
}

static void ProcessLocalCoopCombat()
{
    if (!gCoopMode || gNetStatus == NETSTAT_SINGLEPLAYER || gChatTyping)
        return;

    CPlayerPed* localPed = FindPlayerPed();
    if (!localPed || IsSlotDead(gLocalSlot) || IsPedDead(localPed))
        return;

    CPad* pad = CPad::GetPad(0);
    if (!pad)
        return;

    const int targetSlot = GetOtherActiveSlot(gLocalSlot);
    if (targetSlot < 0)
        return;

    const bool meleeJustDown = pad->MeleeAttackJustDown() != 0;
    if (meleeJustDown && IsPedUsingMelee(localPed)) {
        if (gNetStatus == NETSTAT_SERVER) {
            TryServerProcessMeleeHit(gLocalSlot, targetSlot);
        } else if (gNetStatus == NETSTAT_CLIENTRUNNING) {
            CMsgMeleeAttack msg;
            msg.Message = MSG_MELEEATTACK;
            msg.TargetSlot = targetSlot;
            NetSendToServer(&msg, sizeof(msg));
            LogVerbose("Client melee request attacker=%d target=%d", gLocalSlot, targetSlot);
        }
    }

    CWeapon* weapon = localPed->GetWeapon();
    const eWeaponType weaponType = weapon ? weapon->m_eWeaponType : WEAPONTYPE_UNARMED;
    const unsigned int ammoTotal = weapon ? weapon->m_nAmmoTotal : 0;
    const bool weaponChanged = gLastObservedLocalWeapon != (unsigned char)weaponType;
    if (weaponChanged) {
        gLastObservedLocalWeapon = (unsigned char)weaponType;
        gLastObservedLocalAmmo = ammoTotal;
    }

    if (weaponType == WEAPONTYPE_UNARMED || IsPedUsingMelee(localPed))
        return;

    CWeaponInfo* weaponInfo = GetPedCombatWeaponInfo(localPed, weaponType);
    if (!weaponInfo)
        return;

    const bool ammoDropped = ammoTotal < gLastObservedLocalAmmo;
    const bool continuousFiring = IsWeaponContinuousFire(weaponType) && weapon->m_nState == WEAPONSTATE_FIRING;
    gLastObservedLocalAmmo = ammoTotal;
    if (!ammoDropped && !continuousFiring)
        return;

    if (gNetStatus == NETSTAT_SERVER) {
        TryServerProcessWeaponHit(gLocalSlot, targetSlot, weaponType);
    } else if (gNetStatus == NETSTAT_CLIENTRUNNING) {
        CMsgWeaponAttack msg;
        msg.Message = MSG_WEAPONATTACK;
        msg.TargetSlot = targetSlot;
        msg.WeaponType = (unsigned char)weaponType;
        NetSendToServer(&msg, sizeof(msg));
        LogVerbose("Client weapon request attacker=%d target=%d weapon=%u ammo=%u state=%u",
            gLocalSlot, targetSlot, (unsigned int)weaponType, ammoTotal, (unsigned int)weapon->m_nState);
    }
}

static CVector SelectCoopRespawnPoint(int slot)
{
    int partnerSlot = GetOtherActiveSlot(slot);
    if (partnerSlot >= 0 && !IsSlotDead(partnerSlot))
        return BuildCoopOffsetPosition(GetSlotPosition(partnerSlot));
    return SelectSafeStartPoint(gNetPlayers[slot].Team);
}

static void RespawnSlot(int slot)
{
    if (slot < 0 || slot >= MAX_NETPLAYERS || !gNetPlayers[slot].bActive)
        return;

    const CVector respawnPos = SelectCoopRespawnPoint(slot);
    gNetPlayers[slot].bReportedDead = false;
    gNetPlayers[slot].LastEventTime = 0;

    if (slot == gLocalSlot) {
        CPlayerPed* localPed = FindPlayerPed();
        if (localPed) {
            localPed->Teleport(AdjustSpawnPositionAboveGround(respawnPos), true);
            localPed->m_fHealth = 100.0f;
        }
    } else {
        if (gNetPlayers[slot].pPed) {
            gNetPlayers[slot].pPed->Teleport(AdjustSpawnPositionAboveGround(respawnPos), false);
            gNetPlayers[slot].pPed->m_fHealth = 100.0f;
        }

        CMsgRespawn msg;
        msg.Message = MSG_RESPAWN;
        msg.PlayerSlot = slot;
        const CVector safeRespawnPos = AdjustSpawnPositionAboveGround(respawnPos);
        msg.PosX = safeRespawnPos.x;
        msg.PosY = safeRespawnPos.y;
        msg.PosZ = safeRespawnPos.z;
        NetSendTo(&msg, sizeof(msg), gNetPlayers[slot].SockAddr);
    }
}

static void Server_UpdateCoop()
{
    if (!gCoopMode || gNetStatus != NETSTAT_SERVER)
        return;

    const unsigned int now = CTimer::m_snTimeInMilliseconds;

    for (int slot = 0; slot < MAX_NETPLAYERS; slot++) {
        if (!gNetPlayers[slot].bActive)
            continue;

        const bool dead = IsSlotDead(slot);
        if (dead) {
            if (!gNetPlayers[slot].bReportedDead) {
                gNetPlayers[slot].bReportedDead = true;
                gNetPlayers[slot].LastEventTime = now;
                RegisterPlayerKill(slot, -1);
            } else if (now - gNetPlayers[slot].LastEventTime >= COOP_RESPAWN_DELAY_MS) {
                RespawnSlot(slot);
            }
        } else {
            gNetPlayers[slot].bReportedDead = false;
            gNetPlayers[slot].LastEventTime = 0;
        }
    }

    if (CountActivePlayers() != COOP_MAX_PLAYERS)
        return;
    if (now - gLastCoopCatchup < COOP_CATCHUP_INTERVAL_MS)
        return;
    if (IsSlotDead(0) || IsSlotDead(1))
        return;

    CVector hostPos = GetSlotPosition(0);
    CVector clientPos = GetSlotPosition(1);
    if ((hostPos - clientPos).Magnitude() <= COOP_CATCHUP_DISTANCE)
        return;

    gLastCoopCatchup = now;
    RespawnSlot(1);
    BroadcastText("Co-op catch-up teleport", 180, 220, 255);
}

// ============================================================
//  SERVER-SIDE GAME MODE UPDATE  (ported from CNetGames::Update)
// ============================================================

static void Server_UpdateStashTheCash()
{
    // State 0: pick new cash/stash positions
    if (gGameState == 0) {
        if (gNumCashGenerators < 2 || gNumStashLocations < 2) return;
        int oldGen = gCurrentCashGenerator;
        while (gCurrentCashGenerator == oldGen && gNumCashGenerators > 1)
            gCurrentCashGenerator = CGeneral::GetRandomNumber() % gNumCashGenerators;
        gCurrentStashLocs[0] = CGeneral::GetRandomNumber() % gNumStashLocations;
        gCurrentStashLocs[1] = CGeneral::GetRandomNumber() % gNumStashLocations;
        while (gCurrentStashLocs[0] == gCurrentStashLocs[1])
            gCurrentStashLocs[1] = CGeneral::GetRandomNumber() % gNumStashLocations;
        gCashCoorsX = gCashGenerators[gCurrentCashGenerator * 3];
        gCashCoorsY = gCashGenerators[gCurrentCashGenerator * 3 + 1];
        gCashCoorsZ = gCashGenerators[gCurrentCashGenerator * 3 + 2];
        gCashCarrierSlot = -1;
        gGameState = 1;
    }

    // State 1: game running
    if (gGameState == 1) {
        // Check if the cash carrier stashed it
        if (gCashCarrierSlot >= 0) {
            NetPlayer& carrier = gNetPlayers[gCashCarrierSlot];
            if (carrier.bActive && carrier.pPed) {
                int team = carrier.Team;
                if (team >= 0 && team < 2) {
                    float sx = gStashLocations[gCurrentStashLocs[team] * 3];
                    float sy = gStashLocations[gCurrentStashLocs[team] * 3 + 1];
                    float sz = gStashLocations[gCurrentStashLocs[team] * 3 + 2];
                    if ((carrier.pPed->GetPosition() - CVector(sx, sy, sz)).Magnitude() < 7.0f) {
                        gTeamPoints[team]++;
                        carrier.Points++;
                        char txt[128];
                        int R, G, B;
                        // Capture colour before clearing the carrier slot
                        int savedSlot = gCashCarrierSlot;
                        FindPlayerMarkerColour(savedSlot, &R, &G, &B);
                        gGameState = 0;
                        gCashCarrierSlot = -1;
                        sprintf_s(txt, "%s stashed the cash", carrier.Name);
                        BroadcastText(txt, (unsigned char)R, (unsigned char)G, (unsigned char)B);
                    }
                    else {
                        gCashCoorsX = carrier.pPed->GetPosition().x;
                        gCashCoorsY = carrier.pPed->GetPosition().y;
                        gCashCoorsZ = carrier.pPed->GetPosition().z;
                    }
                }
            }
            else {
                gCashCarrierSlot = -1;
            }
        }
        // Check if someone picks up the cash
        if (gCashCarrierSlot < 0) {
            for (int i = 0; i < MAX_NETPLAYERS; i++) {
                if (!gNetPlayers[i].bActive || !gNetPlayers[i].pPed) continue;
                if ((gNetPlayers[i].pPed->GetPosition() -
                    CVector(gCashCoorsX, gCashCoorsY, gCashCoorsZ)).Magnitude() < 5.0f) {
                    gCashCarrierSlot = i;
                    char txt[128];
                    int R, G, B; FindPlayerMarkerColour(i, &R, &G, &B);
                    sprintf_s(txt, "%s collected the cash", gNetPlayers[i].Name);
                    BroadcastText(txt, (unsigned char)R, (unsigned char)G, (unsigned char)B);
                    break;
                }
            }
        }
    }
    // Send state to clients every ~330ms
    if (CTimer::m_snTimeInMilliseconds / 330 != CTimer::m_snPreviousTimeInMilliseconds / 330) {
        CMsgStashTheCashUpdate msg;
        msg.Message = MSG_STASHTHECASH;
        msg.CashCoorsX = gCashCoorsX;
        msg.CashCoorsY = gCashCoorsY;
        msg.CashCoorsZ = gCashCoorsZ;
        msg.CashCarPlayerSlot = gCashCarrierSlot;
        msg.CurrentStashLocations[0] = gCurrentStashLocs[0];
        msg.CurrentStashLocations[1] = gCurrentStashLocs[1];
        NetBroadcast(&msg, sizeof(msg));
    }
}

static void Server_UpdateCTF()
{
    if (gGameState == 0) {
        if (gNumCTFBases < 2) { gGameState = 1; return; }
        // Pick two distinct bases; decrement minDist each retry and cap
        // iterations to prevent an infinite loop when all bases are clustered.
        int minDist = 1000;
        const int kMaxRetries = 2000;
        int retries = 0;
        gCurrentStashLocs[0] = CGeneral::GetRandomNumber() % gNumCTFBases;
        gCurrentStashLocs[1] = CGeneral::GetRandomNumber() % gNumCTFBases;
        while (retries < kMaxRetries &&
            (gCurrentStashLocs[0] == gCurrentStashLocs[1] ||
                (CVector(gCTFBases[gCurrentStashLocs[0] * 3], gCTFBases[gCurrentStashLocs[0] * 3 + 1], 0.0f) -
                    CVector(gCTFBases[gCurrentStashLocs[1] * 3], gCTFBases[gCurrentStashLocs[1] * 3 + 1], 0.0f)).Magnitude() < minDist)) {
            gCurrentStashLocs[1] = CGeneral::GetRandomNumber() % gNumCTFBases;
            minDist = std::max(0, minDist - 1);
            retries++;
        }
        for (int t = 0; t < NUM_TEAMS; t++) {
            gFlagCoors[t] = CVector(
                gCTFBases[gCurrentStashLocs[t] * 3],
                gCTFBases[gCurrentStashLocs[t] * 3 + 1],
                gCTFBases[gCurrentStashLocs[t] * 3 + 2]);
            gPlayerCarryingFlag[t] = -1;
        }
        gGameState = 1;
    }

    if (gGameState == 1) {
        for (int team = 0; team < NUM_TEAMS; team++) {
            int carrier = gPlayerCarryingFlag[team];
            if (carrier >= 0) {
                NetPlayer& cp = gNetPlayers[carrier];
                if (cp.bActive && cp.pPed) {
                    gFlagCoors[team] = cp.pPed->GetPosition();
                    // Dead carrier drops flag
                    bool isDead = (cp.pPed->m_ePedState == PEDSTATE_DEAD ||
                        cp.pPed->m_ePedState == PEDSTATE_DIE);
                    if (!cp.bActive || isDead) {
                        gPlayerCarryingFlag[team] = -1;
                        BroadcastText("A flag has been dropped",
                            (unsigned char)kTeamColours[team * 3],
                            (unsigned char)kTeamColours[team * 3 + 1],
                            (unsigned char)kTeamColours[team * 3 + 2]);
                    }
                    else {
                        // Check capture
                        int homeBase = gCurrentStashLocs[cp.Team];
                        if (homeBase < gNumCTFBases) {
                            float hx = gCTFBases[homeBase * 3];
                            float hy = gCTFBases[homeBase * 3 + 1];
                            float hz = gCTFBases[homeBase * 3 + 2];
                            if ((cp.pPed->GetPosition() - CVector(hx, hy, hz)).Magnitude() < 7.0f) {
                                gFlagCoors[team] = CVector(
                                    gCTFBases[gCurrentStashLocs[team] * 3],
                                    gCTFBases[gCurrentStashLocs[team] * 3 + 1],
                                    gCTFBases[gCurrentStashLocs[team] * 3 + 2]);
                                gTeamPoints[cp.Team]++;
                                cp.Points++;
                                gPlayerCarryingFlag[team] = -1;
                                char txt[128];
                                int R, G, B; FindPlayerMarkerColour(carrier, &R, &G, &B);
                                sprintf_s(txt, "%s captured a flag", cp.Name);
                                BroadcastText(txt, (unsigned char)R, (unsigned char)G, (unsigned char)B);
                            }
                        }
                    }
                }
                else {
                    gPlayerCarryingFlag[team] = -1;
                }
            }
            else {
                // Try pickup
                for (int p = 0; p < MAX_NETPLAYERS; p++) {
                    NetPlayer& pp = gNetPlayers[p];
                    if (!pp.bActive || !pp.pPed) continue;
                    if (pp.Team == team) continue; // can't pick up own team's flag
                    bool dead = (pp.pPed->m_ePedState == PEDSTATE_DEAD || pp.pPed->m_ePedState == PEDSTATE_DIE);
                    if (dead) continue;
                    if ((pp.pPed->GetPosition() - gFlagCoors[team]).Magnitude() < 4.0f) {
                        gPlayerCarryingFlag[team] = p;
                        char txt[128];
                        int R, G, B; FindPlayerMarkerColour(p, &R, &G, &B);
                        sprintf_s(txt, "%s picked up the enemy flag", pp.Name);
                        BroadcastText(txt, (unsigned char)R, (unsigned char)G, (unsigned char)B);
                        break;
                    }
                }
                // Try return
                for (int p = 0; p < MAX_NETPLAYERS; p++) {
                    NetPlayer& pp = gNetPlayers[p];
                    if (!pp.bActive || !pp.pPed || pp.Team != team) continue;
                    if ((pp.pPed->GetPosition() - gFlagCoors[team]).Magnitude() < 4.0f) {
                        CVector base(gCTFBases[gCurrentStashLocs[team] * 3],
                            gCTFBases[gCurrentStashLocs[team] * 3 + 1],
                            gCTFBases[gCurrentStashLocs[team] * 3 + 2]);
                        if ((gFlagCoors[team] - base).Magnitude() > 1.0f) {
                            gFlagCoors[team] = base;
                            char txt[128];
                            int R, G, B; FindPlayerMarkerColour(p, &R, &G, &B);
                            sprintf_s(txt, "%s returned a flag to base", pp.Name);
                            BroadcastText(txt, (unsigned char)R, (unsigned char)G, (unsigned char)B);
                        }
                    }
                }
            }
        }
    }
    // Send update every ~330ms
    if (CTimer::m_snTimeInMilliseconds / 330 != CTimer::m_snPreviousTimeInMilliseconds / 330) {
        CMsgCaptureTheFlagUpdate msg;
        msg.Message = MSG_CAPTURETHEFLAG;
        for (int t = 0; t < NUM_TEAMS; t++) {
            msg.FlagCoordinates[t * 3] = gFlagCoors[t].x;
            msg.FlagCoordinates[t * 3 + 1] = gFlagCoors[t].y;
            msg.FlagCoordinates[t * 3 + 2] = gFlagCoors[t].z;
        }
        msg.CurrentStashLocations[0] = gCurrentStashLocs[0];
        msg.CurrentStashLocations[1] = gCurrentStashLocs[1];
        NetBroadcast(&msg, sizeof(msg));
    }
}

static void Server_UpdateDomination()
{
    if (!gGameState) {
        gGameState = 1;
        if (gNumStashLocations >= 3) {
            gDomBases[0] = CGeneral::GetRandomNumber() % gNumStashLocations;
            gDomBases[1] = CGeneral::GetRandomNumber() % gNumStashLocations;
            while (gDomBases[0] == gDomBases[1])
                gDomBases[1] = CGeneral::GetRandomNumber() % gNumStashLocations;
            gDomBases[2] = CGeneral::GetRandomNumber() % gNumStashLocations;
            while (gDomBases[2] == gDomBases[0] || gDomBases[2] == gDomBases[1])
                gDomBases[2] = CGeneral::GetRandomNumber() % gNumStashLocations;
        }
        gDomTeam[0] = gDomTeam[1] = gDomTeam[2] = -1;
    }

    unsigned int now = CTimer::m_snTimeInMilliseconds;
    if (now - gLastDomCheck > 500) {
        gLastDomCheck = now;
        for (int s = 0; s < MAX_NETPLAYERS; s++) {
            if (!gNetPlayers[s].bActive || !gNetPlayers[s].pPed) continue;
            for (int b = 0; b < DOMINATIONBASES; b++) {
                if (b >= gNumStashLocations) break;
                if (gNetPlayers[s].Team == gDomTeam[b]) continue;
                float bx = gStashLocations[gDomBases[b] * 3];
                float by = gStashLocations[gDomBases[b] * 3 + 1];
                float bz = gStashLocations[gDomBases[b] * 3 + 2];
                if ((gNetPlayers[s].pPed->GetPosition() - CVector(bx, by, bz)).Magnitude() < 5.0f) {
                    gDomTeam[b] = gNetPlayers[s].Team;
                    char txt[128];
                    int R, G, B; FindPlayerMarkerColour(s, &R, &G, &B);
                    sprintf_s(txt, "%s claimed a base", gNetPlayers[s].Name);
                    BroadcastText(txt, (unsigned char)R, (unsigned char)G, (unsigned char)B);
                }
            }
        }
    }
    // Tick-based scoring
    for (int b = 0; b < DOMINATIONBASES; b++) {
        if (gDomTeam[b] >= 0)
            gTeamPoints[gDomTeam[b]] += (int)(CTimer::ms_fTimeStep * 20.0f);
    }

    // Send update every ~330ms
    if (now / 330 != CTimer::m_snPreviousTimeInMilliseconds / 330) {
        CMsgDominationUpdate msg;
        msg.Message = MSG_DOMINATIONUPDATE;
        for (int b = 0; b < DOMINATIONBASES; b++) {
            msg.DominationBases[b] = gDomBases[b];
            msg.TeamDominatingBases[b] = gDomTeam[b];
        }
        NetBroadcast(&msg, sizeof(msg));
    }
}

// Master server-side game mode tick
static void Server_UpdateGameModes()
{
    switch (gGameType) {
    case GAMETYPE_STASHTHECASH:
        Server_UpdateStashTheCash();
        break;
    case GAMETYPE_CAPTURETHEFLAG:
        Server_UpdateCTF();
        break;
    case GAMETYPE_DOMINATION:
        Server_UpdateDomination();
        break;
    default:
        break;  // DM/TDM scoring is purely on-kill, no per-frame logic needed
    }

    // Broadcast team points periodically (team games)
    if (TeamGameGoingOn() &&
        CTimer::m_snTimeInMilliseconds / 1100 != CTimer::m_snPreviousTimeInMilliseconds / 1100)
        BroadcastTeamPoints();
}

// ============================================================
//  CLIENT-SIDE HANDLING OF SERVER MESSAGES
// ============================================================

static void Client_HandleMessage(const unsigned char* buf, int /*size*/)
{
    const CMsgGeneric* hdr = (const CMsgGeneric*)buf;

    // Every packet from the server is proof that the connection is alive
    gLastServerMsgTime = CTimer::m_snTimeInMilliseconds;

    switch (hdr->Message) {
    case MSG_SYNC:
    {
        const CMsgSync* msg = (const CMsgSync*)buf;
        if (msg->PlayerNumberOfClient < 0 || msg->PlayerNumberOfClient >= MAX_NETPLAYERS) {
            Chat_AddLine("Server is full for co-op", 255, 120, 120);
            SwitchToSinglePlayer();
            break;
        }

        const int previousLocalSlot = gLocalSlot;
        if (previousLocalSlot != msg->PlayerNumberOfClient &&
            previousLocalSlot >= 0 && previousLocalSlot < MAX_NETPLAYERS)
        {
            gNetPlayers[previousLocalSlot].pPed = nullptr;
            if (previousLocalSlot != 0)
                gNetPlayers[previousLocalSlot].Init();
        }

        gLocalSlot = msg->PlayerNumberOfClient;
        gNetPlayers[gLocalSlot].bActive = true;
        strncpy_s(gNetPlayers[gLocalSlot].Name, gLocalPlayerName, NET_MAX_NAME_SIZE - 1);
        gNetPlayers[gLocalSlot].R = gLocalR;
        gNetPlayers[gLocalSlot].G = gLocalG;
        gNetPlayers[gLocalSlot].B = gLocalB;
        BindLocalPlayerToSlot(gLocalSlot);

        CPlayerPed* localPed = FindPlayerPed();
        if (localPed) {
            const CVector joinPos = AdjustSpawnPositionAboveGround(CVector(msg->PedCoorsX, msg->PedCoorsY, msg->PedCoorsZ));
            localPed->Teleport(joinPos, true);
            localPed->m_fHealth = 100.0f;
            LogMessage("Client join teleport slot=%d pos=(%.2f,%.2f,%.2f)", gLocalSlot, joinPos.x, joinPos.y, joinPos.z);
        }

        // Clock sync: keep a separate offset rather than nudging CTimer directly
        unsigned int now = CTimer::m_snTimeInMilliseconds;
        if (now - msg->LocalTimeOnSend < 2000) {
            long long diff = (long long)msg->ServerTimeOnBounce -
                ((long long)now + (long long)msg->LocalTimeOnSend) / 2;
            if (gSyncsDone == 0) gTimeDiffMS = diff;
            else gTimeDiffMS += diff;
            gSyncsDone++;
            if (gSyncsDone >= 3) {
                // Apply accumulated clock offset once after 3 round-trips.
                // We add it only to our tracking variable; modifying
                // CTimer::m_snTimeInMilliseconds globally would corrupt physics/anim.
                gTimeDiffMS = gTimeDiffMS / 3; // now holds avg offset
                gNetStatus = NETSTAT_CLIENTRUNNING;
                gNetPlayers[gLocalSlot].LastMsgTime = CTimer::m_snTimeInMilliseconds;
                Chat_AddLine("Connected to server", 0, 255, 0);
            }
        }
        break;
    }
    case MSG_UPDATEPED:
    {
        const CMsgUpdatePed* msg = (const CMsgUpdatePed*)buf;
        int slot = msg->PlayerSlot;
        if (slot < 0 || slot >= MAX_NETPLAYERS || slot == gLocalSlot) break;
        if (!gNetPlayers[slot].bActive) break;
        gNetPlayers[slot].TargetPos = CVector(msg->PosX, msg->PosY, msg->PosZ);
        gNetPlayers[slot].TargetMoveSpeed = CVector(msg->MoveSpeedX, msg->MoveSpeedY, msg->MoveSpeedZ);
        gNetPlayers[slot].TargetHeading = msg->Heading;
        gNetPlayers[slot].TargetHealth = msg->Health;
        gNetPlayers[slot].ModelIndex = SanitizeRemotePedModel(msg->ModelIndex ? msg->ModelIndex : MODEL_MALE01);
        gNetPlayers[slot].InputBits = msg->InputBits;
        gNetPlayers[slot].AnalogLeftRight = msg->AnalogLeftRight;
        gNetPlayers[slot].AnalogUpDown = msg->AnalogUpDown;
        gNetPlayers[slot].WeaponAmmoInClip = msg->WeaponAmmoInClip;
        gNetPlayers[slot].CurrentWeapon = msg->CurrentWeapon;
        gNetPlayers[slot].WeaponState = msg->WeaponState;
        gNetPlayers[slot].FightingStyle = msg->FightingStyle;
        gNetPlayers[slot].Ducking = msg->Ducking;
        gNetPlayers[slot].WeaponAmmo = msg->WeaponAmmo;
        gNetPlayers[slot].MoveState = msg->MoveState;
        gNetPlayers[slot].WeaponModelId = -1;
        gNetPlayers[slot].AnimGroup = 0;
        gNetPlayers[slot].AnimState.fill(0);
        gNetPlayers[slot].bReportedDead = (msg->PedStateFlags & 2) != 0;
        gNetPlayers[slot].bHasPedSync = true;
        if (CTimer::m_snTimeInMilliseconds - gLastRemoteDebugLog[slot] >= 1000) {
            gLastRemoteDebugLog[slot] = CTimer::m_snTimeInMilliseconds;
            LogVerbose("Client recv slot=%d model=%u weap=%u ammo=%u clip=%u state=%u move=%u keys=0x%X pos=(%.2f,%.2f,%.2f) speed=(%.3f,%.3f,%.3f) flags=%u",
                slot, gNetPlayers[slot].ModelIndex, gNetPlayers[slot].CurrentWeapon, gNetPlayers[slot].WeaponAmmo, gNetPlayers[slot].WeaponAmmoInClip,
                gNetPlayers[slot].WeaponState, gNetPlayers[slot].MoveState, gNetPlayers[slot].InputBits,
                gNetPlayers[slot].TargetPos.x, gNetPlayers[slot].TargetPos.y, gNetPlayers[slot].TargetPos.z,
                gNetPlayers[slot].TargetMoveSpeed.x, gNetPlayers[slot].TargetMoveSpeed.y, gNetPlayers[slot].TargetMoveSpeed.z,
                gNetPlayers[slot].bReportedDead ? 2 : 0);
        }
        if (gNetPlayers[slot].pPed) {
            if (gNetPlayers[slot].bVisualsReady) {
                LogVerbose("Client slot=%d sync queued for frame update", slot);
            } else {
                gNetPlayers[slot].bVisualsReady = true;
                LogMessage("Client slot=%d visuals armed on second sync", slot);
            }
        }
        else {
            CreateRemotePlayerPed(slot, gNetPlayers[slot].TargetPos);
        }
        break;
    }
    case MSG_PLAYERNAMES:
    {
        const CMsgPlayerNames* msg = (const CMsgPlayerNames*)buf;
        for (int i = 0; i < MAX_NETPLAYERS; i++) {
            if (i == gLocalSlot) continue;
            gNetPlayers[i].bActive = (msg->Active[i] != 0);
            if (gNetPlayers[i].bActive) {
                strncpy_s(gNetPlayers[i].Name, msg->Names[i], NET_MAX_NAME_SIZE - 1);
                gNetPlayers[i].R = msg->R[i];
                gNetPlayers[i].G = msg->G[i];
                gNetPlayers[i].B = msg->B[i];
                gNetPlayers[i].Team = msg->Team[i];
                gNetPlayers[i].Points = msg->Points[i];
            }
            else {
                RemoveRemotePlayerPed(i);
                ClearSlotBlip(i);
            }
        }
        break;
    }
    case MSG_TEXT:
    {
        const CMsgText* msg = (const CMsgText*)buf;
        Chat_AddLine(msg->String, msg->ColourR, msg->ColourG, msg->ColourB);
        break;
    }
    case MSG_TEAMPOINTS:
    {
        const CMsgTeamPoints* msg = (const CMsgTeamPoints*)buf;
        gTeamPoints[0] = msg->TeamPoints[0];
        gTeamPoints[1] = msg->TeamPoints[1];
        break;
    }
    case MSG_STASHTHECASH:
    {
        const CMsgStashTheCashUpdate* msg = (const CMsgStashTheCashUpdate*)buf;
        gCashCoorsX = msg->CashCoorsX;
        gCashCoorsY = msg->CashCoorsY;
        gCashCoorsZ = msg->CashCoorsZ;
        gCashCarrierSlot = msg->CashCarPlayerSlot;
        gCurrentStashLocs[0] = msg->CurrentStashLocations[0];
        gCurrentStashLocs[1] = msg->CurrentStashLocations[1];
        break;
    }
    case MSG_CAPTURETHEFLAG:
    {
        const CMsgCaptureTheFlagUpdate* msg = (const CMsgCaptureTheFlagUpdate*)buf;
        for (int t = 0; t < NUM_TEAMS; t++) {
            gFlagCoors[t].x = msg->FlagCoordinates[t * 3];
            gFlagCoors[t].y = msg->FlagCoordinates[t * 3 + 1];
            gFlagCoors[t].z = msg->FlagCoordinates[t * 3 + 2];
        }
        gCurrentStashLocs[0] = msg->CurrentStashLocations[0];
        gCurrentStashLocs[1] = msg->CurrentStashLocations[1];
        break;
    }
    case MSG_RATRACE:
    {
        const CMsgRatRaceUpdate* msg = (const CMsgRatRaceUpdate*)buf;
        gRatPickupCoors.x = msg->PickupCoorsX;
        gRatPickupCoors.y = msg->PickupCoorsY;
        gRatPickupCoors.z = msg->PickupCoorsZ;
        break;
    }
    case MSG_DOMINATIONUPDATE:
    {
        const CMsgDominationUpdate* msg = (const CMsgDominationUpdate*)buf;
        for (int b = 0; b < DOMINATIONBASES; b++) {
            gDomBases[b] = msg->DominationBases[b];
            gDomTeam[b] = msg->TeamDominatingBases[b];
        }
        break;
    }
    case MSG_RESPAWN:
    {
        const CMsgRespawn* msg = (const CMsgRespawn*)buf;
        if (msg->PlayerSlot == gLocalSlot) {
            CPlayerPed* localPed = FindPlayerPed();
            if (localPed) {
                const CVector respawnPos = AdjustSpawnPositionAboveGround(CVector(msg->PosX, msg->PosY, msg->PosZ));
                localPed->Teleport(respawnPos, true);
                localPed->m_fHealth = 100.0f;
                LogMessage("Client respawn teleport slot=%d pos=(%.2f,%.2f,%.2f)", gLocalSlot, respawnPos.x, respawnPos.y, respawnPos.z);
            }
            gNetPlayers[gLocalSlot].bReportedDead = false;
        }
        break;
    }
    case MSG_DAMAGE:
    {
        const CMsgDamage* msg = (const CMsgDamage*)buf;
        if (msg->TargetSlot == gLocalSlot) {
            CPlayerPed* localPed = FindPlayerPed();
            if (localPed)
                localPed->m_fHealth = (gCoopMode && msg->Killed != 0) ? 1.0f : msg->NewHealth;
            gNetPlayers[gLocalSlot].TargetHealth = msg->NewHealth;
            gNetPlayers[gLocalSlot].bReportedDead = msg->Killed != 0;
            LogMessage("Client damage applied attacker=%d target=%d weapon=%u health=%.1f killed=%u",
                msg->AttackerSlot, msg->TargetSlot, msg->WeaponType, msg->NewHealth, msg->Killed);
        }
        break;
    }
    case MSG_ANIMEVENT:
    {
        const CMsgAnimEvent* msg = (const CMsgAnimEvent*)buf;
        if (msg->PlayerSlot >= 0 && msg->PlayerSlot < MAX_NETPLAYERS && msg->PlayerSlot != gLocalSlot) {
            QueueRemoteAnimEvent(msg->PlayerSlot, msg->EventType, msg->WeaponType);
            LogVerbose("Client anim event slot=%d event=%u weapon=%u", msg->PlayerSlot, msg->EventType, msg->WeaponType);
        }
        break;
    }
    default:
        break;
    }
}

// ============================================================
//  SERVER-SIDE HANDLING OF CLIENT MESSAGES
// ============================================================

static void Server_HandleMessage(const unsigned char* buf, int /*size*/,
    const sockaddr_in& fromAddr)
{
    const CMsgGeneric* hdr = (const CMsgGeneric*)buf;

    // Find or allocate a player slot for this sender
    int slot = FindSlotBySockAddr(fromAddr);
    if (slot < 0) {
        if (hdr->Message != MSG_SYNC) return; // ignore unknown unless joining
        if (gCoopMode && CountActivePlayers() >= COOP_MAX_PLAYERS) {
            const CMsgSync* msg = (const CMsgSync*)buf;
            CMsgSync reject = *msg;
            reject.ServerTimeOnBounce = CTimer::m_snTimeInMilliseconds;
            reject.PlayerNumberOfClient = -1;
            reject.PedCoorsX = reject.PedCoorsY = reject.PedCoorsZ = 0.0f;
            NetSendTo(&reject, sizeof(reject), fromAddr);
            return;
        }
        slot = AllocClientSlot(fromAddr);
        if (slot < 0) return; // server full
    }

    gNetPlayers[slot].LastMsgTime = CTimer::m_snTimeInMilliseconds;

    switch (hdr->Message) {
    case MSG_SYNC:
    {
        const CMsgSync* msg = (const CMsgSync*)buf;
        const bool firstHandshake = !gNetPlayers[slot].bHandshakeComplete;
        strncpy_s(gNetPlayers[slot].Name, msg->PlayerName, NET_MAX_NAME_SIZE - 1);
        gNetPlayers[slot].R = msg->R;
        gNetPlayers[slot].G = msg->G;
        gNetPlayers[slot].B = msg->B;
        if (firstHandshake) {
            gNetPlayers[slot].Points = 0;
            gNetPlayers[slot].Points2 = 0;
            gNetPlayers[slot].Team = FindTeamForNewPlayer(slot);

            // Spawn ped for this new player if needed
            if (!gNetPlayers[slot].pPed) {
                CVector spawnPos = SelectSafeStartPoint(gNetPlayers[slot].Team);
                CreateRemotePlayerPed(slot, spawnPos);
            }
            gNetPlayers[slot].bHandshakeComplete = true;
        }

        // Bounce MSG_SYNC back with assigned slot + position
        CMsgSync reply = *msg;
        reply.ServerTimeOnBounce = CTimer::m_snTimeInMilliseconds;
        reply.PlayerNumberOfClient = slot;
        if (gNetPlayers[slot].pPed) {
            reply.PedCoorsX = gNetPlayers[slot].pPed->GetPosition().x;
            reply.PedCoorsY = gNetPlayers[slot].pPed->GetPosition().y;
            reply.PedCoorsZ = gNetPlayers[slot].pPed->GetPosition().z;
        }
        NetSendTo(&reply, sizeof(reply), fromAddr);

        if (firstHandshake) {
            char txt[128];
            sprintf_s(txt, "%s joined the game", gNetPlayers[slot].Name);
            BroadcastText(txt, 200, 200, 200);
            LogMessage("Handshake complete for slot=%d name=%s", slot, gNetPlayers[slot].Name);
        } else {
            LogMessage("Duplicate sync for slot=%d name=%s ignored as re-handshake", slot, gNetPlayers[slot].Name);
        }
        break;
    }
    case MSG_UPDATEPED:
    {
        const CMsgUpdatePed* msg = (const CMsgUpdatePed*)buf;
        if (gNetPlayers[slot].pPed) {
            gNetPlayers[slot].TargetPos = CVector(msg->PosX, msg->PosY, msg->PosZ);
            gNetPlayers[slot].TargetMoveSpeed = CVector(msg->MoveSpeedX, msg->MoveSpeedY, msg->MoveSpeedZ);
            gNetPlayers[slot].TargetHeading = msg->Heading;
            gNetPlayers[slot].TargetHealth = msg->Health;
            gNetPlayers[slot].ModelIndex = SanitizeRemotePedModel(msg->ModelIndex ? msg->ModelIndex : gNetPlayers[slot].ModelIndex);
            gNetPlayers[slot].InputBits = msg->InputBits;
            gNetPlayers[slot].AnalogLeftRight = msg->AnalogLeftRight;
            gNetPlayers[slot].AnalogUpDown = msg->AnalogUpDown;
            gNetPlayers[slot].WeaponAmmoInClip = msg->WeaponAmmoInClip;
            gNetPlayers[slot].CurrentWeapon = msg->CurrentWeapon;
            gNetPlayers[slot].WeaponState = msg->WeaponState;
            gNetPlayers[slot].FightingStyle = msg->FightingStyle;
            gNetPlayers[slot].Ducking = msg->Ducking;
            gNetPlayers[slot].WeaponAmmo = msg->WeaponAmmo;
            gNetPlayers[slot].MoveState = msg->MoveState;
            gNetPlayers[slot].WeaponModelId = -1;
            gNetPlayers[slot].AnimGroup = 0;
            gNetPlayers[slot].AnimState.fill(0);
            gNetPlayers[slot].bHasPedSync = true;
            if (CTimer::m_snTimeInMilliseconds - gLastRemoteDebugLog[slot] >= 1000) {
                gLastRemoteDebugLog[slot] = CTimer::m_snTimeInMilliseconds;
                LogVerbose("Server recv slot=%d model=%u weap=%u ammo=%u clip=%u state=%u move=%u keys=0x%X pos=(%.2f,%.2f,%.2f) speed=(%.3f,%.3f,%.3f) flags=%u",
                    slot, gNetPlayers[slot].ModelIndex, gNetPlayers[slot].CurrentWeapon, gNetPlayers[slot].WeaponAmmo, gNetPlayers[slot].WeaponAmmoInClip,
                    gNetPlayers[slot].WeaponState, gNetPlayers[slot].MoveState, gNetPlayers[slot].InputBits,
                    gNetPlayers[slot].TargetPos.x, gNetPlayers[slot].TargetPos.y, gNetPlayers[slot].TargetPos.z,
                    gNetPlayers[slot].TargetMoveSpeed.x, gNetPlayers[slot].TargetMoveSpeed.y, gNetPlayers[slot].TargetMoveSpeed.z,
                    gNetPlayers[slot].bReportedDead ? 2 : 0);
            }
            if (gNetPlayers[slot].bVisualsReady) {
                LogVerbose("Server slot=%d sync queued for frame update", slot);
            } else {
                gNetPlayers[slot].bVisualsReady = true;
                LogMessage("Server slot=%d visuals armed on second sync", slot);
            }
        }
        gNetPlayers[slot].bReportedDead = (msg->PedStateFlags & 2) != 0;
        // Re-broadcast to all other clients
        CMsgUpdatePed fwd = *msg;
        fwd.PlayerSlot = slot;
        for (int i = 1; i < MAX_NETPLAYERS; i++) {
            if (i == slot || !gNetPlayers[i].bActive) continue;
            NetSendTo(&fwd, sizeof(fwd), gNetPlayers[i].SockAddr);
        }
        break;
    }
    case MSG_TEXT:
    {
        const CMsgText* msg = (const CMsgText*)buf;
        Chat_AddLine(msg->String, msg->ColourR, msg->ColourG, msg->ColourB);
        NetBroadcast(msg, sizeof(CMsgText));
        break;
    }
    case MSG_MELEEATTACK:
    {
        const CMsgMeleeAttack* msg = (const CMsgMeleeAttack*)buf;
        const int targetSlot = (msg->TargetSlot >= 0 && msg->TargetSlot < MAX_NETPLAYERS)
            ? msg->TargetSlot
            : GetOtherActiveSlot(slot);
        LogVerbose("Server melee request attacker=%d target=%d", slot, targetSlot);
        TryServerProcessMeleeHit(slot, targetSlot);
        break;
    }
    case MSG_WEAPONATTACK:
    {
        const CMsgWeaponAttack* msg = (const CMsgWeaponAttack*)buf;
        const int targetSlot = (msg->TargetSlot >= 0 && msg->TargetSlot < MAX_NETPLAYERS)
            ? msg->TargetSlot
            : GetOtherActiveSlot(slot);
        LogVerbose("Server weapon request attacker=%d target=%d weapon=%u", slot, targetSlot, (unsigned int)msg->WeaponType);
        TryServerProcessWeaponHit(slot, targetSlot, (eWeaponType)msg->WeaponType);
        break;
    }
    case MSG_ANIMEVENT:
    {
        const CMsgAnimEvent* msg = (const CMsgAnimEvent*)buf;
        if (msg->PlayerSlot == slot) {
            QueueRemoteAnimEvent(slot, msg->EventType, msg->WeaponType);
            for (int i = 1; i < MAX_NETPLAYERS; i++) {
                if (i == slot || !gNetPlayers[i].bActive) continue;
                NetSendTo(msg, sizeof(CMsgAnimEvent), gNetPlayers[i].SockAddr);
            }
            LogVerbose("Server anim event slot=%d event=%u weapon=%u", slot, msg->EventType, msg->WeaponType);
        }
        break;
    }
    case MSG_PLAYERQUIT:
    {
        char txt[128];
        sprintf_s(txt, "%s left the game", gNetPlayers[slot].Name);
        BroadcastText(txt, 255, 100, 100);
        DisconnectPlayer(slot);
        break;
    }
    default:
        break;
    }
}

// ============================================================
//  SERVER: broadcast player name list to all clients
// ============================================================

static void Server_BroadcastPlayerNames()
{
    CMsgPlayerNames msg;
    msg.Message = MSG_PLAYERNAMES;
    for (int i = 0; i < MAX_NETPLAYERS; i++) {
        msg.Active[i] = (unsigned char)(gNetPlayers[i].bActive ? 1 : 0);
        strncpy_s(msg.Names[i], gNetPlayers[i].Name, NET_MAX_NAME_SIZE - 1);
        msg.R[i] = gNetPlayers[i].R;
        msg.G[i] = gNetPlayers[i].G;
        msg.B[i] = gNetPlayers[i].B;
        msg.Team[i] = (signed char)gNetPlayers[i].Team;
        msg.Points[i] = gNetPlayers[i].Points;
    }
    NetBroadcast(&msg, sizeof(msg));
}

// ============================================================
//  SERVER: send ped positions to each client
// ============================================================

static void Server_BroadcastPeds()
{
    const unsigned int now = CTimer::m_snTimeInMilliseconds;
    for (int s = 0; s < MAX_NETPLAYERS; s++) {
        if (!gNetPlayers[s].bActive || !gNetPlayers[s].pPed) continue;
        CMsgUpdatePed msg;
        memset(&msg, 0, sizeof(msg));
        msg.Message = MSG_UPDATEPED;
        msg.PlayerSlot = s;
        msg.PosX = gNetPlayers[s].pPed->GetPosition().x;
        msg.PosY = gNetPlayers[s].pPed->GetPosition().y;
        msg.PosZ = gNetPlayers[s].pPed->GetPosition().z;
        msg.MoveSpeedX = gNetPlayers[s].pPed->m_vecMoveSpeed.x;
        msg.MoveSpeedY = gNetPlayers[s].pPed->m_vecMoveSpeed.y;
        msg.MoveSpeedZ = gNetPlayers[s].pPed->m_vecMoveSpeed.z;
        msg.Heading = gNetPlayers[s].pPed->m_fCurrentRotation;
        msg.Health = gNetPlayers[s].pPed->m_fHealth;
        msg.ModelIndex = gNetPlayers[s].pPed->m_nModelIndex;
        msg.InputBits = (s == gLocalSlot) ? CollectLocalInputBits(gNetPlayers[s].pPed) : gNetPlayers[s].InputBits;
        msg.AnalogLeftRight = (s == gLocalSlot) ? CPad::GetPad(0)->GetPedWalkLeftRight() : gNetPlayers[s].AnalogLeftRight;
        msg.AnalogUpDown = (s == gLocalSlot) ? CPad::GetPad(0)->GetPedWalkUpDown() : gNetPlayers[s].AnalogUpDown;
        {
            CWeapon* weapon = gNetPlayers[s].pPed->GetWeapon();
            msg.CurrentWeapon = weapon ? (unsigned char)weapon->m_eWeaponType : WEAPONTYPE_UNARMED;
            msg.WeaponAmmoInClip = (unsigned short)std::min<unsigned int>(weapon ? weapon->m_nAmmoInClip : 0u, 65535u);
            msg.WeaponAmmo = (unsigned short)std::min<unsigned int>(weapon ? weapon->m_nAmmoTotal : 0u, 65535u);
            msg.WeaponState = weapon ? (unsigned char)weapon->m_nState : WEAPONSTATE_READY;
            msg.FightingStyle = (unsigned char)gNetPlayers[s].pPed->m_nFightingStyle;
            msg.Ducking = gNetPlayers[s].pPed->bIsDucking ? 1 : 0;
        }
        msg.MoveState = GetLocalMoveState(gNetPlayers[s].pPed);
        msg.PedStateFlags = 0;
        if (gNetPlayers[s].pPed->bInVehicle) msg.PedStateFlags |= 1;
        if (gNetPlayers[s].pPed->m_ePedState == PEDSTATE_DEAD) msg.PedStateFlags |= 2;
        if (!UpdatePedNeedsSend(msg, gLastServerSentPed[s], now, gLastServerSentPedTime[s]))
            continue;
        gLastServerSentPed[s] = msg;
        gLastServerSentPedTime[s] = now;
        if (CTimer::m_snTimeInMilliseconds - gLastRemoteDebugLog[s] >= 3000) {
            gLastRemoteDebugLog[s] = CTimer::m_snTimeInMilliseconds;
            LogVerbose("Broadcast slot=%d model=%u weap=%u ammo=%u clip=%u state=%u duck=%u move=%u keys=0x%X pos=(%.2f,%.2f,%.2f) speed=(%.3f,%.3f,%.3f) flags=%u",
                s, msg.ModelIndex, msg.CurrentWeapon, msg.WeaponAmmo, msg.WeaponAmmoInClip, msg.WeaponState, msg.Ducking, msg.MoveState, msg.InputBits,
                msg.PosX, msg.PosY, msg.PosZ, msg.MoveSpeedX, msg.MoveSpeedY, msg.MoveSpeedZ, msg.PedStateFlags);
        }
        // Send to all clients that are NOT this player
        for (int c = 1; c < MAX_NETPLAYERS; c++) {
            if (c == s || !gNetPlayers[c].bActive) continue;
            NetSendTo(&msg, sizeof(msg), gNetPlayers[c].SockAddr);
        }
    }
}

// ============================================================
//  CLIENT: send own ped update to server
// ============================================================

static void Client_SendPedUpdate()
{
    CPlayerPed* pPed = FindPlayerPed();
    if (!pPed) return;
    CMsgUpdatePed msg;
    memset(&msg, 0, sizeof(msg));
    msg.Message = MSG_UPDATEPED;
    msg.PlayerSlot = gLocalSlot;
    msg.PosX = pPed->GetPosition().x;
    msg.PosY = pPed->GetPosition().y;
    msg.PosZ = pPed->GetPosition().z;
    msg.MoveSpeedX = pPed->m_vecMoveSpeed.x;
    msg.MoveSpeedY = pPed->m_vecMoveSpeed.y;
    msg.MoveSpeedZ = pPed->m_vecMoveSpeed.z;
    msg.Heading = pPed->m_fCurrentRotation;
    msg.Health = pPed->m_fHealth;
    msg.ModelIndex = pPed->m_nModelIndex;
    msg.InputBits = CollectLocalInputBits(pPed);
    if (CPad* pad = CPad::GetPad(0)) {
        msg.AnalogLeftRight = pad->GetPedWalkLeftRight();
        msg.AnalogUpDown = pad->GetPedWalkUpDown();
    } else {
        msg.AnalogLeftRight = 0;
        msg.AnalogUpDown = 0;
    }
    {
        CWeapon* weapon = pPed->GetWeapon();
        msg.CurrentWeapon = weapon ? (unsigned char)weapon->m_eWeaponType : WEAPONTYPE_UNARMED;
        msg.WeaponAmmoInClip = (unsigned short)std::min<unsigned int>(weapon ? weapon->m_nAmmoInClip : 0u, 65535u);
        msg.WeaponAmmo = (unsigned short)std::min<unsigned int>(weapon ? weapon->m_nAmmoTotal : 0u, 65535u);
        msg.WeaponState = weapon ? (unsigned char)weapon->m_nState : WEAPONSTATE_READY;
        msg.FightingStyle = (unsigned char)pPed->m_nFightingStyle;
        msg.Ducking = pPed->bIsDucking ? 1 : 0;
    }
    msg.MoveState = GetLocalMoveState(pPed);
    msg.PedStateFlags = (pPed->bInVehicle ? 1 : 0) |
        (pPed->m_ePedState == PEDSTATE_DEAD ? 2 : 0);
    const unsigned int now = CTimer::m_snTimeInMilliseconds;
    if (!UpdatePedNeedsSend(msg, gLastClientSentPed, now, gLastClientSentPedTime))
        return;
    gLastClientSentPed = msg;
    gLastClientSentPedTime = now;
    NetSendToServer(&msg, sizeof(msg));
}

// ============================================================
//  CLIENT: send periodic sync (clock alignment)
// ============================================================

static void Client_SendSync()
{
    CMsgSync msg;
    memset(&msg, 0, sizeof(msg));
    msg.Message = MSG_SYNC;
    msg.LocalTimeOnSend = CTimer::m_snTimeInMilliseconds;
    msg.PlayerNumberOfClient = -1;
    strncpy_s(msg.PlayerName, gLocalPlayerName, NET_MAX_NAME_SIZE - 1);
    msg.R = gLocalR; msg.G = gLocalG; msg.B = gLocalB;
    NetSendToServer(&msg, sizeof(msg));
}

// ============================================================
//  SERVER: check for client timeouts
// ============================================================

static void Server_CheckTimeouts()
{
    unsigned int now = CTimer::m_snTimeInMilliseconds;
    for (int i = 1; i < MAX_NETPLAYERS; i++) {
        if (!gNetPlayers[i].bActive) continue;
        if (now > gNetPlayers[i].LastMsgTime + NET_TIMEOUT_MS) {
            char txt[128];
            sprintf_s(txt, "%s timed out", gNetPlayers[i].Name);
            BroadcastText(txt, 255, 100, 100);
            DisconnectPlayer(i);
        }
    }
}

// ============================================================
//  SWITCH TO SINGLE-PLAYER (mirrors SwitchToSinglePlayerGame)
// ============================================================

static void SwitchToSinglePlayer()
{
    // Remove all remote player peds
    for (int i = 0; i < MAX_NETPLAYERS; i++) {
        if (i == gLocalSlot) continue;
        DisconnectPlayer(i);
    }
    // Clear game blips
    if (gTargetBlip) { CRadar::ClearBlip(gTargetBlip); gTargetBlip = 0; }
    if (gBase0Blip) { CRadar::ClearBlip(gBase0Blip);  gBase0Blip = 0; }
    if (gBase1Blip) { CRadar::ClearBlip(gBase1Blip);  gBase1Blip = 0; }
    for (int t = 0; t < NUM_TEAMS; t++) {
        if (gFlagBlip[t]) { CRadar::ClearBlip(gFlagBlip[t]); gFlagBlip[t] = 0; }
    }
    for (int b = 0; b < DOMINATIONBASES; b++) {
        if (gDomBlip[b]) { CRadar::ClearBlip(gDomBlip[b]); gDomBlip[b] = 0; }
    }

    if (gSock != INVALID_SOCKET) { closesocket(gSock); gSock = INVALID_SOCKET; }
    gNetStatus = NETSTAT_SINGLEPLAYER;
    gSyncsDone = 0; gTimeDiffMS = 0;
    gLastCoopCatchup = 0;
    memset(gLastRemoteDebugLog, 0, sizeof(gLastRemoteDebugLog));
    memset(gLastServerSentPed, 0, sizeof(gLastServerSentPed));
    memset(gLastServerSentPedTime, 0, sizeof(gLastServerSentPedTime));
    memset(&gLastClientSentPed, 0, sizeof(gLastClientSentPed));
    gLastClientSentPedTime = 0;
    memset(gLastMeleeAttemptTime, 0, sizeof(gLastMeleeAttemptTime));
    memset(gLastWeaponAttackTime, 0, sizeof(gLastWeaponAttackTime));
    gLastObservedLocalAmmo = 0;
    gLastObservedLocalWeapon = WEAPONTYPE_UNARMED;
    gLastObservedLocalWeaponState = WEAPONSTATE_READY;
    gLastObservedLocalInputBits = 0;
    gLastObservedLocalAirborne = false;
    gLastServerMsgTime = 0;  // reset so next session's timeout starts clean
    CloseChatWindow();
    ResetScores();
}

// ============================================================
//  HOST GAME
// ============================================================

static void StartGameAsServer()
{
    if (!NetInit()) {
        Chat_AddLine("Network init failed", 255, 0, 0);
        return;
    }
    SwitchToSinglePlayer(); // clean up first

    if (!NetOpenSocket((unsigned short)gConfigPort)) {
        Chat_AddLine("Failed to open server socket", 255, 0, 0);
        return;
    }
    gNetStatus = NETSTAT_SERVER;
    gLocalSlot = 0;
    gLastCoopCatchup = 0;
    gNetPlayers[0].Init();
    gNetPlayers[0].bActive = true;
    strncpy_s(gNetPlayers[0].Name, gLocalPlayerName, NET_MAX_NAME_SIZE - 1);
    gNetPlayers[0].R = gLocalR; gNetPlayers[0].G = gLocalG; gNetPlayers[0].B = gLocalB;
    gNetPlayers[0].pPed = FindPlayerPed();
    gNetPlayers[0].Team = FindTeamForNewPlayer(0);

    ResetScores();
    Server_InitGameObjects();

    char txt[128];
    sprintf_s(txt, "Hosting on port %d | Game: %d", gConfigPort, gGameType);
    Chat_AddLine(txt, 100, 255, 100);
}

// ============================================================
//  JOIN GAME
// ============================================================

static void StartGameAsClient()
{
    if (!NetInit()) {
        Chat_AddLine("Network init failed", 255, 0, 0);
        return;
    }
    SwitchToSinglePlayer();

    if (!NetOpenSocket(0)) { // port 0 = OS picks ephemeral port
        Chat_AddLine("Failed to open client socket", 255, 0, 0);
        return;
    }

    memset(&gServerAddr, 0, sizeof(gServerAddr));
    gServerAddr.sin_family = AF_INET;
    gServerAddr.sin_port = htons((unsigned short)gConfigPort);
    if (inet_pton(AF_INET, gConfigServerIP, &gServerAddr.sin_addr) != 1) {
        Chat_AddLine("Invalid server IP address", 255, 0, 0);
        closesocket(gSock); gSock = INVALID_SOCKET;
        gNetStatus = NETSTAT_SINGLEPLAYER;
        return;
    }

    gNetStatus = NETSTAT_CLIENTSTARTINGUP;
    gLocalSlot = 0;   // will be updated by server's MSG_SYNC reply
    gLastCoopCatchup = 0;
    gNetPlayers[0].Init();
    gNetPlayers[0].bActive = true;
    strncpy_s(gNetPlayers[0].Name, gLocalPlayerName, NET_MAX_NAME_SIZE - 1);
    gNetPlayers[0].R = gLocalR; gNetPlayers[0].G = gLocalG; gNetPlayers[0].B = gLocalB;
    gNetPlayers[0].pPed = nullptr;

    // Immediately send first MSG_SYNC so server knows we're here
    Client_SendSync();

    char txt[128];
    sprintf_s(txt, "Connecting to %s:%d ...", gConfigServerIP, gConfigPort);
    Chat_AddLine(txt, 200, 200, 100);
}

// ============================================================
//  NETWORK POLL (called every game frame)
// ============================================================

static void NetPoll()
{
    if (gNetStatus == NETSTAT_SINGLEPLAYER || gSock == INVALID_SOCKET) return;

    sockaddr_in fromAddr;
    int r;
    while ((r = NetRecv(fromAddr)) > 0) {
        if (gNetStatus == NETSTAT_SERVER) {
            Server_HandleMessage(gRecvBuf, r, fromAddr);
        }
        else {
            Client_HandleMessage(gRecvBuf, r);
        }
    }

    unsigned int now = CTimer::m_snTimeInMilliseconds;

    if (gNetStatus == NETSTAT_SERVER) {
        // Periodic: player names broadcast
        if (now / 1700 != CTimer::m_snPreviousTimeInMilliseconds / 1700)
            Server_BroadcastPlayerNames();

        // Ped sync every ~100ms
        CPlayerPed* localPed = FindPlayerPed();
        const unsigned int pedSyncInterval = GetDesiredPedSyncInterval(localPed);
        if (now - gLastPedSend >= pedSyncInterval) {
            // Update server-slot ped pointer in case player was loaded
            gNetPlayers[gLocalSlot].pPed = localPed;
            Server_BroadcastPeds();
            gLastPedSend = now;
        }

        // Game mode logic
        Server_UpdateGameModes();
        Server_UpdateCoop();

        // Timeout check
        Server_CheckTimeouts();

    }
    else {
        // Client: send sync if still starting up (~every 500ms)
        if (gNetStatus == NETSTAT_CLIENTSTARTINGUP && now - gLastSyncSend > 500) {
            Client_SendSync();
            gLastSyncSend = now;
        }
        // Client: send ped update every 100ms
        if (gNetStatus == NETSTAT_CLIENTRUNNING) {
            CPlayerPed* localPed = FindPlayerPed();
            const unsigned int pedSyncInterval = GetDesiredPedSyncInterval(localPed);
            if (now - gLastPedSend >= pedSyncInterval) {
                Client_SendPedUpdate();
                gLastPedSend = now;
            }
        }
        // Client timeout: use gLastServerMsgTime which is stamped on every
        // received packet, rather than the per-slot LastMsgTime field that is
        // only updated on join handshakes.
        if (gNetStatus == NETSTAT_CLIENTRUNNING &&
            gLastServerMsgTime > 0 &&
            now > gLastServerMsgTime + NET_TIMEOUT_MS)
        {
            Chat_AddLine("Connection to server lost", 255, 100, 100);
            SwitchToSinglePlayer();
        }
    }
}

// ============================================================
//  HUD: radar blips (ported from CNetGames::Update visual part)
// ============================================================

static void UpdateGameBlips()
{
    if (gNetStatus == NETSTAT_SINGLEPLAYER) return;

    unsigned int now = CTimer::m_snTimeInMilliseconds;

    // ---- Stash The Cash ----
    if (gGameType == GAMETYPE_STASHTHECASH) {
        if (gTargetBlip) { CRadar::ClearBlip(gTargetBlip); gTargetBlip = 0; }
        gTargetBlip = CRadar::SetCoordBlip(BLIP_COORD,
            CVector(gCashCoorsX, gCashCoorsY, gCashCoorsZ),
            0, BLIP_DISPLAY_BOTH, nullptr);
        CRadar::ChangeBlipScale(gTargetBlip, 3);

        for (int t = 0; t < NUM_TEAMS; t++) {
            int& blip = (t == 0) ? gBase0Blip : gBase1Blip;
            if (blip) { CRadar::ClearBlip(blip); blip = 0; }
            int sl = gCurrentStashLocs[t];
            if (sl < gNumStashLocations) {
                blip = CRadar::SetCoordBlip(BLIP_COORD,
                    CVector(gStashLocations[sl * 3], gStashLocations[sl * 3 + 1], gStashLocations[sl * 3 + 2]),
                    0, BLIP_DISPLAY_BOTH, nullptr);
                CRadar::ChangeBlipScale(blip, 5);
            }
        }
        // Corona at cash
        if (now & 512) {
            CCoronas::RegisterCorona(9, nullptr,
                255, 255, 255, 255,
                CVector(gCashCoorsX, gCashCoorsY, gCashCoorsZ),
                2.0f, 200.0f, CORONATYPE_HEADLIGHT, FLARETYPE_HEADLIGHTS,
                false, true, 0, 0.0f, false, 0.0f, 0, 1.0f, false, false);
        }
    }

    // ---- Capture The Flag ----
    if (gGameType == GAMETYPE_CAPTURETHEFLAG) {
        for (int t = 0; t < NUM_TEAMS; t++) {
            if (gFlagBlip[t]) { CRadar::ClearBlip(gFlagBlip[t]); gFlagBlip[t] = 0; }
            gFlagBlip[t] = CRadar::SetCoordBlip(BLIP_COORD,
                gFlagCoors[t], 0, BLIP_DISPLAY_BOTH, nullptr);
            CRadar::ChangeBlipScale(gFlagBlip[t], (now & 512) ? 3 : 4);
            if (now & 512) {
                CCoronas::RegisterCorona(9 + t, nullptr,
                    (unsigned char)kTeamColours[t * 3],
                    (unsigned char)kTeamColours[t * 3 + 1],
                    (unsigned char)kTeamColours[t * 3 + 2], 255,
                    gFlagCoors[t], 2.0f, 200.0f,
                    CORONATYPE_HEADLIGHT, FLARETYPE_HEADLIGHTS,
                    false, true, 0, 0.0f, false, 0.0f, 0, 1.0f, false, false);
            }
        }
    }

    // ---- Domination ----
    if (gGameType == GAMETYPE_DOMINATION) {
        for (int b = 0; b < DOMINATIONBASES; b++) {
            if (gDomBlip[b]) { CRadar::ClearBlip(gDomBlip[b]); gDomBlip[b] = 0; }
            int baseIdx = gDomBases[b];
            if (baseIdx >= gNumStashLocations) continue;
            CVector bPos(gStashLocations[baseIdx * 3],
                gStashLocations[baseIdx * 3 + 1],
                gStashLocations[baseIdx * 3 + 2]);
            gDomBlip[b] = CRadar::SetCoordBlip(BLIP_COORD, bPos, 0, BLIP_DISPLAY_BOTH, nullptr);
            CRadar::ChangeBlipScale(gDomBlip[b], (now & 1024) ? 3 : 4);
            if (now & 512) {
                int t = gDomTeam[b];
                unsigned char r = 255, g = 255, blue = 255;
                if (t >= 0) {
                    r = (unsigned char)kTeamColours[t * 3];
                    g = (unsigned char)kTeamColours[t * 3 + 1];
                    blue = (unsigned char)kTeamColours[t * 3 + 2];
                }
                CCoronas::RegisterCorona(13 + b, nullptr, r, g, blue, 255,
                    bPos, 2.0f, 200.0f, CORONATYPE_HEADLIGHT, FLARETYPE_HEADLIGHTS,
                    false, true, 0, 0.0f, false, 0.0f, 0, 1.0f, false, false);
            }
        }
    }
}

// ============================================================
//  HUD: player names in world-space (ported from RenderPlayerNames)
// ============================================================

static void DrawPlayerNames()
{
    if (gNetStatus == NETSTAT_SINGLEPLAYER) return;

    for (int i = 0; i < MAX_NETPLAYERS; i++) {
        if (i == gLocalSlot) continue;
        if (!gNetPlayers[i].bActive || !gNetPlayers[i].pPed) continue;

        CVector pos = gNetPlayers[i].pPed->GetPosition();
        pos.z += 1.2f;

        // Simple distance check instead of 3D projection
        CPlayerPed* localPed = FindPlayerPed();
        if (!localPed) continue;
        float dist = (localPed->GetPosition() - gNetPlayers[i].pPed->GetPosition()).Magnitude();
        if (dist > 30.0f) continue;

        // We can't do proper 3D→2D without CSprite::CalcScreenCoors easily,
        // so we just show names in the 2D HUD list instead.
        (void)pos;
    }
}

static void UpdateRemotePlayerPeds()
{
    if (gNetStatus == NETSTAT_SINGLEPLAYER)
        return;

    for (int i = 0; i < MAX_NETPLAYERS; i++) {
        if (i == gLocalSlot || !gNetPlayers[i].bActive || !gNetPlayers[i].pPed || !gNetPlayers[i].bHasPedSync || !gNetPlayers[i].bVisualsReady)
            continue;
        if (CTimer::m_snTimeInMilliseconds - gNetPlayers[i].LastEventTime < 500) {
        if (CTimer::m_snTimeInMilliseconds - gLastRemoteDebugLog[i] >= 1000) {
            gLastRemoteDebugLog[i] = CTimer::m_snTimeInMilliseconds;
            LogVerbose("Remote slot=%d waiting for settle window age=%u", i,
                CTimer::m_snTimeInMilliseconds - gNetPlayers[i].LastEventTime);
        }
        continue;
    }
    UpdateRemotePlayerVisual(gNetPlayers[i].pPed, gNetPlayers[i]);
    if (CTimer::m_snTimeInMilliseconds - gLastRemoteDebugLog[i] >= 2000) {
        gLastRemoteDebugLog[i] = CTimer::m_snTimeInMilliseconds;
        LogVerbose("Remote slot=%d model=%u weap=%u ammo=%u move=%u pos=(%.2f,%.2f,%.2f) speed=(%.3f,%.3f,%.3f) dead=%d",
            i,
            gNetPlayers[i].ModelIndex,
            gNetPlayers[i].CurrentWeapon,
            gNetPlayers[i].WeaponAmmo,
                gNetPlayers[i].MoveState,
                gNetPlayers[i].TargetPos.x, gNetPlayers[i].TargetPos.y, gNetPlayers[i].TargetPos.z,
                gNetPlayers[i].TargetMoveSpeed.x, gNetPlayers[i].TargetMoveSpeed.y, gNetPlayers[i].TargetMoveSpeed.z,
                gNetPlayers[i].bReportedDead ? 1 : 0);
        }
    }
}

// ============================================================
//  HUD: scoreboard + chat (ported from PrintNetworkMessagesToScreen)
// ============================================================

static void DrawNetworkHUD()
{
    if (gNetStatus == NETSTAT_SINGLEPLAYER) return;
    if (!CHud::m_Wants_To_Draw_Hud) return;

    float sw = (float)RsGlobal.maximumWidth;
    // ---- Mode/status label ----
    {
        const char* modeNames[] = { "DM", "DM-NB", "TDM", "TDM-NB", "Cash", "CTF", "Race", "Dom" };
        const char* statusStr = (gNetStatus == NETSTAT_SERVER) ? "SRV" :
            (gNetStatus == NETSTAT_CLIENTRUNNING) ? "CLI" : "...";
        char buf[64];
        if (gCoopMode)
            sprintf_s(buf, "[%s] COOP", statusStr);
        else
            sprintf_s(buf, "[%s] %s", statusStr, modeNames[gGameType & 7]);
        FontPrint(sw - 160.0f, 10.0f, buf, 255, 255, 255, 255, 0.5f, 0.5f);
    }

    // ---- Scoreboard (sorted by points) ----
    {
        bool printed[MAX_NETPLAYERS] = {};
        int  numPrinted = 0;
        while (true) {
            int best = -1, bestPts = -9999999;
            for (int i = 0; i < MAX_NETPLAYERS; i++) {
                if (!gNetPlayers[i].bActive || printed[i]) continue;
                if (gNetPlayers[i].Points > bestPts) { bestPts = gNetPlayers[i].Points; best = i; }
            }
            if (best < 0) break;
            printed[best] = true;
            char buf[128];
            if (gGameType == GAMETYPE_RATRACE)
                sprintf_s(buf, "%s: %d (%d)", gNetPlayers[best].Name, gNetPlayers[best].Points, gNetPlayers[best].Points2);
            else
                sprintf_s(buf, "%s: %d", gNetPlayers[best].Name, gNetPlayers[best].Points);
            int R, G, B; FindPlayerMarkerColour(best, &R, &G, &B);
            unsigned char alpha = (best == gLocalSlot) ? 255 : 200;
            FontPrint(sw * 0.78f, 40.0f + numPrinted * 14.0f, buf,
                (unsigned char)R, (unsigned char)G, (unsigned char)B, alpha);
            numPrinted++;
        }
    }

    // ---- Team scores ----
    if (TeamGameGoingOn()) {
        for (int t = 0; t < NUM_TEAMS; t++) {
            char buf[64];
            if (gGameType == GAMETYPE_DOMINATION)
                sprintf_s(buf, "Team%d: %d", t, gTeamPoints[t] / 1000);
            else
                sprintf_s(buf, "Team%d: %d", t, gTeamPoints[t]);
            FontPrint(sw - 160.0f, 110.0f + t * 20.0f, buf,
                (unsigned char)kTeamColours[t * 3],
                (unsigned char)kTeamColours[t * 3 + 1],
                (unsigned char)kTeamColours[t * 3 + 2]);
        }
    }

}

// ============================================================
//  CHAT: keyboard processing
// ============================================================

static void ProcessChat()
{
    gChatTyping = gChatWindowOpen;
}

// ============================================================
//  F-KEY CONTROLS
// ============================================================

static void ProcessHotkeys()
{
    CKeyboardState& nk = CPad::NewKeyState;
    CKeyboardState& ok = CPad::OldKeyState;

    // F1 = Host game
    if (nk.FKeys[0] && !ok.FKeys[0]) {
        StartGameAsServer();
    }
    // F2 = Join game
    if (nk.FKeys[1] && !ok.FKeys[1]) {
        StartGameAsClient();
    }
    // F3 = Open/close chat
    if (nk.FKeys[2] && !ok.FKeys[2]) {
        if (gChatWindowOpen)
            CloseChatWindow();
        else
            OpenChatWindow();
        return;
    }
    if (gChatTyping) return; // don't process gameplay hotkeys while typing

    // F4 = Disconnect
    if (nk.FKeys[3] && !ok.FKeys[3]) {
        if (gNetStatus != NETSTAT_SINGLEPLAYER) {
            if (gNetStatus != NETSTAT_SERVER) {
                CMsgPlayerQuit q; q.Message = MSG_PLAYERQUIT; q.PlayerSlot = gLocalSlot;
                NetSendToServer(&q, sizeof(q));
            }
            SwitchToSinglePlayer();
            Chat_AddLine("Disconnected", 200, 200, 200);
        }
    }
    // F5 = Deathmatch
    if (nk.FKeys[4] && !ok.FKeys[4]) { gGameType = GAMETYPE_DEATHMATCH; Chat_AddLine("Mode: Deathmatch"); }
    // F6 = Team DM
    if (nk.FKeys[5] && !ok.FKeys[5]) { gGameType = GAMETYPE_TEAMDEATHMATCH; Chat_AddLine("Mode: Team DM"); }
    // F7 = CTF
    if (nk.FKeys[6] && !ok.FKeys[6]) { gGameType = GAMETYPE_CAPTURETHEFLAG; Chat_AddLine("Mode: CTF"); }
    // F8 = Domination
    if (nk.FKeys[7] && !ok.FKeys[7]) { gGameType = GAMETYPE_DOMINATION; Chat_AddLine("Mode: Domination"); }
    // F9 = Stash The Cash
    if (nk.FKeys[8] && !ok.FKeys[8]) { gGameType = GAMETYPE_STASHTHECASH; Chat_AddLine("Mode: Stash The Cash"); }
    // F10 = Rat Race
    if (nk.FKeys[9] && !ok.FKeys[9]) { gGameType = GAMETYPE_RATRACE; Chat_AddLine("Mode: Rat Race"); }
}

// ============================================================
//  MAIN UPDATE (called every game frame via Events::gameProcessEvent)
// ============================================================

static void NetUpdate()
{
    ProcessHotkeys();
    ProcessChat();
    ProcessLocalAnimEvents();
    NetPoll();
    ProcessLocalCoopCombat();
    UpdateRemotePlayerPeds();
    UpdateGameBlips();
    DrawPlayerNames();
}

// ============================================================
//  MAIN DRAW (called via Events::drawingEvent or similar)
// ============================================================

static void NetDraw()
{
    DrawNetworkHUD();
    DrawImGui();
}

static void DrawImGui()
{
    if (!gChatWindowOpen)
        return;
    if (!BeginImGuiFrame()) {
        CloseChatWindow();
        return;
    }

    DrawChatWindow();
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

// ============================================================
//  PLUGIN ENTRY POINT
// ============================================================

struct PS2MultiplayerMod {
    PS2MultiplayerMod()
    {
        InitPluginPaths();

        // Initialise all player slots
        for (int i = 0; i < MAX_NETPLAYERS; i++)
            gNetPlayers[i].Init();

        // Load configuration
        LoadConfig();
        SaveConfig();
        LogMessage("PS2MultiRev initialized | PluginDir=%s | Ini=%s | Log=%s",
            gPluginDir, gPluginIniPath, gPluginLogPath);

        // Hook into game events
        Events::gameProcessEvent += [] { NetUpdate(); };
        Events::drawingEvent += [] { NetDraw(); };

        // Save config on exit (best-effort)
        Events::shutdownRwEvent += [] { ShutdownImGui(); SaveConfig(); SwitchToSinglePlayer(); };
    }
} gPS2MultiplayerMod;
