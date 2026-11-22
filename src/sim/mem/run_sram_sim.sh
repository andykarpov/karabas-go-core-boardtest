#!/bin/bash
# bt_sram against the chip model with each defect of tb_sram.v (iverilog, a few seconds; two runs at a time)
cd "$(dirname "$0")" || exit 1
iverilog -g2005 -o tb_sram.vvp -s tb_sram tb_sram.v ../../rtl/mem/bt_sram.v 2>&1 | grep -v "sorry" || true
fail=0
run() { vvp -n tb_sram.vvp "$@" | grep -E "^DEF|PASS|FAIL"; }
for d in "0 3 6" "1 3 6" "2 3 6" "3 3 6" "4 2 7" "5 4 4" "6 3 6" "7 3 6" "2 3 15" "1 3 0"; do
	set -- $d
	iverilog -g2005 -P tb_sram.DEF=$1 -P tb_sram.J=$2 -P tb_sram.K=$3 -o tb_sram_$1.vvp -s tb_sram tb_sram.v ../../rtl/mem/bt_sram.v || exit 1
	out=$(vvp -n tb_sram_$1.vvp)
	echo "$out" | grep -E "^DEF|PASS|FAIL"
	echo "$out" | grep -q "^PASS" || fail=1
	rm -f tb_sram_$1.vvp
done
rm -f tb_sram.vvp
[ $fail = 0 ] && echo "all PASS" || { echo "some FAIL"; exit 1; }
