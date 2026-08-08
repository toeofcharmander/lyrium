// The dinput8 half of the proxy.
//
// lyrium can ship under either of two filenames. As d3d9.dll it is the graphics
// proxy and exports Direct3DCreate9 directly. As dinput8.dll it takes an earlier,
// unrelated slot, forwards every DirectInput call straight through, and reaches
// Direct3DCreate9 by patching the game's import table instead -- which leaves
// d3d9.dll free for ReShade.
//
// Nothing in this file does any work of its own. The whole of lyrium runs from
// d3d9.cpp either way; this only satisfies the loader so the game starts.

#include <string>

#include <windows.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

namespace
{

::HMODULE system_dinput8{};

auto load_system_dinput8() -> ::HMODULE
{
    if (system_dinput8 != nullptr)
    {
        return system_dinput8;
    }

    // From the system directory explicitly. Loading by bare name would find this
    // very file, since the game's own folder is searched first -- that is the
    // whole mechanism a proxy relies on, and here it would be a loop.
    char path[MAX_PATH]{};
    if (::GetSystemDirectoryA(path, MAX_PATH) == 0u)
    {
        return nullptr;
    }

    system_dinput8 = ::LoadLibraryA((std::string{path} + "\\dinput8.dll").c_str());
    return system_dinput8;
}

template <class Fn>
auto resolve(const char *name) -> Fn
{
    auto *library = load_system_dinput8();
    if (library == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<Fn>(::GetProcAddress(library, name));
}

}

// dinput.h and objbase.h declare the other four. These two are ordinary COM
// server exports with no header behind them here, and -Wmissing-declarations
// wants a declaration before the definition.
extern "C" ::HRESULT WINAPI DllRegisterServer();
extern "C" ::HRESULT WINAPI DllUnregisterServer();

extern "C"
{

::HRESULT WINAPI DirectInput8Create(::HINSTANCE instance, ::DWORD version, REFIID iid, ::LPVOID *out, ::LPUNKNOWN outer)
{
    using Fn = ::HRESULT(WINAPI *)(::HINSTANCE, ::DWORD, REFIID, ::LPVOID *, ::LPUNKNOWN);
    const auto original = resolve<Fn>("DirectInput8Create");
    return original == nullptr ? E_FAIL : original(instance, version, iid, out, outer);
}

::HRESULT WINAPI DllCanUnloadNow()
{
    using Fn = ::HRESULT(WINAPI *)();
    const auto original = resolve<Fn>("DllCanUnloadNow");
    return original == nullptr ? S_FALSE : original();
}

::HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, ::LPVOID *out)
{
    using Fn = ::HRESULT(WINAPI *)(REFCLSID, REFIID, ::LPVOID *);
    const auto original = resolve<Fn>("DllGetClassObject");
    return original == nullptr ? CLASS_E_CLASSNOTAVAILABLE : original(clsid, iid, out);
}

::HRESULT WINAPI DllRegisterServer()
{
    using Fn = ::HRESULT(WINAPI *)();
    const auto original = resolve<Fn>("DllRegisterServer");
    return original == nullptr ? E_FAIL : original();
}

::HRESULT WINAPI DllUnregisterServer()
{
    using Fn = ::HRESULT(WINAPI *)();
    const auto original = resolve<Fn>("DllUnregisterServer");
    return original == nullptr ? E_FAIL : original();
}

::LPCDIDATAFORMAT WINAPI GetdfDIJoystick()
{
    using Fn = ::LPCDIDATAFORMAT(WINAPI *)();
    const auto original = resolve<Fn>("GetdfDIJoystick");
    return original == nullptr ? nullptr : original();
}

}
