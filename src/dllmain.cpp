// TS3Debug.asi - debug features for Toy Story 3 Complete Edition (PC, Steam)
//
// Game code used:
//   LoopScenes::ActivateFreeCamera(int, bool)   -> free camera toggle
//   BaseLoop::s_Scenes                          -> LoopScenes instance
//   Cheats::Daemon()                            -> per-frame hook (main thread)
//   Cheats::s_cheats / Cheats::Info[16]         -> Stealth, Ammo flags
//   Cheats::s_avatar                            -> who Stealth/Ammo apply to (never set by the game)
//   CMasterTimer::SetSpeed / g_timer            -> pause, game speed, frame advance
//   HealthIF vtable (GetHealth, SetHealth, DoDamage, Kill) -> god mode
//   FreeCamControlMapper vtable (GetAxis, ButtonDown) -> keyboard flight
//   ParameterSet + CActorCreator::CreateFromParameters -> actor spawner
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
using Daemon_t             = void (__fastcall*)();
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

static volatile bool g_god;
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

static void FreeCamera(bool fullToggle)
{
    if (!g_ActivateFreeCamera || !g_loopScenes) return;
    // toggle=true : off -> on, on -> off
    // toggle=false: off -> on, on -> freeze camera & give control back, again -> off
    bool r = g_ActivateFreeCamera(g_loopScenes, 0, fullToggle);
    Log("ActivateFreeCamera(%d) -> %d", fullToggle, r);
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
// Command queue: the menu window (own thread) posts, the game thread executes
// ---------------------------------------------------------------------------
enum Command
{
    CMD_FREECAM = 1, CMD_DETACH, CMD_GOD, CMD_STEALTH, CMD_AMMO,
    CMD_PAUSE, CMD_STEP, CMD_SPAWN,
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
    case CMD_AMMO:    ToggleCheat("Ammo"); break;
    case CMD_PAUSE:   TogglePause(); break;
    case CMD_STEP:    FrameStep(); break;
    case CMD_SPAWN:   SpawnActor(g_spawnIndex); break;
    default:
        if (cmd >= CMD_SPEED) SetTimeSpeed(cmd - CMD_SPEED);
    }
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
static void __fastcall Daemon_Hook()
{
    g_lastDaemonTick = GetTickCount();
    ApplyGod();
    // AI perception and Gun::UseAmmo only honour Stealth/Ammo for the actor whose
    // handle is in Cheats::s_avatar. The retail game never sets it.
    if (g_cheatAvatar)
    {
        uint8_t* avatar = PlayerAvatar();
        bool needed = CheatEnabled("Stealth") || CheatEnabled("Ammo");
        *g_cheatAvatar = (needed && avatar) ? *reinterpret_cast<uint64_t*>(avatar + 0x38) : 0;
    }

    static KeyEdges keys;
    if (GameHasFocus())
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
    }

    int cmds[64], n;
    AcquireSRWLockExclusive(&g_queueLock);
    n = g_queueLen;
    memcpy(cmds, g_queue, n * sizeof(int));
    g_queueLen = 0;
    ReleaseSRWLockExclusive(&g_queueLock);
    for (int i = 0; i < n; ++i) Execute(cmds[i]);

    g_DaemonOrig();
}

static float KeyAxis(int neg, int pos)
{
    return (Down(pos) ? 1.0f : 0.0f) - (Down(neg) ? 1.0f : 0.0f);
}

