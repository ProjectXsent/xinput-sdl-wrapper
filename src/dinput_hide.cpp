#define DIRECTINPUT_VERSION 0x0800
#include "dinput_hide.h"
#include "log.h"

#include <windows.h>
#include <objbase.h>
#include <dinput.h>

#include <MinHook.h>

#include <cstdint>
#include <cstdio>

namespace dinput_hide {
namespace {

// IDirectInput8 vtable slots (IUnknown = 0..2)
constexpr int kVtCreateDevice = 3;
constexpr int kVtEnumDevices  = 4;
constexpr int kVtEnumDevicesBySemantics = 9;

// IID_IDirectInput8A / IID_IDirectInput8W (defined here to avoid linking dxguid)
const GUID kIID_DI8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
const GUID kIID_DI8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};

DWORD   g_tls = TLS_OUT_OF_INDEXES;
SRWLOCK g_lock = SRWLOCK_INIT;  // guards install state, hidden list and every MH_* call

bool g_mhInit = false;
bool g_createHooked = false;
bool g_hookedA = false, g_hookedW = false;

constexpr int kMaxHidden = 64;
GUID g_hidden[kMaxHidden];
int  g_hiddenCount = 0;

// ---- bypass ---------------------------------------------------------------

bool BypassActive() {
    if (g_tls == TLS_OUT_OF_INDEXES) return false;
    return TlsGetValue(g_tls) != nullptr;
}

// ---- helpers --------------------------------------------------------------

bool IsGameController(DWORD devType) {
    switch (LOBYTE(devType)) {
        case DI8DEVTYPE_JOYSTICK:
        case DI8DEVTYPE_GAMEPAD:
        case DI8DEVTYPE_DRIVING:
        case DI8DEVTYPE_FLIGHT:
        case DI8DEVTYPE_1STPERSON:
        case DI8DEVTYPE_SUPPLEMENTAL:
            return true;
        default:
            return false;
    }
}

struct Name { char s[160]; };
Name ToName(const char* a) {
    Name n;
    lstrcpynA(n.s, a ? a : "", (int)sizeof(n.s));
    return n;
}
Name ToName(const wchar_t* w) {
    Name n;
    n.s[0] = '\0';
    if (w && WideCharToMultiByte(CP_UTF8, 0, w, -1, n.s, (int)sizeof(n.s), nullptr, nullptr) == 0)
        n.s[sizeof(n.s) - 1] = '\0';
    return n;
}

struct GuidStr { char s[40]; };
GuidStr ToStr(const GUID& g) {
    GuidStr o;
    snprintf(o.s, sizeof(o.s), "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
             g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return o;
}

// Returns true if the GUID was not known yet.
bool RememberHidden(const GUID& g) {
    bool added = false;
    AcquireSRWLockExclusive(&g_lock);
    bool known = false;
    for (int i = 0; i < g_hiddenCount; ++i)
        if (IsEqualGUID(g_hidden[i], g)) { known = true; break; }
    if (!known && g_hiddenCount < kMaxHidden) {
        g_hidden[g_hiddenCount++] = g;
        added = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return added;
}

bool IsHidden(const GUID& g) {
    bool hit = false;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < g_hiddenCount; ++i)
        if (IsEqualGUID(g_hidden[i], g)) { hit = true; break; }
    ReleaseSRWLockShared(&g_lock);
    return hit;
}

// ---- EnumDevices ----------------------------------------------------------

// Diagnostics: the first few hooked DirectInput calls (and every device they present) are
// logged on entry and exit, so a crash log shows which call the game was inside.
volatile LONG g_traceCalls = 0, g_traceDevs = 0;
struct Trace {
    const char* what;
    bool        on;
    Trace(const char* w, DWORD a, DWORD b) : what(w), on(!BypassActive() && InterlockedIncrement(&g_traceCalls) <= 40) {
        if (on) LOGI("DirectInput: > %s (arg=%lu flags=0x%lX)", what, (unsigned long)a, (unsigned long)b);
    }
    ~Trace() { if (on) LOGI("DirectInput: < %s returned", what); }
};

template <class Cb>
struct EnumCtx {
    Cb      cb;
    LPVOID  user;
};

// True (and remembers the instance GUID, so CreateDevice can refuse it) when `inst`
// is a game controller that must be hidden from the game.
template <class Inst>
bool HideController(const Inst* inst) {
    if (!IsGameController(inst->dwDevType)) {
        if (!BypassActive() && InterlockedIncrement(&g_traceDevs) <= 40) {
            Name prod = ToName(inst->tszProductName);
            LOGI("DirectInput: visible device '%s' type=0x%02X", prod.s, (unsigned)LOBYTE(inst->dwDevType));
        }
        return false;
    }
    if (RememberHidden(inst->guidInstance)) {
        Name prod = ToName(inst->tszProductName);
        Name inst_n = ToName(inst->tszInstanceName);
        LOGI("DirectInput: hiding '%s' / '%s' type=0x%02X guid=%s",
             prod.s, inst_n.s, (unsigned)LOBYTE(inst->dwDevType), ToStr(inst->guidInstance).s);
    }
    return true;
}

template <class Inst, class Cb>
BOOL CALLBACK EnumShim(const Inst* inst, LPVOID ref) {
    auto* ctx = static_cast<EnumCtx<Cb>*>(ref);
    if (HideController(inst)) return DIENUM_CONTINUE;  // swallow
    return ctx->cb(inst, ctx->user);
}

// EnumDevicesBySemantics hands the callback an already-created device object as well.
// When hiding, that object is deliberately NOT released: whether the callback owns the
// reference is not something we can verify, and a one-off leak is far safer than a
// double release (same policy as the sibling project).
template <class Inst, class Dev, class Cb>
BOOL CALLBACK EnumSemShim(const Inst* inst, Dev* dev, DWORD flags, DWORD remaining, LPVOID ref) {
    auto* ctx = static_cast<EnumCtx<Cb>*>(ref);
    if (HideController(inst)) return DIENUM_CONTINUE;  // swallow
    return ctx->cb(inst, dev, flags, remaining, ctx->user);
}

template <class Inst, class Cb, class Orig>
HRESULT DoEnum(Orig orig, void* self, DWORD devType, Cb cb, LPVOID ref, DWORD flags) {
    if (!cb || BypassActive()) return orig(self, devType, cb, ref, flags);
    Trace trace("EnumDevices", devType, flags);
    EnumCtx<Cb> ctx{cb, ref};
    return orig(self, devType, &EnumShim<Inst, Cb>, &ctx, flags);
}

template <class Inst, class Dev, class Cb, class Fmt, class Str, class Orig>
HRESULT DoEnumSem(Orig orig, void* self, Str user, Fmt* fmt, Cb cb, LPVOID ref, DWORD flags) {
    if (!cb || BypassActive()) return orig(self, user, fmt, cb, ref, flags);
    Trace trace("EnumDevicesBySemantics", 0, flags);
    EnumCtx<Cb> ctx{cb, ref};
    return orig(self, user, fmt, &EnumSemShim<Inst, Dev, Cb>, &ctx, flags);
}

// ---- CreateDevice ---------------------------------------------------------

template <class Dev, class Orig>
HRESULT DoCreate(Orig orig, void* self, REFGUID guid, Dev** out, LPUNKNOWN outer) {
    Trace trace("CreateDevice", 0, 0);
    if (!BypassActive() && IsHidden(guid)) {
        LOGI("DirectInput: CreateDevice refused for hidden device %s", ToStr(guid).s);
        if (out) *out = nullptr;
        return DIERR_DEVICENOTREG;
    }
    return orig(self, guid, out, outer);
}

// ---- detours --------------------------------------------------------------

using PFN_Create   = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using PFN_EnumA    = HRESULT (STDMETHODCALLTYPE*)(void*, DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD);
using PFN_EnumW    = HRESULT (STDMETHODCALLTYPE*)(void*, DWORD, LPDIENUMDEVICESCALLBACKW, LPVOID, DWORD);
using PFN_EnumSemA = HRESULT (STDMETHODCALLTYPE*)(void*, LPCSTR,  LPDIACTIONFORMATA, LPDIENUMDEVICESBYSEMANTICSCBA, LPVOID, DWORD);
using PFN_EnumSemW = HRESULT (STDMETHODCALLTYPE*)(void*, LPCWSTR, LPDIACTIONFORMATW, LPDIENUMDEVICESBYSEMANTICSCBW, LPVOID, DWORD);
using PFN_CreateDA = HRESULT (STDMETHODCALLTYPE*)(void*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
using PFN_CreateDW = HRESULT (STDMETHODCALLTYPE*)(void*, REFGUID, LPDIRECTINPUTDEVICE8W*, LPUNKNOWN);

PFN_Create   g_origCreate = nullptr;
PFN_EnumA    g_origEnumA = nullptr;
PFN_EnumW    g_origEnumW = nullptr;
PFN_EnumSemA g_origEnumSemA = nullptr;
PFN_EnumSemW g_origEnumSemW = nullptr;
PFN_CreateDA g_origCreateDevA = nullptr;
PFN_CreateDW g_origCreateDevW = nullptr;

HRESULT STDMETHODCALLTYPE Hook_EnumDevicesA(void* self, DWORD t, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD f) {
    return DoEnum<DIDEVICEINSTANCEA>(g_origEnumA, self, t, cb, ref, f);
}
HRESULT STDMETHODCALLTYPE Hook_EnumDevicesW(void* self, DWORD t, LPDIENUMDEVICESCALLBACKW cb, LPVOID ref, DWORD f) {
    return DoEnum<DIDEVICEINSTANCEW>(g_origEnumW, self, t, cb, ref, f);
}
HRESULT STDMETHODCALLTYPE Hook_EnumSemA(void* self, LPCSTR user, LPDIACTIONFORMATA fmt,
                                        LPDIENUMDEVICESBYSEMANTICSCBA cb, LPVOID ref, DWORD f) {
    return DoEnumSem<DIDEVICEINSTANCEA, IDirectInputDevice8A>(g_origEnumSemA, self, user, fmt, cb, ref, f);
}
HRESULT STDMETHODCALLTYPE Hook_EnumSemW(void* self, LPCWSTR user, LPDIACTIONFORMATW fmt,
                                        LPDIENUMDEVICESBYSEMANTICSCBW cb, LPVOID ref, DWORD f) {
    return DoEnumSem<DIDEVICEINSTANCEW, IDirectInputDevice8W>(g_origEnumSemW, self, user, fmt, cb, ref, f);
}
HRESULT STDMETHODCALLTYPE Hook_CreateDeviceA(void* self, REFGUID g, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN o) {
    return DoCreate(g_origCreateDevA, self, g, out, o);
}
HRESULT STDMETHODCALLTYPE Hook_CreateDeviceW(void* self, REFGUID g, LPDIRECTINPUTDEVICE8W* out, LPUNKNOWN o) {
    return DoCreate(g_origCreateDevW, self, g, out, o);
}

bool HookOne(void* target, void* detour, void** orig, const char* name) {
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    // Log only after MH_EnableHook returned (it suspends other threads; a
    // suspended thread could be holding the logger lock).
    if (s != MH_OK) {
        LOGE("DirectInput: hook %s failed: %s", name, MH_StatusToString(s));
        return false;
    }
    LOGI("DirectInput: hooked %s @ %p", name, target);
    return true;
}

// Called with g_lock NOT held.
void HookObject(REFIID riid, void* obj) {
    bool isW = IsEqualGUID(riid, kIID_DI8W);
    bool isA = IsEqualGUID(riid, kIID_DI8A);
    if (!isA && !isW) {
        LOGD("DirectInput8Create: unexpected IID %s, vtable not hooked", ToStr(riid).s);
        return;
    }

    AcquireSRWLockExclusive(&g_lock);
    bool& done = isW ? g_hookedW : g_hookedA;
    if (done) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    done = true;  // never retry, even on failure

    void** vt = *reinterpret_cast<void***>(obj);
    void* enumFn   = vt[kVtEnumDevices];
    void* createFn = vt[kVtCreateDevice];
    void* semFn    = vt[kVtEnumDevicesBySemantics];
    bool ok;
    if (isW) {
        ok  = HookOne(enumFn,   reinterpret_cast<void*>(&Hook_EnumDevicesW),   reinterpret_cast<void**>(&g_origEnumW),      "IDirectInput8W::EnumDevices");
        ok &= HookOne(createFn, reinterpret_cast<void*>(&Hook_CreateDeviceW),  reinterpret_cast<void**>(&g_origCreateDevW), "IDirectInput8W::CreateDevice");
        ok &= HookOne(semFn,    reinterpret_cast<void*>(&Hook_EnumSemW),       reinterpret_cast<void**>(&g_origEnumSemW),   "IDirectInput8W::EnumDevicesBySemantics");
    } else {
        ok  = HookOne(enumFn,   reinterpret_cast<void*>(&Hook_EnumDevicesA),   reinterpret_cast<void**>(&g_origEnumA),      "IDirectInput8A::EnumDevices");
        ok &= HookOne(createFn, reinterpret_cast<void*>(&Hook_CreateDeviceA),  reinterpret_cast<void**>(&g_origCreateDevA), "IDirectInput8A::CreateDevice");
        ok &= HookOne(semFn,    reinterpret_cast<void*>(&Hook_EnumSemA),       reinterpret_cast<void**>(&g_origEnumSemA),   "IDirectInput8A::EnumDevicesBySemantics");
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!ok) LOGW("DirectInput: %s interface only partially hooked", isW ? "IDirectInput8W" : "IDirectInput8A");
}

HRESULT WINAPI Hook_DirectInput8Create(HINSTANCE h, DWORD ver, REFIID riid, LPVOID* ppv, LPUNKNOWN outer) {
    HRESULT hr = g_origCreate(h, ver, riid, ppv, outer);
    if (SUCCEEDED(hr) && ppv && *ppv && !BypassActive()) {
        LOGD("DirectInput8Create -> %p", *ppv);
        HookObject(riid, *ppv);
    }
    return hr;
}

}  // namespace

void Init() {
    if (g_tls == TLS_OUT_OF_INDEXES) g_tls = TlsAlloc();
}

bool Install(bool allowLoadLibrary) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_createHooked) {
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }

