// Glass pour Windows : fenêtre sans bordure + DirectX 11, Media Foundation, WebView2 (lecteur Spotify), WIC (images).
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <propvarutil.h>
#include <psapi.h>
#include <dbghelp.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <wininet.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <atomic>
#include <cmath>
#include <fstream>
#include <thread>
#include <initguid.h>  // définit les PKEY_* de propkey.h
#include <propkey.h>
#include "platform.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <wrl.h>
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static HWND g_hwnd;
static HANDLE g_job, g_engIn, g_engProc;
static IMFMediaEngine* g_mf;
static ID3D11Device* g_dev;
static std::atomic<bool> g_wakePending{false};
static double g_lastInput = 0;

static double nowS() { return GetTickCount64() / 1000.0; }
static std::wstring widen(const std::string& s) {
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), (int)w.size());
    return w;
}
static std::string narrow(const std::wstring& w) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), (int)s.size(), nullptr, nullptr);
    return s;
}

void plat::wake() { if (g_hwnd && !g_wakePending.exchange(true)) PostMessageW(g_hwnd, WM_APP, 0, 0); }

std::string plat::wallpaper() {
    wchar_t wall[MAX_PATH] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wall, 0);
    return narrow(wall);
}

// ---------- réseau ----------
std::string plat::httpGet(const std::string& url) {
    std::string out;
    HINTERNET net = InternetOpenW(L"Glass", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    HINTERNET h = net ? InternetOpenUrlW(net, widen(url).c_str(), nullptr, 0, INTERNET_FLAG_SECURE, 0) : nullptr;
    char buf[16384]; DWORD n;
    while (h && InternetReadFile(h, buf, sizeof buf, &n) && n) out.append(buf, n);
    if (h) InternetCloseHandle(h);
    if (net) InternetCloseHandle(net);
    return out;
}

bool plat::download(const std::string& url, const fs::path& path, const std::function<void(int)>& progress) {
    HINTERNET net = InternetOpenW(L"Glass", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    HINTERNET h = net ? InternetOpenUrlW(net, widen(url).c_str(), nullptr, 0, INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD, 0) : nullptr;
    wchar_t len[32] = {}; DWORD ls = sizeof len;
    double total = h && HttpQueryInfoW(h, HTTP_QUERY_CONTENT_LENGTH, len, &ls, nullptr) ? _wtof(len) : 0;
    fs::path part = path; part += ".part";
    FILE* f = h ? _wfopen(part.c_str(), L"wb") : nullptr;
    char buf[65536]; DWORD n; double got = 0; int lastPct = -1;
    bool ok = f != nullptr;
    while (ok && InternetReadFile(h, buf, sizeof buf, &n) && n) {
        ok = fwrite(buf, 1, n, f) == n;
        got += n;
        int pct = total > 0 ? (int)(got * 100 / total) : 0;
        if (pct != lastPct) { lastPct = pct; progress(pct); }
    }
    if (f) fclose(f);
    if (h) InternetCloseHandle(h);
    if (net) InternetCloseHandle(net);
    ok = ok && got > 0 && (total == 0 || got >= total);
    std::error_code ec;
    if (ok) fs::rename(part, path, ec); else fs::remove(part, ec);
    return ok && !ec;
}

// ---------- processus ----------
fs::path plat::findTool(const char* name) {
    std::wstring n = widen(name) + L".exe";
    wchar_t buf[MAX_PATH];
    return SearchPathW(nullptr, n.c_str(), nullptr, MAX_PATH, buf, nullptr) ? fs::path(buf) : fs::path();
}

static std::wstring quoteArg(const std::wstring& a) {  // règles de CommandLineToArgvW
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (size_t i = 0;; i++) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { i++; bs++; }
        if (i == a.size()) { q.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { q.append(bs * 2 + 1, L'\\'); q += L'"'; }
        else { q.append(bs, L'\\'); q += a[i]; }
    }
    return q + L"\"";
}

bool plat::run(const std::vector<std::string>& args, int timeoutMs) {
    std::wstring cmd;
    for (auto& a : args) cmd += (cmd.empty() ? L"" : L" ") + quoteArg(widen(a));
    STARTUPINFOW si{sizeof si}; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) return false;
    AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    bool ok = WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0;
    if (!ok) TerminateProcess(pi.hProcess, 1);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return ok && code == 0;
}

bool plat::engineStart(const fs::path& exe, std::function<void(const std::string&)> onLine, std::function<void()> onExit) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE outR, outW, inR, inW;
    CreatePipe(&outR, &outW, &sa, 0); SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    CreatePipe(&inR, &inW, &sa, 0); SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si}; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR; si.hStdOutput = outW; si.hStdError = outW;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, nullptr, g_dir.c_str(), &si, &pi)) {
        for (HANDLE x : {outR, outW, inR, inW}) CloseHandle(x);
        return false;
    }
    AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread); CloseHandle(inR); CloseHandle(outW);
    if (g_engProc) CloseHandle(g_engProc);
    g_engProc = pi.hProcess;
    g_engIn = inW;
    std::thread([outR, onLine, onExit] {
        std::string buf; char c[8192]; DWORD n;
        while (ReadFile(outR, c, sizeof c, &n, nullptr) && n) {
            buf.append(c, n);
            for (size_t p; (p = buf.find('\n')) != std::string::npos; buf.erase(0, p + 1)) onLine(buf.substr(0, p));
            plat::wake();
        }
        CloseHandle(outR);
        onExit();
        plat::wake();
    }).detach();
    return true;
}

