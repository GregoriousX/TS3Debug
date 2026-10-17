// TS3Debug.asi - debug features for Toy Story 3 Complete Edition (PC, Steam)
//
// Game code used:
//   LoopScenes::ActivateFreeCamera(int, bool)   -> free camera toggle
//   BaseLoop::s_Scenes                          -> LoopScenes instance
//   LoopScenes::CreateFreeCameras()             -> fix: no duplicate free camera on later level loads
//   Cheats::Daemon()                            -> per-frame hook (main thread)
//   Cheats::s_cheats / Cheats::Info[16]         -> Stealth, Ammo flags
//   Cheats::s_avatar                            -> who Stealth/Ammo apply to (never set by the game)
//   CMasterTimer::SetSpeed / g_timer            -> pause, game speed, frame advance
//   HealthIF vtable (GetHealth, SetHealth, DoDamage, Kill) -> god mode
//   FreeCamControlMapper vtable (GetAxis, ButtonDown) -> keyboard flight
//   ParameterSet + CActorCreator::CreateFromParameters -> actor spawner
//   Script_ActorSetPosition (inlined here)      -> save/load position, teleport
// The developer cheats "God", "Unlock levels", "Wireframe" and "Say Stress" are
// registered but nothing in the retail game reads them, so god mode is done here
// with a damage hook and the others are not exposed.
// Everything is found by signature scan at runtime, nothing is hardcoded.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <cmath>

#include "spawnlist.h"

// ---------------------------------------------------------------------------
// Game structures
// ---------------------------------------------------------------------------
struct CheatInfo                  // Cheats::Info, sizeof == 0x68
{
    uint32_t    id;               // FourCC ("god", "ninj", "ammo", ...)
    uint32_t    _pad04;
    const char* name;             // "God", "Stealth", ...
    int32_t     unk10;
    bool        enabled;
    uint8_t     _pad15[3];
    bool      (*callback)(CheatInfo*);
    int32_t     buttons[16];      // controller code sequence
    int32_t     buttonCount;
    uint32_t    _pad64;
};
static_assert(sizeof(CheatInfo) == 0x68, "CheatInfo size");

// CMasterTimer::ETimerSpeed values (same numbers the game's time cheats use)
enum TimeSpeed { SPEED_STOP = 0, SPEED_EIGHTH = 2, SPEED_QUARTER = 3, SPEED_HALF = 4, SPEED_DOUBLE = 5, SPEED_NORMAL = 9 };

using ActivateFreeCamera_t = bool (__fastcall*)(void* loopScenes, int player, bool toggle);
using CreateFreeCameras_t  = void (__fastcall*)(void* loopScenes);
using FocusCurrent_t       = uint8_t*(__fastcall*)(void* focusMgr, int player);
using FocusFind_t          = void*(__fastcall*)(void* focusMgr, const char* name, int player);
using FocusPop_t           = void (__fastcall*)(void* focusMgr, void* mapper, int player);
using PropStackRemove_t    = bool (__fastcall*)(void* stack, int propSetId);
using Update_t             = void (__fastcall*)(uint8_t* self);
using FloatPropGet_t       = float(__fastcall*)(uint8_t* prop, void* arg);
using Daemon_t            = void (__fastcall*)();
using GetAxis_t            = float(__fastcall*)(void* mapper, int axis);
using ButtonDown_t         = bool (__fastcall*)(void* mapper, int button, bool raw);
using VoiceQueue_t         = void (__fastcall*)(void* queue, const char* name, float delay, bool b, float f);
using ParamSet_t           = void (__fastcall*)(void* set, const char* key, const char* value, bool b);
using ParamDtor_t          = void (__fastcall*)(void* set);
using CreateActor_t        = void*(__fastcall*)(void* set, const float* pos, float heading, bool a, bool b);
using CurrentCamera_t      = uint8_t*(__fastcall*)(void* director, void* scene);
using SetSpeed_t           = void (__fastcall*)(void* timer, int speed, bool b);
using DoDamage_t           = uint64_t(__fastcall*)(void* health, float amount, void* source, int reason, int type);
using Kill_t               = uint64_t(__fastcall*)(void* health, void* source);
using GetHealth_t          = float(__fastcall*)(void* health);
using SetHealth_t          = void (__fastcall*)(void* health, float value);
using UpdateState_t        = void (__fastcall*)(void* worldState);

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static HMODULE g_self;
static uint8_t* g_base;
static size_t   g_size;
static char     g_iniPath[MAX_PATH];
static char     g_logPath[MAX_PATH];

static ActivateFreeCamera_t g_ActivateFreeCamera;
static void*      g_loopScenes;
static CreateFreeCameras_t g_CreateFreeCamerasOrig; // trampoline
static CreateFreeCameras_t g_CreateFreeCamerasFn;   // unhooked entry (trampoline or original)
static CreateFreeCameras_t g_DeleteFreeCameras;     // LoopScenes::DeleteFreeCameras
static uint8_t**           g_propSetMgr;            // CSingleton<CameraPropertySetManager>::s_instance
static void**              g_eyePropSetVtbl;        // EyeCameraPropertySet (cutscene shots)
static uint8_t*          g_focusMgr;       // ControllerFocusManager
static FocusCurrent_t    g_FocusCurrent;
static FocusFind_t       g_FocusFind;
static FocusPop_t        g_FocusPop;
static PropStackRemove_t g_PropStackRemove; // CameraPropertySetStack::Remove
static Daemon_t   g_DaemonOrig;          // trampoline
static uint8_t*   g_daemon;
static void**     g_cheatGuard;          // object with vfunc +0x28: "cheats blocked"
static CheatInfo* g_cheats;
static uint64_t*  g_cheatAvatar;        // Cheats::s_avatar (CHandle)
static int32_t*   g_cheatCount;
static uint8_t**  g_voiceOwner;          // *g_voiceOwner + 0x1D8 = VoiceQueue
static void**     g_freeCamVtbl;
static GetAxis_t    g_GetAxisOrig;
static ButtonDown_t g_ButtonDownOrig;
static uint8_t*   g_timer;               // CMasterTimer g_timer
static int32_t*   g_timeSpeedVar;        // Cheats::s_timeSpeed
static SetSpeed_t g_SetSpeed;
static uint8_t**  g_players;             // CSingleton<Players>::s_instance
static void**     g_cameraDirector;
static CurrentCamera_t g_CurrentCamera;
static ParamSet_t      g_ParamSet;
static ParamDtor_t     g_ParamDtor;
static CreateActor_t   g_CreateActor;
static DoDamage_t g_DoDamageOrig[2];
static Kill_t     g_KillOrig[2];
static GetHealth_t       g_GetHealthOrig[2];
static uint8_t*  g_playerHealth;        // player's HealthIF, valid while the avatar below is unchanged
static uint8_t*  g_playerHealthOwner;

static UpdateState_t g_UpdateState;     // WorldStateDBlock::UpdateState
static uint32_t  g_posVfuncA, g_posVfuncB; // actor vtable offsets called after moving
struct SavedPos { bool valid; float p[3]; };
static SavedPos  g_slots[3];
static volatile bool g_carry;           // player follows the free camera
static volatile bool g_capturing;       // a key is being rebound in the menu: hotkeys are suspended
static volatile bool g_god;
static volatile bool g_infiniteAmmo;
static volatile LONG g_spawnIndex;
static volatile DWORD g_lastDaemonTick;  // GetTickCount() of the last in-level frame
static int  g_resumeSpeed = SPEED_NORMAL;

// Config
struct Keys
{
    int freeCam   = VK_F1;
    int detach    = VK_F2;
    int god       = VK_F3;
    int stealth   = VK_F4;
    int ammo      = VK_F5;
    int pause     = VK_F6;
    int timeCycle = VK_F7;
    int frameStep = VK_F8;
    int menu      = VK_HOME;
    int spawn     = VK_INSERT;
    int spawnNext = VK_NEXT;
    int spawnPrev = VK_PRIOR;
    int slot[3]   = { VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3 };   // load; with Ctrl: save
    int toCamera  = VK_END;
    int carry     = VK_DELETE;
    int saveMod   = VK_CONTROL;                                // hold + slot key = save
    // free camera flight
    int fwd = 'W', back = 'S', left = 'A', right = 'D';
    int up = VK_SPACE, down = 'C';
    int lookUp = VK_UP, lookDown = VK_DOWN, lookLeft = VK_LEFT, lookRight = VK_RIGHT;
    int fast = VK_LSHIFT, slow = VK_LCONTROL;
} static g_keys;

static float g_fastMul = 3.0f;
static float g_slowMul = 0.25f;
static float g_lookSpeed = 1.0f;
static bool  g_invertForward = true;     // game negates stick Y for fore/aft
static bool  g_invertPitch   = false;
static bool  g_keyboardFlight = true;
static bool  g_voiceFeedback = true;
static float g_stepFps = 30.0f;
static float g_spawnDistance = 5.0f;
static float g_carryDrop = 1.5f;
static bool  g_godBlocksKill = false;
static bool  g_menuOnStart = false;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static void Log(const char* fmt, ...)
{
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "a") != 0 || !f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Signature scanning
// ---------------------------------------------------------------------------
static uint8_t* FindPattern(const char* pattern, uint8_t* start = nullptr, uint8_t* end = nullptr)
{
    uint8_t bytes[128];
    bool    mask[128];
    int     n = 0;
    for (const char* p = pattern; *p && n < 128;)
    {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { mask[n] = false; bytes[n++] = 0; p += (p[1] == '?') ? 2 : 1; continue; }
        char hex[3] = { p[0], p[1], 0 };
        bytes[n] = (uint8_t)strtoul(hex, nullptr, 16);
        mask[n++] = true;
        p += 2;
    }
    if (!start) start = g_base;
    if (!end)   end = g_base + g_size;
    for (uint8_t* s = start; s + n <= end; ++s)
    {
        int i = 0;
        while (i < n && (!mask[i] || s[i] == bytes[i])) ++i;
        if (i == n) return s;
    }
    return nullptr;
}

static uint8_t* Rip(uint8_t* insn, int dispOffset, int insnLen)
{
    return insn + insnLen + *reinterpret_cast<int32_t*>(insn + dispOffset);
}

