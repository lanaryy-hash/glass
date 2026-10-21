// Glass pour macOS : fenêtre GLFW + OpenGL, son AVFoundation, lecteur Spotify dans un WKWebView, images ImageIO.
#define GLFW_INCLUDE_NONE
#import <Cocoa/Cocoa.h>
#import <AVFoundation/AVFoundation.h>
#import <WebKit/WebKit.h>
#import <MediaPlayer/MediaPlayer.h>
#import <ImageIO/ImageIO.h>
#import <ServiceManagement/ServiceManagement.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <OpenGL/gl3.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach/mach.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <set>
#include <thread>
#include "platform.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

extern char** environ;

static GLFWwindow* g_win;
static NSWindow* g_ns;
static double g_lastInput = 0;

static NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
static void onMain(dispatch_block_t b) { if ([NSThread isMainThread]) b(); else dispatch_async(dispatch_get_main_queue(), b); }

void plat::wake() { glfwPostEmptyEvent(); }

std::string plat::wallpaper() {
    NSURL* u = [[NSWorkspace sharedWorkspace] desktopImageURLForScreen:[NSScreen mainScreen]];
    return u.isFileURL ? std::string(u.path.UTF8String) : "";
}

// ---------- réseau : libcurl (fourni avec macOS) ----------
static size_t curlAppend(char* p, size_t s, size_t n, void* out) { ((std::string*)out)->append(p, s * n); return s * n; }

static CURL* curlFor(const std::string& url) {
    CURL* c = curl_easy_init();
    if (!c) return nullptr;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Glass");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    return c;
}

std::string plat::httpGet(const std::string& url) {
    std::string out;
    CURL* c = curlFor(url);
    if (!c) return out;
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlAppend);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    if (curl_easy_perform(c) != CURLE_OK) out.clear();
    curl_easy_cleanup(c);
    return out;
}

struct Progress { const std::function<void(int)>* cb; int last = -1; };
static int curlProgress(void* p, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    auto* pr = (Progress*)p;
    int pct = total > 0 ? (int)(now * 100 / total) : 0;
    if (pct != pr->last) { pr->last = pct; (*pr->cb)(pct); }
    return 0;
}

bool plat::download(const std::string& url, const fs::path& to, const std::function<void(int)>& progress) {
    fs::path part = to; part += ".part";
    FILE* f = fopen(part.c_str(), "wb");
    CURL* c = f ? curlFor(url) : nullptr;
    if (!c) { if (f) fclose(f); return false; }
    Progress pr{&progress};
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, curlProgress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &pr);
    bool ok = curl_easy_perform(c) == CURLE_OK;
    curl_easy_cleanup(c);
    fclose(f);
    std::error_code ec;
    if (ok) fs::rename(part, to, ec); else fs::remove(part, ec);
    return ok && !ec;
}

// ---------- processus ----------
static std::mutex g_pidMx;
static std::set<pid_t> g_pids;  // arrêtés quand Glass se ferme

fs::path plat::findTool(const char* name) {
    for (fs::path d : {g_bin, g_dir / "bin", fs::path("/opt/homebrew/bin"), fs::path("/usr/local/bin")}) {
        fs::path p = d / name;
        if (access(p.c_str(), X_OK) == 0) return p;
    }
    return {};
}

static pid_t spawn(const std::vector<std::string>& args, int in, int out) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    int devnull = open("/dev/null", O_RDWR);
    posix_spawn_file_actions_adddup2(&fa, in >= 0 ? in : devnull, 0);
    posix_spawn_file_actions_adddup2(&fa, out >= 0 ? out : devnull, 1);
    posix_spawn_file_actions_adddup2(&fa, out >= 0 ? out : devnull, 2);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT);  // l'enfant ne garde que 0, 1, 2 (sinon les tubes du moteur ne se ferment jamais)
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back((char*)a.c_str());
    argv.push_back(nullptr);
    pid_t pid = -1;
    if (posix_spawnp(&pid, argv[0], &fa, &at, argv.data(), environ) != 0) pid = -1;
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    ::close(devnull);
    if (pid > 0) { setpriority(PRIO_PROCESS, pid, 10); std::lock_guard lk(g_pidMx); g_pids.insert(pid); }
    return pid;
}

