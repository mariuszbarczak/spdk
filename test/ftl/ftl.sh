#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

rpc_py=$rootdir/scripts/rpc.py

function ftl_clear_lvols() {
    "$rootdir/build/bin/spdk_tgt" &
    spdk_tgt_pid=$!
    waitforlisten "$spdk_tgt_pid"

    if [[ -n $ftl_nvme_dev_base ]] && [[ -n $ftl_nvme_dev_cache ]] ; then
        "$rpc_py" bdev_nvme_attach_controller -b base -t PCIe -a $ftl_nvme_dev_base_path
        "$rpc_py" bdev_nvme_attach_controller -b cache -t PCIe -a $ftl_nvme_dev_cache_path
    fi

    clear_lvols
    killprocess "$spdk_tgt_pid"
}

function at_ftl_exit() {
    ftl_clear_lvols

    # restore original driver
    $rootdir/scripts/setup.sh reset
    remove_shm
}
trap 'at_ftl_exit' SIGINT SIGTERM EXIT

$rootdir/scripts/setup.sh reset

FTL_BASE_SIZE_MIN_GIB=5
FTL_CACHE_SIZE_MIN_GIB=5
declare -A ftl_nvme_size_gib=( )
declare -A ftl_nvme_pcie_path=( )
declare -A ftl_nvme_lbaf=( )

function ftl_nvme_identify() {
    local data_size="$1"
    local metadata_size="$2"
    local var_nvme="$3"
    local nvme_devices=($(nvme list | awk '/\/dev\/nvme/{print $1}' | sed 's|/dev/||'))

    for dev in "${nvme_devices[@]}"; do
        if mount | grep -q "/dev/$dev"; then
            echo "/dev/$dev is currently mounted, skipping..."
            continue
        fi

        if lsof "/dev/$dev" >/dev/null; then
            echo "/dev/$dev is in use, skipping..."
            continue
        fi

        local info=$(nvme id-ns "/dev/$dev" -H | grep "Data Size: $data_size" | grep "Metadata Size: $metadata_size")
        if [ "" != "{$info}" ]; then
            # Get the LBA format of the NVMe device
            lbaf=$(echo $info | awk '{print $3}')
            declare -n nvme_info=$var_nvme
            nvme_info[$dev]=$lbaf

            # Get the size of the NVMe device
            local size=$(blockdev --getsize64 "/dev/$dev" | awk '{ printf "%d", $1/1024/1024/1024 }')
            ftl_nvme_size_gib[$dev]=$size

            # Get the PCIe device path
            pcie_path=$(nvme list -v | grep pcie | grep "$dev" | awk -F 'pcie +' '{print $2}' | cut -f1 -d' ')
            ftl_nvme_pcie_path[$dev]=$pcie_path
        fi
    done
}

ftl_nvme_dev_base=""
ftl_nvme_dev_base_size=0
ftl_nvme_dev_base_lbaf=""
ftl_nvme_dev_base_path=""

ftl_nvme_dev_cache=""
ftl_nvme_dev_cache_size=0
ftl_nvme_dev_cache_lbaf=""

declare -A ftl_nvme_info_4096_64
ftl_nvme_identify 4096 64 ftl_nvme_info_4096_64

declare -A ftl_nvme_info_4096_0
ftl_nvme_identify 4096 0 ftl_nvme_info_4096_0

function ftl_nvme_select_cache_device () {
    declare -n nvme_info=$1

    for dev in "${!nvme_info[@]}"; do
        if [ "$dev" == "$ftl_nvme_dev_cache" ] || [ "$dev" == "$ftl_nvme_dev_base" ]; then
            continue
        fi

        if [ ${ftl_nvme_size_gib[${dev}]} -lt ${FTL_CACHE_SIZE_MIN_GIB} ]; then
            continue
        fi

        if [ ${ftl_nvme_size_gib[${dev}]} -gt ${ftl_nvme_dev_cache_size} ]; then
            ftl_nvme_dev_cache=$dev
            ftl_nvme_dev_cache_size=${ftl_nvme_size_gib[${dev}]}
            ftl_nvme_dev_cache_lbaf=${nvme_info[${dev}]}
            ftl_nvme_dev_cache_path=${ftl_nvme_pcie_path[${dev}]}
        fi
    done
}

