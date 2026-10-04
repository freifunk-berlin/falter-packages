"""fstest: integration tests for flowsync in unprivileged network namespaces.

A test run builds, per scenario, a routed topology (client CL, server SV,
N gateways in between, a sync bridge on the hub) and runs flowsync on every
gateway. Gateways carry per-gateway profiles (flow offload, stateless ACK
rule), and the runner repeats every scenario across a matrix of profile
combinations, each combination isolated in its own user namespace.

  python3 -m fstest --flowsync PATH --ctquery PATH [--matrix default] [...]
"""
