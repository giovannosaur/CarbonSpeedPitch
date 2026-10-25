// CarbonSpeedPitch.asi
// Raises the engine sound pitch of the player car as its speed increases
//
// How it works (all addresses are NFSC.exe v1.4):
//
//   DMix (dynamic mixer) pitch output, in cents
//        -> CARSFX_GinsuEngine::SetEngineParams  (0x548090)
//             PitchMultipli (this+0x3C) = 2^(cents/1200)          [0x548120]
//             this->SetGinsuParams()   (vtable +0x40)               [0x5484F7]
//        -> CARSFX_SingleGinsuEng::SetGinsuParams (0x546410)
//           CARSFX_DualGinsuEng::SetGinsuParams   (0x549480)
//             SNDSYS pitch multiplier (4.12 fixed) =
//                 PitchMultipli * timeRatio * 4096  -> sub_89D5E7(handle, mult)
//
//   This is the same multiplier that the game's own Doppler (DMix 3D, from
//   SFXCTL_3DObjPos inputs 13/14) and mix-map pitch end up in.
//
//   hook vtable slot 16 (SetGinsuParams) of both Ginsu engine classes and
//   multiply PitchMultipli by our speed factor right before the game converts it
//   into the voice pitch. Because SetEngineParams recomputes PitchMultipli from
//   scratch every frame, nothing accumulates. The Csis CAR pitch-offset that the
//   game sends after SetGinsuParams also reads PitchMultipli, so all engine layers
//   stay in tune with each other.
//
//   Cars that use the older sample-based engine (CARSFX_AEMSEngine) compute
//   PitchMultipli inline at 0x5466E5; we patch that store with a small jump.
//
//   Gating (all must hold, each can be disabled in [Conditions]):
//     GameState (0xA99BBC) == 6        -> otherwise stock pitch, smoother reset
//     NIS instance (0xB4D964) == 0     -> otherwise player glides back to stock pitch
//     DALVehicle::GetIsDrivingCamera (0x4B6D50, player 0) == true -> same
//
//   Player check: CARSFX::m_pEAXCar (this+0x14) == SndCamera::m_PlayerCars[0]
//   (0xA8E180). Speed: EAX_CarState (EAXCar+0x34) +0xA4 = |velocity| in m/s.

#include "pch.h"   // Visual Studio precompiled header (keep as the first include)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <cstdlib>

// ---------------------------------------------------------------- addresses
static const uintptr_t VT_SINGLEGINSU_SETGINSU = 0x009CE140; // vtable 0x9CE100 slot 16
static const uintptr_t VT_DUALGINSU_SETGINSU = 0x009CE188; // vtable 0x9CE148 slot 16
static const uintptr_t FN_SINGLE_SETGINSU = 0x00546410;
static const uintptr_t FN_DUAL_SETGINSU = 0x00549480;
static const uintptr_t AEMS_PATCH_ADDR = 0x005466E5; // fstp [esi+3Ch] ; jnz short 5466EC
static const uintptr_t AEMS_CONT_JNZ = 0x005466EC;
static const uintptr_t AEMS_CONT_FALL = 0x005466EA;
static const uintptr_t PLAYER_EAXCAR_0 = 0x00A8E180; // SndCamera::m_PlayerCars[0]

static const int OFS_SFX_EAXCAR = 0x14;  // CARSFX::m_pEAXCar
static const int OFS_SFX_PITCHMULT = 0x3C;  // CARSFX_EngineBase::PitchMultipli
static const int OFS_EAXCAR_STATE = 0x34;  // EAXCar::mPhysCar (EAX_CarState*)
static const int OFS_STATE_SPEED = 0xA4;  // EAX_CarState::mFWSpeed (m/s)
// Gating
static const uintptr_t GAMESTATE_ADDR = 0x00A99BBC; // int, 6 = in-game / gameplay
static const uintptr_t NIS_ACTIVE_ADDR = 0x00B4D964; // INIS instance (non-zero while a cutscene runs)
static const uintptr_t FN_GETISDRIVINGCAM = 0x004B6D50; // DALVehicle::GetIsDrivingCamera(int&, int) - ret 8, ignores this

