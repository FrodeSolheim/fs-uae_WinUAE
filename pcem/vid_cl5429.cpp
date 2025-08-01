/*Cirrus Logic CL-GD5429 emulation*/
//SR7.0 = "true packed-pixel memory addressing"
#include <stdlib.h>
#include "ibm.h"
#include "cpu.h"
#include "device.h"
#include "io.h"
#include "mca.h"
#include "mem.h"
#include "pci.h"
#include "rom.h"
#include "video.h"
#include "vid_cl5429.h"
#include "vid_svga.h"
#include "vid_svga_render.h"
#include "vid_vga.h"
#include "vid_unk_ramdac.h"

enum
{
        CL_TYPE_AVGA2 = 0,
        CL_TYPE_GD5426,
        CL_TYPE_GD5428,
        CL_TYPE_GD5429,
        CL_TYPE_GD5430,
        CL_TYPE_GD5434,
        CL_TYPE_GD5436,
        CL_TYPE_GD5446,
        CL_TYPE_GD5446B
};

#define BLIT_DEPTH_8  0
#define BLIT_DEPTH_16 1
#define BLIT_DEPTH_24 2
#define BLIT_DEPTH_32 3

#define CL_GD5428_SYSTEM_BUS_MCA  5
#define CL_GD5428_SYSTEM_BUS_VESA 6
#define CL_GD5428_SYSTEM_BUS_ISA  7

#define CL_GD5429_SYSTEM_BUS_VESA 5
#define CL_GD5429_SYSTEM_BUS_ISA  7

#define CL_GD543X_SYSTEM_BUS_PCI  4
#define CL_GD543X_SYSTEM_BUS_VESA 6
#define CL_GD543X_SYSTEM_BUS_ISA  7

typedef struct gd5429_t
{
        mem_mapping_t mmio_mapping;
        mem_mapping_t linear_mapping;
        
        svga_t svga;
        
        rom_t bios_rom;
        
        uint32_t bank[2];
        uint32_t mask;
        
        uint32_t vram_mask;

        int type;
        
        struct
        {
                uint32_t bg_col, fg_col;
                uint16_t trans_col, trans_mask;
                uint16_t width, height;
                uint16_t dst_pitch, src_pitch;               
                uint32_t dst_addr, src_addr;
                uint8_t mask, mode, rop;
                uint8_t status, extensions;

                uint32_t dst_addr_backup, src_addr_backup;
                uint16_t width_backup, height_internal;
                int x_count, y_count;
                int depth;
                
                int mem_word_sel;
                uint16_t mem_word_save;
        } blt;

        struct
        {
                int mode;
                uint16_t stride;
                uint16_t r1sz;
                uint16_t r1adjust;
                uint16_t r2sz;
                uint16_t r2adjust;
                uint16_t r2sdz;
                uint16_t wvs;
                uint16_t wve;
                uint16_t hzoom;
                uint16_t vzoom;
                uint8_t occlusion;
                uint8_t colorkeycomparemask;
                uint8_t colorkeycompare;
                int region1size;
                int region2size;
                int colorkeymode;
                uint32_t ck;
        } overlay;

        uint8_t hidden_dac_reg;
        int hidden_dac_index;
        int dac_3c6_count;
        uint32_t hwcursor_pal[2];
        int hwcursor_extraoffset;
        
        uint8_t pci_regs[256];
        uint8_t int_line;
        int card;
        
        uint8_t pos_regs[8];
        svga_t *mb_vga;
        
        uint32_t lfb_base;
        uint32_t mmio_base;
        uint32_t gpio_base;

        int mmio_vram_overlap;
        
        uint8_t sr10_read, sr11_read;

        uint8_t latch_ext[4];

        int vblank_irq;
        int vportsync;
        
        int vidsys_ena;
} gd5429_t;

#define GRB_X8_ADDRESSING  (1 << 1)
#define GRB_WRITEMODE_EXT  (1 << 2)
#define GRB_8B_LATCHES     (1 << 3)
#define GRB_ENHANCED_16BIT (1 << 4)

static void gd5429_mmio_write(uint32_t addr, uint8_t val, void *p);
static void gd5429_mmio_writew(uint32_t addr, uint16_t val, void *p);
static void gd5429_mmio_writel(uint32_t addr, uint32_t val, void *p);
static uint8_t gd5429_mmio_read(uint32_t addr, void *p);
static uint16_t gd5429_mmio_readw(uint32_t addr, void *p);
static uint32_t gd5429_mmio_readl(uint32_t addr, void *p);

void gd5429_blt_write_w(uint32_t addr, uint16_t val, void *p);
void gd5429_blt_write_l(uint32_t addr, uint32_t val, void *p);

void gd5429_recalc_banking(gd5429_t *gd5429);
void gd5429_recalc_mapping(gd5429_t *gd5429);

uint8_t gd5429_read_linear(uint32_t addr, void *p);

static void ibm_gd5428_mapping_update(gd5429_t *gd5429);


static int gd5429_interrupt_enabled(gd5429_t* gd5429)
{
    return !PCI || (gd5429->svga.gdcreg[0x17] & 4);
}

static int gd5429_vga_vsync_enabled(gd5429_t *gd5429)
{
    if (!(gd5429->svga.crtc[0x11] & 0x20) && (gd5429->svga.crtc[0x11] & 0x10) && gd5429_interrupt_enabled(gd5429))
        return 1;
    return 0;
}

static void gd5429_update_irqs(gd5429_t *gd5429)
{
    if (gd5429->vblank_irq > 0 && gd5429_vga_vsync_enabled(gd5429))
        pci_set_irq(NULL, PCI_INTA, NULL);
    else
        pci_clear_irq(NULL, PCI_INTA, NULL);
}

static void gd5429_vblank_start(svga_t *svga)
{
    gd5429_t *gd5429 = (gd5429_t*)svga->priv;
    if (gd5429->vblank_irq >= 0) {
        gd5429->vblank_irq = 1;
        gd5429_update_irqs(gd5429);
    }
}

#define CLAMP(x) do                                     \
        {                                               \
                if ((x) & ~0xff)                        \
                        x = ((x) < 0) ? 0 : 0xff;       \
        }                               \
        while (0)

#define DECODE_YCbCr()                                                  \
        do                                                              \
        {                                                               \
                int c;                                                  \
                                                                        \
                for (c = 0; c < 2; c++)                                 \
                {                                                       \
                        uint8_t y1, y2;                                 \
                        int8_t Cr, Cb;                                  \
                        int dR, dG, dB;                                 \
                                                                        \
                        y1 = src[0];                                    \
                        Cr = src[1] - 0x80;                             \
                        y2 = src[2];                                    \
                        Cb = src[3] - 0x80;                             \
                        src += 4;                                       \
                                                                        \
                        dR = (359*Cr) >> 8;                             \
                        dG = (88*Cb + 183*Cr) >> 8;                     \
                        dB = (453*Cb) >> 8;                             \
                                                                        \
                        r[x_write] = y1 + dR;                           \
                        CLAMP(r[x_write]);                              \
                        g[x_write] = y1 - dG;                           \
                        CLAMP(g[x_write]);                              \
                        b[x_write] = y1 + dB;                           \
                        CLAMP(b[x_write]);                              \
                                                                        \
                        r[x_write+1] = y2 + dR;                         \
                        CLAMP(r[x_write+1]);                            \
                        g[x_write+1] = y2 - dG;                         \
                        CLAMP(g[x_write+1]);                            \
                        b[x_write+1] = y2 + dB;                         \
                        CLAMP(b[x_write+1]);                            \
                                                                        \
                        x_write = (x_write + 2) & 7;                    \
                }                                                       \
        } while (0)

/*Both YUV formats are untested*/
#define DECODE_YUV211()                                         \
        do                                                      \
        {                                                       \
                uint8_t y1, y2, y3, y4;                         \
                int8_t U, V;                                    \
                int dR, dG, dB;                                 \
                                                                \
                U = src[0] - 0x80;                              \
                y1 = (298 * (src[1] - 16)) >> 8;                \
                y2 = (298 * (src[2] - 16)) >> 8;                \
                V = src[3] - 0x80;                              \
                y3 = (298 * (src[4] - 16)) >> 8;                \
                y4 = (298 * (src[5] - 16)) >> 8;                \
                src += 6;                                       \
                                                                \
                dR = (309*V) >> 8;                              \
                dG = (100*U + 208*V) >> 8;                      \
                dB = (516*U) >> 8;                              \
                                                                \
                r[x_write] = y1 + dR;                           \
                CLAMP(r[x_write]);                              \
                g[x_write] = y1 - dG;                           \
                CLAMP(g[x_write]);                              \
                b[x_write] = y1 + dB;                           \
                CLAMP(b[x_write]);                              \
                                                                \
                r[x_write+1] = y2 + dR;                         \
                CLAMP(r[x_write+1]);                            \
                g[x_write+1] = y2 - dG;                         \
                CLAMP(g[x_write+1]);                            \
                b[x_write+1] = y2 + dB;                         \
                CLAMP(b[x_write+1]);                            \
                                                                \
                r[x_write+2] = y3 + dR;                         \
                CLAMP(r[x_write+2]);                            \
                g[x_write+2] = y3 - dG;                         \
                CLAMP(g[x_write+2]);                            \
                b[x_write+2] = y3 + dB;                         \
                CLAMP(b[x_write+2]);                            \
                                                                \
                r[x_write+3] = y4 + dR;                         \
                CLAMP(r[x_write+3]);                            \
                g[x_write+3] = y4 - dG;                         \
                CLAMP(g[x_write+3]);                            \
                b[x_write+3] = y4 + dB;                         \
                CLAMP(b[x_write+3]);                            \
                                                                \
                x_write = (x_write + 4) & 7;                    \
        } while (0)

#define DECODE_YUV422()                                                 \
        do                                                              \
        {                                                               \
                int c;                                                  \
                                                                        \
                for (c = 0; c < 2; c++)                                 \
                {                                                       \
                        uint8_t y1, y2;                                 \
                        int8_t U, V;                                    \
                        int dR, dG, dB;                                 \
                                                                        \
                        U = src[0] - 0x80;                              \
                        y1 = (298 * (src[1] - 16)) >> 8;                \
                        V = src[2] - 0x80;                              \
                        y2 = (298 * (src[3] - 16)) >> 8;                \
                        src += 4;                                       \
                                                                        \
                        dR = (309*V) >> 8;                              \
                        dG = (100*U + 208*V) >> 8;                      \
                        dB = (516*U) >> 8;                              \
                                                                        \
                        r[x_write] = y1 + dR;                           \
                        CLAMP(r[x_write]);                              \
                        g[x_write] = y1 - dG;                           \
                        CLAMP(g[x_write]);                              \
                        b[x_write] = y1 + dB;                           \
                        CLAMP(b[x_write]);                              \
                                                                        \
                        r[x_write+1] = y2 + dR;                         \
                        CLAMP(r[x_write+1]);                            \
                        g[x_write+1] = y2 - dG;                         \
                        CLAMP(g[x_write+1]);                            \
                        b[x_write+1] = y2 + dB;                         \
                        CLAMP(b[x_write+1]);                            \
                                                                        \
                        x_write = (x_write + 2) & 7;                    \
                }                                                       \
        } while (0)

#define DECODE_RGB555()                                                 \
        do                                                              \
        {                                                               \
                int c;                                                  \
                                                                        \
                for (c = 0; c < 4; c++)                                 \
                {                                                       \
                        uint16_t dat;                                   \
                                                                        \
                        dat = *(uint16_t *)src;                         \
                        src += 2;                                       \
                                                                        \
                        r[x_write + c] = ((dat & 0x001f) << 3) | ((dat & 0x001f) >> 2); \
                        g[x_write + c] = ((dat & 0x03e0) >> 2) | ((dat & 0x03e0) >> 7); \
                        b[x_write + c] = ((dat & 0x7c00) >> 7) | ((dat & 0x7c00) >> 12); \
                }                                                       \
                x_write = (x_write + 4) & 7;                            \
        } while (0)

#define DECODE_RGB565()                                                 \
        do                                                              \
        {                                                               \
                int c;                                                  \
                                                                        \
                for (c = 0; c < 4; c++)                                 \
                {                                                       \
                        uint16_t dat;                                   \
                                                                        \
                        dat = *(uint16_t *)src;                         \
                        src += 2;                                       \
                                                                        \
                        r[x_write + c] = ((dat & 0x001f) << 3) | ((dat & 0x001f) >> 2); \
                        g[x_write + c] = ((dat & 0x07e0) >> 3) | ((dat & 0x07e0) >> 9); \
                        b[x_write + c] = ((dat & 0xf800) >> 8) | ((dat & 0xf800) >> 13); \
                }                                                       \
                x_write = (x_write + 4) & 7;                            \
        } while (0)

#define DECODE_CLUT()                                                   \
        do                                                              \
        {                                                               \
                int c;                                                  \
                                                                        \
                for (c = 0; c < 4; c++)                                 \
                {                                                       \
                        uint8_t dat;                                    \
                                                                        \
                        dat = *(uint8_t *)src;                          \
                        src++;                                          \
                                                                        \
                        r[x_write + c] = svga->pallook[dat] >>  0;      \
                        g[x_write + c] = svga->pallook[dat] >>  8;      \
                        b[x_write + c] = svga->pallook[dat] >> 16;      \
                }                                                       \
                x_write = (x_write + 4) & 7;                            \
        } while (0)



#define OVERLAY_SAMPLE()                        \
        do                                      \
        {                                       \
                switch (gd5429->overlay.mode)   \
                {                               \
                        case 0:                 \
                        DECODE_YUV422();        \
                        break;                  \
                        case 2:                 \
                        DECODE_CLUT();          \
                        break;                  \
                        case 3:                 \
                        DECODE_YUV211();        \
                        break;                  \
                        case 4:                 \
                        DECODE_RGB555();        \
                        break;                  \
                        case 5:                 \
                        DECODE_RGB565();        \
                        break;                  \
                }                               \
        } while (0)


// 5446 overlay
static void gd5429_overlay_draw(svga_t *svga, int displine)
{
    gd5429_t *gd5429 = (gd5429_t*)svga->priv;
    int shift = gd5429->type >= CL_TYPE_GD5446 ? 2 : 0;
    int h_acc = svga->overlay_latch.h_acc;
    int r[8], g[8], b[8];
    int x_read = 4, x_write = 4;
    int x;
    uint32_t *p;
    uint8_t *src = &svga->vram[(svga->overlay_latch.addr << shift) & svga->vram_mask];
    int bpp = svga->bpp;
    int bytesperpix = (bpp + 7) / 8;
    uint8_t *src2 = &svga->vram[(svga->ma - (svga->hdisp * bytesperpix))  & svga->vram_display_mask];
    int w = gd5429->overlay.r2sdz;

    if (gd5429->overlay.mode == 2) {
        w *= 4;
    } else {
        w *= 2;
    }

    p = &((uint32_t *)buffer32->line[displine])[gd5429->overlay.region1size + 32];
    src2 += gd5429->overlay.region1size * bytesperpix;

    OVERLAY_SAMPLE();

    for (x = 0; x < gd5429->overlay.region2size && x + gd5429->overlay.region1size < svga->video_res_x; x++)
    {
        if (gd5429->overlay.occlusion) {
            int occl = 1;
            int ckval = gd5429->overlay.ck;
            if (bytesperpix == 1) {
                if (*src2 == ckval) {
                    occl = 0;
                }
            } else if (bytesperpix == 2) {
                if (*((uint16_t*)src2) == ckval) {
                    occl = 0;
                }
            } else {
                occl = 0;
            }
            if (!occl) {
                *p++ = r[x_read] | (g[x_read] << 8) | (b[x_read] << 16);
            }
            src2 += bytesperpix;
        } else {
            *p++ = r[x_read] | (g[x_read] << 8) | (b[x_read] << 16);
        }

        h_acc += gd5429->overlay.hzoom;
        if (h_acc >= 256)
        {
            if ((x_read ^ (x_read + 1)) & ~3)
                OVERLAY_SAMPLE();
            x_read = (x_read + 1) & 7;

            h_acc -= 256;
        }
    }

    svga->overlay_latch.v_acc += gd5429->overlay.vzoom;
    if (svga->overlay_latch.v_acc >= 256)
    {
        svga->overlay_latch.v_acc -= 256;
        svga->overlay_latch.addr += svga->overlay.pitch << 1;
    }
}

