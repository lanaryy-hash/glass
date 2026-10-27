// Glass — lecteur de musique léger : ta bibliothèque Spotify (via glass-engine), Apple Music (liens) + tes fichiers.
// Son : lecteur web officiel Spotify (page invisible), YouTube (yt-dlp) ou fichiers locaux.
// UI : Dear ImGui, effet verre dépoli, rendu uniquement quand quelque chose change.
// Ce fichier est commun à Windows et macOS ; le reste est dans win.cpp / mac.mm (voir platform.h).
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <nlohmann/json.hpp>
#define IMGUI_DEFINE_MATH_OPERATORS
#include "platform.h"
#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#endif

using json = nlohmann::json;

#ifndef GLASS_VERSION
#define GLASS_VERSION 0  // compilé sur ce PC : pas de mise à jour automatique (les versions de GitHub ont un numéro)
#endif
static const char* kRepo = "lanaryy-hash/glass";  // versions publiées par GitHub (.github/workflows/build.yml)

// ---------- données ----------
struct Track { std::string uri, title, artist, album, image; int ms = 0; bool local = false; };
// custom : playlist Spotify ajoutée par lien ; own : playlist créée dans Glass (uri "glass:...")
struct List { std::string uri, name, image; std::vector<Track> tracks; bool requested = false, loaded = false, custom = false, own = false, hidden = false; };
struct LyricLine { int t; std::string w; };

struct Settings {
    int bg = 0, prefetch = 2, lyricsSize = 30, source = 0, density = 1;  // source : 0 auto, 1 Spotify uniquement, 2 YouTube uniquement
    // bg : 0 pochette floutée, 1 fond d'écran flouté, 2 uni, 3 dégradé de la pochette (Apple Music), 4 néon, 5 aurore animée
    float neon = 0;                     // intensité des lueurs néon (0 = aucune)
    int particles = 0;                  // 0 aucune, 1 pétales de sakura, 2 neige, 3 lucioles, 4 citrouilles
    int halloween = 0;                  // année où le thème Halloween a été mis tout seul
    float accent[3] = {0.12f, 0.84f, 0.38f}, tint[3] = {0.05f, 0.07f, 0.12f};
    float glass = 0.07f, dim = 0.45f, tintAmt = 0.15f, rounding = 20, textScale = 1, speed = 1, vol = 0.8f;
    bool mediaKeys = true, startup = false, animations = true, showAlbum = true, tray = false, topmost = false, lyricsCenter = false, stopAtEnd = false;
    bool accentAuto = false, autoLyrics = true;  // couleur prise sur la pochette ; paroles générées si introuvables
    bool alignLyrics = true;                     // l'IA écoute le titre une fois pour caler les paroles trouvées sur le son
    int seenVersion = 0;                         // dernière version dont les nouveautés ont été montrées
    bool apple = false;                          // compte Apple Music connecté (page music.apple.com lancée au démarrage)
    std::string logo;                            // image choisie comme logo de Glass ("" = logo d'origine)
    std::vector<std::string> pinned = {"glass:favoris", "spotify:collection", "local", "glass:recent"};  // en haut de la bibliothèque
};
static Settings g_set;

struct Theme { const char* name; float acc[3], tint[3]; int bg; float glass, dim, tintAmt, rounding, neon = 0; bool accentAuto = false; int particles = -1; };  // particles : -1 = on garde
static const Theme kThemes[] = {
    {"Glass", {0.12f, 0.84f, 0.38f}, {0.05f, 0.07f, 0.12f}, 0, 0.07f, 0.45f, 0.15f, 20},
    {"Minuit", {0.30f, 0.55f, 1.00f}, {0.03f, 0.05f, 0.22f}, 1, 0.06f, 0.55f, 0.45f, 20},
    {"Aurore", {0.68f, 0.48f, 1.00f}, {0.22f, 0.06f, 0.32f}, 0, 0.10f, 0.40f, 0.35f, 22},
    {"Sakura", {1.00f, 0.48f, 0.72f}, {0.32f, 0.08f, 0.20f}, 0, 0.12f, 0.40f, 0.30f, 24},
    {"Coucher de soleil", {1.00f, 0.60f, 0.22f}, {0.32f, 0.10f, 0.04f}, 0, 0.08f, 0.45f, 0.35f, 20},
    {"Givre", {0.40f, 0.90f, 1.00f}, {0.05f, 0.16f, 0.22f}, 1, 0.18f, 0.30f, 0.25f, 26},
    {"Forêt", {0.55f, 0.85f, 0.35f}, {0.04f, 0.14f, 0.06f}, 0, 0.08f, 0.50f, 0.35f, 18},
    {"OLED", {0.12f, 0.84f, 0.38f}, {0.00f, 0.00f, 0.00f}, 2, 0.04f, 0.00f, 0.00f, 12},
    {"Rétro", {1.00f, 0.82f, 0.25f}, {0.14f, 0.09f, 0.04f}, 2, 0.06f, 0.20f, 0.00f, 4},
    {"Apple Music", {0.98f, 0.18f, 0.28f}, {0.10f, 0.06f, 0.08f}, 3, 0.10f, 0.35f, 0.00f, 14, 0, true},
    {"Apple Clear", {0.04f, 0.52f, 1.00f}, {0.08f, 0.10f, 0.14f}, 1, 0.16f, 0.22f, 0.05f, 22},
    {"Néon Glass", {1.00f, 0.17f, 0.84f}, {0.10f, 0.04f, 0.16f}, 3, 0.09f, 0.40f, 0.10f, 22, 0.8f, true},
    {"Néon Tokyo", {1.00f, 0.17f, 0.84f}, {0.06f, 0.02f, 0.12f}, 4, 0.07f, 0.15f, 0.20f, 18, 1.0f},
    {"Cyber", {0.00f, 0.95f, 1.00f}, {0.02f, 0.05f, 0.10f}, 4, 0.06f, 0.15f, 0.20f, 10, 1.0f},
    {"Aurore boréale", {0.30f, 1.00f, 0.70f}, {0.02f, 0.08f, 0.10f}, 5, 0.08f, 0.20f, 0.15f, 22, 0.5f},
    {"Halloween", {1.00f, 0.50f, 0.05f}, {0.12f, 0.03f, 0.16f}, 2, 0.08f, 0.30f, 0.30f, 20, 0.6f, false, 4},
};

std::mutex g_mx;
float S = 1;
fs::path g_dir, g_bin;
static std::vector<List> g_lists;
static List g_search{"search", "Recherche"};
static std::map<std::string, std::pair<std::string, std::string>> g_over;  // personnalisation : uri -> (nom, image)
static std::string g_status, g_user, g_view, g_wall;
static bool g_engineOk = false, g_spReady = false;
static bool g_anim = false;             // une animation demande l'image suivante
static bool g_animBg = false;

// lecture : Glass gère la file d'attente pour toutes les sources
static std::string g_plUri;             // liste d'où vient la lecture
static int g_tr = -1, g_pos = 0;        // titre de la liste (-1 = vient de la file d'attente) / position dans g_order
static std::vector<int> g_order;
static std::deque<Track> g_queue;       // « lire ensuite » / « ajouter à la file »
static Track g_cur;
static bool g_has = false, g_shuffle = false, g_repeat = false, g_loading = false, g_failed = false;
static fs::path g_ready;                // fichier YouTube prêt à jouer
static std::atomic<int> g_gen{0};       // invalide les téléchargements périmés
static std::set<std::string> g_inflight;
static double g_sleepAt = 0;            // minuterie
static bool g_sleepEnd = false;         // minuterie « à la fin du titre »
// lecteurs officiels dans des pages web invisibles : Spotify (Web Playback SDK) et Apple Music (music.apple.com)
static bool g_webOk = false, g_sp = false, g_wvStarted = false, g_sdkReady = false, g_spPaused = true, g_spEnded = false;
static bool g_spApple = false;                                                      // le lecteur officiel en cours est celui d'Apple
static bool g_amStarted = false, g_amReady = false, g_amAuth = false, g_amLogin = false;  // Apple Music : page lancée, prête, connectée, fenêtre de connexion
static std::string g_amDiag;  // ce que le lecteur d'Apple peut faire ici (titres entiers ou extraits)
static std::string g_sdkDevice;
static double g_spPos = 0, g_spT = 0, g_spDur = 0;
static std::vector<std::pair<int, std::string>> g_wvOut;  // messages JSON pour les pages (postés depuis le thread de la fenêtre)

// interface
static int g_pick = 0;                  // boîtes de dialogue (hors du verrou) : 1 fichiers, 2 dossier, 3 image de pochette
static std::string g_pickUri;           // liste dont on choisit la pochette
static bool g_lyricsOpen = false, g_mini = false, g_openPerso = false, g_openNew = false, g_focusSearch = false;
static std::string g_editUri;           // liste en cours de personnalisation
static std::vector<Track> g_pendingAdd; // titres à mettre dans la nouvelle playlist
static Track g_dragTrack;               // glisser-déposer de titres
static std::string g_dragFrom;
static int g_dragIdx = -1;
static char g_searchBuf[128];
static std::string g_searchSent;
static double g_searchT = 0;

// paroles
static std::string g_lyrKey, g_lyrSrc;
static std::vector<LyricLine> g_lyr;
static bool g_lyrSynced = false;
static int g_lyrState = 0;              // 0 rien, 1 chargement, 2 trouvées, 3 introuvables
static bool g_lyrAlign = false;         // les paroles affichées viennent d'Internet, pas encore calées sur le son

static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void wake() { plat::wake(); }
static fs::path upath(const std::string& s) { return fs::u8path(s); }  // chemins en UTF-8 dans tout Glass
static std::string ustr(const fs::path& p) { return p.u8string(); }
static bool fileExists(const fs::path& p) { std::error_code ec; return fs::exists(p, ec); }
static long long nowId() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// Les champs JSON peuvent être null (titres indisponibles...) : valeurs par défaut sans exception.
static std::string jstr(const json& j, const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : ""; }
static double jnum(const json& j, const char* k, double d = 0) { return j.contains(k) && j[k].is_number() ? j[k].get<double>() : d; }
static json trackJson(const Track& t) { return {{"uri", t.uri}, {"title", t.title}, {"artist", t.artist}, {"album", t.album}, {"image", t.image}, {"ms", t.ms}, {"local", t.local}}; }
static Track trackFrom(const json& j) {
    Track t{jstr(j, "uri"), jstr(j, "title"), jstr(j, "artist"), jstr(j, "album"), jstr(j, "image"), (int)jnum(j, "ms")};
    t.local = j.contains("local") && j["local"].is_boolean() && j["local"].get<bool>();
    return t;
}

static List* findList(const std::string& uri) {
    if (uri == "search") return &g_search;
    for (auto& l : g_lists) if (l.uri == uri) return &l;
    return nullptr;
}
static Track* current() { return g_has ? &g_cur : nullptr; }
static bool isCurrent(const List& l, int i) { return g_has && g_plUri == l.uri && g_tr == i; }

