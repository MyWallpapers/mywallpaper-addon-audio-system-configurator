# Audio média et sons système — A18 0.1.0

Un companion Windows x64 avec mixeur master et par session, périphériques
préférés, commandes média et sons facultatifs. Le panneau Win32 et les 37
définitions de la sidebar MyWallpaper sont implémentés. Ce dépôt publie les sources en cours de qualification. Il ne constitue pas une
release disponible dans MyWallpaper. La validation complète dans l’application
Windows, les personnalisations, l’arrêt, la restauration et la persistance
restent à terminer ; des preuves partielles privées ne valent pas acceptation.

Le panneau sélectionne une **cible de mixage** parmi les sorties et microphones
actifs. Ce choix ne change pas le périphérique Windows par défaut. Un bouton
ouvre les réglages audio Windows pour ce dernier choix. Les volumes et mutes
manuels sont conservés comme dans un mixeur standard. Les microphones sont
verrouillés en écriture par défaut ; leur indicateur compte les sessions actives
sans ouvrir de capture ni lire d’échantillons audio.

Le mixeur d’applications montre les sessions de la sortie sélectionnée, avec
volume et mute. Une application peut posséder plusieurs sessions. Les noms de
périphériques favoris sont des noms affichés, sans identifiant technique à saisir.
Des noms identiques ne permettent pas de distinguer deux favoris persistés ; le
sélecteur du panneau reste utilisable. Un favori absent ou ambigu affiche un
diagnostic et bloque les commandes correspondantes jusqu’à une sélection valide.
Le choix Windows sert de repli seulement sans favori configuré. Une sélection
manuelle disparue bloque également les commandes. Il ne modifie jamais le routage
d’une app.

Les lecteurs qui publient GSMTC fournissent piste, artiste, album, état et
position. Précédent, lecture/pause, suivant et seeking sont activés selon les
capacités de la session. Le contrat natif propose aussi shuffle/repeat conditionnels.
Pas de lecteur audio, pochettes ou spectre dupliqués : Music Player les possède.

Le panneau propose fond, texte et accent libres en RGBA alpha-last (composition
opaque sur la couleur système), taille de texte, largeur, écran principal/du
pointeur, coin/centre, marge et maintien au-dessus. Il utilise les contrôles
Windows et la navigation Tab ; il reste statique et n’ajoute pas d’animation.
DPI, espace de travail et contraste élevé sont traités dans le code, sans recette
visuelle réelle accomplie à ce stade. Une barre de défilement est disponible sur
les petits espaces de travail. Ce panneau appartient à A18, pas au flyout Windows.

La molette du panneau change le master de la sortie choisie. Les raccourcis
globaux sont facultatifs et désactivés par défaut : Ctrl+Alt ou Windows+Alt,
haut/bas pour le volume, M pour mute, A pour afficher le panneau. Les conflits
restent observables. Désactivation/arrêt retirent icône, raccourcis, timer et son.

Une règle facultative limite le volume des sessions de sortie d’un programme
en arrière-plan. Elle rend leur volume initial au retour au premier plan, à la
modification des réglages ou à l’arrêt normal, seulement si le volume observé
correspond encore à sa dernière écriture. Une modification extérieure observée
ou une commande manuelle révoque cette prise en charge pour l’activation courante.
Les groupes de plusieurs processus et les sessions de capture sont exclus.
**Un arrêt forcé peut laisser cette réduction appliquée** : pas de guardian de
récupération livré. La règle est désactivée par défaut et reste à qualifier avec
le focus réel. Les erreurs de restauration sont comptabilisées dans les états.

Les sons optionnels jouent vos WAV locaux (8 MiB max), sans changer le thème
sonore Windows : arrivée d’un périphérique audio, changement secteur, passage
sous le seuil batterie et commande de mute. Un chemin vide désactive le son de
cet événement. Le démarrage et les changements de configuration établissent un
état initial silencieux. Batterie/secteur dépendent du matériel. Les sons ne
sont pas encore qualifiés par écoute ; toutes les fixtures interdisent la lecture.

## Construire et vérifier

Depuis un terminal Windows dans ce dépôt, avec Visual Studio Build Tools x64,
le SDK Windows et CMake installés :

```powershell
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File native/build.ps1
```

Cette entrée, déclarée dans `mywallpaper.config.json`, compile le companion en
Release, exécute les CTest déclarés et prépare `native/out/windows-x86_64` avec
les licences. La route Release reste à qualifier sur Windows ; aucune archive
binaire ni version centrale n’est publiée par ce dépôt de sources.

Les modèles et contrats peuvent aussi être vérifiés sans lancer le companion :

```sh
cmake -S . -B build
cmake --build build --target model_tests service_provider_tests endpoint_selection_tests
ctest --test-dir build --output-on-failure -R '^(audio_contract|native_service_provider|endpoint_selection)$'
```

CMake déclare les cibles et CTest ; `CMakePresets.json` décrit les capacités du
dépôt. Aucun SDK moteur n’est lié. nlohmann/json 3.12.0 MIT est embarqué et
verrouillé par SHA-256. `native/stage-development.mjs` est un outil DEV facultatif
qui demande un checkout MyWallpaper voisin et un reçu local explicite. Il
n’intervient pas dans le build Release autonome ci-dessus.

## Contrats et qualification

Voir les [contrats](docs/contracts.md), le [fournisseur natif](docs/native-provider.md)
et le [contrat audio-system v1](contracts/audio-system/v1/README.md).
Le contrat de service et les fonctionnalités implémentées ne prouvent pas leur
activation dans MyWallpaper ni leur conformité sur tous les périphériques.
La publication centrale, la disponibilité dans le catalogue et l’acceptation
Windows restent des étapes distinctes de la publication de ces sources.