static void gd5429_update_overlay(gd5429_t *gd5429)
{
    svga_t *svga = &gd5429->svga;
    int bpp = svga->bpp;

    svga->overlay.cur_ysize = gd5429->overlay.wve - gd5429->overlay.wvs + 1;
    gd5429->overlay.region1size = 32 * gd5429->overlay.r1sz / bpp + (gd5429->overlay.r1adjust * 8 / bpp);
    gd5429->overlay.region2size = 32 * gd5429->overlay.r2sz / bpp + (gd5429->overlay.r2adjust * 8 / bpp);

    gd5429->overlay.occlusion = (svga->crtc[0x3e] & 0x80) != 0 && svga->bpp <= 16;

    // mask and chroma key ignored.
    if (gd5429->overlay.colorkeymode == 0) {
        gd5429->overlay.ck = gd5429->overlay.colorkeycompare;
    } else if (gd5429->overlay.colorkeymode == 1) {
        gd5429->overlay.ck = gd5429->overlay.colorkeycompare | (gd5429->overlay.colorkeycomparemask << 8);
    } else {
        gd5429->overlay.occlusion = 0;
    }
}

void gd5429_out(uint16_t addr, uint8_t val, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        svga_t *svga = &gd5429->svga;
        uint8_t old;

        int hidden_dac_index = -1;
        uint32_t pal_temp0;
        PALETTE pal_temp1;
        
        if (((addr & 0xfff0) == 0x3d0 || (addr & 0xfff0) == 0x3b0) && !(svga->miscout & 1))
                addr ^= 0x60;

//        pclog("gd5429 out %04X %02X\n", addr, val);
                
        switch (addr)
        {
                case 0x3c3:
                if (MCA)
                {
                        gd5429->vidsys_ena = val & 1;
                        ibm_gd5428_mapping_update(gd5429);
                }
                break;
                        
                case 0x3c4:
                svga->seqaddr = val & 0x1f;
                break;
                case 0x3c5:
                if (svga->seqaddr > 5)
                {
                        svga->seqregs[svga->seqaddr & 0x1f] = val;
                        switch (svga->seqaddr & 0x1f)
                        {
                                case 0x10:
                                svga->hwcursor.x = (val << 3) | ((svga->seqaddr >> 5) & 7);
                                gd5429->sr10_read = svga->seqaddr & 0xe0;
//                                pclog("svga->hwcursor.x = %i\n", svga->hwcursor.x);
                                break;
                                case 0x11:
                                svga->hwcursor.y = (val << 3) | ((svga->seqaddr >> 5) & 7);
                                gd5429->sr11_read = svga->seqaddr & 0xe0;
//                                pclog("svga->hwcursor.y = %i\n", svga->hwcursor.y);
                                break;
                                case 0x12:
                                svga->hwcursor.ena = val & 1;
                                svga->hwcursor.cur_ysize = (val & 4) ? 64 : 32;
                                svga->hwcursor.yoff = 0;
                                if (svga->hwcursor.cur_ysize == 64)
                                        svga->hwcursor.addr = (0x3fc000 + ((svga->seqregs[0x13] & 0x3c) * 256)) & svga->vram_mask;
                                else
                                        svga->hwcursor.addr = (0x3fc000 + ((svga->seqregs[0x13] & 0x3f) * 256)) & svga->vram_mask;
//                                pclog("svga->hwcursor.ena = %i\n", svga->hwcursor.ena);
                                break;                               
                                case 0x13:
                                if (svga->hwcursor.cur_ysize == 64)
                                        svga->hwcursor.addr = (0x3fc000 + ((val & 0x3c) * 256)) & svga->vram_mask;
                                else
                                        svga->hwcursor.addr = (0x3fc000 + ((val & 0x3f) * 256)) & svga->vram_mask;
//                                pclog("svga->hwcursor.addr = %x\n", svga->hwcursor.addr);
                                break;                                
                                
                                case 0x07:
                                    if (gd5429->type >= CL_TYPE_GD5429) {
                                        svga->set_reset_disabled = svga->seqregs[7] & 1;
                                    }
                                    svga->packed_chain4 = svga->seqregs[7] & 1;
                                    svga_recalctimings(svga);
                                    break;
                                case 0x0f:
                                case 0x17:
                                //UAE
                                //if (gd5429->type >= CL_TYPE_GD5429)
                                        gd5429_recalc_mapping(gd5429);
                                break;
                        }
                        return;
                }
                break;

                case 0x3c6:
//                pclog("CL write 3c6 %02x %i\n", val, gd5429->dac_3c6_count);
                if (gd5429->dac_3c6_count == 4)
                {
                        gd5429->dac_3c6_count = 0;
                        gd5429->hidden_dac_reg = val;
                        svga_recalctimings(svga);
                        return;
                }
                gd5429->dac_3c6_count = 0;
                break;
                case 0x3c7: case 0x3c8:
                gd5429->dac_3c6_count = 0;
                break;
                case 0x3c9:
                gd5429->dac_3c6_count = 0;
                // Hidden CLUT entries 256 - 258
                if (svga->dac_pos == 2 && (svga->seqregs[0x12] & 2)) {
                    hidden_dac_index = svga->dac_addr;
                    pal_temp0 = svga->pallook[hidden_dac_index];
                    memcpy(&pal_temp1, &svga->vgapal[hidden_dac_index], sizeof(PALETTE));
                }
                break;

                case 0x3cf:
//                pclog("Write GDC %02x %02x\n", svga->gdcaddr, val);
                if (svga->gdcaddr == 0)
                        gd5429_mmio_write(0xb8000, val, gd5429);
                if (svga->gdcaddr == 1)
                        gd5429_mmio_write(0xb8004, val, gd5429);
                if (svga->gdcaddr == 5)
                {
                        svga->gdcreg[5] = val;
                        if (svga->gdcreg[0xb] & 0x04)
                                svga->writemode = svga->gdcreg[5] & 7;
                        else
                                svga->writemode = svga->gdcreg[5] & 3;
                        svga->readmode = val & 8;
                        svga->chain2_read = val & 0x10;
//                        pclog("writemode = %i\n", svga->writemode);
                        return;
                }
                if (svga->gdcaddr == 6)
                {
                        if ((svga->gdcreg[6] & 0xc) != (val & 0xc))
                        {
                                svga->gdcreg[6] = val;
                                gd5429_recalc_mapping(gd5429);
                        }

                        /*Hack - the Windows 3.x drivers for the GD5426/8 require VRAM wraparound
                          for pattern & cursor writes to work correctly, but the BIOSes require
                          no wrapping to detect memory size - albeit with odd/even mode enabled.
                          This may be a quirk of address mapping. So change wrapping mode based on
                          odd/even mode for now*/
#if 0
                        if (gd5429->type == CL_TYPE_GD5426 || gd5429->type == CL_TYPE_GD5428)
                        {
                                if (val & 2) /*Odd/Even*/
                                        svga->decode_mask = 0x1fffff;
                                else
                                        svga->decode_mask = svga->vram_mask;
                        }
#endif
                        svga->gdcreg[6] = val;
                        return;
                }
                if (svga->gdcaddr > 8)
                {
                        svga->gdcreg[svga->gdcaddr & 0x3f] = val;
                        if (gd5429->type < CL_TYPE_GD5426 && (svga->gdcaddr > 0xb))
                                return;
                        switch (svga->gdcaddr)
                        {
                                case 0x09: case 0x0a: case 0x0b:
                                gd5429_recalc_banking(gd5429);
                                if (svga->gdcreg[0xb] & 0x04)
                                        svga->writemode = svga->gdcreg[5] & 7;
                                else
                                        svga->writemode = svga->gdcreg[5] & 3;
                                break;

                                case 0x0c:
                                gd5429->overlay.colorkeycompare = val;
                                gd5429_update_overlay(gd5429);
                                break;
                                case 0x0d:
                                gd5429->overlay.colorkeycomparemask = val;
                                gd5429_update_overlay(gd5429);
                                break;
                                case 0x0e:
                                if (gd5429->type >= CL_TYPE_GD5426) {
                                    svga->dpms = (val & 0x06) && ((svga->miscout & ((val & 0x06) << 5)) != 0xc0);
                                    svga_recalctimings(svga);
                                }
                                break;
        
                                case 0x10:
                                gd5429_mmio_write(0xb8001, val, gd5429);
                                break;
                                case 0x11:
                                gd5429_mmio_write(0xb8005, val, gd5429);
                                break;
                                case 0x12:
                                gd5429_mmio_write(0xb8002, val, gd5429);
                                break;
                                case 0x13:
                                gd5429_mmio_write(0xb8006, val, gd5429);
                                break;
                                case 0x14:
                                gd5429_mmio_write(0xb8003, val, gd5429);
                                break;
                                case 0x15:
                                gd5429_mmio_write(0xb8007, val, gd5429);
                                break;

                                case 0x20:
                                gd5429_mmio_write(0xb8008, val, gd5429);
                                break;
                                case 0x21:
                                gd5429_mmio_write(0xb8009, val, gd5429);
                                break;
                                case 0x22:
                                gd5429_mmio_write(0xb800a, val, gd5429);
                                break;
                                case 0x23:
                                gd5429_mmio_write(0xb800b, val, gd5429);
                                break;
                                case 0x24:
                                gd5429_mmio_write(0xb800c, val, gd5429);
                                break;
                                case 0x25:
                                gd5429_mmio_write(0xb800d, val, gd5429);
                                break;
                                case 0x26:
                                gd5429_mmio_write(0xb800e, val, gd5429);
                                break;
                                case 0x27:
                                gd5429_mmio_write(0xb800f, val, gd5429);
                                break;
                
                                case 0x28:
                                gd5429_mmio_write(0xb8010, val, gd5429);
                                break;
                                case 0x29:
                                gd5429_mmio_write(0xb8011, val, gd5429);
                                break;
                                case 0x2a:
                                gd5429_mmio_write(0xb8012, val, gd5429);
                                break;

                                case 0x2c:
                                gd5429_mmio_write(0xb8014, val, gd5429);
                                break;
                                case 0x2d:
                                gd5429_mmio_write(0xb8015, val, gd5429);
                                break;
                                case 0x2e:
                                gd5429_mmio_write(0xb8016, val, gd5429);
                                break;

                                case 0x2f:
                                gd5429_mmio_write(0xb8017, val, gd5429);
                                break;
                                case 0x30:
                                gd5429_mmio_write(0xb8018, val, gd5429);
                                break;
                
                                case 0x31:
                                gd5429_mmio_write(0xb8040, val, gd5429);
                                break;

                                case 0x32:
                                gd5429_mmio_write(0xb801a, val, gd5429);
                                break;
                
                                case 0x33:
                                gd5429_mmio_write(0xb801b, val, gd5429);
                                break;

                                case 0x34:
                                if (gd5429->type <= CL_TYPE_GD5428)
                                        gd5429->blt.trans_col = (gd5429->blt.trans_col & 0xff00) | val;
                                break;
                                case 0x35:
                                if (gd5429->type <= CL_TYPE_GD5428)
                                        gd5429->blt.trans_col = (gd5429->blt.trans_col & 0x00ff) | (val << 8);
                                break;
                                case 0x36:
                                if (gd5429->type <= CL_TYPE_GD5428)
                                        gd5429->blt.trans_mask = (gd5429->blt.trans_mask & 0xff00) | val;
                                break;
                                case 0x37:
                                if (gd5429->type <= CL_TYPE_GD5428)
                                        gd5429->blt.trans_mask = (gd5429->blt.trans_mask & 0x00ff) | (val << 8);
                                break;
                        }
                        return;
                }
                break;
                
                case 0x3D4:
                svga->crtcreg = val & svga->crtcreg_mask;
                return;
                case 0x3D5:
                if ((svga->crtcreg < 7) && (svga->crtc[0x11] & 0x80))
                        return;
                if ((svga->crtcreg == 7) && (svga->crtc[0x11] & 0x80))
                        val = (svga->crtc[7] & ~0x10) | (val & 0x10);
                old = svga->crtc[svga->crtcreg];
                svga->crtc[svga->crtcreg] = val;

                if (svga->crtcreg == 0x11) {
                    if (!(val & 0x10)) {
                        if (gd5429->vblank_irq > 0)
                            gd5429->vblank_irq = -1;
                    } else if (gd5429->vblank_irq < 0) {
                        gd5429->vblank_irq = 0;
                    }
                    gd5429_update_irqs(gd5429);
                    if ((val & ~0x30) == (old & ~0x30))
                        old = val;
                }

                if (1)
                {
                        // overlay registers
                        switch (svga->crtcreg)
                        {
                            case 0x1d:
                                if (((old >> 3) & 7) != ((val >> 3) & 7)) {
                                        gd5429->overlay.colorkeymode = (val >> 3) & 7;
                                        gd5429_update_overlay(gd5429);
                                }
                                break;
                            case 0x31:
                                gd5429->overlay.hzoom = val == 0 ? 256 : val;
                                gd5429_update_overlay(gd5429);
                                break;
                            case 0x32:
                                gd5429->overlay.vzoom = val == 0 ? 256 : val;
                                gd5429_update_overlay(gd5429);
                                break;
                            case 0x33:
                                gd5429->overlay.r1sz &= ~0xff;
                                gd5429->overlay.r1sz |= val;
                                gd5429_update_overlay(gd5429);
                                break;
                            case 0x34:
                                gd5429->overlay.r2sz &= ~0xff;
                                gd5429->overlay.r2sz |= val;
                                gd5429_update_overlay(gd5429);
                                break;
                            case 0x35:
                                gd5429->overlay.r2sdz &= ~0xff;
                                gd5429->overlay.r2sdz |= val;
                                gd5429_update_overlay(gd5429);
                                break;
                            case 0x36:
                                gd5429->overlay.r1sz &= 0xff;
                                gd5429->overlay.r1sz |= (val << 8) & 0x300;
                                gd5429->overlay.r2sz &= 0xff;
                                gd5429->overlay.r2sz |= (val << 6) & 0x300;
                                gd5429->overlay.r2sdz &= 0xff;
                                gd5429->overlay.r2sdz |= (val << 4) & 0x300;
                                gd5429_update_overlay(gd5429);
                                break;
                            case 0x37:
                                gd5429->overlay.wvs &= ~0xff;
                                gd5429->overlay.wvs |= val;
                                svga->overlay.y = gd5429->overlay.wvs;
                                break;
                            case 0x38:
                                gd5429->overlay.wve &= ~0xff;
                                gd5429->overlay.wve |= val;
                                gd5429_update_overlay(gd5429);
                                break;
                            case 0x39:
                                gd5429->overlay.wvs &= 0xff;
                                gd5429->overlay.wvs |= (val << 8) & 0x300;
                                gd5429->overlay.wve &= 0xff;
                                gd5429->overlay.wve |= (val << 6) & 0x300;
                                gd5429_update_overlay(gd5429);
                                break;
                            case 0x3a:
                                svga->overlay.addr &= ~0xff;
                                svga->overlay.addr |= val;
                                gd5429_update_overlay(gd5429);
                                break;
                            case 0x3b:
                                svga->overlay.addr &= ~0xff00;
                                svga->overlay.addr |= val << 8;
                                gd5429_update_overlay(gd5429);
                                break;
                            case 0x3c:
                                svga->overlay.addr &= ~0x0f0000;
                                svga->overlay.addr |= (val << 16) & 0x0f0000;
                                svga->overlay.pitch &= ~0x100;
                                svga->overlay.pitch |= (val & 0x20) << 3;
                                gd5429_update_overlay(gd5429);
                                break;
                            case 0x3d:
                                svga->overlay.pitch &= ~0xff;
                                svga->overlay.pitch |= val;
                                gd5429_update_overlay(gd5429);
                                break;
                            case 0x3e:
                                gd5429->overlay.mode = (val >> 1) & 7;
                                svga->overlay.ena = (val & 1) != 0;
                                gd5429_update_overlay(gd5429);
                                break;
                        }

                        if ((svga->crtcreg < 0xe || svga->crtcreg > 0x10) && old != val)
                        {
                                svga->fullchange = changeframecount;
                                svga_recalctimings(svga);
                        }
                }
                break;
        }
        svga_out(addr, val, svga);

        if (hidden_dac_index >= 0) {
            if ((hidden_dac_index & 15) == 0) {
                gd5429->hwcursor_pal[0] = svga->pallook[hidden_dac_index];
            } else if ((hidden_dac_index & 15) == 15) {
                gd5429->hwcursor_pal[1] = svga->pallook[hidden_dac_index];
            }
            // restore overwritten palette entry
            svga->pallook[hidden_dac_index] = pal_temp0;
            memcpy(&svga->vgapal[hidden_dac_index], &pal_temp1, sizeof(PALETTE));
        }
}