static void reaped(pid_t pid) { std::lock_guard lk(g_pidMx); g_pids.erase(pid); }

bool plat::run(const std::vector<std::string>& args, int timeoutMs) {
    pid_t pid = spawn(args, -1, -1);
    if (pid <= 0) return false;
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) { reaped(pid); return WIFEXITED(status) && WEXITSTATUS(status) == 0; }
        if (r < 0) { reaped(pid); return false; }
        if (std::chrono::steady_clock::now() > end) { kill(pid, SIGKILL); waitpid(pid, &status, 0); reaped(pid); return false; }
        usleep(50000);
    }
}

static int g_engIn = -1;
static pid_t g_engPid = 0;

bool plat::engineStart(const fs::path& exe, std::function<void(const std::string&)> onLine, std::function<void()> onExit) {
    int in[2], out[2];
    if (pipe(in)) return false;
    if (pipe(out)) { ::close(in[0]); ::close(in[1]); return false; }
    setenv("GLASS_DATA", g_dir.c_str(), 1);  // l'app est en lecture seule : le moteur range ses données ici
    pid_t pid = spawn({exe.string()}, in[0], out[1]);
    ::close(in[0]); ::close(out[1]);
    if (pid <= 0) { ::close(in[1]); ::close(out[0]); return false; }
    g_engIn = in[1]; g_engPid = pid;
    std::thread([fd = out[0], pid, onLine, onExit] {
        std::string buf; char c[8192]; ssize_t n;
        while ((n = read(fd, c, sizeof c)) > 0) {
            buf.append(c, n);
            for (size_t p; (p = buf.find('\n')) != std::string::npos; buf.erase(0, p + 1)) onLine(buf.substr(0, p));
            plat::wake();
        }
        ::close(fd);
        waitpid(pid, nullptr, 0);
        reaped(pid);
        onExit();
        plat::wake();
    }).detach();
    return true;
}

void plat::engineSend(const std::string& s) {
    for (size_t off = 0; g_engIn >= 0 && off < s.size();) {
        ssize_t w = write(g_engIn, s.data() + off, s.size() - off);
        if (w <= 0) break;
        off += w;
    }
}

void plat::engineStop() {  // stdin fermé = le moteur s'arrête
    if (g_engIn >= 0) { ::close(g_engIn); g_engIn = -1; }
    for (int i = 0; i < 60 && g_engPid > 0 && kill(g_engPid, 0) == 0; i++) usleep(50000);
}

size_t plat::memMB(bool engine) {
    if (engine) {
        rusage_info_v2 ri;
        return g_engPid > 0 && proc_pid_rusage(g_engPid, RUSAGE_INFO_V2, (rusage_info_t*)&ri) == 0 ? (size_t)(ri.ri_phys_footprint >> 20) : 0;
    }
    task_vm_info_data_t vm; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &n) == KERN_SUCCESS ? (size_t)(vm.phys_footprint >> 20) : 0;
}

// ---------- son : AVPlayer (appelé parfois depuis d'autres threads : on passe par le thread principal) ----------
static AVPlayer* g_player;
static id g_endObs, g_failObs;
static std::atomic<bool> g_ended{false}, g_failed{false};
static float g_rate = 1, g_vol = 1;

