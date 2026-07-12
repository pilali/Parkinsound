# Étude de faisabilité — divisions pointées/triolets et adaptation à la signature rythmique

Cette étude couvre les deux plug-ins (**Step Gate** et **Step Gate 4**), dans
toutes leurs variantes : LV2 (desktop, MOD/mod-host, Carla) et
multiplateforme JUCE (VST3 / AU / Standalone), ainsi que leurs interfaces
(modgui et éditeur JUCE).

**Verdict global : les deux évolutions sont faisables sans casser la
compatibilité (presets, pédalboards MOD, sessions DAW), à condition de
respecter deux contraintes : n'ajouter des ports LV2 qu'en fin de liste, et
ne jamais réordonner les valeurs de l'énumération `division` existante.**

- Volet A (pointées / triolets) : **effort faible à moyen** — la logique est
  centralisée dans une seule table, le reste est de la plomberie
  (ports, paramètres, UI).
- Volet B (signature rythmique) : **effort moyen à important** — la donnée
  est disponible chez tous les hôtes visés, mais elle soulève de vraies
  questions de design (que signifie « s'adapter » pour un séquenceur 16 pas ?)
  et corrige au passage un bug latent de normalisation du beat.

---

## 1. État des lieux : où vit la logique rythmique

| Couche | Fichier | Rôle rythmique actuel |
|---|---|---|
| Cœur DSP partagé | `src/stepgate_dsp.c` | `div_factor[6] = {4, 2, 1, 0.5, 0.25, 0.125}` (longueur de pas en noires), position = `host_beat / step_in_beats` modulo 16 |
| Wrapper LV2 mono | `parkinsound-stepgate.lv2/stepgate.c` | lit `time:beat`, `time:beatsPerMinute`, `time:speed`, `time:frame` — **pas** `beatsPerBar` ni `beatUnit` |
| Step Gate 4 | `parkinsound-stepgate4.lv2/stepgate4.c` | copie autonome du même moteur (table `div_factor` dupliquée), master beat partagé par les 4 canaux |
| Descripteurs LV2 | `stepgate.ttl`, `gen_ttl.py` → `stepgate4.ttl` | port `division` : énumération 0–5, scalePoints 1/1 … 1/32 |
| JUCE processor | `juce/PluginProcessor.cpp` | `AudioParameterChoice "division"` (6 choix), lit `getBpm()` + `getPpqPosition()` — **pas** `getTimeSignature()` |
| Éditeur JUCE | `juce/PluginEditor.cpp` | anneau 16 secteurs codé en dur, `divisionBox` 6 entrées |
| modgui mono | `modgui/javascript-parkinsound-stepgate.js` | pas de contrôle de division (géré par le panneau de réglages mod-ui) ; anneau 16 secteurs codé en dur |
| modgui 4 canaux | `modgui/javascript-parkinsound-stepgate4.js` | `DIV_LABELS = ['1/1'…'1/32']`, sélecteur cyclique par canal ; 16 pas codés en dur |
| Presets | `*.ttl` (`pset:value`) | stockent l'**indice** de division (ex. `Polyrhythm.ttl` : 1, 2, 3, 4) |
| Tests | `test/divcheck.c`, `test/sync4.c`, … | vérifient le ratio ×2 entre divisions adjacentes |

Deux observations structurantes :

1. **Toute l'arithmétique de division tient dans une table et une ligne de
   calcul** — l'extension musicale elle-même est triviale. Le coût est dans
   la propagation (ports, paramètres, presets, 2 modguis, éditeur JUCE,
   générateur TTL, tests).
2. **Aucune couche ne connaît la mesure.** Le motif tourne sur un modulo 16
   pas appliqué au beat absolu : en 5/4 ou 7/4, le pas 1 dérive par rapport
   au premier temps de la mesure. « S'adapter à la signature » implique donc
   d'introduire deux notions nouvelles dans le cœur : *l'alignement sur la
   mesure* et *la longueur effective du motif*.

---

## 2. Volet A — Divisions pointées et triolets

### 2.1 Design recommandé : un port « modificateur » séparé

Plutôt qu'étendre l'énumération `division` (voir §2.3), ajouter un second
port/paramètre par voix :

```
div_mod : 0 = droite (×1)   1 = pointée (×1.5)   2 = triolet (×2/3)
```

Dans le cœur DSP, le changement se réduit à :

```c
static const double div_factor[6] = { 4.0, 2.0, 1.0, 0.5, 0.25, 0.125 };
static const double mod_factor[3] = { 1.0, 1.5, 2.0 / 3.0 };
const double step_in_beats = div_factor[div] * mod_factor[mod];
```

Le mode Host Sync reste automatiquement verrouillé en phase : la position
est recalculée à chaque échantillon depuis le beat absolu de l'hôte, donc
deux instances en 1/8T restent synchrones entre elles, exactement comme
aujourd'hui.

Avantages :

- **Compatibilité totale** : `division` garde ses 6 valeurs et son ordre ;
  tous les presets et pédalboards existants restent valides ; `div_mod`
  vaut 0 par défaut → comportement bit-identique à l'existant.
- **UI compacte** : un cycleur à 3 états (`·` / `T` / rien) à côté du
  sélecteur de division, au lieu d'une liste de 16+ entrées.
- **Step Gate 4** : un `chN_div_mod` par canal permet p. ex. un canal en
  1/8 droit contre un canal en 1/8T — polyrythmies 3:2 immédiates.

### 2.2 Impacts fichier par fichier

| Fichier | Modification |
|---|---|
| `src/stepgate_dsp.h/.c` | champ `division_mod` dans `StepGateParams`, table `mod_factor`, clamp 0–2 |
| `stepgate.c` | 1 port en **fin de liste** (index 46), mapping vers le cœur |
| `stepgate.ttl` | port `div_mod` (enum, 3 scalePoints), `lv2:minorVersion` incrémenté |
| `stepgate4.c` | 4 ports appendus **après** l'index 163 (ne pas les insérer dans les blocs par canal — voir §5.1) |
| `gen_ttl.py` | génération des 4 ports appendus |
| `PluginProcessor.cpp` | `AudioParameterChoice "div_mod"` (« Straight / Dotted / Triplet ») — l'APVTS tolère les sessions anciennes (valeur par défaut) |
| `PluginEditor.cpp` | petite ComboBox ou bouton cyclique à côté de `divisionBox` |
| modgui stepgate4 JS | badge cyclique dans la gouttière de canal (`1/8`, `1/8·`, `1/8T`), gestion de `chN_div_mod` dans start/change |
| modgui stepgate mono | rien d'obligatoire : mod-ui expose automatiquement tout nouveau port de contrôle dans son panneau de réglages (comme `division` aujourd'hui) |
| `test/divcheck.c` | cas supplémentaires : ratios attendus ×1.5 et ×2/3 |

