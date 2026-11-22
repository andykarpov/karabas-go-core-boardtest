#!/bin/sh
# tb_bt (Verilator): bt_soc + bt_tiles + menu_compose + kg_l2_rpc with the ROMs of boardtest.kg against a model of
# the MCU; two frames, each compared pixel for pixel with the frame tools/frame_model.py draws from the state the
# CPU wrote (cells, palette, gradient, cursor). About a minute.
set -e
cd "$(dirname "$0")"
make -s -C ../../sw
M=../../../modules/rtl
verilator --cc --exe --build -O2 -j 4 -Wno-fatal -Wno-WIDTH -Wno-UNUSED -Wno-PINCONNECTEMPTY -Wno-CASEINCOMPLETE \
  -Wno-UNOPTFLAT -Wno-MULTIDRIVEN -Wno-SYNCASYNCNET -Wno-TIMESCALEMOD -Wno-DECLFILENAME -Wno-PINMISSING \
  --top-module tb_bt -Mdir obj_bt \
  tb_bt.v ../../rtl/soc/bt_soc.v ../../rtl/soc/vexriscv/VexRiscv.v ../../rtl/video/bt_tiles.v \
  ../../rtl/video/menu_compose.v $M/ctl/kg_sync.v $M/mem/kg_sdpram.v $M/mem/kg_tdpram.v $M/mcu/kg_l2_rpc.v \
  tb_bt.cpp > obj_bt.log 2>&1 || { tail -30 obj_bt.log; exit 1; }
./obj_bt/Vtb_bt ../../roms
for f in frame1 frame2; do
  python3 ../../sw/tools/frame_model.py $f.state ../../roms ${f}_model.ppm $f.ppm
done
echo "tb_bt: frames match the model"