void plat::audioPlay(const fs::path& file) {
    std::string p = file.string();
    onMain(^{
        if (!g_player) g_player = [AVPlayer new];
        AVPlayerItem* it = [AVPlayerItem playerItemWithURL:[NSURL fileURLWithPath:ns(p)]];
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
        if (g_endObs) [nc removeObserver:g_endObs];
        if (g_failObs) [nc removeObserver:g_failObs];
        g_endObs = [nc addObserverForName:AVPlayerItemDidPlayToEndTimeNotification object:it queue:nil usingBlock:^(NSNotification*) { g_ended = true; plat::wake(); }];
        g_failObs = [nc addObserverForName:AVPlayerItemFailedToPlayToEndTimeNotification object:it queue:nil usingBlock:^(NSNotification*) { g_failed = true; plat::wake(); }];
        g_ended = g_failed = false;
        [g_player replaceCurrentItemWithPlayerItem:it];
        g_player.volume = g_vol;
        g_player.rate = g_rate;  // lance la lecture
    });
}
void plat::audioPause() { g_ended = false; onMain(^{ [g_player pause]; }); }
void plat::audioResume() { onMain(^{ g_player.rate = g_rate; }); }
bool plat::audioPaused() { return !g_player || g_player.rate == 0; }
bool plat::audioEnded() { return g_ended; }
bool plat::audioFailed() { return g_failed || (g_player.currentItem && g_player.currentItem.status == AVPlayerItemStatusFailed); }
double plat::audioPos() { double s = g_player ? CMTimeGetSeconds(g_player.currentTime) : 0; return std::isfinite(s) ? s : 0; }
double plat::audioDur() { double s = g_player.currentItem ? CMTimeGetSeconds(g_player.currentItem.duration) : 0; return std::isfinite(s) && s > 0 ? s : 0; }
void plat::audioSeek(double s) { g_ended = false; onMain(^{ [g_player seekToTime:CMTimeMakeWithSeconds(s, 1000) toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero]; }); }
void plat::audioVolume(float v) { g_vol = v; onMain(^{ g_player.volume = v; }); }
void plat::audioRate(float r) { g_rate = r; onMain(^{ if (g_player.rate != 0) g_player.rate = r; }); }

// ---------- lecteur Spotify officiel : Web Playback SDK dans un WKWebView ----------
// shortcut: dépend du DRM du WKWebView ; s'il refuse, le lecteur signale une erreur et Glass joue via YouTube (mode Auto).
@interface GlassWeb : NSObject <WKScriptMessageHandler>
@end
@implementation GlassWeb
- (void)userContentController:(WKUserContentController*)c didReceiveScriptMessage:(WKScriptMessage*)m {
    if ([m.body isKindOfClass:[NSString class]]) { app::onWeb([(NSString*)m.body UTF8String]); plat::wake(); }
}
@end

static WKWebView* g_web;
static NSWindow* g_webWin;
static GlassWeb* g_webHandler;

void plat::webStart(const std::string& html, float vol) {
    WKWebViewConfiguration* cfg = [WKWebViewConfiguration new];
    g_webHandler = [GlassWeb new];
    [cfg.userContentController addScriptMessageHandler:g_webHandler name:@"glass"];
    cfg.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
    // Comme sur Windows, le lecteur ne joue que s'il est « affiché » : minuscule fenêtre quasi transparente, jamais au premier plan.
    NSRect r = NSMakeRect(0, 0, 2, 2);
    g_webWin = [[NSWindow alloc] initWithContentRect:r styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    g_webWin.releasedWhenClosed = NO;
    g_webWin.ignoresMouseEvents = YES;
    g_webWin.alphaValue = 0.01;
    g_webWin.excludedFromWindowsMenu = YES;
    g_webWin.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle;
    g_web = [[WKWebView alloc] initWithFrame:r configuration:cfg];
    g_webWin.contentView = g_web;
    [g_webWin orderBack:nil];
    NSString* base = [NSString stringWithFormat:@"https://glass.player/player.html?v=%f", vol];  // volume dès le départ (lu par la page)
    [g_web loadHTMLString:ns(html) baseURL:[NSURL URLWithString:base]];
}

void plat::webStop() {
    [g_web.configuration.userContentController removeScriptMessageHandlerForName:@"glass"];
    [g_webWin close];
    g_web = nil; g_webWin = nil; g_webHandler = nil;
}

void plat::webPost(const std::string& json) {
    [g_web evaluateJavaScript:ns("window.glassIn && glassIn(" + json + ")") completionHandler:nil];
}

// ---------- images : ImageIO ----------
static NSData* artworkOf(const std::string& audio) {  // pochette intégrée à un fichier audio
    AVURLAsset* a = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:ns(audio)] options:nil];
    for (AVMetadataItem* m in [AVMetadataItem metadataItemsFromArray:a.commonMetadata withKey:AVMetadataCommonKeyArtwork keySpace:AVMetadataKeySpaceCommon])
        if (m.dataValue) return m.dataValue;
    return nil;
}

