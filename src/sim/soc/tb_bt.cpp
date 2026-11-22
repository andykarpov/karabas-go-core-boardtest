// tb_bt: the ROMs of boardtest.kg (roms/atlas1.bin, atlas4.bin, bt.bin) loaded through the LD port as the firmware
// does it, then the program run against a model of the MCU on the link side of kg_l2_rpc (HELLO, SYS INFO,
// TIME_GET, CFG GET with the Ocean colours). Two frames are captured: after the first drawing with a mouse move
// (the cursor), and after the key Down (the selection moves). For each frame the cells, the palette, the gradient
// and the cursor the CPU had written are saved too (frameN.state): tools/frame_model.py draws the frame from them
// as the RTL should, run_sim.sh compares the two pixel for pixel.
#include "Vtb_bt.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static Vtb_bt *t;
static uint64_t now_ps = 0, next_cpu = 0, next_pix = 0;
static const uint64_t P_CPU = 13333, P_PIX = 24444; // 75 MHz, 40.91 MHz
static int errors = 0;
static std::string console;
static std::vector<uint32_t> frame(800 * 600);
static int cap_state = 0; // 0 none, 1 waiting for vs, 2 capturing, 3 done
static int px = 0, py = 0, de_p = 0, vs_p = 0;
// the copy of what the CPU wrote (the state of the screen)
static uint32_t cells[4096], pal[16], grad_top, grad_bot, cursor;
static int cell_writes_in_frame = 0;

static void on_pix_edge() {
  int de = t->de, vs = t->vs;
  if (cap_state == 1 && vs && !vs_p) { cap_state = 2; px = 0; py = 0; cell_writes_in_frame = 0; }
  else if (cap_state == 2) {
    if (de) { if (py < 600 && px < 800) frame[py * 800 + px] = t->rgb; px++; }
    if (!de && de_p) { py++; px = 0; if (py == 600) cap_state = 3; }
  }
  de_p = de; vs_p = vs;
}

static void step() {
  uint64_t nc = next_cpu, np = next_pix;
  uint64_t n = nc < np ? nc : np;
  now_ps = n;
  if (n == nc) { t->clk_cpu = !t->clk_cpu; next_cpu += P_CPU / 2; }
  if (n == np) { t->clk_pix = !t->clk_pix; next_pix += P_PIX / 2; }
  t->eval();
  if (n == np && t->clk_pix) on_pix_edge();
  if (n == nc && t->clk_cpu) {
    if (t->dbg_uart_wr) { char c = (char) t->dbg_uart; putchar(c); console += c; }
    if (t->cell_we) { cells[t->cell_a] = t->cell_wd; if (cap_state == 2) cell_writes_in_frame++; }
    if (t->pal_we) pal[t->pal_wa] = t->pal_wd;
    grad_top = t->grad_top; grad_bot = t->grad_bot; cursor = t->cursor;
  }
}

static void cpu_clock() { do step(); while (!(t->clk_cpu && now_ps == next_cpu - P_CPU / 2)); }
static void cpu_clocks(int n) { for (int i = 0; i < n; i++) cpu_clock(); }

