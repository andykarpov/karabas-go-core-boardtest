// The board test program: the groups of checks, their results, the screens (main, a page per group), the keys.
#pragma once
#include <stdint.h>

// the result of a group (the report of Ф4 relies on these)
enum { ST_IDLE, ST_RUN, ST_ASK, ST_PASS, ST_FAIL, ST_NA, ST_NA_FW };

// USB HID usage codes of the keys the pages use
enum { K_ENTER = 0x28, K_ESC = 0x29, K_BKSP = 0x2a, K_SPACE = 0x2c, K_RIGHT = 0x4f, K_LEFT = 0x50, K_DOWN = 0x51,
       K_UP = 0x52, K_Y = 0x1c, K_N = 0x11, K_R = 0x15, K_A = 0x04, K_L = 0x0f };

typedef struct group group_t;
struct group {
  const char *name;
  void (*open)(group_t *g);        // the page is opened: draw its content (right part), start the check
  void (*tick)(group_t *g);        // called in the loop while the page is open
  void (*key)(group_t *g, int k);  // a key pressed on the page (not Esc)
  void (*close)(group_t *g);       // the page is left: stop what runs (tones, LEDs, the matrix)
  uint8_t status;
  char info[16];                   // a short result for the main screen
  int32_t v[8];                    // the state of the check
};

extern group_t groups[];
extern const int ngroups;

// the page of the open group: its content area (columns PX.., rows 8..26)
#define PX 20
#define PROW 8
void page_title(group_t *g);                       // the title row with the status pill
void page_row(int r, const char *label, const char *value, int color);   // a label / value row (r from 0)
void page_hint(const char *s);                     // the footer line of the page (row 28)
void set_status(group_t *g, int st, const char *info);
uint32_t now_ms(void);
int key_held(int code);                            // the key is in the last keyboard report

// helpers of the checks (tests.c)
void str_copy_n(char *d, const char *s, int n);

// the memory checks (mem.c)
void sram_open(group_t *g);
void sram_tick(group_t *g);
void sram_key(group_t *g, int k);
const char *sram_diag(void);   // the diagnosis line of the SRAM page
void sdram_open(group_t *g);
void sdram_tick(group_t *g);
void sdram_key(group_t *g, int k);
const char *sdram_diag(void);
void mem_stop(void);            // the auto run: the SRAM / SDRAM testers stop

// the storage and the modules (per.c)
void sd_open(group_t *g);
void cf_open(group_t *g);
void ft_open(group_t *g);
void wifi_open(group_t *g);
void wifi_tick(group_t *g);
void wifi_close(group_t *g);
