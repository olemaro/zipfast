// zipfast_gui.cpp — ZIP packer & extractor for Windows 10/11
// Build:
//   python create_icon.py
//   windres resource.rc -o resource.o
//   g++ -O2 -std=c++17 -static -mwindows zipfast_gui.cpp miniz.c resource.o
//       -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luuid -o zipfast.exe

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define MINIZ_IMPLEMENTATION
#include "miniz.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <oleidl.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;

// ── control IDs ───────────────────────────────────────────────────────────────
enum {
    IDC_TAB                = 50,
    // Pack tab
    IDC_LIST               = 100,
    IDC_ADD_FILES          = 101,
    IDC_ADD_FOLDER         = 102,
    IDC_DEL_SEL            = 103,
    IDC_CLEAR              = 104,
    IDC_OUTPUT             = 105,
    IDC_BROWSE             = 106,
    IDC_LEVEL              = 107,
    IDC_PACK               = 108,
    IDC_OUT_LABEL          = 109,
    IDC_LEVEL_LABEL        = 110,
    IDC_DRAG_BTN           = 111,
    // Extract tab
    IDC_ZIP_PATH           = 120,
    IDC_ZIP_BROWSE         = 121,
    IDC_EXTRACT_DIR        = 122,
    IDC_EXTRACT_DIR_BROWSE = 123,
    IDC_EXTRACT            = 124,
    IDC_EXTRACT_HINT       = 125,
    IDC_ZIP_LABEL          = 126,
    IDC_EXTRACT_DIR_LABEL  = 127,
    // Shared
    IDC_PROGRESS           = 140,
    IDC_STATUS             = 141,
};

#define IDI_MAIN 1

#define WM_PACK_PROGRESS    (WM_APP + 1)
#define WM_PACK_DONE        (WM_APP + 2)
#define WM_EXTRACT_PROGRESS (WM_APP + 3)
#define WM_EXTRACT_DONE     (WM_APP + 4)
#define WM_PACK_WRITE       (WM_APP + 5)   // wParam=done, lParam=total

// ── types ─────────────────────────────────────────────────────────────────────
struct SourceItem { fs::path path; };
struct FileEntry  { std::string disk_path; std::string zip_name; };

struct CompressedEntry {
    std::size_t           file_index    = 0;
    std::vector<mz_uint8> data;
    mz_uint32             original_crc  = 0;
    std::size_t           original_size = 0;
    int                   method        = 0;
    bool                  ok            = false;
};

struct PackResult {
    int    error_count;
    double elapsed_seconds;
    size_t total_files;
    size_t total_original_bytes;
    size_t total_compressed_bytes;
};

struct ExtractResult {
    int    error_count;
    double elapsed_seconds;
    size_t total_files;
};

// ── globals ───────────────────────────────────────────────────────────────────
static HWND g_hwnd        = nullptr;
static HWND g_tab         = nullptr;
static HWND g_progress    = nullptr;
static HWND g_status      = nullptr;
// Pack
static HWND g_list        = nullptr;
static HWND g_output      = nullptr;
static HWND g_level_cmb   = nullptr;
static HWND g_pack_btn    = nullptr;
static HWND g_drag_btn    = nullptr;
// Extract
static HWND g_zip_path    = nullptr;
static HWND g_extract_dir = nullptr;
static HWND g_extract_btn = nullptr;

static std::vector<SourceItem> g_sources;
static bool g_packing       = false;
static bool g_extracting    = false;
static bool g_last_pack_ok  = false;   // enables drag after successful pack

// ── OLE drag source ───────────────────────────────────────────────────────────

