# RL-Watchdog: Fast and Predictable SSD Liveness Watchdog on Storage Systems (ATC'24)

## Paper
* https://www.usenix.org/conference/atc24/presentation/ha

## Build & install
* Do the same step to the existing kernel build and installation (NVMe module should not be disabled)
* You can refer to https://docs.kernel.org/kbuild/index.html
* Or, follow the steps below which are valid normally
```bash
make oldconfig
make -j$(nproc) & make modules -j$(nproc) & sudo make modules_install & sudo make install
sudo reboot
```
* RLW module can be found at drivers/nvme/host/nvme-watchdog.ko

## Usage
```bash
insmod nvme-watchdog.ko device_list=[DEVICE_NAME] timeout_ms=[TIMEOUT] polling_duration_ms=[HBI] max_kiops=[MAX_KIOPS]
```
* Example1 (target device: /dev/nvme0n1, timeout: **RLTP on**, HBI: 256ms, max_kiops=100 KIOPS)
```bash
insmod nvme-watchdog.ko device_list=nvme0 timeout_ms=0 polling_duration_ms=256 max_kiops=100
```
* Example2 (target device: /dev/nvme0n1, timeout: **1000ms**, HBI: 256ms, max_kiops=100 KIOPS)
```bash
insmod nvme-watchdog.ko device_list=nvme0 timeout_ms=1000 polling_duration_ms=256 max_kiops=100
```
