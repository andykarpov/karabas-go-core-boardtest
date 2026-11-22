# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

"Karabas Board Test" (id `boardtest`, `boardtest.kg`): one board test core of the Karabas Go, Mini and Mini G (Xilinx Spartan-6 XC6SLX25 -2, ISE 14.7, XST), in place of `../karabas-go-core-test` (the ZXDOS tester) and `../karabas-go-core-sdram-test` (the MiSTer MemTest). Only their ideas are taken (the list of checks, the SDRAM clock sweep with auto mode); the code is new.

Status: RELEASED 2026-11-22 (Ф0-Ф5, PRs #2-#7, build 26112211, karabas-go #53; old test / sdram-test cores removed and their repos archived); build 26112301 (2026-11-23): Go logo without "mini", Go PCA9536 check; build 26112302: Terminus Bold text font. Mini G checked on the board; Go / Mini never run on a board. Open: docs/BACKLOG.md. Docs, in Russian: `docs/PLAN.md` (decisions, architecture, checks, phases), `docs/BACKLOG.md`. Follow the phases; record a changed decision in PLAN.md.

Base: the SoC of `../karabas-go-core-menu` (VexRiscv rv32im, `kg_l2_rpc`, `menu_compose` gradient + ARGB palette, Noto 4 bpp fonts, the cursor, the ESP AT code, the host frame build `src/sw/host`) and its look. The difference (variant A, user 2026-11-22): the CPU code, its data and the screen are ALL in block RAM (a tile layer 80 x 30 of 10 x 20 cells over the gradient: text in Terminus Font Bold 10 x 20 (OFL, since build 26112302; was the OSD 8 x 8 font doubled in height), the big clock digits of the OSD font, the mono logo x4, anti-aliased shapes; no frame buffer, no blitter; atlas `src/sw/tools/atlas.py`, frame model `mockup.py`, approved look `docs/img/f0_mock_*.png`), so the SRAM and the SDRAM are never used by the UI and are tested destructively by RTL BIST engines. Never put code, stack or screen into the SRAM / SDRAM.

## Rules of the core

- Same style as the menu, not an exact copy (user). Every screen needs the user's approval (frames from the host build / sim first).
- No exit item in the UI: the user presses Ctrl+Alt+Backspace (the Global Reset OSD item of `core.json`).
- Read only on media that are not ours: CF / IDE (the bench card has the user's data), SD1 / SD2. A write test only on a scratch area the user named.
- Base board set in Ф1 (all cores): audio peaks to the MCU, TAPE_IN / TAPE_OUT + `kg_tape_monitor`, MIDI_TX, ADC PCM1808, FT_CLK_OUT ~8 MHz on Mini / Mini G.
- Board rules of our cores: syncs always present, Go `FDC_*` outputs 0 when unused, Mini G2 SDRAM `~CS` is on FLASH_CS_N (T3): drive T3 as the SDRAM CS (no flash chip is fitted on any board, its group shows N/A), OSD switch index = item position unless `"index"`, Verilog-2001 for XST, build map / par with `-t 2` first.
- Sims and tests run in seconds; a long sim only after asking. At most 2-3 sims in parallel.
- `karabas-go-modules` is shared with other sessions: change it only in a git worktree, PR there, then move the submodule.
- Before a step that needs the board or the HDMI capture, tell the user; take an hwq ticket; capture frames only after a yes; ask before a firmware flash.
- Git: public repo `andykarpov/karabas-go-core-boardtest`; one branch per phase of `docs/PLAN.md`, one PR into master at its end; never push master directly after the initial commit. Stage explicit paths. After a release (PR merged): `git checkout master && git pull`, `git submodule update`, delete the merged branch.

## Build

- The CPU program and the atlases: `make -C src/sw` (xPack `riscv-none-elf-gcc` as the menu, `-Os`, no libc) writes `src/roms/bt.bin`, `atlas1.bin`, `atlas4.bin` and `src/sw/atlas_gen.h` (all committed). `src/sw/tools/mockup.py OUT` draws the approved look (`docs/img/f0_mock_*.png`).
- Simulation: `src/sim/soc/run_sim.sh` (Verilator, ~1 min): the ROMs through the LD port, a model of the MCU (RPC SYS / CFG), two frames, each compared pixel for pixel with `src/sw/tools/frame_model.py` (the bit-exact model of bt_tiles + menu_compose from the cells / palette the CPU wrote). Run it after every RTL or program change.
- Memory map, MMIO and the cell format: the headers of `src/rtl/soc/bt_soc.v` and `src/rtl/video/bt_tiles.v`, `src/sw/mmio.h`.

- `cd src && make minig` (`go`, `mini`, all without a target): XST, map / par, bitgen, then `make_core.py` (symlink to `../../karabas-go-tools/make_core/make_core.py`) packs `core.json` + the CPU program (ROM section, `LOAD` into BRAM) into `src/boardtest.kg`.
- The `.bin` and `.kg` are committed; ISE outputs are git-ignored.

## Memory

The Claude Code memory of this repo is shared with all karabas-go repos: `~/.claude/projects/<this project>/memory` is a symlink to `../karabas-go-notes` (private repo `andykarpov/karabas-go-notes`, `link.sh` there makes the links). Its `MEMORY.md` is split by repo (section `core-boardtest`, note `boardtest-core.md`); commit and push it after every memory change.
