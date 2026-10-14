// FO2Telemetry - streams FlatOut 2 (v1.2, 5,369,344 byte FlatOut2.exe) car telemetry to SimHub,
// drives a force feedback wheel and feeds wheel + separate pedals into the game.
//
// SimHub's "External sims" support reads a fixed binary UDP packet described by a definition file
// (FO2Telemetry.simdef, next to this ASI). At startup the ASI registers that definition by
// writing %LOCALAPPDATA%\SimHub\ExternalSims\Registrations\{id}.simlink, so FlatOut 2 shows up
// in SimHub as a normal game: dashboards, rev lights, ShakeIt and motion all use the standard
// fields below. Same packet layout as NFSU2Telemetry.
//
// A background thread polls game memory and sends one packet per new physics sample, plus a
// heartbeat while nothing changes (menus, pause).
//
// Settings: FO2Telemetry.ini next to the ASI
//   [SIMHUB] Enabled=1  Host=127.0.0.1  Port=30777  MaxRpm=0 (0 = highest rpm seen in this car)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#include "ffb.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

#ifndef _MSC_VER
// MinGW build (compile check): no SEH, guarded reads become plain reads.
#undef __try
#undef __except
#define __try if (true)
#define __except(x) else
#endif

// ---------------------------------------------------------------- packet (from SimHub's generator)
// Must match FO2Telemetry.simdef. Regenerate both together if fields change.
// SimHub derives both signatures from the definition: the game signature is the CRC-32 of the
// definition's UniqueId; the telemetry signature (layout) is read from the .simdef or the ini.
static const char* kDefinitionId = "464c4f32-0000-4d0d-9f1e-7e1e00000002";
static uint32_t g_gameSignature, g_telemetrySignature;

#pragma pack(push, 1)
struct Packet
{
    // header
    uint32_t GameSignature, TelemetrySignature;
    uint16_t LayoutMajorVersion, LayoutMinorVersion;
    uint64_t EmitterInstanceId;
    uint8_t PacketId;
    uint64_t PacketsCounter;
    uint8_t IsSessionRunning, IsSessionPaused;
    uint64_t SessionId;
    uint8_t IsReplay, IsUserInControl, IsAIInControl, IsSpectator;
    double SessionTimeSeconds;
    uint32_t PhysicsDiscontinuityCounter;
    // fields
    float SpeedKmh, EngineRpm, EngineMaxRpm;
    char Gear[8];
    int32_t RacePosition;
    uint32_t CompletedLaps;
    int32_t TotalLaps;
    double CurrentLapTime, LastLapTimeSeconds, BestLapTimeSeconds;
    float YawDegrees, PitchDegrees, RollDegrees;
    float YawRateDegreesPerSecond, PitchRateDegreesPerSecond, RollRateDegreesPerSecond;
    float LocalSurgeMs2, LocalSwayMs2, LocalHeaveMs2;
    float LocalVelocityForwardMps, LocalVelocityLateralMps, LocalVelocityUpMps;
    double VehiclePositionNorth, VehiclePositionEast, VehiclePositionUp;
    int32_t OpponentCount;
    // layout 1.1 (wheel arrays are FL, FR, RL, RR)
    float Throttle, Brake, Handbrake;
    float WheelSlip[4];
    float WheelLock[4];
    float SuspensionVelocityMps[4];
    uint8_t PushToPassActive;
    float NitroBoost;
};
#pragma pack(pop)
static_assert(sizeof(Packet) == 252, "packet layout must match FO2Telemetry.simdef");

// ---------------------------------------------------------------- settings / log
static char g_dir[MAX_PATH], g_iniPath[MAX_PATH], g_logPath[MAX_PATH];

