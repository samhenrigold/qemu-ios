arm_ss = ss.source_set()
arm_ss.add(files('boot.c'))
arm_ss.add(when: 'CONFIG_ARM_VIRT', if_true: files('virt.c'))
arm_ss.add(when: 'CONFIG_ACPI', if_true: files('virt-acpi-build.c'))
arm_ss.add(when: 'CONFIG_DIGIC', if_true: files('digic_boards.c'))
arm_ss.add(when: 'CONFIG_EMCRAFT_SF2', if_true: files('msf2-som.c'))
arm_ss.add(when: 'CONFIG_HIGHBANK', if_true: files('highbank.c'))
arm_ss.add(when: 'CONFIG_INTEGRATOR', if_true: files('integratorcp.c'))
arm_ss.add(when: 'CONFIG_MICROBIT', if_true: files('microbit.c'))
arm_ss.add(when: 'CONFIG_MPS3R', if_true: files('mps3r.c'))
arm_ss.add(when: 'CONFIG_MUSICPAL', if_true: files('musicpal.c'))
arm_ss.add(when: 'CONFIG_NETDUINOPLUS2', if_true: files('netduinoplus2.c'))
arm_ss.add(when: 'CONFIG_OLIMEX_STM32_H405', if_true: files('olimex-stm32-h405.c'))
arm_ss.add(when: 'CONFIG_NPCM7XX', if_true: files('npcm7xx.c', 'npcm7xx_boards.c'))
arm_ss.add(when: 'CONFIG_NPCM8XX', if_true: files('npcm8xx.c', 'npcm8xx_boards.c'))
arm_ss.add(when: 'CONFIG_REALVIEW', if_true: files('realview.c'))
arm_ss.add(when: 'CONFIG_SBSA_REF', if_true: files('sbsa-ref.c'))
arm_ss.add(when: 'CONFIG_STELLARIS', if_true: files('stellaris.c'))
arm_ss.add(when: 'CONFIG_STM32VLDISCOVERY', if_true: files('stm32vldiscovery.c'))
arm_ss.add(when: 'CONFIG_ZYNQ', if_true: files('xilinx_zynq.c'))
arm_ss.add(when: 'CONFIG_SABRELITE', if_true: files('sabrelite.c'))

arm_ss.add(when: 'CONFIG_ARM_V7M', if_true: files('armv7m.c'))
arm_ss.add(when: 'CONFIG_EXYNOS4', if_true: files('exynos4210.c'))
arm_ss.add(when: 'CONFIG_DIGIC', if_true: files('digic.c'))
arm_ss.add(when: 'CONFIG_OMAP', if_true: files('omap1.c'))
arm_ss.add(when: 'CONFIG_ALLWINNER_A10', if_true: files('allwinner-a10.c', 'cubieboard.c'))
arm_ss.add(when: 'CONFIG_ALLWINNER_H3', if_true: files('allwinner-h3.c', 'orangepi.c'))
arm_ss.add(when: 'CONFIG_ALLWINNER_R40', if_true: files('allwinner-r40.c', 'bananapi_m2u.c'))
arm_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2836.c', 'raspi.c'))
arm_ss.add(when: ['CONFIG_RASPI', 'TARGET_AARCH64'], if_true: files('bcm2838.c', 'raspi4b.c'))
arm_ss.add(when: 'CONFIG_STM32F100_SOC', if_true: files('stm32f100_soc.c'))
arm_ss.add(when: 'CONFIG_STM32F205_SOC', if_true: files('stm32f205_soc.c'))
arm_ss.add(when: 'CONFIG_STM32F405_SOC', if_true: files('stm32f405_soc.c'))
arm_ss.add(when: 'CONFIG_B_L475E_IOT01A', if_true: files('b-l475e-iot01a.c'))
arm_ss.add(when: 'CONFIG_STM32L4X5_SOC', if_true: files('stm32l4x5_soc.c'))
arm_ss.add(when: 'CONFIG_XLNX_ZYNQMP_ARM', if_true: files('xlnx-zynqmp.c', 'xlnx-zcu102.c'))
arm_ss.add(when: 'CONFIG_XLNX_VERSAL', if_true: files('xlnx-versal.c', 'xlnx-versal-virt.c'))
arm_ss.add(when: 'CONFIG_FSL_IMX25', if_true: files('fsl-imx25.c', 'imx25_pdk.c'))
arm_ss.add(when: 'CONFIG_FSL_IMX31', if_true: files('fsl-imx31.c', 'kzm.c'))
arm_ss.add(when: 'CONFIG_FSL_IMX6', if_true: files('fsl-imx6.c'))
arm_ss.add(when: 'CONFIG_ASPEED_SOC', if_true: files(
  'aspeed.c',
  'aspeed_soc_common.c',
  'aspeed_ast2400.c',
  'aspeed_ast2600.c',
  'aspeed_ast10x0.c',
  'aspeed_eeprom.c',
  'fby35.c'))