void plat::engineSend(const std::string& s) {
    if (!g_engIn) return;
    DWORD w;
    WriteFile(g_engIn, s.data(), (DWORD)s.size(), &w, nullptr);
}

void plat::engineStop() {  // stdin fermé = le moteur s'arrête
    if (g_engIn) { CloseHandle(g_engIn); g_engIn = nullptr; }
    if (g_engProc) WaitForSingleObject(g_engProc, 3000);
}

size_t plat::memMB(bool engine) {
    HANDLE p = engine ? g_engProc : GetCurrentProcess();
    PROCESS_MEMORY_COUNTERS_EX m{sizeof m};
    return p && K32GetProcessMemoryInfo(p, (PROCESS_MEMORY_COUNTERS*)&m, sizeof m) ? m.PrivateUsage >> 20 : 0;
}

// ---------- son : Media Foundation ----------
void plat::audioPlay(const fs::path& path) {
    BSTR src = SysAllocString(path.c_str());
    g_mf->SetSource(src); SysFreeString(src);
    g_mf->Play();
}
void plat::audioPause() { g_mf->Pause(); }
void plat::audioResume() { g_mf->Play(); }
bool plat::audioPaused() { return g_mf->IsPaused(); }
bool plat::audioEnded() { return g_mf->IsEnded(); }
bool plat::audioFailed() {
    IMFMediaError* err = nullptr;
    if (FAILED(g_mf->GetError(&err)) || !err) return false;
    err->Release();
    return true;
}
double plat::audioPos() { return g_mf->GetCurrentTime(); }
double plat::audioDur() { double d = g_mf->GetDuration(); return d > 0 && std::isfinite(d) ? d : 0; }
void plat::audioSeek(double s) { g_mf->SetCurrentTime(s); }
void plat::audioVolume(float v) { g_mf->SetVolume(v); }
void plat::audioRate(float r) { g_mf->SetDefaultPlaybackRate(r); g_mf->SetPlaybackRate(r); }

