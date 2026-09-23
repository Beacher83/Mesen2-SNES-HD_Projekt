# Changelog — Mesen2 SNES HD Fork (Emulator C++ Code)

Dieses Changelog dokumentiert Änderungen am Mesen2-Fork (C++ Emulator-Code).
Für Änderungen am DKC2-HD-Tools Viewer siehe `DKC2-HD-Tools/CHANGELOG.md`.
Für die Architektur der Compositing Engine siehe `ARCHITECTURE.md`.

---

## ═══════════════════════════════════════════════════════════════
## HD Compositing Engine (v2)
## ═══════════════════════════════════════════════════════════════
##
## Branch: feature/hd-compositing-engine
## Vorgänger: v0.1-sonderfall-m5.19 (Tag) — M5.1 bis M5.19
##
## Architekturwechsel: Statt pro-Level-Typ Sonderfälle (bg3FogBlend,
## bg1OverlayBlend, colorMathDelta etc.) wird die PPU-Compositing-
## Pipeline (Color Math, Windowing, Brightness) generisch auf HD-
## Auflösung repliziert. Ein Code-Pfad für alle Level-Typen.
## ═══════════════════════════════════════════════════════════════

---

## [2026-09-23] — S48: Framebuffer-Dump, weil ein Fensterfoto kein Messmittel ist

**Der Anlass.** Ein halber Tag Messarbeit am Hintergrund von Barrel Bayou hat nichts ergeben,
weil das einzige verfügbare Bild ein **Fensterfoto** war: 1071 px für 256 native Pixel,
Massstab 4,1836. Diese krumme Skalierung verwischt genau das native Pixelraster — und das ist
das einzige Signal, an dem sich nativ und HD unterscheiden lassen. Ich habe darauf zwei
Analysen aufgebaut und dem User beide Male widersprochen; die erste („Korrelation 0,997“) stand
auf Kacheln mit einer Trennschärfe von 0,57 von 255 und war damit Rauschen.

Mesens eigene Screenshot-Taste (F12) tut das Richtige — `BaseVideoFilter::TakeScreenshot`
kopiert den Ausgabepuffer des Videofilters —, wird auf dem Testrechner aber abgefangen, bevor
Mesen sie sieht. Die Verknüpfung selbst ist in Ordnung (Tastencode 101 = F12,
`KeyDefinitions.h:113`); dass `Mesen2\Screenshots` nie angelegt wurde, belegt, dass sie nie
ausgelöst hat.

**S48 braucht keine Taste.** Der Dump läuft im Filter, direkt hinter `RunFrame` — dort sind
die Render-Threads eingesammelt und der Puffer wird nicht mehr angefasst:

| Umgebungsvariable | Bedeutung |
|---|---|
| `SNES_HD_DUMP_FRAMES` | Anzahl Bilder (0 / ungesetzt = aus) |
| `SNES_HD_DUMP_GFXSET` | nur dieses Gfxset (ungesetzt = jedes) |
| `SNES_HD_DUMP_EVERY` | Abstand in Frames (Standard 30) |

Nur auf einem echten Levelbild (`ActiveGfxset` gesetzt, `BgPixels > 20000`) — beim Levelwechsel
liefert der Filter reihenweise leere Frames, und ein schwarzes PNG beantwortet keine Frage.
Ziel: `%USERPROFILE%\Downloads\snes_hd_frame_gfxNN_MM.png`, dazu eine `FRAMEDUMP`-Zeile im
Diagnoselog mit Groesse, Gfxset, `bg` und `hdBG3`. Ausserhalb eines Dumps kostet es einen
Integer-Vergleich je Frame.

Die Videoeinstellungen des Testrechners wurden vorher geprüft (`VideoFilter=None`,
`ScanlineIntensity=0`, keine Rotation, Farbregler 0) — es kommt wirklich der rohe Puffer
heraus. Dazu `dump_frames.bat` im Projektwurzelverzeichnis, das die Variablen setzt und Mesen
startet.

---

## [2026-09-23] — S47: Ebenen-Anstrich

`SNES_HD_PAINT_LAYERS=1` färbt jeden BG-Gewinnerpixel flächig ein — BG1 rot, BG2 grün, BG3
blau, BG4 gelb, Sprites unberührt — ganz am Ende der Pixelbehandlung, über HD wie nativ.
Beantwortet die Frage, die kein Zähler beantwortet: **wo auf dem Schirm** liegen die Pixel, die
eine Ebene gewinnt. Gebaut, noch nicht im Spiel getestet.

---

## [2026-09-23] — S46: warum sich ein Pack-Ordner nicht abschalten liess

**`gfxset_38_aus` wurde als gfxset 38 geladen.** `ParseGfxsetDirName`
(`SnesHdPackLoader.cpp:311`) rief `std::stoul(dirName.substr(7), nullptr, 10)` — `stoul`
hält beim ersten Nicht-Ziffern-Zeichen an, und mit `pos = nullptr` sieht niemand den Rest.
Der Ordner wurde also ganz normal eingelesen.

**Der Beleg lag die ganze Zeit in der FRAME-Zeile:** `TileByKey=56515`, im Lauf mit Ordner und
im Lauf mit umbenanntem Ordner **identisch**. Dazu `miss=0` und `hdBG3 ≈ 25.482` in allen 639
Frames des 08:40-Laufs — nach einem Power Cycle, der Ordner nachweislich umbenannt.

**Folge für die Befundlage:** beide Ausschaltversuche (21.09. und 23.09.) waren nie in Kraft.
Die Schlussfolgerung vom 21.09. — „die Kunst wird gefunden, aber nie gezeichnet“ — hatte damit
nie eine Grundlage, und S45b hat genau das gemessen: 100 % gezeichnet auf allen drei Ebenen.

**Fix:** der Teil hinter `gfxset_` muss vollständig aus Ziffern bestehen, sonst wird der Ordner
übergangen. Damit tut ein Suffix das, was jeder davon erwartet. Ein Ordner lässt sich ab jetzt
auch ohne Neubau abschalten, indem man ihn NICHT mit `gfxset_` beginnen lässt.

---

## [2026-09-23] — S45b: Ergebnis. Die Kunst WIRD gezeichnet.

Lauf 08:12, `build=S45b`, Barrel Bayou:

```
BG1: win=1348  hdMatch=1348  -> GEZEICHNET 1348 (100%)  clip=0 noSampler=0 alpha0=0 subOp=0
BG2: win=24427 hdMatch=24427 -> GEZEICHNET 24427 (100%) clip=0 noSampler=0 alpha0=0 subOp=0
BG3: win=26886 hdMatch=26886 -> GEZEICHNET 26886 (100%) clip=0 noSampler=0 alpha0=0 subOp=0
```

**Kein Tor verwirft irgendetwas.** Die Summe stimmt auf allen drei Ebenen exakt mit `hdMatch`
überein; die gesuchte Lücke zwischen „gefunden“ und „gezeichnet“ existiert nicht. Über 650
Barrel-Bayou-Frames: `miss=0` in jedem einzelnen.

**Warum der Umbenenn-Test vom 21.09. dann nichts zeigte.** `SnesConsole::LoadHdPack()` hat
genau einen Aufrufer: `LoadRom()` (`SnesConsole.cpp:122`). Das Pack wird **einmal beim
ROM-Laden** von der Platte gelesen — einen Ordner im laufenden Emulator umzubenennen ändert
nichts, bis das ROM neu geladen wird. Gegenprobe über das ganze Log: in **allen 2.855**
Barrel-Bayou-Frames (2.205× S44, 650× S45b) steht `miss=0` und `hdBG3 ≈ 25.000`. Ein Lauf mit
fehlendem `bg3/gfxset_38` ist nirgends aufgezeichnet — der Ausschaltversuch war nie in Kraft.
Offen bleibt die saubere Wiederholung: umbenennen **und Power Cycle**.

Das S45b-Instrument bleibt drin. Es hat seine Frage beantwortet und kostet nichts.

---

## [2026-09-23] — S45b: das fünfte Tor mitzählen, den Hauptverdacht abräumen

Vor dem ersten S45-Lauf zwei Dinge nachgezogen.

**Das fünfte Tor.** Der große Renderblock beginnt mit
`if((hasMainHd || hasSubHd || hasEdgeHd) && !spriteIsSubOperand)` (`:1787`). Ist
`spriteIsSubOperand` gesetzt (`:1513`, P4.1f — Sprite ist der Sub-Gewinner und
Farbmath-Operand), fällt der Pixel in den nativen `else`-Zweig, **bevor** einer der vier
S45-Zähler ihn sieht. Die Summe wäre dann kleiner als `hdBGn` und hätte nach einem
unbekannten Tor ausgesehen, obwohl die Stelle bekannt ist. Neuer Zähler `SkipSubOp`, gesetzt
im `else`-Zweig. Die Log-Zeile rechnet die Summe jetzt selbst aus und stellt sie neben
`hdMatch` — eine Abweichung ist damit wirklich ein Befund und kein Buchhaltungsfehler.

**Der Hauptverdacht `noSampler` ist unwahrscheinlich.** Nachgesehen statt vermutet: alle 223
PNGs in `bg3/gfxset_38` sind **32×32**, genau wie die 993 in `bg1/gfxset_38`. Bei `hdScale=4`
ist der schlimmste Fall `srcTX=7` → `7*4+4 = 32`, und `32 > 32` ist falsch — das Tor bei
`:379` kann so nicht feuern. Dass BG3 in Mode 1 2bpp ist, ändert die Maße im Pack nicht.
**`alpha0` ist als Pauschalgrund ebenfalls raus:** 52 Kacheln sind vollflächig deckend, 171
gemischt, **keine** komplett transparent. Bleiben `clip`, `subOp` und die Möglichkeit, dass
die Summe stimmt und wir das Bild falsch zuordnen. Der Lauf entscheidet.

---

## [2026-09-21] — S45: der Unterschied zwischen GEFUNDEN und GEZEICHNET

**Der Anlass.** Der User meldete, der Hintergrund von Barrel Bayou sei im Spiel nativ. Die
Logs sagten das Gegenteil: `match=52661 miss=0`, BG3 mit **26.886 HD-Treffern je Frame**. Ich
habe ihm das dreimal entgegengehalten. Entschieden hat es ein Handgriff von ihm: Pack-Ordner
`bg/bg3/gfxset_38` umbenannt → **Bild unverändert**. Die Kunst wird gefunden und danach
verworfen.

**Warum kein Zähler das zeigen konnte.** `HdLayers[]` steht unmittelbar hinter
`GetMatchingTile` (`:1130`) und zählt den **Nachschlag**. Zwischen ihm und dem Ausgabepuffer
liegen `clipMain`, `HdTileSampler::Init` und die Alphaprüfung je Texel — drei Tore, die kein
Zähler beobachtet hat.

**S45 schließt die Lücke.** Vier Zähler je Ebene, je nativem Gewinnerpixel (nicht je
Subpixel), damit sie direkt gegen `hdBGn` vergleichbar sind:

| Zähler | bedeutet |
|---|---|
| `DrawnLayer` | die HD-Kunst des Gewinners kam in den Ausgabepuffer |
| `SkipClip` | von `clipMain` geblockt (Fenstermaske) |
| `SkipNoSmp` | Kachel gefunden, aber `mainSampler.valid == false` |
| `SkipAlpha0` | Sampler gültig, aber jeder Texel alpha=0 |
| `SkipSubOp` | (S45b) `spriteIsSubOperand` erzwingt den nativen Pfad |

Die vier schließen einander aus und werden in der Reihenfolge der Tore im Code gezählt; ihre
Summe muss `hdBGn` ergeben — eine Abweichung wäre selbst ein Befund.

Sichtbar an zwei Stellen: `drawBG1..4` in der FRAME-Zeile, und im LEVEL-ANALYSIS-Block eine
Zeile je Ebene, die `hdMatch` gegen `gezeichnet` stellt und den Grund benennt.

