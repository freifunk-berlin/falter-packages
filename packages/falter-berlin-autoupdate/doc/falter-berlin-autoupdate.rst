falter-berlin-autoupdate
========================

Autoupdate ist ein Paket, dass Skripte für automatische Updates bereitstellt. Sobald eine neue Firmware-Version verfügbar ist, wird diese über das ``sysupgrade``-Kommando von OpenWrt eingespielt. Es gibt zusätzlich eine LuCI-App mit der man auch über die Web-GUI Autoupdates ein- oder ausschalten kann.

Funktionsweise
--------------

Autoupdate lädt täglich vom Firmware-Server (firmware.berlin.freifunk.net) eine Datei ``auotupdate.json`` herunter. In dieser ist u.A. die aktuelle Versionsnummer kodiert. Ist diese höher als die eigene Versionsnummer, wird der Update-Prozess angestoßen:

1. Es wird die Liste mit Download-Links aller Router-Modelle heruntergeladen und der für das eigene Modell passende Link herausgesucht.
2. Die ``autoupdate.json`` ist von mehreren Community-Mitgliedern signiert worden. Die Signaturen werden heruntergeladen und gegen die lokal vorhandenen Schlüssel geprüft. Nur dann, wenn mindestens drei gültige Unterschriften vorhanden sind, wird mit dem Update fortgefahren.
3. Die Firmware-Datei wird nach ``/tmp/`` geladen und mit dem Hash-Wert aus der ``autoupdate.json`` verglichen, um die Integrität der Datei zu prüfen.
4. Falls alle Tests erfolgreich waren, wird das Update mit ``sysupgrade UPDATE-DATEI`` eingespielt. Die bisherigen Einstellungen bleiben dabei erhalten.

Der Update-Process lässt sich über mehrere Kommandozeilen-parameter zusätzlich beeinflussen (z.B. Debugging-Modus, etc.). Nähere Infos dazu liefert der Aufruf ``autoupdate -h``.

Hinweise
--------

- Autoupdate unterstützt nur die Updates, welche von OpenWrts ``sysupgrade``-Routine unterstützt werden. Insbesondere funktionieren Upgrades z.B. nicht, wenn das Target wechselt (ar71xx -> ath79).
- Es kann passieren, dass sich die Konfigurations-Dateien von Paketen in unterschiedlichen OpenWrt-Versionen ändern. Da die Firmware-Entwickler nicht alle Konfigurationen testen können, kann es passieren, dass Inkompatibilitäten in Spezialfällen nicht behandelt werden. Wir empfehlen daher, immer ein funktionierendes Backup der Routereinstellungen aufzubewahren.
- Nach einem Update sind nur die Basis-Pakete installiert, welche im Image sowieso vorhanden sind. Pakete, welche durch den Nutzer nachinstalliert wurden, müssen nach einem Update wieder selbst nachinstalliert werden.

Für Contributors: Ein Release signieren
--------------------------------------

Die Linkliste wird mit dem OpenWrt-Tool ``usign`` signiert. Die Installation und Schlüsselgenerierung für ``usign`` wird im `OpenWrt-Wiki <https://openwrt.org/docs/guide-user/security/keygen?s[]=usign&s[]=guide#generate_usign_key_pair>`_ beschrieben. Statt aus dem Quelltext zu kompilieren, kannst du unter Debian-basierten Systemen auch das Paket ``signify-openbsd`` nutzen.

Die öffentlichen Schlüssel der Schlüsselträger sind im ``keys/``-Ordner abgelegt. Gegen diese Schlüssel werden Zertifikate verifiziert.

Um ein Autoupdate im Netz anzustoßen, müssen (standardmäßig) mindestens 3 Schlüsselträger unterschrieben haben. Eine Signatur kann man so erzeugen:

1. ``autoupdate.json`` herunterladen mit: ``wget https://firmware.berlin.freifunk.net/stable/autoupdate.json``.
2. Signatur erzeugen mit: ``./usign -S -m autoupdate.json -s geheimer_Schluessel.sec``. Die Signatur ist die Datei ``autoupdate.json.sig``
3. Die Signatur-Datei unter ``https://firmware.berlin.freifunk.net/stable/autoupdate.json.$NUM.sig`` hochladen. ``$NUM`` ist eine fortlaufende Nummer, beginnend ab 1. Wenn du keinen Zugang zum selector-Server hast, schicke die Signatur-Datei bitte an einen Maintainer.
4. Sich zurücklehnen und freuen, dass mehr aktuelle Firmware im Netz läuft.

Für Maintainer: Ein Autoupdate vorbereiten
------------------------------------------

1. Release bauen (siehe Doku für Releases)
2. Release mit ``./fetch_release.sh`` in den `Firmwareselector <https://selector.berlin.freifunk.net>`_ übertragen. Es muss noch nicht ins Dropdownmenü eingetragen werden, aber die JSON-Dateien für die einzelnen Router müssen vorhanden und zugreifbar sein.
3. ``autoupdate.json`` mit dem script ``/usr/local/src/generate_autoupdate_json.py`` erzeugen und unter ``/usr/local/src/www/htdocs/buildbot/stable`` ablegen.
4. Contributors informieren, dass autoupdate.json nun signiert werden kann
5. Signaturen in entsprechender Nummerierung (siehe oben) in das gleiche Verzeichnis legen.

Es ist sinnvoll, nicht alle Signaturen auf einmal hochzuladen. In der Vergangenheit haben wir zuerst zwei Signaturen hochgeladen. Kontrollierbare Test-Router, welche auf zwei Zertifikate eingestellt waren, haben dann ein Update gemacht. Wenn dort alles funktioniert, können die weiteren Zertifikate hochgeladen werden, sodass der Rest des Netzes folgt.

Für augenblickliche Test kann der Autoupdater auch mit der Option ``-n`` gestartet werden, um eine bestimmte Anzahl an Zertifikaten zu fordern. Das gleich kann auch in der Kondifurationsdatei unter ``/etc/config/autoupdate`` eingetragen werden.