struct Notify : IMFMediaEngineNotify {  // fin de piste / erreur : on réveille la boucle principale
    STDMETHODIMP QueryInterface(REFIID r, void** p) override {
        if (r == __uuidof(IUnknown) || r == __uuidof(IMFMediaEngineNotify)) { *p = this; return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP EventNotify(DWORD ev, DWORD_PTR, DWORD) override {
        if (ev == MF_MEDIA_ENGINE_EVENT_ENDED || ev == MF_MEDIA_ENGINE_EVENT_ERROR || ev == MF_MEDIA_ENGINE_EVENT_PLAYING || ev == MF_MEDIA_ENGINE_EVENT_PAUSE) plat::wake();
        return S_OK;
    }
} g_notify;

// ---------- lecteur Spotify officiel : Web Playback SDK dans un WebView2 ----------
static ICoreWebView2Controller* g_wvc;
static ICoreWebView2* g_wv;
static HWND g_wvHost;  // fenêtre hors écran qui porte le lecteur
static bool g_wvOn = false;

void plat::webStop() {
    if (g_wvc) { g_wvc->Close(); g_wvc->Release(); }
    if (g_wv) g_wv->Release();
    if (g_wvHost) DestroyWindow(g_wvHost);
    g_wvc = nullptr; g_wv = nullptr; g_wvHost = nullptr; g_wvOn = false;
}

void plat::webPost(const std::string& json) { if (g_wv) g_wv->PostWebMessageAsJson(widen(json).c_str()); }

void plat::webStart(const std::string& html, float vol) {  // thread de la fenêtre uniquement
    using Microsoft::WRL::Callback;
    g_wvOn = true;
    std::wstring web = (g_dir / "web").wstring();
    CreateDirectoryW(web.c_str(), nullptr);
    std::ofstream(web + L"\\player.html") << html;
    auto opts = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    // Masqué, le lecteur Spotify ne joue rien (il reste « loading ») : il doit être affiché.
    // On l'affiche donc dans une petite fenêtre hors de l'écran, absente de la barre des tâches et d'Alt+Tab.
    g_wvHost = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Glass player", WS_POPUP, -4000, -4000, 320, 240, nullptr, nullptr, nullptr, nullptr);
    ShowWindow(g_wvHost, SW_SHOWNOACTIVATE);
    // Moins de processus et de mémoire : pas de GPU, un seul processus de rendu, pas d'isolation par site (une seule page de confiance).
    opts->put_AdditionalBrowserArguments(L"--autoplay-policy=no-user-gesture-required --disable-background-timer-throttling --disable-renderer-backgrounding "
                                         L"--disable-gpu --disable-gpu-compositing --renderer-process-limit=1 --disable-site-isolation-trials --disable-extensions --no-first-run "
                                         L"--disable-features=IsolateOrigins,site-per-process,SpareRendererForSitePerProcess,msSmartScreenProtection");
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, (g_dir / "webview-data").c_str(), opts.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([web, vol](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(hr) || !env) { app::onWeb(R"({"t":"error","msg":"WebView2 indisponible : son via YouTube"})"); return S_OK; }
            env->CreateCoreWebView2Controller(g_wvHost, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [web, vol](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
                    if (FAILED(hr) || !c || !g_wvOn) return S_OK;
                    g_wvc = c; g_wvc->AddRef();
                    RECT bounds{0, 0, 320, 240};
                    g_wvc->put_Bounds(bounds);
                    g_wvc->put_IsVisible(TRUE);
                    g_wvc->get_CoreWebView2(&g_wv);
                    ICoreWebView2_3* wv3;
                    if (SUCCEEDED(g_wv->QueryInterface(IID_PPV_ARGS(&wv3)))) {
                        wv3->SetVirtualHostNameToFolderMapping(L"glass.player", web.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                        wv3->Release();
                    }
                    EventRegistrationToken tok;
                    g_wv->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                            LPWSTR s;
                            if (SUCCEEDED(a->get_WebMessageAsJson(&s))) { app::onWeb(narrow(s)); CoTaskMemFree(s); plat::wake(); }
                            return S_OK;
                        }).Get(), &tok);
                    g_wv->Navigate((L"https://glass.player/player.html?v=" + std::to_wstring(vol)).c_str());  // volume dès le départ
                    return S_OK;
                }).Get());
            return S_OK;
        }).Get());
    if (FAILED(hr)) app::onWeb(R"({"t":"error","msg":"WebView2 indisponible : son via YouTube"})");
}

