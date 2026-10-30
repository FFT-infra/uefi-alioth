# Firmware agent guide

This repository is the Mu-Silicium UEFI port for the Redmi K40 (`alioth`,
SM8250-AC). It publishes `SILICIUM_UEFI.fd`, which UniXNU boots before loading
DarwinBoot and the kernel. The board platform lives under
`Platforms/Xiaomi/aliothPkg/`.

## Read first

| Task | Required reading |
| --- | --- |
| Every change | [UEFI firmware](https://unixnu.com/zh/porting/uefi-firmware), [required integration](https://unixnu.com/zh/porting/uefi-firmware#required-integration) |
| Board platform | [Device tree](https://unixnu.com/zh/porting/device-tree), `Platforms/Xiaomi/aliothPkg/` and `Silicon/Qualcomm/KonaPkg/` |
| Boot arguments | [Boot arguments](https://unixnu.com/zh/xnu/boot-arguments), [apply arguments](https://unixnu.com/zh/xnu/boot-arguments#apply-arguments) |
| Diagnostics | [Log capture](https://unixnu.com/zh/porting/log-capture), [green-bar diagnosis](https://unixnu.com/zh/guide/debugging#green-bar) |
| This board | [Device page](https://unixnu.com/zh/devices/alioth), [`../UniXNU-alioth/porting.toml`](../UniXNU-alioth/porting.toml) |

Upstream is [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium).
Merge it as a normal upstream; board changes stay in `Platforms/Xiaomi/aliothPkg/`
and `Resources/`.

## Required integration

`porting/uefi-firmware` requires these six modules in **both** the DSC
`[Components]` and the FDF firmware volume:

| Module | Purpose |
| --- | --- |
| `UniXNUPkg/Application/UniXNU/UniXNU.inf` | boot menu and configuration |
| `UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf` | registers the firmware boot entry |
| `DarwinBootPkg/Driver/DarwinBoot.inf` | XNU loader |
| `UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf` | publishes the Mu-Silicium initrd as a read-only block device |
| `MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf` | disk access for the filesystem drivers |
| `FatPkg/EnhancedFatDxe/Fat.inf` | publishes the payload's FAT filesystem |

`Platforms/Xiaomi/aliothPkg/alioth.dsc` lists the first four plus the ACPI
table and `ShengBootMemoryDxe`. `Platforms/Xiaomi/aliothPkg/Include/DXE.inc`,
included by `alioth.fdf`, adds `DiskIoDxe`, `PartitionDxe`, `Fat` and
`EnglishDxe` as prebuilt binaries from `Binaries/alioth/`.

**Known gap (2026-10-30):** `DiskIoDxe` and `Fat` appear only in the FDF, not
in the DSC `[Components]`. The firmware builds and boots, because those two
enter as prebuilt PE32 binaries rather than compiled components, but the
guide asks for both places. Adding them to the DSC is the correct fix; it has
not been done yet and is not verified to change the built image.

## Build

From the UniXNU checkout:

```sh
./unixnu build firmware -D ../UniXNU-alioth --uefi .
```

`firmware.py` reads `git log` from this tree and applies
`Resources/MuPatches/Gic-Spurious.patch` to the EDK2 tree, so both this
repository and the EDK2 checkout must be complete git trees, not mount
points. The output is `Build/aliothPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd`
(3,145,728 bytes, `0x300000`).

## Firmware log export

`porting/log-capture` section 1 asks for `AdvancedLogDxe` and
`AdvancedLoggerLib` in the DSC and FDF, plus the Tools menu entry. **Neither
is integrated here yet**, so firmware-stage logs come only from the USB
console and photographs. The driver itself exists in
`../UniXNU/package/UniXNUPkg/Driver/AdvancedLogDxe/`; what is missing is the
wiring.

## Layout

| Path | Contents |
| --- | --- |
| `Platforms/Xiaomi/aliothPkg/` | board DSC, FDF, ACPI tables, libraries, `Include/DXE.inc` |
| `Silicon/Qualcomm/KonaPkg/` | SM8250 platform defines, PCDs and library classes |
| `Resources/DTBs/` | the board DTB `alioth.dtb` that `bootimg --dtb` packs |
| `Resources/MuPatches/` | patches applied to the EDK2 tree at build time |
| `Binaries/alioth/` | prebuilt drivers this platform does not compile |
| `Mu_Basecore/` | the EDK2 tree, a submodule |
