// dinput_hide.cpp
// Hides gamepad-class devices from DirectInput 8 using MinHook, so no separate
// dinput8.dll proxy is needed.
//
// DirectInput methods are COM methods, so we can't hook an export. Instead we
// create a throwaway IDirectInput8A / IDirectInput8W, read the method addresses
// out of their (process-wide, shared) vtables, and hook those:
//   CreateDevice, EnumDevices, GetDeviceStatus, EnumDevicesBySemantics.
// Keyboard, mouse and every non-gamepad device class are passed through.

#include <windows.h>
#include <objbase.h>
#include <initguid.h>   // defines IID_/GUID_ symbols in this TU; no dxguid.lib
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include "MinHook.h"
#include "dinput_hide.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifdef _MSC_VER
#include <intrin.h>
#define RETADDR() _ReturnAddress()
#define NOINLINE __declspec(noinline)
#else
#define RETADDR() __builtin_return_address(0)
#define NOINLINE __attribute__((noinline))
#endif

namespace {

bool g_debug = false;

void Log(const char* fmt, ...) {
    if (!g_debug) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    strcat(buf, "\n");
    OutputDebugStringA("[xinput-sdl/dinput] ");
    OutputDebugStringA(buf);
}

// ---- policy ---------------------------------------------------------------

bool IsHiddenType(DWORD devType) {
    switch (GET_DIDEVICE_TYPE(devType)) {
        case DI8DEVTYPE_JOYSTICK:
        case DI8DEVTYPE_GAMEPAD:
        case DI8DEVTYPE_DRIVING:
        case DI8DEVTYPE_FLIGHT:
        case DI8DEVTYPE_1STPERSON:
        case DI8DEVTYPE_SUPPLEMENTAL: return true;
        default:                      return false;  // keyboard, mouse, generic HID, ...
    }
}

bool IsSystemGuid(REFGUID g) {
    return IsEqualGUID(g, GUID_SysKeyboard) || IsEqualGUID(g, GUID_SysMouse) ||
           IsEqualGUID(g, GUID_SysKeyboardEm) || IsEqualGUID(g, GUID_SysKeyboardEm2) ||
           IsEqualGUID(g, GUID_SysMouseEm) || IsEqualGUID(g, GUID_SysMouseEm2);
}

// SDL (which uses DirectInput as a joystick backend and feeds this wrapper) and
// our own xinput*.dll (if SDL is linked statically) must see the real devices.
bool CallerIsExempt(void* retAddr) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)retAddr, &m) || !m)
        return false;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(m, path, MAX_PATH)) return false;
    const char* base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    char low[64];
    size_t i = 0;
    for (; base[i] && i < sizeof(low) - 1; ++i) low[i] = (char)tolower((unsigned char)base[i]);
    low[i] = '\0';
    return strncmp(low, "sdl", 3) == 0 || strncmp(low, "xinput", 6) == 0;
}

// Re-entrancy: dinput's A methods may call W methods internally (both hooked),
// and our own lookups call the original EnumDevices. While depth > 0 we are
// inside such a call and pass straight through; the outermost call has already
// done the filtering. Callbacks into game code reset depth to 0 (see thunks) so
// that a game calling DirectInput from inside its enum callback is still filtered.
thread_local int t_depth = 0;
struct Guard { Guard() { ++t_depth; } ~Guard() { --t_depth; } };

// ---- A/W traits -----------------------------------------------------------

struct TraitsA {
    using Iface  = IDirectInput8A;
    using Dev    = DIDEVICEINSTANCEA;
    using Str    = LPCSTR;
    using EnumCb = LPDIENUMDEVICESCALLBACKA;
    using SemCb  = LPDIENUMDEVICESBYSEMANTICSCBA;
    using Fmt    = LPDIACTIONFORMATA;
    using DevOut = LPDIRECTINPUTDEVICE8A;
    static const IID& Iid() { return IID_IDirectInput8A; }
};
struct TraitsW {
    using Iface  = IDirectInput8W;
    using Dev    = DIDEVICEINSTANCEW;
    using Str    = LPCWSTR;
    using EnumCb = LPDIENUMDEVICESCALLBACKW;
    using SemCb  = LPDIENUMDEVICESBYSEMANTICSCBW;
    using Fmt    = LPDIACTIONFORMATW;
    using DevOut = LPDIRECTINPUTDEVICE8W;
    static const IID& Iid() { return IID_IDirectInput8W; }
};

