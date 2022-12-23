#
# This is free software, licensed under the GNU General Public License v3.0 .
#

LUCIMKFILE:=$(wildcard $(TOPDIR)/feeds/*/luci.mk)

# verify that there is only one single file returned
ifneq (1,$(words $(LUCIMKFILE)))
ifeq (0,$(words $(LUCIMKFILE)))
$(error did not find luci.mk in any feed)
else
$(error found multiple luci.mk files in the feeds)
endif
else
#$(info found luci.mk at $(LUCIMKFILE))
endif

include $(LUCIMKFILE)
