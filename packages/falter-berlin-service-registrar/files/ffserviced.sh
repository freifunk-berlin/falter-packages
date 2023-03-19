#!/bin/sh /etc/rc.common

# Just looks for changes in the config-file and applies them with a
# one-time-run.

# shellcheck disable=SC2034
# shellcheck disable=SC2102

USE_PROCD=1

service_triggers() {
    procd_add_reload_trigger "ffservices"
}

start_service() {
    procd_open_instance
    procd_set_param command /usr/bin/register-services

    procd_set_param stdout 1
    procd_set_param stderr 1
    procd_set_param pidfile /var/run/ffserviced.pid
    procd_set_param term_timeout 60
    procd_close_instance
}
