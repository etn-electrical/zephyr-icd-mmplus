# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_sources(
    icm42688.c
    icm42688_common.c
    icm42688_spi.c
)

zephyr_library_sources_ifdef(CONFIG_EMUL_ICM42688 icm42688_emul.c)
zephyr_include_directories_ifdef(CONFIG_EMUL_ICM42688 .)
