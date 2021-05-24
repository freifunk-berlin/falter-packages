#!/bin/sh

# fetch the translation-tools from the luci repo

LUCI_REPO="https://github.com/openwrt/luci.git"
LUCI_ZIP="https://github.com/openwrt/luci/archive/refs/heads/master.zip"

DIR="_tmp_"

rm -rf build/
mkdir $DIR
cd $DIR
wget "$LUCI_ZIP"
unzip "master.zip"
mv luci-master/build ..
cd ..
rm -rf $DIR

#git clone $LUCI_REPO _tmp_
#mv _tmp_/build .
#rm -rf _tmp_
