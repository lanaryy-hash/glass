// glass-engine : bibliothèque Spotify (via librespot, ta session) + paroles, pour Glass (C++).
// + jeton et Web API (app développeur de l utilisateur) pour le lecteur web Spotify intégré à Glass.
// Protocole : une ligne JSON par message. stdin = commandes {"cmd":..}, stdout = événements {"ev":..}.
use std::{path::PathBuf, sync::Arc, time::{Duration, Instant}};

use futures::{stream, StreamExt};
use http::{header::{ACCEPT, CONTENT_TYPE}, HeaderMap, HeaderValue, Method};
use librespot_core::{
    authentication::Credentials, cache::Cache, config::SessionConfig, session::Session, SpotifyUri,
};
use librespot_metadata::{image::ImageSize, Album, Lyrics, Metadata, Playlist, Track};
use librespot_oauth::OAuthClientBuilder;
use librespot_protocol::playlist4_external::SelectedListContent;
use protobuf::Message;
use serde_json::{json, Value};
use tokio::io::{AsyncBufReadExt, BufReader};

const SCOPES: &[&str] = &[
    "streaming", "playlist-read", "playlist-read-private", "playlist-read-collaborative",
    "user-library-read", "user-read-private", "user-read-email",
];

fn emit(v: Value) {
    println!("{v}");
}

fn image_url(covers: &[librespot_metadata::image::Image]) -> String {
    covers.iter().find(|i| i.size == ImageSize::DEFAULT).or(covers.first())
        .map(|i| format!("https://i.scdn.co/image/{}", i.id.to_base16().to_lowercase()))
        .unwrap_or_default()
}

async fn track_json(s: &Session, uri: &SpotifyUri) -> Option<Value> {
    let t = Track::get(s, uri).await.ok()?;
    let artists: Vec<&str> = t.artists.iter().map(|a| a.name.as_str()).collect();
    Some(json!({
        "uri": uri.to_uri(), "title": t.name, "artist": artists.join(", "),
        "album": t.album.name, "ms": t.duration, "image": image_url(&t.album.covers),
    }))
}

// Mini protobuf pour collection2v2 (non compilé dans librespot-protocol).
fn pb_str(out: &mut Vec<u8>, field: u8, s: &str) {
    out.push(field << 3 | 2);
    pb_varint(out, s.len() as u64);
    out.extend_from_slice(s.as_bytes());
}
fn pb_varint(out: &mut Vec<u8>, mut v: u64) {
    while v >= 0x80 { out.push(v as u8 | 0x80); v >>= 7; }
    out.push(v as u8);
}
fn pb_read_varint(b: &[u8], i: &mut usize) -> Option<u64> {
    let (mut v, mut sh) = (0u64, 0);
    loop {
        let c = *b.get(*i)?; *i += 1;
        v |= ((c & 0x7f) as u64) << sh;
        if c < 0x80 { return Some(v); }
        sh += 7;
        if sh > 63 { return None; }
    }
}
// Champs (numéro, valeur) d'un message : varint -> Err(n), bytes -> Ok(slice). Ignore les autres types.
fn pb_fields(b: &[u8]) -> Vec<(u64, Result<&[u8], u64>)> {
    let mut out = vec![];
    let mut i = 0;
    while i < b.len() {
        let Some(tag) = pb_read_varint(b, &mut i) else { break };
        match tag & 7 {
            0 => { let Some(v) = pb_read_varint(b, &mut i) else { break }; out.push((tag >> 3, Err(v))); }
            2 => {
                let Some(n) = pb_read_varint(b, &mut i) else { break };
                let Some(s) = b.get(i..i + n as usize) else { break };
                i += n as usize;
                out.push((tag >> 3, Ok(s)));
            }
            1 => i += 8,
            5 => i += 4,
            _ => break,
        }
    }
    out
}

