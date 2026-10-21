GLASS - lecteur de musique leger : ta bibliotheque Spotify, Apple Music + tes fichiers
(Windows 10/11 et macOS 12 ou plus recent)

LANCER
  build\Release\Glass.exe
  (clic droit > Envoyer vers > Bureau (creer un raccourci))
  1er lancement : une page Spotify s'ouvre, connecte-toi (pour tes playlists).

D'OU VIENT LE SON ?
  - Titres Spotify, mode "Spotify officiel" (Reglages) : le lecteur web
    officiel de Spotify, integre et invisible -> la vraie version, Premium.
    A activer une fois : Reglages > Son des titres Spotify > Connecter
    (cree une app sur developer.spotify.com, coche Web API + Web Playback SDK).
  - Sinon : YouTube Music / YouTube (via yt-dlp), plus leger mais parfois
    une autre version du titre.
  - Tes fichiers (mp3, flac, m4a, wav, ogg...) : lus directement.

UTILISER
  - Un clic sur un titre pour le jouer. Clic droit = lire ensuite,
    file d'attente, favoris, ajouter a une playlist, copier le lien...
  - Glisse un titre sur une de tes playlists (a gauche) pour l'y ajouter,
    ou dans ta playlist pour changer l'ordre. Clic sur une colonne = trier.
  - + (en haut a gauche) : nouvelle playlist, ou lien Spotify / Apple Music.
  - Clic droit sur une playlist : epingler en haut / desepingler,
    personnaliser (nom, pochette), dupliquer...
  - Barre du bas : coeur (favoris), micro (paroles), file d'attente,
    ... (minuterie, vitesse, mini-lecteur).
  - Mini-lecteur (Ctrl+M) : volume, favoris, temps, et bouton micro qui
    affiche les paroles en karaoke sous le lecteur. Glisse-le par la pochette.
  - Roue : reglages (9 themes, couleurs, verre, texte, densite, son
    Auto / Spotify / YouTube, fenetre toujours devant, icone pres de
    l'horloge, demarrage avec Windows...).
  - Raccourcis : Espace, Ctrl+fleches, Ctrl+F (recherche), Ctrl+L (paroles),
    Ctrl+Q (file), Ctrl+M (mini-lecteur), touches multimedia.

APPLE MUSIC
  + > colle le lien d'une playlist ou d'un album Apple Music (Partager >
  Copier le lien). Pas besoin de compte : Glass lit la liste des titres sur
  la page publique et le son vient de YouTube (comme le mode YouTube).
  Paroles, favoris, hors ligne : tout marche pareil.

TES MUSIQUES A TOI
  Glisse des fichiers ou dossiers dans la fenetre, ou "Mes musiques" >
  "+ Fichiers" / "+ Dossier". Clic droit sur un titre > Retirer.

PAROLES
  Spotify d'abord, sinon LRCLIB. Si un titre n'en a pas, l'IA (whisper.cpp)
  les ecrit sur ton PC quand tu ouvres les paroles (~10 s ; 1re fois
  ~200 Mo telecharges dans build\Release\whisper). Echoue si la voix est
  noyee (versions tres accelerees). Desactivable dans Reglages > Affichage.

THEMES
  Reglages > Themes : Apple Music, Apple Clear, Neon Glass, Neon Tokyo, Cyber,
  Aurore boreale... + fonds (pochette, fond d'ecran, uni, degrade de la
  pochette, neon, aurore animee), lueurs neon, couleur d'apres la pochette.

PERFS
  Glass ne redessine que quand quelque chose change : 0 % CPU au repos,
  ~70 Mo (+ ~190 Mo pour le lecteur Spotify integre). L'aurore animee et
  les particules coutent ~3 % d'un coeur tant que Glass est visible.

SI CA PLANTE
  Envoie le fichier build\Release\crash.txt.

VERSION MAC
  Fabriquee automatiquement par GitHub : onglet Actions du depot > derniere
  execution reussie > Glass-mac. Sur le Mac : dezipper, glisser Glass dans
  Applications, puis la 1re fois CLIC DROIT > Ouvrir (app non signee par Apple).
  Donnees : ~/Library/Application Support/Glass. yt-dlp s'installe tout seul.
  Lecteur Spotify officiel : peut ne pas marcher sur Mac (protection du son),
  Glass passe alors par YouTube. Raccourcis : Cmd au lieu de Ctrl (sauf
  Cmd+Q = quitter et Cmd+M = reduire, comme toutes les apps Mac).

LOGO
  assets\glass.ico (genere par assets\make_logo.js), integre a Glass.exe via glass.rc ;
  assets\glass-1024.png sert a l'icone Mac.

CODE
  main.cpp = commun (interface, lecture, paroles) ; win.cpp = Windows ;
  mac.mm = Mac ; platform.h = ce qui les relie.