// ---------- images : WIC ----------
bool plat::decodeImage(const std::string& s, int size, std::vector<uint8_t>& px, int& outW, int& outH) {
    static thread_local IWICImagingFactory* wic = [] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* f = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
        return f;
    }();
    if (!wic) return false;
    IWICBitmapSource* src = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* frame = nullptr;
    IStream* stm = nullptr; IWICBitmap* hb = nullptr;
    std::string bytes;
    if (s.rfind("http", 0) == 0) {
        bytes = plat::httpGet(s);
        stm = SHCreateMemStream((const BYTE*)bytes.data(), (UINT)bytes.size());
        if (stm && SUCCEEDED(wic->CreateDecoderFromStream(stm, nullptr, WICDecodeMetadataCacheOnLoad, &dec))) dec->GetFrame(0, &frame);
        src = frame;
    } else if (s == "icon:") {  // logo de Glass (icône intégrée à l'exe)
        if (HICON hi = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, 0)) {
            wic->CreateBitmapFromHICON(hi, &hb);
            DestroyIcon(hi);
        }
        src = hb;
    } else if (s.rfind("thumb:", 0) == 0) {  // pochette intégrée d'un fichier audio local
        IShellItemImageFactory* f;
        if (SUCCEEDED(SHCreateItemFromParsingName(widen(s.substr(6)).c_str(), nullptr, IID_PPV_ARGS(&f)))) {
            HBITMAP bmp;
            if (SUCCEEDED(f->GetImage({256, 256}, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &bmp))) {
                wic->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapIgnoreAlpha, &hb);
                DeleteObject(bmp);
            }
            f->Release();
        }
        src = hb;
    } else {  // fichier image (fond d'écran, pochette personnalisée)
        if (SUCCEEDED(wic->CreateDecoderFromFilename(widen(s).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec))) dec->GetFrame(0, &frame);
        src = frame;
    }
    bool ok = false;
    UINT w, h;
    if (src && SUCCEEDED(src->GetSize(&w, &h)) && w && h) {
        float k = (float)size / std::max(w, h);
        UINT tw = std::max(1u, (UINT)(w * k)), th = std::max(1u, (UINT)(h * k));
        IWICBitmapScaler* sc; IWICFormatConverter* cv;
        wic->CreateBitmapScaler(&sc); sc->Initialize(src, tw, th, WICBitmapInterpolationModeFant);
        wic->CreateFormatConverter(&cv); cv->Initialize(sc, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
        px.resize(tw * th * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, tw * 4, (UINT)px.size(), px.data()));
        outW = tw; outH = th;
        cv->Release(); sc->Release();
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{frame, dec, stm, hb}) if (u) u->Release();
    return ok;
}

ImTextureID plat::makeTexture(const uint8_t* px, int w, int h) {
    D3D11_TEXTURE2D_DESC d{(UINT)w, (UINT)h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA init{px, (UINT)w * 4, 0};
    ID3D11Texture2D* t; ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&d, &init, &t))) { g_dev->CreateShaderResourceView(t, nullptr, &srv); t->Release(); }
    return (ImTextureID)srv;
}

// Police lue directement depuis le fichier (mappé en mémoire) : pas de copie, Windows ne charge que les pages utilisées.
static ImFont* addFont(const char* file, bool merge = false) {
    std::string p = std::string("C:\\Windows\\Fonts\\") + file;
    HANDLE f = CreateFileA(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    HANDLE m = CreateFileMappingA(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(f);
    void* data = m ? MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : nullptr;
    if (m) CloseHandle(m);
    if (!data) return nullptr;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.MergeMode = merge;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, (int)sz.QuadPart, 16, &cfg);
}

void plat::loadFonts(ImFont** regular, ImFont** bold, ImFont** icons) {
    auto font = [](const char* file) {
        ImFont* f = addFont(file);
        for (const char* fb : {"msyh.ttc", "YuGothM.ttc", "malgun.ttf", "seguisym.ttf", "seguiemj.ttf"}) addFont(fb, true);  // chinois, japonais, coréen, symboles
        return f;
    };
    *regular = font("segoeui.ttf");
    *bold = font("segoeuib.ttf");
    *icons = addFont("segmdl2.ttf");
}

// ---------- fichiers locaux ----------
void plat::readTags(const fs::path& path, std::string& title, std::string& artist, std::string& album, int& ms) {
    static thread_local bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    (void)com;
    IPropertyStore* ps;
    if (FAILED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps)))) return;
    auto str = [&](REFPROPERTYKEY k) {
        PROPVARIANT v; PropVariantInit(&v); std::string r; PWSTR s;
        if (SUCCEEDED(ps->GetValue(k, &v)) && v.vt != VT_EMPTY && SUCCEEDED(PropVariantToStringAlloc(v, &s))) { r = narrow(s); CoTaskMemFree(s); }
        PropVariantClear(&v);
        return r;
    };
    if (auto s = str(PKEY_Title); !s.empty()) title = s;
    artist = str(PKEY_Music_Artist); album = str(PKEY_Music_AlbumTitle);
    PROPVARIANT v; PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(PKEY_Media_Duration, &v)) && v.vt == VT_UI8) ms = (int)(v.uhVal.QuadPart / 10000);
    PropVariantClear(&v);
    ps->Release();
}

