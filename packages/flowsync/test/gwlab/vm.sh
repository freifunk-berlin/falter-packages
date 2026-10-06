#!/bin/bash
# Run gwlab as root in a throwaway VM on the host's kernel (virtme-ng), for
# implementations that need real root, or to have all of them in the same
# environment.
#
#   VNG=/path/to/vng gwlab/vm.sh OUT_DIR steady --impl flowsync [--set bin=... ...]
#
# OUT_DIR (on the host) receives the results; everything after it goes to
# `python3 -m gwlab`. Paths in --set must be absolute.
HERE=$(cd "$(dirname "$0")" && pwd)

if [ -z "$GWLAB_INSIDE" ]; then
	VNG=${VNG:-$(command -v vng)}
	[ -x "$VNG" ] || { echo "no vng (virtme-ng): set VNG=/path/to/vng"; exit 1; }
	OUT=$(mkdir -p "$1" && cd "$1" && pwd) || exit 1
	shift
	export PATH="$(dirname "$VNG"):$PATH"
	cd "$OUT" || exit 1
	exec "$VNG" -r --user root --rwdir "$OUT" --cpus "${VM_CPUS:-16}" --memory "${VM_MEM:-8G}" -- \
		"GWLAB_INSIDE=1 GWLAB_OUT=$OUT bash $HERE/vm.sh $*"
fi

mountpoint -q /sys/fs/bpf || mount -t bpf bpf /sys/fs/bpf
for m in nf_tables nft_fib_inet nft_ct nft_reject_inet nft_limit \
	nft_flow_offload nf_conntrack_netlink sch_netem veth bridge; do
	modprobe $m 2> /dev/null
done
sysctl -qw net.core.rmem_max=16777216 net.core.wmem_max=16777216
# daemons that drop their privileges write next to their logs: a directory
# root owns, copied out afterwards
rm -rf /tmp/gwlab-out
cd "$HERE/.." || exit 1
python3 -m gwlab "$@" --out /tmp/gwlab-out
rc=$?
cp -r /tmp/gwlab-out/. "$GWLAB_OUT/"
chmod -R a+rX "$GWLAB_OUT"
exit $rc