bool plat::decodeImage(const std::string& s, int size, std::vector<uint8_t>& px, int& w, int& h) {
    @autoreleasepool {
        CGImageSourceRef src = nullptr;
        if (s.rfind("http", 0) == 0) {
            std::string bytes = plat::httpGet(s);
            if (!bytes.empty()) src = CGImageSourceCreateWithData((__bridge CFDataRef)[NSData dataWithBytes:bytes.data() length:bytes.size()], nullptr);
        } else if (s == "icon:") {
            NSString* p = [[NSBundle mainBundle] pathForResource:@"glass-256" ofType:@"png"];
            if (p) src = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:p], nullptr);
        } else if (s.rfind("thumb:", 0) == 0) {
            if (NSData* d = artworkOf(s.substr(6))) src = CGImageSourceCreateWithData((__bridge CFDataRef)d, nullptr);
        } else {
            src = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:ns(s)], nullptr);
        }
        if (!src) return false;
        NSDictionary* opts = @{(id)kCGImageSourceCreateThumbnailFromImageAlways: @YES, (id)kCGImageSourceCreateThumbnailWithTransform: @YES,
                               (id)kCGImageSourceThumbnailMaxPixelSize: @(size)};
        CGImageRef img = CGImageSourceCreateThumbnailAtIndex(src, 0, (__bridge CFDictionaryRef)opts);
        CFRelease(src);
        if (!img) return false;
        w = (int)CGImageGetWidth(img); h = (int)CGImageGetHeight(img);
        px.assign((size_t)w * h * 4, 0);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(cs);
        if (ctx) { CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img); CGContextRelease(ctx); }
        CGImageRelease(img);
        if (!ctx) return false;
        for (size_t i = 0; i < px.size(); i += 4)  // couleurs non prémultipliées, comme sur Windows
            if (uint8_t a = px[i + 3]; a && a < 255) for (int k = 0; k < 3; k++) px[i + k] = (uint8_t)std::min(255, px[i + k] * 255 / a);
        return true;
    }
}

ImTextureID plat::makeTexture(const uint8_t* px, int w, int h) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return (ImTextureID)(intptr_t)t;
}

// Polices lues directement depuis le fichier (mappé en mémoire) : rien n'est copié.
static ImFont* addFont(const std::string& path, bool merge = false) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return nullptr;
    struct stat st;
    void* data = fstat(fd, &st) == 0 ? mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    ::close(fd);
    if (data == MAP_FAILED) return nullptr;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.MergeMode = merge;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, (int)st.st_size, 16, &cfg);
}