class SimpleDropSource : public IDropSource {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {
        if (iid == IID_IUnknown || iid == IID_IDropSource)
            { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD keys) override {
        if (esc)              return DRAGDROP_S_CANCEL;
        if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }
private:
    ULONG ref_ = 1;
};

class FileDataObject : public IDataObject {
public:
    explicit FileDataObject(const std::wstring& path) {
        size_t path_chars = path.size() + 1;
        size_t total = sizeof(DROPFILES) + path_chars * sizeof(wchar_t) + sizeof(wchar_t);
        global_ = GlobalAlloc(GHND, total);
        if (!global_) return;
        auto* df = static_cast<DROPFILES*>(GlobalLock(global_));
        df->pFiles = sizeof(DROPFILES);
        df->fWide  = TRUE;
        wchar_t* dest = reinterpret_cast<wchar_t*>(
            reinterpret_cast<char*>(df) + sizeof(DROPFILES));
        wcscpy_s(dest, path_chars, path.c_str());
        GlobalUnlock(global_);
    }
    ~FileDataObject() { if (global_) GlobalFree(global_); }

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {
        if (iid == IID_IUnknown || iid == IID_IDataObject)
            { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }

    // IDataObject — only CF_HDROP / TYMED_HGLOBAL needed for file drag
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fmt, STGMEDIUM* med) override {
        if (!is_supported(fmt)) return DV_E_FORMATETC;
        size_t sz = GlobalSize(global_);
        med->tymed = TYMED_HGLOBAL;
        med->hGlobal = GlobalAlloc(GHND, sz);
        if (!med->hGlobal) return E_OUTOFMEMORY;
        void* src = GlobalLock(global_);
        void* dst = GlobalLock(med->hGlobal);
        memcpy(dst, src, sz);
        GlobalUnlock(global_);
        GlobalUnlock(med->hGlobal);
        med->pUnkForRelease = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fmt) override {
        return is_supported(fmt) ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) override {
        if (dir != DATADIR_GET) return E_NOTIMPL;
        FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return SHCreateStdEnumFmtEtc(1, &fmt, out);
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* o) override
        { o->ptd = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
        { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    bool is_supported(FORMATETC* fmt) const {
        return fmt && fmt->cfFormat == CF_HDROP
            && (fmt->tymed & TYMED_HGLOBAL)
            && fmt->dwAspect == DVASPECT_CONTENT;
    }
    HGLOBAL global_ = nullptr;
    ULONG   ref_    = 1;
};

static void start_drag(const std::wstring& file_path) {
    if (file_path.empty()) return;
    auto* data   = new FileDataObject(file_path);
    auto* source = new SimpleDropSource();
    DWORD effect = 0;
    DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
    data->Release();
    source->Release();
}

// Subclass proc for the drag button — converts click+drag into OLE drag
static LRESULT CALLBACK drag_btn_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                           UINT_PTR, DWORD_PTR) {
    static bool  tracking = false;
    static POINT press_pt = {};

    switch (msg) {
    case WM_LBUTTONDOWN:
        tracking = true;
        press_pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SetCapture(hwnd);
        return 0;

    case WM_MOUSEMOVE:
        if (tracking) {
            int dx = GET_X_LPARAM(lp) - press_pt.x;
            int dy = GET_Y_LPARAM(lp) - press_pt.y;
            if (abs(dx) > GetSystemMetrics(SM_CXDRAG) ||
                abs(dy) > GetSystemMetrics(SM_CYDRAG)) {
                tracking = false;
                ReleaseCapture();
                WCHAR buf[MAX_PATH] = {};
                GetWindowText(g_output, buf, MAX_PATH);
                if (*buf && g_last_pack_ok)
                    start_drag(std::wstring(buf));
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (tracking) {
            tracking = false;
            ReleaseCapture();   // освобождаем захват даже без перетаскивания
        }
        return 0;

    case WM_CAPTURECHANGED:
        tracking = false;       // захват передан другому — просто сбрасываем флаг
        return 0;

    case WM_SETCURSOR:
        if (IsWindowEnabled(hwnd)) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ── compression helpers ───────────────────────────────────────────────────────
static const char* COMPRESSED_EXTS[] = {
    ".zip",".7z",".rar",".gz",".bz2",".xz",".zst",
    ".jpg",".jpeg",".png",".gif",".webp",".avif",
    ".mp4",".mkv",".avi",".mov",".mp3",".aac",".flac",
    ".docx",".xlsx",".pptx",".pdf", nullptr
};

static bool already_compressed(const fs::path& path) {
    std::string ext = path.extension().string();
    for (char& ch : ext) ch = (char)std::tolower((unsigned char)ch);
    for (int i = 0; COMPRESSED_EXTS[i]; ++i)
        if (ext == COMPRESSED_EXTS[i]) return true;
    return false;
}

static std::vector<mz_uint8> read_file_bytes(const std::string& path) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::vector<mz_uint8> buf(static_cast<size_t>(sz.QuadPart));
    size_t off = 0; DWORD got = 0;
    while (off < buf.size()) {
        DWORD want = (DWORD)std::min(buf.size() - off, (size_t)(64u << 20));
        if (!ReadFile(h, buf.data() + off, want, &got, nullptr) || !got) break;
        off += got;
    }
    CloseHandle(h);
    buf.resize(off);
    return buf;
}

static CompressedEntry compress_entry(size_t index, const FileEntry& entry, int level) {
    CompressedEntry result;
    result.file_index = index;
    auto raw = read_file_bytes(entry.disk_path);
    result.original_size = raw.size();
    result.original_crc  = (mz_uint32)mz_crc32(MZ_CRC32_INIT, raw.data(), raw.size());
    bool skip = already_compressed(fs::path(entry.disk_path)) || raw.size() < 64 || level == 0;
    if (!skip) {
        int flags = tdefl_create_comp_flags_from_zip_params(
            level, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
        size_t comp_len = 0;
        void* comp = tdefl_compress_mem_to_heap(raw.data(), raw.size(), &comp_len, flags);
        if (comp && comp_len < raw.size()) {
            result.data.assign((mz_uint8*)comp, (mz_uint8*)comp + comp_len);
            mz_free(comp);
            result.method = MZ_DEFLATED;
            result.ok = true;
            return result;
        }
        if (comp) mz_free(comp);
    }
    result.data   = std::move(raw);
    result.method = 0;
    result.ok     = true;
    return result;
}

// ── thread pool ───────────────────────────────────────────────────────────────
class ThreadPool {
public:
    explicit ThreadPool(unsigned count) : stop_(false) {
        for (unsigned i = 0; i < count; ++i)
            workers_.emplace_back([this] { run(); });
    }
    ~ThreadPool() {
        { std::unique_lock<std::mutex> lk(mutex_); stop_ = true; }
        condition_.notify_all();
        for (auto& w : workers_) w.join();
    }
    void submit(std::function<void()> task) {
        { std::unique_lock<std::mutex> lk(mutex_); queue_.push(std::move(task)); }
        condition_.notify_one();
    }
private:
    void run() {
        for (;;) {
            std::function<void()> task;
            { std::unique_lock<std::mutex> lk(mutex_);
              condition_.wait(lk, [this] { return stop_ || !queue_.empty(); });
              if (stop_ && queue_.empty()) return;
              task = std::move(queue_.front()); queue_.pop(); }
            task();
        }
    }
    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> queue_;
    std::mutex                        mutex_;
    std::condition_variable           condition_;
    bool                              stop_;
};

// ── file collection ───────────────────────────────────────────────────────────
static void collect_entries(const fs::path& input, std::vector<FileEntry>& out) {
    std::error_code ec;
    if (fs::is_regular_file(input, ec)) {
        out.push_back({input.string(), input.filename().string()});
        return;
    }
    if (!fs::is_directory(input, ec)) return;
    fs::path base = input.parent_path();
    for (const auto& entry : fs::recursive_directory_iterator(
            input, fs::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_regular_file()) continue;
        auto rel = fs::relative(entry.path(), base, ec);
        std::string zip_name = rel.string();
        std::replace(zip_name.begin(), zip_name.end(), '\\', '/');
        out.push_back({entry.path().string(), zip_name});
    }
}

// ── pack thread ───────────────────────────────────────────────────────────────
static void pack_thread(std::vector<SourceItem> sources, std::string output_path, int level) {
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    std::vector<FileEntry> files;
    for (const auto& src : sources) collect_entries(src.path, files);
    if (files.empty()) {
        PostMessage(g_hwnd, WM_PACK_DONE, 0, (LPARAM)(new PackResult{0,0,0,0,0}));
        return;
    }

    unsigned nthreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<CompressedEntry> compressed(files.size());
    std::atomic<size_t> done_count{0};
    {
        ThreadPool pool(nthreads);
        for (size_t i = 0; i < files.size(); ++i) {
            pool.submit([&, i] {
                compressed[i] = compress_entry(i, files[i], level);
                size_t done = done_count.fetch_add(1) + 1;
                PostMessage(g_hwnd, WM_PACK_PROGRESS, (WPARAM)done, (LPARAM)files.size());
            });
        }
    }

    mz_zip_archive zip{};
    int    errors     = 0;
    size_t orig_total = 0, comp_total = 0;

    // Reset progress bar for write phase
    PostMessage(g_hwnd, WM_PACK_WRITE, 0, (LPARAM)files.size());

    if (mz_zip_writer_init_file(&zip, output_path.c_str(), 0)) {
        for (size_t i = 0; i < files.size(); ++i) {
            const auto& e = compressed[i];
            if (!e.ok) { ++errors; continue; }
            orig_total += e.original_size;
            comp_total += e.data.size();
            bool ok = false;
            if (e.method == MZ_DEFLATED) {
                ok = mz_zip_writer_add_mem_ex(&zip,
                    files[i].zip_name.c_str(), e.data.data(), e.data.size(),
                    nullptr, 0, MZ_ZIP_FLAG_COMPRESSED_DATA,
                    (mz_uint64)e.original_size, e.original_crc) == MZ_TRUE;
            } else {
                ok = mz_zip_writer_add_mem_ex(&zip,
                    files[i].zip_name.c_str(), e.data.data(), e.data.size(),
                    nullptr, 0, 0, 0, 0) == MZ_TRUE;
            }
            if (!ok) ++errors;
            PostMessage(g_hwnd, WM_PACK_WRITE, (WPARAM)(i + 1), (LPARAM)files.size());
        }
        mz_zip_writer_finalize_archive(&zip);
        mz_zip_writer_end(&zip);
    } else {
        errors = -1;
    }

    QueryPerformanceCounter(&t1);
    double elapsed = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
    PostMessage(g_hwnd, WM_PACK_DONE, 0,
        (LPARAM)(new PackResult{errors, elapsed, files.size(), orig_total, comp_total}));
}

// ── extract thread ────────────────────────────────────────────────────────────
static void extract_thread(std::string zip_path, std::string output_dir) {
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) {
        PostMessage(g_hwnd, WM_EXTRACT_DONE, 0, (LPARAM)(new ExtractResult{-1,0,0}));
        return;
    }

    mz_uint count = mz_zip_reader_get_num_files(&zip);
    int errors = 0;
    for (mz_uint i = 0; i < count; ++i) {
        PostMessage(g_hwnd, WM_EXTRACT_PROGRESS, (WPARAM)(i+1), (LPARAM)count);
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) { ++errors; continue; }
        fs::path out = fs::path(output_dir) / fs::path(stat.m_filename);
        std::error_code ec;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            fs::create_directories(out, ec);
            continue;
        }
        fs::create_directories(out.parent_path(), ec);
        if (!mz_zip_reader_extract_to_file(&zip, i, out.string().c_str(), 0))
            ++errors;
    }
    mz_zip_reader_end(&zip);

    QueryPerformanceCounter(&t1);
    double elapsed = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
    PostMessage(g_hwnd, WM_EXTRACT_DONE, 0,
        (LPARAM)(new ExtractResult{errors, elapsed, count}));
}

// ── UI helpers ────────────────────────────────────────────────────────────────
static void refresh_list() {
    SendMessage(g_list, LB_RESETCONTENT, 0, 0);
    if (g_sources.empty())
        SendMessage(g_list, LB_ADDSTRING, 0, (LPARAM)L"    Drop files or folders here...");
    else
        for (const auto& src : g_sources)
            SendMessage(g_list, LB_ADDSTRING, 0, (LPARAM)src.path.wstring().c_str());
}

static void add_source(const fs::path& path) {
    for (const auto& existing : g_sources)
        if (existing.path == path) return;
    g_sources.push_back({path});
    if (g_sources.size() == 1) {
        fs::path suggestion = path.parent_path() / (path.stem().wstring() + L".zip");
        SetWindowText(g_output, suggestion.wstring().c_str());
    }
    refresh_list();
}

static void show_tab(int tab_index) {
    static const int PACK_IDS[]    = {IDC_LIST, IDC_ADD_FILES, IDC_ADD_FOLDER,
                                       IDC_DEL_SEL, IDC_CLEAR, IDC_OUTPUT, IDC_BROWSE,
                                       IDC_LEVEL, IDC_PACK, IDC_OUT_LABEL,
                                       IDC_LEVEL_LABEL, IDC_DRAG_BTN};
    static const int EXTRACT_IDS[] = {IDC_ZIP_PATH, IDC_ZIP_BROWSE, IDC_EXTRACT_DIR,
                                       IDC_EXTRACT_DIR_BROWSE, IDC_EXTRACT,
                                       IDC_EXTRACT_HINT, IDC_ZIP_LABEL, IDC_EXTRACT_DIR_LABEL};
    for (int id : PACK_IDS)
        ShowWindow(GetDlgItem(g_hwnd, id), tab_index == 0 ? SW_SHOW : SW_HIDE);
    for (int id : EXTRACT_IDS)
        ShowWindow(GetDlgItem(g_hwnd, id), tab_index == 1 ? SW_SHOW : SW_HIDE);
}

static void set_pack_enabled(bool enabled) {
    for (int id : {IDC_ADD_FILES, IDC_ADD_FOLDER, IDC_DEL_SEL, IDC_CLEAR, IDC_BROWSE})
        EnableWindow(GetDlgItem(g_hwnd, id), enabled);
    EnableWindow(g_level_cmb, enabled);
    EnableWindow(g_pack_btn,  enabled);
    SetWindowText(g_pack_btn, enabled ? L"Pack" : L"Packing...");
    // Drag button enabled only after successful pack and when idle
    EnableWindow(g_drag_btn, enabled && g_last_pack_ok);
}

static void set_extract_enabled(bool enabled) {
    for (int id : {IDC_ZIP_BROWSE, IDC_EXTRACT_DIR_BROWSE})
        EnableWindow(GetDlgItem(g_hwnd, id), enabled);
    EnableWindow(g_zip_path,    enabled);
    EnableWindow(g_extract_dir, enabled);
    EnableWindow(g_extract_btn, enabled);
    SetWindowText(g_extract_btn, enabled ? L"Extract All Files" : L"Extracting...");
}

static void apply_system_font(HWND hwnd) {
    NONCLIENTMETRICS ncm{ sizeof(ncm) };
    SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    HFONT font = CreateFontIndirect(&ncm.lfMessageFont);
    SendMessage(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
    EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
        SendMessage(child, WM_SETFONT, (WPARAM)lp, TRUE);
        return TRUE;
    }, (LPARAM)font);
}

// ── window procedure ──────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        g_hwnd = hwnd;