// ---- the MCU model: the byte bus of kg_l2_link for target 08 (as tb_menu_soc of karabas-go-core-menu) ----
static void bus_idle() { t->l_stb = 0; t->l_rq = 0; }
static uint8_t bus_byte(int n, uint8_t d, bool ask = true) {
  t->l_stb = 1; t->l_n = n; t->l_d = d;
  t->l_rq = ask; t->l_rq_m = n + 2;
  t->eval();
  uint8_t q = t->l_q;
  cpu_clock();
  bus_idle();
  cpu_clocks(6);
  return q;
}
static std::vector<uint8_t> rx_q;
static void read_answers(uint8_t cmd, int n) {
  rx_q.clear();
  t->l_sel = 1; t->l_cmd = cmd;
  t->l_rq = 1; t->l_rq_m = 1; t->eval();
  rx_q.push_back(t->l_q);
  cpu_clock(); bus_idle(); cpu_clocks(6);
  for (int m = 2; m <= n; m++) rx_q.push_back(bus_byte(m - 2, 0));
  t->l_sel = 0; cpu_clocks(4);
}
static void hello() {
  t->l_sel = 1; t->l_cmd = 1;
  bus_byte(0, 1); bus_byte(1, 0); bus_byte(2, 0); bus_byte(3, 0); bus_byte(4, 0);
  t->l_sel = 0; cpu_clocks(4);
}
struct req_t { uint8_t id, svc, fn; std::vector<uint8_t> data; };
static bool read_req(req_t &r) {
  read_answers(2, 3);
  if (!(rx_q[0] & 1)) return false;
  int len = rx_q[1] | (rx_q[2] << 8);
  read_answers(3, 5 + len + 2);
  r.id = rx_q[0]; r.svc = rx_q[1]; r.fn = rx_q[2];
  int l2 = rx_q[3] | (rx_q[4] << 8);
  uint16_t sum = 0;
  for (int i = 0; i < 5 + len; i++) sum += rx_q[i];
  uint16_t got = rx_q[5 + len] | (rx_q[6 + len] << 8);
  if (l2 != len || sum != got) { printf("tb_bt: bad REQ\n"); errors++; return false; }
  r.data.assign(rx_q.begin() + 5, rx_q.begin() + 5 + len);
  return true;
}
static bool respond(uint8_t id, uint8_t st, const std::vector<uint8_t> &d) {
  std::vector<uint8_t> b = { id, st, (uint8_t) (d.size() & 255), (uint8_t) (d.size() >> 8) };
  b.insert(b.end(), d.begin(), d.end());
  uint16_t sum = 0;
  for (uint8_t x : b) sum += x;
  b.push_back(sum & 255); b.push_back(sum >> 8); b.push_back(0);
  t->l_sel = 1; t->l_cmd = 4;
  uint8_t ack = 1;
  int ack_m = (int) d.size() + 7;
  for (size_t i = 0; i < b.size(); i++) {
    uint8_t q = bus_byte((int) i, b[i]);
    if ((int) i + 2 == ack_m) ack = q;
  }
  t->l_sel = 0; cpu_clocks(4);
  return ack == 0;
}
static void put_s(std::vector<uint8_t> &v, const char *s) { while (*s) v.push_back((uint8_t) *s++); v.push_back(0); }

static int served = 0;
static void serve() {
  req_t r;
  if (!read_req(r)) return;
  std::vector<uint8_t> out;
  uint8_t st = 0;
  served++;
  if (r.svc == 0 && r.fn == 0) { for (const char *c = "26112119"; *c; c++) out.push_back(*c); out.insert(out.end(), { 3, 1, 7, 0, 0 }); }
  else if (r.svc == 0 && r.fn == 1) { out = { 0xEA, 0x07, 11, 22, 12, 59, 1, 6 }; }  // 22/11/2026 12:59:01
  else if (r.svc == 1 && r.fn == 0) {
    // CFG GET: the [ft812] colours of the Ocean theme
    static const char *look = ",color_bg=000608,color_gradient=006a80,color_button=0e2228,color_active=00a0b8,"
                              "color_text=e0fcff,color_text_active=ffffff,color_copyright=5a8088,";
    std::vector<std::string> a;
    for (size_t p = 0; p < r.data.size(); p += strlen((const char *) r.data.data() + p) + 1) a.push_back((const char *) r.data.data() + p);
    std::string l = look;
    for (size_t k = 1; k + 1 < a.size(); k += 2) {
      size_t at = l.find("," + a[k + 1] + "=");
      if (at == std::string::npos) { out.push_back(0); out.push_back(0); continue; }
      size_t v = at + a[k + 1].size() + 2, e = l.find(',', v);
      out.push_back(1);
      put_s(out, l.substr(v, e - v).c_str());
    }
  }
  else st = 1;
  if (!respond(r.id, st, out)) { printf("tb_bt: RESP %u refused\n", r.id); errors++; }
}