// Collection (titres likés + albums sauvegardés) : même requête que l'app Spotify, par pages.
async fn collection(s: &Session) -> Result<Vec<SpotifyUri>, String> {
    let mut out = vec![];
    let mut token = String::new();
    let mut headers = HeaderMap::new();
    headers.insert(CONTENT_TYPE, HeaderValue::from_static("application/vnd.collection-v2.spotify.proto"));
    headers.insert(ACCEPT, HeaderValue::from_static("application/vnd.collection-v2.spotify.proto"));
    for _ in 0..200 {
        let mut body = vec![];  // PageRequest { username=1, set=2, pagination_token=3, limit=4 }
        pb_str(&mut body, 1, &s.username());
        pb_str(&mut body, 2, "collection");
        if !token.is_empty() { pb_str(&mut body, 3, &token); }
        body.push(4 << 3);
        pb_varint(&mut body, 300);
        let res = s.spclient().request(&Method::POST, "/collection/v2/paging", Some(headers.clone()), Some(&body)).await.map_err(|e| e.to_string())?;
        token.clear();
        for (f, v) in pb_fields(&res) {  // PageResponse { items=1 (uri=1, is_removed=3), next_page_token=2 }
            match (f, v) {
                (1, Ok(item)) => {
                    let fields = pb_fields(item);
                    let removed = fields.iter().any(|(f, v)| *f == 3 && matches!(v, Err(1)));
                    let uri = fields.iter().find_map(|(f, v)| if *f == 1 { v.ok() } else { None }).map(|u| String::from_utf8_lossy(u).to_string());
                    if let (false, Some(u)) = (removed, uri) {
                        if let Ok(id) = SpotifyUri::from_uri(&u) { out.push(id); }
                    }
                }
                (2, Ok(t)) => token = String::from_utf8_lossy(t).to_string(),
                _ => {}
            }
        }
        if token.is_empty() { break; }
    }
    Ok(out)
}

async fn load_tracks(s: Session, uri: String) {
    let res: Result<(String, Vec<SpotifyUri>), String> = async {
        if uri == "spotify:collection" {
            let all = collection(&s).await?;
            return Ok(("Titres likés".into(), all.into_iter().filter(|u| matches!(u, SpotifyUri::Track { .. })).collect()));
        }
        let id = SpotifyUri::from_uri(&uri).map_err(|e| e.to_string())?;
        match id {
            SpotifyUri::Album { .. } => {
                let a = Album::get(&s, &id).await.map_err(|e| e.to_string())?;
                Ok((a.name.clone(), a.tracks().cloned().collect()))
            }
            _ => {
                let p = Playlist::get(&s, &id).await.map_err(|e| e.to_string())?;
                Ok((p.name().to_string(), p.tracks().filter(|t| matches!(t, SpotifyUri::Track { .. })).cloned().collect()))
            }
        }
    }.await;
    match res {
        Ok((name, ids)) => {
            let items: Vec<Value> = stream::iter(ids).map(|id| { let s = s.clone(); async move { track_json(&s, &id).await } }).buffered(16)
                .filter_map(|v| async { v }).collect().await;
            emit(json!({"ev": "tracks", "uri": uri, "name": name, "items": items}));
        }
        Err(e) => emit(json!({"ev": "tracks", "uri": uri, "error": e, "items": []})),
    }
}

