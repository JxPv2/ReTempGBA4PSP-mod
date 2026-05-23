#include "common.h"
#include "psp_video.h"
#include <pspge.h>
#include "volume_icon.c"

#define VOLICON_OFFSET (GBA_SCREEN_HEIGHT + 32)
#define GBA_TEXTURE_BYTES (GBA_LINE_SIZE * GBA_SCREEN_HEIGHT * 2)

#define GE_CMD_NOP    0x00
#define GE_CMD_VADDR  0x01
#define GE_CMD_IADDR  0x02
#define GE_CMD_PRIM   0x04
#define GE_CMD_SIGNAL 0x0C
#define GE_CMD_FINISH 0x0F
#define GE_CMD_BASE   0x10
#define GE_CMD_VTYPE  0x12
#define GE_CMD_FBP    0x9C
#define GE_CMD_FBW    0x9D
#define GE_CMD_TBP0   0xA0
#define GE_CMD_TBW0   0xA8
#define GE_CMD_TSIZE0 0xB8
#define GE_CMD_TFLUSH 0xCB
#define GE_CMD_CLEAR  0xD3

#define GE_CMD(op, operand) (((op) << 24) | (operand))

static u32 ALIGN_PSPDATA display_list[512];
static u32 ALIGN_PSPDATA ge_present_cmd[24];
static u32 *ge_present_cmd_end;
static u32 ge_present_fbp_slot;
static u32 ge_present_fbw_slot;
static u32 ge_present_tbp0_slot;
static u32 ge_present_tbw0_slot;
static int ge_present_cb_id;

static float ALIGN_PSPDATA screen_sprite[10];

static void *disp_frame;
static void *draw_frame;

typedef struct
{
  u16 u, v;
  u16 x, y, z;
} Vertex;

static void set_gba_resolution(void);
static void update_screen_sprite(float scale_x, float scale_y);
static void init_ge_present(void);
static void bitbilt_gu(void);
static void bitbilt_sw(void);
static void *psp_vram_addr(void *frame, u32 x, u32 y);
static void writeback_screen_texture(void);
static void load_volume_icon(int devkit_version);
static void draw_volume(int volume);

static void ge_present_finish(int id, void *arg)
{
  (void)id;
  (void)arg;
}

static void writeback_screen_texture(void)
{
  sceKernelDcacheWritebackInvalidateRange(screen_texture, GBA_TEXTURE_BYTES);
}

static void writeback_ge_present(void)
{
  u32 bytes = (u32)((u8 *)ge_present_cmd_end - (u8 *)ge_present_cmd);

  sceKernelDcacheWritebackInvalidateRange(ge_present_cmd, bytes);
  sceKernelDcacheWritebackInvalidateRange(screen_sprite, sizeof(screen_sprite));
}

static void patch_ge_present_targets(void *framebuffer, u16 *texture)
{
  u32 fb_addr = 0x04000000 + (u32)framebuffer;
  u32 tex_addr = (u32)texture;

  ge_present_cmd[ge_present_fbp_slot] =
    GE_CMD(GE_CMD_FBP, fb_addr & 0x00FFFFFF);
  ge_present_cmd[ge_present_fbw_slot] =
    GE_CMD(GE_CMD_FBW, ((fb_addr & 0xFF000000) >> 8) | PSP_LINE_SIZE);

  ge_present_cmd[ge_present_tbp0_slot] =
    GE_CMD(GE_CMD_TBP0, tex_addr & 0x00FFFFFF);
  ge_present_cmd[ge_present_tbw0_slot] =
    GE_CMD(GE_CMD_TBW0, ((tex_addr & 0xFF000000) >> 8) | GBA_LINE_SIZE);
}