### 2.3 Alternative écartée : étendre l'énumération `division`

Une liste unique (`1/1, 1/2, 1/2., 1/2T, 1/4, …`) casserait la valeur des
presets existants (l'indice 4 ne signifierait plus 1/16) sauf à *appendre*
les nouvelles valeurs après l'indice 5 — ce qui donnerait un menu dans le
désordre musical dans tous les hôtes (l'ordre d'affichage suit l'indice,
tant en JUCE `AudioParameterChoice` que dans mod-ui). À réserver au cas où
un port supplémentaire serait rédhibitoire ; ce n'est pas le cas ici.

### 2.4 Remarque musicale

Avec des triolets, 16 pas de 1/8T couvrent 16/3 ≈ 5,33 noires : le motif ne
retombe plus sur la mesure en 4/4 — c'est inhérent, pas un bug. Le volet B
(mode « aligné mesure ») offre précisément la réponse propre à ce problème.

---

## 3. Volet B — Adaptation à la signature rythmique de l'hôte

### 3.1 Ce que fournissent les hôtes

| Hôte | Numérateur / dénominateur | Position de mesure |
|---|---|---|
| LV2 (Ardour, Carla, …) | `time:beatsPerBar` (float), `time:beatUnit` (int) dans `time:Position` | `time:bar`, `time:barBeat` |
| mod-host / MOD | oui — le réglage global « Beats Per Bar » du device est transmis via `time:beatsPerBar` | `time:barBeat` émis (quantifié comme `time:beat`) |
| JUCE (VST3/AU) | `PositionInfo::getTimeSignature()` (num/denom) | `getPpqPositionOfLastBarStart()` |
| Standalone JUCE, hôtes muets | absent | absent → **fallback manuel requis** |

La donnée est donc disponible partout où le plug-in est déployé, avec un
fallback nécessaire (deux ports manuels + mode Auto, sur le modèle du couple
`sync_source`/`tempo` existant).

### 3.2 Correction préalable : normalisation du beat (bug latent)

Le cœur suppose aujourd'hui que `time:beat` et le BPM sont exprimés en
noires. La spec LV2 les exprime en **unités de `beatUnit`** : dans un projet
en 6/8 sous Ardour, le plug-in compte actuellement des croches en croyant
compter des noires (motif 2× trop rapide), et le chemin `time:frame` mélange
les deux référentiels. Côté JUCE, `getPpqPosition()` est déjà en noires —
les deux wrappers ne parlent donc pas la même langue au cœur.

Toute implémentation du volet B doit commencer par normaliser :

```
beat_quarters = time:beat × 4 / beatUnit
bpm_quarters  = time:beatsPerMinute × 4 / beatUnit
```

(côté JUCE : aucune conversion, ppq déjà en noires). C'est une correction
utile même sans le reste du volet B.

### 3.3 Que signifie « s'adapter » pour un séquenceur 16 pas ?

Trois comportements complémentaires, à exposer comme un mode :

**a) Longueur de motif adaptative (« 1 mesure »).**
Le nombre de pas actifs devient
`active_steps = beatsPerBar × (4/beatUnit) / step_in_beats`, plafonné à 16 :

