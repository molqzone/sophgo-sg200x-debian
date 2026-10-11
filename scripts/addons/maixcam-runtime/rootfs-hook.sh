#!/bin/sh
set -eu

storage=$(cat /tmp/install/storage)
root_partition=2
if [ "$storage" = "emmc" ]; then
  root_partition=1
fi

mkdir -p /etc/systemd/system/finalize-image.service.d
cat > /etc/systemd/system/finalize-image.service.d/10-maixcam.conf <<EOF
[Service]
ExecStartPre=
ExecStartPre=-/usr/sbin/parted -s -f /dev/mmcblk0 resizepart $root_partition 100%
ExecStartPre=-/usr/sbin/resize2fs /dev/mmcblk0p$root_partition
ExecStartPre=-/bin/sh -c "if [ ! -e /swapfile ]; then fallocate -l 1024M /swapfile && chmod 600 /swapfile && mkswap /swapfile; fi"
ExecStartPre=-/bin/sh -c "grep -q '^/swapfile ' /etc/fstab || echo '/swapfile swap swap defaults 0 0' >> /etc/fstab"
ExecStartPre=-/sbin/swapon /swapfile
ExecStartPre=-/bin/sh -c "if [ -e /dev/hwrng ]; then dd if=/dev/hwrng of=/dev/urandom count=1 bs=4096; fi"
ExecStartPre=-/bin/sh -c "/bin/rm -f -v /etc/ssh/ssh_host_*_key*"
EOF

systemctl enable fake-hwclock-load.service fake-hwclock-save.timer || true
ln -sf /usr/bin/python3 /usr/bin/python
mkdir -p /etc/default
printf 'LANG=C.UTF-8\n' > /etc/default/locale

systemctl disable networking.service 2>/dev/null || true
systemctl mask proc-sys-fs-binfmt_misc.automount 2>/dev/null || true
systemctl mask proc-sys-fs-binfmt_misc.mount 2>/dev/null || true
systemctl mask systemd-binfmt.service 2>/dev/null || true
systemctl disable e2scrub_reap.service e2scrub_all.timer 2>/dev/null || true
touch /etc/.updated /var/.updated

sed -i '/^kernel\.sysrq[[:space:]]*=.*/d' /etc/sysctl.conf 2>/dev/null || true
find /etc/sysctl.d /usr/lib/sysctl.d /lib/sysctl.d -maxdepth 1 -type f -name '*.conf' \
  -exec sed -i '/^kernel\.sysrq[[:space:]]*=.*/d' {} + 2>/dev/null || true
rm -f /etc/sysctl.d/10-magic-sysrq.conf /usr/lib/sysctl.d/10-magic-sysrq.conf /lib/sysctl.d/10-magic-sysrq.conf

mkdir -p /etc/modprobe.d
cat > /etc/modprobe.d/maixcam-blacklist.conf <<'EOF'
blacklist cvitek_mailbox
blacklist cvitek_remoteproc
blacklist aic8800_bsp
blacklist aic8800_fdrv
blacklist aic8800_btlpm
blacklist autofs4
install autofs4 /bin/true
blacklist adc_cvitek
blacklist rtc_cvitek
blacklist pwm_cvitek
EOF