uint8_t gd5429_in(uint16_t addr, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        svga_t *svga = &gd5429->svga;
        uint8_t ret;

        if (((addr & 0xfff0) == 0x3d0 || (addr & 0xfff0) == 0x3b0) && !(svga->miscout & 1))
                addr ^= 0x60;
        
//        if (addr != 0x3da) pclog("IN gd5429 %04X\n", addr);
        
        switch (addr)
        {
                case 0x3c2:
                    ret = svga_in(addr, svga);
                    ret |= gd5429->vblank_irq > 0 ? 0x80 : 0x00;
                    return ret;

                case 0x3c3:
                if (MCA)
                        return gd5429->vidsys_ena;
                break;

                case 0x3c4:
                if ((svga->seqaddr & 0x1f) == 0x10)
                        return (svga->seqaddr & 0x1f) | gd5429->sr10_read;
                if ((svga->seqaddr & 0x1f) == 0x11)
                        return (svga->seqaddr & 0x1f) | gd5429->sr11_read;
                return svga->seqaddr & 0x1f;
                
                case 0x3c5:
                if (svga->seqaddr > 5)
                {
                        uint8_t temp;
                        
                        switch (svga->seqaddr)
                        {
                                case 6:
                                return ((svga->seqregs[6] & 0x17) == 0x12) ? 0x12 : 0x0f;

                                case 0x17:
                                if (gd5429->type < CL_TYPE_GD5426)
                                        break;
                                temp = svga->seqregs[0x17];
                                temp &= ~(7 << 3);
                                if (gd5429->type == CL_TYPE_GD5426 || gd5429->type == CL_TYPE_GD5428)
                                {
                                        if (MCA)
                                                temp |= (CL_GD5428_SYSTEM_BUS_MCA << 3);
                                        else if (has_vlb)
                                                temp |= (CL_GD5428_SYSTEM_BUS_VESA << 3);
                                        else
                                                temp |= (CL_GD5428_SYSTEM_BUS_ISA << 3);
                                }
                                else if (gd5429->type == CL_TYPE_GD5429)
                                {
                                        if (has_vlb)
                                                temp |= (CL_GD5429_SYSTEM_BUS_VESA << 3);
                                        else
                                                temp |= (CL_GD5429_SYSTEM_BUS_ISA << 3);
                                }
                                else
                                {
                                        if (PCI)
                                                temp |= (CL_GD543X_SYSTEM_BUS_PCI << 3);
                                        else if (has_vlb)
                                                temp |= (CL_GD543X_SYSTEM_BUS_VESA << 3);
                                        else
                                                temp |= (CL_GD543X_SYSTEM_BUS_ISA << 3);
                                }
                                return temp;                                
                        }
                        return svga->seqregs[svga->seqaddr & 0x3f];
                }
                break;
                
                case 0x3c6:
//                pclog("CL read 3c6 %i\n", gd5429->dac_3c6_count);
                if (gd5429->dac_3c6_count == 4)
                {
                        gd5429->dac_3c6_count = 0;
                        return gd5429->hidden_dac_reg;
                }
                gd5429->dac_3c6_count++;
                break;
                case 0x3c7: case 0x3c8: case 0x3c9:
                gd5429->dac_3c6_count = 0;
                break;

                case 0x3cf:
                if (svga->gdcaddr > 8)
                {
                    uint8_t addr = svga->gdcaddr & 0x3f;
                    if (addr == 0x31)
                        return gd5429->blt.status;
                    else
                        return svga->gdcreg[addr];
                }
                break;

                case 0x3D4:
                return svga->crtcreg;
                case 0x3D5:
                switch (svga->crtcreg)
                {
                        case 0x27: /*ID*/
                        switch (gd5429->type)
                        {
                                case CL_TYPE_AVGA2:
                                return 0x18; /*AVGA2*/
                                case CL_TYPE_GD5426:
                                return 0x90; /*GD5426*/
                                case CL_TYPE_GD5428:
                                return 0x98; /*GD5428*/
                                case CL_TYPE_GD5429:
                                return 0x9c; /*GD5429*/
                                case CL_TYPE_GD5430:
                                return 0xa0; /*GD5430*/
                                case CL_TYPE_GD5434:
                                return 0xa8; /*GD5434*/
                                case CL_TYPE_GD5446:
                                case CL_TYPE_GD5446B:
                                return 0xb8; /*GD5446*/
                        }
                        break;
                        case 0x28: /*Class ID*/
                        if (gd5429->type == CL_TYPE_GD5430)
                                return 0xff; /*Standard CL-GD5430*/
                        break;
                case 0x3f:
                    // Fake video port vsync toggle
                    if (gd5429->type >= CL_TYPE_GD5446) {
                        gd5429->vportsync = !gd5429->vportsync;
                    }
                    return gd5429->vportsync ? 0x80 : 0x00;
                }
                return svga->crtc[svga->crtcreg];
        }
        return svga_in(addr, svga);
}

void gd5429_recalc_banking(gd5429_t *gd5429)
{
        svga_t *svga = &gd5429->svga;
        
        if (svga->gdcreg[0xb] & 0x20)
                gd5429->bank[0] = (svga->gdcreg[0x09] & 0xff) << 14;
        else
                gd5429->bank[0] = svga->gdcreg[0x09] << 12;
                                
        if (svga->gdcreg[0xb] & 0x01)
        {
                if (svga->gdcreg[0xb] & 0x20)
                        gd5429->bank[1] = (svga->gdcreg[0x0a] & 0xff) << 14;
                else
                        gd5429->bank[1] = svga->gdcreg[0x0a] << 12;
        }
        else
                gd5429->bank[1] = gd5429->bank[0] + 0x8000;

        if (svga->seqregs[7] & 0xf0) {
            gd5429_recalc_mapping(gd5429);
        }
}

void gd5429_recalc_mapping(gd5429_t *gd5429)
{
        svga_t *svga = &gd5429->svga;
        
        if ((PCI && gd5429->type >= CL_TYPE_GD5430 && !(gd5429->pci_regs[PCI_REG_COMMAND] & PCI_COMMAND_MEM)) ||
                (MCA && (!(gd5429->pos_regs[2] & 0x01) || !gd5429->vidsys_ena)))
        {
                mem_mapping_disablex(&svga->mapping);
                mem_mapping_disablex(&gd5429->linear_mapping);
                mem_mapping_disablex(&gd5429->mmio_mapping);
                return;
        }
        
        gd5429->mmio_vram_overlap = 0;
        
//        pclog("Write mapping %02X %i\n", svga->gdcreg[6], svga->seqregs[0x17] & 0x04);
        if (!(svga->seqregs[7] & 0xf0))
        {
                mem_mapping_disablex(&gd5429->linear_mapping);
                switch (svga->gdcreg[6] & 0x0C)
                {
                        case 0x0: /*128k at A0000*/
                        mem_mapping_set_addrx(&svga->mapping, 0xa0000, 0x10000);                        
                        svga->banked_mask = 0xffff;
                        break;
                        case 0x4: /*64k at A0000*/
                        mem_mapping_set_addrx(&svga->mapping, 0xa0000, 0x10000);
                        svga->banked_mask = 0xffff;
                        break;
                        case 0x8: /*32k at B0000*/
                        mem_mapping_set_addrx(&svga->mapping, 0xb0000, 0x08000);
                        svga->banked_mask = 0x7fff;
                        break;
                        case 0xC: /*32k at B8000*/
                        mem_mapping_set_addrx(&svga->mapping, 0xb8000, 0x08000);
                        svga->banked_mask = 0x7fff;
                        gd5429->mmio_vram_overlap = 1;
                        break;
                }
                if (gd5429->type >= CL_TYPE_GD5429 && svga->seqregs[0x17] & 0x04)
                        mem_mapping_set_addrx(&gd5429->mmio_mapping, 0xb8000, 0x00100);
                else
                        mem_mapping_disablex(&gd5429->mmio_mapping);
        }
        else
        {
                uint32_t base, size, offset, max;
                
                if (svga->gdcreg[0xb] & 0x20)
                    offset = (svga->gdcreg[0x09] & 0xff) << 14;
                else
                    offset = svga->gdcreg[0x09] << 12;
                if (gd5429->type <= CL_TYPE_GD5429 || (!PCI && !has_vlb))
                {
                        base = (svga->seqregs[7] & 0xf0) << 16;
                        if (svga->gdcreg[0xb] & 0x20)
                                size = 1 * 1024 * 1024;
                        else
                                size = 2 * 1024 * 1024;
                        max = 2 * 1024 * 1024;
                }
                else if (PCI)
                {
                        base = gd5429->lfb_base;
                        size = 4 * 1024 * 1024;
                        max = 4 * 1024 * 1024;
                }
                else /*VLB*/
                {
                        base = 128*1024*1024;
                        size = 4 * 1024 * 1024;
                        max = 4 * 1024 * 1024;
                }
                if (svga->seqregs[15] & 0x80) {
                    svga->decode_mask = max - 1;
                } else {
                    svga->decode_mask = max / 2 - 1;
                }
                base += offset;
                mem_mapping_disablex(&svga->mapping);
                mem_mapping_set_addrx(&gd5429->linear_mapping, base, size);
                if (gd5429->type >= CL_TYPE_GD5429 && svga->seqregs[0x17] & 0x04)
                        mem_mapping_set_addrx(&gd5429->mmio_mapping, 0xb8000, 0x00100);
                else
                        mem_mapping_disablex(&gd5429->mmio_mapping);
        }        
}
        
void gd5429_recalctimings(svga_t *svga)
{
        gd5429_t *gd5429 = (gd5429_t *)svga->priv;
        int clock = (svga->miscout >> 2) & 3;
        int n, d, p;
        double vclk;

        if (svga->crtc[0x1b] & 0x10)
                svga->rowoffset |= 0x100;
                        
        if (!svga->rowoffset)
                svga->rowoffset = 0x100;
        
        svga->interlace = svga->crtc[0x1a] & 1;
        
        svga->horizontal_linedbl = svga->dispend * 9 / 10 >= svga->hdisp;

        if (svga->seqregs[7] & 0x01) {
            svga->render = svga_render_8bpp_highres;
        }


        svga->ma_latch |= ((svga->crtc[0x1b] & 0x01) << 16) | (((svga->crtc[0x1b] >> 2) & 3) << 17);
        if (gd5429->type >= CL_TYPE_GD5436) {
            svga->ma_latch |= (((svga->crtc[0x1d] >> 7) & 1) << 19);
        }
//      pclog("MA now %05X %02X\n", svga->ma_latch, svga->crtc[0x1b]);
        
        svga->bpp = 8;
        if (gd5429->hidden_dac_reg & 0x80)
        {
                if (gd5429->hidden_dac_reg & 0x40)
                {
                        switch (gd5429->hidden_dac_reg & 0xf)
                        {
                                case 0x0:
                                svga->render = svga_render_15bpp_highres;
                                svga->bpp = 15;
                                break;
                                case 0x1:
                                svga->render = svga_render_16bpp_highres;
                                svga->bpp = 16;
                                break;
                                case 0x5:
                                if (gd5429->type >= CL_TYPE_GD5434 && (svga->seqregs[7] & 8))
                                {
                                        svga->render = svga_render_32bpp_highres;
                                        svga->bpp = 32;
                                        svga->rowoffset *= 2;
                                }
                                else
                                {
                                        svga->render = svga_render_24bpp_highres;
                                        svga->bpp = 24;
                                }
                                break;
                        }
                }
                else
                {
                        svga->render = svga_render_15bpp_highres;
                        svga->bpp = 15;
                }
        }
        
        // Possible chip 5426/5428 chip bug. Not sure if this is the exact condition but Amiga Picasso96 driver
        // has 8 pixel hwcursor offset in 15-bit and 16-bit modes and GR5 is zero but CGX4 driver
        // does not have offset and GR5 has 256-color bit (bit 6) set.
        gd5429->hwcursor_extraoffset = 0;
        if (!(svga->gdcreg[5] & 0x40) && gd5429->type <= CL_TYPE_GD5428 && (svga->bpp == 15 || svga->bpp == 16)) {
            gd5429->hwcursor_extraoffset = 8;
        }

        n = svga->seqregs[0xb + clock] & 0x7f;
        d = (svga->seqregs[0x1b + clock] >> 1) & 0x1f;
        p = svga->seqregs[0x1b + clock] & 1;

        /*Prevent divide by zero during clock setup*/
        if (d && n)
                vclk = (14318184.0 * ((float)n / (float)d)) / (float)(1 + p);
        else
                vclk = 14318184.0;
        switch (svga->seqregs[7] & ((gd5429->type >= CL_TYPE_GD5434) ? 0xe : 0x6))
        {
                case 2:
                vclk /= 2.0;
                break;
                case 4:
                vclk /= 3.0;
                break;
        }
        svga->clock = (cpuclock * (float)(1ull << 32)) / vclk;
        
        svga->vram_display_mask = (svga->crtc[0x1b] & 2) ? gd5429->vram_mask : 0x3ffff;
}

static void gd5429_adjust_panning(svga_t *svga)
{
    gd5429_t *gd5429 = (gd5429_t *)svga->priv;
    int ar11 = svga->attrregs[0x13] & 7;
    int src = 0, dst = 8;
    switch (svga->bpp)
    {
        case 8:
            if (svga->horizontal_linedbl) {
                dst = 8 - ((ar11 & 3) << 1);
            } else {
                dst = 8 - ar11;
            }
            break;
        case 15:
        case 16:
            dst = 8 - ((ar11 & 2) >> 1);
            break;
        case 24:
            if (gd5429->type >= CL_TYPE_GD5446) {
                dst = 8 - ((ar11 & 3) << 1);
                if (ar11 >= 4) {
                    src += 3;
                }
            } else {
                src = ar11;
            }
            break;
        case 32:
            dst = 8 - (ar11 & 1);
            break;
    }

    dst += 24;
    svga->scrollcache_dst = dst;
    svga->scrollcache_src = src;
}

void gd5429_hwcursor_draw(svga_t *svga, int displine)
{
        gd5429_t *gd5429 = (gd5429_t *)svga->priv;
        int x;
        uint8_t dat[2];
        int xx;
        int offset = svga->hwcursor_latch.x - svga->hwcursor_latch.xoff;
        int line_offset = (svga->seqregs[0x12] & 0x04) ? 16 : 4;

        offset <<= svga->horizontal_linedbl;

        if (svga->interlace && svga->hwcursor_oddeven)
                svga->hwcursor_latch.addr += line_offset;

        if (svga->seqregs[0x12] & 0x04)
        {
                for (x = 0; x < 64; x += 8)
                {
                        dat[0] = svga->vram[svga->hwcursor_latch.addr & svga->vram_display_mask];
                        dat[1] = svga->vram[(svga->hwcursor_latch.addr + 8) & svga->vram_display_mask];
                        for (xx = 0; xx < 8; xx++)
                        {
                                if (offset >= svga->hwcursor_latch.x)
                                {
                                        if (dat[1] & 0x80)
                                                ((uint32_t *)buffer32->line[displine])[offset + 32 - gd5429->hwcursor_extraoffset] = gd5429->hwcursor_pal[(dat[0] & 0x80) ? 1 : 0];
                                        else if (dat[0] & 0x80)
                                                ((uint32_t *)buffer32->line[displine])[offset + 32 - gd5429->hwcursor_extraoffset] ^= 0xffffff;
                                }

                                offset++;
                                dat[0] <<= 1;
                                dat[1] <<= 1;
                        }
                        svga->hwcursor_latch.addr++;
                }
                svga->hwcursor_latch.addr += 8;
        }
        else
        {
                for (x = 0; x < 32; x += 8)
                {
                        dat[0] = svga->vram[svga->hwcursor_latch.addr & svga->vram_display_mask];
                        dat[1] = svga->vram[(svga->hwcursor_latch.addr + 0x80) & svga->vram_display_mask];
                        for (xx = 0; xx < 8; xx++)
                        {
                                if (offset >= svga->hwcursor_latch.x)
                                {
                                        if (dat[1] & 0x80)
                                                ((uint32_t *)buffer32->line[displine])[offset + 32 - gd5429->hwcursor_extraoffset] = gd5429->hwcursor_pal[(dat[0] & 0x80) ? 1 : 0];
                                        else if (dat[0] & 0x80)
                                                ((uint32_t *)buffer32->line[displine])[offset + 32 - gd5429->hwcursor_extraoffset] ^= 0xffffff;
                                }

                                offset++;
                                dat[0] <<= 1;
                                dat[1] <<= 1;
                        }
                        svga->hwcursor_latch.addr++;
                }
        }

        if (svga->interlace && !svga->hwcursor_oddeven)
                svga->hwcursor_latch.addr += line_offset;
}