void plat::loadFonts(ImFont** regular, ImFont** bold, ImFont** icons) {
    std::string res = [NSBundle mainBundle].resourcePath.UTF8String;
    auto font = [&](const char* file) {
        ImFont* f = addFont(res + "/" + file);
        if (!f) f = ImGui::GetIO().Fonts->AddFontDefault();
        for (const char* fb : {"/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "/System/Library/Fonts/Hiragino Sans GB.ttc", "/System/Library/Fonts/AppleSDGothicNeo.ttc"})
            addFont(fb, true);  // cyrillique déjà dans Inter ; chinois, japonais, coréen, symboles
        return f;
    };
    *regular = font("Inter-Regular.ttf");
    *bold = font("Inter-Bold.ttf");
    *icons = addFont(res + "/MaterialIcons-Regular.ttf");
    if (!*icons) *icons = *regular;
}

// ---------- fichiers locaux ----------
void plat::readTags(const fs::path& file, std::string& title, std::string& artist, std::string& album, int& ms) {
    @autoreleasepool {
        AVURLAsset* a = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:ns(file.string())] options:nil];
        auto get = [&](AVMetadataKey k) {
            for (AVMetadataItem* m in [AVMetadataItem metadataItemsFromArray:a.commonMetadata withKey:k keySpace:AVMetadataKeySpaceCommon])
                if (m.stringValue.length) return std::string(m.stringValue.UTF8String);
            return std::string();
        };
        if (auto t = get(AVMetadataCommonKeyTitle); !t.empty()) title = t;
        artist = get(AVMetadataCommonKeyArtist);
        album = get(AVMetadataCommonKeyAlbumName);
        double d = CMTimeGetSeconds(a.duration);
        if (std::isfinite(d) && d > 0) ms = (int)(d * 1000);
    }
}

std::vector<fs::path> plat::pick(int kind) {
    NSOpenPanel* p = [NSOpenPanel openPanel];
    p.canChooseFiles = kind != 2;
    p.canChooseDirectories = kind == 2;
    p.allowsMultipleSelection = kind == 1;
    if (kind == 1) p.allowedContentTypes = @[UTTypeAudio];
    if (kind == 3) p.allowedContentTypes = @[UTTypeImage];
    std::vector<fs::path> out;
    if ([p runModal] == NSModalResponseOK)
        for (NSURL* u in p.URLs) out.push_back(u.path.UTF8String);
    return out;
}

// ---------- fenêtre ----------
static int g_nx, g_ny, g_nw, g_nh;
static bool g_wasMax = false;

static void trafficLights(bool show) {  // boutons ronds du Mac : cachés dans le mini-lecteur
    for (NSWindowButton b : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) [g_ns standardWindowButton:b].hidden = !show;
}

bool plat::maximized() { return glfwGetWindowAttrib(g_win, GLFW_MAXIMIZED); }
void plat::minimize() { glfwIconifyWindow(g_win); }
void plat::toggleMaximize() { if (maximized()) glfwRestoreWindow(g_win); else glfwMaximizeWindow(g_win); }
void plat::close() { glfwSetWindowShouldClose(g_win, GLFW_TRUE); }
void plat::setTopmost(bool on) { glfwSetWindowAttrib(g_win, GLFW_FLOATING, on); }

