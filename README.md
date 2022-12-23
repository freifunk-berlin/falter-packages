# Freifunk Berlin: Falter Firmware

## Overall Project Structure and Falter Repositories

The [Falter project](https://github.com/freifunk-berlin/falter-packages) builds a modern OpenWrt-based firmware for Freifunk Berlin. We split our code on different repositories. For the beginning there are three main repositories you should know:

+ **[packages](https://github.com/freifunk-berlin/falter-packages/)**: This repo holds the source code for an OpenWrt package feed. All falter specific packets reside there, regardless if they are luci-apps or just command-line-apps. *Everything* should be bundled as a package. If you want to file an issue or fix a bug you probably want to go here.
+ **[builter](https://github.com/freifunk-berlin/falter-builter)**: The builter assembles Freifunk images from the OpenWrt-imagebuilder and the pre-compiled package feed from repo-builder. If you want to include a new app into falter, you'd need to add it to the packagelists defined here.

## Specific Comments on this Repo

You will find a directory `doc/` in this repository, which contains several documents on workflows, installation, building etc. It is planned, to have an automated documentation system for the falter-packages there too.

### Write your Apps translatable, please

To get apps translated easily into another language, you should write your message prompts in a special way. For more information on that and a tutorial for actually translation, please go to [doc/translation.md](TRANSLATION.md).

### Compiling

In the `build/`-directory in this repository, you can find a build-script. It will download all necessary OpenWrt-SDKs, to compile the packages in this feed for a given architecture. The packages will appear in a directory of your choice. Please refer to [compiling.md](doc/compiling.md) for a more detailed tutorial on how to use that script. For a very quick start, take this:

```sh
# script-name, openwrt-version, CPU-architecture, output-directory
build/build.sh openwrt-22.03 mips_24kc out/
```

### Installation

You might include packages from this repository into your self-compiled OpenWrt-images. For detailed installation look at [installation.md](doc/installation.md).

Or just add following line to `feeds.conf`

```sh
src-git falter https://github.com/freifunk-berlin/falter-packages.git
```
asdf