// Lien Spotify -> "spotify:playlist:<id>" ; lien Apple Music -> l'adresse de sa page (sans paramètres).
static std::string toUri(const std::string& s) {
    std::smatch m;
    if (std::regex_search(s, m, std::regex(R"(music\.apple\.com/[a-z]{2}/(playlist|album)/[^?#\s"]+)"))) return "https://" + m[0].str();
    if (std::regex_search(s, m, std::regex(R"((playlist|album)[/:]([A-Za-z0-9]{22}))"))) return "spotify:" + m[1].str() + ":" + m[2].str();
    if (std::regex_search(s, m, std::regex(R"((on\.)?soundcloud\.com/[^?#\s"]*[^?#\s"/])"))) return "https://" + m[0].str();  // m.soundcloud.com -> soundcloud.com
    return "";
}
// SoundCloud : lien public (titre, playlist, profil, likes) ; titres "soundcloud:track:<id>"
static bool isSoundcloud(const std::string& uri) { return uri.rfind("https://soundcloud.com/", 0) == 0 || uri.rfind("https://on.soundcloud.com/", 0) == 0; }
// Apple Music : lien public (https://music.apple.com/...) ou playlist de ton compte ("am:<id>")
static bool isApple(const std::string& uri) { return uri.rfind("https://music.apple.com/", 0) == 0 || uri.rfind("am:", 0) == 0; }
static bool isAlbum(const std::string& uri) { return uri.rfind("spotify:album", 0) == 0 || (isApple(uri) && uri.find("/album/") != std::string::npos); }

// ---------- persistance ----------
static void saveSettings() {
    auto& s = g_set;
    json j = {{"bg", s.bg}, {"prefetch", s.prefetch}, {"lyricsSize", s.lyricsSize}, {"source2", s.source}, {"density", s.density},
              {"accentRGB", {s.accent[0], s.accent[1], s.accent[2]}}, {"tint", {s.tint[0], s.tint[1], s.tint[2]}},
              {"glass", s.glass}, {"dim", s.dim}, {"tintAmt", s.tintAmt}, {"rounding", s.rounding}, {"textScale", s.textScale}, {"speed", s.speed}, {"vol", s.vol},
              {"mediaKeys", s.mediaKeys}, {"startup", s.startup}, {"animations", s.animations}, {"showAlbum", s.showAlbum}, {"tray", s.tray},
              {"topmost", s.topmost}, {"lyricsCenter", s.lyricsCenter}, {"stopAtEnd", s.stopAtEnd},
              {"neon", s.neon}, {"accentAuto", s.accentAuto}, {"autoLyrics", s.autoLyrics}, {"particles", s.particles}, {"pinned", s.pinned}, {"apple", s.apple}, {"logo", s.logo}, {"alignLyrics", s.alignLyrics}, {"seenVersion", s.seenVersion}, {"halloween", s.halloween}};
    std::ofstream((g_dir / "settings.json")) << j.dump(1);
}
static void loadSettings() {
    std::ifstream f((g_dir / "settings.json"));
    json j = json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    auto& s = g_set;
    auto get = [&](const char* k, auto& v) { if (j.contains(k) && !j[k].is_null()) try { v = j[k].get<std::decay_t<decltype(v)>>(); } catch (...) {} };
    get("bg", s.bg); get("prefetch", s.prefetch); get("lyricsSize", s.lyricsSize); get("source2", s.source); get("density", s.density);
    get("glass", s.glass); get("dim", s.dim); get("tintAmt", s.tintAmt); get("rounding", s.rounding); get("textScale", s.textScale); get("speed", s.speed); get("vol", s.vol);
    get("mediaKeys", s.mediaKeys); get("startup", s.startup); get("animations", s.animations); get("showAlbum", s.showAlbum); get("tray", s.tray);
    get("topmost", s.topmost); get("lyricsCenter", s.lyricsCenter); get("stopAtEnd", s.stopAtEnd);
    get("neon", s.neon); get("accentAuto", s.accentAuto); get("autoLyrics", s.autoLyrics); get("particles", s.particles); get("pinned", s.pinned); get("apple", s.apple); get("logo", s.logo); get("alignLyrics", s.alignLyrics); get("seenVersion", s.seenVersion); get("halloween", s.halloween);
    for (int i = 0; i < 3; i++) {
        if (j.contains("accentRGB") && j["accentRGB"].size() == 3) s.accent[i] = j["accentRGB"][i].get<float>();
        if (j.contains("tint") && j["tint"].size() == 3) s.tint[i] = j["tint"][i].get<float>();
    }
}
static void saveLinks() {
    std::ofstream f((g_dir / "playlists.txt"));
    for (auto& l : g_lists) if (l.custom) f << l.uri << "\n";
}
static void saveLocal() {
    std::ofstream f((g_dir / "local.txt"));
    if (List* l = findList("local"))
        for (auto& t : l->tracks) f << t.uri << '\t' << t.title << '\t' << t.artist << '\t' << t.album << '\t' << t.ms << '\n';
}
static void saveMine() {  // playlists créées dans Glass (dont Favoris et Récents)
    json a = json::array();
    for (auto& l : g_lists) if (l.own) {
        json t = json::array();
        for (auto& x : l.tracks) t.push_back(trackJson(x));
        a.push_back({{"uri", l.uri}, {"name", l.name}, {"image", l.image}, {"tracks", t}});
    }
    std::ofstream((g_dir / "myplaylists.json")) << a.dump();
}
static void saveOverrides() {
    json j = json::object();
    for (auto& [k, v] : g_over) j[k] = {{"name", v.first}, {"image", v.second}};
    std::ofstream((g_dir / "overrides.json")) << j.dump(1);
}
static void applyOverride(List& l) {
    auto it = g_over.find(l.uri);
    if (it == g_over.end()) return;
    if (!it->second.first.empty()) l.name = it->second.first;
    if (!it->second.second.empty()) l.image = it->second.second;
}
static void loadSaved() {
    List local{"local", "Mes musiques"}; local.loaded = true;
    std::ifstream f((g_dir / "local.txt"));
    for (std::string line; std::getline(f, line);) {
        std::stringstream ss(line); Track t; std::string ms;
        std::getline(ss, t.uri, '\t'); std::getline(ss, t.title, '\t'); std::getline(ss, t.artist, '\t'); std::getline(ss, t.album, '\t'); std::getline(ss, ms, '\t');
        if (t.uri.empty()) continue;
        t.ms = atoi(ms.c_str()); t.local = true; t.image = "thumb:" + t.uri;
        local.tracks.push_back(t);
    }
    g_lists.push_back(local);

    std::ifstream o((g_dir / "overrides.json"));
    json ov = json::parse(o, nullptr, false);
    if (ov.is_object()) for (auto& [k, v] : ov.items()) g_over[k] = {jstr(v, "name"), jstr(v, "image")};

    std::ifstream m((g_dir / "myplaylists.json"));
    json mine = json::parse(m, nullptr, false);
    if (mine.is_array()) for (auto& p : mine) {
        List l{jstr(p, "uri"), jstr(p, "name"), jstr(p, "image")};
        l.own = l.loaded = true;
        for (auto& t : p["tracks"]) l.tracks.push_back(trackFrom(t));
        g_lists.push_back(l);
    }
    if (!findList("glass:favoris")) { List l{"glass:favoris", "Favoris"}; l.own = l.loaded = true; g_lists.push_back(l); }
    if (!findList("glass:recent")) { List l{"glass:recent", "Écoutés récemment"}; l.own = l.loaded = true; g_lists.push_back(l); }

    std::ifstream p((g_dir / "playlists.txt"));
    for (std::string line; std::getline(p, line);)
        if (auto u = toUri(line); !u.empty() && !findList(u)) { List l{u, "Chargement..."}; l.custom = true; g_lists.push_back(l); }
    for (auto& l : g_lists) applyOverride(l);
}

static bool isFav(const std::string& uri) {
    List* f = findList("glass:favoris");
    return f && std::any_of(f->tracks.begin(), f->tracks.end(), [&](const Track& t) { return t.uri == uri; });
}
static void toggleFav(const Track& t) {
    List* f = findList("glass:favoris");
    auto it = std::find_if(f->tracks.begin(), f->tracks.end(), [&](const Track& x) { return x.uri == t.uri; });
    if (it != f->tracks.end()) f->tracks.erase(it); else f->tracks.insert(f->tracks.begin(), t);
    saveMine();
}
static void addRecent(const Track& t) {
    List* r = findList("glass:recent");
    if (!r || g_plUri == r->uri) return;  // on ne réordonne pas la liste qu'on est en train d'écouter
    r->tracks.erase(std::remove_if(r->tracks.begin(), r->tracks.end(), [&](const Track& x) { return x.uri == t.uri; }), r->tracks.end());
    r->tracks.insert(r->tracks.begin(), t);
    if (r->tracks.size() > 100) r->tracks.resize(100);
    saveMine();
}
static void addToList(List& l, const Track& t) {
    if (std::none_of(l.tracks.begin(), l.tracks.end(), [&](const Track& x) { return x.uri == t.uri; })) l.tracks.push_back(t);
    if (l.image.empty()) l.image = t.image;
    g_status = "Ajouté à « " + l.name + " »";
    saveMine();
}

// ---------- fichiers locaux ----------
static bool isAudio(const fs::path& p) {
    std::string e = ustr(p.extension());
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)tolower(c); });
    for (auto x : {".mp3", ".m4a", ".aac", ".flac", ".wav", ".wma", ".ogg", ".opus"}) if (e == x) return true;
    return false;
}

static Track readLocal(const fs::path& path) {
    Track t; t.local = true; t.uri = ustr(path); t.image = "thumb:" + t.uri;
    t.title = ustr(path.stem());
    plat::readTags(path, t.title, t.artist, t.album, t.ms);
    for (auto* s : {&t.title, &t.artist, &t.album}) std::replace_if(s->begin(), s->end(), [](char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
    return t;
}

static void addLocal(std::vector<fs::path> paths) {  // fichiers et/ou dossiers (récursif)
    std::thread([paths] {
        std::vector<Track> found;
        for (auto& p : paths) {
            try {
                if (fs::is_directory(p)) {
                    for (auto& e : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied))
                        if (e.is_regular_file() && isAudio(e.path())) found.push_back(readLocal(e.path()));
                } else if (isAudio(p)) found.push_back(readLocal(p));
            } catch (...) {}
        }
        {
            std::lock_guard lk(g_mx);
            List* l = findList("local");
            int added = 0;
            for (auto& t : found)
                if (std::none_of(l->tracks.begin(), l->tracks.end(), [&](const Track& x) { return x.uri == t.uri; })) { l->tracks.push_back(t); added++; }
            g_status = added ? std::to_string(added) + " musique(s) ajoutée(s)" : "Aucun fichier audio trouvé";
            saveLocal();
        }
        wake();
    }).detach();
}

// ---------- Apple Music : la page publique d'une playlist ou d'un album contient ses titres (pas de compte) ----------
// shortcut: seulement les titres présents dans la page, à compléter si de longues playlists arrivent tronquées.
static const json& jget(const json& j, const char* k) { static const json none; return j.is_object() && j.contains(k) ? j[k] : none; }

static std::string appleArt(const json& item) {  // modèle ".../{w}x{h}bb.{f}" -> 300x300 jpg
    std::string u = jstr(jget(jget(item, "artwork"), "dictionary"), "url");
    static const std::pair<const char*, const char*> subs[] = {{"{w}", "300"}, {"{h}", "300"}, {"{c}", ""}, {"{f}", "jpg"}};
    for (auto [a, b] : subs)
        if (size_t p = u.find(a); p != std::string::npos) u.replace(p, strlen(a), b);
    return u;
}

// Remplit une liste ajoutée par lien (Apple Music, SoundCloud) avec ce qu'on a lu.
static void fillList(const std::string& url, const std::string& name, const std::string& image, std::vector<Track>& tracks, const char* error) {
    {
        std::lock_guard lk(g_mx);
        if (List* l = findList(url)) {
            l->loaded = true;
            if (tracks.empty()) { l->name = "Introuvable"; g_status = error; }
            else {
                for (auto& t : tracks) { if (t.image.empty()) t.image = image; if (t.album.empty()) t.album = name; }  // albums : pochette et nom de l'album
                if (!name.empty()) l->name = name;
                if (l->image.empty()) l->image = image;
                l->tracks = std::move(tracks);
            }
            applyOverride(*l);
        }
    }
    wake();
}

static void loadApple(const std::string& url) {
    std::thread([url] {
        std::string html = plat::httpGet(url), name, image;
        std::vector<Track> tracks;
        size_t a = html.find("id=\"serialized-server-data\"");
        if (a != std::string::npos) a = html.find('>', a);
        size_t b = a == std::string::npos ? a : html.find("</script>", a);
        json d = b == std::string::npos ? json() : json::parse(html.substr(a + 1, b - a - 1), nullptr, false);
        json page = d.is_object() && d["data"].is_array() && !d["data"].empty() ? d["data"][0]["data"] : json();
        for (auto& sec : jget(page, "sections")) {
            std::string kind = jstr(sec, "itemKind");
            if (kind == "containerDetailHeaderLockup") for (auto& it : jget(sec, "items")) { name = jstr(it, "title"); image = appleArt(it); }
            if (kind != "trackLockup") continue;
            for (auto& it : jget(sec, "items")) {
                Track t;
                std::string id = jstr(jget(jget(it, "contentDescriptor"), "identifiers"), "storeAdamID");
                t.uri = "apple:song:" + id; t.title = jstr(it, "title"); t.artist = jstr(it, "artistName");
                t.ms = (int)jnum(it, "duration"); t.image = appleArt(it);
                const json& links = jget(it, "tertiaryLinks");
                if (links.is_array() && !links.empty()) t.album = jstr(links[0], "title");
                if (!id.empty() && !t.title.empty()) tracks.push_back(t);
            }
        }
        fillList(url, name, image, tracks, "Apple Music : impossible de lire cette page");
    }).detach();
}

// ---------- SoundCloud : liens publics lus via l'API du site web (comme yt-dlp) ----------
static std::string scClientId() {  // clé publique du site SoundCloud, écrite dans un de ses scripts
    static std::mutex mx;
    static std::string id;
    std::lock_guard lk(mx);
    if (!id.empty()) return id;
    std::string html = plat::httpGet("https://soundcloud.com/");
    std::vector<std::string> scripts;
    const std::string tag = "src=\"https://a-v2.sndcdn.com/assets/";
    for (size_t p = html.find(tag); p != std::string::npos; p = html.find(tag, p + 1)) scripts.push_back(html.substr(p + 5, html.find('"', p + 5) - p - 5));
    for (auto s = scripts.rbegin(); s != scripts.rend() && id.empty(); ++s) {  // elle est dans les derniers
        std::string js = plat::httpGet(*s);
        if (size_t p = js.find("client_id:\""); p != std::string::npos && p + 43 <= js.size()) id = js.substr(p + 11, 32);
    }
    return id;
}

static std::string scArt(const json& j) {  // pochette (sinon photo du compte) en 300 px
    std::string u = jstr(j, "artwork_url");
    if (u.empty()) u = jstr(jget(j, "user"), "avatar_url");
    if (u.empty()) u = jstr(j, "avatar_url");
    if (size_t p = u.find("-large."); p != std::string::npos) u.replace(p, 6, "-t300x300");
    return u;
}

static Track scTrack(const json& t, const std::string& album) {
    std::string title = jstr(t, "title"), artist = jstr(jget(t, "publisher_metadata"), "artist");
    if (size_t d = title.find(" - "); artist.empty() && d != std::string::npos) { artist = title.substr(0, d); title = title.substr(d + 3); }  // « Artiste - Titre »
    if (artist.empty()) artist = jstr(jget(t, "user"), "username");
    return Track{"soundcloud:track:" + std::to_string((long long)jnum(t, "id")), title, artist, album, scArt(t), (int)jnum(t, "duration")};
}

static std::string urlEnc(const std::string& s);
static void loadSoundcloud(const std::string& url) {
    std::thread([url] {
        std::string cid = scClientId(), page = url, name, image;
        std::vector<Track> tracks;
        auto api = [&](std::string path) {
            if (path.rfind("https://", 0) != 0) path = "https://api-v2.soundcloud.com/" + path;
            return json::parse(plat::httpGet(path + (path.find('?') == std::string::npos ? "?" : "&") + "client_id=" + cid), nullptr, false);
        };
        if (page.rfind("https://on.soundcloud.com/", 0) == 0) {  // lien court de l'appli : la page d'arrivée donne le vrai lien
            std::string html = plat::httpGet(page);
            if (size_t p = html.find("rel=\"canonical\" href=\""); p != std::string::npos) { p += 22; page = html.substr(p, html.find('"', p) - p); }
        }
        bool likes = page.size() > 6 && page.compare(page.size() - 6, 6, "/likes") == 0;
        json r = cid.empty() ? json() : api("resolve?url=" + urlEnc(likes ? page.substr(0, page.size() - 6) : page));
        std::string kind = jstr(r, "kind"), id = std::to_string((long long)jnum(r, "id"));
        image = scArt(r);
        if (kind == "track") { name = jstr(r, "title"); tracks.push_back(scTrack(r, "")); }
        else if (kind == "playlist") {  // seuls les premiers titres sont complets : le reste par paquets de 50
            name = jstr(r, "title");
            std::vector<long long> ids, missing;
            std::map<long long, json> full;
            for (auto& t : jget(r, "tracks")) {
                long long tid = (long long)jnum(t, "id");
                ids.push_back(tid);
                if (t.contains("title")) full[tid] = t; else missing.push_back(tid);
            }
            for (size_t i = 0; i < missing.size(); i += 50) {
                std::string q;
                for (size_t k = i; k < std::min(missing.size(), i + 50); k++) q += (q.empty() ? "" : ",") + std::to_string(missing[k]);
                for (auto& t : api("tracks?ids=" + q)) if (t.is_object()) full[(long long)jnum(t, "id")] = t;
            }
            for (long long tid : ids) if (full.count(tid)) tracks.push_back(scTrack(full[tid], name));
        } else if (kind == "user") {  // profil : ses titres, ou ses likes ; pages de 200, 1000 titres max
            name = jstr(r, "username") + (likes ? " - Likes" : "");
            for (std::string path = "users/" + id + (likes ? "/likes" : "/tracks") + "?limit=200"; !path.empty() && tracks.size() < 1000;) {
                json c = api(path);
                for (auto& it : jget(c, "collection")) if (const json& t = likes ? jget(it, "track") : it; t.is_object()) tracks.push_back(scTrack(t, name));
                path = jstr(c, "next_href");
            }
        }
        if (image.empty() && !tracks.empty()) image = tracks[0].image;
        fillList(url, name, image, tracks, "SoundCloud : impossible de lire ce lien (privé ?)");
    }).detach();
}

// ---------- catalogue : artistes, leurs titres et albums ----------
// Spotify (Web API, si le lecteur officiel est connecté), sinon le catalogue public d'Apple / iTunes (sans compte ;
// mêmes titres qu'Apple Music, donc lus par le lecteur Apple si connecté, sinon via YouTube).
struct Card { std::string uri, name, image; };          // artiste ou album à afficher
static std::vector<Card> g_artists;                       // artistes trouvés par la recherche
static std::map<std::string, std::vector<Card>> g_albums; // albums d'un artiste (clé : uri de l'artiste)

static std::string itArt(const json& j) {  // pochette iTunes en 300 px
    std::string u = jstr(j, "artworkUrl100");
    if (size_t p = u.find("100x100"); p != std::string::npos) u.replace(p, 7, "300x300");
    return u;
}
static Track itTrack(const json& r) {
    Track t{"apple:song:" + std::to_string((long long)jnum(r, "trackId")), jstr(r, "trackName"), jstr(r, "artistName"), jstr(r, "collectionName"), itArt(r), (int)jnum(r, "trackTimeMillis")};
    return t;
}
static json itunes(const std::string& q) {  // catalogue français (comme les comptes Apple Music d'ici)
    json r = json::parse(plat::httpGet("https://itunes.apple.com/" + q + "&country=FR"), nullptr, false);
    return r.is_object() && r["results"].is_array() ? r["results"] : json::array();
}
static std::string spImage(const json& imgs) {  // la plus petite image d'au moins 160 px
    std::string best;
    int bw = 1 << 30;
    if (imgs.is_array()) for (auto& i : imgs) { int w = (int)jnum(i, "width"); if (w >= 160 && w < bw) { bw = w; best = jstr(i, "url"); } }
    return best.empty() && imgs.is_array() && !imgs.empty() ? jstr(imgs[0], "url") : best;
}
static Track spTrack(json t) {  // copie : champs absents = null
    std::string artists;
    if (t["artists"].is_array()) for (auto& a : t["artists"]) artists += (artists.empty() ? "" : ", ") + jstr(a, "name");
    return Track{jstr(t, "uri"), jstr(t, "name"), artists, jstr(t["album"], "name"), spImage(t["album"]["images"]), (int)jnum(t, "duration_ms")};
}

static void openCard(const Card& c) {  // g_mx tenu : page d'artiste ou album
    if (!findList(c.uri)) { List l{c.uri, c.name, c.image}; l.hidden = true; g_lists.push_back(l); }
    g_view = c.uri; g_lyricsOpen = false;
}

static void send(const json& j);

static void searchCatalog(const std::string& q) {  // g_mx tenu
    g_artists.clear();
    if (g_webOk) {
        send({{"cmd", "search"}, {"q", q}});
        send({{"cmd", "get"}, {"id", "artists:" + q}, {"path", "/v1/search?type=artist&limit=8&q=" + urlEnc(q)}});
        return;
    }
    std::thread([q] {
        std::vector<Card> artists;
        for (auto& a : itunes("search?entity=musicArtist&limit=8&term=" + urlEnc(q))) {
            std::string id = std::to_string((long long)jnum(a, "artistId"));
            json alb = itunes("lookup?entity=album&limit=1&id=" + id);  // photo : la pochette de son dernier album
            artists.push_back({"artist:it:" + id, jstr(a, "artistName"), alb.size() > 1 ? itArt(alb[1]) : ""});
        }
        std::vector<Track> songs;
        for (auto& r : itunes("search?entity=song&limit=30&term=" + urlEnc(q))) songs.push_back(itTrack(r));
        std::lock_guard lk(g_mx);
        if (q != g_searchSent) return;
        g_artists = std::move(artists);
        for (auto& t : songs)
            if (std::none_of(g_search.tracks.begin(), g_search.tracks.end(), [&](const Track& y) { return y.uri == t.uri; })) g_search.tracks.push_back(t);
        wake();
    }).detach();
}

static void loadCatalogList(List& l) {  // g_mx tenu : titres d'une page d'artiste ou d'un album iTunes
    if (l.uri.rfind("artist:sp:", 0) == 0) {
        std::string id = l.uri.substr(10);
        send({{"cmd", "get"}, {"id", "top:" + l.uri}, {"path", "/v1/artists/" + id + "/top-tracks?market=from_token"}});
        send({{"cmd", "get"}, {"id", "albums:" + l.uri}, {"path", "/v1/artists/" + id + "/albums?include_groups=album,single&limit=30"}});
        return;
    }
    std::thread([uri = l.uri] {
        bool artist = uri.rfind("artist:it:", 0) == 0;
        std::string id = uri.substr(artist ? 10 : 13);
        std::vector<Track> tracks;
        std::vector<Card> albums;
        for (auto& r : itunes("lookup?entity=song&limit=100&id=" + id)) if (jstr(r, "wrapperType") == "track") tracks.push_back(itTrack(r));
        if (artist)
            for (auto& r : itunes("lookup?entity=album&limit=50&id=" + id))
                if (jstr(r, "wrapperType") == "collection") albums.push_back({"itunes:album:" + std::to_string((long long)jnum(r, "collectionId")), jstr(r, "collectionName"), itArt(r)});
        std::lock_guard lk(g_mx);
        if (List* l = findList(uri)) {
            l->tracks = std::move(tracks); l->loaded = true;
            if (l->image.empty() && !l->tracks.empty()) l->image = l->tracks[0].image;
        }
        if (artist) g_albums[uri] = std::move(albums);
        wake();
    }).detach();
}

// ---------- moteur Spotify (glass-engine : bibliothèque, paroles, Web API) ----------
static void send(const json& j) { plat::engineSend(j.dump() + "\n"); }
static void amPost(const json& j);

static void requestTracks(List& l) {
    if (l.requested || l.own || l.uri == "local" || l.uri == "search") return;
    if (l.uri.rfind("artist:", 0) == 0 || l.uri.rfind("itunes:album:", 0) == 0) { l.requested = true; loadCatalogList(l); return; }
    if (l.uri.rfind("am:", 0) == 0) { if (g_amAuth) { l.requested = true; amPost({{"cmd", "list"}, {"id", l.uri.substr(3)}}); } return; }
    if (isApple(l.uri)) { l.requested = true; loadApple(l.uri); return; }
    if (isSoundcloud(l.uri)) { l.requested = true; loadSoundcloud(l.uri); return; }
    if (!g_spReady) return;
    l.requested = true;
    send({{"cmd", "tracks"}, {"uri", l.uri}});
}

static void ytPlay(const Track& t);
static void wvPost(const json& j);

static void onEngine(const std::string& line) try {
    json j = json::parse(line, nullptr, false);
    std::lock_guard lk(g_mx);
    if (j.is_discarded() || !j.is_object()) {
        if (line.find("panicked") != std::string::npos) g_status = "Erreur moteur : " + line.substr(0, 160);
        return;
    }
    std::string ev = jstr(j, "ev");
    if (ev == "status" || ev == "error") g_status = jstr(j, "msg");
    else if (ev == "ready") {
        g_spReady = true; g_user = jstr(j, "user"); g_status.clear();
        for (auto& l : g_lists) if (l.custom) requestTracks(l);  // pour afficher leurs noms
    } else if (ev == "library") {
        std::vector<List> next;
        for (auto& it : j["items"]) {
            List l{jstr(it, "uri"), jstr(it, "name"), jstr(it, "image")};
            if (List* old = findList(l.uri)) { l.tracks = std::move(old->tracks); l.loaded = old->loaded; l.requested = old->requested; }
            applyOverride(l);
            next.push_back(std::move(l));
        }
        for (auto& l : g_lists)
            if ((l.custom || l.own || l.hidden || l.uri == "local" || isApple(l.uri)) && std::none_of(next.begin(), next.end(), [&](const List& x) { return x.uri == l.uri; })) next.push_back(std::move(l));
        g_lists = std::move(next);
    } else if (ev == "tracks") {
        if (List* l = findList(jstr(j, "uri"))) {
            l->loaded = true; l->tracks.clear();
            if (auto n = jstr(j, "name"); !n.empty()) l->name = n;
            for (auto& t : j["items"]) if (Track x = trackFrom(t); !x.uri.empty()) l->tracks.push_back(x);
            if (l->image.empty() && !l->tracks.empty()) l->image = l->tracks[0].image;
            if (j.contains("error")) { g_status = "Impossible de charger : " + jstr(j, "error"); if (l->custom) l->name = "Introuvable"; }
            applyOverride(*l);
        }
    } else if (ev == "lyrics" && jstr(j, "key") == g_lyrKey) {
        g_lyr.clear();
        for (auto& l : j["lines"]) g_lyr.push_back({(int)jnum(l, "t"), jstr(l, "w")});
        g_lyrSynced = j.value("synced", false);
        g_lyrSrc = jstr(j, "src");
        g_lyrState = g_lyr.empty() ? 3 : 2;
        g_lyrAlign = !g_lyr.empty() && g_lyrSrc.rfind("LRCLIB", 0) == 0;  // les paroles de Spotify sont déjà calées
    } else if (ev == "get") {
        std::string id = jstr(j, "id");
        json d = j.contains("data") ? j["data"] : json();  // copie : les champs absents deviennent null au lieu de planter
        if (id == "artists:" + g_searchSent && d.is_object()) {
            g_artists.clear();
            for (auto& a : d["artists"]["items"]) g_artists.push_back({"artist:sp:" + jstr(a, "id"), jstr(a, "name"), spImage(a["images"])});
        } else if (id.rfind("top:", 0) == 0 && d.is_object()) {
            if (List* l = findList(id.substr(4))) {
                l->tracks.clear(); l->loaded = true;
                for (auto& t : d["tracks"]) l->tracks.push_back(spTrack(t));
            }
        } else if (id.rfind("albums:", 0) == 0 && d.is_object()) {
            auto& v = g_albums[id.substr(7)];
            v.clear();
            for (auto& a : d["items"]) v.push_back({jstr(a, "uri"), jstr(a, "name"), spImage(a["images"])});
        }
    } else if (ev == "search" && jstr(j, "q") == g_searchSent) {
        // résultats du catalogue ajoutés après ceux de ta bibliothèque
        for (auto& t : j["items"]) if (Track x = trackFrom(t); !x.uri.empty() &&
            std::none_of(g_search.tracks.begin(), g_search.tracks.end(), [&](const Track& y) { return y.uri == x.uri; })) g_search.tracks.push_back(x);
    } else if (ev == "web") {
        g_webOk = j.value("ok", false);
        if (j.contains("msg")) g_status = jstr(j, "msg");
        else if (g_webOk && g_status.rfind("Accepte", 0) == 0) g_status = "Spotify officiel connecté";
    } else if (ev == "token") {
        wvPost({{"token", jstr(j, "token")}});
    } else if (ev == "api") {
        int st = (int)jnum(j, "status");
        if (st < 200 || st >= 300) {
            g_status = "Spotify : " + (jstr(j, "msg").empty() ? "erreur " + std::to_string(st) : jstr(j, "msg"));
            if (jstr(j, "id") == "play" && g_sp) {  // le lecteur officiel refuse ce titre
                g_sp = false; g_loading = false;
                if (g_set.source == 0 && g_has) ytPlay(g_cur);
                else g_failed = true;
            }
        }
    }
} catch (...) {}  // une ligne inattendue ne doit pas tuer le lecteur

static int g_engGen = 0;  // l'arrêt d'un ancien moteur ne doit pas marquer le nouveau comme arrêté

static void startEngine() {
#ifdef _WIN32
    fs::path exe = g_bin / "glass-engine.exe";
#else
    fs::path exe = g_bin / "glass-engine";
#endif
    if (!fs::exists(exe)) { g_status = "glass-engine manquant : relance la compilation"; return; }
    int gen = ++g_engGen;
    g_engineOk = plat::engineStart(exe, onEngine, [gen] { std::lock_guard lk(g_mx); if (gen == g_engGen) g_engineOk = g_spReady = false; });
    if (!g_engineOk) g_status = "Impossible de lancer glass-engine";
}

static void logout() {  // oublie le compte Spotify et redemande la connexion
    plat::engineStop();
    std::error_code ec;
    fs::remove(g_dir / "spotify-cache" / "credentials.json", ec);
    fs::remove(g_dir / "spotify-cache" / "webapi.json", ec);
    g_lists.erase(std::remove_if(g_lists.begin(), g_lists.end(), [](const List& l) { return !l.custom && !l.own && l.uri != "local" && !isApple(l.uri); }), g_lists.end());
    for (auto& l : g_lists) l.requested = false;
    g_spReady = g_webOk = false; g_user.clear();
    startEngine();
}

// ---------- YouTube : l'audio des titres Spotify / Apple Music (mode Auto ou YouTube) ----------
static const char* kNoYtdlp = "yt-dlp en cours d'installation, réessaie dans un instant";

// yt-dlp : Glass installe sa propre copie « décompressée » (démarre ~4x plus vite que la version en un seul fichier)
// et la renouvelle chaque semaine (YouTube change souvent). La nouvelle copie remplace l'ancienne au lancement suivant.
#ifdef _WIN32
static const char *kYtZip = "yt-dlp_win.zip", *kYtExe = "yt-dlp.exe";
#else
static const char *kYtZip = "yt-dlp_macos.zip", *kYtExe = "yt-dlp_macos";
#endif

static fs::path ytdlpPath() {
    fs::path own = g_dir / "bin" / "yt-dlp" / kYtExe;
    return fileExists(own) ? own : plat::findTool("yt-dlp");
}

static void updateYtdlp() {  // au démarrage
    fs::path bin = g_dir / "bin", dir = bin / "yt-dlp", fresh = bin / "yt-dlp.new";
    std::error_code ec;
    if (fileExists(fresh / kYtExe)) { fs::remove_all(dir, ec); fs::rename(fresh, dir, ec); }  // rien ne l'utilise encore
    auto age = fs::file_time_type::clock::now() - fs::last_write_time(dir / kYtExe, ec);
    if (!ec && age < std::chrono::hours(24 * 7)) return;
    std::thread([bin, dir, fresh] {
        std::error_code ec;
        fs::path zip = bin / "yt-dlp.zip";
        fs::remove_all(fresh, ec);
        fs::create_directories(fresh, ec);
        if (!plat::download(std::string("https://github.com/yt-dlp/yt-dlp/releases/latest/download/") + kYtZip, zip, [](int) {})) return;
#ifdef _WIN32
        bool ok = plat::run({"tar.exe", "-xf", ustr(zip), "-C", ustr(fresh)}, 180000);
#else
        bool ok = plat::run({"/usr/bin/ditto", "-x", "-k", ustr(zip), ustr(fresh)}, 180000);
#endif
        fs::remove(zip, ec);
        if (!ok || !fileExists(fresh / kYtExe)) { fs::remove_all(fresh, ec); return; }
        fs::last_write_time(fresh / kYtExe, fs::file_time_type::clock::now(), ec);  // date de l'installation, pas de la compilation
        if (!fileExists(dir / kYtExe)) { fs::remove_all(dir, ec); fs::rename(fresh, dir, ec); }  // 1re installation : tout de suite
        wake();
    }).detach();
}

static fs::path cachePath(const Track& t) { return g_dir / "cache" / upath(t.uri.substr(t.uri.rfind(':') + 1) + ".m4a"); }

// Télécharge l'audio d'un titre dans cache ; renvoie le chemin ou "" si introuvable.
static fs::path ytDownload(const Track& t) {
    fs::path path = cachePath(t), base = path, ytdlp = ytdlpPath();
    base.replace_extension();
    for (;;) {  // un seul yt-dlp par titre à la fois (lecture + préchargement)
        { std::lock_guard lk(g_mx); if (!g_inflight.count(t.uri)) { g_inflight.insert(t.uri); break; } }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::string bare = std::regex_replace(t.title, std::regex(R"(\s*[\(\[][^\)\]]*[\)\]])"), "");
    std::string artist1 = t.artist.substr(0, t.artist.find(','));
    int sec = t.ms / 1000, tol = std::max(10, sec / 10);
    std::string filter = "duration>=" + std::to_string(sec - tol) + " & duration<=" + std::to_string(sec + tol);
    // la bonne durée ne suffit pas (autre morceau de même longueur) : le titre doit contenir son premier mot
    std::string word = bare.substr(0, bare.find(' '));
    word.erase(std::remove_if(word.begin(), word.end(), [](unsigned char c) { return c < 0x80 && !isalnum(c); }), word.end());
    if (word.size() >= 2) filter += " & title~='(?i)" + word + "'";
    auto ytm = [](const std::string& q) {  // recherche « titres » de YouTube Music : les versions officielles, comme sur Spotify
        std::string u = "https://music.youtube.com/search?q=";
        for (unsigned char c : q) { char h[4]; if (isalnum(c)) u += (char)c; else { snprintf(h, sizeof h, "%%%02X", c); u += h; } }
        return u + "#songs";
    };
    // D'abord la bonne durée (évite clips, live, versions longues), puis sans filtre.
    // YouTube classique filtre certains mots (aucun résultat) : YouTube Music passe en premier.
    struct Try { std::string target; bool filtered; };
    std::string full = t.artist + " " + t.title;
    std::vector<Try> tries = t.uri.rfind("soundcloud:track:", 0) == 0 ? std::vector<Try>{{"https://api.soundcloud.com/tracks/" + t.uri.substr(17), false}} : std::vector<Try>{{ytm(full), true}, {"ytsearch5:" + t.artist + " - " + t.title + " audio", true},
                              {"ytsearch5:" + artist1 + " " + bare, true}, {ytm(full), false}, {"ytsearch1:" + t.artist + " - " + t.title, false}};
    for (auto& [target, filtered] : tries) {
        if (ytdlp.empty() || fileExists(path)) break;
        if (filtered && !sec) continue;
        std::string q = target;
        q.erase(std::remove_if(q.begin(), q.end(), [](char c) { return c == '"' || c == '\\'; }), q.end());
#ifdef _WIN32
        std::vector<std::string> args = {ustr(ytdlp), "-f", "bestaudio[ext=m4a]/bestaudio", "-x", "--audio-format", "m4a"};
#else
        std::vector<std::string> args = {ustr(ytdlp), "-f", "bestaudio[ext=m4a]"};  // sans ffmpeg : directement en m4a, lu par AVFoundation
#endif
        args.insert(args.end(), {"-q", "--no-warnings", "-N", "8", "-o", ustr(base) + ".%(ext)s"});  // -N : morceaux en parallèle (SoundCloud : 28 s -> 2 s)
        if (filtered) args.insert(args.end(), {"--playlist-items", "1:5", "--match-filter", filter, "--max-downloads", "1"});
        else args.insert(args.end(), {"--playlist-items", "1"});
        args.push_back(q);
        plat::run(args, 120000);
    }
    std::lock_guard lk(g_mx);
    g_inflight.erase(t.uri);
    return fileExists(path) ? path : fs::path();
}

// ---------- paroles générées par IA (whisper.cpp, sur l'ordinateur) quand aucune n'existe ----------
// shortcut: échoue sur les voix noyées (versions très accélérées) ; la séparation voix/musique serait trop lourde.
static bool g_genRunning = false;

static fs::path lyricsFile(const Track& t, const char* ext) {
    std::string id = t.local ? std::to_string(std::hash<std::string>{}(t.uri)) : t.uri.substr(t.uri.rfind(':') + 1);
    return g_dir / "lyrics" / upath(id + ext);
}

// Téléchargement avec avancement dans la barre d'état.
static bool httpDownload(const std::string& url, const fs::path& path, const std::string& what) {
    return plat::download(url, path, [&](int pct) { { std::lock_guard lk(g_mx); g_status = what + " : " + std::to_string(pct) + " %"; } wake(); });
}

// Lit des paroles .lrc ; junk : retire les inventions typiques de Whisper (sous-titres, « musique »...). Recale les temps (scale).
static std::vector<LyricLine> parseLrc(const std::string& text, double scale, bool junkFilter = true) {
    std::vector<LyricLine> out;
    std::istringstream f(text);
    static const char* junk[] = {"subtitle", "sous-titr", "amara", "thanks for watching", "merci d'avoir", "music", "musique", "musik",
                                 "субтитр", "редактор", "корректор", "продолжение следует", "музык", "by:whisper", "♪"};
    for (std::string l; std::getline(f, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.size() < 10 || l[0] != '[' || !isdigit((unsigned char)l[1])) continue;
        int mm = atoi(l.c_str() + 1); double ss = atof(l.c_str() + 4);
        std::string w = l.substr(l.find(']') + 1);
        w.erase(0, w.find_first_not_of(' '));
        std::string low = w;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)tolower(c); });
        if (junkFilter && (w.empty() || w[0] == '[' || w[0] == '(' || std::any_of(std::begin(junk), std::end(junk), [&](const char* j) { return low.find(j) != std::string::npos; }))) continue;
        if (junkFilter && out.size() >= 2 && out.back().w == w && out[out.size() - 2].w == w) continue;  // boucle d'hallucination
        out.push_back({(int)((mm * 60 + ss) * 1000 * scale), w});
    }
    return out;
}

static std::string readFile(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static std::vector<LyricLine> cleanLrc(const fs::path& path, double scale) { return parseLrc(readFile(path), scale); }

static int lrcLength(const std::string& text) {  // balise [length: mm:ss.xx] (durée de la version transcrite), 0 si absente
    size_t p = text.find("[length:");
    if (p == std::string::npos) return 0;
    int mm = atoi(text.c_str() + p + 8);
    size_t c = text.find(':', p + 8);
    return c == std::string::npos ? 0 : (int)((mm * 60 + atof(text.c_str() + c + 1)) * 1000);
}

static void loadLrcInto(const fs::path& path, double scale, const char* src = "générées par IA (Whisper)") {  // g_mx tenu
    g_lyr = cleanLrc(path, scale);
    g_lyrSynced = true; g_lyrSrc = src;
    g_lyrState = g_lyr.size() >= 2 ? 2 : 3;
}

// ---------- paroles sur Internet (LRCLIB), sans le moteur Spotify : Apple Music, fichiers, Spotify non connecté ----------
static std::string urlEnc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) { if (isalnum(c) || strchr("-_.~", c)) o += (char)c; else { char h[4]; snprintf(h, sizeof h, "%%%02X", c); o += h; } }
    return o;
}
static std::string firstArtist(const std::string& a) {
    std::string r = a.substr(0, std::min(a.find(','), a.find(" & ")));
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}
static std::string baseTitle(const std::string& t) {  // sans « - Slowed », « (sped up) »... : les paroles de l'original servent aussi
    std::string out;
    int depth = 0;
    for (char c : t.substr(0, t.find(" - "))) { if (c == '(' || c == '[') depth++; else if (c == ')' || c == ']') depth--; else if (depth <= 0) out += c; }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// LRCLIB : paroles (synchronisées si possible) ; false si rien.
static bool lrclib(const Track& t, std::vector<LyricLine>& lines, bool& synced, std::string& src) {
    auto get = [](const std::string& q) { return json::parse(plat::httpGet("https://lrclib.net/api/" + q), nullptr, false); };
    std::string artist = firstArtist(t.artist), base = baseTitle(t.title);
    json found = json::array();
    json exact = get("get?artist_name=" + urlEnc(artist) + "&track_name=" + urlEnc(t.title) + "&duration=" + std::to_string(t.ms / 1000));
    if (exact.is_object() && (exact["syncedLyrics"].is_string() || exact["plainLyrics"].is_string())) found.push_back(exact);
    else for (const std::string& q : {"search?track_name=" + urlEnc(base) + "&artist_name=" + urlEnc(artist), "search?q=" + urlEnc(artist + " " + base)}) {
        json r = get(q);
        if (r.is_array() && !r.empty()) { found = r; break; }
    }
    // des paroles synchronisées d'abord ; version accélérée / ralentie : temps étirés selon la durée
    const json* pick = nullptr;
    for (auto& d : found) if (d.is_object() && !jstr(d, "syncedLyrics").empty()) { pick = &d; break; }
    if (!pick) for (auto& d : found) if (d.is_object() && !jstr(d, "plainLyrics").empty()) { pick = &d; break; }
    lines.clear(); synced = false; src = "LRCLIB";
    if (!pick) return false;
    double dur = jnum(*pick, "duration") * 1000, scale = dur > 0 && t.ms > 0 && std::fabs(t.ms / dur - 1) > 0.04 ? t.ms / dur : 1;
    if (scale != 1) { char b[64]; snprintf(b, sizeof b, "LRCLIB (version originale, tempo ajusté x%.2f)", scale); src = b; }
    if (synced = !jstr(*pick, "syncedLyrics").empty(); synced) lines = parseLrc(jstr(*pick, "syncedLyrics"), scale, false);
    else { std::istringstream ps(jstr(*pick, "plainLyrics")); for (std::string l; std::getline(ps, l);) if (!l.empty() && l != "\r") lines.push_back({0, l}); }
    return !lines.empty();
}

// ---------- base de paroles partagée (GitHub) : les paroles de l'IA (écrites ou calées sur le son) servent à tout le monde ----------
// Lecture publique ; envoi seulement depuis un ordinateur où l'outil « gh » est connecté au compte du dépôt.
static const char* kLyricsRepo = "lanaryy-hash/glass-lyrics";

static std::string sharedKey(const Track& t) {  // même titre = même clé, qu'il vienne de Spotify, d'Apple Music ou d'un fichier
    std::string k;
    for (unsigned char c : firstArtist(t.artist) + "|" + t.title) if (isalnum(c) || c >= 0x80 || c == '|') k += (char)tolower(c);
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : k) { h ^= c; h *= 1099511628211ull; }
    char b[24]; snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
    return b;
}

static std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t n = (uint8_t)in[i] << 16 | (i + 1 < in.size() ? (uint8_t)in[i + 1] << 8 : 0) | (i + 2 < in.size() ? (uint8_t)in[i + 2] : 0);
        o += t[n >> 18 & 63]; o += t[n >> 12 & 63];
        o += i + 1 < in.size() ? t[n >> 6 & 63] : '='; o += i + 2 < in.size() ? t[n & 63] : '=';
    }
    return o;
}

static std::set<std::string> g_shared;  // déjà envoyées pendant cette session

static void shareLyrics(const Track& t, const std::string& lrc, bool replace = false) {  // g_mx tenu
    fs::path gh = plat::findTool("gh");
    if (gh.empty() || (!g_shared.insert(t.uri).second && !replace)) return;
    std::thread([gh, t, lrc, replace] {  // sans droits ou sans gh : rien ne se passe
        std::string path = std::string("repos/") + kLyricsRepo + "/contents/" + sharedKey(t) + ".lrc";
        std::vector<std::string> args = {ustr(gh), "api", "-X", "PUT", path, "-f", "message=Paroles : " + t.artist + " - " + t.title, "-f", "content=" + base64(lrc)};
        if (replace) {  // remplacer une version moins bonne : GitHub demande l'empreinte du fichier actuel
            json cur = json::parse(plat::httpGet(std::string("https://api.github.com/") + path), nullptr, false);
            if (cur.is_object() && !jstr(cur, "sha").empty()) args.insert(args.end(), {"-f", "sha=" + jstr(cur, "sha")});
        }
        plat::run(args, 30000);
    }).detach();
}

