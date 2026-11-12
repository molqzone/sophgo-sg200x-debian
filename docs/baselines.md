# Working baselines

One page so two people can name the same state.  Every tag below was flashed and measured
on the board; the measurements are in the tag message of the respective repository.

## `baseline-2026-11-11`

| Component | Repository | Commit |
|---|---|---|
| C906L firmware | `bsp-guidance-action` @ `refactor/shared-page-contract` | `3959800` |
| LL driver | `sgll` (submodule `sg200x-ll-driver`) | `57cae2e` |
| Vision app | `bsp-guidance-vision` @ `main` | `77ebc28` |
| Camera pipeline | `libcvimpp` (submodule) | `ddba481` |
| libxr | submodule of both apps | `9193bd2` |
| Shared page | submodule `shared-page-xr` | `2b3db3b` |
| Image | `sophgo-sg200x-debian` @ `main` | `3de8796` |
| Vendor firmware (baseline.elf) | `components/guidance-fw/firmware/c906-mcu.elf` | `02f36c3a` |

## What this baseline is known to do

- The little core runs a 1 kHz tick from DW Timer4 and **never writes the CLINT**, so
  starting it can no longer freeze the big core (`mtimecmp[0]` is shared: both cores report
  mhartid 0).
- Both sides come up from a power cycle: `guidance-core.timer` at 20 s submits the core
  start through `systemd-run --no-block` (running `rtos-mode` as a job of its own makes the
  systemd queue depend on itself), then `guidance-vision.service` waits for the shared
  page's telemetry head to advance -- the magic alone survives in DDR from the previous
  boot -- and starts the camera, detector and RTSP server.
- Measured after a power cycle: telemetry 20 Hz, IMU 999 Hz, camera 180 fps, detector
  running, `rtsp://<board>:8554/live` serving H.264 plus the metadata track, command frames
  accepted.

## Known problems at this baseline

- The IMU intermittently stops feeding the loop (`png gyro=0`) with **no** `BMI088 wait
  timeout` lines, i.e. the worker thread stops rather than the data-ready interrupt
  missing.  It comes back on a core restart.  This is the next thing to chase.
- The RTSP stream is 320x240 while the detector works on the 640x480 source; the metadata
  track carries `sw`/`sh` so a client can scale the box.

## Rules that came out of the bring-up

1. `/boot/baseline.elf` is loaded **by the boot chain**.  Never put a firmware there that
   has not been verified to survive the boot, or the board cannot come back without
   re-imaging the card.
2. `rtos-mode remoteproc` unloads the camera chain on its way through, so the camera
   modules are reloaded after the core starts.
3. The clock gate for `clk_timer4` is opened in the bring-up (step ⑤, after the SPI reset
   and pads).  Without it every write to the DW timer vanished into an unclocked slave.

## 自启链路（`guidance-autostart` addon）

镜像自带下面全部内容，**刷写后不需要任何手工注入**：

| 文件 | 作用 |
|---|---|
| `/usr/local/bin/guidance_vision_app` | 视觉应用（相机 / 检测 / RTSP / 录制） |
| `/lib/firmware/c906-mcu.elf` | 小核 DW-timer 固件 |
| `/mnt/data/sensor_cfg.ini` | `SMS_SC035HGS_MIPI_480P_180FPS_10BIT` |
| `/mnt/system/usr/lib/libsns_{sc035hgs,full}.so` | 厂商分区缺的 SC035HGS 传感器库 |
| `/usr/local/bin/loadmedia.sh` | 相机内核模块加载 |
| `/usr/local/bin/guidance-core-{guard,health}.sh` | 放核守卫与健康标记 |
| `/etc/systemd/system/guidance-{core,vision,health}.*` | 三个单元 |
| `/etc/systemd/system/load-systemko.service.d/50-guidance-autostart.conf` | 把厂商放核变成空操作 |

放核顺序（**只有一个放核入口，且发生在相机启动之前**）：

1. `guidance-core.timer`（+20 s）→ `guidance-core.service`
2. `systemd-run --no-block rtos-mode remoteproc`（脱离 systemd 作业队列，避免自依赖）
3. `ExecStartPost` 等 `remoteproc` 真的 `running` 才返回
4. `guidance-vision.service` 排在它之后启动（`After=` + `Requires=`），此时
   `rtos-mode` 的 `stop_media_chain` 面对的是**空闲的**相机链——在应用推流时卸载
   相机驱动是唯一能把大核带死的路径，用顺序彻底消掉，不靠兜底。
5. 厂商的 `load-systemko` 被 `50-` drop-in 变成空操作，所以不存在"两个入口各放一次核"。

实测（同一张卡、连续 3 次上电）：`remoteproc0 = running`、`guidance-core` 与
`guidance-vision` 均 active、相机 180 fps、`[page-telemetry] up`、命令
`accepted 20 / refused 0`、RTSP 解出帧、`/home/debian/guidance-capture.h264` 持续增长
（约 5 MB/100 s），`boot-ok` 与当次 `boot_id` 一致。
