#include "CompareUI.h"
#include "../shared/Localize.h"
#include "../shared/Theme.h"
#include "../shared/Util.h"

#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <strsafe.h>

#include <algorithm>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Msimg32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")

namespace angelcopy {

namespace {

constexpr int ID_OVERWRITE = 1201;
constexpr int ID_SKIP      = 1202;
constexpr int ID_KEEPBOTH  = 1203;
constexpr UINT WM_THUMB    = WM_APP + 1;

// CLIENT layout. Two panes of PW; the thumbnail box sits centered in each.
constexpr int CW = 600, CH = 460;
constexpr int PW = 276, PL = 16, PR = 308;
constexpr int TB = 180;          // thumbnail box (square), "small thumbs"
constexpr int TY = 84;           // thumbnail box top
constexpr int IY = TY + TB + 10; // first info line

// PKEY_Image_HorizontalSize / VerticalSize, spelled out so no propsys/uuid
// lib or INITGUID dance is needed for two constants.
const PROPERTYKEY kPkeyWidth = {
    {0x6444048F, 0x4C8B, 0x11D1, {0x8B, 0x70, 0x08, 0x00, 0x36, 0xB1, 0x1A, 0x03}}, 3};
const PROPERTYKEY kPkeyHeight = {
    {0x6444048F, 0x4C8B, 0x11D1, {0x8B, 0x70, 0x08, 0x00, 0x36, 0xB1, 0x1A, 0x03}}, 4};

// ---- thumbnail loader ------------------------------------------------------
// The shell thumbnail of a RAW file on a slow share can take seconds; doing
// it on the UI thread would freeze the dialog. One worker thread with its
// own STA loads requests in order and posts each result back. The loader is
// shared_ptr-owned by the worker too, so the dialog can close (and detach)
// while a slow GetImage is still in flight.

struct ThumbResult {
    size_t pos = 0;
    int side = 0;              // 0 = source, 1 = destination
    HBITMAP bmp = nullptr;
    bool alpha = false;        // has real per-pixel alpha (else draw opaque)
    UINT w = 0, h = 0;         // image dimensions, 0 = unknown
};

struct ThumbLoader {
    std::mutex m;
    std::condition_variable cv;
    struct Job { size_t pos; int side; std::wstring path; };
    std::deque<Job> jobs;
    bool stop = false;
    HWND target = nullptr;

    void Push(Job j, bool front) {
        std::lock_guard<std::mutex> lock(m);
        if (front) jobs.push_front(std::move(j));
        else jobs.push_back(std::move(j));
        cv.notify_one();
    }
};

// Does a 32bpp DIB carry real alpha? Shell thumbnails of JPEGs often come
// back with alpha == 0 everywhere — AlphaBlend would then draw nothing.
bool HasAlpha(HBITMAP bmp) {
    DIBSECTION ds{};
    if (GetObjectW(bmp, sizeof(ds), &ds) != sizeof(ds)) return false;
    if (ds.dsBm.bmBitsPixel != 32 || !ds.dsBm.bmBits) return false;
    const BYTE* p = (const BYTE*)ds.dsBm.bmBits;
    const size_t n = (size_t)ds.dsBm.bmWidthBytes * (size_t)ds.dsBm.bmHeight;
    for (size_t i = 3; i < n; i += 4)
        if (p[i]) return true;
    return false;
}

void LoadThumb(const std::wstring& path, ThumbResult& r) {
    IShellItem* si = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr,
                                           IID_PPV_ARGS(&si))))
        return;
    IShellItemImageFactory* f = nullptr;
    if (SUCCEEDED(si->QueryInterface(IID_PPV_ARGS(&f)))) {
        // RESIZETOFIT: never larger than the box. Types without a
        // thumbnail handler (RAW without codec) fall back to their icon.
        if (FAILED(f->GetImage(SIZE{TB, TB}, SIIGBF_RESIZETOFIT, &r.bmp)))
            r.bmp = nullptr;
        f->Release();
    }
    IShellItem2* s2 = nullptr;
    if (SUCCEEDED(si->QueryInterface(IID_PPV_ARGS(&s2)))) {
        ULONG v = 0;
        if (SUCCEEDED(s2->GetUInt32(kPkeyWidth, &v))) r.w = v;
        if (SUCCEEDED(s2->GetUInt32(kPkeyHeight, &v))) r.h = v;
        s2->Release();
    }
    si->Release();
    if (r.bmp) r.alpha = HasAlpha(r.bmp);
}

