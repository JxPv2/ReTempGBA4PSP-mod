#include "common.h"
#include "boxart.h"

extern char dir_boxart[MAX_PATH];

static u16 *boxart_buffer = NULL;
static u32 boxart_loaded = 0;

#define BOXART_W  141
#define BOXART_H  141

static inline u16 rgba_to_psp5551(u8 r, u8 g, u8 b)
{
    u16 r5 = r >> 3;
    u16 g5 = g >> 3;
    u16 b5 = b >> 3;
    return 0x8000 | (b5 << 10) | (g5 << 5) | r5;
}

static void scale_nearest(u8 *src, u16 *dst, u32 src_w, u32 src_h, u32 src_pitch)
{
    u32 x, y;
    for (y = 0; y < BOXART_H; y++)
    {
        u32 src_y = (y * src_h) / BOXART_H;
        for (x = 0; x < BOXART_W; x++)
        {
            u32 src_x = (x * src_w) / BOXART_W;
            u32 src_idx = (src_y * src_pitch) + (src_x * 4);
            u8 r = src[src_idx + 0];
            u8 g = src[src_idx + 1];
            u8 b = src[src_idx + 2];
            dst[y * BOXART_W + x] = rgba_to_psp5551(r, g, b);
        }
    }
}

/* ========================================================================
   Minimal PNG decoder — replaces libpng to avoid setjmp/longjmp issues
   Supports: non-interlaced RGB/RGBA/gray/palette PNGs up to 1024x1024
   ======================================================================== */

#define PNG_MAKE_U32(a,b,c,d) (((u32)(a)<<24)|((u32)(b)<<16)|((u32)(c)<<8)|(d))
#define PNG_IHDR PNG_MAKE_U32('I','H','D','R')
#define PNG_IDAT PNG_MAKE_U32('I','D','A','T')
#define PNG_IEND PNG_MAKE_U32('I','E','N','D')
#define PNG_PLTE PNG_MAKE_U32('P','L','T','E')
#define PNG_tRNS PNG_MAKE_U32('t','R','N','S')

static u32 png_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u32 png_crc32(const u8 *buf, u32 len)
{
    static const u32 crc_table[16] = {
        0x00000000,0x1db71064,0x3b6e20c8,0x26d930ac,
        0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,
        0xedb88320,0xf00f9344,0xd6d6a3e8,0xcb61b38c,
        0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c
    };
    u32 c = ~0U;
    u32 i;
    for (i = 0; i < len; i++) {
        c ^= buf[i];
        c = (c >> 4) ^ crc_table[c & 0xF];
        c = (c >> 4) ^ crc_table[c & 0xF];
    }
    return ~c;
}

/* zlib inflate — minimal, using the existing zlib linked into the project */
#include <zlib.h>

static int png_inflate(const u8 *src, u32 src_len, u8 *dst, u32 dst_len)
{
    z_stream zs;
    int ret;
    memset(&zs, 0, sizeof(zs));
    zs.next_in = (Bytef *)src;
    zs.avail_in = src_len;
    zs.next_out = dst;
    zs.avail_out = dst_len;
    ret = inflateInit(&zs);
    if (ret != Z_OK) return -1;
    ret = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (ret != Z_STREAM_END) return -1;
    return (int)zs.total_out;
}

