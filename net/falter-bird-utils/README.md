# falter-bird-utils

This package provides a UCI-to-Bird2 configuration translator for Falter mesh nodes.

## Features
- **Procd Integration:** Automatically regenerates Bird2 config and reloads the daemon on UCI changes.
- **Dynamic Router ID:** Automatically detects the primary IPv4 address (e.g., from `br-lan`).
- **Memory Optimized:** Writes configuration to `/var/etc` to protect flash storage.
- **Link-Local Support:** Handles IPv6 interface scoping for BGP neighbors.

## UCI Configuration
The configuration is managed via `/etc/config/bird`.