function ftl_nvme_select_base_device () {
    for dev in "${!ftl_nvme_info_4096_0[@]}"; do
        if [ "$dev" == "$ftl_nvme_dev_cache" ] || [ "$dev" == "$ftl_nvme_dev_base" ]; then
            continue
        fi

        if [ ${ftl_nvme_size_gib[${dev}]} -lt ${FTL_BASE_SIZE_MIN_GIB} ]; then
            continue
        fi

        if [ ${ftl_nvme_size_gib[${dev}]} -gt ${ftl_nvme_dev_base_size} ]; then
            ftl_nvme_dev_base=$dev
            ftl_nvme_dev_base_size=${ftl_nvme_size_gib[${dev}]}
            ftl_nvme_dev_base_lbaf=${ftl_nvme_info_4096_0[${dev}]}
            ftl_nvme_dev_base_path=${ftl_nvme_pcie_path[${dev}]}
        fi
    done
}

if [ "" == "${FTL_TEST_NV_CACHE_DEVICE_TYPE}" ]; then
    echo $LINENO
    FTL_TEST_NV_CACHE_DEVICE_TYPE="bdev-vss"
fi

case "${FTL_TEST_NV_CACHE_DEVICE_TYPE}" in
    "bdev-vss")
        echo "NV cache device type: bdev-vss"
        ftl_nvme_select_cache_device ftl_nvme_info_4096_64
    ;;
    "bdev-non-vss")
        echo "NV cache device type: bdev-non-vss"
        ftl_nvme_select_cache_device ftl_nvme_info_4096_0
    ;;
    *)
        echo "Unknown NV cache device type: ${FTL_TEST_NV_CACHE_DEVICE_TYPE}"
        exit 1
    ;;
esac
ftl_nvme_select_base_device

if [ "$ftl_nvme_dev_base" == "" ]; then
    echo "No base device found for FTL"
    exit 1
fi
if [ "$ftl_nvme_dev_cache" == "" ]; then
    echo "No cache device found for FTL"
    exit 1
fi

echo "FTL base device: $ftl_nvme_dev_base, size: ${ftl_nvme_dev_base_size} GiB, LBAF: $ftl_nvme_dev_base_lbaf, PCIe path: $ftl_nvme_dev_base_path"
echo "FTL cache device: $ftl_nvme_dev_cache, size: ${ftl_nvme_dev_cache_size} GiB, LBAF: $ftl_nvme_dev_cache_lbaf, PCIe path: $ftl_nvme_dev_cache_path"

echo "Formatting FTL base device ${ftl_nvme_dev_base}..."
nvme format /dev/${ftl_nvme_dev_base} -l ${ftl_nvme_dev_base_lbaf} --force

echo "Formatting FTL cache device ${ftl_nvme_dev_cache}..."
nvme format /dev/${ftl_nvme_dev_cache} -l ${ftl_nvme_dev_cache_lbaf} --force

# Bind device to vfio/uio driver before testing
PCI_ALLOWED="$ftl_nvme_dev_base_path $ftl_nvme_dev_cache_path" PCI_BLOCKED="" DRIVER_OVERRIDE="" $rootdir/scripts/setup.sh
$rootdir/scripts/setup.sh status
ftl_clear_lvols

if [[ -z $SPDK_TEST_FTL_NIGHTLY ]]; then
	run_test "ftl_fio_basic" $testdir/fio.sh $device $nv_cache basic
	run_test "ftl_bdevperf" $testdir/bdevperf.sh $device $nv_cache
	run_test "ftl_trim" $testdir/trim.sh $device $nv_cache
	run_test "ftl_restore" $testdir/restore.sh -c $nv_cache $device
	run_test "ftl_dirty_shutdown" $testdir/dirty_shutdown.sh -c $nv_cache $device
fi

if [ $SPDK_TEST_FTL_EXTENDED -eq 1 ]; then
	run_test "ftl_restore_fast" $testdir/restore.sh -f -c $nv_cache $device
	run_test "ftl_dirty_shutdown" $testdir/dirty_shutdown.sh -c $nv_cache $device
	run_test "ftl_write_after_write" $testdir/write_after_write.sh $device $nv_cache
	run_test "ftl_fio_extended" $testdir/fio.sh $device $nv_cache extended
fi

if [ $SPDK_TEST_FTL_NIGHTLY -eq 1 ]; then
    run_test "ftl_fio_nightly" $testdir/fio.sh $ftl_nvme_dev_base_path $ftl_nvme_dev_cache_path nightly
fi