        // Tab control
        g_tab = CreateWindow(WC_TABCONTROL, nullptr,
            WS_CHILD | WS_VISIBLE | TCS_FIXEDWIDTH,
            0, 0, 530, 26, hwnd, (HMENU)IDC_TAB, nullptr, nullptr);
        TCITEM tab_item{ TCIF_TEXT };
        tab_item.pszText = (LPWSTR)L"  Pack  ";
        TabCtrl_InsertItem(g_tab, 0, &tab_item);
        tab_item.pszText = (LPWSTR)L"  Extract  ";
        TabCtrl_InsertItem(g_tab, 1, &tab_item);

        // ── Pack tab ──────────────────────────────────────────────────────────
        g_list = CreateWindowEx(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_EXTENDEDSEL,
            10, 32, 510, 178, hwnd, (HMENU)IDC_LIST, nullptr, nullptr);

        CreateWindow(L"BUTTON", L"Add Files",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            10, 218, 118, 27, hwnd, (HMENU)IDC_ADD_FILES, nullptr, nullptr);
        CreateWindow(L"BUTTON", L"Add Folder",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            136, 218, 118, 27, hwnd, (HMENU)IDC_ADD_FOLDER, nullptr, nullptr);
        CreateWindow(L"BUTTON", L"Remove Selected",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            262, 218, 130, 27, hwnd, (HMENU)IDC_DEL_SEL, nullptr, nullptr);
        CreateWindow(L"BUTTON", L"Clear All",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            400, 218, 120, 27, hwnd, (HMENU)IDC_CLEAR, nullptr, nullptr);