// Locate a class vtable through MSVC RTTI (.?AVName@@); 'offset' selects the
// base-class subobject (0 = primary vtable).
static void** FindVtable(const char* mangled, uint32_t offset = 0)
{
    size_t len = strlen(mangled);
    uint8_t* str = nullptr;
    for (uint8_t* s = g_base; s + len < g_base + g_size; ++s)
        if (memcmp(s, mangled, len + 1) == 0) { str = s; break; }
    if (!str) return nullptr;
    uint32_t tdRva = (uint32_t)(str - 0x10 - g_base);

    for (uint8_t* p = g_base; p + 0x18 < g_base + g_size; p += 4)
    {
        auto col = reinterpret_cast<uint32_t*>(p);
        if (col[0] != 1 || col[1] != offset || col[3] != tdRva || col[5] != (uint32_t)(p - g_base))
            continue;
        uint64_t colVa = (uint64_t)p;
        for (uint8_t* q = g_base; q + 8 < g_base + g_size; q += 8)
            if (*reinterpret_cast<uint64_t*>(q) == colVa)
                return reinterpret_cast<void**>(q + 8);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Small inline hook: 14-byte absolute jmp + trampoline
// ---------------------------------------------------------------------------
static void* HookEntry(uint8_t* target, size_t stolen, void* detour)
{
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, target, stolen);
    uint8_t jmpBack[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    *reinterpret_cast<uint64_t*>(jmpBack + 6) = (uint64_t)(target + stolen);
    memcpy(tramp + stolen, jmpBack, sizeof(jmpBack));

    DWORD old;
    VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &old);
    uint8_t jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    *reinterpret_cast<uint64_t*>(jmp + 6) = (uint64_t)detour;
    memcpy(target, jmp, sizeof(jmp));
    for (size_t i = sizeof(jmp); i < stolen; ++i) target[i] = 0x90;
    VirtualProtect(target, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    return tramp;
}

static void* PatchVtable(void** vtbl, int slot, void* fn)
{
    DWORD old;
    VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &old);
    void* orig = vtbl[slot];
    vtbl[slot] = fn;
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
    return orig;
}

// ---------------------------------------------------------------------------
// Input helpers
// ---------------------------------------------------------------------------
static bool GameHasFocus()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static bool Down(int vk) { return vk && (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Edge detector; each thread uses its own table.
struct KeyEdges
{
    bool prev[256] = {};
    bool Pressed(int vk)
    {
        if (vk <= 0 || vk > 255) return false;
        bool now = Down(vk);
        bool edge = now && !prev[vk];
        prev[vk] = now;
        return edge;
    }
};

// ---------------------------------------------------------------------------
// Game wrappers (main thread only)
// ---------------------------------------------------------------------------
static void Voice(const char* name)
{
    if (!g_voiceFeedback || !g_voiceOwner || !*g_voiceOwner) return;
    uint8_t* queue = *g_voiceOwner + 0x1D8;
    auto fn = reinterpret_cast<VoiceQueue_t>((*reinterpret_cast<void***>(queue))[3]);
    fn(queue, name, 0.0f, false, 0.0f);
}

static bool CheatsBlocked()
{
    if (!g_cheatGuard || !*g_cheatGuard) return false;
    void* obj = *g_cheatGuard;
    auto fn = reinterpret_cast<bool(__fastcall*)(void*)>((*reinterpret_cast<void***>(obj))[5]);
    return fn(obj);
}

static CheatInfo* FindCheat(const char* name)
{
    if (!g_cheats || !g_cheatCount) return nullptr;
    int n = *g_cheatCount;
    if (n < 0 || n > 16) return nullptr;
    for (int i = 0; i < n; ++i)
        if (g_cheats[i].name && _stricmp(g_cheats[i].name, name) == 0)
            return &g_cheats[i];
    return nullptr;
}

static bool CheatEnabled(const char* name)
{
    CheatInfo* c = FindCheat(name);
    return c && c->enabled;
}

// Flag-only cheats (Stealth, Ammo): the game reads Info::enabled directly.
static void ToggleCheat(const char* name)
{
    CheatInfo* c = FindCheat(name);
    if (!c) { Log("cheat '%s' not registered (not in a level yet?)", name); return; }
    if (CheatsBlocked()) { Log("cheats blocked right now"); return; }
    c->enabled = !c->enabled;
    Log("cheat %s -> %s", c->name, c->enabled ? "ON" : "OFF");
    Voice(c->enabled ? "cheat_on" : "cheat_off");
}

// God mode keeps the player's health full instead of blocking damage: hits,
// knock-backs and their recovery run exactly as in the normal game, they just
// can't kill. (Skipping damage or forcing the invulnerable flag leaves the
// player stuck in hit reactions.)
// HealthIF (= HealthTracker + 0x18): +0x08 max health, +0x14 health, slot 3 SetHealth.
static uint8_t* PlayerAvatar();

static float MaxHealth(uint8_t* h) { return *reinterpret_cast<float*>(h + 0x08); }
static float Health(uint8_t* h)    { return *reinterpret_cast<float*>(h + 0x14); }

static void SetHealth(uint8_t* h, float value)
{
    reinterpret_cast<SetHealth_t>((*reinterpret_cast<void***>(h))[3])(h, value);
}

static uint8_t* PlayerHealth()
{
    uint8_t* avatar = PlayerAvatar();
    if (!avatar || avatar != g_playerHealthOwner) { g_playerHealth = nullptr; g_playerHealthOwner = nullptr; }
    return g_playerHealth;
}

static void ApplyGod()
{
    if (!g_god) return;
    if (uint8_t* h = PlayerHealth())
        if (MaxHealth(h) > 0.0f && Health(h) < MaxHealth(h)) SetHealth(h, MaxHealth(h));
}

static void ToggleGod()
{
    g_god = !g_god;
    ApplyGod();
    Log("god mode -> %s", g_god ? "ON" : "OFF");
    Voice(g_god ? "cheat_on" : "cheat_off");
}

// Weapon hooks do the work (see WeaponUpdate_Hook). The game's own Ammo cheat
// flag is left alone: with it on, some ball counters dropped to zero.
static void ToggleAmmo()
{
    g_infiniteAmmo = !g_infiniteAmmo;
    Log("infinite ammo -> %s", g_infiniteAmmo ? "ON" : "OFF");
    Voice(g_infiniteAmmo ? "cheat_on" : "cheat_off");
}

static int CurrentSpeed()
{
    if (g_timer && *reinterpret_cast<int32_t*>(g_timer + 0x3c) == 0) return SPEED_STOP;
    return g_timeSpeedVar ? *g_timeSpeedVar : SPEED_NORMAL;
}

// Applies the speed straight to the master timer. Going through the cheat
// callbacks can't resume a stopped timer: Cheats::Daemon skips applying
// speed changes while the timer is stopped.
static void SetTimeSpeed(int speed)
{
    if (!g_SetSpeed || !g_timer) return;
    if (g_timeSpeedVar) *g_timeSpeedVar = speed;
    g_SetSpeed(g_timer, speed, false);
    static const struct { const char* name; int speed; } map[] = {
        { "Stop Time", SPEED_STOP }, { "Time 1/8x", SPEED_EIGHTH }, { "Time 1/4x", SPEED_QUARTER },
        { "Time 1/2x", SPEED_HALF }, { "Time 2x", SPEED_DOUBLE } };
    for (auto& m : map)
        if (CheatInfo* c = FindCheat(m.name)) c->enabled = (m.speed == speed);
    Log("time speed -> %d", speed);
}

static void TogglePause()
{
    int cur = CurrentSpeed();
    if (cur == SPEED_STOP) SetTimeSpeed(g_resumeSpeed);
    else { g_resumeSpeed = cur; SetTimeSpeed(SPEED_STOP); }
}

static void CycleSpeed()
{
    static const int order[] = { SPEED_NORMAL, SPEED_DOUBLE, SPEED_HALF, SPEED_QUARTER, SPEED_EIGHTH };
    int cur = CurrentSpeed(), next = SPEED_NORMAL;
    for (int i = 0; i < 5; ++i)
        if (order[i] == cur) next = order[(i + 1) % 5];
    SetTimeSpeed(next);
}

// Frame advance: while the master timer is stopped (mode 0), NewFrame applies
// [+0x4c] milliseconds once when [+0x48] is set.
static void FrameStep()
{
    if (!g_timer) return;
    if (CurrentSpeed() != SPEED_STOP)
    {
        g_resumeSpeed = CurrentSpeed();
        SetTimeSpeed(SPEED_STOP);
        return;
    }
    static float carry;
    float ms = 1000.0f / (g_stepFps > 1.0f ? g_stepFps : 30.0f) + carry;
    uint32_t whole = (uint32_t)(ms + 0.5f);
    carry = ms - (float)whole;
    *reinterpret_cast<uint32_t*>(g_timer + 0x4c) = whole;
    *reinterpret_cast<uint8_t*>(g_timer + 0x48) = 1;
}

static uint8_t* CurrentCamera();
static bool FreeCameraOverridden();
static void ForceFreeCameraOff();
static int g_camCheck;                  // frames until we check that the free camera took over

// CameraScene: +0x78 CameraGroup** stack, +0x80 count (top = last); group name at +8,
// its selected camera at +0x50 (camera name at +0x20).
static void LogCameraGroups(const char* why)
{
    void** scenes = *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(g_loopScenes) + 8);
    uint8_t* scene = scenes ? static_cast<uint8_t*>(scenes[0]) : nullptr;
    if (!scene) return;
    uint8_t** groups = *reinterpret_cast<uint8_t***>(scene + 0x78);
    int count = *reinterpret_cast<int*>(scene + 0x80);
    char text[512] = "";
    for (int i = 0; groups && i < count && i < 16; ++i)
    {
        uint8_t* cam = groups[i] ? *reinterpret_cast<uint8_t**>(groups[i] + 0x50) : nullptr;
        _snprintf_s(text + strlen(text), sizeof(text) - strlen(text), _TRUNCATE, "%s%s/%s", i ? " < " : "",
                  groups[i] ? reinterpret_cast<const char*>(groups[i] + 8) : "?",
                  cam ? reinterpret_cast<const char*>(cam + 0x20) : "-");
    }
    Log("%s: camera groups (bottom..top) %s", why, text);
}

static void PrepareFreeCamera();
static bool FreeCameraActive();
static bool FreeCameraHasFocus();

static void FreeCamera(bool fullToggle)
{
    if (!g_ActivateFreeCamera || !g_loopScenes) return;
    // Another camera (a cutscene) took over while the free camera was on. The
    // game would push the free camera a second time; switch it off instead.
    if (FreeCameraOverridden()) { ForceFreeCameraOff(); return; }
    // Pushing the free camera over a cutscene camera crashes the game when the
    // cutscene ends (its FOV callback is left dangling).
    uint8_t* cam = CurrentCamera();
    if (cam && strcmp(reinterpret_cast<const char*>(cam + 0x20), "EyeCamera") == 0)
    {
        Log("free camera: not available during cutscenes");
        return;
    }
    // toggle=true : off -> on, on -> off
    // toggle=false: off -> on, on -> freeze camera & give control back, again -> off
    if (!FreeCameraActive() && !FreeCameraHasFocus()) PrepareFreeCamera();
    uint8_t* before = CurrentCamera();
    bool r = g_ActivateFreeCamera(g_loopScenes, 0, fullToggle);
    Log("ActivateFreeCamera(%d) -> %d (camera was %s)", fullToggle, r,
        before ? reinterpret_cast<const char*>(before + 0x20) : "?");
    if (r) g_camCheck = 5;
}

static const int kSpawnCount = (int)(sizeof(kSpawnDefs) / sizeof(kSpawnDefs[0]));

static char* Dup(const char* b, const char* e)
{
    while (b < e && (*b == ' ' || *b == '\t')) ++b;
    while (e > b && (e[-1] == ' ' || e[-1] == '\t')) --e;
    size_t n = (size_t)(e - b);
    char* s = new char[n + 1];          // kept alive: the actor may reference its parameters
    memcpy(s, b, n);
    s[n] = 0;
    return s;
}

static uint8_t* PlayerAvatar()
{
    if (!g_players || !*g_players) return nullptr;
    return *reinterpret_cast<uint8_t**>(*g_players + 0x1a0);
}

static void SpawnActor(int index)
{
    if (!g_ParamSet || !g_CreateActor || !g_ParamDtor || index < 0 || index >= kSpawnCount) return;
    uint8_t* avatar = PlayerAvatar();
    if (!avatar) { Log("spawn: no player avatar"); return; }
    const float* p = reinterpret_cast<const float*>(avatar + 0x80);

    // place it in front of the player, as seen from the camera
    float dx = 0.0f, dz = 1.0f;
    if (g_CurrentCamera && g_cameraDirector && *g_cameraDirector && g_loopScenes)
    {
        void** scenes = *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(g_loopScenes) + 8);
        uint8_t* cam = g_CurrentCamera(*g_cameraDirector, scenes ? scenes[0] : nullptr);
        if (cam)
        {
            float cx = *reinterpret_cast<float*>(cam + 0x190);
            float cz = *reinterpret_cast<float*>(cam + 0x198);
            float len = sqrtf((p[0] - cx) * (p[0] - cx) + (p[2] - cz) * (p[2] - cz));
            if (len > 0.001f) { dx = (p[0] - cx) / len; dz = (p[2] - cz) / len; }
        }
    }
    float pos[4] = { p[0] + dx * g_spawnDistance, p[1], p[2] + dz * g_spawnDistance, 0.0f };
    float heading = atan2f(-dx, -dz);    // face the player

    const SpawnDef& def = kSpawnDefs[index];
    alignas(16) uint8_t set[0x40] = {};  // ParameterSet is 0x18 bytes
    for (const char* s = def.params; *s;)
    {
        const char* end = strchr(s, ';');
        if (!end) end = s + strlen(s);
        const char* eq = static_cast<const char*>(memchr(s, '=', (size_t)(end - s)));
        if (eq) g_ParamSet(set, Dup(s, eq), Dup(eq + 1, end), false);
        s = *end ? end + 1 : end;
    }
    g_ParamSet(set, "name", def.name, false);
    void* actor = g_CreateActor(set, pos, heading, false, false);
    g_ParamDtor(set);
    Log("spawn %s at (%.1f %.1f %.1f) -> %p", def.name, pos[0], pos[1], pos[2], actor);
}

// ---------------------------------------------------------------------------
// Positions
// ---------------------------------------------------------------------------
static uint8_t* CurrentCamera()
{
    if (!g_CurrentCamera || !g_cameraDirector || !*g_cameraDirector || !g_loopScenes) return nullptr;
    void** scenes = *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(g_loopScenes) + 8);
    return g_CurrentCamera(*g_cameraDirector, scenes ? scenes[0] : nullptr);
}