async fn load_library(s: Session) {
    let mut items = vec![json!({"uri": "spotify:collection", "name": "Titres likés", "image": ""})];
    if let Ok(bytes) = s.spclient().get_rootlist(0, Some(500)).await {
        if let Ok(root) = SelectedListContent::parse_from_bytes(&bytes) {
            let uris: Vec<String> = root.contents.items.iter().map(|i| i.uri().to_string())
                .filter(|u| u.starts_with("spotify:playlist:")).collect();
            let lists: Vec<Value> = stream::iter(uris).map(|u| {
                let s = s.clone();
                async move {
                    let id = SpotifyUri::from_uri(&u).ok()?;
                    let p = Playlist::get(&s, &id).await.ok()?;
                    let mut image = if p.attributes.picture.is_empty() { String::new() } else {
                        format!("https://i.scdn.co/image/{}", p.attributes.picture.iter().map(|b| format!("{b:02x}")).collect::<String>())
                    };
                    if image.is_empty() {  // playlists auto : on prend la pochette du 1er titre
                        if let Some(first) = p.tracks().find(|t| matches!(t, SpotifyUri::Track { .. })) {
                            if let Some(t) = track_json(&s, first).await { image = t["image"].as_str().unwrap_or("").into(); }
                        }
                    }
                    Some(json!({"uri": u, "name": p.name(), "image": image, "edit": p.capabilities.can_edit_items}))
                }
            }).buffered(8).filter_map(|v| async { v }).collect().await;
            items.extend(lists);
        }
    }
    let albums: Vec<SpotifyUri> = collection(&s).await.unwrap_or_default().into_iter().filter(|u| matches!(u, SpotifyUri::Album { .. })).collect();
    let albums: Vec<Value> = stream::iter(albums).map(|id| {
        let s = s.clone();
        async move {
            let a = Album::get(&s, &id).await.ok()?;
            Some(json!({"uri": id.to_uri(), "name": a.name, "image": image_url(&a.covers)}))
        }
    }).buffered(8).filter_map(|v| async { v }).collect().await;
    items.extend(albums);
    emit(json!({"ev": "library", "items": items}));
}

fn web_track(t: &Value) -> Value {
    let artists: Vec<&str> = t["artists"].as_array().map(|a| a.iter().filter_map(|x| x["name"].as_str()).collect()).unwrap_or_default();
    let imgs = t["album"]["images"].as_array().cloned().unwrap_or_default();
    let image = imgs.iter().find(|i| i["width"].as_i64().unwrap_or(0) <= 320).or(imgs.first()).and_then(|i| i["url"].as_str()).unwrap_or("");
    json!({"uri": t["uri"], "title": t["name"], "artist": artists.join(", "), "album": t["album"]["name"], "ms": t["duration_ms"], "image": image})
}

fn urlencode(s: &str) -> String {
    s.bytes().map(|b| if b.is_ascii_alphanumeric() || b"-_.~".contains(&b) { (b as char).to_string() } else { format!("%{b:02X}") }).collect()
}

fn parse_lrc(lrc: &str, scale: f64) -> Vec<Value> {
    lrc.lines().filter_map(|l| {  // "[mm:ss.xx] paroles"
        let (tag, w) = l.strip_prefix('[')?.split_once(']')?;
        let (m, sec) = tag.split_once(':')?;
        let t = (m.parse::<f64>().ok()? * 60.0 + sec.parse::<f64>().ok()?) * 1000.0 * scale;
        Some(json!({"t": t as i64, "w": w.trim()}))
    }).collect()
}

// Titre sans " - Slowed & Reverb", "(sped up)"... : les paroles de l'original servent pour ces versions.
fn base_title(t: &str) -> String {
    let t = t.split(" - ").next().unwrap_or(t);
    let mut out = String::new();
    let mut depth = 0;
    for c in t.chars() {
        match c { '(' | '[' => depth += 1, ')' | ']' => depth -= 1, _ if depth <= 0 => out.push(c), _ => {} }
    }
    out.trim().to_string()
}

async fn lrclib(http: &reqwest::Client, path: &str, q: &[(&str, String)]) -> Value {
    let Ok(r) = http.get(format!("https://lrclib.net/api/{path}")).query(q).header("User-Agent", "Glass music player").send().await else { return Value::Null };
    serde_json::from_str(&r.text().await.unwrap_or_default()).unwrap_or(Value::Null)
}

