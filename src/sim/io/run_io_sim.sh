#!/bin/sh
# tb_io (iverilog): bt_audio + bt_io with a cable from the generator to the ADC and from TAPE_OUT to TAPE_IN.
set -e
cd "$(dirname "$0")"
M=../../../modules/rtl
U=/opt/Xilinx/14.7/ISE_DS/ISE/verilog/src
iverilog -g2005 -I ../../rtl/io -o tb_io.vvp tb_io.v ../../rtl/io/bt_audio.v ../../rtl/io/bt_io.v \
  $M/audio/kg_tape_monitor.v $M/audio/audio_peaks.v $M/audio/kg_midi_tx.v $M/mem/kg_fifo.v $U/unisims/DNA_PORT.v $U/glbl.v
vvp -n tb_io.vvp | grep tb_io
