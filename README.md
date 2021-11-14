## Overall Project Structure and Falter Repositories
The [Falter project](https://github.com/freifunk-berlin/falter-packages) builds a modern OpenWrt-based firmware for Freifunk Berlin. We split our code on different repositories. For the beginning there are three main repositories you should know:

+ **[packages](https://github.com/freifunk-berlin/falter-packages/)**: This repo holds the source code for an OpenWrt package feed. All falter specific packets reside there, regardless if they are luci-apps or just command-line-apps. *Everything* should be bundled as a package. If you want to file an issue or fix a bug you probably want to go here.
+ **[repo_builder](https://github.com/freifunk-berlin/falter-repo_builder)**: In that repo there is a script which compiles the source codes from *packages* repo into a package feed. We use the dockerized OpenWrt-SDK for that.
+ **[builter](https://github.com/freifunk-berlin/falter-builter)**: The builter assembles Freifunk images from the OpenWrt-imagebuilder and the pre-compiled package feed from repo-builder. If you want to include a new app into falter, you'd need to add it to the packagelists defined here.

# Packages Repository

## Installation
For detailed installation look [here](INSTALLATION.md).

Just add following line to `feeds.conf`

    src-git falter https://github.com/Freifunk-Spalter/packages.git;openwrt-19.07