arm_ss.add(when: ['CONFIG_ASPEED_SOC', 'TARGET_AARCH64'], if_true: files('aspeed_ast27x0.c'))
arm_ss.add(when: 'CONFIG_MPS2', if_true: files('mps2.c'))
arm_ss.add(when: 'CONFIG_MPS2', if_true: files('mps2-tz.c'))
arm_ss.add(when: 'CONFIG_MSF2', if_true: files('msf2-soc.c'))
arm_ss.add(when: 'CONFIG_MUSCA', if_true: files('musca.c'))
arm_ss.add(when: 'CONFIG_ARMSSE', if_true: files('armsse.c'))
arm_ss.add(when: 'CONFIG_FSL_IMX7', if_true: files('fsl-imx7.c', 'mcimx7d-sabre.c'))
arm_ss.add(when: 'CONFIG_FSL_IMX8MP', if_true: files('fsl-imx8mp.c'))
arm_ss.add(when: 'CONFIG_FSL_IMX8MP_EVK', if_true: files('imx8mp-evk.c'))
arm_ss.add(when: 'CONFIG_ARM_SMMUV3', if_true: files('smmuv3.c'))
arm_ss.add(when: 'CONFIG_FSL_IMX6UL', if_true: files('fsl-imx6ul.c', 'mcimx6ul-evk.c'))
arm_ss.add(when: 'CONFIG_NRF51_SOC', if_true: files('nrf51_soc.c'))
arm_ss.add(when: 'CONFIG_XEN', if_true: files(
  'xen-stubs.c',
  'xen-pvh.c',
))
arm_ss.add(when: 'CONFIG_IPOD_TOUCH_2G', if_true: files('ipod_touch_2g.c', 'ipod_touch_clock.c', 'ipod_touch_chipid.c', 'ipod_touch_gpio.c', 'guest-services.c', 'ipod-agent.c', 'guest-gles.c', 'guest-socket.c', 'guest-fds.c', 'guest-file.c', 'ipod_touch_sysic.c', 'ipod_touch_timer.c', 'ipod_touch_usb_otg.c', 'ipod_touch_tcp_usb.c', 'ipod_touch_usb_phys.c', 'ipod_touch_spi.c', 'ipod_touch_nor_spi.c', 'ipod_touch_sha1.c', 'ipod_touch_aes.c', 'ipod_touch_pke.c', 'ipod_touch_pcf50633_pmu.c', 'ipod_touch_unknown1.c', 'ipod_touch_wdt.c', 'ipod_touch_mpvd.c', 'ipod_touch_h264.c', 'ipod_touch_scaler.c', 'ipod_touch_swi.c', 'ipod_video.c', 'ipod_touch_lcd.c', 'ipod_touch_mipi_dsi.c', 'ipod_touch_fmss.c', 'ipod_touch_mbx.c', 'ipod_touch_cd3272_mikey.c', 'ipod_touch_cs42l58.c', 'ipod_touch_amc.c', 'ipod_touch_i2s.c', 'ipod_touch_lm48821.c', 'ipod_touch_lis302dl.c', 'ipod_touch_tethered.c', 'ipod_touch_multitouch.c', 'ipod_touch_sdio.c', 'ipod_touch_tvout.c', 'ipod_touch_isl29003dl.c', 'ipod_touch_bt.c'))

system_ss.add(when: 'CONFIG_ARM_SMMUV3', if_true: files('smmu-common.c'))
system_ss.add(when: 'CONFIG_COLLIE', if_true: files('collie.c'))
system_ss.add(when: 'CONFIG_EXYNOS4', if_true: files('exynos4_boards.c'))
system_ss.add(when: 'CONFIG_NETDUINO2', if_true: files('netduino2.c'))
system_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2835_peripherals.c'))
system_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2838_peripherals.c'))
system_ss.add(when: 'CONFIG_STRONGARM', if_true: files('strongarm.c'))
system_ss.add(when: 'CONFIG_SX1', if_true: files('omap_sx1.c'))
system_ss.add(when: 'CONFIG_VERSATILE', if_true: files('versatilepb.c'))
system_ss.add(when: 'CONFIG_VEXPRESS', if_true: files('vexpress.c'))

# gles-host.c is one renderer with two context backends: a legacy CGL context
# on macOS (created in gles-host.c itself) and an EAGLContext on iOS
# (gles-host-eagl.m). It picks between them on TARGET_OS_IPHONE, so the choice
# here is only which framework and which extra file to add. A host with neither
# framework gets a stub and the guest's GLES HLE probe fails cleanly.
if host_opengl.found()
  arm_ss.add(when: 'CONFIG_IPOD_TOUCH_2G', if_true: [files('gles-host.c'), host_opengl])
elif host_gles.found()
  # gles-host-eagl.c is C, not Objective-C, and explains at length why: an
  # Objective-C source anywhere in qemu-system-arm makes meson link the target
  # with objc_LINKER, and configure emits no objc_link_args, so the iOS
  # sysroot's --extra-ldflags vanish from the link.
  arm_ss.add(when: 'CONFIG_IPOD_TOUCH_2G',
             if_true: [files('gles-host.c', 'gles-host-eagl.c'), host_gles])
else
  arm_ss.add(when: 'CONFIG_IPOD_TOUCH_2G', if_true: files('gles-host-stub.c'))
endif

hw_arch += {'arm': arm_ss}

if host_os == 'darwin'
  arm_ss.add(when: 'CONFIG_IPOD_TOUCH_2G', if_true: dependency('appleframeworks',
    modules: ['CoreFoundation', 'CoreMedia', 'CoreVideo', 'VideoToolbox']))
endif

# Raw AAC DMA may hold several frames without packet boundaries. AudioToolbox
# requires those boundaries; libavcodec can consume the actual guest stream.
ipod_avcodec = dependency('libavcodec', required: false)
ipod_avutil = dependency('libavutil', required: false)
if ipod_avcodec.found() and ipod_avutil.found()
  arm_ss.add(when: 'CONFIG_IPOD_TOUCH_2G', if_true: declare_dependency(
    dependencies: [ipod_avcodec, ipod_avutil], compile_args: ['-DIT_HAVE_AVCODEC']))
endif