/* Paeth predictor for PNG filtering */
static u8 png_paeth(u8 a, u8 b, u8 c)
{
    int p = (int)a + (int)b - (int)c;
    int pa = p > (int)a ? p - (int)a : (int)a - p;
    int pb = p > (int)b ? p - (int)b : (int)b - p;
    int pc = p > (int)c ? p - (int)c : (int)c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* Decode a PNG file buffer into raw RGBA8.
   Returns pointer to RGBA buffer (malloc'd) or NULL on failure.
   Outputs width/height via out_w/out_h. */
static u8 *png_decode_buffer(const u8 *data, u32 data_size, u32 *out_w, u32 *out_h)
{
    u32 pos = 0;
    u32 img_w = 0, img_h = 0, bpp = 4;
    u8 bit_depth = 8, color_type = 2;
    u8 have_plte = 0, have_trns = 0;
    u8 plte[256*3];
    u8 trns[256];
    u8 *idat_buf = NULL;
    u32 idat_size = 0;
    u8 *rgba = NULL;
    u32 y, x;

    if (data_size < 33) return NULL;
    if (data[0] != 0x89 || data[1] != 'P' || data[2] != 'N' || data[3] != 'G')
        return NULL;
    pos = 8;

    /* Parse chunks */
    while (pos + 12 <= data_size) {
        u32 chunk_len = png_be32(data + pos);
        u32 chunk_type = png_be32(data + pos + 4);
        const u8 *chunk_data = data + pos + 8;
        u32 chunk_crc = png_be32(data + pos + 8 + chunk_len);

        if (pos + 12 + chunk_len > data_size) break;
        /* Skip CRC check for speed — not security-critical here */
        (void)chunk_crc;

        if (chunk_type == PNG_IHDR) {
            if (chunk_len != 13) goto fail;
            img_w = png_be32(chunk_data);
            img_h = png_be32(chunk_data + 4);
            bit_depth = chunk_data[8];
            color_type = chunk_data[9];
            /* compression=chunk_data[10], filter=chunk_data[11], interlace=chunk_data[12] */
            if (chunk_data[12] != 0) goto fail; /* no interlace support */
            if (img_w > 1024 || img_h > 1024) goto fail;
            if (bit_depth != 8) goto fail; /* only 8-bit supported */
            /* Determine bytes per pixel after decode */
            if (color_type == 0) bpp = 1;       /* gray */
            else if (color_type == 2) bpp = 3;  /* RGB */
            else if (color_type == 3) bpp = 1;  /* palette */
            else if (color_type == 4) bpp = 2;  /* gray+alpha */
            else if (color_type == 6) bpp = 4;  /* RGBA */
            else goto fail;
        }
        else if (chunk_type == PNG_PLTE) {
            if (chunk_len > 768) goto fail;
            memcpy(plte, chunk_data, chunk_len);
            have_plte = 1;
        }
        else if (chunk_type == PNG_tRNS) {
            if (chunk_len > 256) goto fail;
            memset(trns, 255, 256);
            memcpy(trns, chunk_data, chunk_len);
            have_trns = 1;
        }
        else if (chunk_type == PNG_IDAT) {
            u8 *new_buf = (u8 *)realloc(idat_buf, idat_size + chunk_len);
            if (!new_buf) goto fail;
            idat_buf = new_buf;
            memcpy(idat_buf + idat_size, chunk_data, chunk_len);
            idat_size += chunk_len;
        }
        else if (chunk_type == PNG_IEND) {
            break;
        }
        pos += 12 + chunk_len;
    }

    if (!img_w || !img_h || !idat_buf) goto fail;

    /* Decompress IDAT */
    u32 raw_stride = img_w * bpp + 1; /* +1 for filter byte per row */
    u32 raw_size = raw_stride * img_h;
    u8 *raw = (u8 *)malloc(raw_size);
    if (!raw) goto fail;

    int inflated = png_inflate(idat_buf, idat_size, raw, raw_size);
    if (inflated != (int)raw_size) {
        free(raw);
        goto fail;
    }

    /* Convert to RGBA8 */
    rgba = (u8 *)malloc(img_w * img_h * 4);
    if (!rgba) {
        free(raw);
        goto fail;
    }

    for (y = 0; y < img_h; y++) {
        u8 filter = raw[y * raw_stride];
        const u8 *row = raw + y * raw_stride + 1;
        u8 *out_row = rgba + y * img_w * 4;

        for (x = 0; x < img_w; x++) {
            u8 r, g, b, a;
            u32 idx = x * bpp;

            /* Undo filter */
            u8 left[4] = {0,0,0,0};
            u8 above[4] = {0,0,0,0};
            u8 left_above[4] = {0,0,0,0};
            u8 raw_px[4];
            u8 out_px[4];
            int c;

            if (x > 0) {
                for (c = 0; c < (int)bpp; c++) raw_px[c] = row[idx + c];
                for (c = 0; c < 4; c++) left[c] = out_row[(x-1)*4 + c];
            } else {
                for (c = 0; c < (int)bpp; c++) raw_px[c] = row[idx + c];
            }
            if (y > 0) {
                u8 *prev_out = rgba + (y-1) * img_w * 4;
                for (c = 0; c < 4; c++) above[c] = prev_out[x*4 + c];
                if (x > 0) {
                    for (c = 0; c < 4; c++) left_above[c] = prev_out[(x-1)*4 + c];
                }
            }

            /* Apply filter */
            for (c = 0; c < (int)bpp; c++) {
                u8 v = raw_px[c];
                switch (filter) {
                    case 0: out_px[c] = v; break;
                    case 1: out_px[c] = v + left[c]; break;
                    case 2: out_px[c] = v + above[c]; break;
                    case 3: out_px[c] = v + (left[c] + above[c]) / 2; break;
                    case 4: out_px[c] = v + png_paeth(left[c], above[c], left_above[c]); break;
                    default: out_px[c] = v; break;
                }
            }

            /* Convert to RGBA */
            if (color_type == 0) { /* gray */
                r = g = b = out_px[0];
                a = 255;
            }
            else if (color_type == 2) { /* RGB */
                r = out_px[0]; g = out_px[1]; b = out_px[2];
                a = 255;
            }
            else if (color_type == 3) { /* palette */
                u32 pidx = out_px[0];
                r = plte[pidx*3+0];
                g = plte[pidx*3+1];
                b = plte[pidx*3+2];
                a = have_trns ? trns[pidx] : 255;
            }
            else if (color_type == 4) { /* gray+alpha */
                r = g = b = out_px[0];
                a = out_px[1];
            }
            else { /* RGBA */
                r = out_px[0]; g = out_px[1]; b = out_px[2]; a = out_px[3];
            }

            out_row[x*4+0] = r;
            out_row[x*4+1] = g;
            out_row[x*4+2] = b;
            out_row[x*4+3] = a;
        }
    }

    free(raw);
    free(idat_buf);
    *out_w = img_w;
    *out_h = img_h;
    return rgba;

fail:
    if (idat_buf) free(idat_buf);
    return NULL;
}

/* ========================================================================
   End minimal PNG decoder
   ======================================================================== */

void boxart_load(const char *rom_name)
{
    char png_path[MAX_PATH];
    SceUID fd = -1;
    s32 file_size = 0;
    u8 *file_buffer = NULL;
    u8 *rgba = NULL;
    u32 img_w = 0, img_h = 0;

    /* Free old buffer */
    if (boxart_buffer)
    {
        free(boxart_buffer);
        boxart_buffer = NULL;
    }
    boxart_loaded = 0;

    /* Validate inputs */
    if (!rom_name || !dir_boxart[0])
        return;

    /* Build path safely */
    png_path[0] = '\0';
    strncat(png_path, dir_boxart, MAX_PATH - 1);
    strncat(png_path, rom_name, MAX_PATH - strlen(png_path) - 1);

    char *last_slash = strrchr(png_path, '/');
    char *last_dot = strrchr(png_path, '.');
    if (last_dot && (!last_slash || last_dot > last_slash))
    {
        if ((last_dot - png_path) + 5 < MAX_PATH)
            strcpy(last_dot, ".png");
        else
        {
            png_path[MAX_PATH - 5] = '\0';
            strcat(png_path, ".png");
        }
    }
    else
    {
        if (strlen(png_path) + 4 < MAX_PATH)
            strcat(png_path, ".png");
        else
            return;
    }

    /* Open and read entire file */
    fd = sceIoOpen(png_path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return;

    file_size = sceIoLseek(fd, 0, SEEK_END);
    sceIoLseek(fd, 0, SEEK_SET);

    if (file_size <= 0 || file_size > (2 * 1024 * 1024))
        goto cleanup;

    file_buffer = (u8 *)malloc(file_size);
    if (!file_buffer)
        goto cleanup;

    s32 read_total = 0;
    while (read_total < file_size)
    {
        s32 n = sceIoRead(fd, file_buffer + read_total, file_size - read_total);
        if (n <= 0)
            goto cleanup;
        read_total += n;
    }
    sceIoClose(fd);
    fd = -1;

    /* Decode PNG */
    rgba = png_decode_buffer(file_buffer, (u32)file_size, &img_w, &img_h);
    if (!rgba || img_w == 0 || img_h == 0)
        goto cleanup;

    /* Allocate final buffer and scale */
    boxart_buffer = (u16 *)malloc(BOXART_W * BOXART_H * 2);
    if (!boxart_buffer)
        goto cleanup;

    scale_nearest(rgba, boxart_buffer, img_w, img_h, img_w * 4);
    boxart_loaded = 1;

cleanup:
    if (rgba) free(rgba);
    if (file_buffer) free(file_buffer);
    if (fd >= 0) sceIoClose(fd);
}

void boxart_draw(u16 pos_x, u16 pos_y, u16 border_color)
{
    if (!boxart_loaded || !boxart_buffer)
        return;

    blit_to_screen(boxart_buffer, BOXART_W, BOXART_H, pos_x, pos_y);
    draw_box_line(pos_x - 1, pos_y - 1,
                  pos_x + BOXART_W, pos_y + BOXART_H,
                  border_color);
}

void boxart_free(void)
{
    if (boxart_buffer)
    {
        free(boxart_buffer);
        boxart_buffer = NULL;
    }
    boxart_loaded = 0;
}