void gd5429_write_linear(uint32_t addr, uint8_t val, void *p);

static void gd5429_write(uint32_t addr, uint8_t val, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        svga_t *svga = &gd5429->svga;
        
        addr &= svga->banked_mask;
        addr = (addr & 0x7fff) + gd5429->bank[(addr >> 15) & 1];

        gd5429_write_linear(addr, val, p);
}
static void gd5429_writew(uint32_t addr, uint16_t val, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        svga_t *svga = &gd5429->svga;

        addr &= svga->banked_mask;
        addr = (addr & 0x7fff) + gd5429->bank[(addr >> 15) & 1];

        if ((svga->writemode < 4) && !(svga->gdcreg[0xb] & (GRB_X8_ADDRESSING | GRB_8B_LATCHES)))
                svga_writew_linear(addr, val, svga);
        else
        {
                gd5429_write_linear(addr, val, p);
                gd5429_write_linear(addr+1, val >> 8, p);
        }
}
static void gd5429_writel(uint32_t addr, uint32_t val, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        svga_t *svga = &gd5429->svga;

        addr &= svga->banked_mask;
        addr = (addr & 0x7fff) + gd5429->bank[(addr >> 15) & 1];

        if ((svga->writemode < 4) && !(svga->gdcreg[0xb] & (GRB_X8_ADDRESSING | GRB_8B_LATCHES)))
                svga_writel_linear(addr, val, svga);
        else
        {
                gd5429_write_linear(addr, val, p);
                gd5429_write_linear(addr+1, val >> 8, p);
                gd5429_write_linear(addr+2, val >> 16, p);
                gd5429_write_linear(addr+3, val >> 24, p);
        }
}

static uint8_t gd5429_read(uint32_t addr, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        svga_t *svga = &gd5429->svga;

        addr &= svga->banked_mask;
        addr = (addr & 0x7fff) + gd5429->bank[(addr >> 15) & 1];
        return gd5429_read_linear(addr, gd5429);
}
static uint16_t gd5429_readw(uint32_t addr, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        svga_t *svga = &gd5429->svga;

        addr &= svga->banked_mask;
        addr = (addr & 0x7fff) + gd5429->bank[(addr >> 15) & 1];

        if (!(svga->gdcreg[0xb] & (GRB_X8_ADDRESSING | GRB_8B_LATCHES)))
                return svga_readw_linear(addr, &gd5429->svga);
        return gd5429_read_linear(addr, gd5429) | (gd5429_read_linear(addr+1, gd5429) << 8);
}
static uint32_t gd5429_readl(uint32_t addr, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        svga_t *svga = &gd5429->svga;

        addr &= svga->banked_mask;
        addr = (addr & 0x7fff) + gd5429->bank[(addr >> 15) & 1];

        if (!(svga->gdcreg[0xb] & (GRB_X8_ADDRESSING | GRB_8B_LATCHES)))
                return svga_readl_linear(addr, &gd5429->svga);
        return gd5429_read_linear(addr, gd5429) | (gd5429_read_linear(addr+1, gd5429) << 8) |
                (gd5429_read_linear(addr+2, gd5429) << 16) | (gd5429_read_linear(addr+3, gd5429) << 24);
}

void gd5429_write_linear(uint32_t addr, uint8_t val, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        svga_t *svga = &gd5429->svga;
        uint8_t vala, valb, valc, vald, wm = svga->writemask;
        int writemask2 = svga->seqregs[2];

#if 0
        cycles -= video_timing_write_b;
        cycles_lost += video_timing_write_b;

        egawrites++;
        
//        if (svga_output) pclog("Write LFB %08X %02X ", addr, val);
#endif

        if (!(svga->gdcreg[6] & 1)) 
                svga->fullchange = 2;
        if (svga->gdcreg[0xb] & GRB_ENHANCED_16BIT)
                addr <<= 4;
        else if (svga->gdcreg[0xb] & GRB_X8_ADDRESSING)
                addr <<= 3;
        else if (((svga->chain4 && svga->packed_chain4) || svga->fb_only) && (svga->writemode < 4))
        {
                writemask2 = 1 << (addr & 3);
                addr &= ~3;
        }
        else if (svga->chain4)
        {
                writemask2 = 1 << (addr & 3);
                addr = ((addr & 0xfffc) << 2) | ((addr & 0x30000) >> 14) | (addr & ~0x3ffff);
        }
        else if (svga->chain2_write)
        {
                writemask2 &= ~0xa;
                if (addr & 1)
                        writemask2 <<= 1;
                addr &= ~1;
                addr <<= 2;
        }
        else
        {
                addr <<= 2;
        }
        addr &= svga->decode_mask;
        if (addr >= svga->vram_max)
                return;
        addr &= svga->vram_mask;
//        if (svga_output) pclog("%08X\n", addr);
        svga->changedvram[addr >> 12] = changeframecount;
        
        switch (svga->writemode)
        {
                case 4:
                if (svga->gdcreg[0xb] & GRB_ENHANCED_16BIT)
                {
//                        pclog("Writemode 4 : %X ", addr);
                        svga->changedvram[addr >> 12] = changeframecount;
//                        pclog("%X %X  %02x\n", addr, val, svga->gdcreg[0xb]);
                        if (val & svga->seqregs[2] & 0x80)
                        {
                                svga->vram[addr + 0] = svga->gdcreg[1];
                                svga->vram[addr + 1] = svga->gdcreg[0x11];
                        }
                        if (val & svga->seqregs[2] & 0x40)
                        {
                                svga->vram[addr + 2] = svga->gdcreg[1];
                                svga->vram[addr + 3] = svga->gdcreg[0x11];
                        }
                        if (val & svga->seqregs[2] & 0x20)
                        {
                                svga->vram[addr + 4] = svga->gdcreg[1];
                                svga->vram[addr + 5] = svga->gdcreg[0x11];
                        }
                        if (val & svga->seqregs[2] & 0x10)
                        {
                                svga->vram[addr + 6] = svga->gdcreg[1];
                                svga->vram[addr + 7] = svga->gdcreg[0x11];
                        }
                        if (val & svga->seqregs[2] & 0x08)
                        {
                                svga->vram[addr + 8] = svga->gdcreg[1];
                                svga->vram[addr + 9] = svga->gdcreg[0x11];
                        }
                        if (val & svga->seqregs[2] & 0x04)
                        {
                                svga->vram[addr + 10] = svga->gdcreg[1];
                                svga->vram[addr + 11] = svga->gdcreg[0x11];
                        }
                        if (val & svga->seqregs[2] & 0x02)
                        {
                                svga->vram[addr + 12] = svga->gdcreg[1];
                                svga->vram[addr + 13] = svga->gdcreg[0x11];
                        }
                        if (val & svga->seqregs[2] & 0x01)
                        {
                                svga->vram[addr + 14] = svga->gdcreg[1];
                                svga->vram[addr + 15] = svga->gdcreg[0x11];
                        }
                }
                else
                {
//                        pclog("Writemode 4 : %X ", addr);
                        svga->changedvram[addr >> 12] = changeframecount;
//                        pclog("%X %X  %02x\n", addr, val, svga->gdcreg[0xb]);
                        if (val & svga->seqregs[2] & 0x80)
                                svga->vram[addr + 0] = svga->gdcreg[1];
                        if (val & svga->seqregs[2] & 0x40)
                                svga->vram[addr + 1] = svga->gdcreg[1];
                        if (val & svga->seqregs[2] & 0x20)
                                svga->vram[addr + 2] = svga->gdcreg[1];
                        if (val & svga->seqregs[2] & 0x10)
                                svga->vram[addr + 3] = svga->gdcreg[1];
                        if (val & svga->seqregs[2] & 0x08)
                                svga->vram[addr + 4] = svga->gdcreg[1];
                        if (val & svga->seqregs[2] & 0x04)
                                svga->vram[addr + 5] = svga->gdcreg[1];
                        if (val & svga->seqregs[2] & 0x02)
                                svga->vram[addr + 6] = svga->gdcreg[1];
                        if (val & svga->seqregs[2] & 0x01)
                                svga->vram[addr + 7] = svga->gdcreg[1];
                }
                break;
                        
                case 5:
                if (svga->gdcreg[0xb] & GRB_ENHANCED_16BIT)
                {
//                        pclog("Writemode 5 : %X ", addr);
                        svga->changedvram[addr >> 12] = changeframecount;
//                        pclog("%X %X  %02x\n", addr, val, svga->gdcreg[0xb]);
                        if (svga->seqregs[2] & 0x80)
                        {
                                svga->vram[addr +  0] = (val & 0x80) ? svga->gdcreg[1] : svga->gdcreg[0];
                                svga->vram[addr +  1] = (val & 0x80) ? svga->gdcreg[0x11] : svga->gdcreg[0x10];
                        }
                        if (svga->seqregs[2] & 0x40)
                        {
                                svga->vram[addr +  2] = (val & 0x40) ? svga->gdcreg[1] : svga->gdcreg[0];
                                svga->vram[addr +  3] = (val & 0x40) ? svga->gdcreg[0x11] : svga->gdcreg[0x10];
                        }
                        if (svga->seqregs[2] & 0x20)
                        {
                                svga->vram[addr +  4] = (val & 0x20) ? svga->gdcreg[1] : svga->gdcreg[0];
                                svga->vram[addr +  5] = (val & 0x20) ? svga->gdcreg[0x11] : svga->gdcreg[0x10];
                        }
                        if (svga->seqregs[2] & 0x10)
                        {
                                svga->vram[addr +  6] = (val & 0x10) ? svga->gdcreg[1] : svga->gdcreg[0];
                                svga->vram[addr +  7] = (val & 0x10) ? svga->gdcreg[0x11] : svga->gdcreg[0x10];
                        }
                        if (svga->seqregs[2] & 0x08)
                        {
                                svga->vram[addr +  8] = (val & 0x08) ? svga->gdcreg[1] : svga->gdcreg[0];
                                svga->vram[addr +  9] = (val & 0x08) ? svga->gdcreg[0x11] : svga->gdcreg[0x10];
                        }
                        if (svga->seqregs[2] & 0x04)
                        {
                                svga->vram[addr + 10] = (val & 0x04) ? svga->gdcreg[1] : svga->gdcreg[0];
                                svga->vram[addr + 11] = (val & 0x04) ? svga->gdcreg[0x11] : svga->gdcreg[0x10];
                        }
                        if (svga->seqregs[2] & 0x02)
                        {
                                svga->vram[addr + 12] = (val & 0x02) ? svga->gdcreg[1] : svga->gdcreg[0];
                                svga->vram[addr + 13] = (val & 0x02) ? svga->gdcreg[0x11] : svga->gdcreg[0x10];
                        }
                        if (svga->seqregs[2] & 0x01)
                        {
                                svga->vram[addr + 14] = (val & 0x01) ? svga->gdcreg[1] : svga->gdcreg[0];
                                svga->vram[addr + 15] = (val & 0x01) ? svga->gdcreg[0x11] : svga->gdcreg[0x10];
                        }
                }
                else
                {
//                        pclog("Writemode 5 : %X ", addr);
                        svga->changedvram[addr >> 12] = changeframecount;
//                        pclog("%X %X  %02x\n", addr, val, svga->gdcreg[0xb]);
                        if (svga->seqregs[2] & 0x80)
                                svga->vram[addr + 0] = (val & 0x80) ? svga->gdcreg[1] : svga->gdcreg[0];
                        if (svga->seqregs[2] & 0x40)
                                svga->vram[addr + 1] = (val & 0x40) ? svga->gdcreg[1] : svga->gdcreg[0];
                        if (svga->seqregs[2] & 0x20)
                                svga->vram[addr + 2] = (val & 0x20) ? svga->gdcreg[1] : svga->gdcreg[0];
                        if (svga->seqregs[2] & 0x10)
                                svga->vram[addr + 3] = (val & 0x10) ? svga->gdcreg[1] : svga->gdcreg[0];
                        if (svga->seqregs[2] & 0x08)
                                svga->vram[addr + 4] = (val & 0x08) ? svga->gdcreg[1] : svga->gdcreg[0];
                        if (svga->seqregs[2] & 0x04)
                                svga->vram[addr + 5] = (val & 0x04) ? svga->gdcreg[1] : svga->gdcreg[0];
                        if (svga->seqregs[2] & 0x02)
                                svga->vram[addr + 6] = (val & 0x02) ? svga->gdcreg[1] : svga->gdcreg[0];
                        if (svga->seqregs[2] & 0x01)
                                svga->vram[addr + 7] = (val & 0x01) ? svga->gdcreg[1] : svga->gdcreg[0];
                }
                break;
                
                case 1:
                if (svga->gdcreg[0xb] & GRB_WRITEMODE_EXT)
                {
                        if (writemask2 & 0x80) svga->vram[addr]       = svga->la;
                        if (writemask2 & 0x40) svga->vram[addr | 0x1] = svga->lb;
                        if (writemask2 & 0x20) svga->vram[addr | 0x2] = svga->lc;
                        if (writemask2 & 0x10) svga->vram[addr | 0x3] = svga->ld;
                        if (svga->gdcreg[0xb] & GRB_8B_LATCHES)
                        {
                                if (writemask2 & 0x08) svga->vram[addr | 0x4] = gd5429->latch_ext[0];
                                if (writemask2 & 0x04) svga->vram[addr | 0x5] = gd5429->latch_ext[1];
                                if (writemask2 & 0x02) svga->vram[addr | 0x6] = gd5429->latch_ext[2];
                                if (writemask2 & 0x01) svga->vram[addr | 0x7] = gd5429->latch_ext[3];
                        }
                }
                else
                {
                        if (writemask2 & 1) svga->vram[addr]       = svga->la;
                        if (writemask2 & 2) svga->vram[addr | 0x1] = svga->lb;
                        if (writemask2 & 4) svga->vram[addr | 0x2] = svga->lc;
                        if (writemask2 & 8) svga->vram[addr | 0x3] = svga->ld;
                }
                break;
                case 0:
                if (svga->gdcreg[3] & 7) 
                        val = svga_rotate[svga->gdcreg[3] & 7][val];
                if (svga->gdcreg[8] == 0xff && !(svga->gdcreg[3] & 0x18) && (!svga->gdcreg[1] || svga->set_reset_disabled))
                {
                        if (writemask2 & 1) svga->vram[addr]       = val;
                        if (writemask2 & 2) svga->vram[addr | 0x1] = val;
                        if (writemask2 & 4) svga->vram[addr | 0x2] = val;
                        if (writemask2 & 8) svga->vram[addr | 0x3] = val;
                }
                else
                {
                        if (svga->gdcreg[1] & 1) vala = (svga->gdcreg[0] & 1) ? 0xff : 0;
                        else                     vala = val;
                        if (svga->gdcreg[1] & 2) valb = (svga->gdcreg[0] & 2) ? 0xff : 0;
                        else                     valb = val;
                        if (svga->gdcreg[1] & 4) valc = (svga->gdcreg[0] & 4) ? 0xff : 0;
                        else                     valc = val;
                        if (svga->gdcreg[1] & 8) vald = (svga->gdcreg[0] & 8) ? 0xff : 0;
                        else                     vald = val;

                        switch (svga->gdcreg[3] & 0x18)
                        {
                                case 0: /*Set*/
                                if (writemask2 & 1) svga->vram[addr]       = (vala & svga->gdcreg[8]) | (svga->la & ~svga->gdcreg[8]);
                                if (writemask2 & 2) svga->vram[addr | 0x1] = (valb & svga->gdcreg[8]) | (svga->lb & ~svga->gdcreg[8]);
                                if (writemask2 & 4) svga->vram[addr | 0x2] = (valc & svga->gdcreg[8]) | (svga->lc & ~svga->gdcreg[8]);
                                if (writemask2 & 8) svga->vram[addr | 0x3] = (vald & svga->gdcreg[8]) | (svga->ld & ~svga->gdcreg[8]);
                                break;
                                case 8: /*AND*/
                                if (writemask2 & 1) svga->vram[addr]       = (vala | ~svga->gdcreg[8]) & svga->la;
                                if (writemask2 & 2) svga->vram[addr | 0x1] = (valb | ~svga->gdcreg[8]) & svga->lb;
                                if (writemask2 & 4) svga->vram[addr | 0x2] = (valc | ~svga->gdcreg[8]) & svga->lc;
                                if (writemask2 & 8) svga->vram[addr | 0x3] = (vald | ~svga->gdcreg[8]) & svga->ld;
                                break;
                                case 0x10: /*OR*/
                                if (writemask2 & 1) svga->vram[addr]       = (vala & svga->gdcreg[8]) | svga->la;
                                if (writemask2 & 2) svga->vram[addr | 0x1] = (valb & svga->gdcreg[8]) | svga->lb;
                                if (writemask2 & 4) svga->vram[addr | 0x2] = (valc & svga->gdcreg[8]) | svga->lc;
                                if (writemask2 & 8) svga->vram[addr | 0x3] = (vald & svga->gdcreg[8]) | svga->ld;
                                break;
                                case 0x18: /*XOR*/
                                if (writemask2 & 1) svga->vram[addr]       = (vala & svga->gdcreg[8]) ^ svga->la;
                                if (writemask2 & 2) svga->vram[addr | 0x1] = (valb & svga->gdcreg[8]) ^ svga->lb;
                                if (writemask2 & 4) svga->vram[addr | 0x2] = (valc & svga->gdcreg[8]) ^ svga->lc;
                                if (writemask2 & 8) svga->vram[addr | 0x3] = (vald & svga->gdcreg[8]) ^ svga->ld;
                                break;
                        }
//                        pclog("- %02X %02X %02X %02X   %08X\n",vram[addr],vram[addr|0x1],vram[addr|0x2],vram[addr|0x3],addr);
                }
                break;
                case 2:
                if (!(svga->gdcreg[3] & 0x18) && !svga->gdcreg[1])
                {
                        if (writemask2 & 1) svga->vram[addr]       = (((val & 1) ? 0xff : 0) & svga->gdcreg[8]) | (svga->la & ~svga->gdcreg[8]);
                        if (writemask2 & 2) svga->vram[addr | 0x1] = (((val & 2) ? 0xff : 0) & svga->gdcreg[8]) | (svga->lb & ~svga->gdcreg[8]);
                        if (writemask2 & 4) svga->vram[addr | 0x2] = (((val & 4) ? 0xff : 0) & svga->gdcreg[8]) | (svga->lc & ~svga->gdcreg[8]);
                        if (writemask2 & 8) svga->vram[addr | 0x3] = (((val & 8) ? 0xff : 0) & svga->gdcreg[8]) | (svga->ld & ~svga->gdcreg[8]);
                }
                else
                {
                        vala = ((val & 1) ? 0xff : 0);
                        valb = ((val & 2) ? 0xff : 0);
                        valc = ((val & 4) ? 0xff : 0);
                        vald = ((val & 8) ? 0xff : 0);
                        switch (svga->gdcreg[3] & 0x18)
                        {
                                case 0: /*Set*/
                                if (writemask2 & 1) svga->vram[addr]       = (vala & svga->gdcreg[8]) | (svga->la & ~svga->gdcreg[8]);
                                if (writemask2 & 2) svga->vram[addr | 0x1] = (valb & svga->gdcreg[8]) | (svga->lb & ~svga->gdcreg[8]);
                                if (writemask2 & 4) svga->vram[addr | 0x2] = (valc & svga->gdcreg[8]) | (svga->lc & ~svga->gdcreg[8]);
                                if (writemask2 & 8) svga->vram[addr | 0x3] = (vald & svga->gdcreg[8]) | (svga->ld & ~svga->gdcreg[8]);
                                break;
                                case 8: /*AND*/
                                if (writemask2 & 1) svga->vram[addr]       = (vala | ~svga->gdcreg[8]) & svga->la;
                                if (writemask2 & 2) svga->vram[addr | 0x1] = (valb | ~svga->gdcreg[8]) & svga->lb;
                                if (writemask2 & 4) svga->vram[addr | 0x2] = (valc | ~svga->gdcreg[8]) & svga->lc;
                                if (writemask2 & 8) svga->vram[addr | 0x3] = (vald | ~svga->gdcreg[8]) & svga->ld;
                                break;
                                case 0x10: /*OR*/
                                if (writemask2 & 1) svga->vram[addr]       = (vala & svga->gdcreg[8]) | svga->la;
                                if (writemask2 & 2) svga->vram[addr | 0x1] = (valb & svga->gdcreg[8]) | svga->lb;
                                if (writemask2 & 4) svga->vram[addr | 0x2] = (valc & svga->gdcreg[8]) | svga->lc;
                                if (writemask2 & 8) svga->vram[addr | 0x3] = (vald & svga->gdcreg[8]) | svga->ld;
                                break;
                                case 0x18: /*XOR*/
                                if (writemask2 & 1) svga->vram[addr]       = (vala & svga->gdcreg[8]) ^ svga->la;
                                if (writemask2 & 2) svga->vram[addr | 0x1] = (valb & svga->gdcreg[8]) ^ svga->lb;
                                if (writemask2 & 4) svga->vram[addr | 0x2] = (valc & svga->gdcreg[8]) ^ svga->lc;
                                if (writemask2 & 8) svga->vram[addr | 0x3] = (vald & svga->gdcreg[8]) ^ svga->ld;
                                break;
                        }
                }
                break;
                case 3:
                if (svga->gdcreg[3] & 7) 
                        val = svga_rotate[svga->gdcreg[3] & 7][val];
                wm = svga->gdcreg[8];
                svga->gdcreg[8] &= val;

                vala = (svga->gdcreg[0] & 1) ? 0xff : 0;
                valb = (svga->gdcreg[0] & 2) ? 0xff : 0;
                valc = (svga->gdcreg[0] & 4) ? 0xff : 0;
                vald = (svga->gdcreg[0] & 8) ? 0xff : 0;
                switch (svga->gdcreg[3] & 0x18)
                {
                        case 0: /*Set*/
                        if (writemask2 & 1) svga->vram[addr]       = (vala & svga->gdcreg[8]) | (svga->la & ~svga->gdcreg[8]);
                        if (writemask2 & 2) svga->vram[addr | 0x1] = (valb & svga->gdcreg[8]) | (svga->lb & ~svga->gdcreg[8]);
                        if (writemask2 & 4) svga->vram[addr | 0x2] = (valc & svga->gdcreg[8]) | (svga->lc & ~svga->gdcreg[8]);
                        if (writemask2 & 8) svga->vram[addr | 0x3] = (vald & svga->gdcreg[8]) | (svga->ld & ~svga->gdcreg[8]);
                        break;
                        case 8: /*AND*/
                        if (writemask2 & 1) svga->vram[addr]       = (vala | ~svga->gdcreg[8]) & svga->la;
                        if (writemask2 & 2) svga->vram[addr | 0x1] = (valb | ~svga->gdcreg[8]) & svga->lb;
                        if (writemask2 & 4) svga->vram[addr | 0x2] = (valc | ~svga->gdcreg[8]) & svga->lc;
                        if (writemask2 & 8) svga->vram[addr | 0x3] = (vald | ~svga->gdcreg[8]) & svga->ld;
                        break;
                        case 0x10: /*OR*/
                        if (writemask2 & 1) svga->vram[addr]       = (vala & svga->gdcreg[8]) | svga->la;
                        if (writemask2 & 2) svga->vram[addr | 0x1] = (valb & svga->gdcreg[8]) | svga->lb;
                        if (writemask2 & 4) svga->vram[addr | 0x2] = (valc & svga->gdcreg[8]) | svga->lc;
                        if (writemask2 & 8) svga->vram[addr | 0x3] = (vald & svga->gdcreg[8]) | svga->ld;
                        break;
                        case 0x18: /*XOR*/
                        if (writemask2 & 1) svga->vram[addr]       = (vala & svga->gdcreg[8]) ^ svga->la;
                        if (writemask2 & 2) svga->vram[addr | 0x1] = (valb & svga->gdcreg[8]) ^ svga->lb;
                        if (writemask2 & 4) svga->vram[addr | 0x2] = (valc & svga->gdcreg[8]) ^ svga->lc;
                        if (writemask2 & 8) svga->vram[addr | 0x3] = (vald & svga->gdcreg[8]) ^ svga->ld;
                        break;
                }
                svga->gdcreg[8] = wm;
                break;
        }
}