std::vector<fs::path> plat::pick(int kind) {
    std::vector<fs::path> paths;
    IFileOpenDialog* d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&d)))) return paths;
    COMDLG_FILTERSPEC audio{L"Audio", L"*.mp3;*.m4a;*.aac;*.flac;*.wav;*.wma;*.ogg;*.opus"}, image{L"Images", L"*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.webp"};
    DWORD o; d->GetOptions(&o);
    d->SetOptions(o | (kind == 1 ? FOS_ALLOWMULTISELECT : 0) | (kind == 2 ? FOS_PICKFOLDERS : 0));
    if (kind != 2) d->SetFileTypes(1, kind == 1 ? &audio : &image);
    IShellItemArray* arr;
    if (SUCCEEDED(d->Show(g_hwnd)) && SUCCEEDED(d->GetResults(&arr))) {
        DWORD n = 0; arr->GetCount(&n);
        for (DWORD i = 0; i < n; i++) {
            IShellItem* it; PWSTR s;
            if (SUCCEEDED(arr->GetItemAt(i, &it))) {
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &s))) { paths.push_back(s); CoTaskMemFree(s); }
                it->Release();
            }
        }
        arr->Release();
    }
    d->Release();
    return paths;
}

// ---------- fenêtre ----------
static RECT g_normalRect;
static bool g_wasZoomed = false;

bool plat::maximized() { return IsZoomed(g_hwnd); }
void plat::minimize() { ShowWindow(g_hwnd, SW_MINIMIZE); }
void plat::toggleMaximize() { ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE); }
void plat::close() { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); }
void plat::setTopmost(bool on) { SetWindowPos(g_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }

void plat::enterMini(int w, int h) {
    WINDOWPLACEMENT wp{sizeof wp};
    GetWindowPlacement(g_hwnd, &wp);
    g_normalRect = wp.rcNormalPosition;  // taille « normale », même si la fenêtre était agrandie
    g_wasZoomed = IsZoomed(g_hwnd);
    if (g_wasZoomed) ShowWindow(g_hwnd, SW_RESTORE);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(g_hwnd, nullptr, mi.rcWork.right - w - (int)(20 * S), mi.rcWork.bottom - h - (int)(20 * S), w, h, SWP_NOZORDER);
}

void plat::leaveMini() {
    RECT r = g_normalRect;
    SetWindowPos(g_hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
    if (g_wasZoomed) ShowWindow(g_hwnd, SW_MAXIMIZE);
}

void plat::setMiniHeight(int h) {
    RECT r;
    GetWindowRect(g_hwnd, &r);
    SetWindowPos(g_hwnd, nullptr, r.left, r.bottom - h, r.right - r.left, h, SWP_NOZORDER);
}

bool plat::wallpaperUV(ImVec2& uv0, ImVec2& uv1) {
    RECT wr; GetWindowRect(g_hwnd, &wr);
    MONITORINFO mi{sizeof mi}; GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    float mw = float(mi.rcMonitor.right - mi.rcMonitor.left), mh = float(mi.rcMonitor.bottom - mi.rcMonitor.top);
    uv0 = {(wr.left - mi.rcMonitor.left) / mw, (wr.top - mi.rcMonitor.top) / mh};
    uv1 = {(wr.right - mi.rcMonitor.left) / mw, (wr.bottom - mi.rcMonitor.top) / mh};
    return true;
}

// ---------- système ----------
void plat::openUrl(const std::string& url) { ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void plat::setMediaKeys(bool on) {
    for (int i = 1; i <= 3; i++) UnregisterHotKey(g_hwnd, i);
    if (!on) return;
    RegisterHotKey(g_hwnd, 1, 0, VK_MEDIA_PLAY_PAUSE);
    RegisterHotKey(g_hwnd, 2, 0, VK_MEDIA_NEXT_TRACK);
    RegisterHotKey(g_hwnd, 3, 0, VK_MEDIA_PREV_TRACK);
}

void plat::setStartup(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k)) return;
    std::wstring v = L"\"" + (g_bin / "Glass.exe").wstring() + L"\"";
    if (on) RegSetValueExW(k, L"Glass", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)(v.size() + 1) * 2);
    else RegDeleteValueW(k, L"Glass");
    RegCloseKey(k);
}

