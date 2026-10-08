"""The five-gateway topology. Numbers: ms one-way per traversal of a link.

   client A      client B
        \\          /
     [ Freifunk mesh ]
     |3   |8   |10  |3   |1      mesh link
    gw1  gw2  gw3  gw4  gw5
     |6   |6   |3   |2   |5      uplink (user traffic and sync)
       [ Internet ]
        /          \\
   server A (3)   server B (5)
"""
GW = ["gw1", "gw2", "gw3", "gw4", "gw5"]

TOPOLOGY = dict(
    gateways=dict(gw1=dict(mesh=3,  uplink=6),
                  gw2=dict(mesh=8,  uplink=6),
                  gw3=dict(mesh=10, uplink=3),
                  gw4=dict(mesh=3,  uplink=2),
                  gw5=dict(mesh=1,  uplink=5)),
    clients=dict(A=0, B=0),
    servers=dict(A=3, B=5),
)

# what a gateway's firewall allows besides known flows: bypass = the stateless,
# rate-limited accept for TCP segments with ACK or RST
PROD, STRICT = dict(bypass=True), dict(bypass=False)
FLEETS = dict(
    prod={g: PROD for g in GW},                         # as deployed
    strict={g: STRICT for g in GW},                     # nothing but known flows
    mixed=dict(gw1=PROD, gw2=STRICT,                    # partial offload
               gw3=dict(bypass=True, offload=True),
               gw4=dict(bypass=False, offload=True),
               gw5=STRICT),
)