uint8_t gd5429_read_linear(uint32_t addr, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        svga_t *svga = &gd5429->svga;
        uint8_t temp, temp2, temp3, temp4;
        int readplane = svga->readplane;
        uint32_t latch_addr;
        
#if 0
        cycles -= video_timing_read_b;
        cycles_lost += video_timing_read_b;

        egareads++;
#endif

        if (svga->gdcreg[0xb] & GRB_ENHANCED_16BIT)
                latch_addr = (addr << 4) & svga->decode_mask;
        else if (svga->gdcreg[0xb] & GRB_X8_ADDRESSING)
                latch_addr = (addr << 3) & svga->decode_mask;
        else
                latch_addr = (addr << 2) & svga->decode_mask;
        
        if (svga->gdcreg[0xb] & GRB_ENHANCED_16BIT)
                addr <<= 4;
        else if (svga->gdcreg[0xb] & GRB_X8_ADDRESSING)
                addr <<= 3;
        else if ((svga->chain4 && svga->packed_chain4) || svga->fb_only)
        {
                addr &= svga->decode_mask;
                if (addr >= svga->vram_max)
                        return 0xff;
                return svga->vram[addr & svga->vram_mask];
        }
        else if (svga->chain4)
        {
                readplane = addr & 3;
                addr = ((addr & 0xfffc) << 2) | ((addr & 0x30000) >> 14) | (addr & ~0x3ffff);
        }
        else if (svga->chain2_read)
        {
                readplane = (readplane & 2) | (addr & 1);
                addr &= ~1;
                addr <<= 2;
        }
        else
                addr<<=2;

        addr &= svga->decode_mask;

        if (latch_addr >= svga->vram_max)
        {
                svga->la = svga->lb = svga->lc = svga->ld = 0xff;
                if (svga->gdcreg[0xb] & GRB_8B_LATCHES)
                        gd5429->latch_ext[0] = gd5429->latch_ext[1] = gd5429->latch_ext[2] = gd5429->latch_ext[3] = 0xff;
        }
        else
        {
                latch_addr &= svga->vram_mask;
                svga->la = svga->vram[latch_addr];
                svga->lb = svga->vram[latch_addr | 0x1];
                svga->lc = svga->vram[latch_addr | 0x2];
                svga->ld = svga->vram[latch_addr | 0x3];
                if (svga->gdcreg[0xb] & GRB_8B_LATCHES)
                {
                        gd5429->latch_ext[0] = svga->vram[latch_addr | 0x4];
                        gd5429->latch_ext[1] = svga->vram[latch_addr | 0x5];
                        gd5429->latch_ext[2] = svga->vram[latch_addr | 0x6];
                        gd5429->latch_ext[3] = svga->vram[latch_addr | 0x7];
                }
        }

#if 0
        if (addr >= svga->vram_max)
                return 0xff;

        addr &= svga->vram_mask;
#endif

        if (svga->readmode)
        {
                temp   = svga->la;
                temp  ^= (svga->colourcompare & 1) ? 0xff : 0;
                temp  &= (svga->colournocare & 1)  ? 0xff : 0;
                temp2  = svga->lb;
                temp2 ^= (svga->colourcompare & 2) ? 0xff : 0;
                temp2 &= (svga->colournocare & 2)  ? 0xff : 0;
                temp3  = svga->lc;
                temp3 ^= (svga->colourcompare & 4) ? 0xff : 0;
                temp3 &= (svga->colournocare & 4)  ? 0xff : 0;
                temp4  = svga->ld;
                temp4 ^= (svga->colourcompare & 8) ? 0xff : 0;
                temp4 &= (svga->colournocare & 8)  ? 0xff : 0;
                return ~(temp | temp2 | temp3 | temp4);
        }
//printf("Read %02X %04X %04X\n",vram[addr|svga->readplane],addr,svga->readplane);
        return svga->vram[addr | readplane];
}

static void gd5429_writeb_linear(uint32_t addr, uint8_t val, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        svga_t *svga = &gd5429->svga;

        if ((svga->writemode < 4) && !(svga->gdcreg[0xb] & (GRB_X8_ADDRESSING | GRB_8B_LATCHES)))
                svga_write_linear(addr, val, svga);
        else
                gd5429_write_linear(addr, val & 0xff, gd5429);
}
static void gd5429_writew_linear(uint32_t addr, uint16_t val, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        svga_t *svga = &gd5429->svga;

        if ((svga->writemode < 4) && !(svga->gdcreg[0xb] & (GRB_X8_ADDRESSING | GRB_8B_LATCHES)))
                svga_writew_linear(addr, val, svga);
        else
        {
                gd5429_write_linear(addr, val & 0xff, gd5429);
                gd5429_write_linear(addr+1, val >> 8, gd5429);
        }
}
static void gd5429_writel_linear(uint32_t addr, uint32_t val, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        svga_t *svga = &gd5429->svga;

        if ((svga->writemode < 4) && !(svga->gdcreg[0xb] & (GRB_X8_ADDRESSING | GRB_8B_LATCHES)))
                svga_writel_linear(addr, val, svga);
        else
        {
                gd5429_write_linear(addr, val & 0xff, gd5429);
                gd5429_write_linear(addr+1, val >> 8, gd5429);
                gd5429_write_linear(addr+2, val >> 16, gd5429);
                gd5429_write_linear(addr+3, val >> 24, gd5429);
        }
}

static uint8_t gd5429_readb_linear(uint32_t addr, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        svga_t *svga = &gd5429->svga;

        if (!(svga->gdcreg[0xb] & (GRB_X8_ADDRESSING | GRB_8B_LATCHES)))
                return svga_read_linear(addr, &gd5429->svga);
        return gd5429_read_linear(addr, gd5429);
}
static uint16_t gd5429_readw_linear(uint32_t addr, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        svga_t *svga = &gd5429->svga;

        if (!(svga->gdcreg[0xb] & (GRB_X8_ADDRESSING | GRB_8B_LATCHES)))
                return svga_readw_linear(addr, &gd5429->svga);
        return gd5429_read_linear(addr, gd5429) | (gd5429_read_linear(addr+1, gd5429) << 8);
}
static uint32_t gd5429_readl_linear(uint32_t addr, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        svga_t *svga = &gd5429->svga;

        if (!(svga->gdcreg[0xb] & (GRB_X8_ADDRESSING | GRB_8B_LATCHES)))
                return svga_readl_linear(addr, &gd5429->svga);
        return gd5429_read_linear(addr, gd5429) | (gd5429_read_linear(addr+1, gd5429) << 8) |
                (gd5429_read_linear(addr+2, gd5429) << 16) | (gd5429_read_linear(addr+3, gd5429) << 24);
}