**Verdacht, den der Lauf prüft:** `HdTileSampler::Init` (`:379`) verwirft eine Kachel, wenn
`srcTX * hdScale + hdScale > tile->Width`. BG3 ist in Mode 1 **2bpp** — wenn die Kunst oder
die Offsets dort anders liegen als bei den 4bpp-Ebenen, schlägt genau diese Prüfung zu, und
`SkipNoSmp` würde es benennen. Das ist eine Vermutung; entschieden wird sie vom Lauf.

Kostet nichts außerhalb der BG-Gewinnerpixel und ändert kein Bild.

---

## [2026-09-21] — S44: Kantenglättung als SNES-Einstellung, live umschaltbar

S43 ist im Spiel bestätigt (Lauf 11:45, `build=S43`, A/B leer): Franse je Frame deckungsgleich
mit dem Schalterlauf um 10:28 (Kleever 1.340→1.347, `$13/$14/$23` 8.885→8.844, WORLDMAP
6.779→6.383), Frame-Zeit Median **2,76 ms** gegen 2,82 (S42) und 2,95 (S41) — eher schneller,
weil drei Hash-Nachschläge pro Pixel entfallen. `spritemiss` nur 50 Zeilen. Vom User bestätigt,
Lockjaw (Unterwasser-Farbmath) ebenfalls sauber.

Damit war die Glättung nur noch über eine Umgebungsvariable abschaltbar — also bekommt sie
eine echte Einstellung, wie `EnableHdPacks` eine hat.

**`SnesConfig.HdSmoothSpriteEdges`** (Standard an), Checkbox unter der HD-Pack-Checkbox in
`SnesConfigView.axaml`, `IsEnabled` an `EnableHdPacks` gebunden. SNES-only ergibt sich von
selbst, weil `SnesConfig` eine eigene Struktur ist.

**Live umschaltbar, ohne Neustart.** Die Glättung ist eine reine Kompositions-Entscheidung pro
Pixel in `ApplyFilter`: nichts wird beim Laden gebacken, die Pack-Kacheln behalten ihren
Alphakanal so oder so, und die PPU füllt Slot 2 unabhängig von der Einstellung. Es gibt keinen
Zustand, der neu aufgebaut werden müsste — anders als bei `EnableHdPacks`, das über
`ForceFilterUpdate()` das ganze Filterobjekt tauscht. Dass die `SNES_HD_*`-Schalter einen
Neustart brauchen, liegt allein daran, dass sie `static const bool getenv(...)` sind.

**Wo der Schalter sitzt:** als `smoothEdges` im `HdFilterFrameCtx`, einmal pro Frame in
`ApplyFilter` gefüllt. Nicht als weitere statische Variable, und vor allem nicht in der
Pixelschleife: die Render-Threads bekommen den Kontext per `const&`, ein Settings-Zugriff pro
Pixel wäre ein Datenrennen **und** 57.344 überflüssige Nachschläge je Frame.
`SNES_HD_NO_SPRITE_EDGES=1` bleibt als Zwangs-Aus für kopflose A/B-Läufe, verknüpft mit ODER:
`noSpriteEdges = s_noSpriteEdges || !ctx.smoothEdges`.

**⚠ Die Struktur wird zwischen C# und C++ feldweise gemarshallt.** Beide enden jetzt auf
`EnableHdPacks`, dann `HdSmoothSpriteEdges`; `[StructLayout(LayoutKind.Sequential)]` ist
gesetzt. Ein Feld an anderer Stelle einzufügen hätte still **jede** Einstellung dahinter
verschoben — deshalb steht der Hinweis auch als Kommentar in `SettingTypes.h`.

**C++ und C# geändert: komplette Solution bauen**, nicht nur inkrementell.

**Im Spiel bestätigt:** der Haken wirkt live, ohne Neustart. Damit ist zum ersten Mal ein
A/B-Vergleich der Kantenglättung am laufenden Bild möglich, statt zwei Spielsitzungen aus
Zahlenreihen gegeneinanderzuhalten — für eine Frage, die zuletzt fünfmal am fehlenden Bild
statt an fehlenden Zählern hängen geblieben ist, ist das das nützlichere Werkzeug.

## [2026-09-21] — S42/S43: der Herkunfts-Gate der Kantenglättung ist gemessen und entfernt

**Beobachtung:** die Kleever-Schwertsplitter sind nach dem Upscale in HD im Spiel, aber ohne
sichtbare Kantenglättung.

**Die Kunst ist es nicht.** Die Kette einmal auf derselben Menge gezählt:

| Stufe | weiche Texel (Alpha 1–247) |
|---|---|
| Upscale-Ausgabe `..._hd4x_edge.zip` | 1,9 % aller Texel, 0,49 je deckendem Texel |
| Installierter Pack, dieselben Kacheln | 14,4 % der Kachelfläche, 0,38 je deckendem |
| 400 zufällige andere Sprite-Kacheln | 0,18 je deckendem |

Gegen die SD-Quelle aufgeteilt (158 Bildpaare): **27.867 weiche Texel, 75,7 % innerhalb der
nativen Silhouette, 24,3 % außerhalb.**

**Die Ursache ist ein Gate.** Von den 1.159 Kacheln, die seit dem Pack vom 18.09. neu sind,
haben **162 keine Referenzpalette — und alle 18 Splitter-Objektpräfixe sind darunter**
(im übrigen Pack: 1,4 %). An `GetSpriteRefPalette(...) != nullptr` hängen aber **alle drei**
Glättungspfade: der Untergrund im Main-Pfad (S21/S22), der Untergrund im Sub-Pfad (S26/S27)
und der Fransen-Slot 2 (S21/S23). Für Laufzeit-Kunst ist die Kantenglättung damit dauerhaft
abgeschaltet, und die Splitter sind der erste Gegenstand, der komplett über die
Laufzeit-Kategorie in den Pack kam.

Das ist kein Versehen. S21 (`2756fa1c`, 12.08.) sagt ausdrücklich:

> Both halves are limited to art that ships a reference palette. **That is an origin test, not
> a colour one:** runtime-captured tiles are grabbed off the live screen with background baked
> into their border texels […] Extending those outwards smeared that background in — visible
> as washed-out world map objects on the first test run.

**Warum die Prämisse neu zu prüfen ist:** hätten die Splitter eingebackenen Hintergrund in den
Randtexeln, wäre ihre SD-Silhouette nahezu deckend — dann könnten nicht 24,3 % der weichen
Texel *außerhalb* davon liegen. Die Laufzeit-Erfassung hat sich seit dem 12.08. geändert
(OAM-Layout, geprüfter Export: 158 Objekte, 158 distinkte Kacheln, keine fremde Kunst).

**S42** führte `SNES_HD_EDGE_IGNORE_REF=1` ein, um den Gate an allen drei Stellen zu umgehen.
**S43 entfernt ihn ganz.** Zwei A/B-Läufe am 21.09. haben beide Hälften der Prämisse geprüft.

### Lauf 1 (09:04 Referenz ohne Schalter, 11.658 Frames / 09:36 mit Schalter, 17.482 Frames)

Reichweite: **56,2 % der gesamten Sprite-Fläche** des Laufs hat keine Referenzpalette.
Gemischte Fransen-Subpixel pro Frame, nach Registerprofil:

| Kontext | Frames A/B | Franse S41 → S42 | Faktor |
|---|---|---|---|
| `$13/$14/$23` | 707/1199 | 644 → 7.725 | ×12 |
| `$13/$04/$23` | 3/606 | 83 → 6.990 | ×84 |
| `$11/$00/$00` (Weltkartenprofil) | 606/1246 | 1.254 → 5.260 | ×4,2 |
| `WORLDMAP $11/$00/$00` | 0/772 | — → 5.643 | nur S42 |
| `$17/$00/$07` | 8990/9738 | 1.326 → 1.365 | ×1,03 |

**Die Weltkarte ist der Fall, für den der Gate gebaut wurde.** Im August lag die Franse dort
auf **898.087 Subpixeln pro Frame — 97,9 % eines 4×-Schirms**, und die Objekte waren
ausgewaschen. Jetzt sind es **5.643 = 0,6 %**, Faktor 159 darunter, und der User meldet sie als
sauber. Der Test ist nicht leer: `SPRAREA` weist in diesen Kontexten nur **33,1 % bzw. 51,9 %**
der Sprite-Fläche eine Referenz zu — der Schalter hat dort die Hälfte bis zwei Drittel der
Kunst neu freigeschaltet.

### Lauf 2 (10:28, `SNES_HD_EDGE_IGNORE_REF` + `SNES_HD_SPRWATCH` auf acht Splitter-Hashes)

933 Messzeilen über **705 Frames** Schwertzerfall:

| | Pixel |
|---|---|
| Silhouette (Slot 0) | 16.035, davon 16.035 im Pack |
| **Franse (Slot 2)** | **43.638, davon 43.638 im Pack** |
| Verdrängtes Sprite (Slot 3) | 715, davon 715 im Pack |
| `wonMain` | 15.946 von 16.035 = 99,4 % |

**Pack-Trefferquote 100,0 % auf allen drei Slots**, und die Franse ist das **2,7-fache der
eigenen Silhouette**. Kein `fringe=0/0` wie beim Bonusfass — die Kunst wird erfasst, der Pack
antwortet, und die einzige Sperre war der Gate.

### Was der Gate wirklich war

S21 nennt ihn ausdrücklich einen **Herkunftstest, keinen Farbtest**. Laufzeit-erfasste Kacheln
hatten 2026-08 Hintergrund in den Randtexeln, und nach außen gezogen schmierte der mit.
Das ist ein **Pipeline-Problem und gehört in den Export**, nicht in einen Pro-Pixel-Test, der
die beiden Fälle gar nicht unterscheiden kann. Der Notausgang bleibt unverändert:
`SNES_HD_NO_SPRITE_EDGES=1`.

### Ein Irrweg, der dokumentiert gehört

Zwischendurch sah es so aus, als würfe der Prioritäts-Gleichstand die Splitter-Franse weg:
`sprFrTie` steigt in den Splitter-Frames um **+60,9/Frame**, und SPRWATCH beziffert den Beitrag
der Splitter auf **+61,9 Fransenpixel/Frame** — 98 % Deckung. **Die Gegenprobe pro Frame hat es
umgeworfen** (`r = −0,148`, die Bänder widersprechen sich). Die Kontrollrechnung zeigt warum:
Splitter-Silhouette gegen `sprHd` müsste Steigung 1 haben und liefert **3,96** — in den
Todesframes skaliert der ganze Bildinhalt mit, die Frames sind nicht vergleichbar. Zwei
zufällig gleiche Gruppenmittel. **Merke: eine Übereinstimmung zweier Aggregate ist erst ein
Befund, wenn sie die Auflösung pro Frame überlebt.**

### Was dabei trotzdem herauskam: der Prioritäts-Gleichstand kostet real

| Kontext | Frames | Franse gefunden | vom Gleichstand abgelehnt | Verlust |
|---|---|---|---|---|
| `$17/$00/$07` (Kleever) | 5.145 | 980 | 230 | **19,0 %** |
| `$13/$14/$23` | 855 | 1.422 | 77 | 5,1 % |
| `WORLDMAP $11/$00/$00` | 325 | 1.613 | 86 | 5,1 % |
| `$11/$00/$00` | 509 | 1.623 | 37 | 2,2 % |
| **Gesamt** | **7.229** | **1.074** | **179** | **14,3 %** |

Der Zweig ist `Sprites[2].Priority <= (MainScreenFlags & 0x0F)` — die Franse eines Sprites über
einem Sprite **gleicher** OBJ-Priorität. Der S23-Kommentar hat das wörtlich vorhergesagt:
*„A large number here means equal priorities are common and the tie-break has to come from OAM
order after all."* Jetzt steht die Zahl da: **14,3 % aller Fransen-Kandidaten, im
Kleever-Kontext jeder fünfte.** Das ist kein Splitter-Problem, sondern trifft jede Überlappung
zweier Sprites derselben Priorität — und im heutigen Pro-Pixel-Modell gibt es die OAM-Reihenfolge
schlicht nicht.