static std::string lrcHeader(const Track& t, const char* by = "Glass (IA Whisper)") {
    char len[32]; snprintf(len, sizeof len, "%02d:%05.2f", t.ms / 60000, (t.ms % 60000) / 1000.0);
    return "[ar: " + t.artist + "]\n[ti: " + t.title + "]\n[length: " + len + "]\n[by: " + by + "]\n";
}
static std::string lrcText(const Track& t, const std::vector<LyricLine>& lines, const char* by) {
    std::string text = lrcHeader(t, by);
    for (auto& l : lines) { char ts[16]; snprintf(ts, sizeof ts, "[%02d:%05.2f]", l.t / 60000, (l.t % 60000) / 1000.0); text += std::string(ts) + " " + l.w + "\n"; }
    return text;
}
static const char* lrcSource(const std::string& text) {
    return text.find("IA calée") != std::string::npos ? "trouvées sur Internet, calées sur le son par l'IA" : "écrites par l'IA (Whisper)";
}

static std::string fetchShared(const Track& t) {  // "" si le titre n'est pas dans la base
    std::string s = plat::httpGet(std::string("https://raw.githubusercontent.com/") + kLyricsRepo + "/main/" + sharedKey(t) + ".lrc");
    return parseLrc(s, 1).size() >= 2 ? s : "";
}

static double lrcScale(const Track& t, const std::string& text) {  // paroles d'une autre version (durée différente) : temps étirés
    int len = lrcLength(text);
    return len > 0 && t.ms > 0 && std::fabs((double)t.ms / len - 1) > 0.04 ? (double)t.ms / len : 1;
}


static std::set<std::string> g_listened;  // titres déjà écoutés par l'IA pendant la session

static bool useShared(const Track& t) {  // base partagée : true si trouvées (g_mx NON tenu)
    std::string shared = fetchShared(t);
    if (shared.empty()) return false;
    std::ofstream(lyricsFile(t, ".lrc"), std::ios::binary) << shared;
    std::lock_guard lk(g_mx);
    g_shared.insert(t.uri);
    if (g_lyrKey == t.uri) { loadLrcInto(lyricsFile(t, ".lrc"), lrcScale(t, shared), lrcSource(shared)); g_lyrAlign = false; }
    wake();
    return true;
}

// Paroles sur Internet sans le moteur Spotify (Apple Music, fichiers, Spotify non connecté) : base Glass, puis LRCLIB.
static void findLyrics(const Track& t) {
    std::thread([t] {
        if (useShared(t)) return;
        std::vector<LyricLine> lines;
        bool synced;
        std::string src;
        lrclib(t, lines, synced, src);
        std::lock_guard lk(g_mx);
        if (g_lyrKey != t.uri) return;
        g_lyr = std::move(lines); g_lyrSynced = synced; g_lyrSrc = src;
        g_lyrState = g_lyr.empty() ? 3 : 2;
        g_lyrAlign = !g_lyr.empty();
        wake();
    }).detach();
}

// ---------- l'IA écoute le titre en entier (whisper.cpp) ----------
struct Word { std::string w; int t; };  // mot entendu et moment où il est chanté (ms)

static std::string normWord(const std::string& s) {  // minuscules, sans accents ni ponctuation (latin, cyrillique...)
    std::string o;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        uint32_t cp; int n;
        if (c < 0x80) { cp = c; n = 1; } else if ((c >> 5) == 6) { cp = c & 31; n = 2; } else if ((c >> 4) == 14) { cp = c & 15; n = 3; } else { cp = c & 7; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); k++) cp = cp << 6 | (s[i + k] & 63);
        i += n;
        if (cp < 0x80) { if (isalnum((int)cp)) o += (char)tolower((int)cp); continue; }
        if (cp >= 0xC0 && cp <= 0xFF && cp != 0xD7 && cp != 0xF7) {  // lettres accentuées -> sans accent
            static const char* m = "aaaaaaaceeeeiiiidnooooo/ouuuuyts" "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
            char r = m[cp - 0xC0];
            if (r != '/') o += r;
            continue;
        }
        if (cp >= 0x410 && cp <= 0x42F) cp += 0x20; else if (cp >= 0x400 && cp <= 0x40F) cp += 0x50;  // cyrillique en minuscules
        if (cp == 0x2019 || cp == 0x2018 || cp == 0x2026 || cp == 0xAB || cp == 0xBB || (cp >= 0x2010 && cp <= 0x2015)) continue;  // ponctuation
        if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 63)); }
        else { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | (cp >> 6 & 63)); o += (char)(0x80 | (cp & 63)); }
    }
    return o;
}

static bool sameWord(const std::string& a, const std::string& b) {  // égaux, ou presque pour les mots longs (fautes de l'IA)
    if (a == b) return true;
    size_t n = std::min(a.size(), b.size());
    if (n < 4 || std::max(a.size(), b.size()) - n > 2) return false;
    std::vector<int> d(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) d[j] = (int)j;
    for (size_t i = 1; i <= a.size(); i++) {
        int prev = d[0]; d[0] = (int)i;
        for (size_t j = 1; j <= b.size(); j++) { int cur = d[j]; d[j] = std::min({d[j] + 1, d[j - 1] + 1, prev + (a[i - 1] != b[j - 1])}); prev = cur; }
    }
    return d[b.size()] <= (n >= 8 ? 2 : 1);
}

// Cale chaque ligne des paroles sur le moment où l'IA a entendu ses mots ; ratio = part des mots retrouvés.
static std::vector<LyricLine> alignLines(std::vector<LyricLine> ref, const std::vector<Word>& heard, double& ratio, bool timed) {
    std::vector<std::pair<std::string, int>> rw;  // mot normalisé, ligne
    std::vector<int> firstWord(ref.size());
    for (size_t i = 0; i < ref.size(); i++) {
        firstWord[i] = (int)rw.size();
        std::istringstream ws(ref[i].w);
        for (std::string w; ws >> w;) if (auto n = normWord(w); !n.empty()) rw.push_back({n, (int)i});
    }
    size_t n = rw.size(), m = heard.size();
    ratio = 0;
    if (!n || !m) return {};
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1));  // plus longue suite de mots communs (en tolérant les fautes)
    for (size_t i = 1; i <= n; i++)
        for (size_t j = 1; j <= m; j++)
            dp[i][j] = std::max({dp[i - 1][j], dp[i][j - 1], sameWord(rw[i - 1].first, heard[j - 1].w) ? dp[i - 1][j - 1] + 1 : 0});
    std::vector<int> wordT(n, -1);
    for (size_t i = n, j = m; i && j;) {
        if (sameWord(rw[i - 1].first, heard[j - 1].w) && dp[i][j] == dp[i - 1][j - 1] + 1) { wordT[i - 1] = heard[j - 1].t; i--; j--; }
        else if (dp[i - 1][j] >= dp[i][j - 1]) i--; else j--;
    }
    ratio = (double)dp[n][m] / n;
    // début de chaque ligne : médiane de ce qu'indique chaque mot retrouvé (son moment, moins ~0,35 s par mot avant lui dans la ligne) ;
    // un mot isolé mal reconnu (ex. pendant l'intro) ne déplace pas la ligne
    std::vector<int> start(ref.size(), -1);
    std::vector<std::vector<int>> est(ref.size());
    for (size_t k = 0; k < n; k++)
        if (wordT[k] >= 0) est[rw[k].second].push_back(std::max(0, wordT[k] - (int)(k - firstWord[rw[k].second]) * 350));
    for (size_t i = 0; i < ref.size(); i++)
        if (!est[i].empty()) { std::nth_element(est[i].begin(), est[i].begin() + est[i].size() / 2, est[i].end()); start[i] = est[i][est[i].size() / 2]; }
    if (timed) {
        // Paroles déjà minutées : l'IA situe mal chaque mot chanté, mais très bien l'ensemble. On garde les écarts entre lignes
        // et on corrige seulement le décalage et la vitesse (autre version, intro plus longue...) : temps = a * temps + b.
        std::vector<std::pair<double, double>> pts;
        for (size_t i = 0; i < ref.size(); i++) if (est[i].size() >= 2) pts.push_back({(double)ref[i].t, (double)start[i]});
        if (pts.size() < 6) { ratio = std::min(ratio, 0.2); return {}; }  // trop peu de repères : paroles laissées telles quelles
        std::vector<double> slopes, offs;
        for (size_t i = 0; i < pts.size(); i++)
            for (size_t j = i + 1; j < pts.size(); j++)
                if (pts[j].first - pts[i].first > 5000) slopes.push_back((pts[j].second - pts[i].second) / (pts[j].first - pts[i].first));
        auto median = [](std::vector<double>& v) { std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
        double a = slopes.empty() ? 1 : std::clamp(median(slopes), 0.8, 1.25);
        if (std::fabs(a - 1) < 0.02) a = 1;
        for (auto& [rt, ht] : pts) offs.push_back(ht - a * rt);
        double b = median(offs);
        if (a == 1 && std::fabs(b) < 1000) b = 0;  // moins d'une seconde : c'est l'imprécision de l'IA, les paroles sont déjà bien calées
        for (auto& l : ref) l.t = std::max(0, (int)(a * l.t + b));
        return ref;
    }
    // lignes sans mot retrouvé : entre leurs voisines, selon le nombre de mots
    for (size_t i = 0; i < ref.size(); i++) {
        if (start[i] >= 0) continue;
        int a = (int)i - 1, b = (int)i + 1;
        while (a >= 0 && start[a] < 0) a--;
        while (b < (int)ref.size() && start[b] < 0) b++;
        if (a >= 0 && b < (int)ref.size()) {
            double f = firstWord[b] > firstWord[a] ? double(firstWord[i] - firstWord[a]) / (firstWord[b] - firstWord[a]) : 0.5;
            start[i] = start[a] + (int)((start[b] - start[a]) * std::clamp(f, 0.05, 0.95));
        } else if (a >= 0) start[i] = start[a] + 2500 * ((int)i - a);
        else if (b < (int)ref.size()) start[i] = std::max(0, start[b] - 2500 * (b - (int)i));
        else start[i] = 0;
    }
    for (size_t i = 0; i < ref.size(); i++) ref[i].t = i ? std::max(start[i], ref[i - 1].t + 100) : start[i];
    return ref;
}

static std::vector<LyricLine> wordsToLines(const std::vector<Word>& heard, const std::vector<std::string>& raw) {  // paroles écrites par l'IA
    std::string lrc;
    for (size_t i = 0; i < heard.size();) {
        size_t j = i;
        std::string line;
        while (j < heard.size() && (j == i || (heard[j].t - heard[j - 1].t < 1200 && j - i < 9 && !strchr(".?!", raw[j - 1].back())))) line += (line.empty() ? "" : " ") + raw[j++];
        char ts[16]; snprintf(ts, sizeof ts, "[%02d:%05.2f]", heard[i].t / 60000, (heard[i].t % 60000) / 1000.0);
        lrc += std::string(ts) + " " + line + "\n";
        i = j;
    }
    return parseLrc(lrc, 1);  // + filtre des inventions de l'IA
}

// Écoute le titre une fois : cale les paroles trouvées (ref) sur le son, ou les écrit si ref est vide ou n'est pas ce titre.
static void listenLyrics(const Track& t, std::vector<LyricLine> ref, bool ai, bool fresh = false) {  // g_mx tenu ; fresh : réécouter même si la base Glass a déjà une version
    if (g_genRunning) return;
    g_genRunning = true;
    g_listened.insert(t.uri);
    bool shown = !ref.empty();  // des paroles sont déjà à l'écran : on travaille sans les cacher
    if (!shown && g_lyrKey == t.uri) g_lyrState = 4;
    bool timed = g_lyrSynced && std::any_of(ref.begin(), ref.end(), [](const LyricLine& l) { return l.t > 0; });  // paroles déjà minutées
    for (auto it = ref.begin(); it != ref.end();) if (it->w.empty() && !timed) it = ref.erase(it); else ++it;
    std::thread([t, ref, ai, shown, fresh, timed] {
        fs::path wdir = g_dir / "whisper", model = wdir / "ggml-small-q5_1.bin";
        std::error_code ec;
        fs::create_directories(wdir, ec);
        fs::create_directories(g_dir / "lyrics", ec);
        auto fail = [&](const std::string& msg, bool none) {
            std::lock_guard lk(g_mx);
            g_genRunning = false; g_status = msg;
            if (!shown && g_lyrKey == t.uri) g_lyrState = 5;
            if (none) std::ofstream{lyricsFile(t, ".none")};  // ne pas réessayer à chaque ouverture
            wake();
        };
        { std::lock_guard lk(g_mx); g_status = "Recherche dans la base de paroles Glass..."; } wake();
        if (!fresh && useShared(t)) { std::lock_guard lk(g_mx); g_genRunning = false; g_status = "Paroles trouvées dans la base Glass"; wake(); return; }
        if (!ai) { std::lock_guard lk(g_mx); g_genRunning = false; g_status.clear(); if (!shown && g_lyrKey == t.uri) g_lyrState = 3; wake(); return; }
#ifdef _WIN32
        fs::path exe = wdir / "Release" / "whisper-cli.exe";
        if (!fileExists(exe)) {  // première fois : le programme (8 Mo) puis le modèle multilingue (190 Mo)
            fs::path zip = wdir / "whisper.zip";
            if (!httpDownload("https://github.com/ggml-org/whisper.cpp/releases/download/b5454/whisper-bin-x64.zip", zip, "IA des paroles")) return fail("Téléchargement de l'IA des paroles impossible", false);
            plat::run({"tar.exe", "-xf", ustr(zip), "-C", ustr(wdir)}, 60000);
            fs::remove(zip, ec);
            if (!fileExists(exe)) return fail("IA des paroles : installation impossible", false);
        }
#else
        fs::path exe = plat::findTool("whisper-cli");  // fourni avec Glass.app
        if (exe.empty()) return fail("IA des paroles absente de Glass.app", false);
#endif
        if (!fileExists(model) && !httpDownload("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q5_1.bin", model, "Modèle de l'IA des paroles (190 Mo, une seule fois)"))
            return fail("Téléchargement du modèle impossible", false);
        { std::lock_guard lk(g_mx); g_status = shown ? "L'IA écoute le titre pour caler les paroles..." : "L'IA écoute le titre pour écrire les paroles..."; } wake();
        fs::path audio = t.local ? upath(t.uri) : ytDownload(t);  // le son des lecteurs officiels est protégé : on prend la copie YouTube
        if (audio.empty()) return fail("Paroles : audio introuvable", false);
        fs::path wav = g_dir / "lyrics" / "tmp.wav", out = lyricsFile(t, ""), js = lyricsFile(t, ".json"), lrc = lyricsFile(t, ".lrc");
#ifdef _WIN32
        if (!plat::run({"ffmpeg.exe", "-loglevel", "error", "-y", "-i", ustr(audio), "-ar", "16000", "-ac", "1", "-c:a", "pcm_s16le", ustr(wav)}, 120000))
            return fail("Paroles : ffmpeg introuvable (winget install ffmpeg)", false);
#else
        if (!plat::run({"/usr/bin/afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", ustr(audio), ustr(wav)}, 120000))
            return fail("Paroles : conversion de l'audio impossible", false);
#endif
        auto bytes = fs::file_size(wav, ec);
        double wavMs = ec || bytes < 44 ? 0 : (bytes - 44) / 32.0;  // 16 kHz, 16 bits, mono
        if (t.ms > 0 && wavMs > 0 && std::fabs(t.ms / wavMs - 1) > 0.25) {  // autre morceau (remix, version longue...) : rien de fiable
            fs::remove(wav, ec);
            return fail("Paroles : la copie YouTube n'est pas la même version", !shown);
        }
        // mot par mot, avec les paroles trouvées comme indice (l'IA reconnaît bien mieux des mots qu'elle attend)
        int threads = std::max(2, (int)std::thread::hardware_concurrency() / 2);  // la moitié du processeur, priorité basse
        std::vector<std::string> args = {ustr(exe), "-m", ustr(model), "-f", ustr(wav), "-l", "auto", "-t", std::to_string(threads),
                                         "-oj", "-of", ustr(out), "-ml", "1", "-sow", "-np", "-sns"};
        std::string prompt;
        for (auto& l : ref) { if (prompt.size() > 600) break; prompt += l.w + " "; }
        if (!prompt.empty()) args.insert(args.end(), {"--prompt", prompt});
        bool ok = plat::run(args, 600000);
        fs::remove(wav, ec);
        json r = json::parse(readFile(js), nullptr, false);
        fs::remove(js, ec);
        if (!ok || !r.is_object() || !r["transcription"].is_array()) return fail("Paroles : l'IA n'a pas pu écouter le titre", false);
        double scale = t.ms > 0 && wavMs > 0 && std::fabs(t.ms / wavMs - 1) > 0.04 ? t.ms / wavMs : 1;  // copie YouTube d'une autre durée
        std::vector<Word> heard;
        std::vector<std::string> raw;
        for (auto& seg : r["transcription"]) {
            std::string text = jstr(seg, "text");
            int t0 = (int)(jnum(seg["offsets"], "from") * scale);
            std::istringstream ws(text);
            for (std::string w; ws >> w;) {
                if (w[0] == '[' || w[0] == '(' || w.find("♪") != std::string::npos) continue;
                if (auto n = normWord(w); !n.empty()) { heard.push_back({n, t0}); raw.push_back(w); }
            }
        }
        double ratio = 0;
        std::vector<LyricLine> lines = ref.empty() ? std::vector<LyricLine>() : alignLines(ref, heard, ratio, timed);
        const char* by = "Glass (IA calée sur le son)";
        std::string msg = "Paroles calées sur le son par l'IA";
        if (!ref.empty() && ratio < 0.3) {
            if (ratio >= 0.12) return fail("L'IA n'a pas assez reconnu les mots : paroles laissées telles quelles", false);
            lines.clear();  // presque rien en commun : les paroles trouvées ne sont pas celles de ce titre
            msg = "Les paroles trouvées n'étaient pas celles de ce titre : écrites par l'IA";
        }
        if (lines.empty()) { lines = wordsToLines(heard, raw); by = "Glass (IA Whisper)"; if (ref.empty()) msg = "Paroles écrites par l'IA"; }
        if (lines.size() < 2) return fail("Paroles : voix trop couverte par la musique, rien de fiable", !shown);
        std::string text = lrcText(t, lines, by);
        std::ofstream(lrc, std::ios::binary) << text;
        std::lock_guard lk(g_mx);
        g_genRunning = false; g_status = msg;
        shareLyrics(t, text, true);
        if (g_lyrKey == t.uri) { loadLrcInto(lrc, 1, lrcSource(text)); g_lyrAlign = false; }
        wake();
    }).detach();
}

// ---------- lecture (appelé avec g_mx verrouillé) ----------
static void wvPost(const json& j) { g_wvOut.push_back({0, j.dump()}); wake(); }  // page Spotify
static void amPost(const json& j) { g_wvOut.push_back({1, j.dump()}); wake(); }  // page Apple Music
static void webCmd(const json& j) { if (g_spApple) amPost(j); else wvPost(j); }  // lecteur officiel en cours

static void audioPlay(const std::string& src) {  // fichier (chemin UTF-8)
    plat::audioPlay(src);
    plat::audioRate(g_set.speed);
    g_loading = false;
}

static double posMs() {
    if (!g_has) return 0;
    if (g_sp) return std::min(g_spPos + (g_spPaused || g_loading ? 0 : (now() - g_spT) * 1000), std::max(g_spDur, (double)g_cur.ms));
    return plat::audioPos() * 1000;
}
static double durMs() {
    if (!g_has) return 0;
    if (g_sp) return g_spDur > 0 ? g_spDur : g_cur.ms;
    double d = g_loading ? 0 : plat::audioDur();
    return d > 0 ? d * 1000 : g_cur.ms;
}
static bool isPlaying() { return g_has && !g_loading && (g_sp ? !g_spPaused : !plat::audioPaused()); }

static void stopSp() {
    if (!g_sp) return;
    webCmd({{"cmd", "stop"}});
    g_sp = false;
}

static void pausePlayback() {
    if (g_sp) { g_spPos = posMs(); g_spT = now(); g_spPaused = true; webCmd({{"cmd", "pause"}}); }
    else plat::audioPause();
}

// Les prochains titres : la file d'attente d'abord, puis la suite de la liste.
static std::vector<Track> upcoming(int n) {
    std::vector<Track> out(g_queue.begin(), g_queue.begin() + std::min<size_t>(n, g_queue.size()));
    List* l = findList(g_plUri);
    for (int k = 1; l && (int)out.size() < n && k < (int)g_order.size(); k++) {
        int i = g_order[(g_pos + k) % g_order.size()];
        if (i < (int)l->tracks.size()) out.push_back(l->tracks[i]);
    }
    return out;
}

static void ytPlay(const Track& t) {
    stopSp();
    plat::audioPause();
    g_loading = true;
    if (ytdlpPath().empty()) {
        g_loading = false; g_failed = true;
        g_status = kNoYtdlp;
        return;
    }
    if (!fileExists(cachePath(t)) && g_status.empty()) g_status = "Recherche de « " + t.title + " »...";
    int gen = g_gen;
    std::thread([gen, cur = t, next = upcoming(g_set.prefetch)] {
        fs::path path = ytDownload(cur);
        {
            std::lock_guard lk(g_mx);
            if (gen != g_gen) return;
            if (path.empty()) { g_loading = false; g_failed = true; g_status = "Introuvable sur YouTube : " + cur.title; }
            else g_ready = path;
        }
        wake();
        for (auto& n : next) if (!n.local) ytDownload(n);  // préchargés pendant l'écoute
    }).detach();
}

// ---------- hors ligne : télécharge à l'avance les titres d'une playlist (audio YouTube, en cache) ----------
static std::deque<Track> g_dl;
static int g_dlDone = 0, g_dlTotal = 0;
static bool g_dlRunning = false;

static bool isCached(const Track& t) { return t.local || fileExists(cachePath(t)); }

static void downloadAll(const List& l) {
    if (ytdlpPath().empty()) { g_status = kNoYtdlp; return; }
    int n = 0;
    for (auto& t : l.tracks)
        if (!isCached(t) && std::none_of(g_dl.begin(), g_dl.end(), [&](const Track& x) { return x.uri == t.uri; })) { g_dl.push_back(t); n++; }
    g_dlTotal += n;
    g_status = n ? "Hors ligne : " + std::to_string(n) + " titres à télécharger..." : "« " + l.name + " » est déjà disponible hors ligne";
    if (g_dlRunning || !n) return;
    g_dlRunning = true;
    std::thread([] {  // un titre à la fois, en arrière-plan
        for (;;) {
            Track t;
            {
                std::lock_guard lk(g_mx);
                if (g_dl.empty()) {
                    if (g_dlTotal) g_status = "Hors ligne : " + std::to_string(g_dlDone) + " titres téléchargés";
                    g_dlRunning = false; g_dlDone = g_dlTotal = 0;
                    break;
                }
                t = g_dl.front(); g_dl.pop_front();
            }
            ytDownload(t);
            {
                std::lock_guard lk(g_mx);
                g_dlDone++;
                if (g_dlTotal) g_status = "Hors ligne : " + std::to_string(g_dlDone) + " / " + std::to_string(g_dlTotal);
            }
            wake();
        }
        wake();
    }).detach();
}

static void startTrack(const Track& t) {
    g_cur = t; g_has = true;
    g_failed = g_spEnded = false; ++g_gen;
    g_status.clear();
    addRecent(t);
    if (t.local) { stopSp(); audioPlay(t.uri); return; }
    bool spotify = t.uri.rfind("spotify:", 0) == 0, apple = t.uri.rfind("apple:song:", 0) == 0;
    bool ready = spotify ? g_sdkReady : apple && g_amReady && g_amAuth;
    if ((spotify || apple) && g_set.source != 2 && ready) {  // lecteur officiel : la vraie version (abonnement)
        plat::audioPause();
        if (g_sp && g_spApple != apple) stopSp();  // on change de lecteur
        g_sp = true; g_spApple = apple; g_loading = true; g_spPaused = false;
        g_spPos = 0; g_spDur = t.ms; g_spT = now();
        if (apple) amPost({{"cmd", "play"}, {"id", t.uri.substr(11)}});
        else {
            wvPost({{"cmd", "expect"}, {"uri", t.uri}});
            send({{"cmd", "api"}, {"id", "play"}, {"method", "PUT"}, {"path", "/v1/me/player/play?device_id=" + g_sdkDevice}, {"body", {{"uris", {t.uri}}}}});
        }
        return;
    }
    if ((spotify || apple) && g_set.source == 1) {  // officiel uniquement
        stopSp(); plat::audioPause(); g_failed = true;
        g_status = apple ? "Officiel uniquement : connecte Apple Music dans Réglages"
                 : g_webOk ? "Lecteur Spotify pas encore prêt..." : "Officiel uniquement : connecte le lecteur Spotify dans Réglages";
        return;
    }
    ytPlay(t);
}

static void playPos(int pos) {
    List* l = findList(g_plUri);
    if (!l || g_order.empty() || pos >= (int)g_order.size() || g_order[pos] >= (int)l->tracks.size()) return;
    g_pos = pos; g_tr = g_order[pos];
    startTrack(l->tracks[g_tr]);
}

// Lance la liste à partir du titre tr ; view = ordre affiché (tri/filtre) si fourni.
static void play(const std::string& uri, int tr, const std::vector<int>* view = nullptr) {
    List* l = findList(uri);
    if (!l || l->tracks.empty()) return;
    g_plUri = uri;
    if (view && !view->empty()) g_order = *view;
    else { g_order.resize(l->tracks.size()); for (int i = 0; i < (int)g_order.size(); i++) g_order[i] = i; }
    auto it = std::find(g_order.begin(), g_order.end(), tr);
    if (it == g_order.end()) { g_order.insert(g_order.begin(), tr); it = g_order.begin(); }
    if (g_shuffle) {
        std::iter_swap(g_order.begin(), it);
        std::shuffle(g_order.begin() + 1, g_order.end(), std::mt19937{std::random_device{}()});
        playPos(0);
    } else playPos((int)(it - g_order.begin()));
}

// Aléatoire pendant la lecture : mélange la suite (le titre en cours reste en tête) ou remet l'ordre de la liste.
static void toggleShuffle() {
    g_shuffle = !g_shuffle;
    if (g_order.empty()) return;
    g_pos = std::min(g_pos, (int)g_order.size() - 1);
    int cur = g_order[g_pos];
    if (g_shuffle) {
        std::iter_swap(g_order.begin(), g_order.begin() + g_pos);
        std::shuffle(g_order.begin() + 1, g_order.end(), std::mt19937{std::random_device{}()});
        g_pos = 0;
    } else {
        std::sort(g_order.begin(), g_order.end());  // shortcut: oublie un tri de colonne choisi avant, rejoue la liste pour le retrouver
        g_pos = (int)(std::find(g_order.begin(), g_order.end(), cur) - g_order.begin());
    }
}

static void step(int d) {
    if (d == 0 && g_has) { Track t = g_cur; startTrack(t); return; }
    if (d > 0 && !g_queue.empty()) { Track t = g_queue.front(); g_queue.pop_front(); g_tr = -1; startTrack(t); return; }
    List* l = findList(g_plUri);
    if (!l || g_order.empty()) return;
    int n = (int)g_order.size();
    if (d > 0 && g_pos + 1 >= n && g_set.stopAtEnd) { pausePlayback(); g_status = "Fin de la liste"; return; }
    playPos(((g_pos + d) % n + n) % n);
}

static void onTrackEnd() {
    if (g_sleepEnd) { g_sleepEnd = false; pausePlayback(); g_status = "Minuterie : lecture arrêtée"; return; }
    step(g_repeat ? 0 : 1);
}

