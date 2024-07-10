#!/bin/bash
# source ../../common/env_lgl.sh
export YOSYS=/home/liguolong/install/yosys/bin/yosys
export NEXTPNR=/home/liguolong/gitlab-eda/nextpnr-xilinx-dram/nextpnr-xilinx/build/nextpnr-xilinx
export ARCH_BIN=/home/liguolong/gitlab-eda/nextpnr-xilinx-dram/nextpnr-xilinx/xilinx/xc7a35t.bin
export DBROOT=/home/liguolong/gitlab-eda/nextpnr-xilinx-dram/nextpnr-xilinx/xilinx/external/prjxray-db/artix7

export PART=xc7a35tfgg484-2

export XRAY_DIR=/home/liguolong/gitlab-eda/prjxray
export XRAY_UTILS_DIR=${XRAY_DIR}/utils
export PYTHONPATH=${XRAY_DIR}

export XC7FRAMES2BIT=${XRAY_DIR}/build/tools/xc7frames2bit
# echo $DBROOT

export PART_FILE=$DBROOT/$PART/part.yaml

mkdir build
cd build

$YOSYS -p "synth_xilinx -flatten -nowidelut -abc9 -arch xc7 -top top; write_json synth.json; show -format dot -prefix synth1" ../top.v

# pack
$NEXTPNR --chipdb $ARCH_BIN --xdc ../$PART.xdc --json synth.json -l log_pack.log --debug --pack-only --write pack.json
# place
$NEXTPNR --chipdb $ARCH_BIN --xdc ../$PART.xdc --json pack.json -l log_place.log --debug --no-pack --no-route --write place.json
# route
$NEXTPNR --chipdb $ARCH_BIN --xdc ../$PART.xdc --json place.json -l log_route.log --debug --no-pack --no-place --write route.json --fasm top.fasm

source "${XRAY_DIR}/utils/environment.sh"
${XRAY_UTILS_DIR}/fasm2frames.py --part $PART --db-root $DBROOT top.fasm > top.frames
${XC7FRAMES2BIT} --part_file $PART_FILE --part_name $PART  --frm_file top.frames --output_file top.bit

cd ..