void gd5429_start_blit(uint32_t cpu_dat, int count, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        svga_t *svga = &gd5429->svga;
        int blt_mask = gd5429->blt.mask & 7;
        int fg_col = gd5429->blt.fg_col;
        int bg_col = gd5429->blt.bg_col;
        int x_max = 0;
        int bplcnt = 0;
        int bpp;
        uint8_t dststore[4];
        int maskstore = 0;

        switch (gd5429->blt.depth)
        {
                case BLIT_DEPTH_8:
                x_max = 8;
                bpp = 1;
                break;
                case BLIT_DEPTH_16:
                x_max = 16;
                blt_mask *= 2;
                bpp = 2;
                break;
                case BLIT_DEPTH_24:
                x_max = 24;
                blt_mask *= 3;
                bpp = 3;
                break;
                case BLIT_DEPTH_32:
                x_max = 32;
                blt_mask *= 4;
                bpp = 4;
                break;
        }
                
//        pclog("gd5429_start_blit %i\n", count);
        if (count == -1)
        {
                if (gd5429->blt.status & 4)
                    gd5429->blt.status &= ~(8 | 2 | 1);
                if (!(gd5429->blt.status & 2))
                    return;
                gd5429->blt.status |= 8 | 1;

                gd5429->blt.dst_addr_backup = gd5429->blt.dst_addr;
                gd5429->blt.src_addr_backup = gd5429->blt.src_addr;
                gd5429->blt.width_backup    = gd5429->blt.width;
                gd5429->blt.height_internal = gd5429->blt.height;
                gd5429->blt.x_count         = 0;
                if ((gd5429->blt.mode & 0xc0) == 0xc0)
                        gd5429->blt.y_count = gd5429->blt.src_addr & 7;
                else
                        gd5429->blt.y_count = 0;
//                pclog("gd5429_start_blit : size %i, %i %i %02x %02x %02x %02x %d\n",
//                    gd5429->blt.width, gd5429->blt.height, gd5429->blt.x_count, gd5429->blt.rop, gd5429->blt.mode, gd5429->blt.extensions, gd5429->blt.mask, gd5429->blt.depth);

                if (gd5429->blt.mode & 0x04)
                {
//                        pclog("blt.mode & 0x04\n");
                        if (!(svga->seqregs[7] & 0xf0))
                        {
                                mem_mapping_set_handlerx(&svga->mapping, NULL, NULL, NULL, NULL, gd5429_blt_write_w, gd5429_blt_write_l);
                                mem_mapping_set_px(&svga->mapping, gd5429);
                        }
                        else
                        {
                                mem_mapping_set_handlerx(&gd5429->linear_mapping, NULL, NULL, NULL, NULL, gd5429_blt_write_w, gd5429_blt_write_l);
                                mem_mapping_set_px(&gd5429->linear_mapping, gd5429);
                        }
                        gd5429_recalc_mapping(gd5429);
                        return;
                }
                else
                {
                        if (!(svga->seqregs[7] & 0xf0))
                                mem_mapping_set_handlerx(&svga->mapping, gd5429_read, gd5429_readw, gd5429_readl, gd5429_write, gd5429_writew, gd5429_writel);
                        else
                                mem_mapping_set_handlerx(&gd5429->linear_mapping, gd5429_readb_linear, gd5429_readw_linear, gd5429_readl_linear, gd5429_writeb_linear, gd5429_writew_linear, gd5429_writel_linear);
                        gd5429_recalc_mapping(gd5429);
                }                
        }
        else if (gd5429->blt.height_internal == 0xffff) {
            gd5429->blt.status &= 0x80;
            return;
        }
        
        while (count)
        {
                uint8_t src = 0, dst;
                int mask = 0;
                int shift;
                
                if (gd5429->blt.depth == BLIT_DEPTH_32)
                        shift = (gd5429->blt.x_count & 3) * 8;
                else if (gd5429->blt.depth == BLIT_DEPTH_24)
                    shift = (gd5429->blt.x_count % 3) * 8;
                else if (gd5429->blt.depth == BLIT_DEPTH_8)
                        shift = 0;
                else
                        shift = (gd5429->blt.x_count & 1) * 8;
                
                if (gd5429->blt.mode & 0x04)
                {
                        if (gd5429->blt.mode & 0x80)
                        {                                
                                mask = cpu_dat & 0x80;
                                
                                switch (gd5429->blt.depth)
                                {
                                        case BLIT_DEPTH_8:
                                        src = mask ? fg_col : bg_col;
                                        cpu_dat <<= 1;
                                        count--;
                                        break;
                                        case BLIT_DEPTH_16:
                                        src = mask ? (fg_col >> shift) : (bg_col >> shift);
                                        if (gd5429->blt.x_count & 1)
                                        {
                                                cpu_dat <<= 1;
                                                count--;
                                        }
                                        break;
                                        case BLIT_DEPTH_24:
                                        src = mask ? (fg_col >> shift) : (bg_col >> shift);
                                        if ((gd5429->blt.x_count % 3) == 2)
                                        {
                                                cpu_dat <<= 1;
                                                count--;
                                        }
                                        break;
                                        case BLIT_DEPTH_32:
                                        src = mask ? (fg_col >> shift) : (bg_col >> shift);
                                        if ((gd5429->blt.x_count & 3) == 3)
                                        {
                                                cpu_dat <<= 1;
                                                count--;
                                        }
                                        break;
                                }
                        }
                        else
                        {
                                src = cpu_dat & 0xff;
                                cpu_dat >>= 8;
                                count -= 8;
                                mask = 1;
                        }
                }
                else
                {
                        switch (gd5429->blt.mode & 0xc0)
                        {
                                case 0x00:
                                src = svga->vram[gd5429->blt.src_addr & svga->vram_mask];
                                gd5429->blt.src_addr += (gd5429->blt.mode & 0x01) ? -1 : 1;
                                mask = 1;
                                break;
                                case 0x40:
                                switch (gd5429->blt.depth)
                                {
                                        case BLIT_DEPTH_8:
                                        src = svga->vram[(gd5429->blt.src_addr & (svga->vram_mask & ~7)) + (gd5429->blt.y_count << 3) + (gd5429->blt.x_count & 7)];
                                        break;
                                        case BLIT_DEPTH_16:
                                        src = svga->vram[(gd5429->blt.src_addr & (svga->vram_mask & ~3)) + (gd5429->blt.y_count << 4) + (gd5429->blt.x_count & 15)];
                                        break;
                                        case BLIT_DEPTH_24:
                                        src = svga->vram[(gd5429->blt.src_addr & (svga->vram_mask & ~3)) + (gd5429->blt.y_count << 5) + (gd5429->blt.x_count & 31)];
                                        break;
                                        case BLIT_DEPTH_32:
                                        src = svga->vram[(gd5429->blt.src_addr & (svga->vram_mask & ~3)) + (gd5429->blt.y_count << 5) + (gd5429->blt.x_count & 31)];
                                        break;
                                }
                                mask = 1;
                                break;
                                case 0x80:
                                    if (gd5429->blt.extensions & 4) {
                                        // Solid color fill
                                        src = fg_col >> shift;
                                        mask = 1;
                                    } else {
                                        switch (gd5429->blt.depth)
                                        {
                                        case BLIT_DEPTH_8:
                                            mask = svga->vram[gd5429->blt.src_addr & svga->vram_mask] & (0x80 >> gd5429->blt.x_count);
                                            if (gd5429->blt.extensions & 2)
                                                mask = mask == 0;
                                            src = mask ? fg_col : bg_col;
                                            break;
                                        case BLIT_DEPTH_16:
                                            mask = svga->vram[gd5429->blt.src_addr & svga->vram_mask] & (0x80 >> (gd5429->blt.x_count >> 1));
                                            if (gd5429->blt.extensions & 2)
                                                mask = mask == 0;
                                            src = mask ? (fg_col >> shift) : (bg_col >> shift);
                                            break;
                                        case BLIT_DEPTH_24:
                                            mask = svga->vram[gd5429->blt.src_addr & svga->vram_mask] & (0x80 >> (gd5429->blt.x_count / 3));
                                            if (gd5429->blt.extensions & 2)
                                                mask = mask == 0;
                                            src = mask ? (fg_col >> shift) : (bg_col >> shift);
                                            break;
                                        case BLIT_DEPTH_32:
                                            mask = svga->vram[gd5429->blt.src_addr & svga->vram_mask] & (0x80 >> (gd5429->blt.x_count >> 2));
                                            if (gd5429->blt.extensions & 2)
                                                mask = mask == 0;
                                            src = mask ? (fg_col >> shift) : (bg_col >> shift);
                                            break;
                                        }
                                    }
                                break;
                                case 0xc0:
                                    if (gd5429->blt.extensions & 4) {
                                        // Solid color fill
                                        src = fg_col >> shift;
                                        mask = 1;
                                    } else {
                                        switch (gd5429->blt.depth)
                                        {
                                        case BLIT_DEPTH_8:
                                            mask = svga->vram[(gd5429->blt.src_addr & svga->vram_mask & ~7) | gd5429->blt.y_count] & (0x80 >> gd5429->blt.x_count);
                                            if (gd5429->blt.extensions & 2)
                                                mask = mask == 0;
                                            src = mask ? fg_col : bg_col;
                                            break;
                                        case BLIT_DEPTH_16:
                                            mask = svga->vram[(gd5429->blt.src_addr & svga->vram_mask & ~7) | gd5429->blt.y_count] & (0x80 >> (gd5429->blt.x_count >> 1));
                                            if (gd5429->blt.extensions & 2)
                                                mask = mask == 0;
                                            src = mask ? (fg_col >> shift) : (bg_col >> shift);
                                            break;
                                        case BLIT_DEPTH_24:
                                            mask = svga->vram[(gd5429->blt.src_addr & svga->vram_mask & ~7) | gd5429->blt.y_count] & (0x80 >> (gd5429->blt.x_count / 3));
                                            if (gd5429->blt.extensions & 2)
                                                mask = mask == 0;
                                            src = mask ? (fg_col >> shift) : (bg_col >> shift);
                                            break;
                                        case BLIT_DEPTH_32:
                                            mask = svga->vram[(gd5429->blt.src_addr & svga->vram_mask & ~7) | gd5429->blt.y_count] & (0x80 >> (gd5429->blt.x_count >> 2));
                                            if (gd5429->blt.extensions & 2)
                                                mask = mask == 0;
                                            src = mask ? (fg_col >> shift) : (bg_col >> shift);
                                            break;
                                        }
                                    }
                                break;
                        }
                        count--;                        
                }
                dst = svga->vram[(gd5429->blt.dst_addr + bplcnt) & svga->vram_mask];
               
                //pclog("Blit %i,%i %06X %06X  %06X %02X %02X  %02X %02X\n", gd5429->blt.width, gd5429->blt.height_internal, gd5429->blt.src_addr, gd5429->blt.dst_addr, gd5429->blt.src_addr & svga->vram_mask, svga->vram[gd5429->blt.src_addr & svga->vram_mask], 0x80 >> (gd5429->blt.dst_addr & 7), src, dst);
                switch (gd5429->blt.rop)
                {
                        case 0x00: dst = 0;             break;
                        case 0x05: dst =   src &  dst;  break;
                        case 0x06: dst =   dst;         break;
                        case 0x09: dst =   src & ~dst;  break;
                        case 0x0b: dst = ~ dst;         break;
                        case 0x0d: dst =   src;         break;
                        case 0x0e: dst = 0xff;          break;
                        case 0x50: dst = ~ src &  dst;  break;
                        case 0x59: dst =   src ^  dst;  break;
                        case 0x6d: dst =   src |  dst;  break;
                        case 0x90: dst = ~(src |  dst); break;
                        case 0x95: dst = ~(src ^  dst); break;
                        case 0xad: dst =   src | ~dst;  break;
                        case 0xd0: dst =  ~src;         break;
                        case 0xd6: dst =  ~src |  dst;  break;
                        case 0xda: dst = ~(src &  dst); break;
                }

                dststore[bplcnt++] = dst;
                maskstore |= mask;
                if (bplcnt >= bpp) {
                    if (bpp == 1) {
                        if ((gd5429->blt.width_backup - gd5429->blt.width) >= blt_mask &&
                            !((gd5429->blt.mode & 0x08) && !mask))
                            svga->vram[gd5429->blt.dst_addr & svga->vram_mask] = dst;
                            svga->changedvram[((gd5429->blt.dst_addr) & svga->vram_mask) >> 12] = changeframecount;
                    } else if (bpp == 2) {
                        if ((gd5429->blt.width_backup - gd5429->blt.width) >= blt_mask &&
                            !((gd5429->blt.mode & 0x08) && !maskstore)) {
                            svga->vram[(gd5429->blt.dst_addr + 0) & svga->vram_mask] = dststore[0];
                            svga->vram[(gd5429->blt.dst_addr + 1) & svga->vram_mask] = dststore[1];
                            svga->changedvram[((gd5429->blt.dst_addr + 0) & svga->vram_mask) >> 12] = changeframecount;
                            svga->changedvram[((gd5429->blt.dst_addr + 1) & svga->vram_mask) >> 12] = changeframecount;
                        }
                    } else if (bpp == 3) {
                        if ((gd5429->blt.width_backup - gd5429->blt.width) >= blt_mask &&
                            !((gd5429->blt.mode & 0x08) && !maskstore)) {
                            svga->vram[(gd5429->blt.dst_addr + 0) & svga->vram_mask] = dststore[0];
                            svga->vram[(gd5429->blt.dst_addr + 1) & svga->vram_mask] = dststore[1];
                            svga->vram[(gd5429->blt.dst_addr + 2) & svga->vram_mask] = dststore[2];
                            svga->changedvram[((gd5429->blt.dst_addr + 0) & svga->vram_mask) >> 12] = changeframecount;
                            svga->changedvram[((gd5429->blt.dst_addr + 2) & svga->vram_mask) >> 12] = changeframecount;
                        }
                    } else if (bpp == 4) {
                        if ((gd5429->blt.width_backup - gd5429->blt.width) >= blt_mask &&
                            !((gd5429->blt.mode & 0x08) && !maskstore)) {
                            svga->vram[(gd5429->blt.dst_addr + 0) & svga->vram_mask] = dststore[0];
                            svga->vram[(gd5429->blt.dst_addr + 1) & svga->vram_mask] = dststore[1];
                            svga->vram[(gd5429->blt.dst_addr + 2) & svga->vram_mask] = dststore[2];
                            svga->vram[(gd5429->blt.dst_addr + 3) & svga->vram_mask] = dststore[3];
                            svga->changedvram[((gd5429->blt.dst_addr + 0) & svga->vram_mask) >> 12] = changeframecount;
                            svga->changedvram[((gd5429->blt.dst_addr + 3) & svga->vram_mask) >> 12] = changeframecount;
                        }
                    }
                    gd5429->blt.dst_addr += (gd5429->blt.mode & 0x01) ? -bpp : bpp;
                    bplcnt = 0;
                    maskstore = 0;
                }

                gd5429->blt.x_count++;
                if (gd5429->blt.x_count == x_max)
                {
                        gd5429->blt.x_count = 0;
                        if ((gd5429->blt.mode & 0xc0) == 0x80)
                                gd5429->blt.src_addr++;
                }
                
                gd5429->blt.width--;
                
                if (gd5429->blt.width == 0xffff)
                {
                        gd5429->blt.width = gd5429->blt.width_backup;

                        gd5429->blt.dst_addr = gd5429->blt.dst_addr_backup = gd5429->blt.dst_addr_backup + ((gd5429->blt.mode & 0x01) ? -gd5429->blt.dst_pitch : gd5429->blt.dst_pitch);
                        
                        switch (gd5429->blt.mode & 0xc0)
                        {
                                case 0x00:
                                gd5429->blt.src_addr = gd5429->blt.src_addr_backup = gd5429->blt.src_addr_backup + ((gd5429->blt.mode & 0x01) ? -gd5429->blt.src_pitch : gd5429->blt.src_pitch);
                                break;
                                case 0x80:
                                if (gd5429->blt.x_count != 0)
                                        gd5429->blt.src_addr++;
                                break;
                        }

                        gd5429->blt.x_count = 0;
                        if (gd5429->blt.mode & 0x01)
                                gd5429->blt.y_count = (gd5429->blt.y_count - 1) & 7;
                        else
                                gd5429->blt.y_count = (gd5429->blt.y_count + 1) & 7;
                        
                        gd5429->blt.height_internal--;
                        if (gd5429->blt.height_internal == 0xffff)
                        {
                                if (gd5429->blt.mode & 0x04)
                                {
                                        if (!(svga->seqregs[7] & 0xf0))
                                                mem_mapping_set_handlerx(&svga->mapping, gd5429_read, gd5429_readw, gd5429_readl, gd5429_write, gd5429_writew, gd5429_writel);
                                        else
                                                mem_mapping_set_handlerx(&gd5429->linear_mapping, gd5429_readb_linear, gd5429_readw_linear, gd5429_readl_linear, gd5429_writeb_linear, gd5429_writew_linear, gd5429_writel_linear);
//                                        mem_mapping_set_handlerx(&gd5429->svga.mapping, gd5429_read, NULL, NULL, gd5429_write, NULL, NULL);
//                                        mem_mapping_set_px(&gd5429->svga.mapping, gd5429);
                                        gd5429_recalc_mapping(gd5429);
                                }
                                gd5429->blt.status &= 0x80;
                                return;
                        }
                                
                        if (gd5429->blt.mode & 0x04)
                                return;
                }                        
        }
}

