#!/bin/sh

# fetch the translation-tools from the luci repo

LUCI_REPO="https://github.com/openwrt/luci.git"
LUCI_ZIP="https://github.com/openwrt/luci/archive/refs/heads/master.zip"

DIR="_tmp_"

git clean -df build/
mkdir $DIR
cd $DIR
wget "$LUCI_ZIP"
unzip "master.zip"
mv luci-master/build/* ../build
cd ..
rm -rf $DIR
