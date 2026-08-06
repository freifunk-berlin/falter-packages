#!/bin/sh

set -e

uci add_list uhttpd.main.ucode_prefix='/falter=/usr/share/ucode/falter/uhttpd.uc'
