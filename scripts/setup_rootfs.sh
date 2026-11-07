#!/bin/sh
#
set -ex

BOARD=$(cat /tmp/install/board)
HOSTNAME=$(cat /tmp/install/hostname)
STORAGETYPE=$(cat /tmp/install/storage)


export DEBIAN_FRONTEND=noninteractive DEBCONF_NONINTERACTIVE_SEEN=true
export LC_ALL=C LANGUAGE=C LANG=C

/var/lib/dpkg/info/base-passwd.preinst install || true
/var/lib/dpkg/info/sgml-base.preinst install || true

mkdir -p /etc/sgml
mount proc -t proc /proc
mount -B sys /sys
mount -B run /run
mount -B dev /dev
#mount devpts -t devpts /dev/pts
dpkg --configure -a

unset DEBIAN_FRONTEND DEBCONF_NONINTERACTIVE_SEEN

#
# Change root password to 'rv'
#
usermod --password "$(echo rv | openssl passwd -1 -stdin)" root

#
# Add a new user debian and its passwd is `rv`
#
mkdir -p /home/debian
useradd --password dummy \
    -G cdrom,floppy,sudo,audio,dip,video,plugdev \
    --home-dir /home/debian --shell /bin/bash debian || true
chown debian:debian /home/debian
# Set password to 'debian'
usermod --password "$(echo rv | openssl passwd -1 -stdin)" debian || true

# Set up fstab
cat > /etc/fstab <<EOF
# <file system> <mount point>   <type>  <options>                 <dump>  <pass>
/dev/root       /               auto    defaults                  1       1
EOF

if [ "$STORAGETYPE" = "sd" ]; then
  cat >> /etc/fstab <<EOF
/dev/mmcblk0p1  /boot           auto    defaults                  1       2
EOF
fi



#regenerate SSH keys on first boot
cat > /etc/systemd/system/finalize-image.service <<EOF
[Unit]
Description=Finalize the Image
Before=ssh.service

[Service]
Type=oneshot
ExecStartPre=-/usr/sbin/parted -s -f /dev/mmcblk0 resizepart 2 100%
ExecStartPre=-/usr/sbin/resize2fs /dev/mmcblk0p2
ExecStartPre=-/bin/dd if=/dev/hwrng of=/dev/urandom count=1 bs=4096
ExecStartPre=-/bin/sh -c "/bin/rm -f -v /etc/ssh/ssh_host_*_key*"
ExecStart=/usr/bin/ssh-keygen -A -v
ExecStartPost=/bin/systemctl disable finalize-image

[Install]
WantedBy=multi-user.target
EOF

if [ "$STORAGETYPE" = "emmc" ]; then
sed -i -e 's|ExecStartPre=-/usr/sbin/parted -s -f /dev/mmcblk0 resizepart 2 100%|ExecStartPre=-/usr/sbin/parted -s -f /dev/mmcblk0 resizepart 1 100%|' /etc/systemd/system/finalize-image.service
fi

cat /etc/systemd/system/finalize-image.service

apt_install_flags="-y -f"
if [ -f /tmp/install/apt-allow-downgrades ]; then
  apt_install_flags="$apt_install_flags --allow-downgrades"
fi
apt install $apt_install_flags /tmp/install/*.deb


# change device tree
echo "===== ln -s dtb files ====="
file_prefix="/usr/lib/linux-image-*"

file_path=$(ls -d $file_prefix)

if [ -e "$file_path" ]; then
  echo "File found: $file_path"
  lib_dir=$file_path
else
  echo "File not found: $file_path"
fi

kernel_image=${lib_dir##*/}

# set default dtb file, please verify your board version
mkdir -p /boot/fdt/${kernel_image}