// ---------- icône près de l'horloge (option « fermer = réduire ») ----------
static NOTIFYICONDATAW g_nid{sizeof g_nid};
static bool g_trayShown = false, g_quitting = false;

static void showTray(bool on) {
    if (on == g_trayShown) return;
    g_nid.hWnd = g_hwnd; g_nid.uID = 1; g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP + 1;
    g_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Glass");
    Shell_NotifyIconW(on ? NIM_ADD : NIM_DELETE, &g_nid);
    g_trayShown = on;
}

void plat::nowPlaying(const std::string& title, const std::string& artist, bool) {  // infobulle de l'icône = titre en cours
    if (!g_trayShown || title.empty()) return;
    std::wstring tip = widen(title + " - " + artist).substr(0, 120);
    if (tip != g_nid.szTip) { wcscpy_s(g_nid.szTip, tip.c_str()); Shell_NotifyIconW(NIM_MODIFY, &g_nid); }
}

static void showWindowAgain() {
    showTray(false);
    ShowWindow(g_hwnd, SW_SHOW);
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
}

// ---------- fenêtre sans bordure + DirectX 11 ----------
static ID3D11DeviceContext* g_ctx;
static IDXGISwapChain* g_swap;
static ID3D11RenderTargetView* g_rtv;
static UINT g_resizeW, g_resizeH;

