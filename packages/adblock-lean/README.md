# adblock-lean package

This package intentionally does not vendor adblock-lean runtime sources. The
OpenWrt package build downloads the selected upstream source archive, verifies
`PKG_HASH`, and installs only the required runtime files:

- `/etc/init.d/adblock-lean`
- `/usr/lib/adblock-lean/abl-lib.sh`
- `/usr/lib/adblock-lean/abl-process.sh`
- `/etc/adblock-lean/abl-reg.md5`
- `/usr/share/doc/adblock-lean/LICENCE.md`

Runtime dependencies are declared as package metadata so the image builder
selects them without making this package compile external dependencies.
The package assumes the OpenWrt dnsmasq conf-script support required by upstream.

The package does not install `/etc/adblock-lean/config`, cron entries, dnsmasq
`addnmount` settings, or service enablement. Those are site policy and must stay
managed by bbb-configs.

## Updating upstream

1. Pick an upstream tag or commit.
2. Resolve and record the exact commit:
   `git -C /path/to/adblock-lean rev-parse <tag-or-commit>^{commit}`
3. Download the matching source archive and calculate the hash, for example:
   `wget -O /tmp/adblock-lean.tar.gz https://github.com/lynxthecat/adblock-lean/archive/refs/tags/<tag>.tar.gz`
   `sha256sum /tmp/adblock-lean.tar.gz`
4. Update `PKG_VERSION`, `PKG_RELEASE`, `PKG_SOURCE_TAG`,
   `PKG_SOURCE_VERSION`, and `PKG_HASH` in `Makefile`.
5. Build the package with the normal falter-packages build workflow.
6. Verify package contents and run `sh -n` on the installed shell scripts.
7. Test with a bbb-configs generated image before publishing the feed.

Do not run the upstream installer or `service adblock-lean setup` for managed
Falter routers. The package disables upstream self-update checks; updates are
provided by rebuilding and publishing this package. The package also raises the
internal list download timeout to 15 seconds for slower router uplinks.
