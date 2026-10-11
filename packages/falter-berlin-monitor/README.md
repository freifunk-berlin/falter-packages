# falter berlin monitor

Replacement for owm.sh and collectd.

This package adds a prometheus exporter with additions for our custom freifunk metrics.

## Todo

- Everything
- Only add labels when they have a value
- add Cronjob `curl http://localhost:9101/metrics | curl --data-binary @- http://10.31.130.151:9091/metrics/job/nodes/instance/$(uci get system.@system[0].hostname)`
- fix Metrics
- Think about how to design the metrics correctly
- Lucy config page
    - Enable sending metrics
    - enable beeing shown on map (otherwise only send metrics)
    - custom backend