// Paroles : Spotify d'abord (synchronisées), sinon LRCLIB (gratuit, marche aussi pour les fichiers locaux).
async fn lyrics(s: Session, http: reqwest::Client, v: Value) {
    let key = v["key"].clone();
    if let Ok(SpotifyUri::Track { id }) = SpotifyUri::from_uri(v["uri"].as_str().unwrap_or("")) {
        if let Ok(l) = Lyrics::get(&s, &id).await {
            let lines: Vec<Value> = l.lyrics.lines.iter()
                .map(|x| json!({"t": x.start_time_ms.parse::<i64>().unwrap_or(0), "w": x.words})).collect();
            let synced = lines.iter().any(|x| x["t"].as_i64().unwrap_or(0) > 0);
            if !lines.is_empty() { return emit(json!({"ev": "lyrics", "key": key, "synced": synced, "lines": lines, "src": l.lyrics.provider_display_name})); }
        }
    }
    let artist = v["artist"].as_str().unwrap_or("").split(',').next().unwrap_or("").trim().to_string();
    let title = v["title"].as_str().unwrap_or("").to_string();
    let ms = v["ms"].as_f64().unwrap_or(0.0);
    let exact = lrclib(&http, "get", &[("artist_name", artist.clone()), ("track_name", title.clone()), ("duration", ((ms / 1000.0) as i64).to_string())]).await;
    let mut found = vec![exact];
    let base = base_title(&title);
    if !found[0]["syncedLyrics"].is_string() && !found[0]["plainLyrics"].is_string() {
        for q in [vec![("track_name", base.clone()), ("artist_name", artist.clone())], vec![("q", format!("{artist} {base}"))]] {
            let r = lrclib(&http, "search", &q).await;
            found = r.as_array().cloned().unwrap_or_default();
            if !found.is_empty() { break; }
        }
    }
    // Préfère des paroles synchronisées. Version accélérée/ralentie : on étire les temps selon la durée.
    let pick = found.iter().find(|d| d["syncedLyrics"].as_str().is_some_and(|x| !x.is_empty()))
        .or(found.iter().find(|d| d["plainLyrics"].as_str().is_some_and(|x| !x.is_empty())));
    if let Some(d) = pick {
        let dur = d["duration"].as_f64().unwrap_or(0.0) * 1000.0;
        let scale = if dur > 0.0 && ms > 0.0 && ((ms / dur) - 1.0).abs() > 0.04 { ms / dur } else { 1.0 };
        let src = if scale == 1.0 { "LRCLIB".to_string() } else { format!("LRCLIB (version originale, tempo ajusté x{:.2})", scale) };
        if let Some(lrc) = d["syncedLyrics"].as_str().filter(|x| !x.is_empty()) {
            return emit(json!({"ev": "lyrics", "key": key, "synced": true, "lines": parse_lrc(lrc, scale), "src": src}));
        }
        let plain = d["plainLyrics"].as_str().unwrap_or("");
        let lines: Vec<Value> = plain.lines().map(|w| json!({"t": 0, "w": w})).collect();
        return emit(json!({"ev": "lyrics", "key": key, "synced": false, "lines": lines, "src": "LRCLIB"}));
    }
    emit(json!({"ev": "lyrics", "key": key, "synced": false, "lines": []}));
}

// ---------- Web API (app développeur de l'utilisateur) : pilote le lecteur Spotify intégré à Glass ----------
const WEB_REDIRECT: &str = "http://127.0.0.1:5589/callback";
const WEB_SCOPES: &[&str] = &[
    "streaming", "user-read-email", "user-read-private", "user-read-playback-state", "user-modify-playback-state",
    "playlist-modify-public", "playlist-modify-private", "user-library-modify",  // tes playlists et tes titres likés
];

struct Auth { client_id: String, refresh: String, access: String, expires: Instant }

#[derive(Clone)]
struct Web { auth: Arc<tokio::sync::Mutex<Option<Auth>>>, http: reqwest::Client, dir: PathBuf }

impl Web {
    fn save(&self, a: &Auth) {
        let _ = std::fs::write(self.dir.join("webapi.json"), json!({"client_id": a.client_id, "refresh_token": a.refresh}).to_string());
    }

    async fn refresh(&self, cid: &str, rt: &str) -> Result<Auth, String> {
        let c = OAuthClientBuilder::new(cid, WEB_REDIRECT, WEB_SCOPES.to_vec()).build().map_err(|e| e.to_string())?;
        let t = c.refresh_token_async(rt).await.map_err(|e| e.to_string())?;
        let refresh = if t.refresh_token.is_empty() { rt.to_string() } else { t.refresh_token };
        Ok(Auth { client_id: cid.into(), refresh, access: t.access_token, expires: t.expires_at })
    }