static void togglePlay() {
    if (!g_has || g_loading) return;
    if (g_sp) { g_spPos = posMs(); g_spT = now(); g_spPaused = !g_spPaused; webCmd({{"cmd", "toggle"}}); return; }
    if (plat::audioPaused()) plat::audioResume(); else plat::audioPause();
}

static void seekMs(double ms) {
    if (g_sp) {
        g_spPos = ms; g_spT = now();
        if (g_spApple) amPost({{"cmd", "seek"}, {"ms", ms}});
        else  // seek() du SDK Spotify est ignoré une fois la lecture lancée : on passe par la Web API
            send({{"cmd", "api"}, {"method", "PUT"}, {"path", "/v1/me/player/seek?position_ms=" + std::to_string((int)ms) + "&device_id=" + g_sdkDevice}});
    }
    else if (g_has) plat::audioSeek(ms / 1000);
}

static void setVolume() {
    plat::audioVolume(g_set.vol * g_set.vol);
    json v = {{"cmd", "volume"}, {"v", std::max(g_set.vol * g_set.vol, 0.0001f)}};  // le SDK Spotify ignore 0
    if (g_wvStarted) wvPost(v);
    if (g_amStarted) amPost(v);
}

// ---------- lecteur Spotify officiel : Web Playback SDK dans une page web invisible (WebView2 / WKWebView) ----------
static const char* kPlayerHtml = R"HTML(<!doctype html><meta charset="utf-8"><title>Glass</title>
<script>
// Windows : WebView2 (chrome.webview) ; Mac : WKWebView (webkit.messageHandlers, et glassIn() pour les messages de Glass)
const wv = window.chrome && chrome.webview ? chrome.webview : {
  postMessage: m => webkit.messageHandlers.glass.postMessage(JSON.stringify(m)),
  addEventListener: (_, f) => { window.glassIn = d => f({data: d}); }
};
let player = null, waiting = [], expect = '';
const isExp = t => t && expect && (t.uri === expect || (t.linked_from && t.linked_from.uri === expect));
wv.addEventListener('message', e => {
  const m = e.data;
  if (m.token) { const w = waiting; waiting = []; w.forEach(cb => cb(m.token)); return; }
  if (m.cmd === 'expect') { expect = m.uri; return; }
  if (!player) return;
  if (m.cmd === 'toggle') player.togglePlay();
  else if (m.cmd === 'pause') player.pause();
  else if (m.cmd === 'stop') { expect = ''; player.pause(); }
  else if (m.cmd === 'seek') player.seek(m.ms);
  else if (m.cmd === 'volume') player.setVolume(m.v);
});
window.onSpotifyWebPlaybackSDKReady = () => {
  player = new Spotify.Player({ name: 'Glass', volume: Math.max(parseFloat(new URLSearchParams(location.search).get('v') ?? '0.5') || 0, 0.0001), getOAuthToken: cb => { waiting.push(cb); wv.postMessage({t: 'token'}); } });
  player.addListener('ready', ({device_id}) => wv.postMessage({t: 'ready', id: device_id}));
  player.addListener('not_ready', () => wv.postMessage({t: 'notready'}));
  const onState = s => {
    if (!s) return wv.postMessage({t: 'state', none: true});
    const c = s.track_window.current_track || {}, prev = s.track_window.previous_tracks || [];
    // fin du titre attendu : il passe dans « précédents » (arrêt à 0 ou lecture auto d'autre chose)
    if (expect && prev.some(isExp) && (!isExp(c) || (s.paused && s.position === 0))) { expect = ''; wv.postMessage({t: 'ended'}); return; }
    wv.postMessage({t: 'state', paused: s.paused, pos: s.position, dur: s.duration});
  };
  player.addListener('player_state_changed', onState);
  // le SDK reste souvent « loading » et se tait une fois lancé : on relit la position chaque seconde pendant la lecture
  setInterval(async () => { if (expect) onState(await player.getCurrentState()); }, 1000);
  for (const ev of ['initialization_error', 'authentication_error', 'account_error', 'playback_error', 'autoplay_failed'])
    player.addListener(ev, e => wv.postMessage({t: 'error', msg: ev + ' ' + ((e && e.message) || '')}));
  player.connect();
};
</script>
<script src="https://sdk.scdn.co/spotify-player.js"></script>)HTML";

// ---------- Apple Music officiel : le vrai site music.apple.com, invisible, piloté par ce script (MusicKit du site) ----------
// Même idée que Spotify : ton compte, ton abonnement, le lecteur d'Apple ; Glass ne fait qu'appuyer sur les boutons.
static const char* kAppleJs = R"JS((() => {
  if (window.top !== window || window.__glass) return;
  window.__glass = true;
  const wv = window.chrome && chrome.webview ? chrome.webview : {
    postMessage: m => webkit.messageHandlers.glass.postMessage(JSON.stringify(m)),
    addEventListener: (_, f) => { window.glassIn = d => f({data: d}); }
  };
  const post = m => wv.postMessage(m);
  let mk = null, expect = '', auth = null;
  const art = a => a && a.url ? a.url.replace('{w}', '300').replace('{h}', '300').replace('{c}', '').replace('{f}', 'jpg') : '';
  const track = s => {
    const a = s.attributes || {}, p = a.playParams || {};
    return {uri: 'apple:song:' + (p.catalogId || p.id || s.id), title: a.name || '', artist: a.artistName || '', album: a.albumName || '',
            ms: a.durationInMillis || 0, image: art(a.artwork)};
  };
  const all = async path => {  // toutes les pages d'une liste de l'API Apple Music
    let out = [];
    for (let off = 0; off < 5000; off += 100) {
      const r = await mk.api.music(path, {limit: 100, offset: off});
      const d = (r && r.data) || {};
      out = out.concat(d.data || []);
      if (!d.next) break;
    }
    return out;
  };
  wv.addEventListener('message', async e => {
    const m = e.data;
    if (!mk || !m) return;
    try {
      if (m.cmd === 'play') { expect = m.id; await mk.setQueue({song: m.id, startPlaying: true}); await mk.play(); }
      else if (m.cmd === 'toggle') { if (mk.isPlaying) mk.pause(); else mk.play(); }
      else if (m.cmd === 'pause') mk.pause();
      else if (m.cmd === 'stop') { expect = ''; mk.pause(); }
      else if (m.cmd === 'seek') mk.seekToTime(m.ms / 1000);
      else if (m.cmd === 'volume') mk.volume = m.v;
      else if (m.cmd === 'lib') {
        const pl = await all('/v1/me/library/playlists');
        post({t: 'lib', items: pl.map(p => ({id: p.id, name: (p.attributes || {}).name || 'Playlist', image: art((p.attributes || {}).artwork)}))});
      } else if (m.cmd === 'list') {
        const tr = await all('/v1/me/library/playlists/' + m.id + '/tracks');
        post({t: 'tracks', id: m.id, items: tr.map(track)});
      }
    } catch (err) { post({t: 'error', cmd: m.cmd, msg: String((err && (err.message || err.errorCode || err.name)) || err)}); }
  });
  const tick = () => {
    if (mk.isAuthorized !== auth) { auth = mk.isAuthorized; post({t: 'auth', ok: !!auth}); if (window.__glassDiag) __glassDiag(); }
    if (expect) post({t: 'state', paused: !mk.isPlaying, pos: (mk.currentPlaybackTime || 0) * 1000, dur: (mk.currentPlaybackDuration || 0) * 1000});
  };
  const find = () => {
    try { mk = window.MusicKit && MusicKit.getInstance && MusicKit.getInstance(); } catch (e) { mk = null; }
    if (!mk) return setTimeout(find, 500);
    const PS = MusicKit.PlaybackStates || {ended: 5, completed: 10};
    mk.addEventListener('playbackStateDidChange', ev => {
      const s = ev && ev.state;
      if (expect && (s === PS.ended || s === PS.completed)) { expect = ''; post({t: 'ended'}); }
    });
    auth = !!mk.isAuthorized;
    post({t: 'ready', ok: auth});
    setInterval(tick, 1000);
    const diag = async () => {
      const ks = {fairplay: 'com.apple.fps', 'fairplay 1': 'com.apple.fps.1_0', widevine: 'com.widevine.alpha', playready: 'com.microsoft.playready'};
      const ok = [];
      for (const [n, k] of Object.entries(ks)) {
        try {
          await navigator.requestMediaKeySystemAccess(k, [{initDataTypes: ['cenc', 'sinf', 'skd'], audioCapabilities: [{contentType: 'audio/mp4; codecs="mp4a.40.2"'}]}]);
          ok.push(n);
        } catch (e) {}
      }
      if (window.WebKitMediaKeys) ok.push('webkit');
      post({t: 'diag', msg: 'connecté ' + (mk.isAuthorized ? 'oui' : 'non') + ' · extraits seulement ' + (mk.previewOnly ? 'OUI' : 'non') + ' · protection ' + (ok.join(', ') || 'AUCUNE')});
    };
    window.__glassDiag = diag;
    diag();
  };
  find();
})();)JS";

// Position, pause, durée envoyées chaque seconde par un lecteur officiel (Spotify ou Apple).
static void webState(const json& j) {
    if (j.value("none", false)) { g_spPaused = true; return; }
    double pos = jnum(j, "pos"), dur = jnum(j, "dur");
    bool paused = j.value("paused", true);
    // filet de sécurité : arrêt à 0 juste après la fin du titre
    if (paused && pos == 0 && g_spDur > 0 && posMs() > g_spDur - 4000 && !g_loading) { g_spEnded = true; return; }
    g_spPaused = paused; g_spPos = pos; g_spT = now();
    if (dur > 0) g_spDur = dur;
    if ((!paused || pos > 0) && g_loading) { g_loading = false; setVolume(); }  // drapeau « loading » peu fiable ; volume réappliqué (ignoré avant la lecture)
}

static void onPlayer(const json& j) {  // messages de la page Spotify (g_mx tenu)
    std::string t = jstr(j, "t");
    bool mine = g_sp && !g_spApple;
    if (t == "token") send({{"cmd", "token"}});
    else if (t == "ready") { g_sdkDevice = jstr(j, "id"); g_sdkReady = true; setVolume(); }
    else if (t == "notready") g_sdkReady = false;
    else if (t == "error") g_status = "Lecteur Spotify : " + jstr(j, "msg");
    else if (t == "ended" && mine) g_spEnded = true;
    else if (t == "state" && mine) webState(j);
}

static void onApple(const json& j) {  // messages de la page Apple Music (g_mx tenu)
    std::string t = jstr(j, "t");
    bool mine = g_sp && g_spApple;
    if (t == "ready" || t == "auth") {
        g_amReady = true;
        bool was = g_amAuth;
        g_amAuth = j.value("ok", false);
        if (t == "ready") setVolume();
        if (g_amAuth && !was) {  // connecté : on cache la fenêtre et on charge ses playlists
            if (g_amLogin) { plat::webShow(1, false); g_amLogin = false; g_status = "Apple Music connecté"; }
            amPost({{"cmd", "lib"}});
        }
    } else if (t == "hidden") g_amLogin = false;
    else if (t == "diag") g_amDiag = jstr(j, "msg");
    else if (t == "lib") {
        for (auto& it : j["items"]) {
            std::string uri = "am:" + jstr(it, "id");
            List* l = findList(uri);
            if (!l) { g_lists.push_back(List{uri, jstr(it, "name"), jstr(it, "image")}); l = &g_lists.back(); }
            else { l->name = jstr(it, "name"); if (l->image.empty()) l->image = jstr(it, "image"); }
            applyOverride(*l);
        }
    } else if (t == "tracks") {
        if (List* l = findList("am:" + jstr(j, "id"))) {
            l->loaded = true; l->tracks.clear();
            for (auto& it : j["items"]) if (Track x = trackFrom(it); !x.uri.empty()) l->tracks.push_back(x);
            if (l->image.empty() && !l->tracks.empty()) l->image = l->tracks[0].image;
            applyOverride(*l);
        }
    } else if (t == "error") {
        g_status = "Apple Music : " + jstr(j, "msg");
        if (jstr(j, "cmd") == "play" && mine) {  // le lecteur officiel refuse ce titre
            g_sp = false; g_loading = false;
            if (g_set.source == 0 && g_has) ytPlay(g_cur); else g_failed = true;
        }
    }
    else if (t == "ended" && mine) g_spEnded = true;
    else if (t == "state" && mine) webState(j);
}

static void stopWebView() { plat::webStop(0); g_wvStarted = g_sdkReady = false; if (!g_spApple) g_sp = false; }
static void startWebView() { g_wvStarted = true; plat::webStart(0, kPlayerHtml, "", "", g_set.vol * g_set.vol); }  // thread de la fenêtre
static void stopApple() {
    plat::webStop(1);
    g_amStarted = g_amReady = g_amAuth = g_amLogin = false;
    if (g_spApple) g_sp = false;
    g_lists.erase(std::remove_if(g_lists.begin(), g_lists.end(), [](const List& l) { return l.uri.rfind("am:", 0) == 0; }), g_lists.end());
}
static void startApple() {
    g_amStarted = true;
    plat::webStart(1, "", "https://music.apple.com/", kAppleJs, g_set.vol * g_set.vol);
    if (g_amLogin) plat::webShow(1, true);
}

// ---------- images (pochettes, fond flouté) ----------
// Couleurs d'une pochette (calculées sur sa version floutée) : 4 coins + la plus vive.
struct Palette { ImVec4 c[4], vivid; };
struct TexJob { std::string key, src; int size; bool blur; };
struct TexReady { std::string key, src; std::vector<uint8_t> px; int w = 0, h = 0; bool blur = false; Palette pal{}; };
static std::deque<TexJob> g_jobs;
static std::vector<TexReady> g_texReady;  // décodées par les threads, envoyées à la carte graphique par la fenêtre
static std::condition_variable g_jobCv;
static std::map<std::string, ImTextureID> g_tex;
static std::map<std::string, ImVec2> g_texDim;  // taille en pixels de chaque texture chargée
static std::map<std::string, Palette> g_pal;

static void boxBlur(std::vector<uint8_t>& p, int w, int h, int r) {
    std::vector<uint8_t> t(p.size());
    for (int pass = 0; pass < 6; pass++, p.swap(t)) {  // 3 passes horizontales + 3 verticales ≈ flou gaussien
        bool hz = pass % 2 == 0;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < 4; c++) {
            int s = 0;
            for (int k = -r; k <= r; k++) s += hz ? p[(y * w + std::clamp(x + k, 0, w - 1)) * 4 + c] : p[(std::clamp(y + k, 0, h - 1) * w + x) * 4 + c];
            t[(y * w + x) * 4 + c] = (uint8_t)(s / (2 * r + 1));
        }
    }
}

static Palette palette(const std::vector<uint8_t>& px, int tw, int th) {
    Palette pal{};
    float best = -1;
    for (int q = 0; q < 4; q++) {  // moyenne de chaque quart de l'image
        float r = 0, g = 0, b = 0; int n = 0;
        for (int y = (q / 2) * th / 2; y < (q / 2 + 1) * th / 2; y++) for (int x = (q % 2) * tw / 2; x < (q % 2 + 1) * tw / 2; x++, n++) {
            r += px[(y * tw + x) * 4]; g += px[(y * tw + x) * 4 + 1]; b += px[(y * tw + x) * 4 + 2];
        }
        n = std::max(n, 1);
        pal.c[q] = {r / n / 255, g / n / 255, b / n / 255, 1};
        float mx = std::max({pal.c[q].x, pal.c[q].y, pal.c[q].z}), mn = std::min({pal.c[q].x, pal.c[q].y, pal.c[q].z});
        if (mx - mn > best) { best = mx - mn; pal.vivid = pal.c[q]; }
    }
    // couleur d'accent lisible : on pousse la saturation et la luminosité
    ImVec4& v = pal.vivid;
    float mx = std::max({v.x, v.y, v.z, 0.01f}), mn = std::min({v.x, v.y, v.z});
    auto boost = [&](float c) { return std::clamp((c - mn) / std::max(mx - mn, 0.05f) * 0.75f + 0.25f, 0.f, 1.f); };
    v = {boost(v.x), boost(v.y), boost(v.z), 1};
    return pal;
}

static void texWorker() {
    for (;;) {
        TexJob j;
        {
            std::unique_lock lk(g_mx);
            g_jobCv.wait(lk, [] { return !g_jobs.empty(); });
            j = g_jobs.back(); g_jobs.pop_back();  // le plus récent d'abord = ce qui est à l'écran
        }
        TexReady r{j.key, j.src};
        r.blur = j.blur;
        if (!plat::decodeImage(j.src, j.size, r.px, r.w, r.h)) r.px.clear();
        else if (j.blur) {
            boxBlur(r.px, r.w, r.h, std::max(2, std::max(r.w, r.h) / 14));
            r.pal = palette(r.px, r.w, r.h);
        }
        { std::lock_guard lk(g_mx); g_texReady.push_back(std::move(r)); }
        wake();
    }
}

static void uploadTextures() {  // g_mx tenu, thread de la fenêtre
    for (auto& r : g_texReady) {
        if (r.px.empty()) continue;  // échec : reste à 0 (pochette par défaut)
        g_tex[r.key] = plat::makeTexture(r.px.data(), r.w, r.h);
        g_texDim[r.key] = {(float)r.w, (float)r.h};
        if (r.blur) g_pal[r.src] = r.pal;
    }
    g_texReady.clear();
}

static ImTextureID tex(const std::string& src, int size, bool blur = false) {  // g_mx tenu ; 0 tant que pas chargé
    if (src.empty()) return 0;
    std::string key = src + "|" + std::to_string(size) + (blur ? "b" : "");
    auto it = g_tex.find(key);
    if (it != g_tex.end()) return it->second;
    g_tex[key] = 0;
    g_jobs.push_back({key, src, size, blur});
    g_jobCv.notify_one();
    return 0;
}

// ---------- UI ----------
static ImVec4 g_accLive{0.12f, 0.84f, 0.38f, 1};  // couleur d'accent affichée (glisse vers la cible)
static ImVec4 acc() { return g_accLive; }
static ImU32 accU() { return ImGui::GetColorU32(acc()); }
static ImFont *g_bold, *g_icons;
#ifdef _WIN32  // icônes Segoe MDL2 Assets
static const char *I_PLAY = u8"\uE768", *I_PAUSE = u8"\uE769", *I_PREV = u8"\uE892", *I_NEXT = u8"\uE893",
                  *I_SHUFFLE = u8"\uE8B1", *I_REPEAT1 = u8"\uE8ED", *I_VOL = u8"\uE767", *I_ADD = u8"\uE710",
                  *I_NOTE = u8"\uEC4F", *I_HEART = u8"\uEB52", *I_HEART_O = u8"\uEB51", *I_FOLDER = u8"\uE8D6", *I_MIN = u8"\uE921",
                  *I_MAX = u8"\uE922", *I_RESTORE = u8"\uE923", *I_CLOSE = u8"\uE8BB", *I_GEAR = u8"\uE713",
                  *I_LYRICS = u8"\uE720", *I_QUEUE = u8"\uE8FD", *I_MORE = u8"\uE712", *I_EDIT = u8"\uE70F",
                  *I_MINI = u8"\uE73F", *I_EXPAND = u8"\uE740", *I_SEARCH = u8"\uE721", *I_HISTORY = u8"\uE81C", *I_LIST = u8"\uE8A4", *I_DOWNLOAD = u8"\uE896";
#else  // icônes Material Icons (fournies avec Glass.app)
static const char *I_PLAY = u8"\uE037", *I_PAUSE = u8"\uE034", *I_PREV = u8"\uE045", *I_NEXT = u8"\uE044",
                  *I_SHUFFLE = u8"\uE043", *I_REPEAT1 = u8"\uE041", *I_VOL = u8"\uE050", *I_ADD = u8"\uE145",
                  *I_NOTE = u8"\uE405", *I_HEART = u8"\uE87D", *I_HEART_O = u8"\uE87E", *I_FOLDER = u8"\uE2C7", *I_MIN = u8"\uE931",
                  *I_MAX = u8"\uE3C6", *I_RESTORE = u8"\uE3E0", *I_CLOSE = u8"\uE5CD", *I_GEAR = u8"\uE8B8",
                  *I_LYRICS = u8"\uE029", *I_QUEUE = u8"\uE03D", *I_MORE = u8"\uE5D3", *I_EDIT = u8"\uE3C9",
                  *I_MINI = u8"\uE911", *I_EXPAND = u8"\uF1CE", *I_SEARCH = u8"\uE8B6", *I_HISTORY = u8"\uE889", *I_LIST = u8"\uE896", *I_DOWNLOAD = u8"\uF090";
#endif

static ImU32 col(float r, float g, float b, float a = 1) { return ImGui::GetColorU32(ImVec4{r, g, b, a}); }

static std::string fmtTime(double ms) {
    int s = ms > 0 ? (int)(ms / 1000) : 0;
    char b[16]; snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}

// Texte coupé avec "…" s'il dépasse maxW.
static void textAt(ImDrawList* dl, ImVec2 p, float maxW, ImU32 c, const std::string& s, ImFont* f = nullptr, float size = 0) {
    if (!f) f = ImGui::GetFont();
    size = size ? size * S * g_set.textScale : ImGui::GetFontSize();
    const char *b = s.c_str(), *e = b + s.size();
    if (f->CalcTextSizeA(size, FLT_MAX, 0, b, e).x <= maxW) { dl->AddText(f, size, p, c, b, e); return; }
    float ell = f->CalcTextSizeA(size, FLT_MAX, 0, "…").x;
    size_t lo = 0, hi = s.size();  // recherche par dichotomie de la plus longue partie qui tient (appelé à chaque image)
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (f->CalcTextSizeA(size, FLT_MAX, 0, b, b + mid).x + ell <= maxW) lo = mid; else hi = mid - 1;
    }
    while (lo > 0 && lo < s.size() && (s[lo] & 0xC0) == 0x80) lo--;  // pas au milieu d'un caractère UTF-8
    std::string t(b, b + lo); t += "…";
    dl->AddText(f, size, p, c, t.c_str());
}

static void iconAt(ImDrawList* dl, ImVec2 center, const char* icon, float size, ImU32 c) {
    ImVec2 ts = g_icons->CalcTextSizeA(size, FLT_MAX, 0, icon);
    dl->AddText(g_icons, size, center - ts * 0.5f, c, icon);
}

static void coverAt(ImDrawList* dl, const std::string& img, const std::string& uri, ImVec2 p, float sz, float round) {
    round = std::min(round, g_set.rounding * S * 0.7f);
    if (ImTextureID t = tex(img, sz > 120 * S ? 300 : 96)) { dl->AddImageRounded(t, p, p + ImVec2{sz, sz}, {0, 0}, {1, 1}, IM_COL32_WHITE, round); return; }
    bool liked = uri == "spotify:collection" || uri == "glass:favoris", local = uri == "local", recent = uri == "glass:recent", own = uri.rfind("glass:", 0) == 0;
    auto shade = [](float dh, float v) {  // teinte du thème, décalée pour distinguer les listes
        float h, s, b, r, g, bl;
        ImGui::ColorConvertRGBtoHSV(acc().x, acc().y, acc().z, h, s, b);
        ImGui::ColorConvertHSVtoRGB(fmodf(h + dh + 1, 1), s, v, r, g, bl);
        return col(r, g, bl);
    };
    ImU32 a = liked ? shade(0, 0.8f) : local ? shade(0.08f, 0.65f) : recent ? shade(-0.08f, 0.65f) : own ? shade(0, 0.5f) : col(1, 1, 1, 0.08f);
    dl->AddRectFilled(p, p + ImVec2{sz, sz}, a, round);
    iconAt(dl, p + ImVec2{sz, sz} * 0.5f, liked ? I_HEART : local ? I_FOLDER : recent ? I_HISTORY : own ? I_LIST : I_NOTE, sz * 0.4f, col(1, 1, 1, 0.9f));
}

// Valeur animée vers une cible, gardée par ImGui pour chaque élément ; demande des images tant qu'elle bouge.
static void glow(ImDrawList* dl, ImVec2 c, float r, ImVec4 col, float alpha);
static float animTo(ImGuiID id, float target, float speed = 14) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    float v = st->GetFloat(id, target);
    if (!g_set.animations || std::fabs(target - v) < 0.01f) v = target;
    else { v += (target - v) * std::min(1.f, ImGui::GetIO().DeltaTime * speed); g_anim = true; }
    st->SetFloat(id, v);
    return v;
}

static bool iconBtn(const char* id, const char* icon, float size, bool active = false) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool r = ImGui::InvisibleButton("##b", {size, size});
    bool hov = ImGui::IsItemHovered();
    float hv = animTo(ImGui::GetID("##hv"), hov ? 1.f : 0.f), pr = animTo(ImGui::GetID("##pr"), ImGui::IsItemActive() ? 0.85f : 1.f, 20);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c = p + ImVec2{size, size} * 0.5f;
    if (hv > 0) dl->AddCircleFilled(c, size * 0.5f * (0.7f + 0.3f * hv), col(1, 1, 1, 0.09f * hv));
    if (active && g_set.neon > 0) glow(dl, c, size * 0.55f, acc(), 0.25f * g_set.neon);
    iconAt(dl, c, icon, size * 0.42f * pr, active ? accU() : col(1, 1, 1, 0.72f + 0.28f * hv));
    if (active) dl->AddCircleFilled(p + ImVec2{size * 0.5f, size - 2 * S}, 2 * S, accU());
    ImGui::PopID();
    return r;
}

static bool playBtn(const char* id, float size, bool playing, ImU32 bg, ImU32 fg) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos(), c = p + ImVec2{size, size} * 0.5f;
    bool r = ImGui::InvisibleButton("##p", {size, size});
    float k = animTo(ImGui::GetID("##k"), ImGui::IsItemActive() ? 0.92f : ImGui::IsItemHovered() ? 1.06f : 1.f, 18);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (g_set.neon > 0) glow(dl, c, size * 0.95f * k, acc(), 0.35f * g_set.neon);  // halo néon
    dl->AddCircleFilled(c, size * 0.5f * k, bg);
    iconAt(dl, c + ImVec2{playing ? 0 : size * 0.03f, 0}, playing ? I_PAUSE : I_PLAY, size * 0.38f, fg);
    ImGui::PopID();
    return r;
}

// Barre fine façon Spotify/Apple ; renvoie true quand on relâche.
static bool scrub(const char* id, float* v, float maxv, float w, float wheelStep = 0) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = 16 * S;
    ImGui::InvisibleButton(id, {w, h});
    bool hov = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (ImGui::IsItemActive()) *v = std::clamp((ImGui::GetIO().MousePos.x - p.x) / w, 0.f, 1.f) * maxv;
    float wheel = ImGui::IsItemHovered() ? ImGui::GetIO().MouseWheel : 0;
    if (wheelStep > 0 && wheel != 0) { *v = std::clamp(*v + wheel * wheelStep * maxv, 0.f, maxv); return true; }
    float f = maxv > 0 ? std::clamp(*v / maxv, 0.f, 1.f) : 0, y = p.y + h / 2, th = (hov ? 6 : 4) * S;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled({p.x, y - th / 2}, {p.x + w, y + th / 2}, col(1, 1, 1, 0.18f), th);
    if (g_set.neon > 0 && f > 0) for (int i = 3; i >= 1; i--) dl->AddRectFilled({p.x, y - th / 2 - i * 2 * S}, {p.x + w * f, y + th / 2 + i * 2 * S}, ImGui::GetColorU32(ImVec4{acc().x, acc().y, acc().z, 0.08f * g_set.neon}), th + i * 2 * S);
    dl->AddRectFilled({p.x, y - th / 2}, {p.x + w * f, y + th / 2}, hov || g_set.neon > 0 ? accU() : col(1, 1, 1, 0.92f), th);
    if (hov) dl->AddCircleFilled({p.x + w * f, y}, 7 * S, IM_COL32_WHITE);
    return ImGui::IsItemDeactivated();
}

static ImVec4 scale(ImVec4 c, float k) { return {c.x * k, c.y * k, c.z * k, c.w}; }

// Halo doux : disques concentriques presque transparents (pas de shader, coût négligeable).
static ImTextureID g_glowTex;  // dégradé radial blanc → transparent, créé une fois : halos lisses en un seul rectangle