void ThumbWorker(std::shared_ptr<ThumbLoader> L) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
        ThumbLoader::Job j;
        {
            std::unique_lock<std::mutex> lock(L->m);
            L->cv.wait(lock, [&] { return L->stop || !L->jobs.empty(); });
            if (L->stop) break;
            j = std::move(L->jobs.front());
            L->jobs.pop_front();
        }
        auto* r = new ThumbResult;
        r->pos = j.pos;
        r->side = j.side;
        LoadThumb(j.path, *r);
        bool posted = false;
        {
            std::lock_guard<std::mutex> lock(L->m);
            if (!L->stop) posted = PostMessageW(L->target, WM_THUMB, 0, (LPARAM)r) != 0;
        }
        if (!posted) {
            if (r->bmp) DeleteObject(r->bmp);
            delete r;
        }
    }
    if (SUCCEEDED(hr)) CoUninitialize();
}

// ---- dialog ----------------------------------------------------------------

struct Side {
    bool requested = false, loaded = false;
    HBITMAP bmp = nullptr;
    bool alpha = false;
    UINT w = 0, h = 0;
};

struct DlgState {
    const std::vector<ConflictItem>* items = nullptr;
    std::vector<size_t> images;         // indices into *items, sorted by dst
    size_t nonImages = 0;
    size_t pos = 0;                     // current index into `images`
    std::vector<Side> side[2];          // per position: source / destination
    RenamePlanner* renamer = nullptr;
    std::unordered_map<std::wstring, FileDecision>* decisions = nullptr;
    std::shared_ptr<ThumbLoader> loader;
    CompareOutcome out;

    HFONT font = nullptr, fontBold = nullptr;
    HWND lblHead = nullptr, lblPath = nullptr, chk = nullptr;
    HWND lblRename = nullptr;           // the name "Keep both" will use
    int hover = -1;                     // thumbnail under the mouse (0/1)
    HWND info[2][3] = {};               // size / dimensions / modified
    FILETIME mtime[2] = {};             // current item, for the "newer" marker
    unsigned long long size[2] = {};

    const ConflictItem& Cur() const { return (*items)[images[pos]]; }
    const std::wstring& PathOf(int s) const { return s == 0 ? Cur().src : Cur().dst; }
};

RECT ThumbRect(int s) {
    int x = (s == 0 ? PL : PR) + (PW - TB) / 2;
    return RECT{x, TY, x + TB, TY + TB};
}

void Request(DlgState* st, size_t pos, bool front) {
    if (pos >= st->images.size()) return;
    const ConflictItem& it = (*st->items)[st->images[pos]];
    for (int s = 0; s < 2; ++s) {
        Side& sd = st->side[s][pos];
        if (sd.requested) continue;
        sd.requested = true;
        st->loader->Push({pos, s, s == 0 ? it.src : it.dst}, front);
    }
}

std::wstring FormatWhen(const FILETIME& ftUtc) {
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&ftUtc, &utc)) return L"\x2014";
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) local = utc;
    wchar_t d[64]{}, t[64]{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr,
                    d, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr,
                    t, 64);
    return std::wstring(d) + L" " + t;
}