static const int OFS_STATE_CONTEXT = 0x21C; // EAX_CarState::mContext (0=player,1=AI racer,2=cop,3=traffic)

// ---------------------------------------------------------------- config
struct Config {
    int   mode = 1;      // 0 = disabled, 1 = player car only, 2 = every engine (each by its own speed)
    float startKmh = 40.0f;  // below this: no extra pitch
    float fullKmh = 320.0f; // at/above this: maxCents
    float minCents = 0.0f;   // pitch at/below startKmh (negative = lower)
    float maxCents = 150.0f; // 100 cents = 1 semitone
    float curve = 1.6f;   // >1 = most of the rise happens at high speed
    float smoothing = 0.15f;  // seconds, one-pole smoothing of the pitch (0 = off)
    bool  affectAEMS = true;
    bool  needGameplay = true;   // GameState == 6
    bool  needNoNIS = true;   // no cutscene
    bool  needDrivingCam = true;   // GetIsDrivingCamera(player 0) == true
};

static Config      g_cfg;
static char        g_iniPath[MAX_PATH];
static FILETIME    g_iniTime;
static CRITICAL_SECTION g_cs;

typedef void(__thiscall* SetGinsuParams_t)(void* self);
static SetGinsuParams_t g_origSingle = nullptr;
static SetGinsuParams_t g_origDual = nullptr;

// smoothing state (player only; AI cars use the raw value)
static float   g_smoothedCents = 0.0f;
static LARGE_INTEGER g_qpf, g_lastQpc;

// ---------------------------------------------------------------- ini
static float IniFloat(const char* sec, const char* key, float def)
{
    char buf[64], defs[64];
    snprintf(defs, sizeof(defs), "%g", def);
    GetPrivateProfileStringA(sec, key, defs, buf, sizeof(buf), g_iniPath);
    return (float)atof(buf);
}

static void LoadConfig()
{
    Config c;
    c.mode = GetPrivateProfileIntA("General", "Enabled", 1, g_iniPath);
    if (c.mode < 0 || c.mode > 2) c.mode = 1;
    c.affectAEMS = GetPrivateProfileIntA("General", "AffectAEMSEngines", 1, g_iniPath) != 0;
    c.needGameplay = GetPrivateProfileIntA("Conditions", "RequireGameplay", 1, g_iniPath) != 0;
    c.needNoNIS = GetPrivateProfileIntA("Conditions", "RequireNoNIS", 1, g_iniPath) != 0;
    c.needDrivingCam = GetPrivateProfileIntA("Conditions", "DrivingCameraOnly", 1, g_iniPath) != 0;
    c.startKmh = IniFloat("Pitch", "StartSpeedKmh", c.startKmh);
    c.fullKmh = IniFloat("Pitch", "FullSpeedKmh", c.fullKmh);
    c.minCents = IniFloat("Pitch", "MinCents", c.minCents);
    c.maxCents = IniFloat("Pitch", "MaxCents", c.maxCents);
    c.curve = IniFloat("Pitch", "Curve", c.curve);
    c.smoothing = IniFloat("Pitch", "SmoothingSec", c.smoothing);
    if (c.fullKmh <= c.startKmh + 1.0f) c.fullKmh = c.startKmh + 1.0f;
    if (c.curve < 0.1f) c.curve = 0.1f;
    if (c.maxCents > 1200.0f) c.maxCents = 1200.0f;   // keep inside 1 octave
    if (c.maxCents < -1200.0f) c.maxCents = -1200.0f;
    if (c.minCents > 1200.0f) c.minCents = 1200.0f;
    if (c.minCents < -1200.0f) c.minCents = -1200.0f;
    if (c.smoothing < 0.0f) c.smoothing = 0.0f;
    EnterCriticalSection(&g_cs);
    g_cfg = c;
    LeaveCriticalSection(&g_cs);
}

static bool GetIniTime(FILETIME* ft)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &d)) return false;
    *ft = d.ftLastWriteTime;
    return true;
}