        CreateWindow(L"STATIC", L"Output file:",
            WS_CHILD|WS_VISIBLE|SS_LEFT,
            10, 259, 80, 18, hwnd, (HMENU)IDC_OUT_LABEL, nullptr, nullptr);
        g_output = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            94, 255, 320, 24, hwnd, (HMENU)IDC_OUTPUT, nullptr, nullptr);
        CreateWindow(L"BUTTON", L"Browse...",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            420, 255, 100, 24, hwnd, (HMENU)IDC_BROWSE, nullptr, nullptr);

        CreateWindow(L"STATIC", L"Compression:",
            WS_CHILD|WS_VISIBLE|SS_LEFT,
            10, 294, 82, 18, hwnd, (HMENU)IDC_LEVEL_LABEL, nullptr, nullptr);
        g_level_cmb = CreateWindow(L"COMBOBOX", nullptr,
            WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
            96, 290, 250, 120, hwnd, (HMENU)IDC_LEVEL, nullptr, nullptr);
        SendMessage(g_level_cmb, CB_ADDSTRING, 0, (LPARAM)L"0 — Store (no compression, fastest)");
        SendMessage(g_level_cmb, CB_ADDSTRING, 0, (LPARAM)L"1 — Fastest");
        SendMessage(g_level_cmb, CB_ADDSTRING, 0, (LPARAM)L"6 — Default (balanced)");
        SendMessage(g_level_cmb, CB_ADDSTRING, 0, (LPARAM)L"9 — Best compression");
        SendMessage(g_level_cmb, CB_SETCURSEL, 2, 0);