static void gd5429_mmio_write(uint32_t addr, uint8_t val, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;

        if ((addr & ~0xff) == 0xb8000)
        {
        //        pclog("MMIO write %08X %02X\n", addr, val);
                switch (addr & 0xff)
                {
                        case 0x00:
                        if (gd5429->type >= CL_TYPE_GD5434)
                                gd5429->blt.bg_col = (gd5429->blt.bg_col & 0xffffff00) | val;
                        else
                                gd5429->blt.bg_col = (gd5429->blt.bg_col & 0xff00) | val;
                        break;
                        case 0x01:
                        if (gd5429->type >= CL_TYPE_GD5434)
                                gd5429->blt.bg_col = (gd5429->blt.bg_col & 0xffff00ff) | (val << 8);
                        else
                                gd5429->blt.bg_col = (gd5429->blt.bg_col & 0x00ff) | (val << 8);
                        break;
                        case 0x02:
                        if (gd5429->type >= CL_TYPE_GD5434)
                                gd5429->blt.bg_col = (gd5429->blt.bg_col & 0xff00ffff) | (val << 16);
                        break;
                        case 0x03:
                        if (gd5429->type >= CL_TYPE_GD5434)
                                gd5429->blt.bg_col = (gd5429->blt.bg_col & 0x00ffffff) | (val << 24);
                        break;

                        case 0x04:
                        if (gd5429->type >= CL_TYPE_GD5434)
                                gd5429->blt.fg_col = (gd5429->blt.fg_col & 0xffffff00) | val;
                        else
                                gd5429->blt.fg_col = (gd5429->blt.fg_col & 0xff00) | val;
                        break;
                        case 0x05:
                        if (gd5429->type >= CL_TYPE_GD5434)
                                gd5429->blt.fg_col = (gd5429->blt.fg_col & 0xffff00ff) | (val << 8);
                        else
                                gd5429->blt.fg_col = (gd5429->blt.fg_col & 0x00ff) | (val << 8);
                        break;
                        case 0x06:
                        if (gd5429->type >= CL_TYPE_GD5434)
                                gd5429->blt.fg_col = (gd5429->blt.fg_col & 0xff00ffff) | (val << 16);
                        break;
                        case 0x07:
                        if (gd5429->type >= CL_TYPE_GD5434)
                                gd5429->blt.fg_col = (gd5429->blt.fg_col & 0x00ffffff) | (val << 24);
                        break;

                        case 0x08:
                        gd5429->blt.width = (gd5429->blt.width & 0xff00) | val;
                        break;
                        case 0x09:
                        gd5429->blt.width = (gd5429->blt.width & 0x00ff) | (val << 8);
                        if (gd5429->type >= CL_TYPE_GD5434)
                                gd5429->blt.width &= 0x1fff;
                        else
                                gd5429->blt.width &= 0x07ff;
                        break;
                        case 0x0a:
                        gd5429->blt.height = (gd5429->blt.height & 0xff00) | val;
                        break;
                        case 0x0b:
                        gd5429->blt.height = (gd5429->blt.height & 0x00ff) | (val << 8);
                        if (gd5429->type >= CL_TYPE_GD5434)
                            gd5429->blt.height &= 0x07ff;
                        else
                            gd5429->blt.height &= 0x03ff;
                        break;
                        case 0x0c:
                        gd5429->blt.dst_pitch = (gd5429->blt.dst_pitch & 0xff00) | val;
                        break;
                        case 0x0d:
                        gd5429->blt.dst_pitch = (gd5429->blt.dst_pitch & 0x00ff) | (val << 8);
                        break;
                        case 0x0e:
                        gd5429->blt.src_pitch = (gd5429->blt.src_pitch & 0xff00) | val;
                        break;
                        case 0x0f:
                        gd5429->blt.src_pitch = (gd5429->blt.src_pitch & 0x00ff) | (val << 8);
                        break;
                
                        case 0x10:
                        gd5429->blt.dst_addr = (gd5429->blt.dst_addr & 0xffff00) | val;
                        break;
                        case 0x11:
                        gd5429->blt.dst_addr = (gd5429->blt.dst_addr & 0xff00ff) | (val << 8);
                        break;
                        case 0x12:
                        gd5429->blt.dst_addr = (gd5429->blt.dst_addr & 0x00ffff) | (val << 16);
                        if (gd5429->type >= CL_TYPE_GD5434)
                                gd5429->blt.dst_addr &= 0x3fffff;
                        else
                                gd5429->blt.dst_addr &= 0x1fffff;
                        if (gd5429->blt.status & 0x80) {
                            gd5429->blt.status |= 2;
                            gd5429_start_blit(0, -1, gd5429);
                        }
                        break;

                        case 0x14:
                        gd5429->blt.src_addr = (gd5429->blt.src_addr & 0xffff00) | val;
                        break;
                        case 0x15:
                        gd5429->blt.src_addr = (gd5429->blt.src_addr & 0xff00ff) | (val << 8);
                        break;
                        case 0x16:
                        gd5429->blt.src_addr = (gd5429->blt.src_addr & 0x00ffff) | (val << 16);
                        if (gd5429->type >= CL_TYPE_GD5434)
                                gd5429->blt.src_addr &= 0x3fffff;
                        else
                                gd5429->blt.src_addr &= 0x1fffff;
                        break;

                        case 0x17:
                        gd5429->blt.mask = val;
                        break;
                        case 0x18:
                        gd5429->blt.mode = val;
                        if (gd5429->type >= CL_TYPE_GD5434)
                                gd5429->blt.depth = (val >> 4) & 3;
                        else
                                gd5429->blt.depth = (val >> 4) & 1;
                        break;
                
                        case 0x1a:
                        gd5429->blt.rop = val;
                        break;

                        case 0x1b: // blt mode extensions
                        if (gd5429->type >= CL_TYPE_GD5436) {
                            gd5429->blt.extensions = val & 7;
                        }
                        break;
                        
                        case 0x1c: // transparent blt key color byte 0
                        case 0x1d: // transparent blt key color byte 0
                        break;

                        case 0x40:
                        gd5429->blt.status &= ~(2 | 4 | 0x80);
                        if (gd5429->type < CL_TYPE_GD5436)
                            val &= 0x7f;
                        gd5429->blt.status |= val & (2 | 4 | 0x80);
                        gd5429_start_blit(0, -1, gd5429);
                        break;
                }
        }
        else if (gd5429->mmio_vram_overlap)
                gd5429_write(addr, val, gd5429);
}
static void gd5429_mmio_writew(uint32_t addr, uint16_t val, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;

        if ((addr & ~0xff) == 0xb8000)
        {
                gd5429_mmio_write(addr,     val & 0xff, gd5429);
                gd5429_mmio_write(addr + 1, val >> 8, gd5429);
        }
        else if (gd5429->mmio_vram_overlap)
                gd5429_writew(addr, val, gd5429);
}
static void gd5429_mmio_writel(uint32_t addr, uint32_t val, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;

        if ((addr & ~0xff) == 0xb8000)
        {
                gd5429_mmio_writew(addr,     val & 0xffff, gd5429);
                gd5429_mmio_writew(addr + 2, val >> 16, gd5429);
        }
        else if (gd5429->mmio_vram_overlap)
                gd5429_writel(addr, val, gd5429);
}

static uint8_t gd5429_mmio_read(uint32_t addr, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        
        if ((addr & ~0xff) == 0xb8000)
        {
//        pclog("MMIO read %08X\n", addr);
                switch (addr & 0xff)
                {
                        case 0x40: /*BLT status*/
                        return gd5429->blt.status;
                }
                return 0xff; /*All other registers read-only*/
        }
        if (gd5429->mmio_vram_overlap)
                return gd5429_read(addr, gd5429);
        return 0xff;
}
static uint16_t gd5429_mmio_readw(uint32_t addr, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        
        if ((addr & ~0xff) == 0xb8000)
                return gd5429_mmio_read(addr, gd5429) | (gd5429_mmio_read(addr+1, gd5429) << 8);
        if (gd5429->mmio_vram_overlap)
                return gd5429_readw(addr, gd5429);
        return 0xffff;
}
static uint32_t gd5429_mmio_readl(uint32_t addr, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        
        if ((addr & ~0xff) == 0xb8000)
                return gd5429_mmio_readw(addr, gd5429) | (gd5429_mmio_readw(addr+2, gd5429) << 16);
        if (gd5429->mmio_vram_overlap)
                return gd5429_readl(addr, gd5429);
        return 0xffffffff;
}

void gd5429_blt_write_w(uint32_t addr, uint16_t val, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;

//        pclog("gd5429_blt_write_w %08X %08X\n", addr, val);
        if (!gd5429->blt.mem_word_sel) {
            gd5429->blt.mem_word_save = val;
        } else {
            uint32_t v = gd5429->blt.mem_word_save | (val << 16);
            if ((gd5429->blt.mode & 0x84) == 0x84)
            {
                gd5429_start_blit(v & 0xff, 8, p);
                gd5429_start_blit((v >> 8) & 0xff, 8, p);
                gd5429_start_blit((v >> 16) & 0xff, 8, p);
                gd5429_start_blit((v >> 24) & 0xff, 8, p);
            } else {
                gd5429_start_blit(v, 32, p);
            }
        }
        gd5429->blt.mem_word_sel = !gd5429->blt.mem_word_sel;

}

void gd5429_blt_write_l(uint32_t addr, uint32_t val, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;

        gd5429->blt.mem_word_sel = 0;
//        pclog("gd5429_blt_write_l %08X %08X  %04X %04X\n", addr, val,  ((val >> 8) & 0x00ff) | ((val << 8) & 0xff00), ((val >> 24) & 0x00ff) | ((val >> 8) & 0xff00));
        if ((gd5429->blt.mode & 0x84) == 0x84)
        {
                gd5429_start_blit( val        & 0xff, 8, p);
                gd5429_start_blit((val >> 8)  & 0xff, 8, p);
                gd5429_start_blit((val >> 16) & 0xff, 8, p);
                gd5429_start_blit((val >> 24) & 0xff, 8, p);
        }
        else
                gd5429_start_blit(val, 32, p);
}

uint8_t ibm_gd5428_mca_read(int port, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;

//        pclog("gd5429_pro_mcv_read: port=%04x %02x %04x:%04x\n", port, gd5429->pos_regs[port & 7], CS, cpu_state.pc);

        return gd5429->pos_regs[port & 7];
}

static void ibm_gd5428_mapping_update(gd5429_t *gd5429)
{
        gd5429_recalc_mapping(gd5429);

        io_removehandler(0x03a0, 0x0023, gd5429_in, NULL, NULL, gd5429_out, NULL, NULL, gd5429);
        io_removehandler(0x03c4, 0x001c, gd5429_in, NULL, NULL, gd5429_out, NULL, NULL, gd5429);
        if ((gd5429->pos_regs[2] & 0x01) && gd5429->vidsys_ena)
        {
//                pclog("  GD5429 enable registers\n");
                io_sethandlerx(0x03c0, 0x0003, gd5429_in, NULL, NULL, gd5429_out, NULL, NULL, gd5429);
                io_sethandlerx(0x03c4, 0x001c, gd5429_in, NULL, NULL, gd5429_out, NULL, NULL, gd5429);
                if (!(gd5429->svga.miscout & 1))
                        io_sethandlerx(0x03a0, 0x0020, gd5429_in, NULL, NULL, gd5429_out, NULL, NULL, gd5429);
                gd5429->svga.override = 0;
                if (mb_vga)
                        svga_set_override(mb_vga, 1);
        }
        else
        {
//                pclog("  GD5429 disable registers\n");
                gd5429->svga.override = 1;
                if (mb_vga)
                        svga_set_override(mb_vga, 0);
        }
}

void ibm_gd5428_mca_write(int port, uint8_t val, void *p)
{
//        svga_set_override(voodoo->svga, val & 1);
        gd5429_t *gd5429 = (gd5429_t *)p;

//        pclog("gd5429_pro_mcv_write: port=%04x val=%02x\n", port, val);
        
        if (port < 0x102)
                return;
        gd5429->pos_regs[port & 7] = val;
        
        if ((port & 7) == 2)
        {
                mem_mapping_disablex(&gd5429->bios_rom.mapping);
                io_removehandler(0x03c3, 0x0001, gd5429_in, NULL, NULL, gd5429_out, NULL, NULL, gd5429);
                if (gd5429->pos_regs[2] & 0x01)
                {
//                        pclog("Enable BIOS mapping\n");
                        mem_mapping_enable(&gd5429->bios_rom.mapping);
                        io_sethandlerx(0x03c3, 0x0001, gd5429_in, NULL, NULL, gd5429_out, NULL, NULL, gd5429);
                }
//                else
//                        pclog("Disable BIOS mapping\n");
        }
        
        ibm_gd5428_mapping_update(gd5429);
}

static void ibm_gd5428_mca_reset(void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;

//        pclog("ibm_gd5428_mca_reset\n");
        gd5429->vidsys_ena = 0;
        ibm_gd5428_mca_write(0x102, 0, gd5429);
}

static uint8_t cl_pci_read(int func, int addr, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;

//        pclog("CL PCI read %08X\n", addr);
        switch (addr)
        {
                case 0x00: return 0x13; /*Cirrus Logic*/
                case 0x01: return 0x10;
                
                case 0x02:
                switch (gd5429->type)
                {
                        case CL_TYPE_GD5430:
                        return 0xa0;
                        case CL_TYPE_GD5434:
                        return 0xa8;
                        case CL_TYPE_GD5446:
                        case CL_TYPE_GD5446B:
                        return 0xb8;
                }
                return 0xff;
                case 0x03: return 0x00;
                
                case PCI_REG_COMMAND:
                return gd5429->pci_regs[PCI_REG_COMMAND]; /*Respond to IO and memory accesses*/

                case 0x07: return 0 << 1; /*Fast DEVSEL timing*/
                
                case 0x08: return 0; /*Revision ID*/
                case 0x09: return 0; /*Programming interface*/
                
                case 0x0a: return 0x00; /*Supports VGA interface*/
                case 0x0b: return 0x03;
                
                case 0x10: return 0x08; /*Linear frame buffer address*/
                case 0x11: return 0x00;
                case 0x12: return 0x00;
                case 0x13: return gd5429->lfb_base >> 24;

                case 0x14: return gd5429->mmio_base >> 0; /* mmio */
                case 0x15: return gd5429->mmio_base >> 8;
                case 0x16: return gd5429->mmio_base >> 16;
                case 0x17: return gd5429->mmio_base >> 24;

                case 0x18: return gd5429->gpio_base >> 0; /* gpio (revb) */
                case 0x19: return gd5429->gpio_base >> 8;
                case 0x1a: return gd5429->gpio_base >> 16;
                case 0x1b: return gd5429->gpio_base >> 24;

                case 0x30: return gd5429->pci_regs[0x30] & 0x01; /*BIOS ROM address*/
                case 0x31: return 0x00;
                case 0x32: return gd5429->pci_regs[0x32];
                case 0x33: return gd5429->pci_regs[0x33];
                
                case 0x3c: return gd5429->int_line;
                case 0x3d: return PCI_INTA;
        }
        return 0;
}

static void cl_pci_write(int func, int addr, uint8_t val, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;

//        pclog("cl_pci_write: addr=%02x val=%02x\n", addr, val);
        switch (addr)
        {
                case PCI_REG_COMMAND:
                gd5429->pci_regs[PCI_REG_COMMAND] = val & 0x23;
                io_removehandler(0x03c0, 0x0020, gd5429_in, NULL, NULL, gd5429_out, NULL, NULL, gd5429);
                if (val & PCI_COMMAND_IO)
                        io_sethandlerx(0x03c0, 0x0020, gd5429_in, NULL, NULL, gd5429_out, NULL, NULL, gd5429);
                gd5429_recalc_mapping(gd5429);
                break;
                
                case 0x13: 
                gd5429->lfb_base = val << 24;
                if (gd5429->type == CL_TYPE_GD5446B)
                    gd5429->lfb_base &= 0xfe000000;
                gd5429_recalc_mapping(gd5429); 
                break;                

#if 0
                case 0x14:
                gd5429->mmio_base &= 0xffffff00;
                if (gd5429->type < CL_TYPE_GD5446B) {
                    gd5429->mmio_base |= (val & (0x80|0x40|0x20)) << 0;
                }
                gd5429_recalc_mapping(gd5429);
                break;
                case 0x15:
                gd5429->mmio_base &= 0xffff0000;
                if (gd5429->type == CL_TYPE_GD5446B) {
                    gd5429->mmio_base |= (val & 0xf0) << 8;
                } else {
                    gd5429->mmio_base |= (val & 0xff) << 8;
                }
                gd5429_recalc_mapping(gd5429);
                break;
                case 0x16:
                gd5429->mmio_base &= 0xff00ff00;
                gd5429->mmio_base |= val << 16;
                gd5429_recalc_mapping(gd5429);
                break;
                case 0x17:
                gd5429->mmio_base &= 0x00ffff00;
                gd5429->mmio_base |= val << 24;
                gd5429_recalc_mapping(gd5429);
                break;

                case 0x18:
                gd5429->gpio_base &= 0xffffff00;
                if (gd5429->type >= CL_TYPE_GD5446B) {
                    gd5429->gpio_base |= (val & (0x80|0x40|0x20)) << 0;
                    gd5429_recalc_mapping(gd5429);
                }
                break;
                case 0x19:
                gd5429->gpio_base &= 0xffff0000;
                if (gd5429->type >= CL_TYPE_GD5446B) {
                    gd5429->gpio_base |= (val & 0xf0) << 8;
                }
                gd5429_recalc_mapping(gd5429);
                break;
                case 0x1a:
                gd5429->gpio_base &= 0xff00ff00;
                if (gd5429->type >= CL_TYPE_GD5446B) {
                    gd5429->gpio_base |= val << 16;
                    gd5429_recalc_mapping(gd5429);
                }
                break;
                case 0x1b:
                gd5429->gpio_base &= 0x00ffff00;
                if (gd5429->type >= CL_TYPE_GD5446B) {
                    gd5429->gpio_base |= val << 24;
                    gd5429_recalc_mapping(gd5429);
                }
                break;
#endif

                case 0x30: case 0x32: case 0x33:
                gd5429->pci_regs[addr] = val;
                if (gd5429->pci_regs[0x30] & 0x01)
                {
                        uint32_t addr = (gd5429->pci_regs[0x32] << 16) | (gd5429->pci_regs[0x33] << 24);
                        mem_mapping_set_addrx(&gd5429->bios_rom.mapping, addr, 0x8000);
                }
                else
                        mem_mapping_disablex(&gd5429->bios_rom.mapping);
                return;
                
                case 0x3c:
                gd5429->int_line = val;
                return;
        }
}

