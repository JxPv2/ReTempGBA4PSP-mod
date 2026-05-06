/* Automatic Thumb idle-loop detection/removal (optional; see IDLE_LOOP_PLAN.md). */

#include "common.h"
#include "cpu.h"
#include "memory.h"
#include "main.h"

#define AUTO_IDLE_NONE        0xFFFFFFFFu
#define AUTO_IDLE_FAIL_MAX    10000
#define AUTO_IDLE_MAX_STEPS   64

extern u8 idle_loop_enter_update_gba;

static u32 auto_idle_remove_pc;
static u32 auto_idle_last_pc;
static u32 auto_idle_last_region;
static s32 auto_idle_detect_step;
static u32 auto_idle_detect_fails;
static u32 auto_idle_halt_pending;
static u32 auto_idle_cached[16];

void cpu_auto_idle_loop_reset(void)
{
  auto_idle_remove_pc = AUTO_IDLE_NONE;
  auto_idle_last_pc = AUTO_IDLE_NONE;
  auto_idle_last_region = 0x100u;
  auto_idle_detect_step = -1;
  auto_idle_detect_fails = 0;
  auto_idle_halt_pending = 0;
}

static int auto_idle_pc_in_manual_list(u32 pc)
{
  s32 i;

  for (i = 0; i < idle_loop_targets; i++)
  {
    if (idle_loop_target_pc[i] == AUTO_IDLE_NONE)
      break;
    if (idle_loop_target_pc[i] == pc)
      return 1;
  }
  return 0;
}

static u16 auto_idle_fetch16(u32 address)
{
  u32 page = address >> 15;
  u8 *block = memory_map_read[page];

  if (block == NULL)
  {
    block = load_gamepak_page(page & 0x3FF);
    if (block == NULL)
      return 0;
  }

  return ADDRESS16(block, address & 0x7FFF);
}

static int auto_idle_io_load_ok(u32 addr)
{
  addr &= ~1u;
  if (addr < 0x04000000 || addr >= 0x05000000)
    return 0;

  switch (addr)
  {
    case 0x04000004: /* DISPSTAT */
    case 0x04000006: /* VCOUNT */
    case 0x04000130: /* KEYINPUT */
    case 0x04000132: /* KEYCNT */
    case 0x04000200: /* IE */
    case 0x04000202: /* IF */
      return 1;
    default:
      return 0;
  }
}

static int auto_idle_classify_load(u32 ldaddr)
{
  u32 rgn = ldaddr >> 24;

  if (rgn == 0x02 || rgn == 0x03 || rgn == 0x06)
    return 0;
  if (rgn >= 0x08 && rgn <= 0x0D)
    return 0;
  if (ldaddr >= 0x04000000 && ldaddr < 0x05000000)
  {
    if (!auto_idle_io_load_ok(ldaddr))
      return 1;
    return 0;
  }
  return 1;
}

/* 0 = continue, 1 = reject loop, 2 = loop back to head confirmed */
static int auto_idle_thumb_step(u32 addr, u32 head_pc, u32 *next_addr)
{
  u16 op = auto_idle_fetch16(addr);
  u32 target;
  s32 simm;
  u32 rb;
  u32 imm;
  u32 ldaddr;

  *next_addr = addr + 2;

  if ((op >> 11) == 0x1C)
  {
    simm = (s32)(op & 0x7FF);
    if (simm & 0x400)
      simm |= ~0x7FF;
    simm <<= 1;
    target = addr + 4 + (u32)simm;
    if (target == head_pc)
      return 2;
    return 1;
  }

  if ((op >> 12) == 0xD)
  {
    simm = (s32)((s8)(op & 0xFF));
    simm <<= 1;
    target = addr + 4 + (u32)simm;
    if (target == head_pc)
      return 2;
    return 1;
  }

  if ((op >> 11) >= 0x1E)
    return 1;

  if ((op & 0xFF80) == 0x4700)
    return 1;

  if ((op >> 11) == 0x9)
  {
    ldaddr = ((addr + 4) & ~2u) + (((u32)(op & 0xFF)) << 2);
    return auto_idle_classify_load(ldaddr);
  }

  if ((op >> 12) == 0x6)
  {
    if (((op >> 11) & 1) == 0)
      return 1;
    rb = (op >> 3) & 7;
    imm = ((op >> 6) & 0x1F) << 2;
    if (rb == 15)
    {
      ldaddr = ((addr + 4) & ~2u) + imm;
      return auto_idle_classify_load(ldaddr);
    }
    return 0;
  }

  if ((op >> 12) == 0x5)
    return 1;

  if ((op >> 13) <= 1)
    return 0;

  if (((op >> 12) == 0xB) && ((op & 0x0700) == 0x0100))
    return 1;

  return 0;
}

static int auto_idle_analyze_thumb(u32 head_pc)
{
  u32 addr = head_pc;
  u32 steps;
  u32 next;
  int st;

  for (steps = 0; steps < AUTO_IDLE_MAX_STEPS; steps++)
  {
    st = auto_idle_thumb_step(addr, head_pc, &next);
    if (st == 1)
      return 0;
    if (st == 2)
      return 1;
    addr = next;
    if (addr == head_pc)
      return 0;
  }
  return 0;
}

u8 *cpu_idle_loop_on_block_lookup(u32 pc, u32 is_thumb)
{
  u32 region;

  if (!option_idle_loop_optimize)
  {
    if (auto_idle_remove_pc != AUTO_IDLE_NONE || auto_idle_detect_step >= 0)
      cpu_auto_idle_loop_reset();
    return NULL;
  }

  if (auto_idle_detect_fails > AUTO_IDLE_FAIL_MAX)
    return NULL;

  region = pc >> 24;
  if (region == 0x00)
    goto update_trace;

  if (auto_idle_pc_in_manual_list(pc))
    goto update_trace;

  if (!is_thumb)
  {
    auto_idle_detect_step = -1;
    goto update_trace;
  }

  if (auto_idle_remove_pc != AUTO_IDLE_NONE && pc == auto_idle_remove_pc)
  {
    if (auto_idle_halt_pending)
    {
      auto_idle_halt_pending = 0;
      return (u8 *)&idle_loop_enter_update_gba;
    }
    auto_idle_halt_pending = 1;
    return NULL;
  }

  if (region == auto_idle_last_region && pc == auto_idle_last_pc)
  {
    if (auto_idle_detect_step == 0)
    {
      memcpy(auto_idle_cached, reg, 16 * sizeof(u32));
      auto_idle_detect_step = 1;
    }
    else if (auto_idle_detect_step == 1)
    {
      if (memcmp(auto_idle_cached, reg, 16 * sizeof(u32)) != 0)
      {
        auto_idle_detect_step = -1;
        auto_idle_detect_fails++;
      }
      else
      {
        if (auto_idle_analyze_thumb(pc))
        {
          auto_idle_remove_pc = pc;
          auto_idle_detect_step = -1;
          auto_idle_halt_pending = 0;
        }
        else
          auto_idle_detect_step = -1;
      }
    }
  }
  else
    auto_idle_detect_step = 0;

update_trace:
  auto_idle_last_pc = pc;
  auto_idle_last_region = region;
  return NULL;
}