// FreeCamControlMapper::GetAxis(ANALOGAXIS): 0 strafe, 1 fore/aft, 2 yaw, 3 pitch
static float __fastcall GetAxis_Hook(void* self, int axis)
{
    float v = g_GetAxisOrig(self, axis);
    if (!g_keyboardFlight || !GameHasFocus()) return v;

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
    if (!g_keyboardFlight || !GameHasFocus()) return false;
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
// Debug menu window (runs on its own thread, never activates/steals focus)
// ---------------------------------------------------------------------------
enum
{
    ID_FREECAM = 1001, ID_DETACH, ID_GOD, ID_STEALTH, ID_AMMO,
    ID_PAUSE, ID_STEP, ID_SLOW8, ID_SLOW4, ID_SLOW2, ID_NORMAL, ID_FAST2,
    ID_LIST, ID_SPAWN, ID_STATUS,
};

static HWND g_menu, g_list, g_status, g_chkGod, g_chkStealth, g_chkAmmo;

static HWND Control(HWND parent, const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_self, nullptr);
    SendMessageA(c, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return c;
}

static void BuildMenu(HWND w)
{
    const DWORD btn = BS_PUSHBUTTON, chk = BS_CHECKBOX, grp = BS_GROUPBOX;
    Control(w, "BUTTON", "Camera", grp, 8, 4, 264, 50, 0);
    Control(w, "BUTTON", "Free camera (F1)", btn, 16, 22, 120, 24, ID_FREECAM);
    Control(w, "BUTTON", "Freeze / return (F2)", btn, 144, 22, 120, 24, ID_DETACH);

    Control(w, "BUTTON", "Cheats", grp, 8, 58, 264, 46, 0);
    g_chkGod     = Control(w, "BUTTON", "God (F3)", chk, 16, 76, 76, 20, ID_GOD);
    g_chkStealth = Control(w, "BUTTON", "Stealth (F4)", chk, 96, 76, 84, 20, ID_STEALTH);
    g_chkAmmo    = Control(w, "BUTTON", "Ammo (F5)", chk, 186, 76, 80, 20, ID_AMMO);

    Control(w, "BUTTON", "Time", grp, 8, 108, 264, 78, 0);
    Control(w, "BUTTON", "Pause / resume (F6)", btn, 16, 126, 120, 24, ID_PAUSE);
    Control(w, "BUTTON", "Step frame (F8)", btn, 144, 126, 120, 24, ID_STEP);
    Control(w, "BUTTON", "1/8", btn, 16, 154, 46, 24, ID_SLOW8);
    Control(w, "BUTTON", "1/4", btn, 66, 154, 46, 24, ID_SLOW4);
    Control(w, "BUTTON", "1/2", btn, 116, 154, 46, 24, ID_SLOW2);
    Control(w, "BUTTON", "1x", btn, 166, 154, 46, 24, ID_NORMAL);
    Control(w, "BUTTON", "2x", btn, 216, 154, 48, 24, ID_FAST2);

    Control(w, "BUTTON", "Spawn actor", grp, 8, 190, 264, 268, 0);
    g_list = Control(w, "LISTBOX", "", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                     16, 208, 248, 210, ID_LIST);
    for (int i = 0; i < kSpawnCount; ++i) SendMessageA(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kSpawnDefs[i].name));
    SendMessageA(g_list, LB_SETCURSEL, g_spawnIndex, 0);
    Control(w, "BUTTON", "Spawn (Insert / double-click)", btn, 16, 424, 248, 26, ID_SPAWN);

    g_status = Control(w, "STATIC", "", SS_LEFT, 10, 464, 260, 16, ID_STATUS);
}

static void RefreshMenu()
{
    bool inLevel = GetTickCount() - g_lastDaemonTick < 1000;
    SendMessageA(g_chkGod, BM_SETCHECK, g_god ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(g_chkStealth, BM_SETCHECK, CheatEnabled("Stealth") ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(g_chkAmmo, BM_SETCHECK, CheatEnabled("Ammo") ? BST_CHECKED : BST_UNCHECKED, 0);

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
    sprintf_s(text, inLevel ? "In level  |  time: %s  |  Home hides" : "Not in a level - actions wait  |  Home hides", speed);
    char old[128];
    GetWindowTextA(g_status, old, sizeof(old));
    if (strcmp(old, text) != 0) SetWindowTextA(g_status, text);
}

static void ToggleMenu()
{
    if (IsWindowVisible(g_menu)) { ShowWindow(g_menu, SW_HIDE); return; }
    HWND game = GetForegroundWindow();
    RECT r;
    if (game && game != g_menu && GetWindowRect(game, &r))
        SetWindowPos(g_menu, HWND_TOPMOST, r.left + 24, r.top + 48, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(g_menu, SW_SHOWNOACTIVATE);
}

static LRESULT CALLBACK MenuProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    static KeyEdges keys;
    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;           // clicking must not take focus from the game
    case WM_TIMER:
        if (GameHasFocus())
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
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static DWORD WINAPI MenuThread(LPVOID)
{
    WNDCLASSA wc = {};
    wc.lpfnWndProc = MenuProc;
    wc.hInstance = g_self;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = "TS3DebugMenu";
    RegisterClassA(&wc);

    RECT r = { 0, 0, 280, 486 };
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    AdjustWindowRectEx(&r, style, FALSE, exStyle);
    g_menu = CreateWindowExA(exStyle, wc.lpszClassName, "TS3Debug", style, 40, 60,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, g_self, nullptr);
    if (!g_menu) { Log("menu window failed: %lu", GetLastError()); return 0; }
    BuildMenu(g_menu);
    SetTimer(g_menu, 1, 50, nullptr);
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
    if (uint8_t* p = FindPattern("48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 45 33 C0 33 D2 48 8D 0D ?? ?? ?? ?? E8"))
    {
        g_loopScenes = Rip(p, 3, 7);
        if (Rip(p + 24, 1, 5) != reinterpret_cast<uint8_t*>(g_ActivateFreeCamera)) g_loopScenes = nullptr;
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