static bool FreeCameraActive()
{
    uint8_t* cam = CurrentCamera();
    return cam && strcmp(reinterpret_cast<const char*>(cam + 0x20), "FreeCam") == 0;
}

// Player 0's controller focus is the free camera mapper (name at +0x134).
static bool FreeCameraHasFocus()
{
    if (!g_FocusCurrent || !g_focusMgr) return false;
    uint8_t* mapper = g_FocusCurrent(g_focusMgr, 0);
    return mapper && strcmp(reinterpret_cast<const char*>(mapper + 0x134), "FreeCam") == 0;
}

// The free camera holds the controller, but the scene shows another camera.
static bool FreeCameraOverridden()
{
    return g_FocusPop && g_PropStackRemove && FreeCameraHasFocus() && CurrentCamera() && !FreeCameraActive();
}

// ActivateFreeCamera's "off" path only runs while the free camera is the
// current camera. When a cutscene camera has taken over, undo the activation
// by hand: give the controller back and take the free camera's property set
// off the scene's stack, so nothing is left behind when the cutscene ends.
static void ForceFreeCameraOff()
{
    void** scenes = *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(g_loopScenes) + 8);
    if (!scenes || !scenes[0]) return;
    for (int i = 0; i < 4 && FreeCameraHasFocus(); ++i)
        g_FocusPop(g_focusMgr, g_FocusFind(g_focusMgr, "FreeCam", 0), 0);
    void* stack = *reinterpret_cast<void**>(static_cast<uint8_t*>(scenes[0]) + 0x258);
    int propSet = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(g_loopScenes) + 0x78);
    if (stack && propSet != -1)
        for (int i = 0; i < 4; ++i) g_PropStackRemove(stack, propSet);   // no-op once it is gone
    g_carry = false;
    Log("free camera: another camera took over, free camera switched off");
}

// Cutscenes leave state behind on scene 0's CameraPropertySetStack (+0x258):
// records (+0x28 list head, next at +0, priority at +0x10, set id at +0x14) whose
// property set resolves to "EyeCamera", the animated camera the cutscene drove.
// The game takes the camera from the first record that resolves for the scene,
// so switching the free camera on (or off) lands on the last cutscene frame.
// A property set maps scenes to cameras in a hash map: +0xbb0 buckets, +0xbc0
// bucket count, nodes { next, u32 hash at +8, scene at +0x10, camera at +0x18 }.
static uint8_t* SetCameraForUnsafe(uint8_t* set, uint8_t* scene)
{
    uint8_t** buckets = *reinterpret_cast<uint8_t***>(set + 0xbb0);
    uint32_t n = *reinterpret_cast<uint32_t*>(set + 0xbc0);
    if (!buckets || !n || !scene) return nullptr;
    uint64_t key = reinterpret_cast<uint64_t>(scene);
    uint32_t hash = static_cast<uint32_t>(key >> 32) ^ static_cast<uint32_t>(key);
    for (uint8_t* node = buckets[hash % n]; node; node = *reinterpret_cast<uint8_t**>(node))
        if (*reinterpret_cast<uint32_t*>(node + 8) == hash && *reinterpret_cast<uint8_t**>(node + 0x10) == scene)
            return *reinterpret_cast<uint8_t**>(node + 0x18);
    return nullptr;
}

static uint8_t* SetCameraFor(uint8_t* set, uint8_t* scene)
{
    __try { return SetCameraForUnsafe(set, scene); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static const char* CameraName(uint8_t* cam) { return cam ? reinterpret_cast<const char*>(cam + 0x20) : "-"; }

// Before switching the free camera on: rebuild it if its property set is gone or
// no longer leads to the FreeCam camera, and drop records that lead to EyeCamera.
static void PrepareFreeCamera()
{
    if (!g_propSetMgr || !*g_propSetMgr || !g_PropStackRemove) return;
    uint8_t* mgr = *g_propSetMgr;
    void** sets = *reinterpret_cast<void***>(mgr + 8);
    int setCount = *reinterpret_cast<int*>(mgr + 0x10);
    auto setOf = [&](int id) -> uint8_t* { return sets && id >= 0 && id < setCount ? static_cast<uint8_t*>(sets[id]) : nullptr; };

    void** scenes = *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(g_loopScenes) + 8);
    uint8_t* scene = scenes ? static_cast<uint8_t*>(scenes[0]) : nullptr;
    uint8_t* stack = scene ? *reinterpret_cast<uint8_t**>(scene + 0x258) : nullptr;
    if (!stack) return;
    uint8_t* key = *reinterpret_cast<uint8_t**>(stack + 0x50);
    if (!key) key = scene;

    int id = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(g_loopScenes) + 0x78);
    uint8_t* own = setOf(id);
    uint8_t* ownCam = own ? SetCameraFor(own, key) : nullptr;
    if ((!ownCam || strcmp(CameraName(ownCam), "FreeCam") != 0) && g_DeleteFreeCameras && g_CreateFreeCamerasFn)
    {
        Log("free camera: its camera set %d leads to %s, rebuilding the free camera", id, own ? CameraName(ownCam) : "(deleted)");
        g_DeleteFreeCameras(g_loopScenes);
        g_CreateFreeCamerasFn(g_loopScenes);
        id = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(g_loopScenes) + 0x78);
    }

    char dump[512] = "";
    int stale[32], n = 0;
    for (uint8_t* rec = *reinterpret_cast<uint8_t**>(stack + 0x28); rec && n < 32; rec = *reinterpret_cast<uint8_t**>(rec))
    {
        int recId = *reinterpret_cast<int*>(rec + 0x14);
        uint8_t* set = setOf(recId);
        uint8_t* cam = set ? SetCameraFor(set, key) : nullptr;
        _snprintf_s(dump + strlen(dump), sizeof(dump) - strlen(dump), _TRUNCATE, "%s[%d prio %d -> %s]", dump[0] ? " " : "", recId,
                  *reinterpret_cast<int*>(rec + 0x10), set ? CameraName(cam) : "(no set)");
        if (recId != id && cam && strcmp(CameraName(cam), "EyeCamera") == 0) stale[n++] = recId;
    }
    Log("free camera: camera stack (top first) %s", dump);
    for (int i = 0; i < n; ++i) g_PropStackRemove(stack, stale[i]);
    if (n) Log("free camera: removed %d records left behind by cutscenes", n);
}

// Same steps as the game's ActorSetPosition script command.
static bool SetActorPosition(uint8_t* actor, const float* p)
{
    if (!actor || !g_UpdateState) return false;
    float* pos = reinterpret_cast<float*>(actor + 0x80);
    pos[0] = p[0]; pos[1] = p[1]; pos[2] = p[2];
    g_UpdateState(actor + 0x50);
    uint8_t* vtbl = *reinterpret_cast<uint8_t**>(actor);
    reinterpret_cast<void(__fastcall*)(void*, int)>(*reinterpret_cast<void**>(vtbl + g_posVfuncA))(actor, 0);
    reinterpret_cast<void(__fastcall*)(void*)>(*reinterpret_cast<void**>(vtbl + g_posVfuncB))(actor);
    return true;
}

static void SaveSlotToIni(int i)
{
    char key[16], val[96];
    sprintf_s(key, "Slot%d", i + 1);
    sprintf_s(val, "%.3f %.3f %.3f", g_slots[i].p[0], g_slots[i].p[1], g_slots[i].p[2]);
    WritePrivateProfileStringA("Positions", key, val, g_iniPath);
}

static void SavePosition(int i)
{
    uint8_t* avatar = PlayerAvatar();
    if (!avatar) return;
    memcpy(g_slots[i].p, avatar + 0x80, sizeof(g_slots[i].p));
    g_slots[i].valid = true;
    SaveSlotToIni(i);
    Log("position %d saved: %.2f %.2f %.2f", i + 1, g_slots[i].p[0], g_slots[i].p[1], g_slots[i].p[2]);
    Voice("cheat_on");
}

static void LoadPosition(int i)
{
    if (!g_slots[i].valid) { Log("position %d is empty", i + 1); return; }
    if (SetActorPosition(PlayerAvatar(), g_slots[i].p))
        Log("position %d loaded", i + 1);
}

static void TeleportToCamera()
{
    uint8_t* cam = CurrentCamera();
    if (!cam || !FreeCameraActive()) { Log("teleport: free camera is not active"); return; }
    float p[3] = { *reinterpret_cast<float*>(cam + 0x190), *reinterpret_cast<float*>(cam + 0x194),
                   *reinterpret_cast<float*>(cam + 0x198) };
    if (SetActorPosition(PlayerAvatar(), p))
    {
        Log("teleported to camera: %.2f %.2f %.2f", p[0], p[1], p[2]);
        g_carry = false;
        FreeCamera(true);               // back to the player
    }
}

