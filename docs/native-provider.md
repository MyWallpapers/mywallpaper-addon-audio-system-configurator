# Implémentation du fournisseur natif A18

Cette tranche implémente le fournisseur direct audio-system v1 et ses preuves
locales. Le contrat public adopté au commit 44a2e0f n’est pas modifié. Le Ledger
reste l’autorité ; ce document décrit du code et des preuves, sans décision
produit parallèle. Référence Ledger :
4b206bd87e3414676f7b841f606bf038d18078bc4917961f281e4911a307b162.

## Route retenue et périmètre

Provider est une bibliothèque C++ portable ; Panel la relie aux API Windows
existantes. Le manifeste déclare seulement l’alias audio et sa version. Aucun
wrapper JS, runtime HTML, API privée A24 ou spécialisation audio du broker.
Le broker vérifié est ee14c19603f63accafc8a258374de349c28d4ca96c5733f49bed917b8c0c0d5d.

Les lectures snapshot/inventory servent les derniers caches immuables, sans
interrogation Windows à chaque RPC. Tick produit les observations réelles ;
les états désactivés, vides et en erreur restent distincts. Les capacités
audio suivent les permissions de capture ; les capacités des lecteurs non
sélectionnés restent null. Chaque valeur conserve ses métadonnées complètes.

Chaque commande vérifie schéma, contexte host, génération/révision, ID et
capacité. L’identité anti-rejeu est (connectionId, requestId), retenue jusqu’à
fermeture de génération sans éviction ; saturation des 16 MiB de clés refuse
busy. Les deux valeurs d’inventaire ont séparément un budget de 16 MiB de
JSON sérialisé courant. Ce sont des limites de données, pas une mesure RSS.
Au-delà de 64 KiB de frame complète, les pages utilisent au plus 32768 octets
bruts, base64 UTF-8/JSON, longueur/checksum et invalidation immédiate sur nouvelle
révision. Le fournisseur ne crée ni copie ni historique par lecteur. Le
consommateur doit imposer une page en vol, 5 s et 512 pages d’assemblage.

Prepare fixe les échéances dès lecture du pipe, hors thread UI ; seuls les
contextes mutex/atomiques sont partagés. Le reader continue à lire cancel/EOF
pendant Await. La file UI est limitée à 32 frames. Dispatch marque le passage
à une possible action ; toute erreur/cancel/timeout suivant ce passage donne
unknown. Une réussite reste acknowledged, sans prétendre observer un effet
extérieur. Une session média explicitement épinglée ne retombe jamais sur le
lecteur par défaut. Les exceptions WinRT deviennent des résultats structurés.
Un échec d’écriture service révoque le fournisseur.

## Preuve locale

Le plan natif sélectionne la recette existante. Les commandes mesurées ont un
périmètre explicite : configure, build du nouveau target, CTest limité à
native_service_provider, puis validation Python de sa transcription réelle.
La compilation Windows vise companion et service_provider_tests uniquement.
L’exécutable portable ne lie pas audio_windows : aucun appel COM, Win32, Core
Audio, GSMTC, fenêtre, session, son ou activation du companion.

Les cas couvrent les frames canoniques, l’ACK distinct, la projection/capacité,
les révisions et états indisponibles, la corrélation, l’isolement par connexion,
les doublons et IDs expirés, l’identité injectée dans payload, les délais
incluant la file, cancel avant/après dispatch et révocation EOF. Ils exercent
une continuation réelle de plusieurs pages avec Unicode, remplacement périmé,
refus de budget et trois vecteurs SHA-256 connus. Le contrôle indépendant
JSON Schema 2020-12 valide les sorties C++, décode les pages, vérifie leur
longueur et checksum, puis valide l’inventaire entier sans perte de sessions.

Les reçus provider-*.json sont des commandes mesurées partielles. Les essais
de compilation échoués sont conservés ; seuls les reçus finaux réussis et le
handoff bornent le résultat. Les anciens tests audio_contract et fixtures
Windows ne sont pas rejoués pour cette tranche ; le test de schéma de phase 1
inchangé n’est pas relancé. Aucune recette native complète n’est revendiquée.

Une fixture Windows distincte hidden_native_service_provider lance seulement
le companion --private-fixture, CREATE_NO_WINDOW, sur des pipes possédés. Elle
vérifie HWND/PID exacts, fenêtre invisible, zéro tray/hotkey/son même si demandés,
capacités toutes non modifiables, média absent, séquence provide puis les deux
publications initiales, snapshot réel et commande show refusée sans exécuteur.
Elle termine normalement ; aucun ancien scénario n’est rejoué. Cette preuve
lit les endpoints, sans session audio créée, setters, playback ou UI visible.
Résultat local : fixture passée, huit enveloppes réelles validées par schéma
et SDK, trois publications, frame maximale 1383 octets. Le binaire companion
reste identique à la compilation du commit 20f7ac8 ; seul le nouveau target
de fixture est ajouté après ce commit.

Ressources envisagées pour la recette normale : un seul worker service:<id>
(racine non exécutée en double selon le gel root), un HWND pour la pompe Win32,
énumération Core Audio et GSMTC si activé, timer. Tray, panneau visible, hotkeys,
règle de premier plan et sons dépendent des options utilisateur ; aucun n’est
nécessaire à snapshot/inventory et ils restent à qualifier dans un slot dédié.

La revue root
confirme provide puis publish, avec snapshot après abonnement ; le défaut de
barrière initialement supposé n’est pas établi. Aucun runtime natif normal n’a été activé ; ni
l’intégration A18/A24, ni famille ou release ne sont acceptées par cette
preuve. L’ancien candidat DEV ne couvre pas ces sources.
