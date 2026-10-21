// Frontière entre Glass (main.cpp, commun) et le système : win.cpp (Windows) ou mac.mm (macOS).
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "imgui.h"

namespace fs = std::filesystem;

extern std::mutex g_mx;   // protège tout l'état partagé avec les threads
extern float S;           // échelle DPI (1 sur Mac : ImGui y travaille en points)
extern fs::path g_dir;    // données (réglages, cache, paroles...)
extern fs::path g_bin;    // programmes (glass-engine, outils fournis)

// Fourni par le système.
namespace plat {
void wake();  // réveille la boucle principale, depuis n'importe quel thread
std::string wallpaper();
std::string httpGet(const std::string& url);
bool download(const std::string& url, const fs::path& to, const std::function<void(int)>& progress);  // progress : %
fs::path findTool(const char* name);  // yt-dlp, ffmpeg, whisper-cli... ; vide si introuvable
bool run(const std::vector<std::string>& args, int timeoutMs);  // caché, priorité basse ; true si code 0
bool engineStart(const fs::path& exe, std::function<void(const std::string&)> onLine, std::function<void()> onExit);
void engineSend(const std::string& line);
void engineStop();
size_t memMB(bool engine);

// son des fichiers locaux et de YouTube
void audioPlay(const fs::path& file);
void audioPause();
void audioResume();
bool audioPaused();
bool audioEnded();
bool audioFailed();
double audioPos();  // secondes
double audioDur();  // secondes, 0 si inconnue
void audioSeek(double s);
void audioVolume(float v);
void audioRate(float r);

// lecteurs officiels dans des pages web invisibles ; leurs messages vont à app::onWeb
// id 0 = Spotify (page locale html), id 1 = Apple Music (url + script injecté dans la page)
void webStart(int id, const std::string& html, const std::string& url, const std::string& inject, float vol);
void webStop(int id);
void webPost(int id, const std::string& json);  // thread de la fenêtre
void webShow(int id, bool show);                // fenêtre visible (connexion au compte) ou cachée

// images RGBA (côté le plus long = size) : http..., fichier, "thumb:<audio>", "icon:"
bool decodeImage(const std::string& src, int size, std::vector<uint8_t>& px, int& w, int& h);
ImTextureID makeTexture(const uint8_t* px, int w, int h);  // thread de la fenêtre
void loadFonts(ImFont** regular, ImFont** bold, ImFont** icons);

void readTags(const fs::path& file, std::string& title, std::string& artist, std::string& album, int& ms);
std::vector<fs::path> pick(int kind);  // 1 fichiers audio, 2 dossier, 3 image

bool maximized();
void minimize();
void toggleMaximize();
void close();
void setTopmost(bool on);
void enterMini(int w, int h);
void leaveMini();
void setMiniHeight(int h);  // grandit vers le haut
bool wallpaperUV(ImVec2& uv0, ImVec2& uv1);  // partie du fond d'écran derrière la fenêtre

void openUrl(const std::string& url);  // lien ou dossier
void setAppIcon(const uint8_t* px, int w, int h);  // icône de la fenêtre / barre des tâches / Dock (nullptr = d'origine)
std::string installUpdate(const fs::path& zip);    // remplace Glass par la version du zip et relance ; "" si OK, sinon l'erreur
void setMediaKeys(bool on);
void setStartup(bool on);
void nowPlaying(const std::string& title, const std::string& artist, bool playing);
}

// Fourni par Glass au système.
namespace app {
void init();      // réglages, bibliothèque, moteur (avant la fenêtre)
void initUI();    // polices, style (après la fenêtre et le rendu)
int timeoutMs(bool visible, bool front);  // délai avant la prochaine image, -1 = rien à faire
bool tick();      // travail de la boucle (lecture, fin de titre...) ; true si une boîte de dialogue a été ouverte
void frame();     // construit l'interface (entre NewFrame et Render)
void dropped(const std::vector<fs::path>& paths);
void onWeb(int id, const std::string& msg);  // msg {"t":"hidden"} : l'utilisateur a fermé la fenêtre de connexion
void media(int cmd);  // 1 lecture/pause, 2 suivant, 3 précédent
bool caption(float x, float y, float w, float h);  // zone qui déplace la fenêtre
bool mini();
bool trayOn();
}