void Log(const char* fmt, ...)
{
    FILE* f = fopen(g_logPath, "a");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------- game data (FlatOut2.exe v1.2)
//   pGameFlow *(0x8E8410): +0x458 game state (2 = race), +0x91C race state (8 = racing),
//     +0x938 replay, +0x9B8 -> player host (+0x14 vector<Player*>, player 0 = local player)
//   pause menu: byte at 0x8DBE30
//   Player: +0x33C -> Car, +0x368 player id (1-based), +0x3B0 race time (ms), +0x488 current lap
//   Car: +0x1B0 body matrix (rows right, up, forward, position; world y up, left-handed),
//     +0x280 velocity (m/s, world), +0x5CC nitro left, +0x5D0 nitro capacity, +0x5D8 engine rpm,
//     +0x634 gear (signed byte, -1 = R, 0 = N), +0x1890 suspension[4] (0x40 each, +0x18 travel),
//     +0x1DF4 throttle, +0x1DF8 brake, +0x1DFC nitro button, +0x1E00 handbrake (0-1)
//   pScoreManager *(0x8DA5D0): +0x04 laps, +0x08 vector<PlayerScoreRace*>
//     (entry +0x04 player id 0-based, +0x08 position)
// Layouts partly from gaycoderprincess's nya-common-fo2 headers (MPL-2.0). The rpm, gear and
// suspension travel offsets were found in the game code and are the least certain.
struct Sample
{
    bool inRace = false, car = false, race = false, paused = false, replay = false;
    float pos[3], vel[3], fwd[3], right[3], up[3];
    float rpm = 0;
    int gear = 0;
    float throttle = 0, brake = 0, handbrake = 0, nitroButton = 0, nitro = 0, nitroMax = 0;
    float susp[4] = {};
    int racers = 0, position = 0, laps = 0, lap = 0;
    int clock = 0;
};

static bool Unit(const float* v) { return fabsf(sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) - 1) < 0.05f; }

static BYTE* LocalPlayer()
{
    BYTE* gf = *(BYTE**)0x8E8410;
    if (!gf) return nullptr;
    BYTE* host = *(BYTE**)(gf + 0x9B8);
    if (!host) return nullptr;
    BYTE** b = *(BYTE***)(host + 0x14);
    BYTE** e = *(BYTE***)(host + 0x18);
    return (b && e > b) ? b[0] : nullptr;
}

