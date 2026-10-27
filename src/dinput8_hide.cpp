// dinput8-hide
// A dinput8.dll proxy. Forwards everything to the real system dinput8.dll, but
// wraps IDirectInput8A/W so that gamepad-class devices never show up in
// EnumDevices / EnumDevicesBySemantics and cannot be opened via CreateDevice.
// Keyboard, mouse and everything else are left alone.
//
// Pair it with xinput-sdl-wrapper: the game then only sees the pad through
// XInput (backed by SDL), instead of seeing it twice or picking the wrong API.

#include <windows.h>
#include <initguid.h>   // define IID_/GUID_ symbols here; no dxguid.lib needed
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cctype>

#ifdef _MSC_VER
#include <intrin.h>
#define RETADDR() _ReturnAddress()
#define NOINLINE __declspec(noinline)
#else
#define RETADDR() __builtin_return_address(0)
#define NOINLINE __attribute__((noinline))
#endif

namespace {

// ---- config ---------------------------------------------------------------

// No configuration of its own: the hiding policy is fixed. The only thing read
// from disk is the existing "Debug" key in xinput_sdl.ini (shared with the
// XInput wrapper), so there is no extra ini to ship.
struct Config {
    static constexpr bool hideJoystickClass = true;   // JOYSTICK, GAMEPAD, DRIVING, FLIGHT, 1STPERSON
    static constexpr bool hideSupplemental  = true;   // DI8DEVTYPE_SUPPLEMENTAL (pedals, throttles, odd pads)
    static constexpr bool hideGeneric       = false;  // DI8DEVTYPE_DEVICE: generic HID, may be non-pads
    bool debug = false;
};

HMODULE   g_module = nullptr;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
HMODULE   g_real = nullptr;
Config    g_cfg;

typedef HRESULT (WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT (WINAPI* DllCanUnloadNow_t)(void);
typedef HRESULT (WINAPI* DllGetClassObject_t)(REFCLSID, REFIID, LPVOID*);
typedef HRESULT (WINAPI* DllRegisterServer_t)(void);
typedef HRESULT (WINAPI* GetdfDIJoystick_t)(void);

DirectInput8Create_t p_Create = nullptr;
DllCanUnloadNow_t    p_CanUnload = nullptr;
DllGetClassObject_t  p_GetClassObject = nullptr;
DllRegisterServer_t  p_Register = nullptr;
DllRegisterServer_t  p_Unregister = nullptr;
GetdfDIJoystick_t    p_GetdfDIJoystick = nullptr;

void Log(const char* fmt, ...) {
    if (!g_cfg.debug) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    strcat(buf, "\n");
    OutputDebugStringA("[dinput8-hide] ");
    OutputDebugStringA(buf);
}

BOOL CALLBACK InitOnceFn(PINIT_ONCE, PVOID, PVOID*) {
    char dir[MAX_PATH], ini[MAX_PATH + 32];
    GetModuleFileNameA(g_module, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) slash[1] = '\0';
    snprintf(ini, sizeof(ini), "%sxinput_sdl.ini", dir);   // shared; optional

    g_cfg.debug = GetPrivateProfileIntA("General", "Debug", 0, ini) != 0;

    // Load the *real* dinput8 from the system directory (never ourselves).
    char sys[MAX_PATH + 32];
    GetSystemDirectoryA(sys, MAX_PATH);   // SysWOW64 for 32-bit processes on x64
    strcat(sys, "\\dinput8.dll");
    g_real = LoadLibraryA(sys);
    if (!g_real) { Log("failed to load %s", sys); return TRUE; }

    p_Create          = (DirectInput8Create_t)GetProcAddress(g_real, "DirectInput8Create");
    p_CanUnload       = (DllCanUnloadNow_t)   GetProcAddress(g_real, "DllCanUnloadNow");
    p_GetClassObject  = (DllGetClassObject_t) GetProcAddress(g_real, "DllGetClassObject");
    p_Register        = (DllRegisterServer_t) GetProcAddress(g_real, "DllRegisterServer");
    p_Unregister      = (DllRegisterServer_t) GetProcAddress(g_real, "DllUnregisterServer");
    p_GetdfDIJoystick = (GetdfDIJoystick_t)   GetProcAddress(g_real, "GetdfDIJoystick");
    Log("loaded %s", sys);
    return TRUE;
}

inline void EnsureInit() { InitOnceExecuteOnce(&g_once, InitOnceFn, nullptr, nullptr); }

// ---- filtering policy -----------------------------------------------------

bool IsHiddenType(DWORD devType) {
    switch (GET_DIDEVICE_TYPE(devType)) {
        case DI8DEVTYPE_JOYSTICK:
        case DI8DEVTYPE_GAMEPAD:
        case DI8DEVTYPE_DRIVING:
        case DI8DEVTYPE_FLIGHT:
        case DI8DEVTYPE_1STPERSON:   return g_cfg.hideJoystickClass;
        case DI8DEVTYPE_SUPPLEMENTAL:return g_cfg.hideSupplemental;
        case DI8DEVTYPE_DEVICE:      return g_cfg.hideGeneric;
        default:                     return false;  // keyboard, mouse, screen pointing, remote...
    }
}

bool IsSystemGuid(REFGUID g) {
    return IsEqualGUID(g, GUID_SysKeyboard) || IsEqualGUID(g, GUID_SysMouse) ||
           IsEqualGUID(g, GUID_SysKeyboardEm) || IsEqualGUID(g, GUID_SysKeyboardEm2) ||
           IsEqualGUID(g, GUID_SysMouseEm) || IsEqualGUID(g, GUID_SysMouseEm2);
}

// Callers that must see the real devices: SDL itself (it uses DirectInput as a
// joystick backend and is what feeds xinput-sdl-wrapper) and our own XInput DLLs.
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
    bool exempt = strncmp(low, "sdl", 3) == 0 || strncmp(low, "xinput", 6) == 0;
    Log("DirectInput8Create from '%s' -> %s", low, exempt ? "passthrough" : "filtered");
    return exempt;
}

// ---- A/W traits -----------------------------------------------------------

struct TraitsA {
    using Iface  = IDirectInput8A;
    using Dev    = DIDEVICEINSTANCEA;
    using Str    = LPCSTR;
    using EnumCb = LPDIENUMDEVICESCALLBACKA;
    using SemCb  = LPDIENUMDEVICESBYSEMANTICSCBA;
    using Fmt    = LPDIACTIONFORMATA;
    using Cfg    = LPDICONFIGUREDEVICESPARAMSA;
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
    using Cfg    = LPDICONFIGUREDEVICESPARAMSW;
    using DevOut = LPDIRECTINPUTDEVICE8W;
    static const IID& Iid() { return IID_IDirectInput8W; }
};

template <class Tr> struct EnumCtx { typename Tr::EnumCb cb; LPVOID ref; };

template <class Tr>
BOOL CALLBACK EnumThunk(const typename Tr::Dev* d, LPVOID p) {
    auto* c = (EnumCtx<Tr>*)p;
    if (IsHiddenType(d->dwDevType)) {
        Log("hid device (type 0x%02lX)", (unsigned long)d->dwDevType);
        return DIENUM_CONTINUE;
    }
    return c->cb(d, c->ref);
}

template <class Tr> struct SemCtx { typename Tr::SemCb cb; LPVOID ref; };

template <class Tr>
BOOL CALLBACK SemThunk(const typename Tr::Dev* d, typename Tr::DevOut dev,
                       DWORD flags, DWORD remaining, LPVOID p) {
    auto* c = (SemCtx<Tr>*)p;
    if (IsHiddenType(d->dwDevType)) return DIENUM_CONTINUE;
    return c->cb(d, dev, flags, remaining, c->ref);
}

struct FindCtx { const GUID* guid; bool found; bool hidden; };

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

// ---- the wrapper ----------------------------------------------------------

template <class Tr> class Wrapper;
template <class Tr> HRESULT WrapInto(typename Tr::Iface* real, void** out);

template <class Tr>
class Wrapper : public Tr::Iface {
    using Iface = typename Tr::Iface;
    Iface* real_;
    LONG   ref_ = 1;

public:
    explicit Wrapper(Iface* r) : real_(r) {}

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, Tr::Iid())) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        // Asked for the other charset flavour: wrap that too, so it stays filtered.
        if (IsEqualIID(riid, IID_IDirectInput8A) || IsEqualIID(riid, IID_IDirectInput8W)) {
            void* other = nullptr;
            HRESULT hr = real_->QueryInterface(riid, &other);
            if (FAILED(hr)) return hr;
            if (IsEqualIID(riid, IID_IDirectInput8A))
                return WrapInto<TraitsA>((IDirectInput8A*)other, ppv);
            return WrapInto<TraitsW>((IDirectInput8W*)other, ppv);
        }
        return real_->QueryInterface(riid, ppv);
    }
    STDMETHOD_(ULONG, AddRef)() override { return (ULONG)InterlockedIncrement(&ref_); }
    STDMETHOD_(ULONG, Release)() override {
        LONG n = InterlockedDecrement(&ref_);
        if (n == 0) { real_->Release(); delete this; }
        return (ULONG)n;
    }

    // IDirectInput8
    STDMETHOD(CreateDevice)(REFGUID rguid, typename Tr::DevOut* out, LPUNKNOWN outer) override {
        if (!IsSystemGuid(rguid)) {
            FindCtx fc = { &rguid, false, false };
            real_->EnumDevices(DI8DEVCLASS_ALL, FindThunk<Tr>, &fc, DIEDFL_ALLDEVICES);
            if (fc.found && fc.hidden) {
                Log("CreateDevice blocked");
                if (out) *out = nullptr;
                return DIERR_DEVICENOTREG;
            }
        }
        return real_->CreateDevice(rguid, out, outer);
    }

    STDMETHOD(EnumDevices)(DWORD devType, typename Tr::EnumCb cb, LPVOID ref, DWORD flags) override {
        if (!cb) return real_->EnumDevices(devType, cb, ref, flags);
        EnumCtx<Tr> ctx = { cb, ref };
        return real_->EnumDevices(devType, EnumThunk<Tr>, &ctx, flags);
    }

    STDMETHOD(GetDeviceStatus)(REFGUID rguid) override {
        if (!IsSystemGuid(rguid)) {
            FindCtx fc = { &rguid, false, false };
            real_->EnumDevices(DI8DEVCLASS_ALL, FindThunk<Tr>, &fc, DIEDFL_ALLDEVICES);
            if (fc.found && fc.hidden) return DI_NOTATTACHED;
        }
        return real_->GetDeviceStatus(rguid);
    }

    STDMETHOD(RunControlPanel)(HWND h, DWORD f) override { return real_->RunControlPanel(h, f); }
    STDMETHOD(Initialize)(HINSTANCE h, DWORD v) override { return real_->Initialize(h, v); }

    STDMETHOD(FindDevice)(REFGUID cls, typename Tr::Str name, LPGUID inst) override {
        return real_->FindDevice(cls, name, inst);
    }

    STDMETHOD(EnumDevicesBySemantics)(typename Tr::Str user, typename Tr::Fmt fmt,
                                      typename Tr::SemCb cb, LPVOID ref, DWORD flags) override {
        if (!cb) return real_->EnumDevicesBySemantics(user, fmt, cb, ref, flags);
        SemCtx<Tr> ctx = { cb, ref };
        return real_->EnumDevicesBySemantics(user, fmt, SemThunk<Tr>, &ctx, flags);
    }

    STDMETHOD(ConfigureDevices)(LPDICONFIGUREDEVICESCALLBACK cb, typename Tr::Cfg params,
                                DWORD flags, LPVOID ref) override {
        return real_->ConfigureDevices(cb, params, flags, ref);
    }
};

