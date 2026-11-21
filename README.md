# Karabas Board Test

[Русская версия](README.ru.md)

The board test core of the Karabas Go, Mini and Mini G (Xilinx Spartan-6 XC6SLX25), "Karabas Board
Test" in the core list (`boardtest.kg`). One core in place of the old test and sdram-test cores.

A VexRiscv CPU with all its code and its screen in block RAM draws a UI in the style of the boot
menu and runs the tests. The SRAM and the SDRAM are never used by the UI, so they get full
destructive tests in hardware, and the core still runs and names the fault on a board with a dead
memory chip. Planned checks: SRAM and SDRAM (address / data lines, march, random, byte lanes,
retention, clock margin), SPI flash, SD cards, CF, FT812, ESP8266 / ESP32, audio out / in, tape,
MIDI, video, keyboard / mouse / joysticks, floppy (Go). Leave the core with Ctrl+Alt+Backspace.

Work in progress, see [docs/PLAN.md](docs/PLAN.md) (in Russian).

## License

MIT, see [LICENSE](LICENSE).
