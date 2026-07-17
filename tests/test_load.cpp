// Non-invasive COM smoke test: loads AngelCopyShell.dll directly (no registry,
// no Explorer restart) and verifies the class objects and their interfaces.
#include <windows.h>
#include <shlobj.h>
#include <ole2.h>
#include <cstdio>

static const CLSID kOldDrop = {0x7f3a9c21,0x1b4e,0x4c8a,{0x9e,0x2d,0x4a,0x1f,0x6b,0x0c,0x0d,0x01}};
static const CLSID kCtx     = {0x7f3a9c21,0x1b4e,0x4c8a,{0x9e,0x2d,0x4a,0x1f,0x6b,0x0c,0x0d,0x02}};
static const CLSID kDragDrop= {0x7f3a9c21,0x1b4e,0x4c8a,{0x9e,0x2d,0x4a,0x1f,0x6b,0x0c,0x0d,0x03}};

typedef HRESULT (WINAPI *PFN_DllGetClassObject)(REFCLSID, REFIID, void**);
typedef HRESULT (WINAPI *PFN_DllCanUnloadNow)();

static int g_fail = 0;
static void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* dll = (argc > 1) ? argv[1] : L"..\\dist\\AngelCopyShell.dll";
    CoInitialize(nullptr);

    HMODULE h = LoadLibraryW(dll);
    if (!h) { printf("LoadLibrary failed (%lu)\n", GetLastError()); return 2; }

    auto getco = (PFN_DllGetClassObject)GetProcAddress(h, "DllGetClassObject");
    auto canun = (PFN_DllCanUnloadNow)GetProcAddress(h, "DllCanUnloadNow");
    check(getco != nullptr, "export DllGetClassObject");
    check(canun != nullptr, "export DllCanUnloadNow");
    if (!getco) return 2;

    // --- Drag&drop (right-drag) handler ---
    printf("DragDrop handler:\n");
    IClassFactory* cf = nullptr;
    check(SUCCEEDED(getco(kDragDrop, IID_IClassFactory, (void**)&cf)) && cf,
          "DllGetClassObject -> IClassFactory");
    if (cf) {
        IShellExtInit* si = nullptr;
        check(SUCCEEDED(cf->CreateInstance(nullptr, IID_IShellExtInit, (void**)&si)) && si,
              "CreateInstance -> IShellExtInit");
        if (si) {
            IContextMenu* cm = nullptr;
            check(SUCCEEDED(si->QueryInterface(IID_IContextMenu, (void**)&cm)) && cm,
                  "QI IShellExtInit -> IContextMenu");
            if (cm) cm->Release();
            si->Release();
        }
        cf->Release();
    }

    // --- The old DropHandler class must be gone: the shell never used it. ---
    printf("Retired DropHandler:\n");
    IClassFactory* dead = nullptr;
    check(getco(kOldDrop, IID_IClassFactory, (void**)&dead) == CLASS_E_CLASSNOTAVAILABLE
              && !dead,
          "old DropHandler CLSID no longer served");

    // --- Context menu ---
    printf("Context menu:\n");
    IClassFactory* cf2 = nullptr;
    check(SUCCEEDED(getco(kCtx, IID_IClassFactory, (void**)&cf2)) && cf2,
          "DllGetClassObject -> IClassFactory");
    if (cf2) {
        IShellExtInit* si = nullptr;
        check(SUCCEEDED(cf2->CreateInstance(nullptr, IID_IShellExtInit, (void**)&si)) && si,
              "CreateInstance -> IShellExtInit");
        if (si) {
            IContextMenu* cm = nullptr;
            check(SUCCEEDED(si->QueryInterface(IID_IContextMenu, (void**)&cm)) && cm,
                  "QI IShellExtInit -> IContextMenu");
            if (cm) cm->Release();
            si->Release();
        }
        cf2->Release();
    }

    // --- Unknown CLSID must be rejected ---
    printf("Negative:\n");
    IClassFactory* bogus = nullptr;
    HRESULT hrBogus = getco(IID_IUnknown, IID_IClassFactory, (void**)&bogus);
    check(hrBogus == CLASS_E_CLASSNOTAVAILABLE && !bogus,
          "unknown CLSID -> CLASS_E_CLASSNOTAVAILABLE");

    printf("DllCanUnloadNow after release: %s\n",
           (canun && canun() == S_OK) ? "S_OK (no leaks)" : "S_FALSE (refs held)");

    CoUninitialize();
    printf("\n%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_fail == 0 ? 0 : 1;
}