static void createRTV() {
    ID3D11Texture2D* back;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

static LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_APP: g_wakePending = false; return 0;
    case WM_APP + 1:  // clic sur l'icône près de l'horloge
        if (LOWORD(lp) == WM_LBUTTONUP) showWindowAgain();
        else if (LOWORD(lp) == WM_RBUTTONUP) {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"Lecture / pause");
            AppendMenuW(m, MF_STRING, 2, L"Suivant");
            AppendMenuW(m, MF_STRING, 3, L"Précédent");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 4, L"Afficher Glass");
            AppendMenuW(m, MF_STRING, 5, L"Quitter");
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(m);
            if (c >= 1 && c <= 3) app::media(c);
            else if (c == 4) showWindowAgain();
            else if (c == 5) { g_quitting = true; PostMessageW(hwnd, WM_CLOSE, 0, 0); }
        }
        return 0;
    case WM_CLOSE:
        if (app::trayOn() && !g_quitting) { ShowWindow(hwnd, SW_HIDE); showTray(true); return 0; }
        break;
    case WM_NCCALCSIZE:  // pas de cadre Windows : on dessine notre propre barre de titre
        if (wp) {
            if (IsZoomed(hwnd)) {
                auto& r = ((NCCALCSIZE_PARAMS*)lp)->rgrc[0];
                int f = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                r.left += f; r.top += f; r.right -= f; r.bottom -= f;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(hwnd, &pt);
        RECT rc; GetClientRect(hwnd, &rc);
        int b = (int)(6 * S);
        if (!app::mini() && !IsZoomed(hwnd)) {
            bool l = pt.x < b, r = pt.x >= rc.right - b, t = pt.y < b, bo = pt.y >= rc.bottom - b;
            if (t && l) return HTTOPLEFT; if (t && r) return HTTOPRIGHT;
            if (bo && l) return HTBOTTOMLEFT; if (bo && r) return HTBOTTOMRIGHT;
            if (l) return HTLEFT; if (r) return HTRIGHT; if (t) return HTTOP; if (bo) return HTBOTTOM;
        }
        return app::caption((float)pt.x, (float)pt.y, (float)rc.right, (float)rc.bottom) ? HTCAPTION : HTCLIENT;
    }
    case WM_GETMINMAXINFO: ((MINMAXINFO*)lp)->ptMinTrackSize = app::mini() ? POINT{(LONG)(380 * S), (LONG)(140 * S)} : POINT{(LONG)(960 * S), (LONG)(620 * S)}; return 0;
    case WM_SIZE: if (wp != SIZE_MINIMIZED) { g_resizeW = LOWORD(lp); g_resizeH = HIWORD(lp); } return 0;
    case WM_DROPFILES: {
        HDROP d = (HDROP)wp;
        std::vector<fs::path> paths;
        UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) { wchar_t p[MAX_PATH * 4]; if (DragQueryFileW(d, i, p, MAX_PATH * 4)) paths.push_back(p); }
        DragFinish(d);
        app::dropped(paths);
        return 0;
    }
    case WM_HOTKEY: app::media((int)wp); return 0;  // touches multimédia du clavier
    case WM_SYSCOMMAND: if ((wp & 0xfff0) == SC_KEYMENU) return 0; break;
    case WM_DESTROY: showTray(false); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// yt-dlp a pu être installé après l'ouverture de la session : on relit le PATH du registre.
