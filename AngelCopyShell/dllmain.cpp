#include "Common.h"
#include <new>

// Declared in Register.cpp
STDAPI DllRegisterServer();
STDAPI DllUnregisterServer();

namespace {

class CClassFactory : public IClassFactory {
public:
    explicit CClassFactory(HRESULT (*create)(REFIID, void**))
        : m_ref(1), m_create(create) {
        DllAddRef();
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    IFACEMETHODIMP_(ULONG) Release() override {
        long n = InterlockedDecrement(&m_ref);
        if (n == 0) delete this;
        return n;
    }
    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid,
                                  void** ppv) override {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        return m_create(riid, ppv);
    }
    IFACEMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) DllAddRef(); else DllRelease();
        return S_OK;
    }

private:
    ~CClassFactory() { DllRelease(); }
    long m_ref;
    HRESULT (*m_create)(REFIID, void**);
};

} // namespace

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hInst;
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    *ppv = nullptr;
    HRESULT (*create)(REFIID, void**) = nullptr;

    if (rclsid == CLSID_AngelContextMenu)
        create = CreateAngelContextMenu;
    else if (rclsid == CLSID_AngelDragDrop)
        create = CreateAngelDragDropHandler;
    else
        return CLASS_E_CLASSNOTAVAILABLE;

    CClassFactory* factory = new (std::nothrow) CClassFactory(create);
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return (g_cDllRef == 0) ? S_OK : S_FALSE;
}