static void ReadGame(Sample& s)
{
    s = Sample();
    __try
    {
        BYTE* gf = *(BYTE**)0x8E8410;
        if (!gf) return;
        s.inRace = *(int*)(gf + 0x458) == 2;
        s.replay = *(int*)(gf + 0x938) != 0;
        s.paused = *(bool*)0x8DBE30;
        BYTE* pl = s.inRace ? LocalPlayer() : nullptr;
        BYTE* car = pl ? *(BYTE**)(pl + 0x33C) : nullptr;
        if (car)
        {
            const float* m = (const float*)(car + 0x1B0);
            for (int i = 0; i < 3; i++) { s.right[i] = m[i]; s.up[i] = m[4 + i]; s.fwd[i] = m[8 + i]; s.pos[i] = m[12 + i]; }
            memcpy(s.vel, car + 0x280, 12);
            s.rpm = *(float*)(car + 0x5D8);
            if (!(s.rpm >= 0 && s.rpm < 20000)) s.rpm = 0;
            s.gear = *(signed char*)(car + 0x634);
            if (s.gear < -1 || s.gear > 8) s.gear = 0;
            bool posOk = fabsf(s.pos[0]) < 1e5f && fabsf(s.pos[1]) < 1e5f && fabsf(s.pos[2]) < 1e5f;
            bool velOk = fabsf(s.vel[0]) < 300 && fabsf(s.vel[1]) < 300 && fabsf(s.vel[2]) < 300;
            s.car = posOk && velOk && Unit(s.fwd) && Unit(s.right) && Unit(s.up);
            for (int i = 0; i < 4; i++)
            {
                s.susp[i] = *(float*)(car + 0x1890 + 0x40 * i + 0x18);
                if (!(fabsf(s.susp[i]) < 2)) s.susp[i] = 0;
            }
            auto clamp01 = [](float v) { return v >= 0 && v <= 1 ? v : 0.0f; };
            s.throttle = clamp01(*(float*)(car + 0x1DF4));
            s.brake = clamp01(*(float*)(car + 0x1DF8));
            s.nitroButton = clamp01(*(float*)(car + 0x1DFC));
            s.handbrake = clamp01(*(float*)(car + 0x1E00));
            s.nitro = *(float*)(car + 0x5CC);
            s.nitroMax = *(float*)(car + 0x5D0);
            if (!(s.nitroMax > 0 && s.nitroMax < 1e4f) || !(s.nitro >= 0 && s.nitro <= s.nitroMax * 1.01f)) s.nitro = s.nitroMax = 0;

            s.clock = *(int*)(pl + 0x3B0);
            s.lap = *(int*)(pl + 0x488);
            int id = *(int*)(pl + 0x368);
            BYTE* sm = *(BYTE**)0x8DA5D0;
            if (sm)
            {
                s.laps = *(int*)(sm + 4);
                BYTE** b = *(BYTE***)(sm + 8);
                BYTE** e = *(BYTE***)(sm + 0xC);
                int n = (b && e > b && e - b <= 16) ? (int)(e - b) : 0;
                int minPos = 99, myPos = -1;
                for (int i = 0; i < n; i++)
                {
                    if (!b[i]) continue;
                    int p = *(int*)(b[i] + 8);
                    if (p < minPos) minPos = p;
                    if (*(int*)(b[i] + 4) + 1 == id) myPos = p;
                }
                s.racers = n;
                s.position = myPos + (minPos == 0 ? 1 : 0);
                s.race = s.racers >= 1 && s.position >= 1 && s.position <= s.racers;
                if (s.laps < 1 || s.laps > 127) s.laps = 0;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        s.car = s.race = false;
    }
}

// ---------------------------------------------------------------- sender
static float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static const float kDeg = 57.2957795f;

static uint64_t Random64()
{
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    uint64_t x = (uint64_t)c.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^ GetTickCount();
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return x ? x : 1;
}

static void RegisterDefinition()
{
    char base[MAX_PATH], path[MAX_PATH], def[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH)) return;
    sprintf(path, "%s\\SimHub", base); CreateDirectoryA(path, nullptr);
    sprintf(path, "%s\\SimHub\\ExternalSims", base); CreateDirectoryA(path, nullptr);
    sprintf(path, "%s\\SimHub\\ExternalSims\\Registrations", base); CreateDirectoryA(path, nullptr);
    sprintf(path, "%s\\SimHub\\ExternalSims\\Registrations\\%s.simlink", base, kDefinitionId);
    sprintf(def, "%s\\FO2Telemetry.simdef", g_dir);
    if (GetFileAttributesA(def) == INVALID_FILE_ATTRIBUTES)
    {
        Log("FO2Telemetry.simdef not found next to the ASI - SimHub registration skipped");
        return;
    }
    FILE* f = fopen(path, "w");
    if (!f) { Log("could not write %s", path); return; }
    fprintf(f, "%s\n", def);
    fclose(f);
    Log("registered with SimHub: %s", path);
}

static uint32_t Crc32(const char* str)
{
    uint32_t c = 0xFFFFFFFFu;
    for (const unsigned char* p = (const unsigned char*)str; *p; p++)
    {
        c ^= *p;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

// Telemetry signature: [SIMHUB] TelemetrySignature in the ini (hex) wins, otherwise the value in
// FO2Telemetry.simdef. SimHub shows the expected value in its External sims editor.
static void LoadSignatures()
{
    g_gameSignature = Crc32(kDefinitionId);
    char buf[32];
    GetPrivateProfileStringA("SIMHUB", "TelemetrySignature", "", buf, sizeof(buf), g_iniPath);
    if (buf[0]) g_telemetrySignature = strtoul(buf, nullptr, 16);
    if (!g_telemetrySignature)
    {
        char def[MAX_PATH];
        sprintf(def, "%s\\FO2Telemetry.simdef", g_dir);
        if (FILE* f = fopen(def, "rb"))
        {
            static char text[16384];
            size_t n = fread(text, 1, sizeof(text) - 1, f);
            fclose(f);
            text[n] = 0;
            if (const char* k = strstr(text, "\"TelemetrySignature\""))
                if ((k = strchr(k, ':'))) g_telemetrySignature = (uint32_t)strtoull(k + 1, nullptr, 10);
        }
    }
    if (!g_telemetrySignature) Log("no telemetry signature known - see README (SimHub ignores packets until it is set)");
}

static DWORD WINAPI SendThread(LPVOID)
{
    char host[64];
    GetPrivateProfileStringA("SIMHUB", "Host", "127.0.0.1", host, sizeof(host), g_iniPath);
    int port = GetPrivateProfileIntA("SIMHUB", "Port", 30777, g_iniPath);
    float iniMaxRpm = (float)GetPrivateProfileIntA("SIMHUB", "MaxRpm", 0, g_iniPath);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Log("WSAStartup failed"); return 0; }
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { Log("socket failed"); return 0; }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { Log("bad host %s", host); return 0; }
    Log("sending telemetry to %s:%d (game signature %08X, telemetry signature %08X)", host, port, g_gameSignature, g_telemetrySignature);

    timeBeginPeriod(1);
    Packet p = {};
    p.GameSignature = g_gameSignature;
    p.TelemetrySignature = g_telemetrySignature;
    p.LayoutMajorVersion = 1;
    p.LayoutMinorVersion = 1;
    p.EmitterInstanceId = Random64();

    LARGE_INTEGER freq, t0, now; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
    auto secs = [&](const LARGE_INTEGER& a, const LARGE_INTEGER& b) { return (double)(b.QuadPart - a.QuadPart) / freq.QuadPart; };

    Sample prev; bool havePrev = false;
    LARGE_INTEGER prevT = t0, lastSend = t0, sessionStart = t0;
    int lastLap = -1, lapStartMs = 0, lapsDone = 0, zeroBased = 0;
    float maxRpm = 0;
    float acc[3] = {}, rate[3] = {}, suspVel[4] = {};
    bool wasRunning = false;

    for (;;)
    {
        Sleep(2);
        Sample s; ReadGame(s);
        QueryPerformanceCounter(&now);

        bool running = s.inRace && s.car;
        if (running && !wasRunning)
        {
            // new race: new session, forget lap history and the learned rev limit
            p.SessionId = Random64();
            p.LastLapTimeSeconds = p.BestLapTimeSeconds = 0;
            lastLap = -1; lapStartMs = 0; lapsDone = 0; zeroBased = 0; maxRpm = 0;
            sessionStart = now;
            havePrev = false;
            Log("race started, %d racers, %d laps", s.racers, s.laps);
        }
        wasRunning = running;

        // a new physics sample is any change in the car state
        bool fresh = running && (!havePrev || memcmp(s.pos, prev.pos, 12) || memcmp(s.vel, prev.vel, 12) || s.rpm != prev.rpm);
        if (!fresh && secs(lastSend, now) < 0.05) continue;

        bool paused = running && s.paused;
        p.IsSessionRunning = running;
        p.IsSessionPaused = paused;
        p.IsReplay = running && s.replay;
        p.IsUserInControl = running && !paused && !s.replay;
        p.SessionTimeSeconds = running ? (s.clock > 0 ? s.clock / 1000.0 : secs(sessionStart, now)) : 0;

        if (running)
        {
            p.SpeedKmh = sqrtf(Dot(s.vel, s.vel)) * 3.6f;
            if (p.SpeedKmh > 999) p.SpeedKmh = 0;
            p.LocalVelocityForwardMps = Dot(s.vel, s.fwd);
            p.LocalVelocityLateralMps = Dot(s.vel, s.right);
            p.LocalVelocityUpMps = Dot(s.vel, s.up);

            // World frame: y up, x east, z north (left-handed, so heading is clockwise from north).
            p.VehiclePositionEast = s.pos[0];
            p.VehiclePositionNorth = s.pos[2];
            p.VehiclePositionUp = s.pos[1];
            float yaw = atan2f(s.fwd[0], s.fwd[2]) * kDeg;
            p.YawDegrees = yaw < 0 ? yaw + 360 : yaw;
            p.PitchDegrees = -asinf(fmaxf(-1, fminf(1, s.fwd[1]))) * kDeg;   // nose down positive
            p.RollDegrees = atan2f(-s.right[1], s.up[1]) * kDeg;              // right side down positive

            if (fresh && havePrev)
            {
                float dt = (float)secs(prevT, now);
                float dx = s.pos[0] - prev.pos[0], dy = s.pos[1] - prev.pos[1], dz = s.pos[2] - prev.pos[2];
                if (dx * dx + dy * dy + dz * dz > 30 * 30)
                {
                    // reset to track / teleport
                    p.PhysicsDiscontinuityCounter++;
                    acc[0] = acc[1] = acc[2] = rate[0] = rate[1] = rate[2] = 0;
                }
                else if (dt > 0.004f && dt < 0.2f)
                {
                    float dv[3] = { (s.vel[0] - prev.vel[0]) / dt, (s.vel[1] - prev.vel[1]) / dt, (s.vel[2] - prev.vel[2]) / dt };
                    float df[3] = { (s.fwd[0] - prev.fwd[0]) / dt, (s.fwd[1] - prev.fwd[1]) / dt, (s.fwd[2] - prev.fwd[2]) / dt };
                    float dr[3] = { (s.right[0] - prev.right[0]) / dt, (s.right[1] - prev.right[1]) / dt, (s.right[2] - prev.right[2]) / dt };
                    float a[3] = { Dot(dv, s.fwd), Dot(dv, s.right), Dot(dv, s.up) };
                    // yaw rate: nose turning right; pitch rate: nose going down; roll rate: right side going down
                    float w[3] = { Dot(df, s.right) * kDeg, -Dot(df, s.up) * kDeg, -Dot(dr, s.up) * kDeg };
                    const float k = 0.5f;  // light smoothing, sampling is not locked to the physics step
                    for (int i = 0; i < 3; i++) { acc[i] += (a[i] - acc[i]) * k; rate[i] += (w[i] - rate[i]) * k; }
                    for (int i = 0; i < 4; i++) suspVel[i] += ((s.susp[i] - prev.susp[i]) / dt - suspVel[i]) * k;
                }
            }
            p.LocalSurgeMs2 = acc[0]; p.LocalSwayMs2 = acc[1]; p.LocalHeaveMs2 = acc[2];
            p.YawRateDegreesPerSecond = rate[0]; p.PitchRateDegreesPerSecond = rate[1]; p.RollRateDegreesPerSecond = rate[2];

            p.Throttle = s.throttle; p.Brake = s.brake; p.Handbrake = s.handbrake;
            bool nitroOn = s.nitroButton > 0.5f && s.nitro > 0;
            p.PushToPassActive = nitroOn;
            p.NitroBoost = nitroOn ? 2.0f : 1.0f;
            // FlatOut 2 exposes no wheel spin, so slip is the car's sideways slide (all wheels) and
            // lock is the rear wheels on the handbrake.
            float vf = p.LocalVelocityForwardMps, vAbs = sqrtf(Dot(s.vel, s.vel));
            float lat = vAbs > 2 ? fabsf(p.LocalVelocityLateralMps) / vAbs : 0;
            for (int i = 0; i < 4; i++)
            {
                p.WheelSlip[i] = fminf(1.0f, lat * 2);
                p.WheelLock[i] = (i >= 2 && s.handbrake > 0.5f && vf > 3) ? 1.0f : 0.0f;
                p.SuspensionVelocityMps[i] = suspVel[i];
            }

            p.EngineRpm = s.rpm;
            if (s.rpm > maxRpm) maxRpm = s.rpm;
            p.EngineMaxRpm = iniMaxRpm > 0 ? iniMaxRpm : fmaxf(maxRpm, 6000.0f);
            memset(p.Gear, 0, sizeof(p.Gear));
            if (s.gear < 0) strcpy(p.Gear, "R");
            else if (s.gear == 0) strcpy(p.Gear, "N");
            else sprintf(p.Gear, "%d", s.gear);

            if (s.race)
            {
                p.RacePosition = s.position;
                p.OpponentCount = s.racers;
                p.TotalLaps = s.laps > 0 ? s.laps : -1;
                if (s.lap == 0) zeroBased = 1;
                if (lastLap < 0) { lastLap = s.lap; lapStartMs = 0; }
                else if (s.lap > lastLap)
                {
                    // lap completed: race time now minus the time the lap started
                    double lap = (s.clock - lapStartMs) / 1000.0;
                    lapsDone++;
                    if (lap > 1)
                    {
                        p.LastLapTimeSeconds = lap;
                        if (p.BestLapTimeSeconds <= 0 || lap < p.BestLapTimeSeconds) p.BestLapTimeSeconds = lap;
                        Log("lap %d: %.3f s", lapsDone, lap);
                    }
                    lastLap = s.lap; lapStartMs = s.clock;
                }
                int done = s.lap - 1 + zeroBased;
                if (done < lapsDone) done = lapsDone;
                if (s.laps > 0 && done > s.laps) done = s.laps;
                p.CompletedLaps = done < 0 ? 0 : done;
                p.CurrentLapTime = s.clock > lapStartMs ? (s.clock - lapStartMs) / 1000.0 : 0;
            }
            else
            {
                p.RacePosition = 0; p.OpponentCount = 0; p.CompletedLaps = 0; p.TotalLaps = -1; p.CurrentLapTime = 0;
            }
            prev = s; prevT = now; havePrev = true;
        }

        FfbInput fi;
        fi.running = running; fi.paused = paused;
        fi.vFwd = p.LocalVelocityForwardMps; fi.vLat = p.LocalVelocityLateralMps;
        fi.yawRateDegS = p.YawRateDegreesPerSecond;
        fi.swayAcc = p.LocalSwayMs2; fi.surgeAcc = p.LocalSurgeMs2;
        for (int i = 0; i < 4; i++) fi.suspVel[i] = p.SuspensionVelocityMps[i];
        FfbPublish(fi);

        p.PacketsCounter++;
        sendto(sock, (const char*)&p, sizeof(p), 0, (const sockaddr*)&addr, sizeof(addr));
        lastSend = now;
    }
}

// Player::ApplyControls (stdcall, called for human players at 0x46FA34): the wheel and pedals are
// written into the local player's controls right before the game turns them into car inputs.
typedef void(__stdcall* ApplyControlsFn)(BYTE* player, int arg);
static void __stdcall ApplyControlsHook(BYTE* player, int arg)
{
    if (player && player == LocalPlayer()) FfbApplyInput(player);
    ((ApplyControlsFn)0x46FA50)(player, arg);
}

static bool PatchCall(DWORD site, void* fn, DWORD expectTarget)
{
    BYTE* p = (BYTE*)site;
    if (p[0] != 0xE8 || site + 5 + *(int*)(p + 1) != expectTarget) return false;
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int*)(p + 1) = (int)((DWORD)fn - (site + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(mod);

    GetModuleFileNameA(mod, g_dir, MAX_PATH);
    *strrchr(g_dir, '\\') = 0;
    sprintf(g_iniPath, "%s\\FO2Telemetry.ini", g_dir);
    sprintf(g_logPath, "%s\\FO2Telemetry.log", g_dir);
    DeleteFileA(g_logPath);

    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(exe, GetFileExInfoStandard, &fa) || fa.nFileSizeHigh || fa.nFileSizeLow != 5369344 ||
        !PatchCall(0x46FA34, (void*)ApplyControlsHook, 0x46FA50))
    {
        Log("unsupported FlatOut2.exe (needs v1.2, 5,369,344 bytes) - telemetry disabled");
        return TRUE;
    }
    LoadSignatures();
    FfbStart(g_iniPath);
    if (GetPrivateProfileIntA("SIMHUB", "Enabled", 1, g_iniPath))
    {
        RegisterDefinition();
        CreateThread(nullptr, 0, SendThread, nullptr, 0, nullptr);
    }
    else Log("SimHub output disabled in ini");
    return TRUE;
}