static std::vector<uint8_t> load_file(const char *name) {
  std::vector<uint8_t> v;
  FILE *f = fopen(name, "rb");
  if (!f) { printf("tb_bt: no %s\n", name); exit(1); }
  int c;
  while ((c = fgetc(f)) != EOF) v.push_back((uint8_t) c);
  fclose(f);
  return v;
}
static void load(const std::vector<uint8_t> &d, uint32_t at) {
  for (size_t i = 0; i < d.size(); i++) {
    t->ld_addr = at + (uint32_t) i; t->ld_data = d[i]; t->ld_wr = 1;
    cpu_clock();
    t->ld_wr = 0;
    cpu_clocks(8);
  }
}

static void save_frame(const char *name) {
  std::string base = name;
  FILE *o = fopen((base + ".ppm").c_str(), "wb");
  fprintf(o, "P6 800 600 255\n");
  for (uint32_t p : frame) { fputc(p >> 16, o); fputc((p >> 8) & 255, o); fputc(p & 255, o); }
  fclose(o);
  o = fopen((base + ".state").c_str(), "w");
  fprintf(o, "grad %06x %06x\ncursor %08x\n", grad_top, grad_bot, cursor);
  for (int i = 0; i < 16; i++) fprintf(o, "pal %d %07x\n", i, pal[i]);
  for (int i = 0; i < 80 * 30; i++) fprintf(o, "cell %d %05x\n", i, cells[i]);
  fclose(o);
  if (cell_writes_in_frame) { printf("tb_bt: %d cell writes during %s (the model may differ)\n", cell_writes_in_frame, name); }
}

// run until cond or the limit, serving the RPC calls
template <class F> static bool run_until(F cond, uint64_t limit) {
  for (uint64_t n = 0; n < limit; n += 100) {
    if (t->l_irq) serve();
    cpu_clocks(100);
    if (cond()) return true;
  }
  return false;
}

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  const char *roms = argc > 1 ? argv[1] : "../../roms";
  std::string r = roms;
  std::vector<uint8_t> a1 = load_file((r + "/atlas1.bin").c_str()), a4 = load_file((r + "/atlas4.bin").c_str()),
                       prog = load_file((r + "/bt.bin").c_str());
  t = new Vtb_bt;
  t->reset = 1;
  t->eval();
  cpu_clocks(20);
  t->reset = 0;
  cpu_clocks(20);
  hello();   // as the firmware: HELLO before the ROMs
  t->ld_active = 1;
  load(a1, 0x100000);
  load(a4, 0x110000);
  load(prog, 0);
  t->ld_active = 0; t->ld_end = 1; cpu_clock(); t->ld_end = 0;
  printf("tb_bt: %zu + %zu + %zu bytes loaded\n", a1.size(), a4.size(), prog.size());

  if (!run_until([] { return console.find("bt: drawn") != std::string::npos; }, 20000000)) {
    printf("tb_bt: the screen was not drawn\n"); errors++;
  } else {
    // a mouse move: +20, +10 from the middle (the cursor at 420, 310)
    t->ms_x = 20; t->ms_y = 10; t->ms_wr = 1; cpu_clock(); t->ms_wr = 0;
    run_until([] { return (cursor >> 31) != 0; }, 200000);
    cap_state = 1;
    if (!run_until([] { return cap_state == 3; }, 4000000)) { printf("tb_bt: no frame 1\n"); errors++; }
    else save_frame("frame1");
    // Down: the selection from FPGA / MCU to SRAM
    t->kbd = (uint64_t) 0x51 << 8; t->kbd_wr = 1; cpu_clock(); t->kbd_wr = 0;
    run_until([] { return false; }, 100000);
    t->kbd = 0; t->kbd_wr = 1; cpu_clock(); t->kbd_wr = 0;
    run_until([] { return false; }, 100000);
    cap_state = 1;
    if (!run_until([] { return cap_state == 3; }, 4000000)) { printf("tb_bt: no frame 2\n"); errors++; }
    else save_frame("frame2");
  }
  printf("tb_bt: %d requests served\n", served);
  if (served < 3) { printf("tb_bt: INFO, TIME_GET and CFG GET were not all asked\n"); errors++; }
  printf("tb_bt: %d errors\n", errors);
  delete t;
  return errors ? 1 : 0;
}
