##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = alioth
  PLATFORM_GUID                  = 7F67CDAE-8312-4ECC-B948-D86C870D04D9
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/aliothPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = aliothPkg/alioth.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

  #
  # 0 = SM8250
  # 1 = SM8250-AB
  # 2 = SM8250-AC
  #
  SOC_TYPE                       = 2

!include KonaPkg/KonaPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # Keep UniXNU's early diagnostics visible on the frame-buffer console.
  #
  gUniXNUPkgTokenSpaceGuid.PcdUniXNUDebugLevel|0x80000000

  #
  # The name AdvancedLogDxe puts in the exported log file. log-capture step 1
  # asks for the board's ASCII model name here.
  #
  gUniXNUPkgTokenSpaceGuid.PcdUniXNUDeviceModel|"alioth"

  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x9FF90000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"alioth"
!if $(DEVICE_MODEL) == 0
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"POCO F3"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"M2012K11AG"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"K11AG"
!elseif $(DEVICE_MODEL) == 1
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Redmi K40"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"M2012K11AC"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"K11AC"
!elseif $(DEVICE_MODEL) == 2
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Mi 11X"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"M2012K11AI"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"K11AI"
!endif

  #
  # Simple Frame Buffer
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1080
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2400
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|aliothPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|aliothPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

[Components]
  #
  # ACPI Tables
  #
  alioth/AcpiTables.inf

  #
  # UniXNU: boot manager, registrar, payload block device and XNU loader
  #
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  shengPkg/Drivers/ShengBootMemoryDxe/ShengBootMemoryDxe.inf
  UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf
  DarwinBootPkg/Driver/DarwinBoot.inf
  # Tools -> Advanced Logger: pages the firmware log out as QR codes.
  UniXNUPkg/Driver/AdvancedLogDxe/AdvancedLogDxe.inf

[LibraryClasses]
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
  #
  # Advanced Logger. The DXE Core instance is what installs
  # gAdvancedLoggerProtocolGuid (AdvancedLoggerLib/DxeCore/AdvancedLoggerLib.c:480);
  # the Dxe instance only locates it. Without the DXE_CORE mapping no module installs
  # the protocol, AdvancedLogDxe's Depex never resolves, and Tools stays absent.
  # DXE_DRIVER and UEFI_APPLICATION route DEBUG() into the same buffer so the menu,
  # payload and DarwinBoot lines are in the exported log.
  #
  AdvancedLoggerAccessLib|AdvLoggerPkg/Library/AdvancedLoggerAccessLib/AdvancedLoggerAccessLib.inf
  QrEncoderLib|MsGraphicsPkg/Library/QrEncoderLib/QrEncoderLib.inf
  # BaseDebugLibAdvancedLogger asserts through this library; the platform had no
  # AssertLib instance because the inherited DebugLib never needed one.
  AssertLib|AdvLoggerPkg/Library/AssertLib/AssertLib.inf
  # The log lives in memory and is paged out by AdvancedLogDxe; the board has no
  # separate hardware debug port, and the inherited SerialPortLib still drives the
  # frame-buffer console, so no second writer is added here.
  AdvancedLoggerHdwPortLib|AdvLoggerPkg/Library/AdvancedLoggerHdwPortLibNull/AdvancedLoggerHdwPortLibNull.inf

[LibraryClasses.common.DXE_CORE]
  DebugLib|AdvLoggerPkg/Library/BaseDebugLibAdvancedLogger/BaseDebugLibAdvancedLogger.inf
  AdvancedLoggerLib|AdvLoggerPkg/Library/AdvancedLoggerLib/DxeCore/AdvancedLoggerLib.inf

[LibraryClasses.common.DXE_DRIVER]
  DebugLib|AdvLoggerPkg/Library/BaseDebugLibAdvancedLogger/BaseDebugLibAdvancedLogger.inf
  AdvancedLoggerLib|AdvLoggerPkg/Library/AdvancedLoggerLib/Dxe/AdvancedLoggerLib.inf

[LibraryClasses.common.UEFI_APPLICATION]
  DebugLib|AdvLoggerPkg/Library/BaseDebugLibAdvancedLogger/BaseDebugLibAdvancedLogger.inf
  AdvancedLoggerLib|AdvLoggerPkg/Library/AdvancedLoggerLib/Dxe/AdvancedLoggerLib.inf
