# Contrats A18 et provenance examinée

État examiné : MyWallpaper HEAD 299548ebcc20770fd73b10bdffd5f05586fbe126,
checkout modifié, Ledger SHA-256
4b206bd87e3414676f7b841f606bf038d18078bc4917961f281e4911a307b162.
AuditProtocol puis DEC-0012/0013/0099/0157/0160 lus avant la conception.
Aucun changement de décision ratifié ici.

Conforme DEC-0012 : 37 définitions, sections comprises, companion indépendant
sans couche graphique Canvas artificielle. A01/A04 possèdent leurs surfaces.
Conforme DEC-0013 : le contrat audio appartient à cet add-on, sans API métier
audio ajoutée au core. La connexion de services entre add-ons reste un gate
explicite restant à qualifier dans MyWallpaper. Conforme DEC-0099 : code original MIT, JSON MIT
avec notice et hash. Conforme DEC-0160/0157 : aucune source/SDK interne moteur,
injection, résolution de symboles privés ni moteur générique supposés.

Le SDK public @mywallpaper/sdk 0.2.0 et docs/native-addons.md sont des références
locales lues, pas un SDK moteur silencieusement remplacé. Leurs empreintes de
snapshot sont dans native/dependencies.lock.json, avec le HEAD de référence.
Le checkout partagé étant modifié, ce HEAD ne prétend pas contenir ces octets.
Le wire process-v2 version 5 est implémenté selon cette documentation publique :
uint32 LE, 2 bits kind / 30 bits length, fragments physiques 1 octet–1 MiB,
ordre single ou start/continue/end. Pas de plafond logique ajouté au protocole.
`ready` précède toute donnée ; `settings`, `message`, `shutdown` sont supportés,
v5 exact obligatoire. EOF et shutdown nettoient les ressources. stdout ne
contient que les frames, erreurs de lecture sur stderr. Pas de protocole agent.

## Réutilisation Music Player

Dépôt lu sans modification : mywallpaper-addon-music-player (checkout de référence),
HEAD 34a3a2222231e45341836eab0ad7e716fcb97cbd. Sources observées :

- native/companion/src/media.rs SHA-256 db0fb8fb44fb3244d597524b6127d5e824c872a9fa0937d2579f1b38b447caf5
- src/model.ts SHA-256 949d47265b2b1e1f9d55d928a07dee5c2abd159231d3e3748ba93a391e69a844

Son vocabulaire `media.command`, `media.state`, `media.result`, `requestId`,
`action` et `value` est repris. Aucune implémentation source n’est copiée.
Les identifiants de session sont locaux à chaque fournisseur et ne doivent
pas être envoyés d’une instance à l’autre. Music Player reste le lecteur visuel,
les favoris de pistes et le spectre ; A18 apporte le mixeur et son companion.
Le contrat n’est pas annoncé comme substitut strict de chaque champ du lecteur.

## Projection de service audio-system v1 adoptée

Identité publique exacte : `{ "contract": "a18/audio-system", "version": 1 }`.
L’alias fournisseur déclaré est `audio`. Le nom court reste audio-system v1 ; le
namespace est requis par le validateur public `isServiceContract` du SDK.
La définition appartient à `contracts/audio-system/v1/schema.json` et ses
invariants à `contracts/audio-system/v1/README.md`. Exemples publics synthétiques
dans examples.json. Cette adoption locale est conforme aux contrats métier
délégués aux add-ons par DEC-0012/0013 ; elle ne modifie aucune décision du Ledger.
Ledger revérifié : même SHA-256 que ci-dessus ; baseline A18 examinée
`391514a7dbd248fd7b0ce5b46deaaffdd24fbc1a`.

Cette projection exige corrélation, refus avant dispatch distinct d’une issue
inconnue, capacités par session, cible média épinglée, révocation et budgets
annulables. Elle conserve toutes les actions ci-dessous sans nouveau setter
Windows. Les identifiants restent locaux au fournisseur. Les inventaires non
bornés utilisent une continuation vérifiée seulement s’ils dépassent le message
de 64 KiB. La première phase (44a2e0f) définissait ce contrat sans adaptateur.
Le fournisseur direct est maintenant déclaré et implémenté ; sa route et les
limites du fournisseur sont dans native-provider.md ; son intégration complète
dans MyWallpaper reste à qualifier.
Les payloads companion historiques ci-dessous conservent leurs appelants ;
le fournisseur utilise séparément l’enveloppe générique mywallpaper.services.

## Payloads du companion

Entrée : `message` v5, surface host, payload objet. Sortie : `message` v5,
target `broadcast`, payload objet.

| kind | Champs / résultat |
| --- | --- |
| audio.command | requestId facultatif chaîne ≤256 octets ; action ci-dessous |
| audio.result | requestId, ok, message en cas de refus |
| audio.state | contractVersion 1, endpoints, sessions, errors, restoration, runtime |
| audio.settings | ok après application atomique du snapshot |
| audio.error | rejet de configuration/protocole ; état précédent conservé |
| audio.lifecycle | event stopped, comptages de restauration |
| media.command | mêmes requestId/action/value que Music Player |
| media.result | requestId, ok, message en cas de refus |
| media.state | ready/empty/unavailable, sessions et métadonnées/capacités réelles |

Actions audio : refresh, show ; endpointVolume/sessionVolume avec id et volume
0–100 ; endpointMute/sessionMute avec id et muted booléen. Les IDs sont les
valeurs de l’état courant ; un ID périmé est rejeté. Aucune action de routage.
Les sessions de capture refusent les écritures par application. Les endpoints
capture demandent allowMicrophoneChanges, jamais accordé implicitement.

Actions média : refresh ; session avec value=identifiant ; playPause, previous,
next ; seek avec value=millisecondes absolues dans la plage de la session ;
shuffle avec value booléenne ; repeat avec value none/track/list. Les opérations
asynchrones sont annulées après 1,5 s et rapportées comme échouées ; une annulation
ne prouve pas que le lecteur n’a pas déjà commencé l’action. La capacité est
vérifiée avant l’appel et le booléen de succès Windows est vérifié après.

L’état audio est interrogé toutes les 500–3000 ms. L’activité microphone signifie
« session Core Audio active », sans inférence d’amplitude ou de permission globale.
Les réglages masqués restent dans le snapshot propriétaire. Pas de migration
historique : ce nouveau dépôt ne remplace aucun ancien add-on installé.

Références API primaires :
[Core Audio sessions](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-getsessionenumerator),
[identité des processus](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol2-getprocessid),
[PlaySound](https://learn.microsoft.com/en-us/previous-versions/dd743680(v=vs.85)).