static void makeGlowTexture() {
    const int N = 128;
    std::vector<uint8_t> px(N * N * 4, 255);
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        float d = std::min(1.f, std::hypot(x - N / 2 + 0.5f, y - N / 2 + 0.5f) / (N / 2));
        float a = (1 - d) * (1 - d) * (1 - d);  // décroissance douce, sans bord
        px[(y * N + x) * 4 + 3] = (uint8_t)(a * 255);
    }
    g_glowTex = plat::makeTexture(px.data(), N, N);
}

static void glow(ImDrawList* dl, ImVec2 c, float r, ImVec4 col, float alpha) {
    if (g_glowTex) dl->AddImage(g_glowTex, c - ImVec2{r, r}, c + ImVec2{r, r}, {0, 0}, {1, 1}, ImGui::GetColorU32(ImVec4{col.x, col.y, col.z, std::min(1.f, alpha)}));
}

// Contour néon : traits de plus en plus larges et transparents autour d'un rectangle arrondi.
static void neonRect(ImDrawList* dl, ImVec2 a, ImVec2 b, float round, float amt) {
    if (amt <= 0) return;
    ImVec4 c = acc();
    for (int i = 6; i >= 1; i--) {
        float e = i * 2.2f * S;
        dl->AddRect(a - ImVec2{e, e}, b + ImVec2{e, e}, ImGui::GetColorU32(ImVec4{c.x, c.y, c.z, amt * 0.045f * (7 - i)}), round + e, 0, 2.5f * S);
    }
    dl->AddRect(a, b, ImGui::GetColorU32(ImVec4{c.x, c.y, c.z, 0.55f * amt + 0.15f}), round, 0, 1.2f * S);
}

// Couleur d'accent : celle des réglages, ou la plus vive de la pochette (glisse en douceur).
static void updateAccent() {
    ImVec4 target{g_set.accent[0], g_set.accent[1], g_set.accent[2], 1};
    if (g_set.accentAuto && g_has) {
        tex(g_cur.image, 64, true);
        if (auto it = g_pal.find(g_cur.image); it != g_pal.end()) target = it->second.vivid;
    }
    ImVec4& c = g_accLive;
    float k = g_set.animations ? std::min(1.f, ImGui::GetIO().DeltaTime * 5) : 1.f, d = std::fabs(c.x - target.x) + std::fabs(c.y - target.y) + std::fabs(c.z - target.z);
    if (d < 0.003f) c = target;
    else { c = {c.x + (target.x - c.x) * k, c.y + (target.y - c.y) * k, c.z + (target.z - c.z) * k, 1}; g_anim = true; }
    ImVec4* sc = ImGui::GetStyle().Colors;
    sc[ImGuiCol_SliderGrab] = sc[ImGuiCol_CheckMark] = sc[ImGuiCol_DragDropTarget] = c;
    sc[ImGuiCol_TextSelectedBg] = ImVec4{c.x, c.y, c.z, 0.35f};
}

struct Particle { float x, y, speed, sway, phase, rot, spin, size; };

static void petal(ImDrawList* dl, ImVec2 c, float s, float rot, float flip, ImU32 col) {
    static const ImVec2 shape[] = {{0, 0.95f}, {0.5f, 0.25f}, {0.48f, -0.5f}, {0.18f, -0.85f}, {0, -0.62f}, {-0.18f, -0.85f}, {-0.48f, -0.5f}, {-0.5f, 0.25f}};
    ImVec2 pts[8];
    float cs = cosf(rot), sn = sinf(rot);
    for (int i = 0; i < 8; i++) {
        float x = shape[i].x * s * flip, y = shape[i].y * s;  // flip : le pétale tourne sur lui-même
        pts[i] = {c.x + x * cs - y * sn, c.y + x * sn + y * cs};
    }
    dl->AddConvexPolyFilled(pts, 8, col);
}

static void pumpkin(ImDrawList* dl, ImVec2 c, float r, float rot) {  // citrouille : 3 côtes, tige, visage
    float cs = cosf(rot), sn = sinf(rot);
    auto at = [&](float x, float y) { return ImVec2{c.x + (x * cs - y * sn) * r, c.y + (x * sn + y * cs) * r}; };
    ImU32 side = col(0.82f, 0.36f, 0.04f, 0.92f), face = col(0.22f, 0.07f, 0.f, 0.95f);
    dl->AddEllipseFilled(at(-0.38f, 0), {0.62f * r, 0.78f * r}, side, rot);
    dl->AddEllipseFilled(at(0.38f, 0), {0.62f * r, 0.78f * r}, side, rot);
    dl->AddEllipseFilled(at(0, 0), {0.58f * r, 0.84f * r}, col(1.f, 0.52f, 0.08f, 0.95f), rot);
    dl->AddQuadFilled(at(-0.1f, -0.72f), at(0.1f, -0.72f), at(0.18f, -1.1f), at(-0.02f, -1.12f), col(0.32f, 0.56f, 0.16f, 0.95f));
    dl->AddTriangleFilled(at(-0.44f, -0.06f), at(-0.12f, -0.06f), at(-0.28f, -0.34f), face);
    dl->AddTriangleFilled(at(0.12f, -0.06f), at(0.44f, -0.06f), at(0.28f, -0.34f), face);
    dl->AddQuadFilled(at(-0.46f, 0.18f), at(0.46f, 0.18f), at(0.26f, 0.5f), at(-0.26f, 0.5f), face);
}

static void drawParticles(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    if (!g_set.particles) return;
    static std::vector<Particle> ps;
    static std::mt19937 rng{7};
    static double last = ImGui::GetTime();
    auto rnd = [](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };
    size_t want = (g_mini ? 12 : 34) / (g_set.particles == 4 ? 2 : 1);  // citrouilles : plus grosses, moins nombreuses
    while (ps.size() < want) ps.push_back({rnd(0, 1), rnd(-0.1f, 1), rnd(0.035f, 0.08f), rnd(0.01f, 0.05f), rnd(0, 6.28f), rnd(0, 6.28f), rnd(-1.4f, 1.4f), rnd(5, 9.5f)});
    ps.resize(want);
    double t = ImGui::GetTime();
    float dt = (float)std::min(0.1, t - last), W = b.x - a.x, H = b.y - a.y;
    last = t;
    for (auto& p : ps) {
        p.phase += dt; p.rot += p.spin * dt;
        if (g_set.particles == 3) p.y -= p.speed * 0.25f * dt; else p.y += p.speed * dt;  // les lucioles montent doucement
        if (p.y > 1.05f) { p.y = -0.05f; p.x = rnd(0, 1); }
        if (p.y < -0.05f) { p.y = 1.05f; p.x = rnd(0, 1); }
        ImVec2 c{a.x + (p.x + sinf(p.phase * 0.8f) * p.sway) * W, a.y + p.y * H};
        float sz = p.size * S * (g_mini ? 0.8f : 1.f);
        if (g_set.particles == 1) petal(dl, c, sz, p.rot, cosf(p.phase * 1.7f), col(1.f, 0.74f, 0.84f, 0.8f));
        else if (g_set.particles == 2) dl->AddCircleFilled(c, sz * 0.32f, col(1, 1, 1, 0.7f));
        else if (g_set.particles == 4) { glow(dl, c, sz * 3.f, ImVec4{1.f, 0.5f, 0.05f, 1}, 0.25f); pumpkin(dl, c, sz * 1.5f, sinf(p.phase) * 0.35f); }
        else {
            float pulse = 0.5f + 0.5f * sinf(p.phase * 2.3f);
            glow(dl, c, sz * 2.4f, acc(), 0.55f * pulse);
            dl->AddCircleFilled(c, sz * 0.18f, col(1, 1, 0.85f, 0.9f * pulse));
        }
    }
    g_animBg = true;
}

static void drawBackground(ImGuiViewport* vp) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 a = vp->Pos, b = vp->Pos + vp->Size;
    ImVec4 tint{g_set.tint[0], g_set.tint[1], g_set.tint[2], 1};
    int bg = g_set.bg;
    g_animBg = false;
    if (bg == 2) {  // fond uni : dégradé de la teinte du thème
        ImU32 top = ImGui::GetColorU32(tint), bot = ImGui::GetColorU32(scale(tint, 0.4f));
        dl->AddRectFilledMultiColor(a, b, top, top, bot, bot);
        drawParticles(dl, a, b);
        return;
    }
    dl->AddRectFilled(a, b, ImGui::GetColorU32(tint));
    Track* t = current();
    if (bg >= 3) {  // 3 dégradé de la pochette (façon Apple Music), 4 néon, 5 aurore animée
        float W = b.x - a.x, H = b.y - a.y, R = std::max(W, H);
        ImVec4 ac = acc(), c[4] = {ac, {0.10f, 0.85f, 1.00f, 1}, {0.55f, 0.25f, 1.00f, 1}, scale(ac, 0.7f)};  // accent + cyan + violet néon
        if (bg == 3 && t) {
            tex(t->image, 64, true);
            if (auto it = g_pal.find(t->image); it != g_pal.end()) std::copy(it->second.c, it->second.c + 4, c);
        }
        float tm = 0;
        if (bg == 5 && g_set.animations) {
            tm = (float)ImGui::GetTime() * 3;
            g_animBg = true;
            for (auto& ci : c) {  // les couleurs tournent lentement sur le cercle chromatique
                float h, sat, v;
                ImGui::ColorConvertRGBtoHSV(ci.x, ci.y, ci.z, h, sat, v);
                ImGui::ColorConvertHSVtoRGB(fmodf(h + tm * 0.01f, 1.f), sat, v, ci.x, ci.y, ci.z);
            }
        }
        float amp = bg == 5 ? 1.9f : 1.f;
        ImVec2 p[4] = {
            {a.x + W * (0.15f + amp * 0.12f * sinf(tm * 0.21f)), a.y + H * (0.20f + amp * 0.12f * cosf(tm * 0.17f))},
            {a.x + W * (0.85f + amp * 0.10f * cosf(tm * 0.13f)), a.y + H * (0.15f + amp * 0.14f * sinf(tm * 0.19f))},
            {a.x + W * (0.20f + amp * 0.14f * cosf(tm * 0.11f)), a.y + H * (0.85f + amp * 0.10f * sinf(tm * 0.23f))},
            {a.x + W * (0.80f + amp * 0.12f * sinf(tm * 0.15f)), a.y + H * (0.80f + amp * 0.12f * cosf(tm * 0.12f))}};
        if (bg == 3) dl->AddRectFilledMultiColor(a, b, ImGui::GetColorU32(scale(c[0], 0.55f)), ImGui::GetColorU32(scale(c[1], 0.55f)),
                                                 ImGui::GetColorU32(scale(c[3], 0.55f)), ImGui::GetColorU32(scale(c[2], 0.55f)));
        for (int i = 0; i < 4; i++) glow(dl, p[i], R * (bg == 3 ? 0.6f : 0.42f), c[i], bg == 3 ? 0.9f : 0.6f);
    } else {
        static std::string cur, prev;
        static double fadeT = 0;
        std::string img = t && bg == 0 ? t->image : "";
        if (img != cur && (img.empty() || tex(img, 64, true))) { prev = cur; cur = img; fadeT = ImGui::GetTime(); }  // change quand la nouvelle est prête
        float fade = g_set.animations ? (float)std::min(1.0, (ImGui::GetTime() - fadeT) / 0.8) : 1.f;
        if (fade < 1) g_anim = true;
        auto draw = [&](const std::string& s, float alpha) {
            ImU32 tc = col(1, 1, 1, alpha);
            if (ImTextureID tx = s.empty() ? 0 : tex(s, 64, true)) {
                float W = b.x - a.x, H = b.y - a.y, u = W > H ? 0 : (1 - W / H) / 2, v = W > H ? (1 - H / W) / 2 : 0;
                dl->AddImage(tx, a, b, {u, v}, {1 - u, 1 - v}, tc);
            } else if (ImTextureID wt = tex(g_wall, 160, true)) {  // pas de pochette : fond d'écran flouté, aligné sur l'écran = verre dépoli
                ImVec2 uv0{0, 0}, uv1{1, 1};
                plat::wallpaperUV(uv0, uv1);
                dl->AddImage(wt, a, b, uv0, uv1, tc);
            }
        };
        draw(prev, 1);
        draw(cur, fade);
    }
    if (g_set.tintAmt > 0) dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4{tint.x, tint.y, tint.z, g_set.tintAmt}));  // teinte du thème
    float d = g_set.dim;
    dl->AddRectFilledMultiColor(a, b, col(0, 0, 0, d * 0.65f), col(0, 0, 0, d * 0.65f), col(0, 0, 0, std::min(1.f, d * 1.35f)), col(0, 0, 0, std::min(1.f, d * 1.35f)));
    drawParticles(dl, a, b);
}

static void setTopmost() { plat::setTopmost(g_set.topmost || g_mini); }
static bool g_miniLyrics = false;
static int miniHeight() { return (int)((g_miniLyrics ? 262 : 146) * S); }

static void toggleMini() {
    g_mini = !g_mini;
    if (g_mini) plat::enterMini((int)(480 * S), miniHeight());  // en bas à droite de l'écran
    else plat::leaveMini();                                      // taille d'avant, même si la fenêtre était agrandie
    setTopmost();
}

static void toggleMiniLyrics() {  // agrandit le mini-lecteur vers le haut pour afficher les paroles
    g_miniLyrics = !g_miniLyrics;
    plat::setMiniHeight(miniHeight());
}

// ---------- logo (choisi dans les réglages) ----------
static std::string logoSrc() { return g_set.logo.empty() ? std::string("icon:") : g_set.logo; }

static void applyLogo() {  // icône de la fenêtre / barre des tâches / Dock
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    if (g_set.logo.empty() || !plat::decodeImage(g_set.logo, 256, px, w, h)) { plat::setAppIcon(nullptr, 0, 0); return; }
    int n = std::min(w, h), x0 = (w - n) / 2, y0 = (h - n) / 2;  // carré, centré
    std::vector<uint8_t> sq((size_t)n * n * 4);
    for (int y = 0; y < n; y++) std::copy_n(&px[((size_t)(y + y0) * w + x0) * 4], n * 4, &sq[(size_t)y * n * 4]);
    plat::setAppIcon(sq.data(), n, n);
}

// ---------- mise à jour automatique (versions publiées sur GitHub) ----------
struct Update { int version = 0; std::string url, notes; bool busy = false; };
static std::string g_newsText;  // « Nouveautés » à afficher (fenêtre)
static int g_newsVersion = 0;
static bool g_openNews = false, g_openUpdate = false;

static std::string cleanNotes(const std::string& body) {  // texte de la version GitHub, sans les lignes techniques
    std::istringstream in(body);
    std::string out;
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.find("Co-Authored-By") != std::string::npos) continue;
        out += l + "\n";
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

static void showNews(int version) {  // g_mx tenu : nouveautés d'une version publiée
    if (version <= 0) return;
    std::thread([version] {
        json r = json::parse(plat::httpGet(std::string("https://api.github.com/repos/") + kRepo + "/releases/tags/v" + std::to_string(version)), nullptr, false);
        std::string notes = r.is_object() ? cleanNotes(jstr(r, "body")) : "";
        std::lock_guard lk(g_mx);
        g_newsText = notes;
        g_newsVersion = version; g_openNews = true;
        wake();
    }).detach();
}
static Update g_update;
static ImVec4 g_updRect;      // bouton « Mise à jour disponible » (la barre de titre ne doit pas le prendre pour une poignée)
static double g_updT = -1e9;  // dernière vérification

static std::string g_updMsg;  // résultat de « Rechercher une mise à jour »

static void checkUpdate(bool manual = false) {  // g_mx tenu
    if (GLASS_VERSION <= 0) { if (manual) g_updMsg = "Version compilée sur ce PC : prends Glass sur la page des versions GitHub pour avoir les mises à jour."; return; }
    if (manual) g_updMsg = "Recherche...";
    std::thread([manual] {
        json r = json::parse(plat::httpGet(std::string("https://api.github.com/repos/") + kRepo + "/releases/latest"), nullptr, false);
        int v = atoi(jstr(r, "tag_name").c_str() + 1);  // "v12" -> 12
#ifdef _WIN32
        const char* want = "Glass-windows.zip";
#else
        const char* want = "Glass-mac.zip";
#endif
        std::lock_guard lk(g_mx);
        if (v > GLASS_VERSION && r.is_object() && r["assets"].is_array())
            for (auto& a : r["assets"]) if (jstr(a, "name") == want) { g_update.version = v; g_update.url = jstr(a, "browser_download_url"); g_update.notes = cleanNotes(jstr(r, "body")); }
        if (manual) {
            g_updMsg = v <= 0 ? "Impossible de vérifier (pas d'internet ?)"
                     : g_update.version > GLASS_VERSION ? "Version " + std::to_string(g_update.version) + " disponible !"
                     : "Tu as la dernière version (" + std::to_string(GLASS_VERSION) + ")";
            g_status = g_updMsg;
        }
        wake();
    }).detach();
}

static void installUpdate() {  // g_mx tenu
    if (g_update.busy || g_update.url.empty()) return;
    g_update.busy = true;
    std::thread([url = g_update.url] {
        fs::path zip = g_dir / "update.zip";
        std::string err = httpDownload(url, zip, "Mise à jour") ? plat::installUpdate(zip) : "téléchargement impossible";
        std::lock_guard lk(g_mx);
        g_update.busy = false;
        g_status = err.empty() ? "Mise à jour installée : Glass redémarre..." : "Mise à jour : " + err;
        wake();
    }).detach();
}

static void titleBar(ImGuiViewport* vp, float tb) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
#ifdef __APPLE__
    ImVec2 o = vp->Pos + ImVec2{70 * S, 0};
    const int nb = 1;
#else
    ImVec2 o = vp->Pos;
    const int nb = 4;
#endif
    std::string ls = logoSrc();
    if (ImTextureID logo = tex(ls, 64)) {
        ImVec2 d = g_texDim[ls + "|64"], uv0{0, 0}, uv1{1, 1};  // image pas carrée : on garde le centre
        if (d.x > d.y && d.x > 0) { uv0.x = (1 - d.y / d.x) / 2; uv1.x = 1 - uv0.x; }
        if (d.y > d.x && d.y > 0) { uv0.y = (1 - d.x / d.y) / 2; uv1.y = 1 - uv0.y; }
        dl->AddImageRounded(logo, o + ImVec2{14 * S, tb * 0.5f - 10 * S}, o + ImVec2{34 * S, tb * 0.5f + 10 * S}, uv0, uv1, IM_COL32_WHITE, 5 * S);
    } else dl->AddCircleFilled(o + ImVec2{24 * S, tb * 0.5f}, 7 * S, accU());
    dl->AddText(g_bold, 15 * S, o + ImVec2{38 * S, tb * 0.5f - 10 * S}, col(1, 1, 1, 0.95f), "Glass");
    o = vp->Pos;
    if (!g_status.empty()) {
        ImVec2 ts = ImGui::CalcTextSize(g_status.c_str());
        textAt(dl, o + ImVec2{std::max(120 * S, (vp->Size.x - ts.x) * 0.5f), tb * 0.5f - 9 * S}, vp->Size.x * 0.5f, col(1, 1, 1, 0.6f), g_status);
    }
    float bw = 46 * S;
    g_updRect = {};
    if (g_update.version > GLASS_VERSION) {  // une nouvelle version est publiée : un clic l'installe
        const char* txt = g_update.busy ? "Updating..." : "Update available";
        float w = ImGui::CalcTextSize(txt).x + 28 * S, h = 26 * S;
        ImVec2 p = o + ImVec2{vp->Size.x - bw * nb - w - 12 * S, (tb - h) * 0.5f};
        ImGui::SetCursorScreenPos(p);
        if (ImGui::InvisibleButton("##upd", {w, h}) && !g_update.busy) g_openUpdate = true;
        bool hov = ImGui::IsItemHovered();
        ImGui::SetItemTooltip("Glass %d is out (you have %d) - click to see what's new", g_update.version, GLASS_VERSION);
        if (g_set.neon > 0) glow(dl, p + ImVec2{w, h} * 0.5f, w * 0.6f, acc(), 0.3f * g_set.neon);
        dl->AddRectFilled(p, p + ImVec2{w, h}, ImGui::GetColorU32(ImVec4{acc().x, acc().y, acc().z, hov ? 1.f : 0.8f}), h * 0.5f);
        dl->AddText(p + ImVec2{14 * S, (h - ImGui::GetFontSize()) * 0.5f}, col(0, 0, 0, 0.85f), txt);
        g_updRect = {p.x - o.x, p.y - o.y, w, h};
    }
    const char* icons[4] = {I_MINI, I_MIN, plat::maximized() ? I_RESTORE : I_MAX, I_CLOSE};
    for (int i = 0; i < nb; i++) {
        ImGui::SetCursorPos({vp->Size.x - bw * (nb - i), 0});
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool click = ImGui::InvisibleButton("##w", {bw, tb - 6 * S});
        if (ImGui::IsItemHovered()) dl->AddRectFilled(p, p + ImVec2{bw, tb - 6 * S}, i == 3 ? col(0.9f, 0.15f, 0.15f) : col(1, 1, 1, 0.08f));
        if (i == 0) ImGui::SetItemTooltip("Mini-lecteur");
        iconAt(dl, p + ImVec2{bw, tb - 6 * S} * 0.5f, icons[i], (i == 0 ? 13 : 10) * S, col(1, 1, 1, 0.9f));
        ImGui::PopID();
        if (click) {
            if (i == 0) toggleMini();
            else if (i == 1) plat::minimize();
            else if (i == 2) plat::toggleMaximize();
            else plat::close();
        }
    }
}

static bool hasI(const std::string& s, const char* q) {
    return std::search(s.begin(), s.end(), q, q + strlen(q), [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); }) != s.end();
}

// Recherche : ta bibliothèque tout de suite, le catalogue Spotify une demi-seconde après la frappe.
static void updateSearch() {
    g_search.tracks.clear();
    g_search.loaded = true;
    if (!g_searchBuf[0]) return;
    std::set<std::string> seen;
    for (auto& l : g_lists) for (auto& t : l.tracks)
        if (g_search.tracks.size() < 200 && (hasI(t.title, g_searchBuf) || hasI(t.artist, g_searchBuf) || hasI(t.album, g_searchBuf)) && seen.insert(t.uri).second)
            g_search.tracks.push_back(t);
    g_searchT = now();
}

static void sidebarItem(List& l, int index) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(l.uri.c_str());
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = (g_set.density == 0 ? 46 : 56) * S, iw = ImGui::GetContentRegionAvail().x, cs = h - 12 * S;
    if (ImGui::Selectable("##it", g_view == l.uri && !g_lyricsOpen, 0, {iw, h})) { g_view = l.uri; g_lyricsOpen = false; }
    if (l.own && l.uri != "glass:recent" && ImGui::BeginDragDropTarget()) {  // glisser un titre sur une de tes playlists
        if (ImGui::AcceptDragDropPayload("TRACK")) addToList(l, g_dragTrack);
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Lire")) { if (!l.tracks.empty()) play(l.uri, 0); }
        if (ImGui::MenuItem("Ajouter à la file d'attente")) for (auto& t : l.tracks) g_queue.push_back(t);
        if (l.uri != "local" && ImGui::MenuItem("Télécharger pour écouter hors ligne")) downloadAll(l);
        auto& pins = g_set.pinned;
        auto pin = std::find(pins.begin(), pins.end(), l.uri);
        if (ImGui::MenuItem(pin == pins.end() ? "Épingler en haut" : "Désépingler")) {
            if (pin == pins.end()) pins.push_back(l.uri); else pins.erase(pin);
            saveSettings();
        }
        if (ImGui::MenuItem("Personnaliser (nom, pochette)...")) { g_editUri = l.uri; g_openPerso = true; }
        if (!l.own && ImGui::MenuItem("Dupliquer dans mes playlists")) {
            List c{"glass:" + std::to_string(nowId()), l.name + " (copie)", l.image, l.tracks};
            c.own = c.loaded = true;
            g_lists.push_back(c); saveMine(); g_view = c.uri;
            ImGui::EndPopup(); ImGui::PopID(); return;  // g_lists a pu être réalloué : on ne touche plus à l
        }
        if (l.custom && ImGui::MenuItem("Retirer de Glass")) { g_lists.erase(g_lists.begin() + index); saveLinks(); ImGui::EndPopup(); ImGui::PopID(); return; }
        if (l.own && l.uri != "glass:favoris" && l.uri != "glass:recent" && ImGui::MenuItem("Supprimer la playlist")) {
            g_lists.erase(g_lists.begin() + index); saveMine(); ImGui::EndPopup(); ImGui::PopID(); return;
        }
        ImGui::EndPopup();
    }
    coverAt(dl, l.image, l.uri, p + ImVec2{6 * S, 6 * S}, cs, 8 * S);
    bool playingHere = g_plUri == l.uri && current();
    textAt(dl, p + ImVec2{cs + 16 * S, h * 0.5f - 19 * S}, iw - cs - 22 * S, playingHere ? accU() : col(1, 1, 1, 0.95f), l.name);
    std::string sub = l.uri == "local" ? "Tes fichiers" : l.own ? "Playlist Glass" : isAlbum(l.uri) ? "Album" : "Playlist";
    if (l.loaded) sub += " · " + std::to_string(l.tracks.size()) + " titres";
    textAt(dl, p + ImVec2{cs + 16 * S, h * 0.5f + 1 * S}, iw - cs - 22 * S, col(1, 1, 1, 0.5f), sub);
    ImGui::PopID();
}

