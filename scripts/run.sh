#!/bin/sh -e
#
# This file is open source software, licensed to you under the terms
# of the Apache License, Version 2.0 (the "License").  See the NOTICE file
# distributed with this work for additional information regarding copyright
# ownership.  You may not use this file except in compliance with the License.
#
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

if [ $# -lt 1 ]; then
    echo "usage: $0 [config]"
    exit 1
fi

. ./$1

if [ $dpdk_enable -eq 1 ]; then
    modprobe uio
    insmod $dpdk_target/kmod/igb_uio.ko
    $dpdk_target/../tools/dpdk_nic_bind.py --bind=igb_uio $eth
    mkdir -p /mnt/huge
    mount -t hugetlbfs nodev /mnt/huge
    echo 64 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
    args="$args --dpdk-pmd"
fi

if [ $nat_enable -eq 1 ]; then
    tunctl -d $tap
    ip tuntap add mode tap dev $tap one_queue vnet_hdr
    ifconfig $tap up
    brctl addif $bridge $tap
    modprobe vhost-net
    chown $user.$user /dev/vhost-net
    args="$args --nat-adapter"
fi

if [ $virtio_enable -eq 1 ]; then
    user=`whoami`
    sudo tunctl -d $tap
    sudo ip tuntap add mode tap dev $tap user $user one_queue vnet_hdr
    sudo ifconfig $tap up
    sudo brctl addif $bridge $tap
    sudo brctl stp $bridge off
    sudo modprobe vhost-net
    sudo chown $user.$user /dev/vhost-net
fi

export LD_LIBRARY_PATH=$dpdk_target/lib
$program $args
