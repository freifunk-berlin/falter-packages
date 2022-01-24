falter-berlin-autoupdate
========================

Autoupdate ist ein Paket, dass Skripte für automatische Updates bereitstellt. Sobald eine neue Firmware-Version verfügbar ist, wird diese über das ``sysupgrade``-Komando von OpenWrt eingespielt. Es gibt zusätzlich eine LuCI-App mit der man auch über die Web-GUI Autoupdates ein- oder ausschalten kann.

Funktionsweise
--------------

Autoupdate fragt täglich beim Firmware-selector (selector.berlin.freifunk.net) die Versionnummer der aktuellen Stable-Version an. Ist diese höher als die eigene Versionsnummer, wird ein Update-Prozess angestoßen:

1. Es wird die Liste mit Download-Links aller Router-Modelle heruntergeladen und der für das eigene Modell passende Link herausgesucht.
2. Die Linkliste ist von mehreren Community-Mitlgiedern signiert worden. Die Signaturen werden heruntergeladen und gegen die lokal vorhandenen Schlüssel geprüft. Nur dann, wenn mindestens drei gültige Unterschriften vorhanden sind, wird mit dem Update fortgefahren.
3. Die Firmware-Datei wird nach ``/tmp/`` geladen und mit dem Hash-Wert aus der Linkliste verglichen, um die Integrität der Datei zu prüfen.
4. Falls alle Tests erfolgreich waren, wird das Update mit ``sysupgrade UPDATE-DATEI`` eingespielt. Die bisherigen Einstellungen bleiben dabei erhalten.

Der Update-Process lässt sich über mehrere Kommandozeilen-parameter zusätzlich beeinflussen (z.B. Debugging-Modus, etc.). Nähere Infos dazu liefert der Aufruf ``autoupdate -h``.

Hinweise
--------

- Autoupdate unterstützt nur die Updates, welche von OpenWrts ``sysupgrade``-Routine unterstützt werden. Insbesondere funktionieren Upgrades z.B. nicht, wenn das Target wechselt (ar71xx -> ath79).
- Es kann passieren, dass sich die Konfigurations-Dateien von Paketen in unterschiedlichen OpenWrt-Versionen ändern. Da die Firmware-Entwickler nicht alle Konfigurationen testen können, kann es passieren, dass Inkompatibilitäten in Spezialfällen nicht behandelt werden. Wir empfehlen daher, immer ein funktionierendes Backup der Routereinstellungen aufzubewahren.
- Nach einem Update sind nur die Basis-Pakete installiert, welche im Image sowieso vorhanden sind. Ein Zusatzpaket, wie z.B. bbbdigger, muss nach einem Update selbstständig nachinstalliert werden. Dies wird in einem späteren Release evtl. noch automatisiert.

Für Contributer: Ein Release signieren
--------------------------------------

Die Linkliste wird mit dem OpenWrt-Tool ``usign`` signiert. Die Installation und Schlüsselgenerierung für ``usign`` wird im `OpenWrt-Wiki <https://openwrt.org/docs/guide-user/security/keygen?s[]=usign&s[]=guide#generate_usign_key_pair>`_ beschrieben. Statt aus dem Quelltext zu kompilieren, kannst du unter Debian-basierten Systemen auch das Paket ``signify-openbsd`` nutzen.

Die öffentlichen Schlüssel der Schlüsselträger sind im ``keys/``-Ordner abgelegt. Gegen diese Schlüssel werden Zertifikate verifiziert.

Um ein Autoupdate im Netz anzustoßen, müssen (standardmäßig) mindestens 3 Schlüsselträger unterschrieben haben. Eine Signatur kann man so erzeugen:

1. Link-Liste herunterladen mit: ``wget https://selector.berlin.freifunk.net/$VERSION/tunneldigger/overview.json``. Bitte daran denken, auch die notunnel-liste zu signieren: ``wget https://selector.berlin.freifunk.net/$VERSION/notunnel/overview.json``
2. Signatur erzeugen mit: ``./usign -S -m overview.json -s akira.sec``. Die Signatur ist die Datei ``overview.json.sig``
3. Die Signatur-Datei unter ``https://selector.berlin.freifunk.net/$VERSION/$FLAVOUR/overview.json.$NUM.sig`` hochladen. ``$NUM`` ist eine fortlaufende Nummer, beginnend ab 1. Wenn du keinen Zugang zum selector-Server hast, schicke die Signatur-Datei bitte an einen Maintainer.
4. Sich zurücklehnen und feuen, dass mehr aktuelle Firmware im Netz läuft.
