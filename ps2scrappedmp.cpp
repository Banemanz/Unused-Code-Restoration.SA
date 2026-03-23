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
 *   F3        - Disconnect / return to single-player
 *   F5-F10    - Select game mode (DM / TDM / CTF / Domination / Cash / RatRace)
 *   T         - Begin typing a chat message
 *   Enter     - Send chat message (while typing)
 *   Escape    - Cancel chat message
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

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

using namespace plugin;

// ============================================================
//  FORWARD DECLARATIONS
// ============================================================

static void NetUpdate();
static void NetDraw();

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

// Sync interval (ms) – send ped update every this many ms
#define PED_SYNC_INTERVAL    100
// Timeout (ms)
#define NET_TIMEOUT_MS       6000

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
    float    Heading;
    float    Health;
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

#pragma pack(pop)

// ============================================================
//  NET-PLAYER SLOT  (parallel player info - plugin-sdk's
//  CPlayerInfo doesn't carry Name/bInUse/Team/Points/etc.)
// ============================================================

struct NetPlayer {
    bool          bActive;
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

    void Init() {
        bActive = false;
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

// Tracks the last time any UDP packet arrived from the server (client only).
// Kept separate from LastMsgTime (which is per-slot) so we can detect server
// silence even if the local slot's data was never updated.
static unsigned int    gLastServerMsgTime = 0;

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

static void LoadConfig()
{
    char iniPath[MAX_PATH];
    GetModuleFileNameA(nullptr, iniPath, MAX_PATH);
    // Trim to the directory and append the INI filename with bounds checking
    char* lastSlash = strrchr(iniPath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    static const char kIniName[] = "MultiplayerMod.ini";
    if (strlen(iniPath) + strlen(kIniName) < MAX_PATH)
        strcat_s(iniPath, MAX_PATH, kIniName);
    else
        return; // path too long, skip config

    GetPrivateProfileStringA("Network", "ServerIP", "127.0.0.1",
        gConfigServerIP, sizeof(gConfigServerIP), iniPath);
    gConfigPort = GetPrivateProfileIntA("Network", "Port", NET_DEFAULT_PORT, iniPath);

    GetPrivateProfileStringA("Player", "Name", "Player",
        gLocalPlayerName, sizeof(gLocalPlayerName), iniPath);
    gLocalR = (unsigned char)GetPrivateProfileIntA("Player", "R", 255, iniPath);
    gLocalG = (unsigned char)GetPrivateProfileIntA("Player", "G", 128, iniPath);
    gLocalB = (unsigned char)GetPrivateProfileIntA("Player", "B", 0, iniPath);
}

static void SaveConfig()
{
    char iniPath[MAX_PATH];
    GetModuleFileNameA(nullptr, iniPath, MAX_PATH);
    char* lastSlash = strrchr(iniPath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    static const char kIniName[] = "MultiplayerMod.ini";
    if (strlen(iniPath) + strlen(kIniName) >= MAX_PATH) return;
    strcat_s(iniPath, MAX_PATH, kIniName);

    WritePrivateProfileStringA("Network", "ServerIP", gConfigServerIP, iniPath);
    char buf[32];
    sprintf_s(buf, "%d", gConfigPort);
    WritePrivateProfileStringA("Network", "Port", buf, iniPath);
    WritePrivateProfileStringA("Player", "Name", gLocalPlayerName, iniPath);
    sprintf_s(buf, "%d", gLocalR); WritePrivateProfileStringA("Player", "R", buf, iniPath);
    sprintf_s(buf, "%d", gLocalG); WritePrivateProfileStringA("Player", "G", buf, iniPath);
    sprintf_s(buf, "%d", gLocalB); WritePrivateProfileStringA("Player", "B", buf, iniPath);
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
static unsigned int gChatInputMs = 0;

static void Chat_AddLine(const char* text, unsigned char R = 200, unsigned char G = 200, unsigned char B = 200)
{
    ChatLine& cl = gChatHistory[gChatHead % CHAT_MAX_LINES];
    strncpy_s(cl.text, text, CHAT_STRING_LEN - 1);
    cl.timestamp = CTimer::m_snTimeInMilliseconds;
    cl.R = R; cl.G = G; cl.B = B;
    gChatHead++;
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
    // CTF: spawn near own base
    if (gGameType == GAMETYPE_CAPTURETHEFLAG && team >= 0 && gNumCTFBases > 0) {
        int b = gCurrentStashLocs[team] < gNumCTFBases ? gCurrentStashLocs[team] : 0;
        return CVector(
            gCTFBases[b * 3] + (float)((int)(CGeneral::GetRandomNumber() & 7) - 3),
            gCTFBases[b * 3 + 1] + (float)((int)(CGeneral::GetRandomNumber() & 7) - 3),
            gCTFBases[b * 3 + 2] + 1.0f);
    }

    if (gNumStartPoints == 0) {
        // Fallback: original PS2 hard-coded start
        return CVector(1034.9f, -940.0f, 15.0f);
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
    return CVector(gNetStartCoors[bestIdx][0], gNetStartCoors[bestIdx][1], gNetStartCoors[bestIdx][2]);
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
    CStreaming::RequestModel(1, 2);
    CStreaming::LoadAllRequestedModels(false);

    // Use civilian ped model 1 (male civilian - not CJ, CJ is model 0).
    CPed* pPed = new CPed(PED_TYPE_CIVMALE);
    pPed->SetModelIndex(MODEL_MALE01);
    pPed->SetCharCreatedBy(2); // 2 = PED_MISSION (keeps SA from auto-deleting it)
    pPed->SetPosn(pos); // Safely sets placement matrix
    CWorld::Add(pPed);
    gNetPlayers[slot].pPed = pPed;
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
        gLocalSlot = msg->PlayerNumberOfClient;
        gNetPlayers[gLocalSlot].bActive = true;
        strncpy_s(gNetPlayers[gLocalSlot].Name, gLocalPlayerName, NET_MAX_NAME_SIZE - 1);
        gNetPlayers[gLocalSlot].R = gLocalR;
        gNetPlayers[gLocalSlot].G = gLocalG;
        gNetPlayers[gLocalSlot].B = gLocalB;

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
        if (gNetPlayers[slot].pPed) {
            // Use Teleport() so collision and world-sector registration stay valid, safely updates matrix
            gNetPlayers[slot].pPed->Teleport(CVector(msg->PosX, msg->PosY, msg->PosZ), false);
            gNetPlayers[slot].pPed->m_fCurrentRotation = msg->Heading;
            gNetPlayers[slot].pPed->m_fHealth = msg->Health;
        }
        else {
            CreateRemotePlayerPed(slot, CVector(msg->PosX, msg->PosY, msg->PosZ));
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
                localPed->Teleport(CVector(msg->PosX, msg->PosY, msg->PosZ), true);
                localPed->m_fHealth = 100.0f;
            }
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
        slot = AllocClientSlot(fromAddr);
        if (slot < 0) return; // server full
    }

    gNetPlayers[slot].LastMsgTime = CTimer::m_snTimeInMilliseconds;

    switch (hdr->Message) {
    case MSG_SYNC:
    {
        const CMsgSync* msg = (const CMsgSync*)buf;
        strncpy_s(gNetPlayers[slot].Name, msg->PlayerName, NET_MAX_NAME_SIZE - 1);
        gNetPlayers[slot].R = msg->R;
        gNetPlayers[slot].G = msg->G;
        gNetPlayers[slot].B = msg->B;
        gNetPlayers[slot].Points = 0;
        gNetPlayers[slot].Points2 = 0;
        gNetPlayers[slot].Team = FindTeamForNewPlayer(slot);

        // Spawn ped for this new player if needed
        if (!gNetPlayers[slot].pPed) {
            CVector spawnPos = SelectSafeStartPoint(gNetPlayers[slot].Team);
            CreateRemotePlayerPed(slot, spawnPos);
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

        char txt[128];
        sprintf_s(txt, "%s joined the game", gNetPlayers[slot].Name);
        BroadcastText(txt, 200, 200, 200);
        break;
    }
    case MSG_UPDATEPED:
    {
        const CMsgUpdatePed* msg = (const CMsgUpdatePed*)buf;
        if (gNetPlayers[slot].pPed) {
            gNetPlayers[slot].pPed->GetPosition() = CVector(msg->PosX, msg->PosY, msg->PosZ);
            gNetPlayers[slot].pPed->m_fCurrentRotation = msg->Heading;
            gNetPlayers[slot].pPed->m_fHealth = msg->Health;
        }
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
    for (int s = 0; s < MAX_NETPLAYERS; s++) {
        if (!gNetPlayers[s].bActive || !gNetPlayers[s].pPed) continue;
        CMsgUpdatePed msg;
        msg.Message = MSG_UPDATEPED;
        msg.PlayerSlot = s;
        msg.PosX = gNetPlayers[s].pPed->GetPosition().x;
        msg.PosY = gNetPlayers[s].pPed->GetPosition().y;
        msg.PosZ = gNetPlayers[s].pPed->GetPosition().z;
        msg.Heading = gNetPlayers[s].pPed->m_fCurrentRotation;
        msg.Health = gNetPlayers[s].pPed->m_fHealth;
        msg.PedStateFlags = 0;
        if (gNetPlayers[s].pPed->bInVehicle) msg.PedStateFlags |= 1;
        if (gNetPlayers[s].pPed->m_ePedState == PEDSTATE_DEAD) msg.PedStateFlags |= 2;
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
    msg.Message = MSG_UPDATEPED;
    msg.PlayerSlot = gLocalSlot;
    msg.PosX = pPed->GetPosition().x;
    msg.PosY = pPed->GetPosition().y;
    msg.PosZ = pPed->GetPosition().z;
    msg.Heading = pPed->m_fCurrentRotation;
    msg.Health = pPed->m_fHealth;
    msg.PedStateFlags = (pPed->bInVehicle ? 1 : 0) |
        (pPed->m_ePedState == PEDSTATE_DEAD ? 2 : 0);
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
    gLastServerMsgTime = 0;  // reset so next session's timeout starts clean
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
    gNetPlayers[0].Init();
    gNetPlayers[0].bActive = true;
    strncpy_s(gNetPlayers[0].Name, gLocalPlayerName, NET_MAX_NAME_SIZE - 1);
    gNetPlayers[0].R = gLocalR; gNetPlayers[0].G = gLocalG; gNetPlayers[0].B = gLocalB;
    gNetPlayers[0].pPed = FindPlayerPed();

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
        if (now - gLastPedSend >= PED_SYNC_INTERVAL) {
            // Update server-slot ped pointer in case player was loaded
            gNetPlayers[gLocalSlot].pPed = FindPlayerPed();
            Server_BroadcastPeds();
            gLastPedSend = now;
        }

        // Game mode logic
        Server_UpdateGameModes();

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
        if (gNetStatus == NETSTAT_CLIENTRUNNING && now - gLastPedSend >= PED_SYNC_INTERVAL) {
            Client_SendPedUpdate();
            gLastPedSend = now;
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

// ============================================================
//  HUD: scoreboard + chat (ported from PrintNetworkMessagesToScreen)
// ============================================================

static void DrawNetworkHUD()
{
    if (gNetStatus == NETSTAT_SINGLEPLAYER) return;
    if (!CHud::m_Wants_To_Draw_Hud) return;

    float sw = (float)RsGlobal.maximumWidth;
    float sh = (float)RsGlobal.maximumHeight;

    // ---- Mode/status label ----
    {
        const char* modeNames[] = { "DM", "DM-NB", "TDM", "TDM-NB", "Cash", "CTF", "Race", "Dom" };
        const char* statusStr = (gNetStatus == NETSTAT_SERVER) ? "SRV" :
            (gNetStatus == NETSTAT_CLIENTRUNNING) ? "CLI" : "...";
        char buf[64];
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

    // ---- Chat history ----
    unsigned int now = CTimer::m_snTimeInMilliseconds;
    int numShown = 0;
    for (int i = CHAT_MAX_LINES - 1; i >= 0 && numShown < 6; i--) {
        int idx = (gChatHead - 1 - i + CHAT_MAX_LINES * 2) % CHAT_MAX_LINES;
        ChatLine& cl = gChatHistory[idx];
        if (cl.text[0] == '\0') continue;
        if (now - cl.timestamp > CHAT_DISPLAY_MS) continue;
        float y = sh - 80.0f - numShown * 14.0f;
        unsigned char alpha = 255;
        if (now - cl.timestamp > (CHAT_DISPLAY_MS - 1500))
            alpha = (unsigned char)(255 * (CHAT_DISPLAY_MS - (now - cl.timestamp)) / 1500);
        FontPrint(8.0f, y, cl.text, cl.R, cl.G, cl.B, alpha);
        numShown++;
    }

    // ---- Chat input box ----
    if (gChatTyping) {
        char buf[CHAT_STRING_LEN + 4];
        strcpy_s(buf, gChatInput);
        if (now & 512) strcat_s(buf, "_");
        FontPrint(8.0f, sh - 90.0f, buf, 255, 255, 255, 255);
    }
}

// ============================================================
//  CHAT: keyboard processing
// ============================================================

static void ProcessChat()
{
    if (gNetStatus == NETSTAT_SINGLEPLAYER) return;

    CKeyboardState& newKeys = CPad::NewKeyState;
    CKeyboardState& oldKeys = CPad::OldKeyState;

    if (!gChatTyping) {
        // 'T' to start typing
        if (newKeys.standardKeys['T'] && !oldKeys.standardKeys['T']) {
            gChatTyping = true;
            gChatInput[0] = '\0';
        }
        return;
    }

    // Escape cancels
    if (newKeys.esc && !oldKeys.esc) {
        gChatTyping = false;
        gChatInput[0] = '\0';
        return;
    }

    // Enter sends
    if ((newKeys.enter && !oldKeys.enter) || (newKeys.extenter && !oldKeys.extenter)) {
        if (gChatInput[0] != '\0') {
            char full[CHAT_STRING_LEN];
            sprintf_s(full, "%s: %s", gLocalPlayerName, gChatInput);
            int R, G, B;
            FindPlayerMarkerColour(gLocalSlot, &R, &G, &B);
            CMsgText msg;
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
        gChatTyping = false;
        gChatInput[0] = '\0';
        return;
    }

    // Backspace
    if (newKeys.back && !oldKeys.back) {
        int len = (int)strlen(gChatInput);
        if (len > 0) gChatInput[len - 1] = '\0';
        return;
    }

    // Printable ASCII characters (space=0x20 through tilde=0x7E)
    static const int kFirstPrintable = 0x20;
    static const int kLastPrintable = 0x7F;
    for (int k = kFirstPrintable; k < kLastPrintable; k++) {
        if (newKeys.standardKeys[k] && !oldKeys.standardKeys[k]) {
            int len = (int)strlen(gChatInput);
            if (len < CHAT_STRING_LEN - 2) {
                gChatInput[len] = (char)k;
                gChatInput[len + 1] = '\0';
            }
        }
    }
}

// ============================================================
//  F-KEY CONTROLS
// ============================================================

static void ProcessHotkeys()
{
    CKeyboardState& nk = CPad::NewKeyState;
    CKeyboardState& ok = CPad::OldKeyState;

    if (gChatTyping) return; // don't process hotkeys while typing

    // F1 = Host game
    if (nk.FKeys[0] && !ok.FKeys[0]) {
        StartGameAsServer();
    }
    // F2 = Join game
    if (nk.FKeys[1] && !ok.FKeys[1]) {
        StartGameAsClient();
    }
    // F3 = Disconnect
    if (nk.FKeys[2] && !ok.FKeys[2]) {
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
    NetPoll();
    UpdateGameBlips();
    DrawPlayerNames();
}

// ============================================================
//  MAIN DRAW (called via Events::drawingEvent or similar)
// ============================================================

static void NetDraw()
{
    DrawNetworkHUD();
}

// ============================================================
//  PLUGIN ENTRY POINT
// ============================================================

struct PS2MultiplayerMod {
    PS2MultiplayerMod()
    {
        // Initialise all player slots
        for (int i = 0; i < MAX_NETPLAYERS; i++)
            gNetPlayers[i].Init();

        // Load configuration
        LoadConfig();

        // Hook into game events
        Events::gameProcessEvent += [] { NetUpdate(); };
        Events::drawingEvent += [] { NetDraw(); };

        // Save config on exit (best-effort)
        Events::shutdownRwEvent += [] { SaveConfig(); SwitchToSinglePlayer(); };
    }
} gPS2MultiplayerMod;