static void init_ge_present(void)
{
  u32 *cmd = ge_present_cmd;
  u32 fb_addr = 0x04000000 + (u32)draw_frame;
  u32 tex_addr = (u32)screen_texture;
  PspGeCallbackData ge_cb;

  ge_present_fbp_slot = (u32)(cmd - ge_present_cmd);
  *cmd++ = GE_CMD(GE_CMD_FBP, fb_addr & 0x00FFFFFF);

  ge_present_fbw_slot = (u32)(cmd - ge_present_cmd);
  *cmd++ = GE_CMD(GE_CMD_FBW, ((fb_addr & 0xFF000000) >> 8) | PSP_LINE_SIZE);

  ge_present_tbp0_slot = (u32)(cmd - ge_present_cmd);
  *cmd++ = GE_CMD(GE_CMD_TBP0, tex_addr & 0x00FFFFFF);

  ge_present_tbw0_slot = (u32)(cmd - ge_present_cmd);
  *cmd++ = GE_CMD(GE_CMD_TBW0, ((tex_addr & 0xFF000000) >> 8) | GBA_LINE_SIZE);

  *cmd++ = GE_CMD(GE_CMD_TSIZE0, (8 << 8) | 8);
  *cmd++ = GE_CMD(GE_CMD_TFLUSH, 0);
  *cmd++ = GE_CMD(GE_CMD_VTYPE, (1 << 23) | (3 << 7) | 3);
  *cmd++ = GE_CMD(GE_CMD_BASE, 0);
  *cmd++ = GE_CMD(GE_CMD_IADDR, 0);
  *cmd++ = GE_CMD(GE_CMD_BASE, ((u32)screen_sprite & 0xFF000000) >> 8);
  *cmd++ = GE_CMD(GE_CMD_VADDR, (u32)screen_sprite & 0x00FFFFFF);
  *cmd++ = GE_CMD(GE_CMD_PRIM, (6 << 16) | 2);
  *cmd++ = GE_CMD(GE_CMD_FINISH, 0);

  ge_present_cmd_end = cmd;

  ge_cb.finish_func = ge_present_finish;
  ge_cb.finish_arg = NULL;
  ge_cb.signal_func = NULL;
  ge_cb.signal_arg = NULL;
  ge_present_cb_id = sceGeSetCallback(&ge_cb);
}

static void update_screen_sprite(float scale_x, float scale_y)
{
  s32 dw = (s32)(GBA_SCREEN_WIDTH  * scale_x);
  s32 dh = (s32)(GBA_SCREEN_HEIGHT * scale_y);
  s32 dx = (PSP_SCREEN_WIDTH  - dw) / 2;
  s32 dy = (PSP_SCREEN_HEIGHT - dh) / 2;

  screen_sprite[0] = 0.0f;
  screen_sprite[1] = 0.0f;
  screen_sprite[2] = (float)dx;
  screen_sprite[3] = (float)dy;
  screen_sprite[4] = 0.0f;

  screen_sprite[5] = (float)GBA_SCREEN_WIDTH;
  screen_sprite[6] = (float)GBA_SCREEN_HEIGHT;
  screen_sprite[7] = (float)(dx + dw);
  screen_sprite[8] = (float)(dy + dh);
  screen_sprite[9] = 0.0f;

  sceKernelDcacheWritebackInvalidateRange(screen_sprite, sizeof(screen_sprite));
}

int (*__draw_volume_status)(int draw);