| Signature | Division | Pas actifs |
|---|---|---|
| 4/4 | 1/16 | 16 (comportement actuel) |
| 3/4 | 1/16 | 12 |
| 6/8 | 1/16 | 12 |
| 5/4 | 1/8 | 10 |
| 7/4 | 1/8 | 14 |
| 5/4 | 1/4 | 5 |

Le modulo passe de `% 16` à `% active_steps`, et les résultats non entiers
(divisions pointées/triolets vs mesure) sont arrondis avec un comportement
documenté (arrondi au plus proche, minimum 1).

**b) Alignement sur la mesure.**
La position du motif se calcule depuis le début de mesure
(`time:bar`/`time:barBeat` en LV2, `getPpqPositionOfLastBarStart()` en JUCE)
et non plus depuis le beat absolu : le pas 1 tombe toujours sur le premier
temps, y compris après un changement de signature en cours de morceau ou un
saut de boucle dans la DAW. Entre deux événements de transport, l'intégration
par échantillon existante continue de piloter la phase (même mécanique que
l'anti-gel `prev_received_beat` actuel).

**c) Cas `active_steps > 16`.**
7/4 en 1/16 = 28 pas > 16. Trois options ; recommandation : **le motif de 16
pas s'étale sur la mesure et le reliquat boucle** (wrap), avec l'indication
du nombre théorique dans l'UI. Étendre `NUM_STEPS` à 32 est écarté : sur
Step Gate 4 cela ajouterait 128 ports LV2 et doublerait les deux modguis,
pour un bénéfice marginal.

### 3.4 Ports / paramètres nouveaux (par plug-in, partagés sur Step Gate 4)

Tous **appendus en fin de liste**, valeurs par défaut = comportement actuel :

| Port | Type | Rôle |
|---|---|---|
| `pattern_mode` | enum : `16 pas (fixe)` (défaut) / `1 mesure` / `2 mesures` | active l'adaptation ; défaut = comportement identique à aujourd'hui |
| `meter_source` | enum : `Auto (hôte)` (défaut) / `Manuel` | fallback quand l'hôte ne fournit rien (Free Run, Standalone) |
| `meter_num` | int 1–16, défaut 4 | numérateur manuel |
| `meter_denom` | enum 1/2/4/8/16, défaut 4 | dénominateur manuel |
| `active_steps` | **sortie** int 1–16 | nombre de pas effectif calculé par le DSP — c'est le canal d'information vers les UIs |

Le port de sortie `active_steps` est la clé côté interfaces : le modgui n'a
**aucun accès au transport de l'hôte** ; il ne peut réagir à la signature que
si le plug-in la lui republie via un port monitoré (`modgui:monitoredOutputs`,
même mécanisme que `current_step` aujourd'hui). Côté JUCE, un simple
`std::atomic<int>` dans le processor (comme `currentStep`).

### 3.5 API du cœur DSP

`stepgate_dsp_update_position()` gagne deux couples
(`have_beats_per_bar`, `beats_per_bar`, `have_beat_unit`, `beat_unit`) — ou,
plus durable, migre vers une struct `StepGatePosition` à drapeaux. C'est une
API interne au dépôt (deux wrappers seulement) : le changement de signature
est libre. `StepGateParams` gagne `pattern_mode`, `meter_source`,
`meter_num`, `meter_denom` ; `stepgate_dsp_process()` retourne en plus
`active_steps` (nouveau paramètre de sortie ou struct de résultat).

**Step Gate 4** doit au passage être rebasé sur le cœur partagé (son moteur
est aujourd'hui une copie divergente dans `stepgate4.c`) — sinon chaque
évolution du volet B sera écrite et testée deux fois. C'est le bon moment :
une instance `StepGateDsp` étendue par canal, ou un cœur multi-voix.

### 3.6 Adaptation des interfaces

**Éditeur JUCE** : l'anneau dessine `active_steps` secteurs (angle
`360/active_steps`) ; les pas au-delà sont grisés/masqués ; un libellé
« 12 pas — 3/4 » sous le sélecteur de division. Le hit-testing
(`hitStepIndex`) paramétré par le nombre de pas. Effort modéré : tout est
déjà dessiné procéduralement.

**modgui mono (anneau SVG)** : les 16 secteurs sont construits une fois
(`data-built`) ; il faut soit reconstruire l'anneau quand `active_steps`
change, soit — plus simple et plus robuste dans mod-ui — **garder 16
secteurs fixes et griser les pas inactifs** (classe CSS `inactive`,
non cliquables). Recommandé : grisage, géométrie inchangée.

**modgui Step Gate 4 (grille)** : même approche, colonne par colonne ;
`active_steps` peut différer par canal (division différente) → une sortie
`chN_active_steps` par canal.

### 3.7 Points de vigilance

- **mod-host quantifie `barBeat`** comme il quantifie `beat` : la même
  mécanique « n'adopter la valeur que si elle a changé » (déjà en place pour
  `prev_received_beat`) doit s'appliquer à la resynchronisation de mesure.
