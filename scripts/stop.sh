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

if [ $nat_enable -eq 1 ]; then
    tunctl -d $tap
fi

if [ $virtio_enable -eq 1 ]; then
    sudo tunctl -d $tap
fi

if [ $dpdk_enable -eq 1 ]; then
    pci_id=`$dpdk_target/../tools/dpdk_nic_bind.py --status|grep "drv=igb_uio"|awk '{print $1}'`
    $dpdk_target/../tools/dpdk_nic_bind.py -u $pci_id
    $dpdk_target/../tools/dpdk_nic_bind.py -b $eth_driver $pci_id
    rmmod igb_uio
    umount /mnt/huge
fi
