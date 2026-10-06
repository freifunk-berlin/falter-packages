"""fstest: integration tests for flowsync in network namespaces (as root).

A test run builds, per scenario, a routed topology (client CL, server SV,
N gateways in between, a sync bridge on the hub) and runs flowsync with its
tc programs on every gateway. Gateways carry per-gateway profiles (the
stateless ACK rule), and the runner repeats every scenario across a matrix of
profile combinations, each combination in network namespaces of its own.

  python3 -m fstest --flowsync PATH --bpf-object PATH [--matrix default] [...]
"""
