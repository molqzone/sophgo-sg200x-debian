#!/bin/sh
# Assign the management address to usb0 without depending on ifupdown.
#
# etc/network/interfaces.d/usb0 declares "allow-hotplug usb0" with a static
# 10.42.0.1/24, but the image disables the networking service that would
# process hotplug events (scripts/addons/network-services), so the address
# is frequently never applied.  dnsmasq then cannot bind to its
# --listen-address and the board comes up with no management IP.
#
# Called from usb-gadget-ncm.service's ExecStartPost, right after the UDC
# is bound.  The netdev can appear a moment after the bind, so retry.
i=0
while [ "$i" -lt 100 ]; do
    if ip addr replace 10.42.0.1/24 dev usb0 2>/dev/null; then
        ip link set usb0 up 2>/dev/null
        exit 0
    fi
    i=$((i + 1))
    sleep 0.1
done
echo "usb0-ip: usb0 never appeared" >&2
exit 1
