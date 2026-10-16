// TS3Debug.asi - debug features for Toy Story 3 Complete Edition (PC, Steam)
//
// Game code used:
//   LoopScenes::ActivateFreeCamera(int, bool)   -> free camera toggle
//   BaseLoop::s_Scenes                          -> LoopScenes instance
//   Cheats::Daemon()                            -> per-frame hook (main thread)
//   Cheats::s_cheats / Cheats::Info[16]         -> God, Stealth, Ammo, Time...
//   ("Wireframe" and "Say Stress" are registered but compiled out of the retail
//    game: their callbacks are stubs, so they are not exposed here.)
//   FreeCamControlMapper vtable (GetAxis, ButtonDown) -> keyboard flight
// Everything is found by signature scan at runtime, nothing is hardcoded.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>

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

using ActivateFreeCamera_t = bool (__fastcall*)(void* loopScenes, int player, bool toggle);
using Daemon_t             = void (__fastcall*)();
using GetAxis_t            = float(__fastcall*)(void* mapper, int axis);
using ButtonDown_t         = bool (__fastcall*)(void* mapper, int button, bool raw);
using VoiceQueue_t         = void (__fastcall*)(void* queue, const char* name, float delay, bool b, float f);

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
static int32_t*   g_cheatCount;
static uint8_t**  g_voiceOwner;          // *g_voiceOwner + 0x1D8 = VoiceQueue
static void**     g_freeCamVtbl;
static GetAxis_t    g_GetAxisOrig;
static ButtonDown_t g_ButtonDownOrig;


// Config
struct Keys
{
    int freeCam      = VK_F1;
    int detach       = VK_F2;
    int god          = VK_F3;
    int stealth      = VK_F4;
    int ammo         = VK_F5;
    int unlockLevels = VK_F6;
    int stopTime     = VK_F7;
    int timeCycle    = VK_F8;
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

// Locate a class vtable through MSVC RTTI (.?AVName@@).
static void** FindVtable(const char* mangled)
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
        if (col[0] != 1 || col[1] != 0 || col[3] != tdRva || col[5] != (uint32_t)(p - g_base))
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

static bool Pressed(int vk)
{
    static bool prev[256];
    if (vk <= 0 || vk > 255) return false;
    bool now = Down(vk);
    bool edge = now && !prev[vk];
    prev[vk] = now;
    return edge;
}

// ---------------------------------------------------------------------------
// Game wrappers
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

// Same behaviour as Cheats::Toggle(Info&) / the code-entry path in Cheats::Daemon.
static void ToggleCheat(const char* name)
{
    CheatInfo* c = FindCheat(name);
    if (!c) { Log("cheat '%s' not registered (not in a level yet?)", name); return; }
    if (CheatsBlocked()) { Log("cheats blocked right now"); return; }
    bool on = c->callback ? c->callback(c) : !c->enabled;
    c->enabled = on;
    Log("cheat %s -> %s", c->name, on ? "ON" : "OFF");
    Voice(on ? "cheat_on" : "cheat_off");
}

static void SetCheat(const char* name, bool on)
{
    CheatInfo* c = FindCheat(name);
    if (c && c->enabled != on) ToggleCheat(name);
}

static void CycleTime()
{
    static const char* const steps[] = { "Time 2x", "Time 1/2x", "Time 1/4x", "Time 1/8x" };
    int current = -1;
    for (int i = 0; i < 4; ++i)
    {
        CheatInfo* c = FindCheat(steps[i]);
        if (c && c->enabled) current = i;
    }
    for (int i = 0; i < 4; ++i) SetCheat(steps[i], false);
    int next = current + 1;
    if (next < 4) SetCheat(steps[next], true);
    Log("time speed -> %s", next < 4 ? steps[next] : "normal");
}

static void FreeCamera(bool fullToggle)
{
    if (!g_ActivateFreeCamera || !g_loopScenes) return;
    // toggle=true : off -> on, on -> off
    // toggle=false: off -> on, on -> freeze camera & give control back, again -> off
    bool r = g_ActivateFreeCamera(g_loopScenes, 0, fullToggle);
    Log("ActivateFreeCamera(%d) -> %d", fullToggle, r);
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
static void __fastcall Daemon_Hook()
{
    if (GameHasFocus())
    {
        if (Pressed(g_keys.freeCam))      FreeCamera(true);
        if (Pressed(g_keys.detach))       FreeCamera(false);
        if (Pressed(g_keys.god))          ToggleCheat("God");
        if (Pressed(g_keys.stealth))      ToggleCheat("Stealth");
        if (Pressed(g_keys.ammo))         ToggleCheat("Ammo");
        if (Pressed(g_keys.unlockLevels)) ToggleCheat("Unlock levels");
        if (Pressed(g_keys.stopTime))     ToggleCheat("Stop Time");
        if (Pressed(g_keys.timeCycle))    CycleTime();
    }
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

static void LoadConfig()
{
    Keys& k = g_keys;
    k.freeCam      = IniKey("FreeCamera", k.freeCam);
    k.detach       = IniKey("FreeCameraDetach", k.detach);
    k.god          = IniKey("God", k.god);
    k.stealth      = IniKey("Stealth", k.stealth);
    k.ammo         = IniKey("Ammo", k.ammo);
    k.unlockLevels = IniKey("UnlockLevels", k.unlockLevels);
    k.stopTime     = IniKey("StopTime", k.stopTime);
    k.timeCycle    = IniKey("TimeCycle", k.timeCycle);
    k.fwd          = IniKey("CamForward", k.fwd);
    k.back         = IniKey("CamBack", k.back);
    k.left         = IniKey("CamLeft", k.left);
    k.right        = IniKey("CamRight", k.right);
    k.up           = IniKey("CamUp", k.up);
    k.down         = IniKey("CamDown", k.down);
    k.lookUp       = IniKey("LookUp", k.lookUp);
    k.lookDown     = IniKey("LookDown", k.lookDown);
    k.lookLeft     = IniKey("LookLeft", k.lookLeft);
    k.lookRight    = IniKey("LookRight", k.lookRight);
    k.fast         = IniKey("CamFast", k.fast);
    k.slow         = IniKey("CamSlow", k.slow);

    g_fastMul        = IniFloat("FreeCamera", "FastMultiplier", g_fastMul);
    g_slowMul        = IniFloat("FreeCamera", "SlowMultiplier", g_slowMul);
    g_lookSpeed      = IniFloat("FreeCamera", "LookSpeed", g_lookSpeed);
    g_invertForward  = GetPrivateProfileIntA("FreeCamera", "InvertForward", g_invertForward, g_iniPath) != 0;
    g_invertPitch    = GetPrivateProfileIntA("FreeCamera", "InvertPitch", g_invertPitch, g_iniPath) != 0;
    g_keyboardFlight = GetPrivateProfileIntA("FreeCamera", "KeyboardControl", g_keyboardFlight, g_iniPath) != 0;
    g_voiceFeedback  = GetPrivateProfileIntA("General", "VoiceFeedback", g_voiceFeedback, g_iniPath) != 0;
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

    Log("base=%p ActivateFreeCamera=%p s_Scenes=%p Daemon=%p cheats=%p count=%p guard=%p voice=%p FreeCamVtbl=%p",
        g_base, g_ActivateFreeCamera, g_loopScenes, g_daemon, g_cheats, g_cheatCount, g_cheatGuard, g_voiceOwner, g_freeCamVtbl);

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
        Init();
    }
    return TRUE;
}