        // Pack + Drag buttons side by side
        g_pack_btn = CreateWindow(L"BUTTON", L"Pack",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_DEFPUSHBUTTON,
            10, 325, 390, 36, hwnd, (HMENU)IDC_PACK, nullptr, nullptr);

        g_drag_btn = CreateWindow(L"BUTTON", L"Drag ZIP →",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            408, 325, 112, 36, hwnd, (HMENU)IDC_DRAG_BTN, nullptr, nullptr);
        EnableWindow(g_drag_btn, FALSE);
        SetWindowSubclass(g_drag_btn, drag_btn_subclass, 1, 0);

        // ── Extract tab ───────────────────────────────────────────────────────
        CreateWindowEx(WS_EX_STATICEDGE, L"STATIC",
            L"Drag a ZIP file onto this window, or use Browse below",
            WS_CHILD|SS_CENTER|SS_CENTERIMAGE,
            10, 32, 510, 48, hwnd, (HMENU)IDC_EXTRACT_HINT, nullptr, nullptr);

        CreateWindow(L"STATIC", L"ZIP file:",
            WS_CHILD|SS_LEFT,
            10, 96, 62, 18, hwnd, (HMENU)IDC_ZIP_LABEL, nullptr, nullptr);
        g_zip_path = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD|ES_AUTOHSCROLL,
            76, 92, 338, 24, hwnd, (HMENU)IDC_ZIP_PATH, nullptr, nullptr);
        CreateWindow(L"BUTTON", L"Browse...",
            WS_CHILD|BS_PUSHBUTTON,
            420, 92, 100, 24, hwnd, (HMENU)IDC_ZIP_BROWSE, nullptr, nullptr);

        CreateWindow(L"STATIC", L"Extract to:",
            WS_CHILD|SS_LEFT,
            10, 130, 62, 18, hwnd, (HMENU)IDC_EXTRACT_DIR_LABEL, nullptr, nullptr);
        g_extract_dir = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD|ES_AUTOHSCROLL,
            76, 126, 338, 24, hwnd, (HMENU)IDC_EXTRACT_DIR, nullptr, nullptr);
        CreateWindow(L"BUTTON", L"Browse...",
            WS_CHILD|BS_PUSHBUTTON,
            420, 126, 100, 24, hwnd, (HMENU)IDC_EXTRACT_DIR_BROWSE, nullptr, nullptr);

        g_extract_btn = CreateWindow(L"BUTTON", L"Extract All Files",
            WS_CHILD|BS_PUSHBUTTON|BS_DEFPUSHBUTTON,
            10, 162, 510, 36, hwnd, (HMENU)IDC_EXTRACT, nullptr, nullptr);

        // ── Shared ────────────────────────────────────────────────────────────
        g_progress = CreateWindowEx(0, PROGRESS_CLASS, nullptr,
            WS_CHILD|WS_VISIBLE|PBS_SMOOTH,
            10, 372, 510, 18, hwnd, (HMENU)IDC_PROGRESS, nullptr, nullptr);
        g_status = CreateWindow(L"STATIC", L"Ready.",
            WS_CHILD|WS_VISIBLE|SS_LEFT,
            10, 396, 510, 18, hwnd, (HMENU)IDC_STATUS, nullptr, nullptr);

        apply_system_font(hwnd);
        DragAcceptFiles(hwnd, TRUE);
        show_tab(0);
        refresh_list();
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lp;
        if (nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE) {
            show_tab(TabCtrl_GetCurSel(g_tab));
            SendMessage(g_progress, PBM_SETPOS, 0, 0);
            SetWindowText(g_status, L"Ready.");
        }
        return 0;
    }

    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        UINT count = DragQueryFile(drop, 0xFFFFFFFF, nullptr, 0);
        int current_tab = TabCtrl_GetCurSel(g_tab);
        if (current_tab == 1) {
            WCHAR buf[MAX_PATH];
            DragQueryFile(drop, 0, buf, MAX_PATH);
            fs::path dropped(buf);
            std::wstring ext = dropped.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (ext == L".zip") {
                SetWindowText(g_zip_path, buf);
                WCHAR dir_buf[MAX_PATH] = {};
                GetWindowText(g_extract_dir, dir_buf, MAX_PATH);
                if (!*dir_buf)
                    SetWindowText(g_extract_dir, dropped.parent_path().wstring().c_str());
            }
        } else {
            for (UINT i = 0; i < count; ++i) {
                WCHAR buf[MAX_PATH];
                DragQueryFile(drop, i, buf, MAX_PATH);
                add_source(fs::path(buf));
            }
        }
        DragFinish(drop);
        SetForegroundWindow(hwnd);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);

        if (id == IDC_CLEAR) {
            g_sources.clear(); refresh_list();
            SetWindowText(g_status, L"List cleared.");

        } else if (id == IDC_DEL_SEL) {
            int n = (int)SendMessage(g_list, LB_GETCOUNT, 0, 0);
            for (int i = n - 1; i >= 0; --i)
                if (SendMessage(g_list, LB_GETSEL, i, 0) > 0 && i < (int)g_sources.size())
                    g_sources.erase(g_sources.begin() + i);
            refresh_list();

        } else if (id == IDC_ADD_FILES) {
            WCHAR buf[32768] = {};
            OPENFILENAME ofn{};
            ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd;
            ofn.lpstrFile = buf; ofn.nMaxFile = 32768;
            ofn.lpstrTitle = L"Select files to pack";
            ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST;
            if (GetOpenFileName(&ofn)) {
                WCHAR* first = buf + ofn.nFileOffset;
                WCHAR* after = first + wcslen(first) + 1;
                if (*after == L'\0') {
                    add_source(fs::path(buf));
                } else {
                    std::wstring dir(buf, ofn.nFileOffset > 0 ? ofn.nFileOffset - 1 : 0);
                    for (WCHAR* p = first; *p; p += wcslen(p) + 1)
                        add_source(fs::path(dir) / p);
                }
            }

        } else if (id == IDC_ADD_FOLDER) {
            BROWSEINFO bi{};
            bi.hwndOwner = hwnd; bi.lpszTitle = L"Select a folder to pack";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
            LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
            if (pidl) {
                WCHAR path[MAX_PATH];
                if (SHGetPathFromIDList(pidl, path)) add_source(fs::path(path));
                CoTaskMemFree(pidl);
            }

        } else if (id == IDC_BROWSE) {
            WCHAR buf[MAX_PATH] = {};
            GetWindowText(g_output, buf, MAX_PATH);
            OPENFILENAME ofn{};
            ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd;
            ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = L"ZIP archive (*.zip)\0*.zip\0All files\0*.*\0";
            ofn.lpstrDefExt = L"zip"; ofn.lpstrTitle = L"Save archive as";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileName(&ofn)) SetWindowText(g_output, buf);

        } else if (id == IDC_PACK && !g_packing) {
            if (g_sources.empty()) {
                MessageBox(hwnd, L"Add files or folders to the list first.",
                           L"zipfast", MB_ICONWARNING|MB_OK); return 0;
            }
            WCHAR out_buf[MAX_PATH] = {};
            GetWindowText(g_output, out_buf, MAX_PATH);
            if (!*out_buf) {
                MessageBox(hwnd, L"Specify the output ZIP file path.",
                           L"zipfast", MB_ICONWARNING|MB_OK); return 0;
            }
            static const int LEVEL_MAP[] = {0, 1, 6, 9};
            int sel   = (int)SendMessage(g_level_cmb, CB_GETCURSEL, 0, 0);
            int level = LEVEL_MAP[sel >= 0 && sel < 4 ? sel : 2];

            g_packing = true; g_last_pack_ok = false;
            set_pack_enabled(false);
            SendMessage(g_progress, PBM_SETPOS, 0, 0);
            SetWindowText(g_status, L"Packing...");

            std::string output_utf8 = fs::path(out_buf).string();
            auto sources_copy = g_sources;
            std::thread([sources_copy, output_utf8, level] {
                pack_thread(sources_copy, output_utf8, level);
            }).detach();

        } else if (id == IDC_ZIP_BROWSE) {
            WCHAR buf[MAX_PATH] = {};
            GetWindowText(g_zip_path, buf, MAX_PATH);
            OPENFILENAME ofn{};
            ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd;
            ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = L"ZIP archive (*.zip)\0*.zip\0All files\0*.*\0";
            ofn.lpstrTitle = L"Open ZIP archive";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileName(&ofn)) {
                SetWindowText(g_zip_path, buf);
                WCHAR dir_buf[MAX_PATH] = {};
                GetWindowText(g_extract_dir, dir_buf, MAX_PATH);
                if (!*dir_buf)
                    SetWindowText(g_extract_dir, fs::path(buf).parent_path().wstring().c_str());
            }

        } else if (id == IDC_EXTRACT_DIR_BROWSE) {
            BROWSEINFO bi{};
            bi.hwndOwner = hwnd; bi.lpszTitle = L"Select extraction destination folder";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
            LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
            if (pidl) {
                WCHAR path[MAX_PATH];
                if (SHGetPathFromIDList(pidl, path)) SetWindowText(g_extract_dir, path);
                CoTaskMemFree(pidl);
            }

        } else if (id == IDC_EXTRACT && !g_extracting) {
            WCHAR zip_buf[MAX_PATH] = {};
            GetWindowText(g_zip_path, zip_buf, MAX_PATH);
            if (!*zip_buf) {
                MessageBox(hwnd, L"Select or drop a ZIP file first.",
                           L"zipfast", MB_ICONWARNING|MB_OK); return 0;
            }
            WCHAR dir_buf[MAX_PATH] = {};
            GetWindowText(g_extract_dir, dir_buf, MAX_PATH);
            if (!*dir_buf) {
                fs::path parent = fs::path(zip_buf).parent_path();
                wcsncpy_s(dir_buf, parent.wstring().c_str(), MAX_PATH-1);
                SetWindowText(g_extract_dir, dir_buf);
            }
            g_extracting = true;
            set_extract_enabled(false);
            SendMessage(g_progress, PBM_SETPOS, 0, 0);
            SetWindowText(g_status, L"Extracting...");

            std::string zip_utf8 = fs::path(zip_buf).string();
            std::string dir_utf8 = fs::path(dir_buf).string();
            std::thread([zip_utf8, dir_utf8] {
                extract_thread(zip_utf8, dir_utf8);
            }).detach();
        }
        return 0;
    }

    case WM_PACK_PROGRESS: {
        size_t done = (size_t)wp, total = (size_t)lp;
        SendMessage(g_progress, PBM_SETRANGE32, 0, (LPARAM)total);
        SendMessage(g_progress, PBM_SETPOS, (WPARAM)done, 0);
        WCHAR buf[128];
        swprintf_s(buf, L"Compressing %zu of %zu file(s)...", done, total);
        SetWindowText(g_status, buf);
        return 0;
    }

    case WM_PACK_WRITE: {
        size_t done = (size_t)wp, total = (size_t)lp;
        SendMessage(g_progress, PBM_SETRANGE32, 0, (LPARAM)total);
        SendMessage(g_progress, PBM_SETPOS, (WPARAM)done, 0);
        WCHAR buf[128];
        if (done == 0)
            swprintf_s(buf, L"Writing archive (%zu file(s))...", total);
        else
            swprintf_s(buf, L"Writing %zu of %zu file(s)...", done, total);
        SetWindowText(g_status, buf);
        return 0;
    }

    case WM_PACK_DONE: {
        auto* result = reinterpret_cast<PackResult*>(lp);
        g_packing = false;
        g_last_pack_ok = (result->error_count == 0);
        set_pack_enabled(true);
        SendMessage(g_progress, PBM_SETRANGE32, 0, (LPARAM)result->total_files);
        SendMessage(g_progress, PBM_SETPOS, (WPARAM)result->total_files, 0);
        WCHAR buf[256];
        if (result->error_count < 0) {
            swprintf_s(buf, L"Error: could not create output file.");
        } else {
            double orig = result->total_original_bytes   / 1048576.0;
            double comp = result->total_compressed_bytes / 1048576.0;
            double ratio = result->total_original_bytes > 0
                ? 100.0 * (1.0 - (double)result->total_compressed_bytes / result->total_original_bytes)
                : 0.0;
            swprintf_s(buf, L"Done in %.1f s  │  %.1f → %.1f MB  (%.0f%% saved)%ls",
                result->elapsed_seconds, orig, comp, ratio,
                result->error_count > 0 ? L"  [errors!]" : L"");
        }
        SetWindowText(g_status, buf);
        delete result;
        return 0;
    }

    case WM_EXTRACT_PROGRESS: {
        size_t done = (size_t)wp, total = (size_t)lp;
        SendMessage(g_progress, PBM_SETRANGE32, 0, (LPARAM)total);
        SendMessage(g_progress, PBM_SETPOS, (WPARAM)done, 0);
        WCHAR buf[128];
        swprintf_s(buf, L"Extracting %zu of %zu file(s)...", done, total);
        SetWindowText(g_status, buf);
        return 0;
    }

    case WM_EXTRACT_DONE: {
        auto* result = reinterpret_cast<ExtractResult*>(lp);
        g_extracting = false;
        set_extract_enabled(true);
        SendMessage(g_progress, PBM_SETRANGE32, 0, (LPARAM)result->total_files);
        SendMessage(g_progress, PBM_SETPOS, (WPARAM)result->total_files, 0);
        WCHAR buf[256];
        if (result->error_count < 0)
            swprintf_s(buf, L"Error: could not open the ZIP file.");
        else
            swprintf_s(buf, L"Extracted %zu file(s) in %.1f s%ls",
                result->total_files, result->elapsed_seconds,
                result->error_count > 0 ? L"  [some errors]" : L"");
        SetWindowText(g_status, buf);
        delete result;
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ── entry point ───────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_cmd) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc),
        ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);
    OleInitialize(nullptr);

    WNDCLASSEX wc{ sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = instance;
    wc.lpszClassName = L"zipfast";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon         = LoadIcon(instance, MAKEINTRESOURCE(IDI_MAIN));
    wc.hIconSm       = LoadIcon(instance, MAKEINTRESOURCE(IDI_MAIN));
    RegisterClassEx(&wc);

    RECT client{ 0, 0, 530, 420 };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&client, style, FALSE);

    CreateWindowEx(
        WS_EX_ACCEPTFILES, L"zipfast",
        L"zipfast — ZIP packer & extractor",
        style, CW_USEDEFAULT, CW_USEDEFAULT,
        client.right - client.left, client.bottom - client.top,
        nullptr, nullptr, instance, nullptr);

    ShowWindow(g_hwnd, show_cmd);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    OleUninitialize();
    return (int)msg.wParam;
}
