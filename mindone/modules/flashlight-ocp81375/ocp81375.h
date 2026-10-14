/* SPDX-License-Identifier: GPL-2.0 */
/*
 * flashlights-ocp81375.h - OCP81375 dual-channel LED flash driver
 *
 * Register map, bit fields and timing/current constants reverse
 * engineered from a Transsion-built ocp81375.ko (see RE-NOTES.md
 * for the full methodology: aarch64 disassembly + Unicorn-based
 * emulation of the ioctl dispatcher). None of this is published in
 * the public OCP81375 datasheet (Rev.1.3, Feb 24 2023), which only
 * documents the analog pin description and block diagram.
 *
 * Chip: Orient-Chip OCP81375, I2C-compatible, I2C address 0x63.
 */

#ifndef __FLASHLIGHTS_OCP81375_H
#define __FLASHLIGHTS_OCP81375_H

#include <linux/bits.h>

/* ---------------------------------------------------------------- *
 * Device tree
 * ---------------------------------------------------------------- */
#define OCP81375_DTNAME_I2C "mediatek,strobe_main"
#define OCP81375_DTNAME     "mediatek,flashlights_ocp81375"
#define OCP81375_NAME       "flashlights-ocp81375"
#define OCP81375_REGULATOR_NAME "ocp81375_regulator"

/* ---------------------------------------------------------------- *
 * Register map (reverse engineered - not in the public datasheet)
 * ---------------------------------------------------------------- */
#define OCP81375_REG_ENABLE            0x01
#define OCP81375_REG_IVFM              0x02
#define OCP81375_REG_FLASH_LEVEL_LED2  0x03
#define OCP81375_REG_FLASH_LEVEL_LED1  0x04
#define OCP81375_REG_TORCH_LEVEL_LED2  0x05
#define OCP81375_REG_TORCH_LEVEL_LED1  0x06
#define OCP81375_REG_BOOST_CONFIG      0x07
#define OCP81375_REG_TIMING_CONFIG     0x08
#define OCP81375_REG_TEMP              0x09
#define OCP81375_REG_FLAG1             0x0A
#define OCP81375_REG_FLAG2             0x0B
#define OCP81375_REG_DEVICE_ID         0x0C

/* OCP81375_REG_DEVICE_ID expected value */
#define OCP81375_DEVICE_ID             0x3A

/* OCP81375_REG_ENABLE bit fields */
#define OCP81375_ENABLE_LED2           BIT(0)
#define OCP81375_ENABLE_LED1           BIT(1)
#define OCP81375_MODE_TORCH            BIT(3)
#define OCP81375_MODE_FLASH            (BIT(3) | BIT(2))

#define OCP81375_LED1_TORCH_ON \
	(OCP81375_ENABLE_LED1 | OCP81375_MODE_TORCH)        /* 0x0A */
#define OCP81375_LED1_FLASH_ON \
	(OCP81375_ENABLE_LED1 | OCP81375_MODE_FLASH)        /* 0x0E */
#define OCP81375_LED2_TORCH_ON \
	(OCP81375_ENABLE_LED2 | OCP81375_MODE_TORCH)        /* 0x09 */
#define OCP81375_LED2_FLASH_ON \
	(OCP81375_ENABLE_LED2 | OCP81375_MODE_FLASH)        /* 0x0D */

/* init-time boost/timing config (identical to the public aw3644
 * driver's values - OCP81375 appears register-compatible for these)
 */
#define OCP81375_BOOST_CONFIG_VAL      0x09
#define OCP81375_TIMING_CONFIG_VAL     0x1F

/* power-rail settle delay used after regulator_enable() and after
 * every current/level write - matches the 3 ms constant-udelay in
 * the original module.
 */
#define OCP81375_WAIT_TIME_US          3000

/* current (mA) <-> register-code conversion.
 * Torch range:  code = (mA*100 - 390)  / 390    (~3.90 mA / code, 0..127)
 * Flash range:  code = (mA*100 - 1563) / 1563   (~15.63 mA / code, 0..127)
 * Recovered from the magic-number division sequences the compiler
 * emitted in ocp81375_set_current(); matches the datasheet's
 * "3.90mA to 2A" programmable range exactly (3.90 mA step for Torch,
 * 2000 mA / 128 levels = 15.625 mA step for Flash) and the 501 mA
 * threshold at which the driver switches from the torch register to
 * the flash register.
 */
#define OCP81375_TORCH_STEP_X100       390
#define OCP81375_FLASH_STEP_X100       1563
#define OCP81375_FLASH_THRESHOLD_MA    501
#define OCP81375_LEVEL_MASK            0x7F

/* channels */
#define OCP81375_CHANNEL_NUM 2
#define OCP81375_CHANNEL_CH1 0
#define OCP81375_CHANNEL_CH2 1

/* duty (level) table sizes, as registered via flashlight_dev_register()
 * and reported through FLASH_IOC_GET_DUTY_NUMBER
 */
#define OCP81375_LEVEL_NUM   30 /* main channel (CH1) duty table size */
#define OCP81375_LEVEL2_NUM  2  /* secondary channel (CH2) duty table size */

#define OCP81375_NONE          (-1)
#define OCP81375_DISABLE       0
#define OCP81375_ENABLE        1
#define OCP81375_ENABLE_TORCH  1
#define OCP81375_ENABLE_FLASH  2

/*
 * Two extra ioctl commands exist in this module that are NOT part of
 * the public flashlight.h FLASH_IOC_* set and are NOT _IOWR()-encoded
 * at all - they are bare small integers. Confirmed by emulating
 * ocp81375_ioctl() with Unicorn and sweeping cmd values (see
 * verify-ioctl-emu.py / RE-NOTES.md): every standard
 * _IOR/_IOW/_IOWR('S', nr, {0,4,8}) encoding in nr=0..255 was tried
 * first and none reached these two branches; cmd==20 and cmd==21
 * (literal, undecorated) do. Because a real _IOWR()-encoded command
 * always sets high bits (large/negative as a signed int), these
 * can never collide with a value coming from a userspace ioctl()
 * syscall - this is a vendor-internal extension invoked kernel-side
 * (most likely by flashlight-core.c itself while enumerating a
 * device's parameters), not through the public uapi.
 */
#define OCP81375_IOC_GENERATE_DEV_PARA 20
#define OCP81375_IOC_SET_CURRENT       21
#define OCP81375_SET_CURRENT_MAX_MA    1999

#endif /* __FLASHLIGHTS_OCP81375_H */
