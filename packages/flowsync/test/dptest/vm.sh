#!/bin/bash
# Run dptest as root in a throwaway VM on the host's kernel (virtme-ng).
#
#   VNG=/path/to/vng dptest/vm.sh OUT_DIR --flowsync PATH --bpf-object PATH [...]
#
# OUT_DIR (on the host) receives the logs of every run and the progress log;
# everything after it goes to `python3 -m dptest`. Paths must be absolute.
# DPTEST_BYPASS=1 is passed on.
HERE=$(cd "$(dirname "$0")" && pwd)

if [ -z "$DPTEST_INSIDE" ]; then
	VNG=${VNG:-$(command -v vng)}
	[ -x "$VNG" ] || { echo "no vng (virtme-ng): set VNG=/path/to/vng"; exit 1; }
	OUT=$(mkdir -p "$1" && cd "$1" && pwd) || exit 1
	shift
	export PATH="$(dirname "$VNG"):$PATH"
	cd "$OUT" || exit 1
	exec "$VNG" -r --user root --rwdir "$OUT" --cpus "${VM_CPUS:-16}" --memory "${VM_MEM:-8G}" -- \
		"DPTEST_INSIDE=1 DPTEST_OUT=$OUT DPTEST_BYPASS=$DPTEST_BYPASS bash $HERE/vm.sh $*"
fi

mountpoint -q /sys/fs/bpf || mount -t bpf bpf /sys/fs/bpf
# sch_ingress and cls_bpf are not the datapath's (it uses tcx hooks), the
# ingress_qdisc scenario puts them on an uplink as SQM would
for m in sch_ingress ifb act_mirred cls_matchall sch_fq_codel nf_tables nft_fib_inet nft_ct \
	nft_reject_inet nft_numgen nft_limit sch_netem veth bridge ip_gre wireguard; do
	modprobe $m 2> /dev/null
done
sysctl -qw net.core.rmem_max=16777216 net.core.wmem_max=16777216
# the daemon drops its capabilities and writes its status file as plain
# root: the runs go to a directory root owns and are copied out afterwards
rm -rf /tmp/dptest-out
cd "$HERE/.." || exit 1
python3 -m dptest "$@" --out /tmp/dptest-out --log "$DPTEST_OUT/progress.log" --keep
rc=$?
cp -r /tmp/dptest-out/. "$DPTEST_OUT/"
chmod -R a+rX "$DPTEST_OUT"
exit $rc
