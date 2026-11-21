# Karabas Go board test

The board test core of the Karabas Go, Mini and Mini G (Xilinx Spartan-6 XC6SLX25): one core in
place of the old test and sdram-test cores. A VexRiscv CPU with all its code and screen in block RAM
draws a menu-style UI and runs the tests, so the SRAM and the SDRAM are free for destructive tests.

Work in progress, see [docs/PLAN.md](docs/PLAN.md) (in Russian).

## License

MIT, see [LICENSE](LICENSE).