// Reload the ini whenever it changes on disk, so values can be tuned live (Alt+Tab, edit, save).
static DWORD WINAPI WatchThread(LPVOID)
{
    for (;;) {
        Sleep(500);
        FILETIME ft;
        if (GetIniTime(&ft) && CompareFileTime(&ft, &g_iniTime) != 0) {
            g_iniTime = ft;
            Sleep(100);           // let the editor finish writing
            LoadConfig();
        }
    }
    return 0;
}

// ---------------------------------------------------------------- core
static float TargetCents(float speedMs, const Config& c)
{
    float kmh = speedMs * 3.6f;
    if (!(kmh > c.startKmh)) return c.minCents;     // also catches NaN
    float t = (kmh - c.startKmh) / (c.fullKmh - c.startKmh);
    if (t > 1.0f) t = 1.0f;
    return c.minCents + (c.maxCents - c.minCents) * powf(t, c.curve);
}

// ---- gate
static bool IsDrivingCamera()
{
    typedef bool(__thiscall* Fn)(void* self, int* out, int player);
    int out = 0;
    bool ok = ((Fn)FN_GETISDRIVINGCAM)(nullptr, &out, 0);
    return ok && out != 0;
}

static int  g_gate = 0;
static LARGE_INTEGER g_gateQpc = {};

static int EvaluateGate(const Config& c)
{
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    // Re-evaluate at most every ~10 ms (the hook runs once per engine object per frame).
    if (g_gateQpc.QuadPart && (now.QuadPart - g_gateQpc.QuadPart) < g_qpf.QuadPart / 100)
        return g_gate;
    g_gateQpc = now;

    int gate = 2;
    if (c.needGameplay && *(volatile int*)GAMESTATE_ADDR != 6)               gate = 0;
    else if (c.needNoNIS && *(volatile uint32_t*)NIS_ACTIVE_ADDR != 0)       gate = 1;
    else if (c.needDrivingCam && !IsDrivingCamera())                          gate = 1;
    g_gate = gate;
    return gate;
}

// Returns the multiplier for the engine object, or 1.0 if it should not be touched.
static float ComputeFactor(void* sfx)
{
    Config c;
    EnterCriticalSection(&g_cs); c = g_cfg; LeaveCriticalSection(&g_cs);
    if (c.mode == 0 || !sfx) return 1.0f;

    int gate = EvaluateGate(c);
    if (gate == 0) {                       // menus / loading: stock pitch, reset smoother
        g_smoothedCents = 0.0f;
        QueryPerformanceCounter(&g_lastQpc);
        return 1.0f;
    }

    {
        char* eaxCar = *(char**)((char*)sfx + OFS_SFX_EAXCAR);
        if (!eaxCar) return 1.0f;
        char* state = *(char**)(eaxCar + OFS_EAXCAR_STATE);
        if (!state) return 1.0f;

        bool isPlayer = (eaxCar == *(char**)PLAYER_EAXCAR_0);
        if (c.mode == 1 && !isPlayer) return 1.0f;

        float speed = *(float*)(state + OFS_STATE_SPEED);
        // While a cutscene / non-driving camera is active the player's target is stock pitch (0 cents);
        // with smoothing on it glides back instead of snapping. AI cars just get stock pitch.
        float cents = (gate == 2) ? TargetCents(speed, c) : 0.0f;
        if (gate != 2 && !isPlayer) return 1.0f;

        if (isPlayer && c.smoothing > 0.0f) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            float dt = (float)(now.QuadPart - g_lastQpc.QuadPart) / (float)g_qpf.QuadPart;
            g_lastQpc = now;
            if (dt < 0.0f || dt > 0.5f) dt = 0.5f;
            float a = 1.0f - expf(-dt / c.smoothing);
            g_smoothedCents += (cents - g_smoothedCents) * a;
            cents = g_smoothedCents;
        }
        return powf(2.0f, cents / 1200.0f);
    }
}

static void ApplyToEngine(void* sfx)
{
    float f = ComputeFactor(sfx);
    if (f != 1.0f) {
        float* pm = (float*)((char*)sfx + OFS_SFX_PITCHMULT);
        float v = *pm * f;
        if (v > 15.0f) v = 15.0f;                    // SNDSYS stores 4.12 in 16 bits
        *pm = v;
    }
}

// -------- Ginsu engines: vtable hook on SetGinsuParams
static void __fastcall Hook_SingleSetGinsu(void* self, void* /*edx*/)
{
    ApplyToEngine(self);
    g_origSingle(self);
}