- **`beatsPerBar` est un float** en LV2 (signatures composées possibles) :
  clamp + arrondi défensifs.
- **Changement de signature en cours de lecture** : `active_steps` change de
  valeur → le modulo saute. Comportement défini : recalage immédiat sur la
  nouvelle grille au prochain début de mesure.
- **Free Run** : pas de transport → la signature manuelle (`meter_num/denom`)
  fait foi ; `meter_source=Auto` sans donnée hôte retombe sur 4/4.

---

## 4. Compatibilité et règles de non-régression

1. **LV2 interdit de renuméroter les ports d'un plug-in sans changer d'URI.**
   Tous les nouveaux ports sont appendus après le dernier index existant
   (46+ pour Step Gate, 164+ pour Step Gate 4 — surtout ne pas les insérer
   dans les blocs par canal malgré l'élégance du `CH_STRIDE`). Incrémenter
   `lv2:minorVersion`.
2. **Presets** : les fichiers `pset:` existants ne référencent que des ports
   conservés ; les nouveaux ports absents d'un preset prennent leur défaut
   (= comportement actuel). Aucun preset à migrer.
3. **Sessions JUCE** : l'APVTS restaure les paramètres présents et laisse les
   nouveaux à leur défaut ; garder les IDs existants inchangés
   (`kVersionHint` inchangé pour eux).
4. **Défauts conservateurs** : `div_mod=0`, `pattern_mode=16 pas fixe`,
   `meter_source=Auto` ⇒ un utilisateur qui ne touche à rien obtient une
   sortie bit-identique à la version actuelle (vérifiable par test A/B).

---

## 5. Plan de mise en œuvre proposé

| Phase | Contenu | Effort estimé |
|---|---|---|
| **0** | ✅ *Fait* — Rebaser Step Gate 4 sur `src/stepgate_dsp` (`stepgate_dsp_process_multi`), non-régression vérifiée bit-exacte contre l'ancien binaire | ~1 j |
| **1** | ✅ *Fait* — Volet A complet : `div_mod` cœur + 2 wrappers LV2 + TTL/gen_ttl + JUCE + modgui SG4 + tests `divcheck`/`sync4` étendus | 1–2 j |
| **2** | Normalisation `beatUnit` (§3.2) + ingestion `beatsPerBar`/`bar`/`barBeat` (LV2) et `getTimeSignature` (JUCE) dans le cœur | ~1 j |
| **3** | `pattern_mode` + `active_steps` + alignement mesure dans le DSP ; ports manuels de fallback ; tests (3/4, 6/8, 5/4, 7/4, changement en cours de lecture) | 2–3 j |
| **4** | Adaptation des UIs : éditeur JUCE, modgui mono (grisage), modgui SG4 (par canal) ; captures modgui régénérées | 2–3 j |

Chaque phase est livrable indépendamment ; les phases 1 et 2 apportent déjà
une valeur utilisateur nette (nouvelles divisions + lecture correcte en
6/8) sans toucher au format du motif.

### Tests à ajouter

- `divcheck` : ratios ×1.5 (pointée) et ×2/3 (triolet) entre variantes.
- Nouveau `barcheck` : hôte simulé émettant `time:Position` avec
  `beatsPerBar`/`barBeat` variables — vérifier `active_steps`, l'alignement
  du pas 1 sur le premier temps, et la stabilité face au `barBeat` quantifié
  façon mod-host.
- Test A/B de non-régression : défauts nouveaux ⇒ sortie bit-identique à la
  version actuelle sur un corpus de blocs aléatoires.
- `sync4` étendu : deux canaux 1/8 vs 1/8T restent verrouillés sur le master
  beat.