    HMODULE mod = GetModuleHandleW(L"dinput8.dll");
    if (!mod && allowLoadLibrary) mod = LoadLibraryW(L"dinput8.dll");
    if (!mod) {
        ReleaseSRWLockExclusive(&g_lock);
        LOGD("DirectInput: dinput8.dll not loaded yet, hook postponed");
        return false;
    }

    if (!g_mhInit) {
        MH_STATUS s = MH_Initialize();
        if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
            ReleaseSRWLockExclusive(&g_lock);
            LOGE("MH_Initialize failed: %s", MH_StatusToString(s));
            return false;
        }
        g_mhInit = true;
    }

    void* target = reinterpret_cast<void*>(GetProcAddress(mod, "DirectInput8Create"));
    if (!target) {
        ReleaseSRWLockExclusive(&g_lock);
        LOGE("DirectInput: DirectInput8Create not found in dinput8.dll");
        return false;
    }

    // HookOne logs; it is safe to log while holding g_lock because the logger
    // uses its own lock and MH_EnableHook has already returned at that point.
    bool ok = HookOne(target, reinterpret_cast<void*>(&Hook_DirectInput8Create),
                      reinterpret_cast<void**>(&g_origCreate), "dinput8!DirectInput8Create");
    g_createHooked = ok;
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

void Shutdown() {
    AcquireSRWLockExclusive(&g_lock);
    if (g_mhInit) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_mhInit = false;
        g_createHooked = g_hookedA = g_hookedW = false;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

Bypass::Bypass() {
    if (g_tls == TLS_OUT_OF_INDEXES) return;
    auto depth = reinterpret_cast<uintptr_t>(TlsGetValue(g_tls));
    TlsSetValue(g_tls, reinterpret_cast<LPVOID>(depth + 1));
}

Bypass::~Bypass() {
    if (g_tls == TLS_OUT_OF_INDEXES) return;
    auto depth = reinterpret_cast<uintptr_t>(TlsGetValue(g_tls));
    if (depth) TlsSetValue(g_tls, reinterpret_cast<LPVOID>(depth - 1));
}

}  // namespace dinput_hide
