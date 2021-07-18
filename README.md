## Overall Project Structure and Falter Repositories
The Falter project is split into different repositories. For the beginning there are three main repositories you should know:

+ **[packages](https://github.com/Freifunk-Spalter/packages/)**: This repo holds the source code for an OpenWrt package feed. All falter specific packets reside there, regardless if they are luci-apps or just command-line-apps. *Everything* should be bundled as a package. If you want to file an issue or fix a bug you probably want to go here.
+ **[repo_builder](https://github.com/Freifunk-Spalter/repo_builder)**: In that repo there is a script which compiles the source codes from *packages* repo into a package feed. We use the dockerized OpenWrt-SDK for that.
+ **[builter](https://github.com/Freifunk-Spalter/builter)**: The builter assembles Freifunk images from the OpenWrt-imagebuilder and the pre-compiled package feed from repo-builder. If you want to include a new app into falter, you'd need to add it to the packagelists defined here.


# Falter Packages Repository

### Write your Apps translatable, please
To get apps translated easily into another language, you should write your message prompts in a special way. For more information on that and a tutorial for actually translation, please go to [TRANSLATION.md](TRANSLATION.md).

### Installation
You might include packages from this repository into your self-compiled OpenWrt-images. For detailed installation look at [INSTALLATION.md](INSTALLATION.md).

Or just add following line to `feeds.conf`
```sh
src-git falter https://github.com/Freifunk-Spalter/packages.git
```
