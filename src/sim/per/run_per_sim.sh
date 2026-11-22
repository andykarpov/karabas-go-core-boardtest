#!/bin/bash
# bt_per against the models of tb_per.v (iverilog, a second)
cd "$(dirname "$0")" || exit 1
iverilog -g2005 -o tb_per.vvp -s tb_per tb_per.v ../../rtl/io/bt_per.v ../../../modules/rtl/mem/kg_fifo.v || exit 1
out=$(vvp -n tb_per.vvp); rm -f tb_per.vvp
echo "$out" | grep -E "PASS|FAIL"
echo "$out" | grep -q "^PASS"