template <class Tr> struct EnumCtx { typename Tr::EnumCb cb; LPVOID ref; };
template <class Tr> struct SemCtx  { typename Tr::SemCb cb;  LPVOID ref; };
struct FindCtx { const GUID* guid; bool found; bool hidden; };

template <class Tr>
BOOL CALLBACK EnumThunk(const typename Tr::Dev* d, LPVOID p) {
    auto* c = (EnumCtx<Tr>*)p;
    if (IsHiddenType(d->dwDevType)) {
        Log("hid device (type 0x%02lX)", (unsigned long)d->dwDevType);
        return DIENUM_CONTINUE;
    }
    int saved = t_depth; t_depth = 0;
    BOOL r = c->cb(d, c->ref);
    t_depth = saved;
    return r;
}

template <class Tr>
BOOL CALLBACK SemThunk(const typename Tr::Dev* d, typename Tr::DevOut dev,
                       DWORD flags, DWORD remaining, LPVOID p) {
    auto* c = (SemCtx<Tr>*)p;
    if (IsHiddenType(d->dwDevType)) return DIENUM_CONTINUE;
    int saved = t_depth; t_depth = 0;
    BOOL r = c->cb(d, dev, flags, remaining, c->ref);
    t_depth = saved;
    return r;
}

template <class Tr>
BOOL CALLBACK FindThunk(const typename Tr::Dev* d, LPVOID p) {
    auto* c = (FindCtx*)p;
    if (IsEqualGUID(d->guidInstance, *c->guid)) {
        c->found = true;
        c->hidden = IsHiddenType(d->dwDevType);
        return DIENUM_STOP;
    }
    return DIENUM_CONTINUE;
}

// ---- detours --------------------------------------------------------------

template <class Tr>
struct H {
    using I = typename Tr::Iface;
    using EnumFn   = HRESULT (STDMETHODCALLTYPE*)(I*, DWORD, typename Tr::EnumCb, LPVOID, DWORD);
    using CreateFn = HRESULT (STDMETHODCALLTYPE*)(I*, REFGUID, typename Tr::DevOut*, LPUNKNOWN);
    using StatusFn = HRESULT (STDMETHODCALLTYPE*)(I*, REFGUID);
    using SemFn    = HRESULT (STDMETHODCALLTYPE*)(I*, typename Tr::Str, typename Tr::Fmt,
                                                  typename Tr::SemCb, LPVOID, DWORD);

    static inline EnumFn   oEnum   = nullptr;
    static inline CreateFn oCreate = nullptr;
    static inline StatusFn oStatus = nullptr;
    static inline SemFn    oSem    = nullptr;

    // True if guid names a device that should be hidden. Uses the original
    // EnumDevices so it sees everything.
    static bool IsHiddenGuid(I* self, REFGUID g) {
        if (IsSystemGuid(g)) return false;
        FindCtx fc = { &g, false, false };
        Guard guard;
        oEnum(self, DI8DEVCLASS_ALL, FindThunk<Tr>, &fc, DIEDFL_ALLDEVICES);
        return fc.found && fc.hidden;
    }

    static NOINLINE HRESULT STDMETHODCALLTYPE EnumDevices(I* self, DWORD devType,
            typename Tr::EnumCb cb, LPVOID ref, DWORD flags) {
        void* ret = RETADDR();
        if (!cb || t_depth > 0 || CallerIsExempt(ret)) {
            Guard g;
            return oEnum(self, devType, cb, ref, flags);
        }
        EnumCtx<Tr> ctx = { cb, ref };
        Guard g;
        return oEnum(self, devType, EnumThunk<Tr>, &ctx, flags);
    }