void plat::enterMini(int w, int h) {
    g_wasMax = maximized();
    if (g_wasMax) glfwRestoreWindow(g_win);
    glfwGetWindowPos(g_win, &g_nx, &g_ny);
    glfwGetWindowSize(g_win, &g_nw, &g_nh);
    int mx, my, mw, mh;
    glfwGetMonitorWorkarea(glfwGetPrimaryMonitor(), &mx, &my, &mw, &mh);
    glfwSetWindowSizeLimits(g_win, 380, 140, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwSetWindowSize(g_win, w, h);
    glfwSetWindowPos(g_win, mx + mw - w - 20, my + mh - h - 20);
    trafficLights(false);
}

void plat::leaveMini() {
    glfwSetWindowSizeLimits(g_win, 960, 620, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwSetWindowSize(g_win, g_nw, g_nh);
    glfwSetWindowPos(g_win, g_nx, g_ny);
    if (g_wasMax) glfwMaximizeWindow(g_win);
    trafficLights(true);
}

void plat::setMiniHeight(int h) {
    int x, y, w, oh;
    glfwGetWindowPos(g_win, &x, &y);
    glfwGetWindowSize(g_win, &w, &oh);
    glfwSetWindowSize(g_win, w, h);
    glfwSetWindowPos(g_win, x, y + oh - h);
}

bool plat::wallpaperUV(ImVec2& uv0, ImVec2& uv1) {  // shortcut: écran principal seulement
    NSSize sc = [NSScreen screens].firstObject.frame.size;
    int x, y, w, h;
    glfwGetWindowPos(g_win, &x, &y);
    glfwGetWindowSize(g_win, &w, &h);
    uv0 = {x / (float)sc.width, y / (float)sc.height};
    uv1 = {(x + w) / (float)sc.width, (y + h) / (float)sc.height};
    return true;
}

// ---------- système ----------
void plat::openUrl(const std::string& s) {
    NSString* str = ns(s);
    [[NSWorkspace sharedWorkspace] openURL:[str hasPrefix:@"http"] ? [NSURL URLWithString:str] : [NSURL fileURLWithPath:str]];
}

void plat::setMediaKeys(bool on) {  // touches du clavier, AirPods, Centre de contrôle
    MPRemoteCommandCenter* c = [MPRemoteCommandCenter sharedCommandCenter];
    MPRemoteCommand* cmds[] = {c.togglePlayPauseCommand, c.playCommand, c.pauseCommand, c.nextTrackCommand, c.previousTrackCommand};
    int codes[] = {1, 1, 1, 2, 3};  // shortcut: lecture et pause basculent toutes les deux
    for (int i = 0; i < 5; i++) {
        [cmds[i] removeTarget:nil];
        if (!on) continue;
        int code = codes[i];
        [cmds[i] addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) { app::media(code); plat::wake(); return MPRemoteCommandHandlerStatusSuccess; }];
    }
}

void plat::setStartup(bool on) {
    if (@available(macOS 13.0, *)) {
        NSError* e = nil;
        if (on) [[SMAppService mainAppService] registerAndReturnError:&e];
        else [[SMAppService mainAppService] unregisterAndReturnError:&e];
    }
}

void plat::nowPlaying(const std::string& title, const std::string& artist, bool playing) {
    static std::string last;
    static bool lastPlaying = false;
    if (title + "\n" + artist == last && playing == lastPlaying) return;
    last = title + "\n" + artist; lastPlaying = playing;
    MPNowPlayingInfoCenter* c = [MPNowPlayingInfoCenter defaultCenter];
    c.nowPlayingInfo = @{MPMediaItemPropertyTitle: ns(title), MPMediaItemPropertyArtist: ns(artist)};
    c.playbackState = playing ? MPNowPlayingPlaybackStatePlaying : MPNowPlayingPlaybackStatePaused;
}

// yt-dlp : téléchargé au premier lancement (aucune installation à faire), mis à jour chaque semaine (YouTube change souvent).
static void ensureYtdlp() {
    fs::path p = g_dir / "bin" / "yt-dlp";
    std::error_code ec;
    if (!plat::findTool("yt-dlp").empty()) {
        auto age = fs::file_time_type::clock::now() - fs::last_write_time(p, ec);
        if (!ec && age > std::chrono::hours(24 * 7)) std::thread([p] { plat::run({p.string(), "-U"}, 120000); fs::path q = p; std::error_code e; fs::last_write_time(q, fs::file_time_type::clock::now(), e); }).detach();
        return;
    }
    std::thread([p] {
        if (plat::download("https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_macos", p, [](int) {})) chmod(p.c_str(), 0755);
    }).detach();
}

static void markInput() { g_lastInput = glfwGetTime(); }

int main(int, char**) {
    @autoreleasepool {
        signal(SIGPIPE, SIG_IGN);  // écrire au moteur arrêté ne doit pas tuer Glass
        curl_global_init(CURL_GLOBAL_DEFAULT);
        g_bin = fs::path([NSBundle mainBundle].executablePath.UTF8String).parent_path();
        g_dir = fs::path(NSHomeDirectory().UTF8String) / "Library" / "Application Support" / "Glass";
        std::error_code ec;
        fs::create_directories(g_dir / "bin", ec);
        if (!glfwInit()) return 1;
        ensureYtdlp();
        app::init();

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
        g_win = glfwCreateWindow(1320, 840, "Glass", nullptr, nullptr);
        if (!g_win) return 1;
        glfwSetWindowSizeLimits(g_win, 960, 620, GLFW_DONT_CARE, GLFW_DONT_CARE);
        glfwMakeContextCurrent(g_win);
        glfwSwapInterval(1);
        // Barre de titre transparente : l'interface de Glass va jusqu'en haut, les boutons ronds du Mac restent à gauche.
        g_ns = glfwGetCocoaWindow(g_win);
        g_ns.styleMask |= NSWindowStyleMaskFullSizeContentView;
        g_ns.titlebarAppearsTransparent = YES;
        g_ns.titleVisibility = NSWindowTitleHidden;
        g_ns.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        g_ns.backgroundColor = NSColor.blackColor;

        // Notre fonction d'abord, puis celle d'ImGui (qui enchaîne sur la nôtre) : on sait quand l'utilisateur bouge.
        glfwSetCursorPosCallback(g_win, [](GLFWwindow*, double, double) { markInput(); });
        glfwSetMouseButtonCallback(g_win, [](GLFWwindow*, int, int, int) { markInput(); });
        glfwSetScrollCallback(g_win, [](GLFWwindow*, double, double) { markInput(); });
        glfwSetKeyCallback(g_win, [](GLFWwindow*, int, int, int, int) { markInput(); });
        glfwSetDropCallback(g_win, [](GLFWwindow*, int n, const char** paths) { app::dropped(std::vector<fs::path>(paths, paths + n)); });

        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui_ImplGlfw_InitForOpenGL(g_win, true);
        ImGui_ImplOpenGL3_Init("#version 150");
        app::initUI();

        // Déplacer la fenêtre : un clic sur la barre de titre (ou la pochette du mini-lecteur) la fait glisser comme une fenêtre Mac.
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown handler:^NSEvent*(NSEvent* e) {
            if (e.window != g_ns) return e;
            NSSize sz = g_ns.contentView.bounds.size;
            NSPoint p = e.locationInWindow;
            if (!app::caption(p.x, sz.height - p.y, sz.width, sz.height)) return e;
            if (e.clickCount == 2 && !app::mini()) [g_ns zoom:nil];
            else [g_ns performWindowDragWithEvent:e];
            return nil;
        }];

        // Boucle à la demande : on ne redessine que s'il se passe quelque chose (0 % CPU au repos).
        while (!glfwWindowShouldClose(g_win)) {
            bool visible = !glfwGetWindowAttrib(g_win, GLFW_ICONIFIED) && (g_ns.occlusionState & NSWindowOcclusionStateVisible);
            int t = app::timeoutMs(visible, glfwGetWindowAttrib(g_win, GLFW_FOCUSED));
            if (visible && glfwGetTime() - g_lastInput < 1.0) t = t < 0 ? 15 : std::min(t, 15);
            if (t < 0) glfwWaitEvents(); else glfwWaitEventsTimeout(t / 1000.0);
            if (app::tick()) continue;
            if (glfwGetWindowAttrib(g_win, GLFW_ICONIFIED) || !(g_ns.occlusionState & NSWindowOcclusionStateVisible)) continue;
            ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
            app::frame();
            ImGui::Render();
            int fw, fh;
            glfwGetFramebufferSize(g_win, &fw, &fh);
            glViewport(0, 0, fw, fh);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(g_win);
        }

        if (g_engIn >= 0) ::close(g_engIn);  // le moteur s'arrête seul
        { std::lock_guard lk(g_pidMx); for (pid_t p : g_pids) kill(p, SIGTERM); }
        ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext();
        glfwTerminate();
    }
    return 0;
}
