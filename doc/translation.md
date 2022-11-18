# Translation for falter-packages

For translating falter-packages the general rules from the [luci-wiki](https://github.com/openwrt/luci/wiki/i18n)
apply. For marking translateable strings in the apps source-code, use
`<%: string to be translated %>` for lua+htm-apps and `_("string to be translated.")`
for the new-style javascript-apps.

You should read the really short and informative [luci-wiki on translation](https://github.com/openwrt/luci/wiki/i18n)
to understand the rest of what we do here.

## get LuCI translation scripts

Run `fetch_translation_tools.sh` once. It will download the current luci-translation scripts from master and place them in the `build` directory. Please don't commit them, as they might change. If you feel, that those scripts aren't up to date, just run that script again.

## prepare translation of a new app

If the app wasn't translated anytime before or there where changes in the UI, create a template first:

```sh
./build/i18n-scan.pl luci/[application] > luci/[application]/po/templates/[application_basename].pot
```

After that create a new subdir for the language you are going to translate i.e. `de/` for German. create an empty file in that dir with `touch de/[application_basename].po`. Then call:

```sh
./build/i18n-update.pl luci/[application]/po
```

from the base directory! It will fill the empty file(s) with the ground structure. From there you can start the translation with an editor.

## update po-files on new app versions

To update the translation templates (`.pot`) and the transaltion files (`.po`) of all apps in the repo, go to the root and just run `./build/i18n-sync.sh`. There might be an error-message on the luci-base files but that is okay.

If you really just want to limit to one app, you might use `./build/i18n-scan.pl` for the templates and ./build/i18n-update.pl for the language files individually. But running sync is far more easy and thus encouraged to use.

## tools for translation

The luci-scripts place/update the `.po` files in a subdirectory of the apps directory. Those files contain the original string as key and bind it with the translated string. You could work on that files with any editor of your choice.

But it is far more easy to use a specialised translation editor like `poedit`. That one will show you the original string and the translated version side by side and can even help you with the translation by suggesting machine translation. Taking a machine translation and just improving it can speed up the whole process massively.

## notes on packagelists

The package-buildsystem will automatically generate packages that contain a translation of an app to a specific language. If a falter-image should contain that translation, you should add it to the packagelists at falter-builter-repo. Take this example for getting an idea on how that should look:

```txt
# GUI transaltion stuff
luci-i18n-base-de
luci-i18n-base-en
[...]
luci-i18n-falter-de
luci-i18n-falter-en
luci-i18n-ffwizard-falter-de
luci-i18n-ffwizard-falter-en
```