cp ${lib_dir}/cvitek/*.dtb /boot/fdt/${kernel_image}/


cat /boot/extlinux/extlinux.conf

if [ "$STORAGETYPE" = "sd" ]; then
sed -i -i 's|#U_BOOT_PARAMETERS=".*"|U_BOOT_PARAMETERS="console=ttyS1,115200 earlycon=uart8250,mmio32,0x04150000,115200 root=/dev/mmcblk0p2 rootwait rw"|' /etc/default/u-boot
else
sed -i -i 's|#U_BOOT_PARAMETERS=".*"|U_BOOT_PARAMETERS="console=ttyS1,115200 earlycon=uart8250,mmio32,0x04150000,115200 root=/dev/mmcblk0p1 rootwait rw"|' /etc/default/u-boot
fi

sed -i -e 's|#U_BOOT_SYNC_DTBS=".*"|U_BOOT_SYNC_DTBS="true"|' /etc/default/u-boot
#doing this dance, as in the chroot, / and /boot are same filesystem, so systemctl enable serial-getty@ttyS1.service
#u-boot-update doesn't setup correctly
echo "U_BOOT_FDT_DIR=\"/usr/lib/linux-image-$BOARD-\"" >> /etc/default/u-boot
u-boot-update
if [ "$STORAGETYPE" = "sd" ]; then
  sed -i -e 's|fdtdir /usr/lib/|fdtdir /fdt/|' /boot/extlinux/extlinux.conf
  sed -i -e 's|linux /boot/|linux /|' /boot/extlinux/extlinux.conf
  sed -i -e "s|U_BOOT_FDT_DIR=\".*\"|U_BOOT_FDT_DIR=\"/fdt/linux-image-$BOARD-\"|" /etc/default/u-boot
else 
  sed -i -e 's|fdtdir /usr/lib/|fdtdir /boot/fdt/|' /boot/extlinux/extlinux.conf
fi

cat /boot/extlinux/extlinux.conf

# Set hostname
cat /tmp/install/hostname > /etc/hostname

# 
cat >> /etc/hosts << EOF
127.0.0.1      ${HOSTNAME} 
EOF

# 
# Enable system services
#
systemctl enable finalize-image.service
if [ -f /tmp/install/systemd-enable ]; then
  systemctl enable `cat /tmp/install/systemd-enable`
fi

#
# 5.10 的 riscv 没有 arch RNG（CONFIG_ARCH_RANDOM 不存在），冷启动要等 crng init——实测
# 10.9 秒，而 basic.target 就堵在它后面（systemd-random-seed 需要熵）。镜像里预置一份
# 随机种子，systemd-random-seed 会在启动早期把它塞进内核熵池，crng 立即就绪。每个镜像
# 种子不同（构建时从 /dev/urandom 取），不是写死的常量。
# 5.10 on riscv has no arch RNG, so a cold boot waits ~11 s for crng init with
# basic.target stuck behind it.  Seed the pool from the image instead: the build writes
# per-image bytes and systemd-random-seed credits them early.
dd if=/dev/urandom of=/var/lib/systemd/random-seed bs=256 count=1 status=none 2>/dev/null \
	|| true
chmod 0600 /var/lib/systemd/random-seed 2>/dev/null || true

# 板上没有人读文档/手册，也不需要几十种语言；这些都是镜像体积和冷插拔时间的纯开销。
# A board nobody reads man pages on does not need them; this is pure image size and
# coldplug work.  zh* and C stay so the console and our tools keep working.
#
rm -rf /usr/share/doc/* /usr/share/man/* /usr/share/info/* 2>/dev/null || true
find /usr/share/locale -mindepth 1 -maxdepth 1 -type d ! -name 'zh*' ! -name 'C*' \
	-exec rm -rf {} + 2>/dev/null || true
rm -rf /var/lib/apt/lists/* /var/cache/apt/* 2>/dev/null || true

# Update source list 

rm -rf /etc/apt/sources.list.d/multistrap-debian.list

if [ -f /tmp/install/public-key.asc ]; then
  install -d -m 0755 /etc/apt/keyrings
  install -m 0644 /tmp/install/public-key.asc /etc/apt/keyrings/sophgo.asc
  cat > /etc/apt/sources.list <<EOF
deb http://deb.debian.org/debian sid main non-free-firmware
deb [signed-by=/etc/apt/keyrings/sophgo.asc] https://sophgo.my-ho.st:8443/ debian sophgo
EOF
else
  cat > /etc/apt/sources.list <<EOF
deb http://deb.debian.org/debian sid main non-free-firmware
EOF
fi

for hook in /tmp/install/rootfs-hooks/*.sh; do
  [ -f "$hook" ] || continue
  /bin/sh "$hook"
done

# 启动阶段标记：U-Boot 在 bootcmd 之前交替写 /stage-a、/stage-b。板子挂死之后**不要断电**，
# 直接把卡拔下来看：标记翻了一个，就说明这次的复位走到了 U-Boot；一个都没翻，说明复位根本
# 没到 U-Boot（关机卡住，或者 FSBL/ROM 复位后起不来）。热复位到底死在哪一步，靠这个就能切
# 成两半，而不是靠灯猜。`fatrm` 失败只会让两个文件同时存在，仍然能看出"这次跑过"。
# U-Boot toggles /stage-a and /stage-b before bootcmd runs.  After a hang, pull the card
# (do not power cycle) and look: a new marker means the reset reached U-Boot; no new
# marker means it never got there.
if ! grep -q '^preboot=' /etc/u-boot-initial-env; then
	printf '%s\n' 'preboot=if test -e mmc 0:1 /stage-a; then fatwrite mmc 0 ${loadaddr} /stage-b 4; fatrm mmc 0 /stage-a; else fatwrite mmc 0 ${loadaddr} /stage-a 4; fatrm mmc 0 /stage-b; fi' >> /etc/u-boot-initial-env
fi

# journal 默认只写 /run，掉电即失——而板子最需要的恰好是"上一次启动死在哪"。持久化到
# /var/log/journal 并限制大小（16 MiB），挂死后再上电就能 journalctl -b -1 看到关机序列。
# The journal defaults to /run, so a hang leaves nothing behind.  Persist it, bounded.
install -d -m 2755 -o root -g systemd-journal /var/log/journal 2>/dev/null || true
install -d -m 0755 /etc/systemd/journald.conf.d 2>/dev/null || true
cat > /etc/systemd/journald.conf.d/10-persistent.conf <<'JOURNAL'
[Journal]
Storage=persistent
SystemMaxUse=16M
JOURNAL

echo "/boot/uboot.env	0x0000          0x20000" > /etc/fw_env.config
mkenvimage -s 0x20000 -o /boot/uboot.env /etc/u-boot-initial-env


#
# Clean apt cache on the system
#
apt-get clean


rm -rf /var/cache/*
find /var/lib/apt/lists -type f -not -name '*.gpg' -print0 | xargs -0 rm -f
find /var/log -type f -print0 | xargs -0 truncate --size=0