unsigned long long U64(const FILETIME& ft) {
    return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

// Dimensions line for side `s`; the "more pixels" marker needs both sides.
void UpdateDims(DlgState* st) {
    const Side& a = st->side[0][st->pos];
    const Side& b = st->side[1][st->pos];
    const unsigned long long pa = (unsigned long long)a.w * a.h;
    const unsigned long long pb = (unsigned long long)b.w * b.h;
    for (int s = 0; s < 2; ++s) {
        const Side& sd = s == 0 ? a : b;
        wchar_t line[128];
        if (sd.w && sd.h) {
            bool more = pa && pb && (s == 0 ? pa > pb : pb > pa);
            StringCchPrintfW(line, 128, loc::T(loc::S::CmpDims), sd.w, sd.h,
                             more ? loc::T(loc::S::CmpMarkMorePixels) : L"");
        } else {
            StringCchCopyW(line, 128, loc::T(loc::S::CmpDimsUnknown));
        }
        SetWindowTextW(st->info[s][1], line);
    }
}

void ShowCurrent(HWND hwnd, DlgState* st) {
    const ConflictItem& it = st->Cur();
    wchar_t head[160];
    StringCchPrintfW(head, 160, loc::T(loc::S::CmpHead),
                     (unsigned long long)st->pos + 1,
                     (unsigned long long)st->images.size());
    SetWindowTextW(st->lblHead, head);
    SetWindowTextW(st->lblPath, it.dst.c_str());

    // Live stat of both sides (the scan's sizes may be seconds old; the
    // dates were never kept). One stat each per shown image — trivial.
    for (int s = 0; s < 2; ++s) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(acutil::ExtLongPath(st->PathOf(s)).c_str(),
                                 GetFileExInfoStandard, &fa)) {
            st->size[s] = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            st->mtime[s] = fa.ftLastWriteTime;
        } else {
            st->size[s] = s == 0 ? it.size : it.dstSize;
            st->mtime[s] = FILETIME{};
        }
    }
    const unsigned long long t0 = U64(st->mtime[0]), t1 = U64(st->mtime[1]);
    const unsigned long long kTol = 20000000ULL; // 2 s, same as ClassifyFile
    for (int s = 0; s < 2; ++s) {
        const int o = 1 - s;
        wchar_t line[160];
        bool larger = st->size[s] > st->size[o];
        StringCchPrintfW(line, 160, loc::T(loc::S::CmpSize),
                         acutil::HumanBytes(st->size[s]).c_str(),
                         larger ? loc::T(loc::S::CmpMarkLarger) : L"");
        SetWindowTextW(st->info[s][0], line);
        const unsigned long long ts = s == 0 ? t0 : t1, to = s == 0 ? t1 : t0;
        bool newer = ts && to && ts > to + kTol;
        StringCchPrintfW(line, 160, loc::T(loc::S::CmpModified),
                         ts ? FormatWhen(st->mtime[s]).c_str() : L"\x2014",
                         newer ? loc::T(loc::S::CmpMarkNewer) : L"");
        SetWindowTextW(st->info[s][2], line);
    }
    UpdateDims(st);

    // Show the exact name "Keep both" will give the incoming file — the
    // choice is only clear when its result is visible.
    wchar_t ren[MAX_PATH + 96];
    StringCchPrintfW(ren, MAX_PATH + 96, loc::T(loc::S::CmpRenameTo),
                     st->renamer->Peek(it).c_str());
    SetWindowTextW(st->lblRename, ren);

    // "Do the same for all remaining": everything after this one, images
    // and non-images alike. Hidden on the very last conflict.
    const unsigned long long rest =
        (unsigned long long)(st->images.size() - st->pos - 1) + st->nonImages;
    if (rest == 0) {
        ShowWindow(st->chk, SW_HIDE);
    } else {
        wchar_t all[160];
        if (rest == 1)
            StringCchCopyW(all, 160, loc::T(loc::S::CmpAllOne));
        else
            StringCchPrintfW(all, 160, loc::T(loc::S::CmpAllMany), rest);
        SetWindowTextW(st->chk, all);
    }

    // Current pair first, then prefetch the next so clicking through stays
    // instant.
    Request(st, st->pos, true);
    Request(st, st->pos + 1, false);
    for (int s = 0; s < 2; ++s) {
        RECT r = ThumbRect(s);
        InvalidateRect(hwnd, &r, FALSE);
    }
}

