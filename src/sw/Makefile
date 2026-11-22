# The program of the board test CPU (rtl/soc/bt_soc.v) and the atlases of bt_tiles: the ROMs of
# boardtest.kg (core.json roms): roms/bt.bin at 0, roms/atlas1.bin at 0x100000, roms/atlas4.bin at 0x110000.
XPACK  ?= $(HOME)/opt/xpack-riscv-none-elf-gcc-15.2.0-1/bin
CROSS  := $(XPACK)/riscv-none-elf-
CFLAGS := -march=rv32im -mabi=ilp32 -Os -g -ffreestanding -fno-builtin -ffunction-sections -fdata-sections \
          -fno-tree-loop-distribute-patterns -Wall -Wextra
B      := build
R      := ../roms

all: $(R)/bt.bin $(R)/atlas1.bin

$(R)/bt.bin: $(B)/bt.bin
	mkdir -p $(R)
	cp $< $@

# the atlases and their tile codes (atlas_gen.h, committed)
$(R)/atlas1.bin $(R)/atlas4.bin atlas_gen.h: tools/atlas.py tools/mkatlas.py assets/osd_font_866.bin assets/logo_mini_56x16.bin
	mkdir -p $(R)
	python3 tools/mkatlas.py $(R)/atlas1.bin $(R)/atlas4.bin atlas_gen.h

$(B)/%.o: %.c $(wildcard *.h) atlas_gen.h | $(B)
	$(CROSS)gcc $(CFLAGS) -c $< -o $@
$(B)/%.o: %.S | $(B)
	$(CROSS)gcc $(CFLAGS) -c $< -o $@
SRCS   := main.c tests.c scr.c rpc.c util.c
OBJS   := $(B)/crt0.o $(addprefix $(B)/,$(SRCS:.c=.o))
$(B)/bt.elf: $(OBJS) link.ld
	$(CROSS)gcc $(CFLAGS) -nostdlib -nostartfiles -T link.ld -Wl,--gc-sections -o $@ $(OBJS) -lgcc
	$(CROSS)size $@
$(B)/bt.bin: $(B)/bt.elf
	$(CROSS)objcopy -O binary $< $@
$(B):
	mkdir -p $(B)
clean:
	rm -rf $(B)
.PHONY: all clean