    async fn token(&self) -> Result<String, String> {
        let mut g = self.auth.lock().await;
        let (cid, rt) = match g.as_ref() {
            None => return Err("Spotify officiel non configuré".into()),
            Some(a) if a.expires > Instant::now() + Duration::from_secs(60) => return Ok(a.access.clone()),
            Some(a) => (a.client_id.clone(), a.refresh.clone()),
        };
        let n = self.refresh(&cid, &rt).await?;
        self.save(&n);
        let tok = n.access.clone();
        *g = Some(n);
        Ok(tok)
    }

    async fn call(&self, method: &str, path: &str, body: &Value) -> Result<(u16, Value), String> {
        let tok = self.token().await?;
        let m = reqwest::Method::from_bytes(method.as_bytes()).map_err(|e| e.to_string())?;
        let mut req = self.http.request(m, format!("https://api.spotify.com{path}")).bearer_auth(tok);
        req = if body.is_null() { req.header("Content-Length", "0") } else { req.header("Content-Type", "application/json").body(body.to_string()) };
        let r = req.send().await.map_err(|e| e.to_string())?;
        let st = r.status().as_u16();
        let text = r.text().await.unwrap_or_default();
        Ok((st, serde_json::from_str(&text).unwrap_or(Value::Null)))
    }
}