void Record(DlgState* st, const ConflictItem& it, FileAction a) {
    FileDecision d;
    d.action = a;
    if (a == FileAction::Rename) d.newName = st->renamer->Reserve(it);
    (*st->decisions)[acutil::LowerCopy(it.src)] = std::move(d);
}

void Pick(HWND hwnd, DlgState* st, FileAction a) {
    Record(st, st->Cur(), a);
    const bool all = IsWindowVisible(st->chk) &&
                     SendMessageW(st->chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (all) {
        st->out.restDecided = true;
        if (a == FileAction::Rename) {
            // Keep both needs a name per file — no policy expresses it.
            for (const ConflictItem& it : *st->items)
                if (!st->decisions->count(acutil::LowerCopy(it.src)))
                    Record(st, it, FileAction::Rename);
        } else {
            st->out.restPolicy =
                a == FileAction::Overwrite ? Conflict::Replace : Conflict::Skip;
        }
        st->out.cancelled = false;
        DestroyWindow(hwnd);
        return;
    }
    if (++st->pos >= st->images.size()) {
        st->out.cancelled = false;
        DestroyWindow(hwnd);
        return;
    }
    ShowCurrent(hwnd, st);
}

void PaintThumb(HDC dc, DlgState* st, int s) {
    const theme::Colors& c = theme::C();
    RECT r = ThumbRect(s);
    HBRUSH bg = CreateSolidBrush(c.chartBg);
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    const Side& sd = st->side[s][st->pos];
    if (sd.bmp) {
        BITMAP bm{};
        GetObjectW(sd.bmp, sizeof(bm), &bm);
        int bw = bm.bmWidth, bh = std::abs(bm.bmHeight);
        if (bw <= 0 || bh <= 0) return;
        // Fit inside the box (inset 4 px), never upscale.
        const int box = TB - 8;
        double k = (std::min)(1.0, (std::min)((double)box / bw, (double)box / bh));
        int dw = (std::max)(1, (int)(bw * k)), dh = (std::max)(1, (int)(bh * k));
        int dx = r.left + (TB - dw) / 2, dy = r.top + (TB - dh) / 2;
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, sd.bmp);
        if (sd.alpha) {
            BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            AlphaBlend(dc, dx, dy, dw, dh, mem, 0, 0, bw, bh, bf);
        } else {
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchBlt(dc, dx, dy, dw, dh, mem, 0, 0, bw, bh, SRCCOPY);
        }
        SelectObject(mem, old);
        DeleteDC(mem);
    } else {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, c.text);
        HGDIOBJ of = SelectObject(dc, st->font);
        DrawTextW(dc, sd.loaded ? loc::T(loc::S::CmpNoPreview) : L"\x2026", -1,
                  &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
    }

    // Frame; the hovered side gets a thick accent frame plus a "Keep this
    // one" band — clicking the image IS the choice.
    if (st->hover == s) {
        HBRUSH acc = CreateSolidBrush(c.chartCurve);
        RECT band{r.left, r.bottom - 26, r.right, r.bottom};
        FillRect(dc, &band, acc);
        for (int i = 0; i < 3; ++i) {
            RECT f{r.left + i, r.top + i, r.right - i, r.bottom - i};
            FrameRect(dc, &f, acc);
        }
        DeleteObject(acc);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        HGDIOBJ of = SelectObject(dc, st->fontBold);
        DrawTextW(dc, loc::T(loc::S::CmpKeepThis), -1, &band,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
    } else {
        HBRUSH frame = CreateSolidBrush(c.border);
        FrameRect(dc, &r, frame);
        DeleteObject(frame);
    }
}

void SetHover(HWND hwnd, DlgState* st, int s) {
    if (st->hover == s) return;
    st->hover = s;
    for (int i = 0; i < 2; ++i) {
        RECT r = ThumbRect(i);
        InvalidateRect(hwnd, &r, FALSE);
    }
}

int ThumbAt(LPARAM lp) {
    POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
    for (int s = 0; s < 2; ++s) {
        RECT r = ThumbRect(s);
        if (PtInRect(&r, p)) return s;
    }
    return -1;
}

LRESULT CALLBACK CompareProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DlgState* st = (DlgState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, theme::C().text);
        SetBkColor((HDC)wp, theme::C().bg);
        return (LRESULT)theme::BgBrush();
    case WM_DRAWITEM:
        if (st) theme::DrawButton((const DRAWITEMSTRUCT*)lp, st->font);
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (st && !st->images.empty() && st->pos < st->images.size())
            for (int s = 0; s < 2; ++s) PaintThumb(dc, st, s);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_THUMB: {
        ThumbResult* r = (ThumbResult*)lp;
        if (st && r->pos < st->images.size()) {
            Side& sd = st->side[r->side][r->pos];
            sd.loaded = true;
            sd.bmp = r->bmp;
            sd.alpha = r->alpha;
            sd.w = r->w;
            sd.h = r->h;
            if (r->pos == st->pos) {
                RECT rc = ThumbRect(r->side);
                InvalidateRect(hwnd, &rc, FALSE);
                UpdateDims(st);
            }
        } else if (r->bmp) {
            DeleteObject(r->bmp);
        }
        delete r;
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            if (ThumbAt(MAKELPARAM(p.x, p.y)) >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_MOUSEMOVE:
        if (st) {
            if (st->hover < 0) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            SetHover(hwnd, st, ThumbAt(lp));
        }
        return 0;
    case WM_MOUSELEAVE:
        if (st) SetHover(hwnd, st, -1);
        return 0;
    case WM_LBUTTONUP: {
        // Clicking an image keeps THAT one: the new one overwrites, the
        // existing one means skip. Same as the two buttons, just direct.
        int s = ThumbAt(lp);
        if (st && s >= 0)
            Pick(hwnd, st, s == 0 ? FileAction::Overwrite : FileAction::Skip);
        return 0;
    }
    case WM_RBUTTONUP: {
        // Full-size comparison: open the file in its default viewer.
        int s = ThumbAt(lp);
        if (st && s >= 0)
            ShellExecuteW(hwnd, nullptr, st->PathOf(s).c_str(), nullptr,
                          nullptr, SW_SHOWNORMAL);
        return 0;
    }
    case WM_COMMAND:
        if (!st) break;
        switch (LOWORD(wp)) {
        case ID_OVERWRITE: Pick(hwnd, st, FileAction::Overwrite); return 0;
        case ID_SKIP:      Pick(hwnd, st, FileAction::Skip);      return 0;
        case ID_KEEPBOTH:  Pick(hwnd, st, FileAction::Rename);    return 0;
        case IDCANCEL:     st->out.cancelled = true; DestroyWindow(hwnd); return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd); // out.cancelled stays true
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

using theme::SetFont;

} // namespace

CompareOutcome AskCompareImages(
    const std::vector<ConflictItem>& items, RenamePlanner& renamer,
    std::unordered_map<std::wstring, FileDecision>& decisions) {
    DlgState st;
    st.items = &items;
    st.renamer = &renamer;
    st.decisions = &decisions;
    st.out.cancelled = true; // closing the window == cancel
    for (size_t i = 0; i < items.size(); ++i) {
        if (IsImageFile(items[i].dst)) st.images.push_back(i);
        else ++st.nonImages;
    }
    if (st.images.empty()) { st.out.cancelled = false; return st.out; }
    std::sort(st.images.begin(), st.images.end(), [&](size_t a, size_t b) {
        return acutil::LowerCopy(items[a].dst) < acutil::LowerCopy(items[b].dst);
    });
    st.side[0].resize(st.images.size());
    st.side[1].resize(st.images.size());

    // ShellExecute (click-to-open) wants an STA on this thread.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    const DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    HWND hwnd = theme::CreateCenteredWindow(CompareProc, L"AngelCopyCompare",
                                            loc::T(loc::S::CmpCaption), CW, CH,
                                            kStyle, WS_EX_TOPMOST);
    if (!hwnd) {
        if (SUCCEEDED(com)) CoUninitialize();
        return st.out; // can't ask -> cancel (safe default)
    }

    theme::UiFonts fonts;
    st.font = fonts.normal;
    st.fontBold = fonts.bold;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&st);

    auto label = [&](const wchar_t* text, int x, int y, int w, DWORD extra = 0) {
        HWND h = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_NOPREFIX | extra,
                               x, y, w, 18, hwnd, nullptr, hInst, nullptr);
        SetFont(h, st.font);
        return h;
    };
    st.lblHead = label(L"", 16, 12, CW - 32);
    SetFont(st.lblHead, st.fontBold);
    st.lblPath = label(L"", 16, 34, CW - 32, SS_PATHELLIPSIS);
    HWND capL = label(loc::T(loc::S::CmpNew), PL, 62, PW, SS_CENTER);
    HWND capR = label(loc::T(loc::S::CmpExisting), PR, 62, PW, SS_CENTER);
    SetFont(capL, st.fontBold);
    SetFont(capR, st.fontBold);
    for (int s = 0; s < 2; ++s)
        for (int l = 0; l < 3; ++l)
            st.info[s][l] = label(L"", s == 0 ? PL : PR, IY + l * 20, PW);
    label(loc::T(loc::S::CmpClickHint), 16, IY + 66, CW - 32);
    st.lblRename = label(L"", 16, IY + 86, CW - 32, SS_PATHELLIPSIS);

    st.chk = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                           16, IY + 112, CW - 32, 20, hwnd, nullptr, hInst, nullptr);
    SetFont(st.chk, st.font);

    const int by = IY + 142, bh = 30;
    auto button = [&](loc::S text, int id, int x, int w, bool def) {
        HWND b = CreateWindowW(L"BUTTON", loc::T(text),
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | theme::ButtonStyle(def),
                               x, by, w, bh, hwnd, (HMENU)(INT_PTR)id, hInst, nullptr);
        SetFont(b, st.font);
        theme::ApplyToControl(b);
        return b;
    };
    // Wording follows the images: keep the NEW one / keep the EXISTING one
    // ("Vorhandenes behalten" is long in German — widest slot).
    button(loc::S::BtnOverwrite, ID_OVERWRITE, 16, 140, false);
    // Keep existing is the default: the least destructive answer.
    HWND bSkip = button(loc::S::BtnSkipOne, ID_SKIP, 164, 160, true);
    button(loc::S::BtnKeepBoth, ID_KEEPBOTH, 332, 144, false);
    button(loc::S::BtnCancel, IDCANCEL, CW - 116, 100, false);

    theme::ApplyToWindow(hwnd);
    theme::ApplyToControl(st.chk);

    st.loader = std::make_shared<ThumbLoader>();
    st.loader->target = hwnd;
    std::thread(ThumbWorker, st.loader).detach();

    ShowCurrent(hwnd, &st);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(bSkip);

    theme::RunModalLoop(hwnd);

    // Stop the worker (it owns its share of the loader; a slow GetImage in
    // flight finishes on its own and its result is discarded), then free
    // every bitmap that arrived.
    {
        std::lock_guard<std::mutex> lock(st.loader->m);
        st.loader->stop = true;
        st.loader->jobs.clear();
    }
    st.loader->cv.notify_all();
    for (auto& v : st.side)
        for (Side& sd : v)
            if (sd.bmp) DeleteObject(sd.bmp);
    // Drain results posted before the stop so their bitmaps don't leak.
    MSG m;
    while (PeekMessageW(&m, nullptr, WM_THUMB, WM_THUMB, PM_REMOVE)) {
        ThumbResult* r = (ThumbResult*)m.lParam;
        if (r->bmp) DeleteObject(r->bmp);
        delete r;
    }

    if (SUCCEEDED(com)) CoUninitialize();
    return st.out;
}

} // namespace angelcopy