Nebenbei nachgetragen: `SNES_HD_NO_SUB_BG_UNDER` (S37) und `SNES_HD_SPRWATCH` (S41) fehlten in
der A/B-Liste von `WriteSessionBanner` — ein A/B-Lauf gehört ins Banner.

Die Zähler `subGateNoRef` / `subGateOpaque` und die `ref=`-Spalte von `SPRAREA` bleiben. Sie
entscheiden nichts mehr, sagen aber weiterhin, welche Kunst der lebenden CGRAM-Zeile folgt und
welche per Palettenslot gebacken ist — das ist, was den Recolor-Pfad steuert.

**Der größere Befund, noch nicht umgesetzt:** neun Commits (`S21`, `S21b`, `S22/S24`,
`S25–S27`, `S25 zurück`, `S30`, `S32`, `S34`, `S37`) haben dieselbe Form — der Filter löst
Sichtbarkeit nativ auf, komponiert in HD, und muss für jeden halbtransparenten Texel raten,
was dahinter liegt. Die Zahl der Fälle ist das Produkt aus {Main, Sub} × {BG, Sprite, Backdrop,
nichts} × {Farbmath an/aus} × {Overlay oder nicht}, deshalb ist die Liste nicht abgeschlossen.
Der allgemeine Ausweg wäre ein **zweiter Durchgang**, der die HD-Sprite-Kunst mit Alpha über
das bereits fertige HD-Bild legt: dann ist der Mischpartner per Konstruktion das, was wirklich
dahinter steht — ohne Slot 3, ohne Ground-Suche, auf Main wie Sub wie Overlay. Position trägt
Slot 2 (die PPU-Hälfte von S21), Priorität `MainScreenFlags & 0x0F`, und die Farbmath des
Zielpixels muss auf den Texel angewandt werden (sonst kommt Lockjaws heller Saum zurück).
Die `drawMain`-Sperre auf Slot 2 — der Grund für das kantige B auf dem Bonusfass — fällt
dabei mit weg.

**Und er erbt den Prioritäts-Gleichstand oben.** Auch wer über das fertige Bild malt, muss
entscheiden, ob er hinter dem Sprite davor liegt. Die Antwort steht seit S23 im Code: bei
Gleichstand entscheidet die OAM-Reihenfolge. Der zweite Durchgang hat sie ohnehin in der Hand,
weil er in Reihenfolge malt — das Pro-Pixel-Modell hat sie nicht.

## [2026-09-18] — S41: ein Recorder für Treffer, nicht für Fehlschläge

`snes_hd_spritemiss.txt` zeichnet eine Kachel nur auf, wenn sie **erfasst** wurde **und** keine
HD-Kunst fand. Eine nie erfasste Kachel hinterlässt keine Spur — „nicht in spritemiss" heißt
also nicht „abgedeckt", und genau diese Lesart hat zwei Objekte für geklärt erklärt, die der
User als SD im Spiel meldet:

- **`Gfx 3170`**, das Sternemblem mit dem B auf dem Bonusfass: 10 von 10 Kacheln im Pack, alle
  slot-frei geladen, und die Referenzpalette ist zeichengleich mit der lebenden CGRAM-Zeile
  (`00C8 012E 1DB5 027B 077F 7FFF 6B5A`) — `HdSpriteRecolor::Init` setzt bei lauter
  Null-Deltas `valid = false`, das Umfärben läuft also gar nicht.
- **`Gfx 2D40`–`2D64`**, die großen weißen Ziffern 0–9: 60 von 60 Kacheln im Pack.

Beide in zwei Läufen kein einziger Miss. Fehlende Kunst, falscher Palettenslot und Umfärben
sind damit ausgeschlossen — offen ist die Erfassungsseite, und dafür gab es kein Instrument.

**`SNES_HD_SPRWATCH=<16hex>[,<16hex>...]`** (max. 8 Hashes) schreibt je Frame und beobachtetem
Hash eine Diag-Zeile: Pixel je Erfassungs-Slot (`main` = Slot 0, `sub` = 1, `fringe` = 2 aus
S23, `under` = 3 aus S22), wie viele davon `GetMatchingTile` beantwortet, die Palettenslots und
`wonMain`/`wonSub`. **Keine Zeile bedeutet: nie erfasst** — dann kann der Pack nicht die
Ursache sein. Pro Pixel werden nur zwei Sprite-Kacheln erfasst, ein kleines Aufkleber-Sprite
auf einem Fass kann alle vier Slots verlieren; das ist die Hypothese, die der Lauf prüft.

Liegt im Recorder-Block und kostet nichts, solange die Variable nicht gesetzt ist. Der
Hex-Parser ist gegen echte Eingaben geprüft (`DKC2-HD-Tools/tools/spritemiss/test_sprwatch_parser.py`,
8 Fälle). **Im Spiel bestätigt:** der Lauf mit
`SNES_HD_SPRWATCH=E563D7702FE967CC,38E78F2E2A1400EC` hat das Bonusfass-Emblem entschieden —
100 % Trefferquote, aber ausschließlich auf dem Sub-Screen (`main=0/0 sub=21250/21250`),
`fringe=0/0` in allen 341 Frames. Das Instrument hat genau die Frage beantwortet, für die es
gebaut wurde.

---

## [2026-09-17] — S40: spritecap war ebenfalls seit dem 10. August tot