// Fly with the character: while the free camera is active the player is kept
// just below it, so it travels with the camera and drops where you leave it.
static void UpdateCarry()
{
    if (!g_carry) return;
    uint8_t* cam = CurrentCamera();
    if (!cam || !FreeCameraActive()) return;
    float p[3] = { *reinterpret_cast<float*>(cam + 0x190), *reinterpret_cast<float*>(cam + 0x194) - g_carryDrop,
                   *reinterpret_cast<float*>(cam + 0x198) };
    SetActorPosition(PlayerAvatar(), p);
}

static void ToggleCarry()
{
    g_carry = !g_carry;
    if (g_carry && !FreeCameraActive()) FreeCamera(true);
    Log("fly with camera -> %s", g_carry ? "ON" : "OFF");
}

// ---------------------------------------------------------------------------
// Command queue: the menu window (own thread) posts, the game thread executes
// ---------------------------------------------------------------------------
enum Command
{
    CMD_FREECAM = 1, CMD_DETACH, CMD_GOD, CMD_STEALTH, CMD_AMMO,
    CMD_PAUSE, CMD_STEP, CMD_SPAWN, CMD_TO_CAMERA, CMD_CARRY,
    CMD_SAVE = 200,                     // CMD_SAVE + slot
    CMD_LOAD = 210,                     // CMD_LOAD + slot
    CMD_SPEED = 100,                    // CMD_SPEED + TimeSpeed
};

static SRWLOCK g_queueLock = SRWLOCK_INIT;
static int     g_queue[64];
static int     g_queueLen;

static void Post(int cmd)
{
    AcquireSRWLockExclusive(&g_queueLock);
    if (g_queueLen < 64) g_queue[g_queueLen++] = cmd;
    ReleaseSRWLockExclusive(&g_queueLock);
}

static void Execute(int cmd)
{
    switch (cmd)
    {
    case CMD_FREECAM: FreeCamera(true); break;
    case CMD_DETACH:  FreeCamera(false); break;
    case CMD_GOD:     ToggleGod(); break;
    case CMD_STEALTH: ToggleCheat("Stealth"); break;
    case CMD_AMMO:    ToggleAmmo(); break;
    case CMD_PAUSE:   TogglePause(); break;
    case CMD_STEP:    FrameStep(); break;
    case CMD_SPAWN:   SpawnActor(g_spawnIndex); break;
    case CMD_TO_CAMERA: TeleportToCamera(); break;
    case CMD_CARRY:   ToggleCarry(); break;
    default:
        if (cmd >= CMD_LOAD && cmd < CMD_LOAD + 3) LoadPosition(cmd - CMD_LOAD);
        else if (cmd >= CMD_SAVE && cmd < CMD_SAVE + 3) SavePosition(cmd - CMD_SAVE);
        else if (cmd >= CMD_SPEED && cmd < CMD_SAVE) SetTimeSpeed(cmd - CMD_SPEED);
    }
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
static void __fastcall Daemon_Hook()
{
    g_lastDaemonTick = GetTickCount();
    ApplyGod();
    // AI perception only honours Stealth for the actor whose
    // handle is in Cheats::s_avatar. The retail game never sets it.
    if (g_cheatAvatar)
    {
        uint8_t* avatar = PlayerAvatar();
        bool needed = CheatEnabled("Stealth");
        *g_cheatAvatar = (needed && avatar) ? *reinterpret_cast<uint64_t*>(avatar + 0x38) : 0;
    }

    static KeyEdges keys;
    if (GameHasFocus() && !g_capturing)
    {
        if (keys.Pressed(g_keys.freeCam))   Post(CMD_FREECAM);
        if (keys.Pressed(g_keys.detach))    Post(CMD_DETACH);
        if (keys.Pressed(g_keys.god))       Post(CMD_GOD);
        if (keys.Pressed(g_keys.stealth))   Post(CMD_STEALTH);
        if (keys.Pressed(g_keys.ammo))      Post(CMD_AMMO);
        if (keys.Pressed(g_keys.pause))     Post(CMD_PAUSE);
        if (keys.Pressed(g_keys.timeCycle)) CycleSpeed();
        if (keys.Pressed(g_keys.frameStep)) Post(CMD_STEP);
        if (keys.Pressed(g_keys.spawn))     Post(CMD_SPAWN);
        if (keys.Pressed(g_keys.toCamera))  Post(CMD_TO_CAMERA);
        if (keys.Pressed(g_keys.carry))     Post(CMD_CARRY);
        for (int i = 0; i < 3; ++i)
            if (keys.Pressed(g_keys.slot[i])) Post((Down(g_keys.saveMod) ? CMD_SAVE : CMD_LOAD) + i);
    }

    int cmds[64], n;
    AcquireSRWLockExclusive(&g_queueLock);
    n = g_queueLen;
    memcpy(cmds, g_queue, n * sizeof(int));
    g_queueLen = 0;
    ReleaseSRWLockExclusive(&g_queueLock);
    for (int i = 0; i < n; ++i) Execute(cmds[i]);
    UpdateCarry();

    if (g_camCheck && --g_camCheck == 0 && !FreeCameraActive()) LogCameraGroups("free camera did not take over");

    g_DaemonOrig();
}

// CameraScene::FindGroup("FreeCam"): groups list at scene+0x60, name inline at +8, next at +0x68
static bool SceneHasFreeCam(uint8_t* scene)
{
    for (uint8_t* g = scene ? *reinterpret_cast<uint8_t**>(scene + 0x60) : nullptr; g; g = *reinterpret_cast<uint8_t**>(g + 0x68))
        if (strcmp(reinterpret_cast<const char*>(g + 8), "FreeCam") == 0) return true;
    return false;
}

// LoopScenes::CreateFreeCameras. Level loads after the first one call it twice:
// ActorLoop::Initialize (the scenes survive the previous level) and again from
// Players::Create. Level teardown removes only one free camera, so the leftover
// mapper is found first by ActivateFreeCamera and gets the controller focus
// while the camera reads the new mapper: player and camera are both frozen.
// Create the free cameras only once per scene set.
static void __fastcall CreateFreeCameras_Hook(uint8_t* loopScenes)
{
    int count = *reinterpret_cast<int*>(loopScenes);
    uint8_t** scenes = *reinterpret_cast<uint8_t***>(loopScenes + 8);
    for (int i = 0; scenes && i < count; ++i)
        if (SceneHasFreeCam(scenes[i]))
        {
            Log("CreateFreeCameras: free camera already exists, skipped");
            return;
        }
    g_CreateFreeCamerasOrig(loopScenes);
}

static float KeyAxis(int neg, int pos)
{
    return (Down(pos) ? 1.0f : 0.0f) - (Down(neg) ? 1.0f : 0.0f);
}

// FreeCamControlMapper::GetAxis(ANALOGAXIS): 0 strafe, 1 fore/aft, 2 yaw, 3 pitch
static float __fastcall GetAxis_Hook(void* self, int axis)
{
    float v = g_GetAxisOrig(self, axis);
    if (!g_keyboardFlight || g_capturing || !GameHasFocus()) return v;

    float k = 0.0f;
    switch (axis)
    {
    case 0: k = KeyAxis(g_keys.left, g_keys.right); break;
    case 1: k = KeyAxis(g_keys.back, g_keys.fwd) * (g_invertForward ? -1.0f : 1.0f); break;
    case 2: k = KeyAxis(g_keys.lookLeft, g_keys.lookRight) * g_lookSpeed; break;
    case 3: k = KeyAxis(g_keys.lookDown, g_keys.lookUp) * g_lookSpeed * (g_invertPitch ? -1.0f : 1.0f); break;
    }
    if (k != 0.0f && v == 0.0f) v = k;

    if (axis <= 1)
    {
        if (Down(g_keys.fast)) v *= g_fastMul;
        else if (Down(g_keys.slow)) v *= g_slowMul;
    }
    return v;
}

// ControlMapper::ButtonDown(int, bool) on the free-cam vtable only: 0 raise, 1 lower
static bool __fastcall ButtonDown_Hook(void* self, int button, bool raw)
{
    if (g_ButtonDownOrig(self, button, raw)) return true;
    if (!g_keyboardFlight || g_capturing || !GameHasFocus()) return false;
    if (button == 0) return Down(g_keys.up);
    if (button == 1) return Down(g_keys.down);
    return false;
}

// HealthIF methods receive the HealthIF subobject (HealthTracker + 0x18);
// the owning CActor* sits at HealthTracker + 8, i.e. this - 0x10.
static bool IsPlayerHealth(void* health)
{
    uint8_t* avatar = PlayerAvatar();
    if (!avatar || *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(health) - 0x10) != avatar) return false;
    g_playerHealth = static_cast<uint8_t*>(health);
    g_playerHealthOwner = avatar;
    return true;
}

// The HUD reads the player's health every frame: remember the tracker early.
template <int I>
static float __fastcall GetHealth_Hook(void* health)
{
    IsPlayerHealth(health);
    return g_GetHealthOrig[I](health);
}

template <int I>
static uint64_t __fastcall DoDamage_Hook(void* health, float amount, void* source, int reason, int type)
{
    if (!g_god || !IsPlayerHealth(health))
        return g_DoDamageOrig[I](health, amount, source, reason, type);

    uint8_t* h = static_cast<uint8_t*>(health);
    float maxHealth = MaxHealth(h);
    if (maxHealth > 0.0f)
    {
        if (Health(h) < maxHealth) SetHealth(h, maxHealth);
        if (amount >= maxHealth) amount = maxHealth * 0.5f;   // never lethal
    }
    uint64_t r = g_DoDamageOrig[I](health, amount, source, reason, type);
    ApplyGod();
    return r;
}

template <int I>
static uint64_t __fastcall Kill_Hook(void* health, void* source)
{
    if (g_god && g_godBlocksKill && IsPlayerHealth(health)) return 0;
    return g_KillOrig[I](health, source);
}

// ---------------------------------------------------------------------------
// Cutscene camera FOV
// ---------------------------------------------------------------------------
// A cutscene's AnimatedCameraNode binds a FOV callback (with itself as context) to
// camera float properties. After the cutscene the node is freed but the binding
// can stay behind; switching cameras then calls it and the game crashes. The
// float property getter (TypeProperty<float> vtable slot 1: +0x20 cached value,
// +0x28 context, +0x30 callback) drops a binding whose node is gone, so the
// cached value is used instead.
static FloatPropGet_t g_FloatPropGetOrig;
static void*          g_animFovGetter;  // AnimatedCameraNode FOV callback
static void**         g_animNodeVtbl;