void init_video(int devkit_version)
{
  u32 ALIGN_PSPDATA clear_list[4];

  disp_frame = (void *)0;
  draw_frame = (void *)PSP_FRAME_SIZE;

  sceGuDisplay(GU_FALSE);

  sceGuInit();
  sceGuStart(GU_DIRECT, display_list);

  sceGuDrawBuffer(GU_PSM_5551, draw_frame, PSP_LINE_SIZE);
  sceGuDispBuffer(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT, disp_frame, PSP_LINE_SIZE);

  sceGuOffset(2048 - (PSP_SCREEN_WIDTH / 2), 2048 - (PSP_SCREEN_HEIGHT / 2));
  sceGuViewport(2048, 2048, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);

  sceGuScissor(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
  sceGuEnable(GU_SCISSOR_TEST);

  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
  sceGuTexMode(GU_PSM_5551, 0, 0, GU_FALSE);
  sceGuTexImage(0, 256, 256, GBA_LINE_SIZE, screen_texture);
  sceGuTexFlush();
  sceGuEnable(GU_TEXTURE_2D);

  sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
  sceGuDisable(GU_BLEND);

  sceGuFinish();
  sceGuSync(0, GU_SYNC_FINISH);

  sceDisplayWaitVblankStart();
  sceGuDisplay(GU_TRUE);

  update_screen_sprite(1.5f, 1.5f);
  init_ge_present();
  update_screen = bitbilt_gu;

  sceGuStart(GU_DIRECT, clear_list);
  sceGuClearColor(0);
  sceGuClear(GU_COLOR_BUFFER_BIT | GU_FAST_CLEAR_BIT);
  sceGuFinish();
  sceGuSync(0, GU_SYNC_FINISH);

  load_volume_icon(devkit_version);
}

void video_term(void)
{
  sceGuDisplay(GU_FALSE);
  sceGuTerm();
}

void flip_screen(u32 vsync)
{
  if (vsync != 0) sceDisplayWaitVblankStart();

  disp_frame = draw_frame;
  draw_frame = sceGuSwapBuffers();
}

static void *psp_vram_addr(void *frame, u32 x, u32 y)
{
  return (void *)(((u32)frame | 0x44000000) + ((x + (y << 9)) << 1));
}

static void bitbilt_gu(void)
{
  u32 ALIGN_PSPDATA clear_list[4];

  writeback_screen_texture();
  patch_ge_present_targets(draw_frame, screen_texture);
  writeback_ge_present();

  sceGuClearColor(0);
  sceGuStart(GU_DIRECT, clear_list);
  sceGuDrawBufferList(GU_PSM_5551, draw_frame, PSP_LINE_SIZE);
  sceGuClear(GU_COLOR_BUFFER_BIT | GU_FAST_CLEAR_BIT);
  sceGuFinish();

  sceGeListEnQueue(ge_present_cmd, ge_present_cmd_end, ge_present_cb_id, NULL);
  sceGuSync(0, GU_SYNC_FINISH);
}

#define NORMAL_BLEND(c0, c1) ((c0 & c1) + (((c0 ^ c1) & 0x7bde) >> 1))

static void bitbilt_sw(void)
{
  u32 x, y;
  u16 *vptr, *vptr0;
  u16 *d, *d0;
  u32 ALIGN_PSPDATA clear_list[4];

  writeback_screen_texture();

  sceGuStart(GU_DIRECT, clear_list);
  sceGuClearColor(0);
  sceGuDrawBufferList(GU_PSM_5551, draw_frame, PSP_LINE_SIZE);
  sceGuClear(GU_COLOR_BUFFER_BIT | GU_FAST_CLEAR_BIT);
  sceGuFinish();
  sceGuSync(0, GU_SYNC_FINISH);

  vptr0 = (u16 *)psp_vram_addr(draw_frame, 60, 16);
  d0 = screen_texture;

  for (y = 0; y < (GBA_SCREEN_HEIGHT / 2); y++)
  {
    vptr = vptr0;
    d = d0;

    for (x = 0; x < (GBA_SCREEN_WIDTH / 2); x++, d += 2)
    {
      vptr[0] = d[0];
      vptr[2] = d[1];

      vptr[0 + PSP_LINE_SIZE * 2] = d[0 + GBA_LINE_SIZE];
      vptr[2 + PSP_LINE_SIZE * 2] = d[1 + GBA_LINE_SIZE];

      *++vptr = NORMAL_BLEND(d[0], d[1]);
      vptr += (PSP_LINE_SIZE - 1);

      *vptr++ = NORMAL_BLEND(d[0], d[0 + GBA_LINE_SIZE]);
      *vptr++ = NORMAL_BLEND(NORMAL_BLEND(d[0], d[0 + GBA_LINE_SIZE]), NORMAL_BLEND(d[1], d[1 + GBA_LINE_SIZE]));
      *vptr   = NORMAL_BLEND(d[1], d[1 + GBA_LINE_SIZE]);
      vptr += (PSP_LINE_SIZE - 1);

      *vptr   = NORMAL_BLEND(d[0 + GBA_LINE_SIZE], d[1 + GBA_LINE_SIZE]);
      vptr += (2 - PSP_LINE_SIZE * 2);
    }

    vptr0 += (PSP_LINE_SIZE * 3);
    d0 += (GBA_LINE_SIZE * 2);
  }
}

void video_resolution_large(void)
{
}

void video_resolution_small(void)
{
  set_gba_resolution();

  sceGuStart(GU_DIRECT, display_list);

  sceGuClearColor(0);
  sceGuClear(GU_COLOR_BUFFER_BIT | GU_FAST_CLEAR_BIT);

  sceGuTexFilter(option_screen_filter, option_screen_filter);

  sceGuFinish();
  sceGuSync(0, GU_SYNC_FINISH);
}

static void set_gba_resolution(void)
{
  switch (option_screen_scale)
  {
    case SCALED_NONE:
      update_screen_sprite(1.0f, 1.0f);
      update_screen = bitbilt_gu;
      break;

    case SCALED_X15_GU:
      update_screen_sprite(1.5f, 1.5f);
      update_screen = bitbilt_gu;
      break;

    case SCALED_X15_SW:
      update_screen = bitbilt_sw;
      break;

    case SCALED_USER:
      update_screen_sprite(option_screen_mag / 100.0f, option_screen_mag / 100.0f);
      update_screen = bitbilt_gu;
      break;

    case SCALED_16X9_GU:
      update_screen_sprite((float)PSP_SCREEN_WIDTH / (float)GBA_SCREEN_WIDTH,
                           (float)PSP_SCREEN_HEIGHT / (float)GBA_SCREEN_HEIGHT);
      update_screen = bitbilt_gu;
      break;
  }
}

void clear_screen(u32 color)
{
  u32 r8 = (color >>  0) & 0xFF;
  u32 g8 = (color >>  8) & 0xFF;
  u32 b8 = (color >> 16) & 0xFF;
  u16 color16 = ((r8 >> 3) << 0) | ((g8 >> 3) << 5) | ((b8 >> 3) << 10) | 0x8000;

  u16 *vram_ptr = (u16 *)((u32)draw_frame | 0x44000000);
  u32 pixels = PSP_LINE_SIZE * PSP_SCREEN_HEIGHT;

  for (u32 i = 0; i < pixels; i++)
  {
    vram_ptr[i] = color16;
  }
}

void clear_texture(u16 color)
{
  u32 x, y;
  u16 *p_dest, *p_dest0;

  p_dest0 = screen_texture;

  for (y = 0; y < GBA_SCREEN_HEIGHT; y++)
  {
    p_dest = p_dest0;

    for (x = 0; x < GBA_SCREEN_WIDTH; x++, p_dest++)
      *p_dest = color;

    p_dest0 += GBA_LINE_SIZE;
  }
}

u16 *copy_screen(void)
{
  u32 x, y;
  u16 *copy;
  u16 *p_src, *p_src0;
  u16 *p_dest;

  sceDisplayWaitVblankStart();
  sceGuSync(0, GU_SYNC_FINISH);
  writeback_screen_texture();

  copy = (u16 *)safe_malloc(GBA_SCREEN_SIZE);

  p_src0 = screen_texture;
  p_dest = copy;

  for (y = 0; y < GBA_SCREEN_HEIGHT; y++)
  {
    p_src = p_src0;

    for (x = 0; x < GBA_SCREEN_WIDTH; x++, p_src++, p_dest++)
      *p_dest = *p_src;

    p_src0 += GBA_LINE_SIZE;
  }

  return copy;
}

void blit_to_screen(u16 *src, u16 w, u16 h, u16 dest_x, u16 dest_y)
{
  u32 x, y;
  u16 *p_src, *p_dest, *p_dest0;

  p_src   = src;
  p_dest0 = (u16 *)psp_vram_addr(draw_frame, dest_x, dest_y);

  for (y = 0; y < h; y++)
  {
    p_dest = p_dest0;

    for (x = 0; x < w; x++, p_src++, p_dest++)
      *p_dest = *p_src;

    p_dest0 += PSP_LINE_SIZE;
  }
}

void draw_box_line(u16 x1, u16 y1, u16 x2, u16 y2, u16 color)
{
  draw_hline(x1, x2, y1, color);
  draw_hline(x1, x2, y2, color);
  draw_vline(x1, y1, y2, color);
  draw_vline(x2, y1, y2, color);
}

void draw_box_fill(u16 x1, u16 y1, u16 x2, u16 y2, u16 color)
{
  u32 y;

  for (y = y1; y <= y2; y++)
    draw_hline(x1, x2, y, color);
}

static u16 blend_pixel5551(u16 dst, u32 src8888)
{
  u32 sa = (src8888 >> 24) & 0xFF;

  if (sa == 0)
    return dst;

  if (sa >= 255)
  {
    u32 r5 = ((src8888 >>  0) & 0xFF) >> 3;
    u32 g5 = ((src8888 >>  8) & 0xFF) >> 3;
    u32 b5 = ((src8888 >> 16) & 0xFF) >> 3;

    return (u16)(0x8000 | (b5 << 10) | (g5 << 5) | r5);
  }

  u32 dr = COL15_GET_R8(dst);
  u32 dg = COL15_GET_G8(dst);
  u32 db = COL15_GET_B8(dst);
  u32 sr = src8888 & 0xFF;
  u32 sg = (src8888 >> 8) & 0xFF;
  u32 sb = (src8888 >> 16) & 0xFF;
  u32 inv = 255 - sa;
  u32 r8 = (sr * sa + dr * inv) / 255;
  u32 g8 = (sg * sa + dg * inv) / 255;
  u32 b8 = (sb * sa + db * inv) / 255;

  return (u16)(0x8000 | ((b8 >> 3) << 10) | ((g8 >> 3) << 5) | (r8 >> 3));
}

void draw_box_alpha(u16 x1, u16 y1, u16 x2, u16 y2, u32 color)
{
  u32 x, y;
  u16 *dst;

  for (y = y1; y <= y2; y++)
  {
    dst = (u16 *)psp_vram_addr(draw_frame, x1, y);

    for (x = x1; x <= x2; x++, dst++)
      *dst = blend_pixel5551(*dst, color);
  }
}

void draw_hline(u16 sx, u16 ex, u16 y, u16 color)
{
  u32 width = (u32)ex - sx + 1;
  u16 *dst = (u16 *)psp_vram_addr(draw_frame, sx, y);
  u32 i;

  for (i = 0; i < width; i++)
    dst[i] = color;
}

void draw_vline(u16 x, u16 sy, u16 ey, u16 color)
{
  u32 y;
  u16 *dst = (u16 *)psp_vram_addr(draw_frame, x, sy);

  for (y = sy; y <= ey; y++, dst += PSP_LINE_SIZE)
    *dst = color;
}

void print_string(const char *str, s16 x, u16 y, u16 fg_color, s16 bg_color)
{
  if (x < 0) x = (PSP_SCREEN_WIDTH - (strlen(str) * FONTWIDTH)) >> 1;

  mh_print(str, x, y, fg_color, bg_color, (u16 *)((u32)draw_frame | 0x44000000), PSP_LINE_SIZE);
}

void print_string_ext(const char *str, s16 x, u16 y, u16 fg_color, s16 bg_color, void *_dest_ptr, u16 pitch)
{
  if (x < 0) x = (pitch - (strlen(str) * FONTWIDTH)) >> 1;

  mh_print(str, x, y, fg_color, bg_color, _dest_ptr, pitch);
}

void print_string_gbk(const char *str, s16 x, u16 y, u16 fg_color, s16 bg_color)
{
  if (x < 0) x = (PSP_SCREEN_WIDTH - (strlen(str) * FONTWIDTH)) >> 1;

  ch_print(str, x, y, fg_color, bg_color, (u16 *)((u32)draw_frame | 0x44000000), PSP_LINE_SIZE);
}

void print_string_ext_gbk(const char *str, s16 x, u16 y, u16 fg_color, s16 bg_color, void *_dest_ptr, u16 pitch)
{
  if (x < 0) x = (pitch - (strlen(str) * FONTWIDTH)) >> 1;

  ch_print(str, x, y, fg_color, bg_color, _dest_ptr, pitch);
}

static void load_volume_icon(int devkit_version)
{
  if (devkit_version >= 0x03050210)
  {
    u32 x, y, alpha;
    u16 *dst, *tex_volicon;

    tex_volicon = screen_texture + (GBA_LINE_SIZE * VOLICON_OFFSET);

    dst = tex_volicon + SPEEKER_X;
    for (y = 0; y < 32; y++)
    {
      for (x = 0; x < 32; x++)
      {
        if ((x & 1) != 0)
          alpha = icon_speeker[y][(x >> 1)] >> 4;
        else
          alpha = icon_speeker[y][(x >> 1)] & 0x0f;

        dst[x] = (alpha << 12) | 0x0fff;
      }
      dst += GBA_LINE_SIZE;
    }

    dst = tex_volicon + SPEEKER_SHADOW_X;
    for (y = 0; y < 32; y++)
    {
      for (x = 0; x < 32; x++)
      {
        if ((x & 1) != 0)
          alpha = icon_speeker_shadow[y][(x >> 1)] >> 4;
        else
          alpha = icon_speeker_shadow[y][(x >> 1)] & 0x0f;

        dst[x] = alpha << 12;
      }
      dst += GBA_LINE_SIZE;
    }

    dst = tex_volicon + VOLUME_BAR_X;
    for (y = 0; y < 32; y++)
    {
      for (x = 0; x < 12; x++)
      {
        if ((x & 1) != 0)
          alpha = icon_bar[y][(x >> 1)] >> 4;
        else
          alpha = icon_bar[y][(x >> 1)] & 0x0f;

        dst[x] = (alpha << 12) | 0x0fff;
      }
      dst += GBA_LINE_SIZE;
    }

    dst = tex_volicon + VOLUME_BAR_SHADOW_X;
    for (y = 0; y < 32; y++)
    {
      for (x = 0; x < 12; x++)
      {
        if ((x & 1) != 0)
          alpha = icon_bar_shadow[y][(x >> 1)] >> 4;
        else
          alpha = icon_bar_shadow[y][(x >> 1)] & 0x0f;

        dst[x] = alpha << 12;
      }
      dst += GBA_LINE_SIZE;
    }

    dst = tex_volicon + VOLUME_DOT_X;
    for (y = 0; y < 32; y++)
    {
      for (x = 0; x < 12; x++)
      {
        if ((x & 1) != 0)
          alpha = icon_dot[y][(x >> 1)] >> 4;
        else
          alpha = icon_dot[y][(x >> 1)] & 0x0f;

        dst[x] = (alpha << 12) | 0x0fff;
      }
      dst += GBA_LINE_SIZE;
    }

    dst = tex_volicon + VOLUME_DOT_SHADOW_X;
    for (y = 0; y < 32; y++)
    {
      for (x = 0; x < 12; x++)
      {
        if ((x & 1) != 0)
          alpha = icon_dot_shadow[y][(x >> 1)] >> 4;
        else
          alpha = icon_dot_shadow[y][(x >> 1)] & 0x0f;

        dst[x] = alpha << 12;
      }
      dst += GBA_LINE_SIZE;
    }
  }
}

static void draw_volume(int volume)
{
  Vertex *vertices, *vertices_tmp;

  writeback_screen_texture();

  sceGuStart(GU_DIRECT, display_list);

  sceGuEnable(GU_BLEND);
  sceGuTexMode(GU_PSM_4444, 0, 0, GU_FALSE);

  vertices = (Vertex *)sceGuGetMemory(2 * 31 * 2 * sizeof(Vertex));

  if (vertices != NULL)
  {
    int i, x;

    memset(vertices, 0, 2 * 31 * 2 * sizeof(Vertex));
    vertices_tmp = vertices;

    x = 24;

    vertices_tmp[0].u = SPEEKER_SHADOW_X;
    vertices_tmp[0].v = 0 + VOLICON_OFFSET;
    vertices_tmp[0].x = 3 + x;
    vertices_tmp[0].y = 3 + 230;

    vertices_tmp[1].u = SPEEKER_SHADOW_X + 32;
    vertices_tmp[1].v = 32 + VOLICON_OFFSET;
    vertices_tmp[1].x = 3 + x + 32;
    vertices_tmp[1].y = 3 + 230 + 32;

    vertices_tmp += 2;

    vertices_tmp[0].u = SPEEKER_X;
    vertices_tmp[0].v = 0 + VOLICON_OFFSET;
    vertices_tmp[0].x = x;
    vertices_tmp[0].y = 230;

    vertices_tmp[1].u = SPEEKER_X + 32;
    vertices_tmp[1].v = 32 + VOLICON_OFFSET;
    vertices_tmp[1].x = x + 32;
    vertices_tmp[1].y = 230 + 32;

    vertices_tmp += 2;

    x = 64;

    for (i = 0; i < volume; i++)
    {
      vertices_tmp[0].u = VOLUME_BAR_SHADOW_X;
      vertices_tmp[0].v = 0 + VOLICON_OFFSET;
      vertices_tmp[0].x = 3 + x;
      vertices_tmp[0].y = 3 + 230;

      vertices_tmp[1].u = VOLUME_BAR_SHADOW_X + 12;
      vertices_tmp[1].v = 32 + VOLICON_OFFSET;
      vertices_tmp[1].x = 3 + x + 12;
      vertices_tmp[1].y = 3 + 230 + 32;

      vertices_tmp += 2;

      vertices_tmp[0].u = VOLUME_BAR_X;
      vertices_tmp[0].v = 0 + VOLICON_OFFSET;
      vertices_tmp[0].x = x;
      vertices_tmp[0].y = 230;

      vertices_tmp[1].u = VOLUME_BAR_X + 12;
      vertices_tmp[1].v = 32 + VOLICON_OFFSET;
      vertices_tmp[1].x = x + 12;
      vertices_tmp[1].y = 230 + 32;

      vertices_tmp += 2;

      x += 12;
    }

    for (; i < 30; i++)
    {
      vertices_tmp[0].u = VOLUME_DOT_SHADOW_X;
      vertices_tmp[0].v = 0 + VOLICON_OFFSET;
      vertices_tmp[0].x = 3 + x;
      vertices_tmp[0].y = 3 + 230;

      vertices_tmp[1].u = VOLUME_DOT_SHADOW_X + 12;
      vertices_tmp[1].v = 32 + VOLICON_OFFSET;
      vertices_tmp[1].x = 3 + x + 12;
      vertices_tmp[1].y = 3 + 230 + 32;

      vertices_tmp += 2;

      vertices_tmp[0].u = VOLUME_DOT_X;
      vertices_tmp[0].v = 0 + VOLICON_OFFSET;
      vertices_tmp[0].x = x;
      vertices_tmp[0].y = 230;

      vertices_tmp[1].u = VOLUME_DOT_X + 12;
      vertices_tmp[1].v = 32 + VOLICON_OFFSET;
      vertices_tmp[1].x = x + 12;
      vertices_tmp[1].y = 230 + 32;

      vertices_tmp += 2;

      x += 12;
    }

    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2 * 31 * 2, 0, vertices);
  }

  sceGuDisable(GU_BLEND);
  sceGuTexMode(GU_PSM_5551, 0, 0, GU_FALSE);

  sceGuFinish();
  sceGuSync(0, GU_SYNC_FINISH);
}

int draw_volume_status(int draw)
{
  static u64 disp_end = 0;
  int volume = kuImposeGetParam(PSP_IMPOSE_MAIN_VOLUME);

  if ((volume < 0) || (volume > 30))
    return 0;

  if (get_pad_input(PSP_CTRL_VOLUP | PSP_CTRL_VOLDOWN | PSP_CTRL_NOTE) != 0)
  {
    disp_end = ticker() + (2 * 1000 * 1000);
    draw = 1;
  }

  if (disp_end != 0)
  {
    if (ticker() < disp_end)
    {
      if (draw != 0)
        draw_volume(volume);
    }
    else
    {
      disp_end = 0;
    }
  }

  return 0;
}

int draw_volume_status_null(int draw)
{
  return 0;
}