S39 hat zwei Recorder geweckt und `spritecap` ausdrücklich als das gesunde der drei
eingeordnet („inserts AFTER the open"). Das war nur halb richtig. spritecap hatte den
**zweiten** Fehler, den dieselbe Commit-Nachricht benennt und nur in spritemiss behoben hat:
der `OpenRecorder` hing an der Antwort des Seedings.

```cpp
SeedRecorderSet(...);
if(seen.find(setKey) != end) continue;   // bei 88 MB Datei fast immer wahr
s_spriteCapFile = OpenRecorder(...);     // nie erreicht
```

Der erste Kandidat eines Laufs ist bei einer gesättigten Datei fast immer einer, den eine
frühere Sitzung schon hatte. Dann greift `continue` bei noch geschlossener Datei,
`s_spriteCapAttempted` bleibt `true`, und ab der zweiten Kachel endet der Scan am
`!s_spriteCapFile`-`break`.

**Beleg:** das letzte Sitzungsbanner in `snes_hd_spritecap.txt` war
`=== SESSION 2026-08-10 21:24:30 build=S19 ===` — 17 Banner, keines aus den fünf Wochen
danach, und 10. August ist der Tag, an dem `SeedRecorderSet` dazukam.

Fix: seeden und öffnen, **danach** die „hatte eine frühere Sitzung"-Abfrage — dieselbe Form,
die S39 spritemiss gegeben hat. Dazu der Kommentar über dem OAM-Recorder korrigiert, damit die
falsche Einordnung nicht ein zweites Mal als Beleg gelesen wird.

**Im Spiel bestätigt** (Lauf 17.09. 22:04): Banner `build=S40`, die Datei ist von 38.289 auf
43.006 distinkte Hashes gewachsen.

**Warum das mehr als ein Recorder-Fehler ist:** der Pack-Export des Viewers liefert eine
Kachel nur aus, wenn sie in spritecap steht (`spriteCapPairs.get(hash)` leer → `continue`) —
der Palettenslot ist Spiellogik und aus dem ROM nicht herleitbar. Gemessen: **30.610 von
30.610 Sprite-Hashes des installierten Packs haben einen spritecap-Eintrag, keine Ausnahme.**
Fünf Wochen lang wurde also jede upgescalte Kachel, die nicht bereits vor dem 10.08. im Spiel
gesehen worden war, beim Export stillschweigend verworfen. Auf die Kachel genau belegt:
DD Ducking hat 108 ROM-Kacheln, 14 davon standen im alten spritecap — und genau **14** liegen
im Export-ZIP (DX Ducking 14 / 5 / **5**). Nach dem S40-Lauf sind es 252 von 252.

---

## [2026-09-16] — S39: die zwei Recorder, die seit August tot waren (`1954cc80`)

`snes_hd_spritemiss.txt` und `snes_hd_bgcap.txt` wurden seit dem 10.08. nicht mehr
geschrieben. Der Kommentar über dem OAM-Recorder erklärte das mit Sättigung („saturated back
on 10 Aug") — ihre Schlüssel sind pro Kachel, und davon gibt es endlich viele. Diese Lesart
blieb fünf Wochen unhinterfragt und war falsch: der 10.08. ist der Tag, an dem
`SeedRecorderSet` dazukam, und beide Recorder befragten es in der falschen Reihenfolge — der
Schlüssel ging vor dem Seeding ins Dedup-Set, danach fand die Abfrage den eigenen Eintrag,
`OpenRecorder` wurde nie erreicht, und ab der zweiten Kachel beendete der `!file`-`break` den
Scan.

Der Fix behält alle drei Eigenschaften der alten Reihenfolge: das Seeding bleibt faul, gedeckte
Kacheln werden weiter memoisiert (sonst läuft `GetMatchingTile` je Sprite-Pixel je Frame), und
der Stale-Zweig memoisiert gar nicht mehr, was das alte insert/erase-Paar erübrigt.

**Im Spiel bestätigt:** spritemiss kommt zurück (1.199 Zeilen, 635 distinkte Hashes aus einem
kurzen gezielten Lauf) und benennt zwei Animationen, deren Kunst wirklich fehlt — DD Ducking
(94 von 108 Kacheln) und DX Ducking (9 von 14). `bgcap` schrieb im Vormittagslauf noch nicht,
im Abendlauf dann doch (986 Zeilen, Kopf `build=S39`, gfxset 32 und 4 auf BG2) — der damals
notierte Verdacht auf die Live-VRAM-Gegenprobe war unbegründet.

---

## [2026-09-14] — S38: die Messung aus dem Standardpfad nehmen

Die Frage, für die S35/S36 gebaut wurden, ist beantwortet: das Ruckeln war die Bildausgabe
(60,0988 fps gegen 60 Hz ohne VSync), nicht die Emulation und nicht der HD-Pack. Damit gibt es
keinen Grund mehr, in jeder Sitzung mitzumessen.

**`SnesHdPerf::Forced()` → `SnesHdPerf::Enabled()`, Standard AUS.** Vorher lief die Messung,
sobald ein HD-Pack aktiv war, und `SNES_HD_PERF=1` erzwang sie zusätzlich ohne Pack. Jetzt ist
die Umgebungsvariable der einzige Schalter — mit oder ohne Pack, womit auch der A/B-Lauf gegen
einen abgeschalteten Pack weiter funktioniert.

Gegatet ist alles, was etwas kostet:
- **Die Uhr-Abfragen je SCANLINE** in `ProcessEndOfScanline` — die einzigen im Build, 224 Zeilen
  × 2 pro Frame. Ohne Schalter wird `RenderScanline()` wieder nackt aufgerufen.
- `SendFrame`: die Wartezeit auf den Decoder und das 16-MB-Clear.
- `AddEmuFrame` (Mutex + Dateischreiben je Frame) und `AddFilterFrame` im Filter.

**Der Code bleibt im Build.** Die nächste Performance-Frage bekommt damit in einem Lauf eine
Antwort, statt wieder diskutiert zu werden — und die Lesart steht in den Einträgen S35/S36.

Die Recorder (`spritecap`, `bgcap`, `spritemiss`) bleiben unverändert an: sie sind seit
2026-08-10 fertig — `SeedRecorderSet()` liest sie ein und hängt nur neue Schlüssel an, es ist
nichts Neues mehr aufgetaucht — und sollen weiterlaufen, solange noch nicht jedes Level im
Detail geprüft ist.

Build-Kennung **S38**. `SnesHdPerf.h`, `SnesPpu.cpp`, `SnesHdVideoFilter.cpp` — inkrementeller
Build reicht.

---

## [2026-09-14] — S37: der Untergrund, den nur die Sprites hatten — jetzt auch für BG-Kacheln

**Anlass (User):** In Mainbrace Mayhem sind die BG1-Level-Kacheln an den **Rändern** pixelig,
obwohl geglättete HD-Kunst für alle Ebenen im Pack liegt und nichts in SD gezeichnet wird.
Für die BG1-HD-Kacheln war bis hierher **keine Glättung aktiv** — S21–S34 haben ausschließlich
Sprite-Silhouetten behandelt.

### Levelaufbau — aus der Disassembly, `bank_FD.asm`, `DATA_FD7AB6`
```
$212C = $04   MAIN: nur BG3 (der Nebel)
$212D = $13   SUB:  BG1 + BG2 + OBJ  (Level-Geometrie, Hintergrund, Kongs)
$210B = $0642 BG1 chr = $2000, BG2 chr = $4000   (normal — Rambi hat $0725, vertauscht)
$2130 = $02   Sub-Screen ist der zweite Color-Math-Operand
$2131 = $24   ADD, keine Halbierung
```
**BG1 ist die Level-Geometrie und liegt auf dem Sub-Screen** — sie kommt ausschließlich als
Color-Math-Operand ins Bild. Deckt sich mit `SNES_HD_PACK_PROJECT.md` („Belegte Profile") und
dem S34-Eintrag.

### Root Cause
Im Sub-Operanden-Pfad (`SnesHdVideoFilter.cpp:1929-1976`) startet der Operand `oR/oG/oB` als
**native Sub-Screen-Farbe**. Ein voll deckendes HD-Texel nimmt den Schnellpfad; ein
**halbtransparentes** wird über `oR/oG/oB` gemischt, davor optional der `subBotSampler`.

`subTileBot` wurde aber **nur im Sprite-Zweig** gesetzt (Zuweisungen bei 1294 und 1334, beide
tief in dem `} else {` ab 1225). Der BG-Zweig `} else if(!pixelInfo.SubScreenHasSprite) {`
(1208–1224) hat ihn nie gesetzt — der Kommentar bei 1944 sagt es selbst: *„Invalid for every
case but a sub-screen sprite."*

Für eine BG1-Kachel blieb `subBotSampler` also ungültig, und ein halbtransparentes Texel wurde
gegen die native SD-Farbe **desselben Pixels** gemischt — BG1s eigene Farbe. **Kunst mit sich
selbst gemischt kann keine weiche Kante zeigen:** die Silhouette folgt der SD-8×8-Maske und
wird bei 4× zur Treppe. Wörtlich dieselbe Wand, die S21 im Main-Pfad und S26 für
Sub-Screen-Sprites eingerissen hat — für BG-Kacheln stand sie noch.

### Fix
Der BG-Operand bekommt dieselbe Untergrundsuche wie der Sprite-Operand in S26:
- Schleife über die BG-Ebenen, die **auf dem Sub-Screen** liegen und an diesem Pixel Inhalt
  haben, eigene Ebene übersprungen. `sLayer` **ist** der Sub-Screen-Gewinner, also hat jede
  andere dort vorhandene Ebene gegen ihn verloren und liegt hinter ihm — es muss keine
  Prioritätsordnung nachgebaut werden.
- **Mit dem S34-Retry BG1↔BG2**, aus demselben Grund wie dort: bei vertauschten chr-Basen
  liegt die Kunst im Pack unter dem anderen Layer-Index, und die strenge Abfrage findet nichts,
  obwohl der Pack vollständig ist.
- Findet die Suche nichts und liegt überhaupt keine BG-Ebene dahinter, gibt der Sub-Screen dort
  den **Backdrop** aus — das ist dann der Grund, in den die weiche Kante ausläuft (Logik von S32,
  dort für Sprites hergeleitet).
- Voll deckende Kacheln werden übersprungen: sie haben keine Kante zu glätten.

**Die Sprite-Pfade sind unberührt.**

### Neue Zähler in der FRAME-Zeile
`bgBot` (BG-Operand hat einen Untergrund bekommen) · `bgBotRetry` (davon nur über den
BG1↔BG2-Retry gefunden) · `bgNoBot` (transparente Kachel, kein Untergrund gefunden) ·
`bgOpaque` (Kachel voll deckend — der ehrliche Nenner, massives Terrain braucht keinen Grund).

### A/B
`set SNES_HD_NO_SUB_BG_UNDER=1` stellt das Verhalten bis S36 wieder her (Texel mischt mit der
eigenen SD-Farbe). Batch: `bin\win-x64\Release\TEST_S37_ohne_BG_Untergrund.bat`.

Build-Kennung **S37**. Nur `SnesHdVideoFilter.cpp` — inkrementeller Build reicht.

---

## [2026-09-14] — Test I ausgewertet: der Emulator liefert jeden Frame pünktlich

**Drei Läufe am 14.09., je ~2,5 min, Mainbrace Mayhem dann Lockjaw's Locker, Build S36.**
Lauf A und B liegen in einer Mesen-Sitzung (Haken live umgelegt), C ist die Sitzung aus
`TEST_I_LaufC_ohne_Pack.bat`. Trennung von A/B über `filter=0.0/0.0` ab 09:17:14.

| | Dauer | Frames | `over` | `late` | `drop` | Periode Mittel/p99 | Arbeit ⌀ | `sleep` ⌀ |
|---|---|---|---|---|---|---|---|---|
| **A** Pack + Filter an | 155 s | 9.454 | 1 | 1 | 1 | 16,65 / 19,1 ms | 5,8 ms | 10,9 ms |
| **B** Filter aus, Erfassung an | 127 s | 7.746 | **0** | **0** | **0** | 16,62 / 18,8 ms | 5,5 ms | 11,2 ms |
| **C** gar kein Pack | 140 s | 8.540 | 0 | 1 | 1 | 16,66 / 18,8 ms | 3,3 ms | 13,4 ms |

Die je eine Ausnahme in A und C ist kein Spielgeschehen: A bei t=12,5 s ein Levelübergang
(`wait=348 ms`, der Filter schreibt Recorder-Dateien), C bei t=0,000 der allererste Frame
(`sleep=395 ms`, Start). **In 25.740 Frames Spielbetrieb kein einziger verspäteter Frame.**

Die Periodenverteilung ist über alle drei Läufe deckungsgleich (Median der Sekundenmaxima
18,1 ms, p90 18,4 ms, p99 18,8–19,1 ms) — der Frame-Limiter schwankt um ±1,5 ms, in jeder
Konfiguration gleich. **Der Pack kostet nachweislich Arbeit (5,8 gegen 3,3 ms) und kostet
nachweislich keine Pünktlichkeit.**

**Entscheidend ist die Beobachtung des Users:** Lauf C — gar kein Pack, die geringste Last,
13,4 ms Reserve je Frame — kam ihm vor wie der **ruckeligste** der drei. Damit ist das
Ruckeln von unserer Bildrate unabhängig. Zwei Ursachen kommen infrage, beide außerhalb des
HD-Packs:

**1. Bildausgabe gegen 60 Hz ohne VSync (periodischer Mikroruckler).**
`SnesConsole::GetFps()` liefert NTSC **60,0988118623484** fps. Der Bildschirm läuft mit
1920×1080 @ **60 Hz** (Intel Iris Xe), in `settings.json` steht `VerticalSync = False` und
`IntegerFpsMode = False`. Differenz 0,0988 fps ⇒ **alle ~10,1 s ein Bild zu viel**, das die
Anzeige verschluckt oder zerreißt. Für unsere Messung unsichtbar: sie endet am Emulations-
Thread, nicht an der Ausgabe. Passt zu „kleine Slowdowns immer mal wieder im Level".
Abhilfe ohne Code: Video → *Enable integer FPS mode* (rundet auf 60,00 und zieht die
Audio-Abtastrate mit, `Emulator.cpp:750`, `SoundResampler.cpp:82`) **plus** VSync.

**2. DKC2s eigene Verlangsamung (Ruckeln bei vielen Gegnern).**
Läuft die Spiellogik in einem Frame nicht fertig, wiederholt das Spiel das Bild. Der Emulator
gibt das originalgetreu wieder — 60 pünktliche Frames, aber weniger Bewegung darin.
Originalverhalten, nicht behebbar und nicht wünschenswert zu beheben.

**Nicht bestätigt:** Der HD-Filter als Engpass. `wait` (Emu-Thread wartet auf den Filter) war
außerhalb der Levelübergänge durchgehend 0,0–0,1 ms.

**Offen als Aufräumarbeit, nicht als Ruckler-Ursache:** `rec` kostet in Lauf A weiterhin
~1 ms je Frame für Recorder, die gerade nichts sammeln, und `clear` 1,3 ms für das
16-MB-Löschen. Zusammen ~14 % des Budgets — vorhanden, aber ohne Wirkung auf die Pünktlichkeit.

---

## [2026-09-14] — S36: Der Messlauf sagt: die Arbeit ist es nicht

**Messlauf S35 vom 14.09., 08:15:23–08:19:58** (Mainbrace Mayhem, dann Lockjaw's Locker),
User meldete Ruckler gegen 08:16, 08:18 und 08:19. 275 Zeilen, davon 271 `PERF`.

**Ergebnis: zu den gemeldeten Zeiten ist kein einziger Frame über dem Budget.**

| | |
|---|---|
| Sekunden mit `over>0` | 3 von 271 — alle in den ersten 36 s (Levelübergänge), keine davon 08:16/08:18/08:19 |
| `fps` | 268× 61, je 1× 59/60/62 |
| `work` (emu+send) | Mittel 6,2 ms von 16,64 ms Budget; Maximum über den ganzen Lauf 14,8 ms |
| `sleep` (Reserve des Limiters) | Mittel ~10,4 ms pro Frame — knapp zwei Drittel des Budgets ungenutzt |
| `wait` (Emu-Thread wartet auf den Filter) | 0,0/0,1 ms ab 08:16 durchgehend |
| `filter` | Mittel 4–6 ms, Maximum 16,9 ms (08:17), sonst < 7 ms |

Die drei `SLOW`-Frames (127 ms, 391 ms, 31 ms bei t=0 / 13,6 / 35,3) sind Levelübergänge:
`wait` und `rec` dominieren, d. h. der Filter schreibt gerade Recorder-Dateien. Das passiert
beim Kontextwechsel, nicht im Spiel.

**Zwei Scheinbefunde, die keine sind:**
- `fps=61` statt 60: die Sekunde wird beim ersten Frame ab 1000 ms geschlossen, deckt also
  ~1015 ms ab. 61 Frames / 1,015 s = 60,1 fps = NTSC. Korrekt.
- Scheinbar periodische Sprünge alle ~68 s (`t=78 -> 80`, Uhrzeit `+2 s`): dieselbe
  Bucket-Drift. 1,5 % Überdeckung je Zeile ergibt nach ~67 Zeilen eine Sekunde Versatz.
  **Kein Ereignis im Spiel.**

**Was die Messung nicht gesehen hat — und warum:** `over` und `SLOW` hängen beide an `work`.
Ein Frame kann aber auch zu spät kommen, ohne dass Arbeit anfällt: der Frame-Limiter
verschläft, das Betriebssystem entzieht den Thread, die Ausgabe blockiert. Die Periode
(Frameende → Frameende) wurde zwar mitgerechnet, aber **nie ausgegeben**.

**Neu in dieser Messung (`SnesHdPerf.h`):**
- `PERF`-Zeile: `period=Mittel/Max` sowie `late=` (Frames > 20 ms Periode) und
  `drop=` (Frames > 25 ms — die Anzeige wiederholt das vorige Bild).
- `SLOW` löst jetzt auch bei langer Periode aus, nicht nur bei langer Arbeit; `why=` sagt,
  welche Bedingung gegriffen hat (`work`, `period` oder `work+period`).
- Die 500-ms-Grenze aus S35 (Pause/Laden/Menü zählen nicht) gilt unverändert.

**Lesart des nächsten Laufs:**
- `late`/`drop` ≈ 0, Ruckler trotzdem sichtbar → es ist **nicht die Bildrate**, sondern
  DKC2s eigene Verlangsamung bei vielen Objekten. Originalverhalten, nichts zu beheben.
- `late`/`drop` > 0 **nur mit Pack** → unser Pfad; `period` gegen `work` halten.
- `late`/`drop` > 0 **auch ohne Pack** → Mesens Frame-Pacing / VSync, unabhängig vom HD-Pack.

Build-Kennung **S36**. Nur `SnesHdPerf.h` + Versionsstring — inkrementeller Build reicht.

---

## [2026-09-10] — S35: Frame-Zeit-Messung — wo ein langsamer Frame seine Zeit verbringt

**Anlass:** Das Spiel ruckelt an manchen Stellen, vor allem wenn viel los ist (viele Gegner).
Weltkarte und Level-Laden sind unauffällig. **Nur eine Messung, keine Verhaltensänderung.**

**Warum die bisherigen Zahlen die Frage nicht beantworten:** `ms=` in der FRAME-Zeile misst
**nur die parallele Pixelschleife** des Filters (Median ~2,5 ms, max 4,9 ms am 10.09.) — und
FRAME-Zeilen gibt es nur in den ersten 600 Frames eines Kontexts, also am **Levelanfang**. Die
vollen Szenen später im Level wurden nie gemessen, und nichts rund um die Schleife auch nicht.

**Wie ein Frame läuft** (`VideoDecoder::UpdateFrame`, `SnesPpu::SendFrame`): der
Emulations-Thread übergibt jeden Frame an den Decode-Thread; ist der noch mit dem vorigen
beschäftigt, **dreht UpdateFrame Warteschleifen**. Ein Frame kommt also zu spät, wenn
- der Emulations-Thread (CPU + PPU inkl. HD-Erfassung je Pixel, dann das Löschen von
  ~16 MB `SnesHdPpuPixelInfo` je Frame) über 16,64 ms braucht, oder
- der Decode-Thread (der ganze HD-Filter) länger braucht und der Emulations-Thread wartet.

**Neu: `%USERPROFILE%\Downloads\snes_hd_perf.txt`** (`SnesHdPerf.h`, header-only)
- Jede Zeile trägt die **Uhrzeit** (`PERF 21:42:13 …`), damit „um 21:42 hat es geruckelt"
  direkt auffindbar ist, und `t=` (Sekunden seit dem ersten Frame).
- `PERF` — eine Zeile pro Sekunde, jeweils Mittel/Max:
  `fps` · `over` (Frames, deren Arbeit > 16,64 ms) · `work` = emu + send ·
  `emu` (CPU+PPU dieses Frames) · `scan` (davon RenderScanline) · `wait` (UpdateFrame wartet
  auf den Filter) · `clear` (Pixelinfo löschen) · `sleep` (Limiter-Reserve) ·
  `filter` gesamt mit `pre` (Kontext, Fingerprints, LUT) / `render` (Pixelschleife) /
  `rec` (spritecap, bgcap, cgramcap, spritemiss, OAM) / `post` (Summen, Diagnose, Log) ·
  Szenenlast `sprWon`, `sprHd`, `sprSub` · Kontext `sig`.
- `SLOW` — eine Zeile pro Frame mit Arbeit > 20 ms (höchstens 6 pro Sekunde), mit seiner
  Aufschlüsselung und der des zuletzt gefilterten Frames.
- Immer an, solange ein HD-Pack aktiv ist (ein paar hundert Uhr-Abfragen pro Frame).
  `SNES_HD_PERF=1` erzwingt es ohne Pack — für den A/B-Lauf mit HD aus.
- Perioden ≥ 500 ms (Pause, Laden, Menü) werden nicht gezählt.

**Lesart:** `over` > 0 und `wait` hoch → der **Filter** ist der Engpass (dann `filter` und
seine Teile ansehen). `over` > 0 und `emu`/`scan` hoch, `wait` klein → die **Emulation bzw.
HD-Erfassung** in der PPU. Periodische Spitzen in `emu` bei ruhigem `scan` → etwas außerhalb
der PPU (z. B. Rewind-Zustände).

Build-Kennung **S35**. Nur `.cpp` + neuer Header — inkrementeller Build reicht.


## [2026-09-09] — S28–S34: Rambi Rumble — die Kantenglättung greift

**Ergebnis:** In Rambi Rumble blieb die Silhouette der Kongs hart, während sie in Mainbrace
weich ist. Ursache gefunden und behoben. `subNoBot` fällt von 42,8 % auf **1,7 %**, die
Untergrund-Abdeckung steigt von 54,1 % auf **93,1 %** (Mainbrace: 84,9 %). Im Spiel bestätigt.

### S34 — der eigentliche Fix (fünf Zeilen)
Der S26-Untergrundsuche fehlte der **BG1↔BG2 layer-agnostische Retry**, den die beiden
Rendering-Pfade seit jeher haben (`SnesHdVideoFilter.cpp` ~1059 und ~1211). Sie fragte
einmal streng nach `BgTiles[layer].Key` und gab auf.

**Warum das nur Rambi traf — chr-Basen vertauscht.** ppuConfig `DATA_FD7ADF` (`bank_FD.asm`):
`$210B = $0725` → **BG1 = $5000, BG2 = $2000**. Die Kunst liegt im Pack unter dem anderen
Layer-Index. Mainbrace `DATA_FD7AB6`: `$210B = $0642` → BG1 = $2000, BG2 = $4000, normal.
Messwert: `lRetry` = **36.427 px/Frame = 64 % des Bildschirms** in Rambi, **0 in jedem
anderen Level im Log**. Die Rendering-Pfade überbrücken das — deshalb sah das Level
makellos aus — die Untergrundsuche nicht. Kunst vorhanden, Level sauber, Kanten hart.
Von `subBot` (656.340) kommen nach dem Fix **642.312 = 97,9 % über den Retry**.

Das ist das Laufzeit-Gegenstück zum `terrainChrBase`-Export-Fix im Viewer vom 2026-07-02
(siehe `SNES_HD_PACK_PROJECT.md`, Abschnitt „Viewer-Fix Session").

### Levelarchitektur — aus der Disassembly belegt (`bank_FD.asm`)
Beide Level: Mode 1 + BG3-Priority, `$2130 = $02` (Sub-Screen als CM-Operand), ADD ohne
Halbierung. Gleiche Struktur, vertauschte Rollen:

| | Schleier auf MAIN | Welt + Kongs auf SUB | `$210B` | `$2131` |
|---|---|---|---|---|
| Rambi `DATA_FD7ADF` | **BG1** (Honig) | BG2+BG3+OBJ | `$0725` | `$21` |
| Mainbrace `DATA_FD7AB6` | **BG3** (Nebel) | BG1+BG2+OBJ | `$0642` | `$24` |

Der Main-Screen trägt in beiden Fällen **eine** durchscheinende Ebene, der Sub-Screen die
Welt samt Kongs. In Mainbrace liegt die Level-Geometrie (BG1) mit auf dem Sub-Screen und
wird von der Untergrundsuche gefunden; in Rambi liegt sie als Schleier auf dem Main-Screen.

### Diagnostik
- **S28/S29** — `SPRAREA` zählt jetzt auch Sub-Screen-Sprites (in Overlay-Leveln ist
  `sprWon = 0`, der Recorder war dort blind — also in genau den Leveln, für die er gebaut
  war) und meldet aus der Mitte des Fensters statt vom Levelanfang.
- **`SNES_HD_DIAG_FRAMES` Default 60 → 600.** 60 Frames = 1 Sekunde maßen den
  Türdurchgang, nicht das Level. Ein Default, den man überschreiben muss, damit er stimmt,
  ist der falsche Default — und er erzwang eine Wrapper-`.bat` zum Starten. Entfällt.
- **`sprHoleHd`** trennt die zweite `SprSubHd`-Fundstelle heraus. Sie liegt außerhalb der
  `if(cmActive && ColorMathAddSubscreen)`-Kette, die `SprSub` erhöht, und feuert nur, wo
  `cmActive` falsch war — also auf Pixeln, die der Nenner ausschließt. `sprHdSub/sprSub`
  war damit keine Quote. (In den untersuchten Leveln ist der Zähler 0, die Zahlen waren
  dort also unverzerrt.)
- **`subNoRef` / `subOpaque`** messen erstmals das Referenz-Tor im **Sub**-Pfad. Die
  bisherigen Zähler `sprRecol`/`sprNoRef` hängen am MAIN-Sprite-Pfad und sind in
  Overlay-Leveln strukturell 0 — auch in Mainbrace, wo alles funktioniert. Ergebnis:
  **0–99 px.** Das Referenz-Tor ist an der harten Kante unbeteiligt.
- **`nbEmpty` / `nbNoHd`** trennen in `subNoBot` „kein BG auf dem Sub-Screen" von „BG da,
  aber ohne HD-Kachel". In Rambi 1,1 % / 98,9 %; in Mudhole Marsh (noch ohne HD-Kunst)
  100 % / 0 % — der Gegencheck, dass die Zähler das Richtige messen.
- **`subBotRetry`** zeigt, wie viel der S34-Retry zurückholt.

### Verworfen, gemessen, dokumentiert (nicht erneut versuchen)
- **S30 — Untergrund vom Main-Screen.** `subNoBot` → 0, `subMainBot` = 326.902, **Bild
  unverändert**. Bei `Main=$01` ist die einzige findbare Ebene der Main-Screen-Gewinner
  selbst, und `r = min(255, r + oR)` addiert ihn danach auf sich. Wieder ausgebaut.
- **S32 — Backdrop für alle `subNoBot`.** Wirkte leicht verschlechternd: 98,9 % davon sind
  Fälle mit echtem Terrain hinter der Figur, dessen Farbe durch die Backdrop ersetzt wurde.
  In S33 auf `!anyBgOnSub` eingeschränkt, wo die Backdrop tatsächlich das ist, was der
  Sub-Screen ausgibt. Aktiv, aber ohne nachgewiesene Bildwirkung — Aufräumkandidat nach dem
  Vorbild von `18f6381f` (S25).

### Was NICHT die Ursache war
Rambi Rumble hat **keine HD-Lücken** — der Pack ist für dieses Level vollständig, der
Honig-Overlay (`cmFg/gfxset_04`, 336 Kacheln) inbegriffen. `bg/bg2/gfxset_04` hat nur 65
Dateien, aber BG2 gehört hier nicht zur Architektur; das ist kein Defizit. Fehlend sind
lediglich einige Gegner-Sprites und das HUD.

---

## [2026-09-08] — HINWEIS: LÜCKE 13.07.–08.09.

Zwischen P3.10 und hier wurde die Mesen-Arbeit (M5.x, R*, S1–S27) **nicht in dieser
Datei** geführt, sondern in den Projektnotizen und — ab 07.09. — fälschlich im
**Viewer**-Changelog, obwohl am Viewer nichts geändert wurde. Die Kopfzeile oben regelt
die Aufteilung eindeutig: C++-Änderungen gehören hierher. Ab S25–S27 wieder korrekt.
Die älteren fehlgeleiteten Einträge (S21b, S22–S24) stehen noch im Viewer-Changelog.

---

## [2026-09-08] — S25 wieder ausgebaut (Rekonstruktion: Commit `4482908a`)

Nur Mesen. **S25 zeichnete, aber niemand konnte es sehen** — also raus, bevor es zu Code
wird, dessen einziger Beleg ist, dass er ausgeführt wird. Genau so hat sich der S23-Zweig
zu lange gehalten.

**Entfernt:** die Aufzeichnung des Saum-Slots auf dem Sub-Screen in `RenderSprites`
(`objDrawn`/`fringeWindowCount` zurück auf `drawMain`/`mainWindowCount`), die
Scanline-Flags `objOnMain`/`objOnSub`, der dritte Saum-Zweig im Filter samt Prio-Test gegen
den Sub-Gewinner, der Saum-Blend im Operanden, der Schalter `SNES_HD_NO_SUB_FRINGE` und die
Zähler `sprEdgeSub=A/B`.

**Beleg:** fünf A/B-Läufe über 3.634 Frames. S25 zeichnete 747 Subpixel je Frame in
Mainbrace — mit S25 aus meldete der User **keinen** Unterschied, mit dem Untergrund
(S26/S27) aus dagegen „nicht geglättet“. Vermutung, ungeprüft: der Operand wird zum Nebel
addiert und oft halbiert, was ein schwach gedeckter Saum nicht überlebt.
**Einschränkung:** Augen-A/B, kein Standbildvergleich derselben Stelle — wer S25
rehabilitieren will, macht genau den.

**S26 und S27 bleiben** und hängen nicht an S25: Slot 3 wird über den `drawSub`-Pfad
gefüllt, nicht über den Saum-Slot. Log-Rotation und OAM-Gate bleiben ebenfalls.

## [2026-09-08] — S25–S27: die Kantenglättung erreicht die Overlay-Level

Nur Mesen (`SnesPpu.cpp`, `SnesHdVideoFilter.cpp`), Commit `4482908a`.

**In Mainbrace, Rambi Rumble und Lockjaw unter Wasser lief die Kantenglättung nie — und die
Ursache war eine einzige Gate-Bedingung.** DKC2 nimmt OBJ per HDMA vom Main-Screen
(`$212C=$04`), also war `drawMain` für Sprites falsch und `RenderSprites` füllte den
Saum-Slot dort gar nicht erst. `sprEdge=0/0` in jedem Frame las sich wie kaputter Code und
war dieses Tor; die Saum-Zeilenpuffer waren die ganze Zeit gefüllt.

**S25** zeichnet den Saum in den **Color-Math-Operanden** statt in die Hauptfarbe — in einem
Overlay-Level existiert die Figur nur dort, ein Saum in der Hauptfarbe würde zum Nebel
*addiert* statt durch ihn hindurch gezeichnet.
**S26** gibt dem Sub-Sprite einen Untergrund. Es mischte noch gegen `nsR/nsG/nsB`, und das
ist an einem Sprite-Pixel die **eigene SD-Farbe** — dieselbe Wand, die S21 im Main-Pfad
eingerissen hat, hier unangetastet. Der Untergrund ist streng begründet: in diesem Zweig hat
das Sprite jede Sub-BG-Ebene geschlagen, sonst hätte `RenderTilemap` `SubScreenHasSprite`
gelöscht.
**S27** nimmt bevorzugt ein **zweites Sprite** als diesen Untergrund, exakt wie S22 im
Main-Pfad. `Sprites[3]` wird auch für den Sub-Slot gefüllt — nichts Neues aufzuzeichnen.
Beide S22-Fallen mitgenommen: die R3-Zeilen-LUT bleibt von einer Sprite-Unterlage fern, und
die Unterlage braucht die Umfärbung.

**Gemessen, nicht argumentiert.** Fünf A/B-Läufe über 3.634 Frames haben S25/S26 getrennt:
mit S26 aus meldet der User „nicht geglättet“ in Mainbrace und Lockjaw, während S25 weiter
über 400 Subpixel je Frame zeichnet — **der sichtbare Effekt ist S26, S25s Nutzen ist
unbelegt.** S27 hat eine saubere A/B/A-Kette am Fass, das Dixie über dem Kopf trägt:
glatt → hart → glatt, bei `subSprUnder` = 106 px/Frame in 140 von 248 Frames.

**Ein Zweig wurde als Zähler ausgeliefert und danach entfernt.** Die Vermutung, auch auf
Scanlines *mit* OBJ auf Main fehle Saum, war aus `sprHdSub` hochgerechnet — aber diese Pixel
liegen INNERHALB der Silhouette, und direkt daneben lässt `$212D=$10` den Sub-Screen leer.
Der Zähler kam in allen fünf Läufen als **0** zurück, auch in dem, der den Schalter
einschaltete. Raus statt auf der Begründung behalten — wie `sprFrOver` in S23.

**Recorder aufgeräumt.** `snes_hd_oam.txt` dedupliziert auf die **Komposition** eines Frames,
also zählte fast jedes Frame als neu; die Datei war auf **813 MB** gewachsen, während
`spritecap`/`bgcap`/`spritemiss` (Dedup je Kachel) seit dem 10.08. gesättigt sind. OAM ist
jetzt aus per Vorgabe (`SNES_HD_OAMCAP=1`) — dieselbe Behandlung wie `cgramcap` in S18; der
Viewer liest die Datei weiterhin über `parseOam` für die Laufzeit-Objekte.
Die zwei kleinen Logs hängen an und rotieren bei 16 MB. Ein erster Versuch mit „frisch je
Start“ hat binnen einer Stunde eine Vergleichsserie zerstört, weil ein außerhalb der
Testskripte gestarteter Lauf die beiden davor löschte.

## [2026-07-13] — Phase 3.10: Targeted BG3 Overlay Swap (P3.10)

### BG3+Mode1Bg3Priority Overlay Swap + Lockjaw Fix

**Datei:** `Core/SNES/HdPacks/SnesHdVideoFilter.cpp`

**Problem P3.9:** `useHdSubPixel` (HD-Bottom als CM-Operand) verursachte zwei
Regressionen:
- Lockjaw: BG1 Terrain halb-transparent statt solid (CM ADD mit voller 8-bit
  HD-Background-Farbe → zu starke Farbveränderung)
- Mainbrace (ohne Nebel): leichte blaue Tönung auf BG1/BG2 Tiles in Bereichen
  wo kein Nebel ist (useHdSubPixel feuerte für alle sole-on-main Pixel)

**Lösung P3.10:** `useHdSubPixel` komplett entfernt. Stattdessen:

1. **Gezielter BG3-Overlay-Swap:** Nur wenn ALLE drei PPU-Bedingungen erfüllt:
   - `winLayer == 2` (BG3 ist der Compositing-Winner)
   - `Mode1Bg3Priority` (Spiel hat BG3 priorisiert → Overlay-Pattern)
   - `(MainScreenLayers & 0x0F) == 0x04` (BG3 ist sole BG auf Main)

   → Swap: HD Content (BG1/BG2) wird Primary, Nebel-Tint aus nativem PPU-Output.

2. **Alle anderen CM-Fälle:** Winner HD wird solid gerendert mit nativem
   `SubScreenColor` als CM-Operand → leichte Tönung, kein Halbe-Transparenz.

**Ergebnis — drei unabhängige Pfade ohne Heuristic:**

| Level | Winner | Swap? | Rendering |
|-------|--------|-------|-----------|
| **Mainbrace (Nebel)** | BG3+Mode1Prio+sole | **JA** | HD Content + Nebel-Tint |
| **Mainbrace (kein Nebel)** | BG1/BG2 | Nein | HD Winner + native CM |
| **Lockjaw (unter Wasser)** | BG1 sole | Nein | HD Terrain + native Wasser-Tint |
| **Rambi** | BG1 (entfernt) | Nein | Step 3 Overlay-Fallback |
| **Normal** | BG1/BG2 | Nein | HD Winner, kein CM |

---

## [2026-07-13] — Phase 3.9: HD Sub-Screen CM Operand (P3.9) — REVERTED in P3.10

### Full-HD Color Math: CM(HD_main, HD_sub) statt CM(HD_main, native_sub)

**Datei:** `Core/SNES/HdPacks/SnesHdVideoFilter.cpp`

**Problem P3.8:** Der Overlay-Swap-Ansatz (Winner verwerfen → Bottom als Primary)
war nicht generisch genug. Er funktionierte für Mainbrace (Nebel=Overlay) aber
verursachte Lockjaw-Regression (BG1=Terrain fälschlicherweise verworfen, da BG1
ebenfalls sole-on-main + CM + AddSubscreen).

**Lösung P3.9:** P3.8-Swap komplett entfernt. Stattdessen neuer Ansatz:
Wenn BEIDE Layer (Winner + Bottom) HD-Tiles haben UND CM + AddSubscreen aktiv,
wird der HD-Bottom-Pixel (raw, pre-brightness) als CM-Operand verwendet statt
des nativen SubScreenColor. Ergebnis: `CM(HD_main, HD_sub)` — beide Seiten HD.

**Rendering-Flow:**
1. Bottom HD Tile → als Hintergrund gerendert (mit Brightness)
2. Top HD Tile (Winner) → CM angewendet mit HD-Bottom als Operand → über Hintergrund
3. Brightness auf CM-Ergebnis angewendet

**Per-Subpixel Override:** `pxCmR/pxCmG/pxCmB` ersetzt `cmR/cmG/cmB` wenn
`useHdSubPixel=true`. Wenn Bottom-Pixel transparent → Fallback auf native SubScreenColor.

**Generische Wirkung — kein Heuristic, kein Sonderfall:**
- Mainbrace: `CM(HD_fog, HD_content)` → nebliger HD-Content ✓
- Lockjaw:   `CM(HD_terrain, HD_background)` → Wasser-getöntes HD-Terrain ✓
- Rambi:     `CM(HD_honey, HD_terrain)` → Honig-Glow auf HD-Terrain ✓
- Normal:    Kein CM → Winner HD normal gerendert ✓
- Overlay-Fallback (Step 3): Unverändert — greift wenn Winner KEIN HD-Tile hat

---

## [2026-07-13] — Phase 3.8: CM+AddSubscreen Overlay Swap (P3.8) — REVERTED in P3.9

### Winner found + Color Math + AddSubscreen → overlay mode

**Datei:** `Core/SNES/HdPacks/SnesHdVideoFilter.cpp`

**Problem P3.7:** Wenn der Winner-Layer (z.B. BG3 Nebel in Mainbrace) ein
HD-Tile hat UND Color Math mit AddSubscreen aktiv ist, wurde der Nebel
opak gerendert und hat den darunter liegenden HD-Content (BG1/BG2)
verdeckt. Der Overlay-Fallback (Step 3) griff nur wenn KEIN HD-Tile
für den Winner gefunden wurde.

**Lösung P3.8:** Nach der Bottom-Layer-Suche in Step 2 wird geprüft:
wenn `cmActive && AddSubscreen && hdTileBot` gefunden, wird in den
Overlay-Modus gewechselt — das Sub-Screen HD-Tile wird zum Primary,
der Nebel-Tint wird aus dem nativen PPU-Output extrahiert. So wird
HD-Content DURCH den Nebel-Effekt hindurch angezeigt.

**Betroffene Level:** Mainbrace (BG3 Nebel über BG1/BG2 Content),
potenziell alle Level mit Overlay + Color Math + AddSubscreen.

---

## [2026-07-13] — Phase 3.5: Enhanced HDMA Diagnostics (P3.5)

### Per-Scanline HDMA Dump + Expanded Split Detection

**Datei:** `Core/SNES/HdPacks/SnesHdVideoFilter.cpp`

**Analyse-Ergebnis P3.4:** Lockjaw hdBG1=0 ist KEIN Regression — BG1-Tiles
bei VRAM $2000-$2340 sind DMA-animiert und wurden in keinem Build (auch P3.3)
jemals gefunden. Die MISS-Hashes sind zwischen P3.3 und P3.4 identisch.

**Änderungen P3.5:**

1. **HDMA Split Detection erweitert:** Erkennt jetzt auch Änderungen in
   SubScreenLayers, ColorMathEnabled, FixedColor und ScreenBrightness
   (nicht mehr nur MainScreenLayers). → Gangplank Galleon wird jetzt als
   HDMA-aktiv erkannt falls HDMA FixedColor/CM per Scanline ändert.

2. **Per-Scanline HDMA Dump:** Bei HDMA-aktiven Kontexten wird einmalig
   pro Kontext ein kompakter Dump ausgegeben der Scanline-Bereiche mit
   gleichen Registerwerten zusammenfasst:
   ```
   HDMA SCANLINE DUMP (first HDMA frame):
     Y   1- 95: Main=$13 Sub=$14 CM=$23 AddSub=1 Fixed=$0000 Br=15
     Y  96-224: Main=$01 Sub=$14 CM=$23 AddSub=1 Fixed=$0000 Br=15
   ```
   Damit können wir Gangplank/Lockjaw HDMA-Effekte verstehen.

---

## [2026-07-13] — Phase 3.4: Overlay Tint Extraction (P3.4)

### Winner-Only bleibt Top-Layer, Multi-Layer nur für Bottom

**Datei:** `Core/SNES/HdPacks/SnesHdVideoFilter.cpp`

P3.0 war rein Winner-Only. P3.1 erweitert das sicher um Bottom-Layer-
Compositing für HD Tiles mit Transparenz:

**Ansatz (P2.0-Bug-sicher):**
1. PPU-Winner ist IMMER der Top-Layer (keine Priority-Walk-Ersetzung!)
2. HD Lookup für Winner (+BG1↔BG2 Retry) — wie bisher
3. Kein HD-Match für Winner → natives Pixel (Winner ist opak, fertig)
4. HD-Match gefunden → Top-Layer rendern
5. Wenn Top-Layer transparente Pixel hat: Nächsten Layer darunter
   (Mode 1 Prioritätsreihenfolge) als Bottom-Layer suchen
6. Bottom-Layer wird hinter transparenten Top-Pixeln compositet

**Sicherheitsgarantien:**
- Winner ohne HD Tile → natives Pixel (kein Lower-Priority-Tile-Durchscheinen)
- Bottom-Layer-Suche filtert nach MainScreenLayers (Sub-Screen ausgeschlossen)
- Sprites → natives Pixel (spriteWon unverändert)

**Änderungen gegenüber P3.0:**
- Prioritätsreihenfolge wird NUR für Bottom-Layer-Suche berechnet (nicht für Top)
- Neuer Diagnosezähler `multi=` zeigt Pixel mit Bottom-Layer-Match
- MainScreenLayers-Check verhindert Sub-Screen-Layer als Bottom
- Build-Version: `P3.1`

**Erwartetes Verhalten:**
- Pirate Panic: BG2 gewinnt (Winner) → HD Tile. BG3 als Bottom falls transparent.
- Mainbrace: BG3 gewinnt (Nebel) → HD Tile + Color Math (SUBTRACT FixedColor)
- NPC-Text: Sprite gewinnt → natives Pixel → kein Bug
- BG3 vor BG1: Unmöglich, da Winner=BG1 (PPU korrekt)

---

## [2026-07-10] — Phase 3: HD Color Math (P3.0)

### CM-Skip entfernt, echte HD Color Math implementiert

**Datei:** `Core/SNES/HdPacks/SnesHdVideoFilter.cpp`

Phase 2.1 übersprang HD-Tile-Rendering komplett wenn `AllowColorMath` aktiv war
(CM-Skip). Das war zu aggressiv — in HDMA-Leveln (Mainbrace, Hot Head Hop, Rambi
Rumble) war AllowColorMath auf ALLEN Pixeln gesetzt → 100% native Fallback.

**Phase 3 ersetzt CM-Skip durch echte HD Color Math:**

1. HD-Tile-Lookup erfolgt IMMER, unabhängig von AllowColorMath
2. Wenn HD-Tile gefunden UND AllowColorMath aktiv → Color Math auf HD-Pixel anwenden
3. Color Math Quelle: `FixedColor` (aus ScanlineInfo) oder `SubScreenColor` (per-pixel),
   abhängig von `ColorMathAddSubscreen`
4. Operationen: ADD oder SUBTRACT, mit optionalem HALVE
5. Brightness (0-15) wird NACH Color Math angewendet (wie die PPU)
6. Gilt für opaque und semi-transparent Alpha-Pfade

**Build-Version:** `P3.0`

**Diagnostik:** `frameCmSkip` Counter umbenannt zu `frameHdCm` (zählt Pixel mit
angewandter HD Color Math statt übersprungener)

---

## [2026-07-06] — Phase 1: Per-Scanline PPU Register Snapshot

### Neue Datenstruktur: SnesHdScanlineInfo

**Datei:** `Core/SNES/HdPacks/SnesHdData.h`

Neue Struktur `SnesHdScanlineInfo` — Snapshot aller PPU-Register die für
HD-Compositing relevant sind, erfasst pro Scanline:

- **Color Math:** `ColorMathEnabled`, `SubtractMode`, `HalveResult`,
  `AddSubscreen`, `ClipMode`, `PreventMode` ($2130, $2131)
- **Fixed Color:** `FixedColor` ($2132) — HDMA-animiert pro Scanline
  (Nebelgradient, Lavatönung, Eiseffekt)
- **Brightness:** `ScreenBrightness` ($2100) — Fade-In/Out
- **Layer-Zuordnung:** `MainScreenLayers`, `SubScreenLayers` ($212C, $212D)
- **Window-Positionen:** `Window1Left/Right`, `Window2Left/Right` ($2126-$2129)
- **Color-Window-Konfiguration:** `ColorWindowActive[2]`, `ColorWindowInverted[2]`,
  `ColorWindowMaskLogic` — für Color-Window-Auswertung auf HD-Auflösung

### SnesHdScreenInfo erweitert

`ScanlineInfo[239]` Array hinzugefügt. Wird in `SendFrame()` beim
Double-Buffer-Swap via `memset` gecleared.

### PPU befüllt ScanlineInfo

**Datei:** `Core/SNES/SnesPpu.cpp`

In `RenderScanline()`, direkt nach `hdScanline`-Berechnung und VOR
`ApplyColorMath()`, wird `_state` in `ScanlineInfo[hdScanline]` kopiert.
So hat der HD-Filter den Register-Zustand *vor* der nativen Color Math
und kann sie selbst auf HD-Pixeln neu berechnen.

### Include-Abhängigkeit

`SnesHdData.h` inkludiert jetzt `SNES/SnesPpuTypes.h` für die Enum-Typen
`ColorWindowMode` und `WindowMaskLogic`. Keine zirkuläre Abhängigkeit
(SnesPpuTypes.h ist ein reines Type-Definition-Header).

### Kein Verhaltensunterschied

Phase 1 fügt nur Daten hinzu — die werden noch nicht konsumiert.
Der bestehende HD-Filter arbeitet exakt wie M5.19 weiter.

---

## ═══════════════════════════════════════════════════════════════
## v1 — Per-Level-Typ Sonderfälle (M5.1 bis M5.19)
## Gesichert als Tag: v0.1-sonderfall-m5.19
## ═══════════════════════════════════════════════════════════════

---

## [2026-07-06f] — M5.19: Beehive Regression Fix — frameBg3FogSkip statt BgLayerMask (Issue O)

### Fix: bg1OverlayWinner Kriterium korrigiert

**Problem:** M5.17 fügte `BgLayerMask & 0x04` als Bedingung für `bg1OverlayWinner` hinzu,
um Beehive-Overlay (BG1 über BG3) von Mainbrace-Terrain (BG1 ohne BG3) zu unterscheiden.
Aber BG3 deckt in Beehive-Leveln nur ~56% der Pixel ab (31636/56056). Die restlichen 44%
(24420 Pixel) wurden fälschlich als nicht-overlay klassifiziert → Fallback auf native
Rendering → Regression ("wilder Mix aus HD und nativem Muster").

**Root Cause:** Per-Pixel BG3-Masken-Check ist zu granular. Nicht alle Beehive-Pixel haben
BG3 (transparente Bereiche), aber ALLE brauchen Overlay-Blend-Rendering.

**Lösung:** Frame-Level-Entscheidung statt Per-Pixel-Mask:
```cpp
// M5.17-M5.18 (broken): Per-Pixel BG3 check
bool bg1OverlayWinner = (!hdTile && winLayer == 0 && (pixelInfo.MainScreenFlags & 0x80)
                         && (pixelInfo.BgLayerMask & 0x04));
// M5.19 (fixed): Frame-Level Fog-Erkennung
bool bg1OverlayWinner = (!hdTile && winLayer == 0 && (pixelInfo.MainScreenFlags & 0x80)
                         && frameBg3FogSkip == 0);
```

- **Beehive:** `frameBg3FogSkip == 0` (kein Fog-Gate feuert jemals) → Overlay auf ALLEN Pixeln ✓
- **Mainbrace:** `frameBg3FogSkip > 0` (tausende Fog-Pixel ab erster Scanline) → kein Overlay ✓

### Diagnostik-Erweiterung: Overlay-Blend Aufschlüsselung

Neue Counter im WINNERS-Log: `ovBg2=X ovBg3=Y ovMiss=Z`
- `ovBg2`: Overlay-Pixel die BG2-Terrain via Layer-Retry fanden (korrekt)
- `ovBg3`: Overlay-Pixel die BG3-Hintergrund als Fallback fanden (kein BG2 vorhanden)
- `ovMiss`: Overlay-eligible Pixel die weder BG2 noch BG3 fanden

---

## [2026-07-06e] — M5.18: Fog Contour Fix — ADD Color Math für BG3 Fog (Issue L)

### Fix: Native ADD statt gewichtetem Blend für BG3-Fog

**Problem:** Die M5.17 Fog-Blend-Formel `(hdR * 4 + fogR) / 5` (80% HD + 20% Fog)
erzeugte einen uniformen dunklen Schleier statt sichtbarer Nebelkonturen/Wisps.
- Schwarzer Fog (0x0000): `hdR * 0.80` → 20% Verdunklung (FALSCH — nativ: keine Änderung)
- Heller Fog (0x2108): `hdR * 0.85` → nur 5% Variation (FALSCH — nativ: deutlicher Kontrast)
- Ergebnis: kaum sichtbarer Fog-Gradient trotz HDMA-Animation der Fog-Dichte

**Root Cause:** Mainbrace Mayhem nutzt ADD Color Math (MSFlags=0x81, Bit 5=0).
Die native PPU addiert die BG3-Fog-Farbe (HDMA-animiert, 0x0000–0x2108 Graustufen)
auf das Terrain. Die 80/20-Gewichtung ignoriert den ADD-Modus komplett.

**Lösung:** Separater Blend-Pfad für `bg3FogBlend` vs `bg1OverlayBlend`:
- **bg3FogBlend (Mainbrace Fog):** `result = min(255, hdPixel + fogColor)` — nativer ADD.
  HDMA variiert fogColor pro Scanline → Contour/Wisps werden sichtbar.
  Half-Add (MSFlags Bit 6) wird ebenfalls unterstützt: `fog/2` statt vollem `fog`.
- **bg1OverlayBlend (Beehive Honey):** Unverändert `(hdR * 4 + fogR) / 5` — keine Regression.

Beide Rendering-Pfade betroffen: alpha=255 (opak) UND alpha>0 (semi-transparent).

---

## [2026-07-06d] — M5.17: BG3 Fog-Winner Gate + BG1 Overlay Fix (Issue L — Mainbrace Mayhem)

### Fix 1: BG3 Fog-Winner Gate (Step 1)

**Problem:** In Mainbrace Mayhem liegen BG3-Fog-HD-Tiles im Pack (`bg/bg3/`). Step 1 fand
diese Tiles und renderte sie opak — der Nebel war sichtbar, aber das Terrain darunter
komplett verdeckt. Der bestehende `bg3FogBlend`-Pfad (Step 3) wurde nie erreicht, weil
`!hdTile` bereits false war.

**Root Cause:** Step 1 macht keinen Unterschied zwischen BG3-Fog-Winnern (semi-transparent,
AllowColorMath aktiv) und normalen BG3-Winnern. BG3-Fog-HD-Tiles wurden wie normale
opake Tiles behandelt.

**Lösung:** `bg3FogWinner`-Gate in Step 1 (~Zeile 315): Wenn `winLayer == 2` UND
`AllowColorMath` gesetzt ist, wird der HD-Tile-Lookup für BG3 übersprungen. Step 3
(`bg3FogBlend`) findet stattdessen BG1/BG2-Terrain-HD-Tiles und rendert sie mit
Nebel-Tint (80% HD + 20% Fog-Farbe).

**Neuer Diagnostik-Counter:** `fogSkip=%u` im FRAME-Log — zählt übersprungene
BG3-Fog-Winner-Lookups pro Frame (erwarteter Wert: ~49800 in Mainbrace).

### Fix 2: BG1 Overlay-Winner Einschränkung

**Problem:** Die `bg1OverlayWinner`-Erkennung (M5.16) war zu breit. In Mainbrace Mayhem
feuerte sie ~3914 Mal pro Frame — BG1 gewann in Nebellücken (wo BG3 keinen Tile hat),
wurde aber als "Overlay" fehlinterpretiert. Das Ergebnis: BG2-Hintergrund mit
BG1-Terrain-Tint (falsche Darstellung).

**Root Cause:** Die Bedingung `!hdTile && winLayer == 0 && AllowColorMath` unterschied
nicht zwischen echtem Overlay (BG1 über BG3, Beehive) und Terrain in Nebellücken
(BG1 wo BG3 fehlt, Mainbrace).

**Lösung:** Zusätzliche Bedingung `(pixelInfo.BgLayerMask & 0x04)` (~Zeile 359):
BG1 ist nur Overlay, wenn BG3 am selben Pixel AUCH einen Tile hat.
- **Beehive:** BG1 gewinnt ÜBER BG3 (beide vorhanden) → `0x04` gesetzt → Overlay ✓
- **Mainbrace:** BG1 gewinnt WO BG3 FEHLT (Nebellücke) → `0x04` = 0 → Terrain ✓

**Erwartete Log-Werte für Mainbrace nach Fix:**
- `fogSkip` ≈ 49800 (alle BG3-Fog-Pixel übersprungen)
- `fogB` ≈ 49800 (Step 3 findet Terrain unter Fog)
- `ovBlend` = 0 (kein BG1-Overlay in diesem Level)

---

## [2026-07-06c] — M5.16: BG1 Overlay-Blend (Issue O — Rambi Rumble Beehive HD)

### Neue Feature: BG1 Overlay-Blend Rendering-Pfad

**Problem:** In Beehive-Leveln (ppuConfig $03) liegt die semi-transparente Honig-Overlay
auf BG1 (layer=0). BG1 gewinnt das PPU-Compositing auf 100% der Pixel (wn0=55140,
acm0=55140). Die Honig-HD-Tiles wurden gerendert und deckten den BG2-Terrain komplett ab.
BG2 Terrain-HD-Tiles wurden nie nachgeschlagen → kein HD-Terrain sichtbar.

**Root Cause:** Exakt das gleiche Problem wie BG3-Fog in Mainbrace Mayhem, aber auf BG1
statt BG3. Der bestehende `bg3FogBlend`-Pfad triggert nur für `winLayer == 2`. Kein
äquivalenter Pfad für `winLayer == 0` (BG1 overlay).

**Lösung: `bg1OverlayBlend`-Pfad in `SnesHdVideoFilter.cpp`:**

1. **Erkennung** (~Zeile 333): `bg1OverlayWinner` = Pixel wo BG1 gewinnt, kein HD-Tile
   für BG1 vorhanden, und AllowColorMath aktiv → Honig-Overlay erkannt
2. **Step 2 Skip** (~Zeile 347): Overlay-Winner überspringen Palette-Vergleichs-Logik
3. **Step 3b** (~Zeile 423-462): Neuer BG1 Overlay-Blend Pfad — sucht HD-Tile auf
   BG2 (Terrain) und BG3 (Hintergrund), inkl. Layer-Retry
4. **Fog-Color Berechnung** (~Zeile 707): `bg3FogBlend || bg1OverlayBlend` →
   MainScreenColor als Overlay-Tint (80% HD + 20% Honig-Farbe)
5. **Color Math Delta Guard** (~Zeile 727): Overlay-Blend Pixel überspringen
   cmDelta (falsche Berechnung für diesen Fall)
6. **Rendering** (~Zeile 759, 808): Fog-Blend auf opaque und alpha-blended Pixel
   wird für Overlay-Blend identisch angewendet
7. **Diagnostik**: `ovBlend=%u` im FRAME-Log, `(OV-BLEND)` Tag bei MATCH-Einträgen,
   `frameBg1OverlayBlend` Counter

**Betroffene Level (alle ppuConfig $03, $2131=$21):**
Rambi Rumble (0x02), Hornet Hole (0x11), Rambi Scene (0x12),
Parrot Chute Panic (0x13), Shortcut (0x26), King Zing Sting (0x60),
plus 7 Bonus-Räume — insgesamt 13 Level-Varianten.

**Dateien:**
- `Core/SNES/HdPacks/SnesHdVideoFilter.cpp` — BG1 overlay-blend Erkennung,
  Rendering-Pfad, Diagnostik (Build M5.15 → M5.16)

---

## [2026-07-06b] — M5.15: Per-Layer Diagnostik + Issue O/P Journal-Update

### Diagnostik-Erweiterung (M5.15)

**Neue Features in `SnesHdVideoFilter.cpp`:**
- `frameWin[4]` / `frameWinACM[4]` — Per-Layer Winner- und AllowColorMath-Pixelzähler
- `BG3WIN sample` Logging — Erste 3 BG3-gewinnende Pixel pro Context (MSFlags, MSColor, ppuOut, Tile-Info)
- `WINNERS` Summary-Zeile pro Frame: `wn0/wn1/wn2/wn3 | acm0/acm1/acm2/acm3`
- Build-Version: M5.14 → M5.15

### Journal-Update

**Issue O (Rambi Rumble):** Post-Log-Analyse (36.931 Zeilen) dokumentiert —
Layer-Fix von vorheriger Session war FALSCH (reverted im Viewer), Palette pal=6 korrekt,
Content-Hash-Mismatch bleibt als offenes Problem.

**Issue P (Gusty Glade):** Pre-Log-Analyse zeigt cmDelta=0 in JEDEM Gameplay-Frame —
unser Subtract-Mode-Fix (9d97024c) wird nie ausgelöst, blaue Quadrate haben eine
völlig andere Ursache. M5.15 WINNERS-Daten sollen die echte Quelle identifizieren.

---

## [2026-07-06] — Issue P Fix: Blaue Quadrate (Color Math Subtract) + Brightness + Delta-Skalierung

### Fix 1: Multiplikative Color Math für Subtract-Modus (Issue P — Blaue Quadrate)

**Problem:** Gusty Glade HD-Tiles zeigten blaue Quadrate. Color Math SUBTRACT
subtrahiert eine FixedColor (hoher R/G, niedriger B) — das additive cmDelta-System
clampte R/G auf 0 während B unverändert blieb → asymmetrisches Clamping → blaues
Erscheinungsbild.

**Lösung:** Neuer `IsSubtractMode`-Flag (Bit 0x20) im `PixelFlags`-Enum, gesetzt in
`SnesPpu.cpp` überall wo `AllowColorMath` gesetzt wird. `SnesHdVideoFilter.cpp` nutzt
multiplikative Skalierung (`hdR * cmPostR / cmPreR`) für Subtract-Modus — bewahrt
Farbverhältnisse statt absoluter Offsets. Additiver Pfad beibehalten für Add-Modus
(Nebel, Glow-Effekte).

**Dateien:**
- `Core/SNES/SnesPpuTypes.h` — `IsSubtractMode = 0x20` zu `PixelFlags` Enum
- `Core/SNES/SnesPpu.cpp` — Flag an 4 Stellen gesetzt (RenderBgColor, RenderSprites,
  BG-Layer-Template, Mode 7)
- `Core/SNES/HdPacks/SnesHdVideoFilter.cpp` — Neuer `if(cmSubtractMode)` Branch
  für opaque und alpha-blend Pfade

### Fix 2: Brightness-Kontamination bei MainScreenColor

**Problem:** `MainScreenColor` wurde VOR `ApplyBrightness()` erfasst, während
`ppuOutputBuffer` (Post-Math) NACH Brightness erfasst wurde. Die cmDelta-Ratio
enthielt dadurch den Brightness-Faktor → HD-Tiles wurden bei Fades über-verdunkelt.

**Lösung:** Brightness inline auf `MainScreenColor` anwenden mit
`channel * ScreenBrightness / 15` (gleiche Formel wie `ApplyBrightness()`).
Bei ScreenBrightness=15 (normales Gameplay) ist dies ein No-Op.

**Datei:** `Core/SNES/SnesPpu.cpp` (MainScreenColor-Capture-Block)

### Fix 3: 5-bit→8-bit Delta-Skalierung

**Problem:** Konvertierung nutzte `val*8` statt `val*8 + val/4` (≈ `val*255/31`).
3% Unter-Anwendung des Color-Math-Effekts.

**Lösung:** Akkurate Konvertierung `val*8 + val/4` für beide Seiten (pre und post).

**Datei:** `Core/SNES/HdPacks/SnesHdVideoFilter.cpp` (cmDelta-Berechnung)

### Verweis
- Issue P dokumentiert in `DEBUG_JOURNAL.md` (ab Zeile 2150)
- Sub-Issues P(b) Layer-Mismatch und P(c) Blätter-Tiles weiterhin OFFEN

---

## [2026-07-05] — M5.14: Layer Retry Bubble-Regression Fix

- Fallback-Loop in `SnesHdVideoFilter.cpp` entfernt die fälschlicherweise Layer-Retry
  auf ALLE Tiles anwendete (nicht nur BG1↔BG2). Verursachte Bubble-Artefakte in
  Hot Head Hop (Issue N Regression).

## [2026-07-03] — M5.13: Layer-Agnostic Retry BG1↔BG2

- Layer-Retry-Mechanismus für Issue H: Wenn BG1-Tile-Lookup fehlschlägt, wird mit
  BG2-Layer-Index nachgeschlagen (und umgekehrt). Behebt Layer-Index-Mismatch
  zwischen Viewer-Export und Mesen-Runtime.
- BG3-Fog-Blend-Pfad korrekt von Retry ausgenommen.

## [2026-06-28] — M5.12: Issue H BG3 Background Fallback

- `frameHasBg1ColorMath` Flag für korrektes BG3-Rendering bei Levels mit Color Math.

## [2026-06-26] — M5.11: Color Math Delta für HDMA Lava-Glow

- Initiale cmDelta-Implementierung: Differenz zwischen Pre- und Post-Color-Math
  MainScreenColor wird als additiver Offset auf HD-Tiles angewendet.
- BG3-Fog-Blend (80/20 Mischung) für Sub-Screen-Nebel-Compositing.

## [2026-06-17] — M5.4: Enhanced MISS Diagnostik

- Erweiterte Diagnose-Logs: MATCH, PAL MISMATCH, MISS, FRAME, CONTEXT CHANGE.
- VRAM-Signatur-basierte Level-Erkennung mit automatischem Counter-Reset.
- MISS-Limit von 30 auf 60 erhöht.

## [2026-06-12] — M5.3: Content Hash Matching

- FNV-1a 64-bit Content Hash System (`hashes.bin` Loader).
- Fingerprint-System (`fingerprints.bin`) für Gfxset-Identifikation.
- BG3 2bpp Tile-Support (Layer 2).

## [2026-06-11] — M5.2: Content Hash Export

- `SnesHdPackLoader.cpp`: Laden von `hashes.bin` Content-Hash-Einträgen.
- Ablösung des adressbasierten Tile-Matchings durch Content-basiertes.

## [2026-06-10] — M5.1: Checksum-Based Tile Matching

- VRAM-Checksums (`checksums.bin`) für Tile-Kollisionserkennung.