static void sidebar() {
    static char url[512];
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushFont(g_bold, 18); ImGui::TextUnformatted("Bibliothèque"); ImGui::PopFont();
    ImGui::SameLine(w - 58 * S);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 3 * S);
    if (iconBtn("settings", I_GEAR, 30 * S, g_view == "settings")) { g_view = "settings"; g_lyricsOpen = false; }
    ImGui::SetItemTooltip("Réglages");
    ImGui::SameLine(0, 2 * S);
    if (iconBtn("add", I_ADD, 30 * S)) ImGui::OpenPopup("add");
    ImGui::SetItemTooltip("Nouvelle playlist / ajouter un lien Spotify, Apple Music ou SoundCloud");
    if (ImGui::BeginPopup("add")) {
        if (ImGui::MenuItem("Nouvelle playlist...")) { g_pendingAdd.clear(); g_openNew = true; }
        ImGui::Separator();
        ImGui::TextUnformatted("Lien Spotify, Apple Music ou SoundCloud (playlist, album, titre, profil, likes)");
        ImGui::SetNextItemWidth(360 * S);
        bool ok = ImGui::InputTextWithHint("##url", "https://open.spotify.com/..., music.apple.com/... ou soundcloud.com/...", url, sizeof url, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ok || ImGui::Button("Ajouter")) {
            std::string u = toUri(url);
            if (u.empty()) g_status = "Lien Spotify, Apple Music ou SoundCloud invalide";
            else if (!findList(u)) { List l{u, "Chargement..."}; l.custom = true; g_lists.push_back(l); saveLinks(); g_view = u; }
            else g_view = u;
            url[0] = 0; g_lyricsOpen = false; ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    // recherche globale
    ImGui::SetNextItemWidth(-1);
    if (g_focusSearch) { ImGui::SetKeyboardFocusHere(); g_focusSearch = false; }
    if (ImGui::InputTextWithHint("##search", u8"Rechercher  (Ctrl+F)", g_searchBuf, sizeof g_searchBuf)) { g_view = "search"; g_lyricsOpen = false; updateSearch(); }
    if (g_view == "search" && g_searchBuf[0] && g_searchSent != g_searchBuf && now() - g_searchT > 0.5) {
        g_searchSent = g_searchBuf;
        searchCatalog(g_searchSent);
    }
    if (!g_engineOk) ImGui::TextDisabled("Spotify : moteur arrêté");
    else if (!g_spReady) ImGui::TextDisabled("Spotify : connexion...");
    ImGui::Spacing();

    ImGui::BeginChild("items", {0, 0}, 0, ImGuiWindowFlags_NoBackground);
    auto header = [](const char* s) { ImGui::Spacing(); ImGui::TextDisabled("%s", s); };
    auto pinned = [](const List& l) { return std::find(g_set.pinned.begin(), g_set.pinned.end(), l.uri) != g_set.pinned.end(); };
    // copie : un clic droit peut modifier g_set.pinned pendant la boucle
    std::vector<std::string> pins = g_set.pinned;
    if (std::any_of(g_lists.begin(), g_lists.end(), pinned)) header("ÉPINGLÉES");
    for (auto& u : pins) for (int i = 0; i < (int)g_lists.size(); i++) if (g_lists[i].uri == u) { sidebarItem(g_lists[i], i); break; }
    auto mine = [&](const List& l) { return (l.uri == "local" || l.own) && !pinned(l); };
    auto spot = [&](const List& l) { return l.uri != "local" && !l.own && !l.hidden && !isApple(l.uri) && !isSoundcloud(l.uri) && !pinned(l); };
    auto apple = [&](const List& l) { return isApple(l.uri) && !l.hidden && !pinned(l); };
    auto sc = [&](const List& l) { return isSoundcloud(l.uri) && !pinned(l); };
    if (std::any_of(g_lists.begin(), g_lists.end(), mine)) header("TES PLAYLISTS");
    for (int i = 0; i < (int)g_lists.size(); i++) if (mine(g_lists[i])) sidebarItem(g_lists[i], i);
    if (std::any_of(g_lists.begin(), g_lists.end(), spot)) header("SPOTIFY");
    for (int i = 0; i < (int)g_lists.size(); i++) if (spot(g_lists[i])) sidebarItem(g_lists[i], i);
    if (std::any_of(g_lists.begin(), g_lists.end(), apple)) header("APPLE MUSIC");
    for (int i = 0; i < (int)g_lists.size(); i++) if (apple(g_lists[i])) sidebarItem(g_lists[i], i);
    if (std::any_of(g_lists.begin(), g_lists.end(), sc)) header("SOUNDCLOUD");
    for (int i = 0; i < (int)g_lists.size(); i++) if (sc(g_lists[i])) sidebarItem(g_lists[i], i);
    ImGui::EndChild();
}

static void centerText(const char* s, ImFont* f, float size, ImU32 c) {
    float w = ImGui::GetContentRegionAvail().x;
    size *= S;
    ImVec2 ts = f->CalcTextSizeA(size, w, w, s);
    ImGui::GetWindowDrawList()->AddText(f, size, ImGui::GetCursorScreenPos() + ImVec2{std::max(0.f, (w - ts.x) * 0.5f), 0}, c, s, nullptr, w);
    ImGui::Dummy({w, ts.y + 8 * S});
}

static void loginPanel() {
    ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2{0, ImGui::GetContentRegionAvail().y * 0.32f});
    centerText("Connecte ton compte Spotify", g_bold, 30, col(1, 1, 1, 0.95f));
    centerText(!g_engineOk ? "Le moteur Spotify ne tourne pas (voir Réglages ou relance Glass)."
                           : "Une page de connexion Spotify s'est ouverte dans ton navigateur.", ImGui::GetFont(), 16, col(1, 1, 1, 0.6f));
    centerText("En attendant, tes musiques locales marchent déjà (« Mes musiques »).", ImGui::GetFont(), 16, col(1, 1, 1, 0.45f));
}

static std::map<std::string, int> g_lyrOff;  // décalage des paroles par titre (ms), gardé pendant la session

static void ensureLyrics(const Track& t) {
    if (g_lyrKey != t.uri) {
        g_lyrKey = t.uri; g_lyr.clear(); g_lyrState = 1;
        g_lyrAlign = false;
        if (fileExists(lyricsFile(t, ".lrc"))) {  // déjà écrites ou calées par l'IA : on les partage si ce n'est pas fait
            std::string text = readFile(lyricsFile(t, ".lrc"));
            loadLrcInto(lyricsFile(t, ".lrc"), 1, lrcSource(text));
            if (text.find("[ar:") == std::string::npos) text = lrcHeader(t) + text;  // anciennes paroles sans en-tête
            if (g_lyrState == 2) shareLyrics(t, text);
        }
        else if (t.uri.rfind("spotify:", 0) == 0 && g_spReady)  // Spotify d'abord, puis LRCLIB
            send({{"cmd", "lyrics"}, {"key", t.uri}, {"uri", t.uri}, {"title", t.title}, {"artist", t.artist}, {"ms", t.ms}});
        else findLyrics(t);  // Apple Music, fichiers, Spotify non connecté : base Glass puis LRCLIB
    }
    if (g_genRunning || g_listened.count(t.uri)) return;
    // trouvées sur Internet : l'IA écoute le titre une fois pour les caler sur le vrai son (base Glass d'abord)
    if (g_lyrState == 2 && g_lyrAlign && g_set.alignLyrics) listenLyrics(t, g_lyr, true);
    // rien trouvé : base Glass, puis l'IA les écrit (si activé)
    else if (g_lyrState == 3 && !fileExists(lyricsFile(t, ".none"))) listenLyrics(t, {}, g_set.autoLyrics);
}

static void lyricsView() {
    Track* t = current();
    if (!t) { ImGui::Dummy({0, 80 * S}); centerText("Lance un titre pour voir ses paroles.", g_bold, 22, col(1, 1, 1, 0.6f)); return; }
    ensureLyrics(*t);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    coverAt(dl, t->image, "", p, 64 * S, 8 * S);
    textAt(dl, p + ImVec2{80 * S, 8 * S}, ImGui::GetContentRegionAvail().x - 90 * S, col(1, 1, 1), t->title, g_bold, 22);
    textAt(dl, p + ImVec2{80 * S, 38 * S}, ImGui::GetContentRegionAvail().x - 90 * S, col(1, 1, 1, 0.6f), t->artist);
    ImGui::Dummy({0, 76 * S});
    if (g_lyrState != 2) {
        ImGui::Dummy({0, 60 * S});
        const char* msg = g_lyrState == 1 ? "Chargement des paroles..."
                        : g_lyrState == 4 ? "L'IA écoute le titre et écrit les paroles..."
                        : g_lyrState == 5 ? "Impossible d'écrire des paroles fiables pour ce titre." : "Pas de paroles publiées pour ce titre.";
        centerText(msg, g_bold, 22, col(1, 1, 1, 0.6f));
        if (g_lyrState == 4) { centerText(g_status.c_str(), ImGui::GetFont(), 15, col(1, 1, 1, 0.45f)); g_anim = false; }
        if ((g_lyrState == 3 || g_lyrState == 5) && !g_genRunning) {
            float bw = 300 * S;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - bw) * 0.5f);
            if (ImGui::Button(g_lyrState == 5 ? "Réessayer avec l'IA" : "Écrire les paroles avec l'IA", {bw, 0})) {
                std::error_code ec;
                fs::remove(lyricsFile(*t, ".none"), ec);
                listenLyrics(*t, {}, true);
            }
            centerText("Sur ton PC, ~10 s par titre. 1re fois : téléchargement de ~200 Mo.", ImGui::GetFont(), 14, col(1, 1, 1, 0.4f));
        }
        return;
    }
    // Décalage réglable par titre (paroles en avance / en retard), gardé pendant la session.
    int& off = g_lyrOff[t->uri];
    double pos = posMs() + off;
    int curLine = -1;
    if (g_lyrSynced) for (int i = 0; i < (int)g_lyr.size(); i++) if (g_lyr[i].t <= pos + 120) curLine = i;
    if (g_lyrSynced) {  // petite barre : décalage
        ImGui::SetCursorPos({ImGui::GetWindowContentRegionMax().x - 330 * S, ImGui::GetCursorPosY() - 64 * S});
        ImGui::TextDisabled("Décalage %+.1f s", off / 1000.0);
        ImGui::SameLine();
        if (ImGui::SmallButton(" - ")) off -= 250;
        ImGui::SetItemTooltip("Paroles en avance : les retarder");
        ImGui::SameLine();
        if (ImGui::SmallButton(" + ")) off += 250;
        ImGui::SetItemTooltip("Paroles en retard : les avancer");
        ImGui::SameLine();
        if (off && ImGui::SmallButton("0")) off = 0;
        ImGui::SameLine();
        if (g_genRunning) ImGui::TextDisabled("l'IA écoute...");
        else if (ImGui::SmallButton("Recaler (IA)")) { g_listened.erase(t->uri); listenLyrics(*t, g_lyr, true, true); }
        ImGui::SetItemTooltip("L'IA écoute le titre en entier et cale chaque ligne sur le moment où elle est chantée");
        ImGui::Dummy({0, 34 * S});
    }
    ImGui::BeginChild("lyr", {0, 0}, 0, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    static double userScrollT = -10;
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0) userScrollT = ImGui::GetTime();  // on laisse la main 3 s
    float h = ImGui::GetWindowHeight(), w = ImGui::GetContentRegionAvail().x - 24 * S;
    ImGui::PushFont(g_bold, (float)g_set.lyricsSize);
    ImFont* f = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), lh = fs * 1.22f, targetScroll = -1;
    ImDrawList* ldl = ImGui::GetWindowDrawList();
    ImVec4 a4 = acc();
    ImGui::Dummy({0, h * 0.36f});
    if (g_lyrSynced && curLine < 0 && !g_lyr.empty()) {  // intro : trois points qui se remplissent jusqu'à la 1re ligne
        float k = std::clamp((float)(pos / std::max(1, g_lyr[0].t)), 0.f, 1.f);
        ImVec2 c = ImGui::GetCursorScreenPos() + ImVec2{g_set.lyricsCenter ? w * 0.5f - 24 * S : 8 * S, lh * 0.5f};
        for (int d = 0; d < 3; d++) ldl->AddCircleFilled(c + ImVec2{d * 24 * S, 0}, 6 * S, col(1, 1, 1, 0.25f + 0.75f * std::clamp(k * 3 - d, 0.f, 1.f)));
        ImGui::Dummy({w, lh});
        targetScroll = 0;
    }
    // Découpe en lignes visuelles + largeurs : recalculée seulement si les paroles, la largeur ou la taille changent.
    struct Seg { std::string text; float w; };
    static std::vector<std::vector<Seg>> layout;
    static std::string layoutKey;
    std::string key = g_lyrKey + "|" + std::to_string(g_lyr.size()) + "|" + std::to_string((int)w) + "|" + std::to_string((int)fs);
    if (key != layoutKey) {
        layoutKey = key;
        layout.assign(g_lyr.size(), {});
        for (size_t i = 0; i < g_lyr.size(); i++) {
            std::string txt = g_lyr[i].w.empty() ? u8"♪" : g_lyr[i].w;
            for (const char *p = txt.c_str(), *e = p + txt.size(); p < e;) {
                const char* nx = f->CalcWordWrapPosition(fs, p, e, w);
                if (nx <= p) nx = p + 1;
                std::string part(p, nx);
                while (!part.empty() && part.back() == ' ') part.pop_back();
                layout[i].push_back({part, f->CalcTextSizeA(fs, FLT_MAX, 0, part.c_str()).x});
                p = nx;
                while (p < e && *p == ' ') p++;
            }
        }
    }
    for (int i = 0; i < (int)g_lyr.size(); i++) {
        const auto& segs = layout[i];
        ImVec2 sp = ImGui::GetCursorScreenPos();
        float y0 = ImGui::GetCursorPosY(), blockH = segs.size() * lh;
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("##l", {w, blockH});
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        float on = animTo(ImGui::GetID(i + 100000), i == curLine ? 1.f : 0.f, 8);  // passage en douceur d'une ligne à l'autre
        float base = !g_lyrSynced ? 0.85f : i < curLine ? 0.3f : 0.42f;
        float alpha = base + (0.95f - base) * on + (hov && g_lyrSynced ? 0.15f : 0);
        // progression karaoké dans la ligne chantée
        float prog = 0;
        if (i == curLine) {
            int end = i + 1 < (int)g_lyr.size() ? g_lyr[i + 1].t : g_lyr[i].t + 4000;
            prog = std::clamp((float)((pos - g_lyr[i].t) / std::max(1, std::min(end - g_lyr[i].t, 8000))), 0.f, 1.f);
        }
        float total = 0;
        for (auto& sg : segs) total += sg.w;
        float done = prog * total, acc0 = 0;
        bool visible = ImGui::IsRectVisible(sp, sp + ImVec2{w, blockH});  // hors écran : on ne dessine pas
        for (size_t k = 0; visible && k < segs.size(); k++) {
            const char *sb = segs[k].text.c_str(), *se = sb + segs[k].text.size();
            float sw = segs[k].w;
            ImVec2 tp{sp.x + (g_set.lyricsCenter ? (w - sw) * 0.5f : 0), sp.y + k * lh};
            if (on > 0.01f && g_set.neon > 0)  // lueur néon
                for (ImVec2 o : {ImVec2{2 * S, 0}, ImVec2{-2 * S, 0}, ImVec2{0, 2 * S}, ImVec2{0, -2 * S}})
                    ldl->AddText(f, fs, tp + o, ImGui::GetColorU32(ImVec4{a4.x, a4.y, a4.z, 0.28f * g_set.neon * on}), sb, se);
            ldl->AddText(f, fs, tp, col(1, 1, 1, i == curLine ? 0.42f + 0.2f * (1 - on) : alpha), sb, se);
            if (i == curLine) {  // partie déjà chantée : blanc plein, se remplit de gauche à droite
                float fill = std::clamp(done - acc0, 0.f, sw);
                if (fill > 0) {
                    ldl->PushClipRect(tp, tp + ImVec2{fill, lh}, true);
                    ldl->AddText(f, fs, tp, col(1, 1, 1, on), sb, se);
                    ldl->PopClipRect();
                }
            }
            acc0 += sw;
        }
        if (g_lyrSynced && hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (g_lyrSynced && clicked) { seekMs(g_lyr[i].t - off); userScrollT = -10; }
        if (i == curLine) targetScroll = std::max(0.f, y0 + blockH * 0.5f - h * 0.4f);
        ImGui::Dummy({0, 14 * S});
    }
    ImGui::PopFont();
    ImGui::Dummy({0, h * 0.55f});
    if (!g_lyrSrc.empty()) ImGui::TextDisabled("Paroles : %s", g_lyrSrc.c_str());
    if (g_lyrSynced && targetScroll >= 0 && ImGui::GetTime() - userScrollT > 3) {  // suit la ligne chantée
        float cur = ImGui::GetScrollY(), d = targetScroll - cur;
        if (std::fabs(d) > 0.5f) {
            float k = g_set.animations ? std::min(1.f, ImGui::GetIO().DeltaTime * 7) : 1.f;
            ImGui::SetScrollY(cur + d * k);
            g_anim = g_set.animations;
        }
    }
    ImGui::EndChild();
}

static long long dirSize(const fs::path& d) {
    long long s = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(d, ec)) if (e.is_regular_file(ec)) s += (long long)e.file_size(ec);
    return s;
}

// Assistant : l'app développeur Spotify qui autorise Glass à utiliser le lecteur officiel (Premium).
static void webSetup() {
    static char cid[96];
    if (!ImGui::BeginPopup("websetup")) return;
    ImGui::PushTextWrapPos(470 * S);
    ImGui::PushFont(g_bold, 18); ImGui::TextUnformatted("Son officiel Spotify"); ImGui::PopFont();
    ImGui::TextWrapped("Glass joue la vraie version des titres avec le lecteur web officiel de Spotify, intégré et invisible (pas besoin de l'app Spotify). Il faut une « app développeur » Spotify, une seule fois :");
    ImGui::Spacing();
    ImGui::TextUnformatted("1.");
    ImGui::SameLine();
    if (ImGui::Button("Ouvrir le tableau de bord Spotify")) plat::openUrl("https://developer.spotify.com/dashboard");
    ImGui::TextWrapped("2. Ouvre ton app « Glass » (ou « Create app » si tu n'en as pas : nom et description Glass).");
    ImGui::TextUnformatted("3. Redirect URI :");
    ImGui::SameLine(); ImGui::TextColored(acc(), "http://127.0.0.1:5589/callback");
    ImGui::SameLine(); if (ImGui::SmallButton("Copier")) ImGui::SetClipboardText("http://127.0.0.1:5589/callback");
    ImGui::TextWrapped("4. Dans « APIs used », coche « Web API » ET « Web Playback SDK », puis « Save ».");
    ImGui::TextWrapped("5. Copie le « Client ID » et colle-le ici :");
    ImGui::SetNextItemWidth(300 * S);
    ImGui::InputTextWithHint("##cid", "Client ID", cid, sizeof cid, ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SameLine();
    if (ImGui::Button("Connecter") && strlen(cid) >= 20) {
        send({{"cmd", "webauth"}, {"client_id", cid}});
        g_status = "Accepte la connexion dans la page qui s'ouvre...";
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
}

static void applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.ChildRounding = g_set.rounding * S;
    s.FrameRounding = std::min(g_set.rounding, 18.f) * S;
    s.PopupRounding = std::min(g_set.rounding, 14.f) * S;
    s.GrabRounding = std::min(g_set.rounding, 8.f) * S;
    s.FontScaleMain = g_set.textScale;
    ImVec4* c = s.Colors;
    c[ImGuiCol_ChildBg] = ImVec4{1, 1, 1, g_set.glass};
    c[ImGuiCol_SliderGrab] = c[ImGuiCol_CheckMark] = acc();
    c[ImGuiCol_TextSelectedBg] = ImVec4{acc().x, acc().y, acc().z, 0.35f};
    c[ImGuiCol_DragDropTarget] = acc();
    c[ImGuiCol_PopupBg] = ImVec4{g_set.tint[0] * 0.5f + 0.08f, g_set.tint[1] * 0.5f + 0.08f, g_set.tint[2] * 0.5f + 0.1f, 0.98f};
}

static void applyTheme(const Theme& t) {
    std::copy(t.acc, t.acc + 3, g_set.accent);
    std::copy(t.tint, t.tint + 3, g_set.tint);
    g_set.bg = t.bg; g_set.glass = t.glass; g_set.dim = t.dim; g_set.tintAmt = t.tintAmt; g_set.rounding = t.rounding;
    g_set.neon = t.neon; g_set.accentAuto = t.accentAuto;
    g_set.particles = t.particles >= 0 ? t.particles : g_set.particles == 4 ? 0 : g_set.particles;  // les citrouilles partent avec Halloween
    applyStyle();
}

static void settingsView() {
    static long long cacheBytes = -1;
    static double cacheT = 0;
    if (now() - cacheT > 5) { cacheBytes = dirSize((g_dir / "cache")); cacheT = now(); }
    bool changed = false;
    ImGui::PushFont(g_bold, 34); ImGui::TextUnformatted("Réglages"); ImGui::PopFont();
    ImGui::Spacing();
    auto section = [](const char* s) { ImGui::Spacing(); ImGui::Spacing(); ImGui::PushFont(g_bold, 18); ImGui::TextUnformatted(s); ImGui::PopFont(); ImGui::Separator(); ImGui::Spacing(); };
    float lw = 300 * S;  // assez large pour les libellés longs (sinon ils passent sous les contrôles)
    auto label = [&](const char* s) { ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(s); ImGui::SameLine(lw); ImGui::SetNextItemWidth(280 * S); };

    ImGui::BeginChild("set", {0, 0}, 0, ImGuiWindowFlags_NoBackground);
    section("Thèmes");
    {  // cartes de thème cliquables
        float cw = 150 * S, chh = 74 * S, x0 = ImGui::GetCursorPosX(), avail = ImGui::GetContentRegionAvail().x;
        int perRow = std::max(1, (int)(avail / (cw + 10 * S)));
        for (int i = 0; i < (int)std::size(kThemes); i++) {
            const Theme& t = kThemes[i];
            if (i % perRow) ImGui::SameLine(); else ImGui::SetCursorPosX(x0);
            ImGui::PushID(i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##t", {cw, chh})) { applyTheme(t); changed = true; }
            bool hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImU32 a = ImGui::GetColorU32(ImVec4{t.tint[0] + 0.05f, t.tint[1] + 0.05f, t.tint[2] + 0.05f, 1}), b = ImGui::GetColorU32(ImVec4{t.tint[0] * 0.4f, t.tint[1] * 0.4f, t.tint[2] * 0.4f, 1});
            dl->AddRectFilledMultiColor(p, p + ImVec2{cw, chh}, a, a, b, b);
            dl->AddRect(p, p + ImVec2{cw, chh}, hov ? IM_COL32_WHITE : col(1, 1, 1, 0.15f), 0, 0, hov ? 2 * S : 1 * S);
            dl->AddCircleFilled(p + ImVec2{20 * S, 22 * S}, 9 * S, ImGui::GetColorU32(ImVec4{t.acc[0], t.acc[1], t.acc[2], 1}));
            dl->AddRectFilled(p + ImVec2{36 * S, 16 * S}, p + ImVec2{cw - 14 * S, 28 * S}, col(1, 1, 1, t.glass + 0.08f), 4 * S);
            dl->AddText(g_bold, 14 * S, p + ImVec2{12 * S, chh - 26 * S}, IM_COL32_WHITE, t.name);
            ImGui::PopID();
        }
    }
    section("Couleurs et verre");
    label("Couleur d'accent");
    changed |= ImGui::ColorEdit3("##acc", g_set.accent, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel);
    label("Teinte du fond");
    changed |= ImGui::ColorEdit3("##tint", g_set.tint, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel);
    label("Force de la teinte");
    changed |= ImGui::SliderFloat("##tamt", &g_set.tintAmt, 0, 0.8f, "%.2f");
    label("Fond");
    changed |= ImGui::Combo("##bg", &g_set.bg, "Pochette floutée\0Fond d'écran flouté\0Uni (teinte)\0Dégradé de la pochette (Apple Music)\0Néon\0Aurore animée\0");
    if (g_set.bg == 5) { ImGui::SameLine(); ImGui::TextDisabled("(bouge seulement quand Glass est au premier plan)"); }
    label("Particules");
    changed |= ImGui::Combo("##part", &g_set.particles, "Aucune\0Pétales de sakura\0Neige\0Lucioles\0Citrouilles\0");
    if (g_set.particles) { ImGui::SameLine(); ImGui::TextDisabled("(~30 images/s tant que Glass est visible)"); }
    label("Lueurs néon");
    changed |= ImGui::SliderFloat("##neon", &g_set.neon, 0, 1, g_set.neon > 0 ? "%.2f" : "aucune");
    label("Logo de Glass");
    if (ImGui::Button("Choisir une image...##logo")) g_pick = 4;
    if (!g_set.logo.empty()) { ImGui::SameLine(); if (ImGui::Button("Logo d'origine")) { g_set.logo.clear(); applyLogo(); changed = true; } }
    ImGui::SameLine(); ImGui::TextDisabled("(barre de titre, barre des tâches / Dock ; image carrée conseillée)");
    label("Couleur d'après la pochette");
    changed |= ImGui::Checkbox("##aauto", &g_set.accentAuto);
    label("Transparence des panneaux");
    changed |= ImGui::SliderFloat("##glass", &g_set.glass, 0, 0.35f, "%.2f");
    label("Assombrir le fond");
    changed |= ImGui::SliderFloat("##dim", &g_set.dim, 0, 0.9f, "%.2f");
    label("Arrondi des coins");
    changed |= ImGui::SliderFloat("##round", &g_set.rounding, 0, 30, "%.0f");

    section("Affichage");
    label("Taille du texte");
    changed |= ImGui::SliderFloat("##ts", &g_set.textScale, 0.85f, 1.25f, "%.2f");
    label("Densité des listes");
    changed |= ImGui::Combo("##dens", &g_set.density, "Compacte\0Normale\0Aérée\0");
    label("Colonne Album");
    changed |= ImGui::Checkbox("##alb", &g_set.showAlbum);
    label("Animations");
    changed |= ImGui::Checkbox("##anim", &g_set.animations);
    ImGui::SameLine(); ImGui::TextDisabled("(fondu des pochettes, défilement des paroles)");
    label("Taille des paroles");
    changed |= ImGui::SliderInt("##ly", &g_set.lyricsSize, 18, 48);
    label("Paroles centrées");
    changed |= ImGui::Checkbox("##lyc", &g_set.lyricsCenter);
    label("Caler les paroles sur le son (IA)");
    changed |= ImGui::Checkbox("##alg", &g_set.alignLyrics);
    ImGui::SameLine(); ImGui::TextDisabled("(l'IA écoute chaque titre une fois quand tu ouvres ses paroles)");
    label("Écrire les paroles manquantes (IA)");
    changed |= ImGui::Checkbox("##alyr", &g_set.autoLyrics);
    ImGui::SameLine(); ImGui::TextDisabled("(seulement quand tu ouvres les paroles d'un titre qui n'en a pas)");

    section("Lecture");
    label("Son des titres Spotify / Apple Music");
    changed |= ImGui::Combo("##src", &g_set.source, "Auto (officiel, sinon YouTube)\0Officiel uniquement\0YouTube uniquement\0");
    if (g_set.source != 2) {
        ImGui::SameLine();
        if (!g_webOk) { if (ImGui::Button("Connecter Spotify")) ImGui::OpenPopup("websetup"); }
        else ImGui::TextDisabled(g_sdkReady ? "lecteur prêt" : "démarrage du lecteur...");
        webSetup();
    }
    label("");
    ImGui::TextDisabled(g_set.source == 2 ? "Plus léger, mais parfois une autre version du titre."
                                          : "Vraie version (Premium). Lecteur intégré : ~100-150 Mo de RAM en plus.");
    label("Titres préchargés (YouTube)");
    {
        static const int counts[] = {0, 1, 2, 3, 5, 10, 20};
        int sel = (int)(std::find(std::begin(counts), std::end(counts), g_set.prefetch) - std::begin(counts));
        if (ImGui::Combo("##pre", &sel, "Aucun\0" "1 titre\0" "2 titres\0" "3 titres\0" "5 titres\0" "10 titres\0" "20 titres\0")) { g_set.prefetch = counts[sel]; changed = true; }
    }
    ImGui::SameLine(); ImGui::TextDisabled("(au cas où la connexion lâche)");
    label("Hors ligne");
    if (g_dlRunning) {
        ImGui::Text("%d / %d titres", g_dlDone, g_dlTotal);
        ImGui::SameLine();
        if (ImGui::Button("Arrêter")) { g_dl.clear(); g_dlTotal = g_dlDone; }
    } else ImGui::TextDisabled("Clic droit sur une playlist > Télécharger pour écouter hors ligne");
    label("Vitesse (fichiers / YouTube)");
    if (ImGui::SliderFloat("##spd", &g_set.speed, 0.5f, 2.0f, "x%.2f")) { plat::audioRate(g_set.speed); changed = true; }
    label("S'arrêter à la fin de la liste");
    changed |= ImGui::Checkbox("##sae", &g_set.stopAtEnd);
    label("Touches multimédia du clavier");
    if (ImGui::Checkbox("##mk", &g_set.mediaKeys)) { plat::setMediaKeys(g_set.mediaKeys); changed = true; }
    label("Cache audio YouTube");
    ImGui::Text("%.0f Mo", cacheBytes / 1048576.0);
    ImGui::SameLine();
    if (ImGui::Button("Vider le cache")) {
        std::error_code ec;
        for (auto& e : fs::directory_iterator((g_dir / "cache"), ec)) fs::remove(e.path(), ec);  // le titre en cours reste (fichier ouvert)
        cacheT = 0;
    }

    section("Fenêtre et système");
    label("Toujours au premier plan");
    if (ImGui::Checkbox("##top", &g_set.topmost)) { setTopmost(); changed = true; }
#ifdef _WIN32
    label("Fermer = réduire près de l'horloge");
    changed |= ImGui::Checkbox("##tray", &g_set.tray);
    ImGui::SameLine(); ImGui::TextDisabled("(la musique continue)");
    label("Lancer au démarrage de Windows");
#else
    label("Ouvrir à l'ouverture de session");
#endif
    if (ImGui::Checkbox("##st", &g_set.startup)) { plat::setStartup(g_set.startup); changed = true; }
    label("Mémoire utilisée");
    ImGui::Text("Glass %zu Mo + moteur %zu Mo", plat::memMB(false), plat::memMB(true));
    label("Version");
    if (GLASS_VERSION <= 0) ImGui::TextDisabled("compilée sur ce PC");
    else ImGui::Text("%d", GLASS_VERSION);
    ImGui::SameLine();
    if (g_update.version > GLASS_VERSION) {
        if (ImGui::Button(g_update.busy ? "Installation..." : "Voir et installer la mise à jour") && !g_update.busy) g_openUpdate = true;
    } else if (ImGui::Button("Rechercher une mise à jour")) { g_updT = now(); checkUpdate(true); }
    if (GLASS_VERSION > 0) { ImGui::SameLine(); if (ImGui::Button("Nouveautés de cette version")) showNews(GLASS_VERSION); }
    if (!g_updMsg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", g_updMsg.c_str()); }
    label("Dossier de Glass");
    if (ImGui::Button("Ouvrir")) plat::openUrl(ustr(g_dir));

    section("Apple Music");
    label(g_amAuth ? "Connecté" : g_set.apple && !g_amReady ? "Démarrage..." : "Non connecté");
    if (!g_set.apple || !g_amAuth) {
        if (ImGui::Button("Connecter Apple Music")) { g_set.apple = true; g_amLogin = true; changed = true; if (g_amStarted) plat::webShow(1, true); }
    } else if (ImGui::Button("Afficher la page Apple Music")) { g_amLogin = true; plat::webShow(1, true); }
    if (g_set.apple) { ImGui::SameLine(); if (ImGui::Button("Retirer de Glass##am")) { g_set.apple = false; changed = true; } }
    label("");
    ImGui::TextDisabled("Le vrai lecteur d'Apple, caché dans Glass. Abonnement requis pour les titres entiers (sinon extraits de 30 s).");
    label("");
    ImGui::TextDisabled("Tes playlists apparaissent à gauche. Sans compte, colle un lien Apple Music avec « + ».");
    if (!g_amDiag.empty()) { label("Diagnostic"); ImGui::TextDisabled("%s", g_amDiag.c_str()); }

    section("Compte Spotify");
    label(g_spReady ? "Connecté" : "Non connecté");
    if (ImGui::Button(g_spReady ? "Se déconnecter" : "Se connecter")) logout();

    section("Raccourcis");
    for (const char* s : {"Espace : lecture / pause", "Ctrl + flèches gauche/droite : précédent / suivant", "Ctrl + flèches haut/bas : volume",
                          "Ctrl + F : rechercher", "Ctrl + L : paroles", "Ctrl + Q : file d'attente", "Ctrl + M : mini-lecteur"})
        ImGui::TextDisabled("%s", s);
    ImGui::Dummy({0, 20 * S});
    ImGui::EndChild();
    if (changed) { applyStyle(); saveSettings(); }
}

// Menu clic droit d'un titre (toutes les listes).
static void trackMenu(List& l, int i, int& removeAt) {
    Track t = l.tracks[i];
    if (ImGui::MenuItem("Lire ensuite")) g_queue.push_front(t);
    if (ImGui::MenuItem("Ajouter à la file d'attente")) g_queue.push_back(t);
    if (ImGui::MenuItem(isFav(t.uri) ? "Retirer des favoris" : "Ajouter aux favoris")) toggleFav(t);
    if (ImGui::BeginMenu("Ajouter à la playlist")) {
        for (auto& p : g_lists) if (p.own && p.uri != "glass:recent" && ImGui::MenuItem(p.name.c_str())) addToList(p, t);
        ImGui::Separator();
        if (ImGui::MenuItem("Nouvelle playlist...")) { g_pendingAdd = {t}; g_openNew = true; }
        ImGui::EndMenu();
    }
    if (!t.local && t.uri.rfind("spotify:track:", 0) == 0 && ImGui::MenuItem("Copier le lien Spotify"))
        ImGui::SetClipboardText(("https://open.spotify.com/track/" + t.uri.substr(14)).c_str());
    if (l.own && ImGui::MenuItem("Retirer de cette playlist")) removeAt = i;
    if (l.uri == "local" && ImGui::MenuItem("Retirer de Mes musiques")) removeAt = i;
}

// Tableau des titres d'une liste : tri par colonne, filtre, glisser-déposer, menu clic droit.
static void trackTable(List& l, const char* filter) {
    std::vector<int> idx;
    for (int i = 0; i < (int)l.tracks.size(); i++) {
        auto& t = l.tracks[i];
        if (!filter || !filter[0] || hasI(t.title, filter) || hasI(t.artist, filter) || hasI(t.album, filter)) idx.push_back(i);
    }
    float rowH = (g_set.density == 0 ? 44 : g_set.density == 1 ? 56 : 68) * S, cov = rowH - 16 * S;
    bool canReorder = l.own && l.uri != "glass:recent" && (!filter || !filter[0]);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2{10 * S, 0});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0, 0, 0, 0});
    int cols = g_set.showAlbum ? 4 : 3;
    if (ImGui::BeginTable("tracks", cols, ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 34 * S);
        ImGui::TableSetupColumn("Titre", ImGuiTableColumnFlags_WidthStretch, 2);
        if (g_set.showAlbum) ImGui::TableSetupColumn("Album", ImGuiTableColumnFlags_WidthStretch, 1);
        ImGui::TableSetupColumn("Durée", ImGuiTableColumnFlags_WidthFixed, 56 * S);
        ImGui::TableHeadersRow();
        bool sorted = false;
        if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs(); sp && sp->SpecsCount) {
            sorted = true; canReorder = false;
            auto s = sp->Specs[0];
            int c = s.ColumnIndex - (!g_set.showAlbum && s.ColumnIndex >= 2 ? -1 : 0);  // sans colonne Album, la 2 est Durée
            std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
                const Track &x = l.tracks[a], &y = l.tracks[b];
                int r = c == 1 ? _stricmp(x.title.c_str(), y.title.c_str()) : c == 2 ? _stricmp(x.album.c_str(), y.album.c_str()) : (x.ms > y.ms) - (x.ms < y.ms);
                return s.SortDirection == ImGuiSortDirection_Ascending ? r < 0 : r > 0;
            });
        }
        ImDrawList* tdl = ImGui::GetWindowDrawList();
        ImGuiListClipper clip;
        clip.Begin((int)idx.size(), rowH);
        int removeAt = -1, moveFrom = -1, moveTo = -1;
        while (clip.Step()) for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            int i = idx[r];
            Track& t = l.tracks[i];
            bool cur = isCurrent(l, i);
            ImGui::TableNextRow(0, rowH);
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            ImVec2 c0 = ImGui::GetCursorScreenPos();
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ImGui::GetStyle().ItemSpacing.x, 0});  // sinon les surlignages des lignes se chevauchent
            bool clicked = ImGui::Selectable("##r", cur, ImGuiSelectableFlags_SpanAllColumns, {0, rowH});
            ImGui::PopStyleVar();
            if (clicked)  // un clic suffit
                play(l.uri, i, (sorted || (filter && filter[0])) ? &idx : nullptr);
            bool hov = ImGui::IsItemHovered();
            if (ImGui::BeginDragDropSource()) {  // vers une playlist à gauche, ou pour réordonner
                g_dragTrack = t; g_dragFrom = l.uri; g_dragIdx = i;
                ImGui::SetDragDropPayload("TRACK", &i, sizeof i);
                ImGui::TextUnformatted(t.title.c_str());
                ImGui::EndDragDropSource();
            }
            if (canReorder && ImGui::BeginDragDropTarget()) {
                if (ImGui::AcceptDragDropPayload("TRACK")) {
                    if (g_dragFrom == l.uri) { moveFrom = g_dragIdx; moveTo = i; }
                    else { moveFrom = -2; moveTo = i; }  // titre venu d'une autre liste : inséré ici
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem()) { trackMenu(l, i, removeAt); ImGui::EndPopup(); }
            ImGui::PopID();
            float cy = c0.y + rowH * 0.5f;
            ImU32 dim = col(1, 1, 1, 0.55f), green = accU();
            if (cur) {
                if (g_set.neon > 0) {  // barre néon sur le titre en cours
                    ImVec4 a4 = acc();
                    for (int g = 3; g >= 1; g--) tdl->AddRectFilled({c0.x - 10 * S - g * 2 * S, c0.y + 8 * S}, {c0.x - 7 * S + g * 2 * S, c0.y + rowH - 8 * S}, ImGui::GetColorU32(ImVec4{a4.x, a4.y, a4.z, 0.12f * g_set.neon}), 4 * S);
                    tdl->AddRectFilled({c0.x - 10 * S, c0.y + 8 * S}, {c0.x - 7 * S, c0.y + rowH - 8 * S}, green, 2 * S);
                }
                iconAt(tdl, {c0.x + 12 * S, cy}, isPlaying() ? I_VOL : I_PAUSE, 14 * S, green);
            }
            else if (hov) iconAt(tdl, {c0.x + 12 * S, cy}, I_PLAY, 14 * S, col(1, 1, 1));
            else { std::string n = std::to_string(r + 1); tdl->AddText({c0.x + 12 * S - ImGui::CalcTextSize(n.c_str()).x * 0.5f, cy - 9 * S}, dim, n.c_str()); }

            ImGui::TableNextColumn();
            ImVec2 c1 = ImGui::GetCursorScreenPos();
            float cw = ImGui::GetContentRegionAvail().x;
            coverAt(tdl, t.image, "", {c1.x, cy - cov / 2}, cov, 6 * S);
            bool fav = isFav(t.uri);
            float tx = c1.x + cov + 12 * S;
            textAt(tdl, {tx, cy - 19 * S}, cw - cov - 16 * S - (fav ? 18 * S : 0), cur ? green : col(1, 1, 1, 0.95f), t.title);
            if (fav) iconAt(tdl, {c1.x + cw - 8 * S, cy - 10 * S}, I_HEART, 11 * S, green);
            if (!t.local && isCached(t)) iconAt(tdl, {c1.x + cw - 8 * S, cy + 10 * S}, I_DOWNLOAD, 11 * S, col(1, 1, 1, 0.45f));  // dispo hors ligne
            textAt(tdl, {tx, cy + 1 * S}, cw - cov - 16 * S, dim, t.artist.empty() ? "Artiste inconnu" : t.artist);

            if (g_set.showAlbum) { ImGui::TableNextColumn(); textAt(tdl, {ImGui::GetCursorScreenPos().x, cy - 9 * S}, ImGui::GetContentRegionAvail().x, dim, t.album); }
            ImGui::TableNextColumn();
            tdl->AddText({ImGui::GetCursorScreenPos().x, cy - 9 * S}, dim, fmtTime(t.ms).c_str());
        }
        ImGui::EndTable();
        bool changedList = false;
        if (moveTo >= 0 && moveFrom != moveTo) {
            if (moveFrom == -2) {
                if (std::none_of(l.tracks.begin(), l.tracks.end(), [](const Track& x) { return x.uri == g_dragTrack.uri; })) l.tracks.insert(l.tracks.begin() + moveTo, g_dragTrack);
            } else if (moveFrom >= 0) {
                Track tmp = l.tracks[moveFrom];
                l.tracks.erase(l.tracks.begin() + moveFrom);
                l.tracks.insert(l.tracks.begin() + moveTo, tmp);
            }
            changedList = true;
        }
        if (removeAt >= 0) { l.tracks.erase(l.tracks.begin() + removeAt); changedList = true; }
        if (changedList) {
            if (g_plUri == l.uri) {  // la liste en cours change : on retrouve le titre en cours
                auto it = std::find_if(l.tracks.begin(), l.tracks.end(), [](const Track& x) { return x.uri == g_cur.uri; });
                g_tr = it == l.tracks.end() ? -1 : (int)(it - l.tracks.begin());
                g_order.resize(l.tracks.size());
                for (int k = 0; k < (int)g_order.size(); k++) g_order[k] = k;
                g_pos = std::max(0, g_tr);
            }
            if (l.uri == "local") saveLocal(); else saveMine();
        }
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

