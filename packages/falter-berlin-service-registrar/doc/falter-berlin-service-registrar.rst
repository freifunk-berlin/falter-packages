falter-berlin-service-registrar
===============================

Der Service-Registrar ist eine App, die die Einrichtung von Diensten im Berliner Freifunknetz stark vereinfacht. Die Anwendung besteht aus einer LuCI-App für das komfortable Anlegen der Dienste über die Weboberfläche und einem Shell-Script/Daemon, welcher die Eintragung der Dienste übernimmt.

Gebrauchsanweisung
------------------

Der Service-Registrar kann sowohl von der Kommandozeile, als auch vom Webinterface genutzt werden. Die Konzepte sind dabei sehr ähnlich, nur die Bedienung ist anders.

Er generiert die Einstellungen ausschließlich aus seinen eigenen Dateien. Dienste, die z.B. von Hand im OLSR-Daemon eingetragen wurden, werden entfernt, sofern sie nicht beim Service-Registrar eingetragen sind.

Möchte man den Service-Registrar nutzen, muss man ihn als Zusatzpaket nachinstallieren. Das geht im Webinterface über *System* -> *Paketverwaltung*, wo man die Pakete

..
    falter-berlin-service-registrar
    luci-app-falter-service-registrar
    luci-i18n-falter-service-registrar-de

installiert.

Falls man den Service-Registrar ausschließlich über die Kommandozeile nutzen möchte, geht auch ein ``opkg install falter-berlin-service-registrar``.

Beide Varianten haben den gleicen Funktionsumfang und unterscheiden sich ausschließlich in der Bedienung.


LuCI-App (Webinterface)
^^^^^^^^^^^^^^^^^^^^^^^

Der Service-Registrar befindet sich in der Admin-Oberfläche unter *Freifunk* -> *Freifunkdienste registrieren*. In der App können entweder Webseiten oder Allgemeine Dienste angelegt werden.

Webeiten sind Dienste, bei denen der Freifunk-Router selbst eine (statische) Webseite ausliefert. Es wird also nicht nur entsprechend OLSR konfiguriert, sondern auch der Webserver auf dem Freifunkrouter so eingestellt, dass er die Webseite ausliefert.

Allgemeine Dienste sind etwas anders. Sie eignen für Dienste, die auf einem anderen Gerät im lokalen Freifunknetzwerk laufen. Wenn man z.B. eine Webcam bereitstellen möchte, oder irgendeine andere Art von Server hat, eignet sich diese Art von Diensten am besten dafür.

Am besten sollte die IP-Adresse des entsprechenden Gerätes dann auf statisch gestellt werden (unter *Status* -> *Übersicht* und dann im Abschnitt *Aktive DHCP-Leases* auf den Knopf *Auf statisch setzen* drücken).

Die benötigten Felder sind sind eigentlich relativ selbsterklärend. Nachdem die Änderungen mit dem Knopf *Speichern & Anwenden* abgespeichert wurden, beginnt der Freifunkrouter mit der Einrichtung.

Bis der Service im kompletten Freifunknetz verfügbar ist, können bis zu 5 Minuten vergehen.


Kommandozeile
^^^^^^^^^^^^^

Um einen Dienst über die Kommondozeile einzutragen, muss zuerst die Konfigurationsdatei unter ``/etc/config/ffservices`` angepasst werden und anschließend das Skript ``register-services`` ausgeführt werden.

Je nachdem, ob man eine Webseite oder einen Allgemeinen Dienst eintragen möchte, gibt es zwei unterscheidliche Typen von UCI-Sections. Einmal den Typen ``website`` und einmal den Typen ``service``. Beide Typen unterscheiden sich geringfügig in ihren Feldern:

..
    config website "testsite"
            option fqdn         "testsite.olsr"
            option description  "Webseite auf dem lokalen Router"
            option protocol     "tcp"
            option port         "81"
            option web_root     "/tmp/www/testsite"
            option disabled     0

..
    config service "extern"
            option fqdn         "extern.olsr"
            option description  "Ein Dienst der auf einem anderen Geraet lauft"
            option protocol     "udp"
            option port         "80"
            option ip_addr      "10.36.0.34"
            option disabled     0

Besondere Vorsicht braucht es bei dem Feld ``description``: Dort sollten keine Sonderzeichen, wie z.B. Punkte``.``, Kommata``,``, Dollarzeichen ``$``, Umlaute usw. verwendet werden.

Die IP-Adresse im Feld ``ip_addr`` muss aus dem Host-Network des eigenen Routers stammen ("DHCP-Adressen" aus der IP-Email).

Nachdem die Datei entsprechend angepasst wurde, werden die Dienste mit einem Aufruf von ``register-services`` eingetragen. Dienste, die vorher von Hand im OLSR-Daemon eingetragen wurden, werden dabei **ohne Hinweis** entfernt.
