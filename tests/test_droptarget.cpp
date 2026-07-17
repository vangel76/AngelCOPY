// Does the shell actually use our registered Directory\shellex\DropHandler?
//
// This asks the shell for a folder's IDropTarget exactly the way Explorer does
// when you drag something over that folder, then checks whose object came back.
// No dragging required.
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <ole2.h>
#include <cstdio>

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Shlwapi.lib")

static const CLSID kOurDrop = {0x7f3a9c21, 0x1b4e, 0x4c8a,
                               {0x9e, 0x2d, 0x4a, 0x1f, 0x6b, 0x0c, 0x0d, 0x01}};

static void PrintClsid(const wchar_t* label, const CLSID& c) {
    LPOLESTR s = nullptr;
    StringFromCLSID(c, &s);
    wprintf(L"%s%s\n", label, s ? s : L"<?>");
    if (s) CoTaskMemFree(s);
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* folder = (argc > 1) ? argv[1] : L"C:\\Windows";
    CoInitialize(nullptr);
    wprintf(L"Asking the shell for IDropTarget of: %s\n\n", folder);

    PIDLIST_ABSOLUTE pidl = nullptr;
    HRESULT hr = SHParseDisplayName(folder, nullptr, &pidl, 0, nullptr);
    if (FAILED(hr)) { wprintf(L"SHParseDisplayName failed 0x%08X\n", hr); return 2; }

    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD child = nullptr;
    hr = SHBindToParent(pidl, IID_IShellFolder, (void**)&parent, &child);
    if (FAILED(hr)) { wprintf(L"SHBindToParent failed 0x%08X\n", hr); return 2; }

    // This is the exact call Explorer makes to obtain a drop target for an item.
    IDropTarget* dt = nullptr;
    hr = parent->GetUIObjectOf(nullptr, 1, &child, IID_IDropTarget, nullptr,
                               (void**)&dt);
    if (FAILED(hr) || !dt) {
        wprintf(L"GetUIObjectOf(IID_IDropTarget) failed 0x%08X\n", hr);
        return 2;
    }
    wprintf(L"Got an IDropTarget.\n");

    // Whose is it? A drop handler is initialized via IPersistFile, so if the
    // shell used ours, this object can tell us its CLSID.
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(dt->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf) {
        CLSID id{};
        if (SUCCEEDED(pf->GetClassID(&id))) {
            PrintClsid(L"  its CLSID    : ", id);
            PrintClsid(L"  our handler  : ", kOurDrop);
            wprintf(L"\n==> %s\n", IsEqualCLSID(id, kOurDrop)
                        ? L"IT IS OURS - the shell honours Directory\\shellex\\DropHandler"
                        : L"NOT ours - the shell ignores our DropHandler for folders");
        }
        pf->Release();
    } else {
        wprintf(L"  The object does not implement IPersistFile.\n");
        wprintf(L"  A registered DropHandler would (that is how it is bound to a\n"
                L"  path). So this is the shell's own internal folder drop target.\n");
        wprintf(L"\n==> NOT ours - the shell ignores our DropHandler for folders\n");
    }

    dt->Release();
    parent->Release();
    CoTaskMemFree(pidl);
    CoUninitialize();
    return 0;
}