    static NOINLINE HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(I* self,
            typename Tr::Str user, typename Tr::Fmt fmt, typename Tr::SemCb cb,
            LPVOID ref, DWORD flags) {
        void* ret = RETADDR();
        if (!cb || t_depth > 0 || CallerIsExempt(ret)) {
            Guard g;
            return oSem(self, user, fmt, cb, ref, flags);
        }
        SemCtx<Tr> ctx = { cb, ref };
        Guard g;
        return oSem(self, user, fmt, SemThunk<Tr>, &ctx, flags);
    }

    static NOINLINE HRESULT STDMETHODCALLTYPE CreateDevice(I* self, REFGUID guid,
            typename Tr::DevOut* out, LPUNKNOWN outer) {
        void* ret = RETADDR();
        if (t_depth == 0 && !IsSystemGuid(guid) && !CallerIsExempt(ret) &&
            IsHiddenGuid(self, guid)) {
            Log("CreateDevice blocked");
            if (out) *out = nullptr;
            return DIERR_DEVICENOTREG;
        }
        Guard g;
        return oCreate(self, guid, out, outer);
    }

    static NOINLINE HRESULT STDMETHODCALLTYPE GetDeviceStatus(I* self, REFGUID guid) {
        void* ret = RETADDR();
        if (t_depth == 0 && !IsSystemGuid(guid) && !CallerIsExempt(ret) &&
            IsHiddenGuid(self, guid))
            return DI_NOTATTACHED;
        Guard g;
        return oStatus(self, guid);
    }
};

// ---- installation ---------------------------------------------------------

constexpr int kVtCreateDevice = 3;
constexpr int kVtEnumDevices = 4;
constexpr int kVtGetDeviceStatus = 5;
constexpr int kVtEnumDevicesBySemantics = 9;

bool HookOne(void* target, void* detour, void** orig, const char* what) {
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s != MH_OK) { Log("MH_CreateHook(%s) failed: %s", what, MH_StatusToString(s)); return false; }
    s = MH_EnableHook(target);
    if (s != MH_OK) { Log("MH_EnableHook(%s) failed: %s", what, MH_StatusToString(s)); return false; }
    return true;
}

typedef HRESULT (WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

template <class Tr>
bool HookInterface(DirectInput8Create_t create) {
    typename Tr::Iface* obj = nullptr;
    if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, Tr::Iid(),
                      (void**)&obj, nullptr)) || !obj) {
        Log("DirectInput8Create failed");
        return false;
    }
    void** vt = *(void***)obj;   // shared by every instance of this class
    void* tCreate = vt[kVtCreateDevice];
    void* tEnum   = vt[kVtEnumDevices];
    void* tStatus = vt[kVtGetDeviceStatus];
    void* tSem    = vt[kVtEnumDevicesBySemantics];
    obj->Release();              // dinput8.dll stays loaded; vtable is static data

    using HH = H<Tr>;
    bool ok = true;
    ok &= HookOne(tEnum,   (void*)&HH::EnumDevices,           (void**)&HH::oEnum,   "EnumDevices");
    ok &= HookOne(tSem,    (void*)&HH::EnumDevicesBySemantics,(void**)&HH::oSem,    "EnumDevicesBySemantics");
    ok &= HookOne(tCreate, (void*)&HH::CreateDevice,          (void**)&HH::oCreate, "CreateDevice");
    ok &= HookOne(tStatus, (void*)&HH::GetDeviceStatus,       (void**)&HH::oStatus, "GetDeviceStatus");
    return ok;
}

}  // namespace

bool InstallDInputHooks(bool debug) {
    g_debug = debug;

    // Use whatever "dinput8.dll" the game would get (system one, or another
    // proxy such as ReShade/Special K if present), so we hook the object it uses.
    HMODULE di = LoadLibraryA("dinput8.dll");
    if (!di) { Log("dinput8.dll not found"); return false; }
    auto create = (DirectInput8Create_t)GetProcAddress(di, "DirectInput8Create");
    if (!create) return false;

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MH_Initialize failed: %s", MH_StatusToString(s));
        return false;
    }

    bool a = HookInterface<TraitsA>(create);
    bool w = HookInterface<TraitsW>(create);
    Log("hooks installed: A=%d W=%d", (int)a, (int)w);
    return a || w;
}