static void queueView() {
    ImGui::PushFont(g_bold, 34); ImGui::TextUnformatted("File d'attente"); ImGui::PopFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto row = [&](const Track& t, const char* id, bool playing) {
        ImGui::PushID(id);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = 52 * S;
        bool dbl = ImGui::Selectable("##q", playing, ImGuiSelectableFlags_AllowDoubleClick, {w - 40 * S, h}) && ImGui::IsMouseDoubleClicked(0);
        coverAt(dl, t.image, "", p + ImVec2{4 * S, 6 * S}, 40 * S, 6 * S);
        textAt(dl, p + ImVec2{56 * S, 7 * S}, w - 110 * S, playing ? accU() : col(1, 1, 1, 0.95f), t.title);
        textAt(dl, p + ImVec2{56 * S, 27 * S}, w - 110 * S, col(1, 1, 1, 0.55f), t.artist);
        ImGui::PopID();
        return dbl;
    };
    ImGui::Spacing(); ImGui::TextDisabled("EN COURS");
    if (Track* t = current()) row(*t, "cur", true); else ImGui::TextDisabled("Rien pour l'instant");
    ImGui::Spacing(); ImGui::TextDisabled("DANS LA FILE (%d)", (int)g_queue.size());
    if (!g_queue.empty()) { ImGui::SameLine(); if (ImGui::SmallButton("Vider")) g_queue.clear(); }
    for (int i = 0; i < (int)g_queue.size(); i++) {
        std::string id = "q" + std::to_string(i);
        if (row(g_queue[i], id.c_str(), false)) { Track t = g_queue[i]; g_queue.erase(g_queue.begin() + i); g_tr = -1; startTrack(t); break; }
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10 * S);
        if (iconBtn(id.c_str(), I_CLOSE, 30 * S)) { g_queue.erase(g_queue.begin() + i); break; }
    }
    List* l = findList(g_plUri);
    if (l && !g_order.empty()) {
        ImGui::Spacing(); ImGui::TextDisabled("ENSUITE DANS « %s »", l->name.c_str());
        for (int k = 1; k <= 30 && k < (int)g_order.size(); k++) {
            int pos = (g_pos + k) % (int)g_order.size();
            if (g_order[pos] >= (int)l->tracks.size()) continue;
            std::string id = "n" + std::to_string(k);
            if (row(l->tracks[g_order[pos]], id.c_str(), false)) { playPos(pos); break; }
        }
    }
}

// Rangée de cartes (artistes ronds ou albums carrés) ; un clic ouvre la page.
static void cardRow(const std::vector<Card>& cards, bool round) {
    float cs = 112 * S, cw = cs + 16 * S, avail = ImGui::GetContentRegionAvail().x;
    int n = std::min((int)cards.size(), std::max(1, (int)(avail / cw)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Card open;
    for (int i = 0; i < n; i++) {
        const Card& c = cards[i];
        if (i) ImGui::SameLine(0, 0);
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##c", {cw, cs + 42 * S})) open = c;
        bool hov = ImGui::IsItemHovered();
        ImVec2 a = p + ImVec2{8 * S, 4 * S};
        float k = animTo(ImGui::GetID("##k"), hov ? 1.04f : 1.f, 16), d = cs * (k - 1) * 0.5f;
        if (ImTextureID t = tex(c.image, 300)) dl->AddImageRounded(t, a - ImVec2{d, d}, a + ImVec2{cs + d, cs + d}, {0, 0}, {1, 1}, IM_COL32_WHITE, round ? cs : 10 * S);
        else dl->AddRectFilled(a, a + ImVec2{cs, cs}, col(1, 1, 1, 0.08f), round ? cs : 10 * S);
        if (hov && g_set.neon > 0) neonRect(dl, a - ImVec2{d, d}, a + ImVec2{cs + d, cs + d}, round ? cs : 10 * S, g_set.neon * 0.6f);
        float tw = ImGui::CalcTextSize(c.name.c_str()).x;
        textAt(dl, {a.x + std::max(0.f, (cs - tw) * 0.5f), a.y + cs + 8 * S}, cs, hov ? accU() : col(1, 1, 1, 0.9f), c.name);
        ImGui::PopID();
    }
    if (!open.uri.empty()) openCard(open);
}

static void searchView() {
    ImGui::PushFont(g_bold, 34); ImGui::TextUnformatted("Recherche"); ImGui::PopFont();
    if (!g_searchBuf[0]) { ImGui::TextDisabled("Tape un titre, un artiste ou un album dans la barre à gauche."); return; }
    if (!g_artists.empty()) {  // artistes : photo ronde + nom, un clic ouvre sa page
        ImGui::Spacing(); ImGui::TextDisabled("ARTISTES");
        cardRow(g_artists, true);
    }
    ImGui::Spacing();
    ImGui::TextDisabled(g_webOk ? "TITRES (ta bibliothèque + catalogue Spotify, %d)" : "TITRES (ta bibliothèque + catalogue Apple Music, %d)", (int)g_search.tracks.size());
    trackTable(g_search, nullptr);
}

static void mainView() {
    static char filter[128];
    if (g_lyricsOpen) { lyricsView(); return; }
    if (g_view == "settings") { settingsView(); return; }
    if (g_view == "queue") { queueView(); return; }
    if (g_view == "search") { searchView(); return; }
    List* l = findList(g_view);
    if (!l) { if (g_lists.empty()) return; l = &g_lists[0]; g_view = l->uri; }
    if (l->uri.rfind("spotify:", 0) == 0 && !g_spReady) { loginPanel(); return; }
    requestTracks(*l);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool local = l->uri == "local";

    // --- en-tête ---
    float hero = 200 * S, w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool artist = l->uri.rfind("artist:", 0) == 0;
    dl->AddRectFilled(p + ImVec2{4 * S, 8 * S}, p + ImVec2{hero + 4 * S, hero + 8 * S}, col(0, 0, 0, 0.35f), artist ? hero : 16 * S);  // ombre (ronde pour un artiste)
    if (ImTextureID t = artist ? tex(l->image, 300) : 0) dl->AddImageRounded(t, p, p + ImVec2{hero, hero}, {0, 0}, {1, 1}, IM_COL32_WHITE, hero);
    else coverAt(dl, l->image, l->uri, p, hero, 14 * S);
    ImGui::Dummy({hero, hero});
    ImGui::SameLine(0, 28 * S);
    ImGui::BeginGroup();
    float tx = w - hero - 28 * S;
    long long total = 0;
    for (auto& t : l->tracks) total += t.ms;
    ImVec2 q = ImGui::GetCursorScreenPos();
    const char* kind = local ? "TES MUSIQUES" : l->own ? "PLAYLIST GLASS" : artist ? "ARTISTE · TITRES LES PLUS ÉCOUTÉS" : l->uri.rfind("itunes:album:", 0) == 0 ? "ALBUM"
                     : isAlbum(l->uri) ? (isApple(l->uri) ? "ALBUM APPLE MUSIC" : "ALBUM") : isApple(l->uri) ? "PLAYLIST APPLE MUSIC" : "PLAYLIST";
    dl->AddText(g_bold, 12 * S, q + ImVec2{0, 26 * S}, col(1, 1, 1, 0.7f), kind);
    textAt(dl, q + ImVec2{0, 40 * S}, tx, col(1, 1, 1), l->name, g_bold, 46);
    char meta[96];
    snprintf(meta, sizeof meta, "%d titres · %lld h %lld min", (int)l->tracks.size(), total / 3600000, total / 60000 % 60);
    dl->AddText(q + ImVec2{0, 108 * S}, col(1, 1, 1, 0.6f), l->loaded ? meta : "Chargement...");
    ImGui::SetCursorScreenPos(q + ImVec2{0, 144 * S});
    bool playingThis = g_plUri == l->uri && isPlaying();
    if (playBtn("hero", 54 * S, playingThis, accU(), col(0, 0, 0))) {
        if (g_plUri == l->uri && current()) togglePlay();
        else if (!l->tracks.empty()) play(l->uri, g_shuffle ? std::mt19937{std::random_device{}()}() % l->tracks.size() : 0);
    }
    float bx = 68 * S;
    auto btn = [&](const char* id, const char* icon, bool active, const char* tip) {
        ImGui::SetCursorScreenPos(q + ImVec2{bx, 153 * S}); bx += 42 * S;
        bool r = iconBtn(id, icon, 36 * S, active);
        ImGui::SetItemTooltip("%s", tip);
        return r;
    };
    if (btn("shuf", I_SHUFFLE, g_shuffle, "Aléatoire")) toggleShuffle();
    if (btn("edit", I_EDIT, false, "Personnaliser (nom, pochette)")) { g_editUri = l->uri; g_openPerso = true; }
    if (btn("qadd", I_QUEUE, false, "Tout ajouter à la file d'attente")) { for (auto& t : l->tracks) g_queue.push_back(t); g_status = "Ajouté à la file d'attente"; }
    if (!local && btn("dl", I_DOWNLOAD, g_dlRunning, g_dlRunning ? "Téléchargement en cours (voir en haut)" : "Télécharger pour écouter hors ligne")) downloadAll(*l);
    if (local) {
        ImGui::SetCursorScreenPos(q + ImVec2{bx + 6 * S, 153 * S});
        if (ImGui::Button("+ Fichiers")) g_pick = 1;
        ImGui::SameLine();
        if (ImGui::Button("+ Dossier")) g_pick = 2;
    }
    ImGui::SetCursorScreenPos({std::max(q.x + 420 * S, q.x + tx - 260 * S), q.y + 153 * S});
    ImGui::SetNextItemWidth(260 * S);
    ImGui::InputTextWithHint("##f", "Rechercher dans la liste", filter, sizeof filter);
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(p + ImVec2{0, hero + 18 * S});
    ImGui::Dummy({0, 0});  // ImGui exige un élément après un déplacement du curseur
    if (artist) if (auto it = g_albums.find(l->uri); it != g_albums.end() && !it->second.empty()) {  // ses albums
        ImGui::TextDisabled("ALBUMS (%d)", (int)it->second.size());
        ImGui::BeginChild("albums", {0, 170 * S}, 0, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_HorizontalScrollbar);
        std::vector<Card> albums = it->second;  // copie : un clic ajoute une liste
        float cw = 128 * S;
        ImGui::Dummy({cw * albums.size(), 0});
        ImGui::SetCursorPos({0, 0});
        for (size_t i = 0; i < albums.size(); i++) {
            if (i) ImGui::SameLine(0, 0);
            ImGui::PushID((int)i);
            ImVec2 a = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##a", {cw, 160 * S})) { openCard(albums[i]); ImGui::PopID(); break; }
            bool hov = ImGui::IsItemHovered();
            ImDrawList* adl = ImGui::GetWindowDrawList();
            coverAt(adl, albums[i].image, "", a + ImVec2{6 * S, 0}, 112 * S, 10 * S);
            textAt(adl, a + ImVec2{6 * S, 118 * S}, 112 * S, hov ? accU() : col(1, 1, 1, 0.85f), albums[i].name);
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::TextDisabled("TITRES");
    }

    if (l->tracks.empty() && l->loaded) {
        if (!local && !l->own) ImGui::TextDisabled("Aucun titre dans cette liste.");
        if (local) {
            ImGui::TextDisabled("Glisse tes fichiers ou dossiers de musique (mp3, flac, m4a, wav...) dans la fenêtre,");
            ImGui::TextDisabled("ou clique sur « + Fichiers » / « + Dossier ».");
        } else if (l->own) {
            ImGui::TextDisabled("Playlist vide : clic droit sur un titre > « Ajouter à la playlist »,");
            ImGui::TextDisabled("ou glisse un titre sur cette playlist dans la colonne de gauche.");
        }
        return;
    }
    trackTable(*l, filter);
}

static void playerBar() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    float w = avail.x, h = avail.y, cy = o.y + h * 0.5f;
    Track* t = current();

    // gauche : piste en cours (clic = aller à sa liste) + favori
    if (t) {
        float cs = 60 * S, tw = w * 0.3f - cs - 54 * S;
        coverAt(dl, t->image, t->local ? "local" : "", {o.x, cy - cs / 2}, cs, 8 * S);
        textAt(dl, {o.x + cs + 14 * S, cy - 20 * S}, tw, col(1, 1, 1), t->title, g_bold, 16);
        textAt(dl, {o.x + cs + 14 * S, cy + 2 * S}, tw, col(1, 1, 1, 0.6f), t->artist);
        ImGui::SetCursorScreenPos({o.x, cy - cs / 2});
        if (ImGui::InvisibleButton("##now", {cs + tw, cs})) { if (findList(g_plUri)) g_view = g_plUri; g_lyricsOpen = false; }
        ImGui::SetCursorScreenPos({o.x + cs + 14 * S + tw + 6 * S, cy - 17 * S});
        bool fav = isFav(t->uri);
        if (iconBtn("fav", fav ? I_HEART : I_HEART_O, 34 * S, fav)) toggleFav(*t);
        ImGui::SetItemTooltip(fav ? "Retirer des favoris" : "Ajouter aux favoris");
    }

    // centre : contrôles + progression
    float cw = std::min(w * 0.40f, 620 * S), cx = o.x + (w - cw) * 0.5f;
    float bs = 36 * S, ps = 44 * S, gap = 12 * S, row = bs * 4 + ps + gap * 4;
    float bx = cx + (cw - row) * 0.5f, by = o.y + 2 * S;
    ImGui::SetCursorScreenPos({bx, by + (ps - bs) / 2});
    if (iconBtn("shuffle", I_SHUFFLE, bs, g_shuffle)) toggleShuffle();
    ImGui::SetCursorScreenPos({bx + (bs + gap), by + (ps - bs) / 2});
    if (iconBtn("prev", I_PREV, bs)) { if (posMs() > 3000) seekMs(0); else step(-1); }
    ImGui::SetCursorScreenPos({bx + (bs + gap) * 2, by});
    if (playBtn("main", ps, isPlaying() || g_loading, col(1, 1, 1), col(0, 0, 0))) togglePlay();
    ImGui::SetCursorScreenPos({bx + (bs + gap) * 2 + ps + gap, by + (ps - bs) / 2});
    if (iconBtn("next", I_NEXT, bs)) step(1);
    ImGui::SetCursorScreenPos({bx + (bs + gap) * 3 + ps + gap, by + (ps - bs) / 2});
    if (iconBtn("repeat", I_REPEAT1, bs, g_repeat)) g_repeat = !g_repeat;
    ImGui::SetItemTooltip("Répéter le titre");

    static float sv = 0;
    static bool drag = false;
    double dur = durMs();
    if (!drag) sv = (float)posMs();
    float sy = by + ps + 8 * S, tw = 44 * S;
    std::string a = g_loading && t ? "..." : fmtTime(sv), b = fmtTime(dur);
    dl->AddText({cx + tw - ImGui::CalcTextSize(a.c_str()).x - 8 * S, sy - 1 * S}, col(1, 1, 1, 0.6f), a.c_str());
    ImGui::SetCursorScreenPos({cx + tw, sy});
    bool rel = scrub("##seek", &sv, (float)std::max(dur, 1.0), cw - tw * 2);
    drag = ImGui::IsItemActive();
    if (rel && t) seekMs(sv);
    dl->AddText({cx + cw - tw + 8 * S, sy - 1 * S}, col(1, 1, 1, 0.6f), b.c_str());

    // droite : paroles, file, plus, volume
    float vw = 100 * S, rx = o.x + w - vw - 30 * S - 34 * S * 3;
    ImGui::SetCursorScreenPos({rx, cy - 17 * S});
    if (iconBtn("lyrics", I_LYRICS, 34 * S, g_lyricsOpen)) g_lyricsOpen = !g_lyricsOpen;
    ImGui::SetItemTooltip("Paroles (Ctrl+L)");
    ImGui::SetCursorScreenPos({rx + 34 * S, cy - 17 * S});
    if (iconBtn("queue", I_QUEUE, 34 * S, g_view == "queue" && !g_lyricsOpen)) { g_view = "queue"; g_lyricsOpen = false; }
    ImGui::SetItemTooltip("File d'attente (Ctrl+Q)");
    ImGui::SetCursorScreenPos({rx + 68 * S, cy - 17 * S});
    if (iconBtn("more", I_MORE, 34 * S, g_sleepAt > 0 || g_sleepEnd)) ImGui::OpenPopup("more");
    ImGui::SetItemTooltip("Minuterie, vitesse, mini-lecteur");
    if (ImGui::BeginPopup("more")) {
        ImGui::TextDisabled("MINUTERIE");
        if (g_sleepAt > 0) ImGui::Text("Arrêt dans %d min", (int)((g_sleepAt - now()) / 60) + 1);
        if (g_sleepEnd) ImGui::Text("Arrêt à la fin du titre");
        for (int m : {15, 30, 45, 60, 90}) if (ImGui::MenuItem((std::to_string(m) + " minutes").c_str())) { g_sleepAt = now() + m * 60; g_sleepEnd = false; }
        if (ImGui::MenuItem("À la fin du titre")) { g_sleepEnd = true; g_sleepAt = 0; }
        if ((g_sleepAt > 0 || g_sleepEnd) && ImGui::MenuItem("Désactiver la minuterie")) { g_sleepAt = 0; g_sleepEnd = false; }
        ImGui::Separator();
        ImGui::TextDisabled("VITESSE (fichiers / YouTube)");
        ImGui::SetNextItemWidth(200 * S);
        if (ImGui::SliderFloat("##spd2", &g_set.speed, 0.5f, 2.0f, "x%.2f")) { plat::audioRate(g_set.speed); saveSettings(); }
        ImGui::Separator();
        if (ImGui::MenuItem("Mini-lecteur", "Ctrl+M")) toggleMini();
        if (g_update.version > GLASS_VERSION) { if (ImGui::MenuItem("Mise à jour disponible...")) g_openUpdate = true; }
        else if (ImGui::MenuItem("Rechercher une mise à jour")) { g_updT = now(); checkUpdate(true); }
        ImGui::EndPopup();
    }
    iconAt(dl, {o.x + w - vw - 16 * S, cy}, I_VOL, 15 * S, col(1, 1, 1, 0.75f));
    ImGui::SetCursorScreenPos({o.x + w - vw, cy - 8 * S});
    float old = g_set.vol;
    if (scrub("##vol", &g_set.vol, 1, vw, 0.05f)) saveSettings();
    if (g_set.vol != old) setVolume();
}

// Texte centré sur une ligne, coupé avec « … » ; fill = partie déjà chantée (karaoké, 0..1).
static void centeredLine(ImDrawList* dl, float cx, float y, float maxW, const std::string& txt, ImFont* f, float size, ImU32 dim, ImU32 bright, float fill) {
    float tw = std::min(maxW, f->CalcTextSizeA(size * S * g_set.textScale, FLT_MAX, 0, txt.c_str()).x);
    ImVec2 p{cx - tw * 0.5f, y};
    textAt(dl, p, maxW, dim, txt, f, size);
    if (fill > 0) {
        dl->PushClipRect(p, p + ImVec2{tw * fill, size * S * 2}, true);
        textAt(dl, p, maxW, bright, txt, f, size);
        dl->PopClipRect();
    }
}

static void miniPlayer(ImGuiViewport* vp) {  // fenêtre compacte, toujours au premier plan
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = vp->Pos, sz = vp->Size;
    Track* t = current();
    float pad = 14 * S, cs = 112 * S, x = o.x + pad + cs + 16 * S, right = o.x + sz.x - pad;
    coverAt(dl, t ? t->image : "icon:", "", o + ImVec2{pad, pad}, cs, 12 * S);

    // en haut à droite : paroles, agrandir
    ImGui::SetCursorScreenPos({right - 62 * S, o.y + 8 * S});
    if (iconBtn("mlyr", I_LYRICS, 30 * S, g_miniLyrics)) toggleMiniLyrics();
    ImGui::SetItemTooltip("Paroles");
    ImGui::SetCursorScreenPos({right - 30 * S, o.y + 8 * S});
    if (iconBtn("expand", I_EXPAND, 30 * S)) toggleMini();
    ImGui::SetItemTooltip("Agrandir");
    float tw = right - 70 * S - x;
    textAt(dl, {x, o.y + 15 * S}, tw, col(1, 1, 1), t ? t->title : "Glass", g_bold, 15);
    textAt(dl, {x, o.y + 36 * S}, tw, col(1, 1, 1, 0.6f), t ? t->artist : "Rien en lecture");

    // contrôles + volume
    float by = o.y + 60 * S;
    ImGui::SetCursorScreenPos({x - 6 * S, by + 4 * S});
    if (iconBtn("mprev", I_PREV, 32 * S)) { if (posMs() > 3000) seekMs(0); else step(-1); }
    ImGui::SetCursorScreenPos({x + 30 * S, by});
    if (playBtn("mplay", 40 * S, isPlaying() || g_loading, col(1, 1, 1), col(0, 0, 0))) togglePlay();
    ImGui::SetCursorScreenPos({x + 74 * S, by + 4 * S});
    if (iconBtn("mnext", I_NEXT, 32 * S)) step(1);
    ImGui::SetCursorScreenPos({x + 108 * S, by + 4 * S});
    bool fav = t && isFav(t->uri);
    if (iconBtn("mfav", fav ? I_HEART : I_HEART_O, 32 * S, fav) && t) toggleFav(*t);
    float vx = x + 152 * S, vw = right - vx - 22 * S;
    if (vw > 30 * S) {
        iconAt(dl, {vx + 8 * S, by + 20 * S}, I_VOL, 13 * S, col(1, 1, 1, 0.7f));
        ImGui::SetCursorScreenPos({vx + 22 * S, by + 12 * S});
        float old = g_set.vol;
        if (scrub("##mvol", &g_set.vol, 1, vw, 0.05f)) saveSettings();
        if (g_set.vol != old) setVolume();
    }

    // progression avec les temps
    static float sv = 0;
    static bool drag = false;
    double dur = durMs();
    if (!drag) sv = (float)posMs();
    float py = o.y + 112 * S, tl = 36 * S;
    std::string a1 = g_loading && t ? "..." : fmtTime(sv), a2 = fmtTime(dur);
    dl->AddText({x, py - 1 * S}, col(1, 1, 1, 0.55f), a1.c_str());
    dl->AddText({right - ImGui::CalcTextSize(a2.c_str()).x, py - 1 * S}, col(1, 1, 1, 0.55f), a2.c_str());
    ImGui::SetCursorScreenPos({x + tl, py});
    bool rel = scrub("##mseek", &sv, (float)std::max(dur, 1.0), right - x - tl * 2);
    drag = ImGui::IsItemActive();
    if (rel && t) seekMs(sv);

    // paroles sous le lecteur : ligne précédente, ligne chantée (karaoké), ligne suivante
    if (g_miniLyrics) {
        float ly = o.y + 150 * S, cx = o.x + sz.x * 0.5f, mw = sz.x - pad * 2;
        dl->AddLine({o.x + pad, ly - 6 * S}, {right, ly - 6 * S}, col(1, 1, 1, 0.08f));
        if (!t) { centeredLine(dl, cx, ly + 40 * S, mw, "Lance un titre", g_bold, 15, col(1, 1, 1, 0.5f), 0, 0); return; }
        ensureLyrics(*t);
        if (g_lyrState != 2) {
            const char* m = g_lyrState == 1 ? "Chargement des paroles..." : g_lyrState == 4 ? "L'IA écrit les paroles..." : "Pas de paroles pour ce titre";
            centeredLine(dl, cx, ly + 40 * S, mw, m, g_bold, 15, col(1, 1, 1, 0.5f), 0, 0);
            return;
        }
        double pos = posMs() + g_lyrOff[t->uri];
        int cur = -1;
        if (g_lyrSynced) for (int i = 0; i < (int)g_lyr.size(); i++) if (g_lyr[i].t <= pos + 120) cur = i;
        auto line = [&](int i) { return i >= 0 && i < (int)g_lyr.size() ? (g_lyr[i].w.empty() ? std::string(u8"♪") : g_lyr[i].w) : std::string(); };
        float fill = 0;
        if (cur >= 0) {
            int end = cur + 1 < (int)g_lyr.size() ? g_lyr[cur + 1].t : g_lyr[cur].t + 4000;
            fill = std::clamp((float)((pos - g_lyr[cur].t) / std::max(1, std::min(end - g_lyr[cur].t, 8000))), 0.f, 1.f);
        }
        if (!g_lyrSynced) cur = 0;
        ImVec4 a4 = acc();
        centeredLine(dl, cx, ly + 4 * S, mw, line(cur - 1), ImGui::GetFont(), 14, col(1, 1, 1, 0.35f), 0, 0);
        if (g_set.neon > 0 && cur >= 0) glow(dl, {cx, ly + 50 * S}, mw * 0.35f, a4, 0.18f * g_set.neon);
        centeredLine(dl, cx, ly + 36 * S, mw, cur >= 0 ? line(cur) : std::string(u8"• • •"), g_bold, 19, col(1, 1, 1, 0.45f), col(1, 1, 1), g_lyrSynced ? fill : 1);
        centeredLine(dl, cx, ly + 74 * S, mw, line(cur + 1), ImGui::GetFont(), 14, col(1, 1, 1, 0.35f), 0, 0);
    }
}

// Notes de version (texte des versions GitHub) : 1re ligne = titre, « ## New / Improved / Fixed » = sections, « - » = puces.
static void notesText(const std::string& notes) {
    std::istringstream in(notes);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool first = true;
    for (std::string l; std::getline(in, l);) {
        if (l.empty()) continue;
        if (first) { first = false; ImGui::PushFont(g_bold, 17); ImGui::TextWrapped("%s", l.c_str()); ImGui::PopFont(); ImGui::Spacing(); continue; }
        if (l.rfind("## ", 0) == 0) {  // section : pastille colorée
            std::string name = l.substr(3);
            ImVec4 c = name == "Fixed" ? ImVec4{1.f, 0.55f, 0.35f, 1} : name == "Improved" ? ImVec4{0.35f, 0.75f, 1.f, 1} : acc();
            ImGui::Dummy({0, 6 * S});
            ImVec2 p = ImGui::GetCursorScreenPos(), ts = g_bold->CalcTextSizeA(12 * S, FLT_MAX, 0, name.c_str());
            dl->AddRectFilled(p, p + ImVec2{ts.x + 20 * S, 22 * S}, ImGui::GetColorU32(ImVec4{c.x, c.y, c.z, 0.22f}), 11 * S);
            dl->AddText(g_bold, 12 * S, p + ImVec2{10 * S, 11 * S - ts.y * 0.5f}, ImGui::GetColorU32(c), name.c_str());
            ImGui::Dummy({ts.x + 20 * S, 26 * S});
            continue;
        }
        bool bullet = l.rfind("- ", 0) == 0 || l.rfind("* ", 0) == 0;
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (bullet) dl->AddCircleFilled(p + ImVec2{6 * S, ImGui::GetTextLineHeight() * 0.5f + 1 * S}, 3 * S, accU());
        ImGui::Indent(18 * S);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1, 1, 1, bullet ? 0.88f : 0.7f});
        ImGui::TextWrapped("%s", l.c_str() + (bullet ? 2 : 0));
        ImGui::PopStyleColor();
        ImGui::Unindent(18 * S);
        ImGui::Dummy({0, 2 * S});
    }
}

