#!/bin/bash
# bt_sdram against the SDRAM model with each defect of tb_sdram.v, at 166.7 and 100 MHz (iverilog, one at a time)
cd "$(dirname "$0")" || exit 1
fail=0
for d in "0 3 6 6.0" "0 3 6 10.0" "1 3 6 6.0" "2 3 12 6.0" "3 3 6 6.0" "4 2 6 6.0" "5 1 7 6.0" "6 1 0 6.0" "7 3 6 6.0" "1 3 0 10.0"; do
	set -- $d
	iverilog -g2012 -DSIM -P tb_sdram.DEF=$1 -P tb_sdram.J=$2 -P tb_sdram.K=$3 -P tb_sdram.T=$4 -o tb_sdram_$1.vvp -s tb_sdram \
		tb_sdram.v ../../rtl/mem/bt_sdram.v || exit 1
	out=$(vvp -n tb_sdram_$1.vvp)
	echo "$out" | grep -E "^DEF|PASS|FAIL|timing"
	echo "$out" | grep -q "^PASS" || fail=1
	rm -f tb_sdram_$1.vvp
done
[ $fail = 0 ] && echo "all PASS" || { echo "some FAIL"; exit 1; }