static bool LiveAnimNode(uint8_t* node)
{
    __try
    {
        if (*reinterpret_cast<void***>(node) != g_animNodeVtbl) return false;
        uint64_t handle = *reinterpret_cast<uint64_t*>(node + 0x2b8);   // what the callback reads first
        return handle == 0 || (handle < 0x00007FFFFFFFFFFFull && (handle & 7) == 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static float __fastcall FloatPropGet_Hook(uint8_t* prop, void* arg)
{
    uint8_t* ctx = *reinterpret_cast<uint8_t**>(prop + 0x28);
    if (*reinterpret_cast<void**>(prop + 0x30) == g_animFovGetter && ctx && !LiveAnimNode(ctx))
    {
        *reinterpret_cast<void**>(prop + 0x28) = nullptr;
        *reinterpret_cast<void**>(prop + 0x30) = nullptr;
        Log("camera: dropped a FOV binding to a finished cutscene");
    }
    return g_FloatPropGetOrig(prop, arg);
}

static void HookCutsceneFov()
{
    // AnimatedCameraNode FOV callback: ...; call ...; mov rbp,rax; xor ebx,ebx; ...; mov rdi,[rdi+2B8h]
    g_animFovGetter = FindPattern("48 89 5C 24 10 55 56 57 48 83 EC 30 0F 29 74 24 20 48 8B F9 E8 ?? ?? ?? ?? "
                                  "48 8B E8 33 DB 48 89 5C 24 60 48 8B BF B8 02 00 00");
    g_animNodeVtbl = FindVtable(".?AVAnimatedCameraNode@Meridian@@");
    void** propVtbl = FindVtable(".?AV?$TypeProperty@M@@");
    // slot 1: push rbx; sub rsp,20h; mov rbx,rcx; test rdx,rdx
    bool ok = g_animFovGetter && g_animNodeVtbl && propVtbl &&
              memcmp(propVtbl[1], "\x40\x53\x48\x83\xEC\x20\x48\x8B\xD9\x48\x85\xD2", 12) == 0;
    if (ok) g_FloatPropGetOrig = reinterpret_cast<FloatPropGet_t>(PatchVtable(propVtbl, 1, reinterpret_cast<void*>(&FloatPropGet_Hook)));
    Log("cutscene FOV guard: %s (getter %p node vtbl %p)", ok ? "on" : "NOT installed", g_animFovGetter, g_animNodeVtbl);
}

// ---------------------------------------------------------------------------
// Infinite ammo
// ---------------------------------------------------------------------------
// The game's own Ammo cheat only covers Gun and Lobbed, and only for the actor in
// Cheats::s_avatar; the Blaster (Buzz's laser) ignores it. Instead, every
// weapon class's Update is hooked and the player's weapon is topped up.
enum AmmoKind { AMMO_COUNT, AMMO_RESERVE, AMMO_ENERGY };
struct AmmoClass
{
    const char* rtti;
    AmmoKind    kind;
    uint32_t    field;      // int ammo count / reserve, or float energy (0..1)
    Update_t    orig;
    void**      vtbl;
};
static AmmoClass g_ammoClasses[] =
{
    { ".?AVGun@@",           AMMO_COUNT,   0x3d8 },
    { ".?AVTS3ShooterGun@@", AMMO_COUNT,   0x3d8 },
    { ".?AVLobbed@@",        AMMO_COUNT,   0x3cc },   // thrown balls
    { ".?AVFirearm@@",       AMMO_RESERVE, 0x410 },   // clip +0x3d8 reloads from here
    { ".?AVBlaster@@",       AMMO_ENERGY,  0x408 },
};
static const int kAmmoClassCount = (int)(sizeof(g_ammoClasses) / sizeof(g_ammoClasses[0]));

static bool PlayerOwns(uint8_t* weapon)
{
    uint8_t* avatar = PlayerAvatar();
    return avatar && *reinterpret_cast<uint64_t*>(weapon + 0x18) == *reinterpret_cast<uint64_t*>(avatar + 0x38);
}

static void FillAmmo(uint8_t* weapon, const AmmoClass& c)
{
    if (c.kind == AMMO_ENERGY)
    {
        *reinterpret_cast<float*>(weapon + c.field) = 1.0f;
        return;
    }
    // MaxAmmo() is vtable slot 15
    int max = reinterpret_cast<int(__fastcall*)(void*)>((*reinterpret_cast<void***>(weapon))[15])(weapon);
    int* ammo = reinterpret_cast<int*>(weapon + c.field);
    if (max > 0 && *ammo >= 0 && *ammo < max) *ammo = max;   // -1 = unlimited, leave it
}

// A Weapon points to its properties at +0x10; the name is inline at +0x14.
static const char* WeaponName(uint8_t* weapon)
{
    uint8_t* props = weapon ? *reinterpret_cast<uint8_t**>(weapon + 0x10) : nullptr;
    return props ? reinterpret_cast<const char*>(props + 0x14) : nullptr;
}

template <int I>
static void __fastcall WeaponUpdate_Hook(uint8_t* weapon)
{
    g_ammoClasses[I].orig(weapon);
    if (!g_infiniteAmmo || !PlayerOwns(weapon)) return;
    const AmmoClass& c = g_ammoClasses[I];
    static uint8_t* seen[32];           // log each player weapon once
    for (int i = 0; i < 32; ++i)
    {
        if (seen[i] == weapon) break;
        if (seen[i]) continue;
        seen[i] = weapon;
        const char* name = WeaponName(weapon);
        Log("ammo: %s %s field %#x = %d", c.rtti, name ? name : "?", c.field,
            c.kind == AMMO_ENERGY ? (int)(*reinterpret_cast<float*>(weapon + c.field) * 100) : *reinterpret_cast<int*>(weapon + c.field));
        break;
    }
    FillAmmo(weapon, c);
}

template <int I> static void HookWeaponClass()
{
    AmmoClass& c = g_ammoClasses[I];
    void** vtbl = FindVtable(c.rtti);
    if (!vtbl) { Log("ammo: %s not found", c.rtti); return; }
    c.vtbl = vtbl;
    c.orig = reinterpret_cast<Update_t>(PatchVtable(vtbl, 36, reinterpret_cast<void*>(&WeaponUpdate_Hook<I>)));
}

static void HookWeapons()
{
    HookWeaponClass<0>(); HookWeaponClass<1>(); HookWeaponClass<2>(); HookWeaponClass<3>(); HookWeaponClass<4>();
    static_assert(kAmmoClassCount == 5, "one HookWeaponClass per entry");
}

// ---------------------------------------------------------------------------
// Key bindings (shared by the menu and the bindings window)
// ---------------------------------------------------------------------------
struct Binding { const char* ini; const char* label; int* key; bool modifier; };

static Binding g_bindings[] =
{
    { "Menu",             "Debug menu",                    &g_keys.menu },
    { "FreeCamera",       "Free camera",                   &g_keys.freeCam },
    { "FreeCameraDetach", "Freeze camera / return",        &g_keys.detach },
    { "God",              "God mode",                      &g_keys.god },
    { "Stealth",          "Stealth",                       &g_keys.stealth },
    { "Ammo",             "Infinite ammo",                 &g_keys.ammo },
    { "Pause",            "Pause / resume",                &g_keys.pause },
    { "TimeCycle",        "Game speed",                    &g_keys.timeCycle },
    { "FrameStep",        "Step frame",                    &g_keys.frameStep },
    { "PositionSlot1",    "Position slot 1",               &g_keys.slot[0] },
    { "PositionSlot2",    "Position slot 2",               &g_keys.slot[1] },
    { "PositionSlot3",    "Position slot 3",               &g_keys.slot[2] },
    { "SaveModifier",     "Save modifier (hold + slot)",   &g_keys.saveMod, true },
    { "PlayerToCamera",   "Player to camera",              &g_keys.toCamera },
    { "FlyWithCamera",    "Fly with camera",               &g_keys.carry },
    { "Spawn",            "Spawn actor",                   &g_keys.spawn },
    { "SpawnNext",        "Next actor",                    &g_keys.spawnNext },
    { "SpawnPrev",        "Previous actor",                &g_keys.spawnPrev },
    { "CamForward",       "Free camera: forward",          &g_keys.fwd },
    { "CamBack",          "Free camera: back",             &g_keys.back },
    { "CamLeft",          "Free camera: left",             &g_keys.left },
    { "CamRight",         "Free camera: right",            &g_keys.right },
    { "CamUp",            "Free camera: up",               &g_keys.up },
    { "CamDown",          "Free camera: down",             &g_keys.down },
    { "LookUp",           "Free camera: look up",          &g_keys.lookUp },
    { "LookDown",         "Free camera: look down",        &g_keys.lookDown },
    { "LookLeft",         "Free camera: look left",        &g_keys.lookLeft },
    { "LookRight",        "Free camera: look right",       &g_keys.lookRight },
    { "CamFast",          "Free camera: faster",           &g_keys.fast, true },
    { "CamSlow",          "Free camera: slower",           &g_keys.slow, true },
};
static const int kBindingCount = (int)(sizeof(g_bindings) / sizeof(g_bindings[0]));

static const char* KeyName(int vk)
{
    static char buf[8][48];
    static int next;
    char* out = buf[next++ & 7];
    if (vk <= 0) { strcpy_s(out, 48, "-"); return out; }
    if (vk >= VK_F1 && vk <= VK_F24) { sprintf_s(out, 48, "F%d", vk - VK_F1 + 1); return out; }
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    switch (vk)
    {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_RCONTROL: case VK_RMENU:
    case VK_DIVIDE: case VK_NUMLOCK:
        sc |= 0x100;                    // extended key
    }
    if (!sc || !GetKeyNameTextA(static_cast<LONG>(sc << 16), out, 48)) sprintf_s(out, 48, "0x%02X", vk);
    return out;
}

static void WriteBinding(const Binding& b)
{
    char val[16];
    if (*b.key >= VK_F1 && *b.key <= VK_F24) sprintf_s(val, "F%d", *b.key - VK_F1 + 1);
    else sprintf_s(val, "0x%02X", *b.key);
    WritePrivateProfileStringA("Keys", b.ini, val, g_iniPath);
}

// ---------------------------------------------------------------------------
// Debug menu window (runs on its own thread, never activates/steals focus)
// ---------------------------------------------------------------------------
enum
{
    ID_FREECAM = 1001, ID_DETACH, ID_GOD, ID_STEALTH, ID_AMMO,
    ID_PAUSE, ID_STEP, ID_SLOW8, ID_SLOW4, ID_SLOW2, ID_NORMAL, ID_FAST2,
    ID_LIST, ID_SPAWN, ID_STATUS,
    ID_SAVE1, ID_SAVE2, ID_SAVE3, ID_LOAD1, ID_LOAD2, ID_LOAD3, ID_TOCAM, ID_CARRY,
    ID_POSHINT, ID_KEYS,
    ID_BIND_LIST = 1101, ID_BIND_CHANGE, ID_BIND_CLOSE, ID_BIND_STATUS,
};

static HWND g_menu, g_list, g_status, g_chkGod, g_chkStealth, g_chkAmmo, g_chkCarry;
static HWND g_keysWnd, g_bindList, g_bindStatus;

// Layout is written in 96-DPI units and scaled to the system DPI.
static int   g_dpi = 96;
static HFONT g_font;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static HWND Control(HWND parent, const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_self, nullptr);
    SendMessageA(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font ? g_font : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return c;
}

static void SetText(HWND w, int id, const char* fmt, ...)
{
    char text[160];
    va_list ap;
    va_start(ap, fmt);
    vsprintf_s(text, fmt, ap);
    va_end(ap);
    SetDlgItemTextA(w, id, text);
}

// Button captions show the current key bindings.
static void UpdateMenuLabels()
{
    HWND w = g_menu;
    const Keys& k = g_keys;
    SetText(w, ID_FREECAM, "Free camera (%s)", KeyName(k.freeCam));
    SetText(w, ID_DETACH,  "Freeze / return (%s)", KeyName(k.detach));
    SetText(w, ID_GOD,     "God (%s)", KeyName(k.god));
    SetText(w, ID_STEALTH, "Stealth (%s)", KeyName(k.stealth));
    SetText(w, ID_AMMO,    "Ammo (%s)", KeyName(k.ammo));
    SetText(w, ID_PAUSE,   "Pause / resume (%s)", KeyName(k.pause));
    SetText(w, ID_STEP,    "Step frame (%s)", KeyName(k.frameStep));
    SetText(w, ID_POSHINT, "Load: %s / %s / %s     Save: %s + slot key", KeyName(k.slot[0]), KeyName(k.slot[1]),
            KeyName(k.slot[2]), KeyName(k.saveMod));
    SetText(w, ID_TOCAM,   "Player to camera (%s)", KeyName(k.toCamera));
    SetText(w, ID_CARRY,   "Fly with camera (%s)", KeyName(k.carry));
    SetText(w, ID_SPAWN,   "Spawn (%s / double-click)", KeyName(k.spawn));
}

// Client area: 360 x 676 (96-DPI units); every row spans 16..344.
static void BuildMenu(HWND w)
{
    const DWORD btn = BS_PUSHBUTTON, chk = BS_CHECKBOX, grp = BS_GROUPBOX;
    Control(w, "BUTTON", "Camera", grp, 8, 4, 344, 56, 0);
    Control(w, "BUTTON", "", btn, 16, 24, 160, 26, ID_FREECAM);
    Control(w, "BUTTON", "", btn, 184, 24, 160, 26, ID_DETACH);

    Control(w, "BUTTON", "Cheats", grp, 8, 64, 344, 50, 0);
    g_chkGod     = Control(w, "BUTTON", "", chk, 16, 84, 104, 22, ID_GOD);
    g_chkStealth = Control(w, "BUTTON", "", chk, 124, 84, 108, 22, ID_STEALTH);
    g_chkAmmo    = Control(w, "BUTTON", "", chk, 236, 84, 112, 22, ID_AMMO);

    Control(w, "BUTTON", "Time", grp, 8, 118, 344, 86, 0);
    Control(w, "BUTTON", "", btn, 16, 138, 160, 26, ID_PAUSE);
    Control(w, "BUTTON", "", btn, 184, 138, 160, 26, ID_STEP);
    Control(w, "BUTTON", "1/8x", btn, 16, 170, 62, 26, ID_SLOW8);
    Control(w, "BUTTON", "1/4x", btn, 82, 170, 62, 26, ID_SLOW4);
    Control(w, "BUTTON", "1/2x", btn, 148, 170, 62, 26, ID_SLOW2);
    Control(w, "BUTTON", "1x", btn, 214, 170, 62, 26, ID_NORMAL);
    Control(w, "BUTTON", "2x", btn, 280, 170, 64, 26, ID_FAST2);

    Control(w, "BUTTON", "Position", grp, 8, 208, 344, 140, 0);
    Control(w, "BUTTON", "Save 1", btn, 16, 228, 105, 26, ID_SAVE1);
    Control(w, "BUTTON", "Save 2", btn, 127, 228, 106, 26, ID_SAVE2);
    Control(w, "BUTTON", "Save 3", btn, 239, 228, 105, 26, ID_SAVE3);
    Control(w, "BUTTON", "Load 1", btn, 16, 258, 105, 26, ID_LOAD1);
    Control(w, "BUTTON", "Load 2", btn, 127, 258, 106, 26, ID_LOAD2);
    Control(w, "BUTTON", "Load 3", btn, 239, 258, 105, 26, ID_LOAD3);
    Control(w, "BUTTON", "", btn, 16, 290, 160, 26, ID_TOCAM);
    g_chkCarry = Control(w, "BUTTON", "", chk, 184, 292, 164, 22, ID_CARRY);
    Control(w, "STATIC", "", SS_LEFT | SS_ENDELLIPSIS, 16, 322, 330, 18, ID_POSHINT);

    Control(w, "BUTTON", "Spawn actor", grp, 8, 352, 344, 262, 0);
    g_list = Control(w, "LISTBOX", "", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                     16, 372, 328, 200, ID_LIST);
    for (int i = 0; i < kSpawnCount; ++i) SendMessageA(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kSpawnDefs[i].name));
    SendMessageA(g_list, LB_SETCURSEL, g_spawnIndex, 0);
    Control(w, "BUTTON", "", btn, 16, 578, 328, 28, ID_SPAWN);

    Control(w, "BUTTON", "Key bindings...", btn, 8, 620, 344, 28, ID_KEYS);
    g_status = Control(w, "STATIC", "", SS_LEFT | SS_ENDELLIPSIS, 10, 654, 340, 18, ID_STATUS);
    UpdateMenuLabels();
}

static void RefreshMenu()
{
    bool inLevel = GetTickCount() - g_lastDaemonTick < 1000;
    SendMessageA(g_chkGod, BM_SETCHECK, g_god ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(g_chkStealth, BM_SETCHECK, CheatEnabled("Stealth") ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(g_chkAmmo, BM_SETCHECK, g_infiniteAmmo ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(g_chkCarry, BM_SETCHECK, g_carry ? BST_CHECKED : BST_UNCHECKED, 0);

    const char* speed = "1x";
    switch (CurrentSpeed())
    {
    case SPEED_STOP:    speed = "paused"; break;
    case SPEED_EIGHTH:  speed = "1/8x"; break;
    case SPEED_QUARTER: speed = "1/4x"; break;
    case SPEED_HALF:    speed = "1/2x"; break;
    case SPEED_DOUBLE:  speed = "2x"; break;
    }
    char text[128];
    if (inLevel) sprintf_s(text, "In level  |  time: %s  |  %s hides", speed, KeyName(g_keys.menu));
    else         sprintf_s(text, "Not in a level - actions wait  |  %s hides", KeyName(g_keys.menu));
    char old[128];
    GetWindowTextA(g_status, old, sizeof(old));
    if (strcmp(old, text) != 0) SetWindowTextA(g_status, text);
}

static void PlaceNearGame(HWND w, int dx, int dy)
{
    HWND game = GetForegroundWindow();
    RECT r;
    if (game && game != g_menu && game != g_keysWnd && GetWindowRect(game, &r))
        SetWindowPos(w, HWND_TOPMOST, r.left + dx, r.top + dy, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

static void ToggleMenu()
{
    if (IsWindowVisible(g_menu)) { ShowWindow(g_menu, SW_HIDE); ShowWindow(g_keysWnd, SW_HIDE); return; }
    PlaceNearGame(g_menu, 24, 48);
    ShowWindow(g_menu, SW_SHOWNOACTIVATE);
}

// --- key bindings window ----------------------------------------------------
static int  g_captureIndex = -1;
static int  g_captureStage;             // 1 wait for release, 2 wait for key, 3 wait for release again
static bool g_captureHeld[256];

static void FillBindingList()
{
    LRESULT sel = SendMessageA(g_bindList, LB_GETCURSEL, 0, 0);
    SendMessageA(g_bindList, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < kBindingCount; ++i)
    {
        char line[128];
        sprintf_s(line, "%s\t%s", g_bindings[i].label, KeyName(*g_bindings[i].key));
        SendMessageA(g_bindList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line));
    }
    SendMessageA(g_bindList, LB_SETCURSEL, sel >= 0 ? sel : 0, 0);
}

static bool AnyKeyDown()
{
    for (int vk = 8; vk < 255; ++vk)
        if (Down(vk)) return true;
    return false;
}

static void StartCapture()
{
    LRESULT sel = SendMessageA(g_bindList, LB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    g_captureIndex = (int)sel;
    g_captureStage = 1;
    g_capturing = true;
    char text[160];
    sprintf_s(text, "Press a key for \"%s\" (Esc cancels)", g_bindings[sel].label);
    SetWindowTextA(g_bindStatus, text);
}

static void UpdateCapture()
{
    if (g_captureStage == 1 && !AnyKeyDown()) g_captureStage = 2;
    else if (g_captureStage == 2)
    {
        for (int vk = 8; vk < 255; ++vk)
        {
            if (!Down(vk)) continue;
            Binding& b = g_bindings[g_captureIndex];
            if (vk == VK_ESCAPE)
                SetWindowTextA(g_bindStatus, "Cancelled");
            else
            {
                // generic Shift/Ctrl/Alt only make sense as modifiers; otherwise keep the left/right key
                if ((vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) && !b.modifier) continue;
                *b.key = vk;
                WriteBinding(b);
                char text[160];
                sprintf_s(text, "%s = %s (saved to ini)", b.label, KeyName(vk));
                SetWindowTextA(g_bindStatus, text);
                FillBindingList();
                UpdateMenuLabels();
            }
            g_captureStage = 3;
            break;
        }
    }
    else if (g_captureStage == 3 && !AnyKeyDown())
    {
        g_captureStage = 0;
        g_captureIndex = -1;
        g_capturing = false;
    }
}

static LRESULT CALLBACK KeysProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_TIMER:
        if (g_captureStage) UpdateCapture();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_BIND_CHANGE || (LOWORD(wp) == ID_BIND_LIST && HIWORD(wp) == LBN_DBLCLK)) StartCapture();
        if (LOWORD(wp) == ID_BIND_CLOSE) ShowWindow(w, SW_HIDE);
        return 0;
    case WM_CLOSE:
        ShowWindow(w, SW_HIDE);
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static void ShowKeysWindow()
{
    if (IsWindowVisible(g_keysWnd)) { ShowWindow(g_keysWnd, SW_HIDE); return; }
    FillBindingList();
    SetWindowTextA(g_bindStatus, "Pick an action, then Change key (or double-click)");
    RECT r;
    GetWindowRect(g_menu, &r);
    SetWindowPos(g_keysWnd, HWND_TOPMOST, r.right + 8, r.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(g_keysWnd, SW_SHOWNOACTIVATE);
}

static HWND CreateToolWindow(const char* cls, const char* title, WNDPROC proc, int w, int h)
{
    WNDCLASSA wc = {};
    wc.lpfnWndProc = proc;
    wc.hInstance = g_self;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    RegisterClassA(&wc);

    RECT r = { 0, 0, S(w), S(h) };
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    AdjustWindowRectEx(&r, style, FALSE, exStyle);
    return CreateWindowExA(exStyle, cls, title, style, 40, 60, r.right - r.left, r.bottom - r.top,
                           nullptr, nullptr, g_self, nullptr);
}

// --- main menu window ----------------------------------------------------------
static LRESULT CALLBACK MenuProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    static KeyEdges keys;
    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;           // clicking must not take focus from the game
    case WM_TIMER:
        if (GameHasFocus() && !g_capturing)
        {
            if (keys.Pressed(g_keys.menu)) ToggleMenu();
            int step = keys.Pressed(g_keys.spawnNext) ? 1 : keys.Pressed(g_keys.spawnPrev) ? -1 : 0;
            if (step)
            {
                g_spawnIndex = (g_spawnIndex + step + kSpawnCount) % kSpawnCount;
                SendMessageA(g_list, LB_SETCURSEL, g_spawnIndex, 0);
            }
        }
        if (IsWindowVisible(w)) RefreshMenu();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case ID_FREECAM: Post(CMD_FREECAM); break;
        case ID_DETACH:  Post(CMD_DETACH); break;
        case ID_GOD:     Post(CMD_GOD); break;
        case ID_STEALTH: Post(CMD_STEALTH); break;
        case ID_AMMO:    Post(CMD_AMMO); break;
        case ID_PAUSE:   Post(CMD_PAUSE); break;
        case ID_STEP:    Post(CMD_STEP); break;
        case ID_SLOW8:   Post(CMD_SPEED + SPEED_EIGHTH); break;
        case ID_SLOW4:   Post(CMD_SPEED + SPEED_QUARTER); break;
        case ID_SLOW2:   Post(CMD_SPEED + SPEED_HALF); break;
        case ID_NORMAL:  Post(CMD_SPEED + SPEED_NORMAL); break;
        case ID_FAST2:   Post(CMD_SPEED + SPEED_DOUBLE); break;
        case ID_SPAWN:   Post(CMD_SPAWN); break;
        case ID_SAVE1: case ID_SAVE2: case ID_SAVE3: Post(CMD_SAVE + LOWORD(wp) - ID_SAVE1); break;
        case ID_LOAD1: case ID_LOAD2: case ID_LOAD3: Post(CMD_LOAD + LOWORD(wp) - ID_LOAD1); break;
        case ID_TOCAM:   Post(CMD_TO_CAMERA); break;
        case ID_CARRY:   Post(CMD_CARRY); break;
        case ID_KEYS:    ShowKeysWindow(); break;
        case ID_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE || HIWORD(wp) == LBN_DBLCLK)
            {
                LRESULT sel = SendMessageA(g_list, LB_GETCURSEL, 0, 0);
                if (sel >= 0) g_spawnIndex = (LONG)sel;
                if (HIWORD(wp) == LBN_DBLCLK) Post(CMD_SPAWN);
            }
            break;
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(w, SW_HIDE);
        ShowWindow(g_keysWnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static DWORD WINAPI MenuThread(LPVOID)
{
    // Per-monitor DPI awareness for our windows only (the game keeps its own setting).
    using SetCtx_t = HANDLE(WINAPI*)(HANDLE);
    if (auto setCtx = reinterpret_cast<SetCtx_t>(GetProcAddress(GetModuleHandleA("user32.dll"), "SetThreadDpiAwarenessContext")))
        setCtx(reinterpret_cast<HANDLE>(-3));   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE
    using GetDpi_t = UINT(WINAPI*)();
    if (auto getDpi = reinterpret_cast<GetDpi_t>(GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForSystem")))
        g_dpi = static_cast<int>(getDpi());
    g_font = CreateFontA(-S(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");

    g_menu = CreateToolWindow("TS3DebugMenu", "TS3Debug", MenuProc, 360, 676);
    g_keysWnd = CreateToolWindow("TS3DebugKeys", "TS3Debug - key bindings", KeysProc, 380, 452);
    if (!g_menu || !g_keysWnd) { Log("menu window failed: %lu", GetLastError()); return 0; }
    BuildMenu(g_menu);

    g_bindList = Control(g_keysWnd, "LISTBOX", "", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_USETABSTOPS,
                         8, 8, 364, 360, ID_BIND_LIST);
    int tab = 150;                      // dialog units: key column
    SendMessageA(g_bindList, LB_SETTABSTOPS, 1, reinterpret_cast<LPARAM>(&tab));
    Control(g_keysWnd, "BUTTON", "Change key", BS_PUSHBUTTON, 8, 376, 180, 28, ID_BIND_CHANGE);
    Control(g_keysWnd, "BUTTON", "Close", BS_PUSHBUTTON, 192, 376, 180, 28, ID_BIND_CLOSE);
    g_bindStatus = Control(g_keysWnd, "STATIC", "", SS_LEFT, 8, 412, 364, 36, ID_BIND_STATUS);

    SetTimer(g_menu, 1, 50, nullptr);
    SetTimer(g_keysWnd, 1, 30, nullptr);
    if (g_menuOnStart) ShowWindow(g_menu, SW_SHOWNOACTIVATE);

    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static int IniKey(const char* name, int def)
{
    char buf[32];
    GetPrivateProfileStringA("Keys", name, "", buf, sizeof(buf), g_iniPath);
    if (!buf[0]) return def;
    if ((buf[0] == 'F' || buf[0] == 'f') && buf[1] >= '1' && buf[1] <= '9')
        return VK_F1 + atoi(buf + 1) - 1;
    if (buf[1] == 0) { char c = buf[0]; if (c >= 'a' && c <= 'z') c -= 32; return c; }
    return (int)strtol(buf, nullptr, 0);   // e.g. 0x20
}

static float IniFloat(const char* sec, const char* name, float def)
{
    char buf[32];
    GetPrivateProfileStringA(sec, name, "", buf, sizeof(buf), g_iniPath);
    return buf[0] ? (float)atof(buf) : def;
}

static bool IniBool(const char* sec, const char* name, bool def)
{
    return GetPrivateProfileIntA(sec, name, def, g_iniPath) != 0;
}

static void LoadConfig()
{
    Keys& k = g_keys;
    k.freeCam   = IniKey("FreeCamera", k.freeCam);
    k.detach    = IniKey("FreeCameraDetach", k.detach);
    k.god       = IniKey("God", k.god);
    k.stealth   = IniKey("Stealth", k.stealth);
    k.ammo      = IniKey("Ammo", k.ammo);
    k.pause     = IniKey("Pause", k.pause);
    k.timeCycle = IniKey("TimeCycle", k.timeCycle);
    k.frameStep = IniKey("FrameStep", k.frameStep);
    k.menu      = IniKey("Menu", k.menu);
    k.spawn     = IniKey("Spawn", k.spawn);
    k.spawnNext = IniKey("SpawnNext", k.spawnNext);
    k.spawnPrev = IniKey("SpawnPrev", k.spawnPrev);
    k.slot[0]   = IniKey("PositionSlot1", k.slot[0]);
    k.slot[1]   = IniKey("PositionSlot2", k.slot[1]);
    k.slot[2]   = IniKey("PositionSlot3", k.slot[2]);
    k.toCamera  = IniKey("PlayerToCamera", k.toCamera);
    k.carry     = IniKey("FlyWithCamera", k.carry);
    k.saveMod   = IniKey("SaveModifier", k.saveMod);
    k.fwd       = IniKey("CamForward", k.fwd);
    k.back      = IniKey("CamBack", k.back);
    k.left      = IniKey("CamLeft", k.left);
    k.right     = IniKey("CamRight", k.right);
    k.up        = IniKey("CamUp", k.up);
    k.down      = IniKey("CamDown", k.down);
    k.lookUp    = IniKey("LookUp", k.lookUp);
    k.lookDown  = IniKey("LookDown", k.lookDown);
    k.lookLeft  = IniKey("LookLeft", k.lookLeft);
    k.lookRight = IniKey("LookRight", k.lookRight);
    k.fast      = IniKey("CamFast", k.fast);
    k.slow      = IniKey("CamSlow", k.slow);

    g_fastMul        = IniFloat("FreeCamera", "FastMultiplier", g_fastMul);
    g_slowMul        = IniFloat("FreeCamera", "SlowMultiplier", g_slowMul);
    g_lookSpeed      = IniFloat("FreeCamera", "LookSpeed", g_lookSpeed);
    g_invertForward  = IniBool("FreeCamera", "InvertForward", g_invertForward);
    g_invertPitch    = IniBool("FreeCamera", "InvertPitch", g_invertPitch);
    g_keyboardFlight = IniBool("FreeCamera", "KeyboardControl", g_keyboardFlight);
    g_voiceFeedback  = IniBool("General", "VoiceFeedback", g_voiceFeedback);
    g_menuOnStart    = IniBool("General", "ShowMenuOnStart", g_menuOnStart);
    g_stepFps        = IniFloat("General", "FrameStepFps", g_stepFps);
    g_spawnDistance  = IniFloat("General", "SpawnDistance", g_spawnDistance);
    g_godBlocksKill  = IniBool("General", "GodBlocksInstantKill", g_godBlocksKill);
    g_carryDrop      = IniFloat("General", "FlyHeightBelowCamera", g_carryDrop);
    for (int i = 0; i < 3; ++i)
    {
        char key[16], val[96];
        sprintf_s(key, "Slot%d", i + 1);
        GetPrivateProfileStringA("Positions", key, "", val, sizeof(val), g_iniPath);
        g_slots[i].valid = sscanf_s(val, "%f %f %f", &g_slots[i].p[0], &g_slots[i].p[1], &g_slots[i].p[2]) == 3;
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
static bool Init()
{
    g_base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
    g_size = nt->OptionalHeader.SizeOfImage;

    // LoopScenes::ActivateFreeCamera(int, bool)
    g_ActivateFreeCamera = reinterpret_cast<ActivateFreeCamera_t>(FindPattern(
        "48 89 5C 24 20 55 56 57 48 83 EC 20 48 63 FA 41 0F B6 D8 48 8B F1 85 D2 0F 88"));

    // BaseLoop::Initialize: lea rcx,s_Scenes; call CreateFreeCameras; xor r8d,r8d; xor edx,edx; lea rcx,s_Scenes; call ActivateFreeCamera
    uint8_t* createFreeCameras = nullptr;
    if (uint8_t* p = FindPattern("48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 45 33 C0 33 D2 48 8D 0D ?? ?? ?? ?? E8"))
    {
        g_loopScenes = Rip(p, 3, 7);
        if (Rip(p + 24, 1, 5) != reinterpret_cast<uint8_t*>(g_ActivateFreeCamera)) g_loopScenes = nullptr;
        // prologue: push rbx/rbp/rsi/rdi/r14/r15; sub rsp,58h; movaps [rsp+40h],xmm6 (18 bytes, no rip-relative)
        createFreeCameras = Rip(p + 7, 1, 5);
        if (memcmp(createFreeCameras, "\x40\x53\x55\x56\x57\x41\x56\x41\x57\x48\x83\xEC\x58\x0F\x29\x74\x24\x40\x48\x8B\xF1", 21) != 0)
            createFreeCameras = nullptr;
    }

    // Cheats::Daemon
    g_daemon = FindPattern("48 89 5C 24 18 56 57 41 57 48 81 EC 80 00 00 00 48 8B 0D ?? ?? ?? ?? 4C 8D 3D");
    if (g_daemon)
    {
        g_cheatGuard = reinterpret_cast<void**>(Rip(g_daemon + 0x10, 3, 7));
        uint8_t* end = g_daemon + 0x400;
        if (uint8_t* p = FindPattern("39 1D ?? ?? ?? ?? 8B EB 0F 8E ?? ?? ?? ?? 0F 29 74 24 70 48 8D 3D", g_daemon, end))
        {
            g_cheatCount = reinterpret_cast<int32_t*>(Rip(p, 2, 6));
            g_cheats = reinterpret_cast<CheatInfo*>(Rip(p + 19, 3, 7) - 0x14);
        }
        if (uint8_t* p = FindPattern("48 8B 0D ?? ?? ?? ?? 48 81 C1 D8 01 00 00 84 C0 48 8B 01", g_daemon, end))
            g_voiceOwner = reinterpret_cast<uint8_t**>(Rip(p, 3, 7));
    }

    g_freeCamVtbl = FindVtable(".?AVFreeCamControlMapper@@");

    // AiPerception::Munition*: mov rcx, [Cheats::s_avatar]; test rcx, rcx; jz; cmp [rax+0x38], rcx; jne
    if (uint8_t* p = FindPattern("48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 48 39 48 38 75"))
        g_cheatAvatar = reinterpret_cast<uint64_t*>(Rip(p, 3, 7));

    // BaseLoop::Initialize tail: call ActivateFreeCamera; lea rcx, g_timer; ...; jmp CMasterTimer::Reset
    if (uint8_t* p = FindPattern("45 33 C0 48 8D 0D ?? ?? ?? ?? 33 D2 E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 83 C4 20 5B E9"))
        g_timer = Rip(p + 17, 3, 7);
    // CMasterTimer::SetSpeed(ETimerSpeed, bool)
    g_SetSpeed = reinterpret_cast<SetSpeed_t>(FindPattern(
        "48 89 74 24 10 57 48 83 EC 30 F3 0F 10 0D ?? ?? ?? ?? 48 8B F9 8B 49 3C"));
    // time cheat callback: cmp [s_timeSpeed], eax; mov byte [changed], 1; cmove eax, r8d; cmp eax, r8d; mov [s_timeSpeed], eax
    if (uint8_t* p = FindPattern("39 05 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01 41 0F 44 C0 41 3B C0 89 05"))
        g_timeSpeedVar = reinterpret_cast<int32_t*>(Rip(p, 2, 6));

    // ActivateFreeCamera: mov rcx, [CameraDirector]; ...; call CameraDirector::CurrentCamera
    if (g_ActivateFreeCamera)
    {
        uint8_t* f = reinterpret_cast<uint8_t*>(g_ActivateFreeCamera);
        if (f[0x7b] == 0x48 && f[0x7c] == 0x8B && f[0x7d] == 0x0D && f[0x86] == 0xE8)
        {
            g_cameraDirector = reinterpret_cast<void**>(Rip(f + 0x7b, 3, 7));
            g_CurrentCamera = reinterpret_cast<CurrentCamera_t>(Rip(f + 0x86, 1, 5));
        }
        // lea rcx, ControllerFocusManager; call Current | call Find | call Pop | mov rcx,[scene+258h]; call CameraPropertySetStack::Remove
        if (f[0x42] == 0x48 && f[0x43] == 0x8D && f[0x44] == 0x0D && f[0x49] == 0xE8 && f[0xd5] == 0xE8 && f[0x183] == 0xE8 &&
            memcmp(f + 0x1ba, "\x48\x8B\x89\x58\x02\x00\x00\xE8", 8) == 0)
        {
            g_focusMgr       = Rip(f + 0x42, 3, 7);
            g_FocusCurrent   = reinterpret_cast<FocusCurrent_t>(Rip(f + 0x49, 1, 5));
            g_FocusFind      = reinterpret_cast<FocusFind_t>(Rip(f + 0xd5, 1, 5));
            g_FocusPop       = reinterpret_cast<FocusPop_t>(Rip(f + 0x183, 1, 5));
            g_PropStackRemove = reinterpret_cast<PropStackRemove_t>(Rip(f + 0x1c1, 1, 5));
        }
    }

    // mov rcx, [Players]; call Players::GetPlayerIdFromAvatar; cmp eax, -1
    if (uint8_t* p = FindPattern("48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 F8 FF 74 ?? 48 8B 0D ?? ?? ?? ?? 48 8D 15"))
        g_players = reinterpret_cast<uint8_t**>(Rip(p, 3, 7));

    g_ParamSet = reinterpret_cast<ParamSet_t>(FindPattern(
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 8B D8 48 8B FA 48 8B F1 45 84 C9 75"));
    g_ParamDtor = reinterpret_cast<ParamDtor_t>(FindPattern(
        "40 57 48 83 EC 20 48 8B 41 08 48 8B F9 48 85 C0 74 0B FF 48"));
    // a spawner calling CreateFromParameters(set, pos, 0.0f, false, false)
    if (uint8_t* p = FindPattern("C6 44 24 20 00 45 33 C9 0F 57 D2 48 8D 54 24 38 48 8D 4C 24 78 E8"))
        g_CreateActor = reinterpret_cast<CreateActor_t>(Rip(p + 21, 1, 5));

    // Script_ActorSetPosition: pop vector; pop actor; write actor+0x80; UpdateState(actor+0x50); vfunc A(0); vfunc B()
    if (uint8_t* p = FindPattern("40 53 48 83 EC 30 48 8D 54 24 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? "
                                 "48 8B D8 48 85 C0 74 ?? F3 0F 10 54 24 20 48 8D 48 50"))
    {
        if (p[0x48] == 0xE8 && p[0x52] == 0x48 && p[0x53] == 0x8B && p[0x54] == 0x81 && p[0x64] == 0xFF && p[0x65] == 0x90)
        {
            g_UpdateState = reinterpret_cast<UpdateState_t>(Rip(p + 0x48, 1, 5));
            g_posVfuncA = *reinterpret_cast<uint32_t*>(p + 0x55);
            g_posVfuncB = *reinterpret_cast<uint32_t*>(p + 0x66);
        }
    }
    Log("UpdateState=%p posVfunc=%#x/%#x", g_UpdateState, g_posVfuncA, g_posVfuncB);


    // LoopScenes::DeleteFreeCameras
    g_DeleteFreeCameras = reinterpret_cast<CreateFreeCameras_t>(FindPattern("48 89 5C 24 10 56 48 83 EC 20 33 F6 48 8B D9 39 31 0F 8E"));
    // CameraPropertySet::Create: mov rbx,[CameraPropertySetManager]; mov rbp,[rbx+10h]; mov rcx,[rbx+18h]; cmp rbp,rcx
    if (uint8_t* p = FindPattern("48 8B 1D ?? ?? ?? ?? 48 8B 6B 10 48 8B 4B 18 48 3B E9"))
        g_propSetMgr = reinterpret_cast<uint8_t**>(Rip(p, 3, 7));
    g_eyePropSetVtbl = FindVtable(".?AVEyeCameraPropertySet@@");
    Log("DeleteFreeCameras=%p propSetMgr=%p eyeSetVtbl=%p", g_DeleteFreeCameras, g_propSetMgr, g_eyePropSetVtbl);

    // HealthIF subobject vtables (offset 0x18) of both health tracker classes
    void** healthVtbl[2] = { FindVtable(".?AVTS3HealthTracker@@", 0x18), FindVtable(".?AVHealthTracker@@", 0x18) };

    Log("base=%p ActivateFreeCamera=%p s_Scenes=%p Daemon=%p cheats=%p count=%p guard=%p voice=%p FreeCamVtbl=%p",
        g_base, g_ActivateFreeCamera, g_loopScenes, g_daemon, g_cheats, g_cheatCount, g_cheatGuard, g_voiceOwner, g_freeCamVtbl);
    Log("cheatAvatar=%p", g_cheatAvatar);
    Log("timer=%p SetSpeed=%p timeSpeed=%p players=%p camDirector=%p CurrentCamera=%p ParamSet=%p ParamDtor=%p CreateActor=%p health=%p/%p",
        g_timer, g_SetSpeed, g_timeSpeedVar, g_players, g_cameraDirector, g_CurrentCamera, g_ParamSet, g_ParamDtor, g_CreateActor,
        healthVtbl[0], healthVtbl[1]);

    if (!g_daemon)
    {
        Log("ERROR: Cheats::Daemon not found - unsupported game version, plugin disabled");
        return false;
    }
    g_DaemonOrig = reinterpret_cast<Daemon_t>(HookEntry(g_daemon, 16, reinterpret_cast<void*>(&Daemon_Hook)));
    if (createFreeCameras)
        g_CreateFreeCamerasOrig = reinterpret_cast<CreateFreeCameras_t>(
            HookEntry(createFreeCameras, 18, reinterpret_cast<void*>(&CreateFreeCameras_Hook)));
    g_CreateFreeCamerasFn = g_CreateFreeCamerasOrig;
    Log("CreateFreeCameras=%p focus=%p/%p/%p/%p propRemove=%p", createFreeCameras,
        g_focusMgr, g_FocusCurrent, g_FocusFind, g_FocusPop, g_PropStackRemove);

    if (g_freeCamVtbl)
    {
        g_ButtonDownOrig = reinterpret_cast<ButtonDown_t>(PatchVtable(g_freeCamVtbl, 6, reinterpret_cast<void*>(&ButtonDown_Hook)));
        g_GetAxisOrig    = reinterpret_cast<GetAxis_t>(PatchVtable(g_freeCamVtbl, 12, reinterpret_cast<void*>(&GetAxis_Hook)));
    }
    // HealthIF slots: 9 GetHealth, 16 DoDamage, 18 Kill
    void* healthHooks[2]  = { reinterpret_cast<void*>(&GetHealth_Hook<0>), reinterpret_cast<void*>(&GetHealth_Hook<1>) };
    void* damageHooks[2]  = { reinterpret_cast<void*>(&DoDamage_Hook<0>), reinterpret_cast<void*>(&DoDamage_Hook<1>) };
    void* killHooks[2]    = { reinterpret_cast<void*>(&Kill_Hook<0>), reinterpret_cast<void*>(&Kill_Hook<1>) };
    for (int i = 0; i < 2; ++i)
    {
        if (!healthVtbl[i] || !g_players) continue;
        g_GetHealthOrig[i]       = reinterpret_cast<GetHealth_t>(PatchVtable(healthVtbl[i], 9, healthHooks[i]));
        g_DoDamageOrig[i]        = reinterpret_cast<DoDamage_t>(PatchVtable(healthVtbl[i], 16, damageHooks[i]));
        g_KillOrig[i]            = reinterpret_cast<Kill_t>(PatchVtable(healthVtbl[i], 18, killHooks[i]));
    }
    HookWeapons();
    HookCutsceneFov();

    Log("hooks installed");
    return true;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        GetModuleFileNameA(mod, g_iniPath, MAX_PATH);
        strcpy_s(g_logPath, g_iniPath);
        if (char* dot = strrchr(g_iniPath, '.')) strcpy_s(dot, MAX_PATH - (dot - g_iniPath), ".ini");
        if (char* dot = strrchr(g_logPath, '.')) strcpy_s(dot, MAX_PATH - (dot - g_logPath), ".log");
        DeleteFileA(g_logPath);
        LoadConfig();
        if (Init())
            if (HANDLE t = CreateThread(nullptr, 0, MenuThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