template <class Tr>
HRESULT WrapInto(typename Tr::Iface* real, void** out) {
    *out = new Wrapper<Tr>(real);   // takes ownership of the reference we were given
    return S_OK;
}

}  // namespace

// ---- exports (ordinals assigned in dinput8.def) ----------------------------

extern "C" {

NOINLINE HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid,
                                           LPVOID* ppv, LPUNKNOWN outer) {
    void* ret = RETADDR();
    EnsureInit();
    if (!p_Create) return DIERR_GENERIC;
    HRESULT hr = p_Create(inst, ver, riid, ppv, outer);
    if (FAILED(hr) || !ppv || !*ppv || outer) return hr;
    if (CallerIsExempt(ret)) return hr;

    if (IsEqualIID(riid, IID_IDirectInput8A))
        return WrapInto<TraitsA>((IDirectInput8A*)*ppv, ppv);
    if (IsEqualIID(riid, IID_IDirectInput8W))
        return WrapInto<TraitsW>((IDirectInput8W*)*ppv, ppv);
    return hr;  // some other interface: leave untouched
}

HRESULT WINAPI DllCanUnloadNow(void) {
    EnsureInit();
    return S_FALSE;  // never let the host unload us while wrappers may be alive
}
HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID i, LPVOID* p) {
    EnsureInit();
    return p_GetClassObject ? p_GetClassObject(c, i, p) : CLASS_E_CLASSNOTAVAILABLE;
}
HRESULT WINAPI DllRegisterServer(void)   { EnsureInit(); return p_Register   ? p_Register()   : E_FAIL; }
HRESULT WINAPI DllUnregisterServer(void) { EnsureInit(); return p_Unregister ? p_Unregister() : E_FAIL; }

// Returns a pointer to the joystick data format. Pure forward.
const DIDATAFORMAT* WINAPI GetdfDIJoystick(void) {
    EnsureInit();
    return p_GetdfDIJoystick ? (const DIDATAFORMAT*)p_GetdfDIJoystick() : nullptr;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;  // no real work under loader lock; init is lazy
}