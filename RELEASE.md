# How to have a release

The properties of a falter-release get defined by the `freifunk_release` file in the `falter-common` package *(packages/falter-common/files-common/etc/freifunk_release)*. This file controls parameters like the releases version number and the underlying OpenWrt version. All scripts and tools for building falter-releases get the information they need from that file.

For Falter-releases, we follow this workflow:

## Have a pre-release

At first, we trigger the buildbot to build a pre-release. For this, you need to adjust the `freifunk_release` file in the falter-common package. Adjust the string in `FREIFUNK_RELEASE` to something like `1.2.5-rc1` or similar. In addition, you might need to adjust `FREIFUNK_OPENWRT_BASE` to a proper OpenWrt-release like `21.02.1`. Once you've pushed the commit, the buildbot should start building the release automatically (-> webhook).

As testing every configuration can be quite time-consuming, we usally involve the community into that process. Just write a short mail to the mailing list asking for some testing. :)

## Do the actual release

When everything is ready, do the following:
1. Create a commit with the new version number (like in pre-release) and push it to the repo. We use only stable OpenWrt-releases versions as release base.
2. Then follow the instructions on the [github manual](https://docs.github.com/en/github/administering-a-repository/releasing-projects-on-github/managing-releases-in-a-repository#creating-a-release). If everything went right, the release should be based on the commit you've just pushed.

## After a release

This step is very important: You need to do another commit immediately after the buildbot has start building and before merging other commits. In `freifunk_release` you need to adjust these variables to values matching your situation:

```sh
FREIFUNK_RELEASE='1.2-snapshot'
FREIFUNK_OPENWRT_BASE='21.01-SNAPSHOT'
```

If you don't do this, buildbot will just overwrite your precious release images once you've merged the next commit. That's becaus buildbot derived the destination-directory from the version.

So please don't miss this step.
