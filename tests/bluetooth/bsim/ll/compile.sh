#!/usr/bin/env bash
# Copyright 2023 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

# Compile all the applications needed by the bsim tests

#set -x #uncomment this line for debugging
set -ue

: "${BSIM_OUT_PATH:?BSIM_OUT_PATH must be defined}"
: "${BSIM_COMPONENTS_PATH:?BSIM_COMPONENTS_PATH must be defined}"
: "${ZEPHYR_BASE:?ZEPHYR_BASE must be set to point to the zephyr root\
 directory}"

WORK_DIR="${WORK_DIR:-${ZEPHYR_BASE}/bsim_out}"
BOARD="${BOARD:-nrf52_bsim}"
BOARD_ROOT="${BOARD_ROOT:-${ZEPHYR_BASE}}"

mkdir -p ${WORK_DIR}

source ${ZEPHYR_BASE}/tests/bluetooth/bsim/compile.source

app=tests/bluetooth/bsim/ll/advx compile &

app=tests/bluetooth/bsim/ll/conn conf_file=prj_split.conf compile &
app=tests/bluetooth/bsim/ll/conn conf_file=prj_split_privacy.conf compile &
app=tests/bluetooth/bsim/ll/conn conf_file=prj_split_low_lat.conf compile &

app=tests/bluetooth/bsim/ll/iso compile &
app=tests/bluetooth/bsim/ll/iso conf_file=prj_vs_dp.conf compile &

app=tests/bluetooth/bsim/ll/edtt/hci_test_app \
  conf_file=prj_dut_llcp.conf compile &
app=tests/bluetooth/bsim/ll/edtt/hci_test_app \
  conf_file=prj_tst_llcp.conf compile &
app=tests/bluetooth/bsim/ll/edtt/hci_test_app \
  conf_file=prj_dut.conf compile &
app=tests/bluetooth/bsim/ll/edtt/hci_test_app \
  conf_file=prj_tst.conf compile &
app=tests/bluetooth/bsim/ll/edtt/gatt_test_app \
  conf_file=prj.conf compile &
app=tests/bluetooth/bsim/ll/edtt/gatt_test_app \
  conf_file=prj_llcp.conf compile &

wait