static void refreshPath() {
    auto get = [](HKEY root, const wchar_t* sub) {
        std::vector<wchar_t> buf(32767); DWORD sz = (DWORD)buf.size() * sizeof(wchar_t);
        return RegGetValueW(root, sub, L"Path", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf.data(), &sz) == ERROR_SUCCESS ? std::wstring(buf.data()) : L"";
    };
    std::wstring p = get(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment") + L";" + get(HKEY_CURRENT_USER, L"Environment");
    SetEnvironmentVariableW(L"PATH", p.c_str());
}

// Rapport de plantage : écrit la pile d'appels (fonctions + lignes) dans crash.txt.
static void writeCrash(const char* what, CONTEXT* ctx) {
    FILE* f = _wfopen((g_dir / "crash.txt").c_str(), L"w");
    if (!f) return;
    fprintf(f, "Glass a planté : %s\n\n", what);
    HANDLE proc = GetCurrentProcess(), th = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);
    CONTEXT c;
    if (ctx) c = *ctx; else RtlCaptureContext(&c);
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = c.Rip; sf.AddrFrame.Offset = c.Rbp; sf.AddrStack.Offset = c.Rsp;
    sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 40 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &c, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) && sf.AddrPC.Offset; i++) {
        char buf[sizeof(SYMBOL_INFO) + 256]{};
        auto* sym = (SYMBOL_INFO*)buf; sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 255;
        IMAGEHLP_LINE64 line{sizeof line}; DWORD disp = 0; DWORD64 d64 = 0;
        bool hasSym = SymFromAddr(proc, sf.AddrPC.Offset, &d64, sym);
        bool hasLine = SymGetLineFromAddr64(proc, sf.AddrPC.Offset, &disp, &line);
        fprintf(f, "%2d  %s", i, hasSym ? sym->Name : "?");
        if (hasLine) fprintf(f, "  (%s:%lu)", strrchr(line.FileName, '\\') ? strrchr(line.FileName, '\\') + 1 : line.FileName, line.LineNumber);
        fprintf(f, "\n");
    }
    fclose(f);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // STA : requis par la boîte de dialogue de fichiers et WebView2
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* e) -> LONG {
        char what[64]; snprintf(what, sizeof what, "exception 0x%08lX", e->ExceptionRecord->ExceptionCode);
        writeCrash(what, e->ContextRecord);
        return EXCEPTION_EXECUTE_HANDLER;
    });
    std::set_terminate([] { writeCrash("std::terminate (exception C++ non attrapée)", nullptr); abort(); });
    ImGui_ImplWin32_EnableDpiAwareness();

    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_dir = g_bin = fs::path(exe).parent_path();
    refreshPath();

    g_job = CreateJobObjectW(nullptr, nullptr);  // le moteur et yt-dlp meurent avec Glass
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl{}; jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &jl, sizeof jl);

    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    IMFMediaEngineClassFactory* factory; IMFAttributes* attr;
    CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&factory));
    MFCreateAttributes(&attr, 1);
    attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, &g_notify);
    if (FAILED(factory->CreateInstance(MF_MEDIA_ENGINE_AUDIOONLY, attr, &g_mf))) { MessageBoxW(nullptr, L"Media Foundation indisponible.", L"Glass", MB_ICONERROR); return 1; }
    attr->Release(); factory->Release();

    app::init();

    WNDCLASSEXW wc{sizeof wc, CS_CLASSDC, wndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"Glass",
                   (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0)};
    RegisterClassExW(&wc);
    S = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
    g_hwnd = CreateWindowW(L"Glass", L"Glass", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, (int)(1320 * S), (int)(840 * S), nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE; DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    int corner = DWMWCP_ROUND; DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
    MARGINS margins{0, 0, 1, 0}; DwmExtendFrameIntoClientArea(g_hwnd, &margins);  // garde l'ombre Windows
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    DragAcceptFiles(g_hwnd, TRUE);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, nullptr, &g_ctx)) &&
        FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, nullptr, &g_ctx))) {
        MessageBoxW(nullptr, L"DirectX 11 indisponible.", L"Glass", MB_ICONERROR); return 1;
    }
    createRTV();
    ShowWindow(g_hwnd, SW_SHOWDEFAULT); UpdateWindow(g_hwnd);

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    app::initUI();

    // Boucle à la demande : on ne redessine que s'il se passe quelque chose (0 % CPU au repos).
    for (bool done = false; !done;) {
        bool visible = IsWindowVisible(g_hwnd) && !IsIconic(g_hwnd);
        int t = app::timeoutMs(visible, GetForegroundWindow() == g_hwnd);
        if (visible && nowS() - g_lastInput < 1.0) t = t < 0 ? 15 : std::min(t, 15);  // survol : fluide, plafonné à ~60 images/s
        if (visible && g_resizeW) t = 15;
        // On ne redessine que pour : nos propres messages (souris, clavier, peinture), un réveil (WM_APP) ou le délai écoulé.
        // Le lecteur web poste beaucoup de messages internes sur ce thread : ils ne doivent pas déclencher d'image.
        bool redraw = MsgWaitForMultipleObjectsEx(0, nullptr, t < 0 ? INFINITE : (DWORD)t, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_TIMEOUT;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
            else if (msg.message == WM_APP) redraw = true;
            else if (msg.hwnd == g_hwnd) {
                redraw = true;
                if ((msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST) || (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) || msg.message == WM_MOUSELEAVE || msg.message == WM_NCMOUSEMOVE)
                    g_lastInput = nowS();
            }
        }
        if (done) break;
        if (app::tick()) continue;  // une boîte de dialogue s'est ouverte
        if (!IsWindowVisible(g_hwnd) || IsIconic(g_hwnd) || !(redraw || g_resizeW)) continue;
        if (g_resizeW) {
            g_rtv->Release();
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0; createRTV();
        }
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        app::frame();
        ImGui::Render();
        const float clear[4] = {0, 0, 0, 1};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    showTray(false);
    if (g_engIn) CloseHandle(g_engIn);  // le moteur s'arrête seul
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    g_mf->Shutdown();
    CloseHandle(g_job);
    return 0;
}