static void __fastcall Hook_DualSetGinsu(void* self, void* /*edx*/)
{
    ApplyToEngine(self);
    g_origDual(self);
}

// -------- AEMS engines: called from the code cave with esi = this
extern "C" void __cdecl AEMS_Apply(void* self)
{
    if (!g_cfg.affectAEMS) return;
    ApplyToEngine(self);
}

// ---------------------------------------------------------------- patching
static bool PatchPointer(uintptr_t addr, void* newVal, void** oldVal)
{
    DWORD prot;
    if (!VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &prot)) return false;
    if (oldVal) *oldVal = *(void**)addr;
    *(void**)addr = newVal;
    VirtualProtect((void*)addr, 4, prot, &prot);
    return true;
}

static void WriteRel32(uint8_t* at, uintptr_t target)
{
    *(int32_t*)at = (int32_t)(target - ((uintptr_t)at + 4));
}

static bool InstallAEMSCave()
{
    static const uint8_t expect[] = { 0xD9, 0x5E, 0x3C, 0x75, 0x02, 0xB0, 0x01 };
    if (memcmp((void*)AEMS_PATCH_ADDR, expect, sizeof(expect)) != 0) return false;

    uint8_t* cave = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) return false;
    uint8_t* p = cave;
    *p++ = 0xD9; *p++ = 0x5E; *p++ = 0x3C;            // fstp dword ptr [esi+3Ch]   (original)
    *p++ = 0x9C;                                       // pushfd   (flags of cmp cl,0Ch)
    *p++ = 0x60;                                       // pushad
    *p++ = 0x56;                                       // push esi
    *p++ = 0xE8; WriteRel32(p, (uintptr_t)&AEMS_Apply); p += 4;  // call AEMS_Apply
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;            // add esp,4
    *p++ = 0x61;                                       // popad
    *p++ = 0x9D;                                       // popfd
    *p++ = 0x0F; *p++ = 0x85; WriteRel32(p, AEMS_CONT_JNZ); p += 4;  // jnz 5466EC
    *p++ = 0xE9; WriteRel32(p, AEMS_CONT_FALL); p += 4;              // jmp 5466EA
    FlushInstructionCache(GetCurrentProcess(), cave, p - cave);

    DWORD prot;
    if (!VirtualProtect((void*)AEMS_PATCH_ADDR, 5, PAGE_EXECUTE_READWRITE, &prot)) return false;
    uint8_t* site = (uint8_t*)AEMS_PATCH_ADDR;
    site[0] = 0xE9; WriteRel32(site + 1, (uintptr_t)cave);   // overwrites fstp + jnz short
    VirtualProtect((void*)AEMS_PATCH_ADDR, 5, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

static void Install()
{
    // Version check: both vtable slots must point at the functions we analysed.
    if (*(uintptr_t*)VT_SINGLEGINSU_SETGINSU != FN_SINGLE_SETGINSU ||
        *(uintptr_t*)VT_DUALGINSU_SETGINSU != FN_DUAL_SETGINSU) {
        return;
    }
    PatchPointer(VT_SINGLEGINSU_SETGINSU, (void*)&Hook_SingleSetGinsu, (void**)&g_origSingle);
    PatchPointer(VT_DUALGINSU_SETGINSU, (void*)&Hook_DualSetGinsu, (void**)&g_origDual);

    InstallAEMSCave();   // silently skipped if the patch site is not recognised
}

static void InitPaths(HMODULE hMod)
{
    GetModuleFileNameA(hMod, g_iniPath, MAX_PATH);
    char* dot = strrchr(g_iniPath, '.');
    if (dot) *dot = 0;
    strcat_s(g_iniPath, MAX_PATH, ".ini");
}

BOOL WINAPI DllMain(HMODULE hMod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        InitializeCriticalSection(&g_cs);
        QueryPerformanceFrequency(&g_qpf);
        QueryPerformanceCounter(&g_lastQpc);
        InitPaths(hMod);
        LoadConfig();
        GetIniTime(&g_iniTime);
        Install();
        CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
    }
    return TRUE;
}