static void *cl_init(int type, char *fn, int pci_card, uint32_t force_vram_size)
{
        gd5429_t *gd5429 = (gd5429_t*)malloc(sizeof(gd5429_t));
        svga_t *svga = &gd5429->svga;
        int vram_size;
        memset(gd5429, 0, sizeof(gd5429_t));
        
        if (force_vram_size)
                vram_size = force_vram_size;
        else
                vram_size = device_get_config_int("memory");
        if (vram_size >= 256)
                gd5429->vram_mask = (vram_size << 10) - 1;
        else
                gd5429->vram_mask = (vram_size << 20) - 1;
        
        gd5429->type = type;

        if (fn)
                rom_init(&gd5429->bios_rom, fn, 0xc0000, 0x8000, 0x7fff, 0, MEM_MAPPING_EXTERNAL);
        
        svga_init(NULL, &gd5429->svga, gd5429, (vram_size >= 256) ? (vram_size << 10) : (vram_size << 20),
                   gd5429_recalctimings,
                   gd5429_in, gd5429_out,
                   gd5429_hwcursor_draw,
                   gd5429_overlay_draw);

        gd5429->svga.vsync_callback = gd5429_vblank_start;
        gd5429->svga.adjust_panning = gd5429_adjust_panning;

        mem_mapping_set_handlerx(&gd5429->svga.mapping, gd5429_read, gd5429_readw, gd5429_readl, gd5429_write, gd5429_writew, gd5429_writel);
        mem_mapping_set_px(&gd5429->svga.mapping, gd5429);

        mem_mapping_addx(&gd5429->mmio_mapping, 0, 0, gd5429_mmio_read, gd5429_mmio_readw, gd5429_mmio_readl, gd5429_mmio_write, gd5429_mmio_writew, gd5429_mmio_writel,  NULL, 0, gd5429);
        mem_mapping_addx(&gd5429->linear_mapping, 0, 0, gd5429_readb_linear, gd5429_readw_linear, gd5429_readl_linear, gd5429_writeb_linear, gd5429_writew_linear, gd5429_writel_linear,  NULL, 0, gd5429);

        io_sethandlerx(0x03c0, 0x0020, gd5429_in, NULL, NULL, gd5429_out, NULL, NULL, gd5429);
        if (type == CL_TYPE_AVGA2)
        {
                io_sethandlerx(0x0102, 0x0001, gd5429_in, NULL, NULL, gd5429_out, NULL, NULL, gd5429);
                io_sethandlerx(0x46e8, 0x0002, gd5429_in, NULL, NULL, gd5429_out, NULL, NULL, gd5429);
                svga->decode_mask = svga->vram_mask;
        }

        svga->hwcursor.yoff = 32;
        svga->hwcursor.xoff = 0;
        gd5429->hidden_dac_index = -1;
        gd5429->overlay.colorkeycompare = 0xff;

        gd5429->bank[1] = 0x8000;

        /*Default VCLK values*/
        svga->seqregs[0xb] = 0x66;
        svga->seqregs[0xc] = 0x5b;
        svga->seqregs[0xd] = 0x45;
        svga->seqregs[0xe] = 0x7e;
        svga->seqregs[0x1b] = 0x3b;
        svga->seqregs[0x1c] = 0x2f;
        svga->seqregs[0x1d] = 0x30;
        svga->seqregs[0x1e] = 0x33;

        if (type >= CL_TYPE_GD5446) {
            svga->crtcreg_mask = 0x7f;
        }

        if (PCI && type >= CL_TYPE_GD5430)
        {
                if (pci_card != -1)
                {
                        pci_add_specific(pci_card, cl_pci_read, cl_pci_write, gd5429);
                        gd5429->card = pci_card;
                }
                else
                        gd5429->card = pci_add(cl_pci_read, cl_pci_write, gd5429);

                gd5429->pci_regs[0x04] = 7;
        
                gd5429->pci_regs[0x30] = 0x00;
                gd5429->pci_regs[0x32] = 0x0c;
                gd5429->pci_regs[0x33] = 0x00;
        }

        gd5429->svga.fb_only = -1;
        gd5429->svga.fb_auto = 1;
        
        return gd5429;
}


static void *avga2_init(const device_t *info)
{
        return cl_init(CL_TYPE_AVGA2, "avga2vram.vbi", -1, 0);
}
static void *avga2_cbm_sl386sx_init(const device_t *info)
{
        return cl_init(CL_TYPE_AVGA2, "cbm_sl386sx25/c000.rom", -1, 0);
}
static void *gd5426_ps1_init(const device_t *info)
{
        return cl_init(CL_TYPE_GD5426, NULL, -1, 1);
}
static void *gd5426_init(const device_t *info)
{
    return cl_init(CL_TYPE_GD5426, "Picasso II", -1, 0);
}
static void *gd5426_swapped_init(const device_t *info)
{
    gd5429_t *gd5429 = (gd5429_t*)cl_init(CL_TYPE_GD5426, "Picasso II", -1, 0);
    gd5429->svga.swaprb = 1;
    return gd5429;
}
static void *gd5428_init(const device_t *info)
{
    return cl_init(CL_TYPE_GD5428, "Machspeed_VGA_GUI_2100_VLB.vbi", -1, 0);
}
static void *gd5428_swapped_init(const device_t *info)
{
    gd5429_t *gd5429 = (gd5429_t *)cl_init(CL_TYPE_GD5428, "Picasso II", -1, 0);
    gd5429->svga.swaprb = 1;
    return gd5429;
}
static void *ibm_gd5428_init(const device_t *info)
{
        gd5429_t *gd5429;
        svga_t *mb_vga = svga_get_pri();

        gd5429 = (gd5429_t*)cl_init(CL_TYPE_GD5428, "SVGA141.ROM", -1, 1); /*Only supports 1MB*/
        gd5429->mb_vga = mb_vga;
        
        mca_add(ibm_gd5428_mca_read, ibm_gd5428_mca_write, ibm_gd5428_mca_reset, gd5429);
        gd5429->pos_regs[0] = 0x7b;
        gd5429->pos_regs[1] = 0x91;
        gd5429->pos_regs[2] = 0;
        
        gd5429_recalc_mapping(gd5429);
        io_removehandler(0x03a0, 0x0040, gd5429_in, NULL, NULL, gd5429_out, NULL, NULL, gd5429);
        gd5429->svga.override = 1;
        mem_mapping_disablex(&gd5429->bios_rom.mapping);
        io_sethandlerx(0x46e8, 0x0001, gd5429_in, NULL, NULL, gd5429_out, NULL, NULL, gd5429);

        return gd5429;
}
static void *gd5429_init(const device_t *info)
{
        return cl_init(CL_TYPE_GD5429, "5429.vbi", -1, 0);
}
static void *gd5430_init(const device_t *info)
{
        return cl_init(CL_TYPE_GD5430, "gd5430/pci.bin", -1, 0);
}
static void *gd5430_pb570_init(const device_t *info)
{
        return cl_init(CL_TYPE_GD5430, "pb570/gd5430.bin", 8, 0);
}
static void *gd5434_init(const device_t *info)
{
        return cl_init(CL_TYPE_GD5434, "gd5434.bin", -1, 0);
}
static void *gd5434_vlb_swapped_init(const device_t *info)
{
    gd5429_t *gd5429 = (gd5429_t *)cl_init(CL_TYPE_GD5434, "CL", -1, 0);
    has_vlb = 1;
    gd5429->svga.swaprb = 1;
    return gd5429;
}
static void *gd5434_vlb_init(const device_t *info)
{
    gd5429_t *gd5429 = (gd5429_t *)cl_init(CL_TYPE_GD5434, "CL", -1, 0);
    has_vlb = 1;
    gd5429->svga.swaprb = 0;
    return gd5429;
}
static void *gd5446_init(const device_t *info)
{
    PCI = 1;
    gd5429_t *gd5429 = (gd5429_t *)cl_init(CL_TYPE_GD5446, "gd5446.bin", -1, 0);
    gd5429->int_line = 1;
    return gd5429;
}

static void *gd5434_pb520r_init(const device_t *info)
{
        return cl_init(CL_TYPE_GD5434, "pb520r/gd5434.bin", 3, 0);
}

static int avga2_available()
{
        return rom_present("avga2vram.vbi");
}
static int gd5428_available()
{
        return rom_present("Machspeed_VGA_GUI_2100_VLB.vbi");
}
static int ibm_gd5428_available()
{
        return rom_present(/*"FILE.ROM"*/"SVGA141.ROM");
}
static int gd5429_available()
{
        return rom_present("5429.vbi");
}
static int gd5430_available()
{
        return rom_present("gd5430/pci.bin");
}
static int gd5434_available()
{
        return rom_present("gd5434.bin");
}

void gd5429_close(void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;

        svga_close(&gd5429->svga);
        
        free(gd5429);
}

void gd5429_speed_changed(void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        
        svga_recalctimings(&gd5429->svga);
}

void gd5429_force_redraw(void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;

        gd5429->svga.fullchange = changeframecount;
}

void gd5429_add_status_info(char *s, int max_len, void *p)
{
        gd5429_t *gd5429 = (gd5429_t *)p;
        
        svga_add_status_info(s, max_len, &gd5429->svga);
}

static device_config_t avga2_config[] =
{
        {
                .name = "memory",
                .description = "Memory size",
                .type = CONFIG_SELECTION,
                .default_int = 512,
                .selection =
                {
                        {
                                .description = "256 kB",
                                .value = 256
                        },
                        {
                                .description = "512 kB",
                                .value = 512
                        },
                        {
                                .description = ""
                        }
                },
        },
        {
                .type = -1
        }
};

static device_config_t gd5429_config[] =
{
        {
                .name = "memory",
                .description = "Memory size",
                .type = CONFIG_SELECTION,
                .default_int = 2,
                .selection =
                {
                        {
                                .description = "1 MB",
                                .value = 1
                        },
                        {
                                .description = "2 MB",
                                .value = 2
                        },
                        {
                                .description = ""
                        }
                },
        },
        {
                .type = -1
        }
};
static device_config_t gd5434_config[] =
{
        {
                .name = "memory",
                .description = "Memory size",
                .type = CONFIG_SELECTION,
                .default_int = 4,
                .selection =
                {
                        {
                                .description = "2 MB",
                                .value = 2
                        },
                        {
                                .description = "4 MB",
                                .value = 4
                        },
                        {
                                .description = ""
                        }
                },
        },
        {
                .type = -1
        }
};

device_t avga2_device =
{
        "AVGA2 / Cirrus Logic GD5402", NULL,
        0, 0,
        avga2_init,
        gd5429_close,
        NULL,
        avga2_available,
        gd5429_speed_changed,
        gd5429_force_redraw,
        avga2_config
};

device_t avga2_cbm_sl386sx_device =
{
        "AVGA2 (Commodore SL386SX-25)", NULL,
        0, 0,
        avga2_cbm_sl386sx_init,
        gd5429_close,
        NULL,
        gd5430_available,
        gd5429_speed_changed,
        gd5429_force_redraw,
        avga2_config
};

device_t gd5426_ps1_device =
{
        "Cirrus Logic GD5426 (IBM PS/1)", NULL,
        0, 0,
        gd5426_ps1_init,
        gd5429_close,
        NULL,
        NULL,
        gd5429_speed_changed,
        gd5429_force_redraw,
        NULL
};

device_t gd5426_device =
{
        "Cirrus Logic GD5426", NULL,
        0, 0,
        gd5426_init,
        gd5429_close,
        NULL,
        gd5428_available,
        gd5429_speed_changed,
        gd5429_force_redraw,
        gd5429_config
};

device_t gd5426_swapped_device =
{
    "Cirrus Logic GD5426", NULL,
    0, 0,
    gd5426_swapped_init,
    gd5429_close,
    NULL,
    gd5428_available,
    gd5429_speed_changed,
    gd5429_force_redraw,
    gd5429_config
};

device_t gd5428_device =
{
        "Cirrus Logic GD5428", NULL,
        0, 0,
        gd5428_init,
        gd5429_close,
        NULL,
        gd5428_available,
        gd5429_speed_changed,
        gd5429_force_redraw,
        gd5429_config
};

device_t gd5428_swapped_device =
{
    "Cirrus Logic GD5428", NULL,
    0, 0,
    gd5428_swapped_init,
    gd5429_close,
        NULL,
    gd5428_available,
    gd5429_speed_changed,
    gd5429_force_redraw,
    gd5429_config
};


device_t ibm_gd5428_device =
{
        "IBM 1MB SVGA Adapter/A (Cirrus Logic GD5428)", NULL,
        DEVICE_MCA, 0,
        ibm_gd5428_init,
        gd5429_close,
        NULL,
        ibm_gd5428_available,
        gd5429_speed_changed,
        gd5429_force_redraw,
        NULL
};

device_t gd5429_device =
{
        "Cirrus Logic GD5429", NULL,
        0, 0,
        gd5429_init,
        gd5429_close,
        NULL,
        gd5429_available,
        gd5429_speed_changed,
        gd5429_force_redraw,
        gd5429_config
};

device_t gd5430_device =
{
        "Cirrus Logic GD5430", NULL,
        0, 0,
        gd5430_init,
        gd5429_close,
        NULL,
        gd5430_available,
        gd5429_speed_changed,
        gd5429_force_redraw
};

device_t gd5430_pb570_device =
{
        "Cirrus Logic GD5430 (PB570)", NULL,
        0, 0,
        gd5430_pb570_init,
        gd5429_close,
        NULL,
        gd5430_available,
        gd5429_speed_changed,
        gd5429_force_redraw
};

device_t gd5434_device =
{
        "Cirrus Logic GD5434", NULL,
        0, 0,
        gd5434_init,
        gd5429_close,
        NULL,
        gd5434_available,
        gd5429_speed_changed,
        gd5429_force_redraw
};

device_t gd5434_vlb_device =
{
    "Cirrus Logic GD5434", NULL,
    0, 0,
    gd5434_vlb_init,
    gd5429_close,
    NULL,
    gd5434_available,
    gd5429_speed_changed,
    gd5429_force_redraw
};

device_t gd5434_vlb_swapped_device =
{
    "Cirrus Logic GD5434", NULL,
    0, 0,
    gd5434_vlb_swapped_init,
    gd5429_close,
    NULL,
    gd5434_available,
    gd5429_speed_changed,
    gd5429_force_redraw
};

device_t gd5434_pb520r_device =
{
        "Cirrus Logic GD5434 (PB520r)", NULL,
        0, 0,
        gd5434_pb520r_init,
        gd5429_close,
        NULL,
        gd5434_available,
        gd5429_speed_changed,
        gd5429_force_redraw,
};

device_t gd5446_device =
{
    "Cirrus Logic GD5446", NULL,
    0, 0,
    gd5446_init,
    gd5429_close,
    NULL,
    gd5434_available,
    gd5429_speed_changed,
    gd5429_force_redraw
};