#[tokio::main(flavor = "current_thread")]  // un seul thread : le moteur dort quand il n'a rien à faire
async fn main() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("off")).target(env_logger::Target::Stderr).init();
    // Données à côté de l'exe (Windows) ou dans le dossier donné par Glass (Mac : l'app elle-même est en lecture seule)
    let data = std::env::var_os("GLASS_DATA").map(PathBuf::from).unwrap_or_else(|| std::env::current_exe().unwrap().parent().unwrap().to_path_buf());
    let dir: PathBuf = data.join("spotify-cache");
    let cache = Cache::new(Some(&dir), None, None, None).ok();
    let config = SessionConfig::default();

    let mut creds = cache.as_ref().and_then(Cache::credentials);
    let session = loop {
        let c = match creds.take() {
            Some(c) => c,
            None => {
                emit(json!({"ev": "status", "msg": "Connecte-toi à Spotify dans la page qui vient de s'ouvrir"}));
                let client = OAuthClientBuilder::new(&config.client_id, "http://127.0.0.1:5588/login", SCOPES.to_vec())
                    .open_in_browser()
                    .with_custom_message("Connecté ! Tu peux fermer cette page et retourner sur Glass.")
                    .build();
                match client {
                    Ok(c) => match c.get_access_token_async().await {
                        Ok(t) => Credentials::with_access_token(t.access_token),
                        Err(e) => { emit(json!({"ev": "error", "msg": format!("Connexion refusée : {e}")})); return; }
                    },
                    Err(e) => { emit(json!({"ev": "error", "msg": e.to_string()})); return; }
                }
            }
        };
        emit(json!({"ev": "status", "msg": "Connexion à Spotify..."}));
        let s = Session::new(config.clone(), cache.clone());
        match s.connect(c, true).await {
            Ok(()) => break s,
            Err(e) => emit(json!({"ev": "status", "msg": format!("Session expirée ({e}), reconnexion...")})),
        }
    };
    emit(json!({"ev": "ready", "user": session.username()}));
    tokio::spawn(load_library(session.clone()));

    let http = reqwest::Client::new();
    let web = Web { auth: Default::default(), http: http.clone(), dir: dir.clone() };
    {
        let web = web.clone();
        tokio::spawn(async move {
            let saved = std::fs::read_to_string(web.dir.join("webapi.json")).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok());
            match saved {
                Some(v) => match web.refresh(v["client_id"].as_str().unwrap_or(""), v["refresh_token"].as_str().unwrap_or("")).await {
                    Ok(a) => { web.save(&a); *web.auth.lock().await = Some(a); emit(json!({"ev": "web", "ok": true})); }
                    Err(e) => emit(json!({"ev": "web", "ok": false, "msg": format!("Reconnexion Spotify : {e}")})),
                },
                None => emit(json!({"ev": "web", "ok": false})),
            }
        });
    }

    let mut lines = BufReader::new(tokio::io::stdin()).lines();
    while let Ok(Some(line)) = lines.next_line().await {
        let Ok(v) = serde_json::from_str::<Value>(&line) else { continue };
        let s = |k: &str| v[k].as_str().unwrap_or("").to_string();
        match v["cmd"].as_str().unwrap_or("") {
            "library" => { tokio::spawn(load_library(session.clone())); }
            "tracks" => { tokio::spawn(load_tracks(session.clone(), s("uri"))); }
            "lyrics" => { tokio::spawn(lyrics(session.clone(), http.clone(), v)); }
            "search" => {  // catalogue Spotify (Web API, app développeur)
                let (web, q) = (web.clone(), s("q"));
                tokio::spawn(async move {
                    let items: Vec<Value> = match web.call("GET", &format!("/v1/search?type=track&limit=10&q={}", urlencode(&q)), &Value::Null).await {
                        Ok((200, d)) => d["tracks"]["items"].as_array().cloned().unwrap_or_default().iter().map(web_track).collect(),
                        _ => vec![],
                    };
                    emit(json!({"ev": "search", "q": q, "items": items}));
                });
            }
            "token" => {  // jeton pour le lecteur web Spotify intégré
                let web = web.clone();
                tokio::spawn(async move {
                    match web.token().await {
                        Ok(t) => emit(json!({"ev": "token", "token": t})),
                        Err(e) => emit(json!({"ev": "status", "msg": e})),
                    }
                });
            }
            "api" => {  // lancer un titre, aléatoire, répétition...
                let (web, method, path, body, id) = (web.clone(), s("method"), s("path"), v["body"].clone(), v["id"].clone());
                tokio::spawn(async move {
                    let (st, msg) = match web.call(&method, &path, &body).await {
                        Ok((st, d)) => (st, d["error"]["message"].as_str().unwrap_or("").to_string()),
                        Err(e) => (0, e),
                    };
                    emit(json!({"ev": "api", "id": id, "status": st, "msg": msg}));
                });
            }
            "get" => {  // lecture dans la Web API (artistes, albums...) : la réponse complète revient à Glass
                let (web, path, id) = (web.clone(), s("path"), v["id"].clone());
                tokio::spawn(async move {
                    match web.call("GET", &path, &Value::Null).await {
                        Ok((st, d)) => emit(json!({"ev": "get", "id": id, "status": st, "data": d})),
                        Err(e) => emit(json!({"ev": "get", "id": id, "status": 0, "msg": e})),
                    }
                });
            }
            "webauth" => {
                let (web, mut cid) = (web.clone(), s("client_id"));
                tokio::spawn(async move {
                    if cid.is_empty() {  // reconnexion (nouvelles autorisations) avec l'app déjà enregistrée
                        cid = web.auth.lock().await.as_ref().map(|a| a.client_id.clone()).unwrap_or_default();
                    }
                    let r: Result<Auth, String> = async {
                        let c = OAuthClientBuilder::new(&cid, WEB_REDIRECT, WEB_SCOPES.to_vec())
                            .open_in_browser()
                            .with_custom_message("Glass est connecté à Spotify ! Tu peux fermer cette page.")
                            .build().map_err(|e| e.to_string())?;
                        let t = c.get_access_token_async().await.map_err(|e| e.to_string())?;
                        Ok(Auth { client_id: cid.clone(), refresh: t.refresh_token, access: t.access_token, expires: t.expires_at })
                    }.await;
                    match r {
                        Ok(a) => { web.save(&a); *web.auth.lock().await = Some(a); emit(json!({"ev": "web", "ok": true})); }
                        Err(e) => emit(json!({"ev": "web", "ok": false, "msg": format!("Connexion refusée : {e}")})),
                    }
                });
            }
            _ => {}
        }
    }
    // stdin fermé = Glass s'est fermé
}