// Fenêtre des nouveautés / de la mise à jour : bandeau aux couleurs du thème, notes, bouton principal.
static bool newsWindow(const char* id, const char* heading, const std::string& sub, const std::string& notes, const char* primary, const char* secondary, bool& primaryClicked) {
    primaryClicked = false;
    ImGui::SetNextWindowSize({560 * S, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18 * S);
    bool open = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar(2);
    if (!open) return false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    float hh = 96 * S;
    ImVec4 a = acc();
    // bandeau : dégradé de l'accent, halo, logo
    dl->AddRectFilled(p, p + ImVec2{sz.x, hh}, ImGui::GetColorU32(ImVec4{a.x * 0.35f, a.y * 0.35f, a.z * 0.35f, 1}), 18 * S, ImDrawFlags_RoundCornersTop);
    dl->PushClipRect(p, p + ImVec2{sz.x, hh}, true);
    glow(dl, p + ImVec2{sz.x * 0.85f, hh * 0.2f}, hh * 1.6f, a, 0.55f);
    glow(dl, p + ImVec2{sz.x * 0.15f, hh}, hh * 1.2f, ImVec4{0.55f, 0.3f, 1.f, 1}, 0.35f);
    dl->PopClipRect();
    if (ImTextureID logo = tex(logoSrc(), 64)) dl->AddImageRounded(logo, p + ImVec2{24 * S, hh * 0.5f - 24 * S}, p + ImVec2{72 * S, hh * 0.5f + 24 * S}, {0, 0}, {1, 1}, IM_COL32_WHITE, 12 * S);
    dl->AddText(g_bold, 24 * S, p + ImVec2{88 * S, hh * 0.5f - 26 * S}, IM_COL32_WHITE, heading);
    dl->AddText(ImGui::GetFont(), 14 * S, p + ImVec2{88 * S, hh * 0.5f + 4 * S}, col(1, 1, 1, 0.7f), sub.c_str());
    ImGui::Dummy({sz.x, hh + 14 * S});
    // notes
    ImGui::SetCursorPosX(24 * S);
    ImGui::BeginChild("notes", {sz.x - 48 * S, std::min(340 * S, ImGui::GetTextLineHeightWithSpacing() * 15)}, 0, ImGuiWindowFlags_NoBackground);
    notesText(notes.empty() ? "No release notes for this version." : notes);
    ImGui::EndChild();
    // boutons
    ImGui::Dummy({0, 10 * S});
    float bw = 170 * S, sw = secondary ? 110 * S : 0, x = sz.x - 24 * S - bw - (secondary ? sw + 10 * S : 0);
    ImGui::SetCursorPosX(x);
    if (secondary) { if (ImGui::Button(secondary, {sw, 38 * S})) ImGui::CloseCurrentPopup(); ImGui::SameLine(0, 10 * S); }
    ImGui::PushStyleColor(ImGuiCol_Button, a);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{std::min(1.f, a.x * 1.15f), std::min(1.f, a.y * 1.15f), std::min(1.f, a.z * 1.15f), 1});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, scale(a, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0, 0, 0, 0.9f});
    ImGui::PushFont(g_bold, 15);
    if (ImGui::Button(primary, {bw, 38 * S})) { primaryClicked = true; ImGui::CloseCurrentPopup(); }
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    ImGui::Dummy({0, 14 * S});
    ImGui::EndPopup();
    return true;
}

// Fenêtres « Personnaliser » et « Nouvelle playlist ».
static void dialogs() {
    static char name[128];
    if (g_openPerso) { ImGui::OpenPopup("Personnaliser"); g_openPerso = false; if (List* l = findList(g_editUri)) snprintf(name, sizeof name, "%s", l->name.c_str()); }
    if (g_openNew) { ImGui::OpenPopup("Nouvelle playlist"); g_openNew = false; snprintf(name, sizeof name, "Ma playlist"); }
    if (g_openUpdate) { ImGui::OpenPopup("Mise à jour"); g_openUpdate = false; }
    if (g_openNews) { ImGui::OpenPopup("Nouveautés"); g_openNews = false; }
    bool go;
    newsWindow("Mise à jour", "Update available", "Glass " + std::to_string(g_update.version) + " is ready (you have " + std::to_string(GLASS_VERSION) + "). Glass restarts by itself.",
               g_update.notes, g_update.busy ? "Installing..." : "Install & restart", "Later", go);
    if (go && !g_update.busy) installUpdate();
    newsWindow("Nouveautés", "What's new in Glass", "Version " + std::to_string(g_newsVersion), g_newsText, "Got it", nullptr, go);
    ImGui::SetNextWindowSize({460 * S, 0});
    if (ImGui::BeginPopupModal("Personnaliser", nullptr, ImGuiWindowFlags_NoResize)) {
        List* l = findList(g_editUri);
        if (!l) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        ImVec2 p = ImGui::GetCursorScreenPos();
        coverAt(ImGui::GetWindowDrawList(), l->image, l->uri, p, 120 * S, 12 * S);
        ImGui::Dummy({120 * S, 120 * S});
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextDisabled("Nom");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##name", name, sizeof name);
        if (ImGui::Button("Choisir une image...")) { g_pick = 3; g_pickUri = l->uri; }
        if (!l->own) {
            ImGui::SameLine();
            if (ImGui::Button("Réinitialiser")) { g_over.erase(l->uri); saveOverrides(); l->requested = false; g_status = "Nom et pochette d'origine au prochain chargement"; }
        }
        ImGui::EndGroup();
        if (!l->own) ImGui::TextDisabled("Le changement reste dans Glass (la playlist d'origine n'est pas modifiée).");
        ImGui::Spacing();
        if (ImGui::Button("Enregistrer", {120 * S, 0}) && name[0]) {
            l->name = name;
            if (l->own) saveMine();
            else { g_over[l->uri].first = name; saveOverrides(); }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Fermer", {120 * S, 0})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowSize({400 * S, 0});
    if (ImGui::BeginPopupModal("Nouvelle playlist", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextDisabled("Nom de la playlist");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool ok = ImGui::InputText("##nn", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button("Créer", {120 * S, 0}) || ok) && name[0]) {
            List l{"glass:" + std::to_string(nowId()), name, g_pendingAdd.empty() ? "" : g_pendingAdd[0].image, g_pendingAdd};
            l.own = l.loaded = true;
            g_lists.push_back(l); saveMine();
            g_view = l.uri; g_lyricsOpen = false; g_pendingAdd.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Annuler", {120 * S, 0})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

static void drawUI() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    updateAccent();
    drawBackground(vp);
    ImGui::SetNextWindowPos(vp->Pos); ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0, 0});
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    if (g_mini) {
        if (g_set.neon > 0) neonRect(ImGui::GetBackgroundDrawList(), vp->Pos + ImVec2{3 * S, 3 * S}, vp->Pos + vp->Size - ImVec2{3 * S, 3 * S}, 10 * S, g_set.neon);
        miniPlayer(vp);
    } else {
        float tb = 42 * S, gap = 10 * S, barH = 100 * S, sideW = 290 * S;
        titleBar(vp, tb);
        float bodyH = vp->Size.y - tb - barH - gap * 2;
        if (g_set.neon > 0) {  // contours néon des panneaux, dessinés sous le verre
            ImDrawList* bg = ImGui::GetBackgroundDrawList();
            float r = g_set.rounding * S;
            ImVec2 o = vp->Pos;
            neonRect(bg, o + ImVec2{gap, tb}, o + ImVec2{gap + sideW, tb + bodyH}, r, g_set.neon * 0.6f);
            neonRect(bg, o + ImVec2{gap * 2 + sideW, tb}, o + ImVec2{vp->Size.x - gap, tb + bodyH}, r, g_set.neon * 0.6f);
            neonRect(bg, o + ImVec2{gap, tb + bodyH + gap}, o + ImVec2{vp->Size.x - gap, tb + bodyH + gap + barH}, r, g_set.neon);
        }
        ImGui::SetCursorPos({gap, tb});
        ImGui::BeginChild("side", {sideW, bodyH}, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        sidebar();
        ImGui::EndChild();
        ImGui::SameLine(0, gap);
        ImGui::BeginChild("main", {vp->Size.x - sideW - gap * 3, bodyH}, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        static std::string lastView;
        static double viewT = 0;
        std::string key = g_view + (g_lyricsOpen ? "|paroles" : "");
        if (key != lastView) { lastView = key; viewT = ImGui::GetTime(); }
        float k = g_set.animations ? (float)std::min(1.0, (ImGui::GetTime() - viewT) / 0.22) : 1.f;
        if (k < 1) g_anim = true;
        k = 1 - (1 - k) * (1 - k);  // ralentit en fin de course
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.15f + 0.85f * k);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1 - k) * 14 * S);
        mainView();
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::SetCursorPos({gap, tb + bodyH + gap});
        ImGui::BeginChild("bar", {vp->Size.x - gap * 2, barH}, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        playerBar();
        ImGui::EndChild();
        dialogs();
    }
    ImGui::End();

    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) togglePlay();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_RightArrow)) step(1);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_LeftArrow)) step(-1);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_L)) g_lyricsOpen = !g_lyricsOpen;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q)) { g_view = "queue"; g_lyricsOpen = false; }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_M)) toggleMini();
        float dv = ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_UpArrow) ? 0.05f : ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_DownArrow) ? -0.05f : 0;
        if (dv) { g_set.vol = std::clamp(g_set.vol + dv, 0.f, 1.f); setVolume(); saveSettings(); }
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) { if (g_mini) toggleMini(); g_focusSearch = true; }
}

static void setupStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = {16, 16}; s.FramePadding = {12, 8}; s.ItemSpacing = {10, 8}; s.CellPadding = {10, 8};
    s.ScrollbarRounding = 8; s.ScrollbarSize = 8; s.WindowBorderSize = s.PopupBorderSize = 0; s.ChildBorderSize = 1;
    s.SelectableTextAlign = {0, 0.5f};
    ImVec4* c = s.Colors;
    auto W = [](float a) { return ImVec4{1, 1, 1, a}; };
    c[ImGuiCol_Text] = W(0.95f); c[ImGuiCol_TextDisabled] = W(0.5f);
    c[ImGuiCol_Border] = W(0.12f);  // panneaux en verre dépoli
    c[ImGuiCol_FrameBg] = W(0.09f); c[ImGuiCol_FrameBgHovered] = W(0.13f); c[ImGuiCol_FrameBgActive] = W(0.17f);
    c[ImGuiCol_Button] = W(0.10f); c[ImGuiCol_ButtonHovered] = W(0.18f); c[ImGuiCol_ButtonActive] = W(0.25f);
    c[ImGuiCol_Header] = W(0.12f); c[ImGuiCol_HeaderHovered] = W(0.07f); c[ImGuiCol_HeaderActive] = W(0.15f);
    c[ImGuiCol_TableHeaderBg] = W(0); c[ImGuiCol_TableRowBg] = W(0); c[ImGuiCol_TableRowBgAlt] = W(0);
    c[ImGuiCol_TableBorderLight] = W(0.06f); c[ImGuiCol_TableBorderStrong] = W(0.08f);
    c[ImGuiCol_ScrollbarBg] = W(0); c[ImGuiCol_ScrollbarGrab] = W(0.15f); c[ImGuiCol_ScrollbarGrabHovered] = W(0.25f); c[ImGuiCol_ScrollbarGrabActive] = W(0.35f);
    c[ImGuiCol_NavCursor] = W(0); c[ImGuiCol_Separator] = W(0.08f);
    c[ImGuiCol_SliderGrabActive] = W(1);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4{0, 0, 0, 0.5f};
}

// ---------- ce que le système appelle (win.cpp / mac.mm) ----------
void app::init() {
    std::error_code ec;
    fs::create_directories(g_dir / "cache", ec);
    g_wall = plat::wallpaper();
    loadSettings();
    g_accLive = {g_set.accent[0], g_set.accent[1], g_set.accent[2], 1};
    plat::audioVolume(g_set.vol * g_set.vol);
    loadSaved();
    for (auto& l : g_lists) if (isApple(l.uri) || isSoundcloud(l.uri)) requestTracks(l);  // pas besoin du compte Spotify
    updateYtdlp();
    startEngine();
}

void app::initUI() {
    ImFont* regular;
    plat::loadFonts(&regular, &g_bold, &g_icons);
    setupStyle();
    ImGui::GetStyle().ScaleAllSizes(S); ImGui::GetStyle().FontScaleDpi = S;
    applyStyle();
    for (int i = 0; i < 2; i++) std::thread(texWorker).detach();
    makeGlowTexture();
    plat::setMediaKeys(g_set.mediaKeys);
    setTopmost();
    if (!g_set.logo.empty()) applyLogo();
    {  // période d'Halloween (15 oct. - 1er nov.) : le thème se met tout seul, une fois par an (on peut en changer)
        std::time_t tt = std::time(nullptr);
        std::tm d = *std::localtime(&tt);
        if (((d.tm_mon == 9 && d.tm_mday >= 15) || (d.tm_mon == 10 && d.tm_mday == 1)) && g_set.halloween != d.tm_year + 1900) {
            g_set.halloween = d.tm_year + 1900;
            applyTheme(kThemes[std::size(kThemes) - 1]);
            saveSettings();
        }
    }
    if (GLASS_VERSION > 0 && g_set.seenVersion != GLASS_VERSION) {  // nouvelle version installée : on montre ce qui a changé
        if (g_set.seenVersion > 0) showNews(GLASS_VERSION);
        g_set.seenVersion = GLASS_VERSION;
        saveSettings();
    }
}

int app::timeoutMs(bool visible, bool front) {
    std::lock_guard lk(g_mx);
    if (visible && g_anim) return 15;     // animations : fluide, plafonné à ~60 images/s
    if (visible && g_animBg) return 33;   // fond animé, particules : ~30 images/s
    if (isPlaying()) return !visible ? 1000 : g_lyricsOpen || (g_mini && g_miniLyrics) ? 80 : front ? 500 : 1000;  // progression, fin de titre
    if (g_sleepAt > 0) return 1000;       // minuterie
    return -1;
}

bool app::tick() {
    int pick;
    {
        std::lock_guard lk(g_mx);
        uploadTextures();
        if (!g_ready.empty()) { audioPlay(ustr(g_ready)); g_ready.clear(); g_status.clear(); }
        if (!g_sp && g_has && !g_loading && !g_failed && plat::audioFailed()) { g_failed = true; g_status = "Impossible de lire ce titre"; }
        if (!g_sp && g_has && !g_loading && !g_failed && plat::audioEnded()) onTrackEnd();
        if (g_spEnded) { g_spEnded = false; onTrackEnd(); }
        if (g_sleepAt > 0 && now() >= g_sleepAt) { g_sleepAt = 0; pausePlayback(); g_status = "Minuterie : lecture arrêtée"; }
        if (g_webOk && g_set.source != 2 && !g_wvStarted) startWebView();  // lecteur officiel seulement s'il sert
        if (g_wvStarted && g_set.source == 2) stopWebView();
        if (g_set.apple && !g_amStarted) startApple();
        if (g_amStarted && !g_set.apple) stopApple();
        for (auto& [id, m] : g_wvOut) plat::webPost(id, m);
        g_wvOut.clear();
        if (g_has) plat::nowPlaying(g_cur.title, g_cur.artist, isPlaying());
        if (now() - g_updT > 6 * 3600) { g_updT = now(); checkUpdate(); }
        pick = g_pick; g_pick = 0;
    }
    if (!pick) return false;
    auto paths = plat::pick(pick);  // boîte modale : hors du verrou
    if (pick == 4) {  // logo
        std::lock_guard lk(g_mx);
        if (!paths.empty()) { g_set.logo = ustr(paths[0]); saveSettings(); applyLogo(); }
    } else if (pick == 3) {
        std::lock_guard lk(g_mx);
        if (!paths.empty()) if (List* l = findList(g_pickUri)) {
            l->image = ustr(paths[0]);
            if (l->own) saveMine(); else { g_over[l->uri].second = l->image; saveOverrides(); }
        }
    } else if (!paths.empty()) addLocal(paths);
    return true;
}

void app::frame() {
    std::lock_guard lk(g_mx);
    g_anim = false;
    drawUI();
}

void app::dropped(const std::vector<fs::path>& paths) {
    addLocal(paths);
    std::lock_guard lk(g_mx);
    g_view = "local"; g_lyricsOpen = false;
}

void app::onWeb(int id, const std::string& msg) {
    json j = json::parse(msg, nullptr, false);
    if (!j.is_object()) return;
    std::lock_guard lk(g_mx);
    if (id == 1) onApple(j); else onPlayer(j);
}

void app::media(int cmd) {
    std::lock_guard lk(g_mx);
    if (cmd == 1) togglePlay(); else if (cmd == 2) step(1); else if (cmd == 3) step(-1);
}

bool app::caption(float x, float y, float w, float) {
    if (g_mini) return x < 140 * S || y < 8 * S || y > 146 * S;  // pochette, bord haut, paroles = poignée
    if (x >= g_updRect.x && x < g_updRect.x + g_updRect.z && y >= g_updRect.y && y < g_updRect.y + g_updRect.w) return false;
#ifdef __APPLE__
    return y < 42 * S && x > 70 * S && x < w - 46 * S;  // boutons ronds du Mac à gauche, mini-lecteur à droite
#else
    return y < 42 * S && x < w - 4 * 46 * S;
#endif
}

bool app::mini() { return g_mini; }
bool app::trayOn() { return g_set.tray; }
