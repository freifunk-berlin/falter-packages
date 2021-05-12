This folder contains some supporting-scripts that might help to ease some configuration-tasks. 
The intention is that these scripts can be called directly from the shell or by the wizard. There is  the parameter "-c" which tells the script to do the UCI-commit before finishing. The default is to not commit the changes, that helps for review and for calling by teh wizard.

## bandwidth-change.sh
a small script to setup / change the configuration for QoS, as these values should be kept in sync between several files

## make_batman-adv.sh
script to setup a BATMAN-adv bridge on all interfaces of the freifunk-zone

## sharenet-switch.sh
script to handle the problem described in https://github.com/freifunk-berlin/firmware/issues/292. This is a common problem on 1 LAN-port routers or PoE-powered routers to have the freifunk-lan on the port you want to have the WAN-zone.
