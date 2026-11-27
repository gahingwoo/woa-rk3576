/*
 * vdpu383test - bring-up tool for the RK3576 video decoder (VDPU383) on
 * Windows, through vdpu383.sys.
 *
 *   vdpu383test info                      driver state, link and IOMMU registers,
 *                                         the warm-up result from D0 entry
 *   vdpu383test warmup                    run the RK3576 warm-up decode again
 *   vdpu383test warmup-direct             the warm-up picture through the normal
 *                                         (non-link) submit path
 *   vdpu383test regs [offset] [count]     read registers (byte offset, words)
 *   vdpu383test reset                     IP reset (link 0x44)
 *   vdpu383test decode IN.264 [options]   decode an Annex B file (H.264, or HEVC
 *                                         Main: told apart by the first NAL unit)
 *                                         or an IVF file (VP9 profile 0 or 2, AV1
 *                                         Main 8 or 10 bit)
 *   vdpu383test replay DIR/replay.txt     run one dumped picture again
 *   vdpu383test selftest                  checks that need no hardware
 *
 * decode options:
 *   -o FILE         write the output, I420, cropped (as the conformance .yuv)
 *   --ref FILE      compare every frame with this reference .yuv
 *   -n N            stop after N pictures
 *   --frame-md5     print an MD5 per output frame
 *   --dump DIR      write each picture's registers and buffers to DIR/pic_NNNN
 *   --readback      read all registers back after each picture
 *   --regs          print each picture's register file
 *   --dxva-only     give the builder only what DXVA carries (no profile_idc,
 *                   sps id; pps id read from the slice header), as a D3D11
 *                   driver would have to
 *   --stride mpp    MPP's stride (128-odd-plus-64) instead of mainline's (64)
 *   --link-vendor   link 0x54 = 0x7FFFFF and 0x58 = 0x01000000 (vendor kernel)
 *                   instead of mainline's (register 13, 0)
 *   --null          no device: parse and build registers only
 *   --session       with --null: decode as one of the driver's sessions
 *                   (vdpu383_check.h): its run rules and address check,
 *                   "session check" at the end
 *   --hevc, --h264  the codec, when the first NAL unit does not tell
 *   --vp9-mpp-intra, --vp9-mpp-update, --vp9-mpp
 *                   VP9: MPP's frame context handling instead of the
 *                   specification's where the two differ (see
 *                   lib/vdpu383_vp9.c): every intra frame resets all four
 *                   contexts; the adapted context is written to the frame's
 *                   context even when the frame does not keep it; both
 *
 * VP9: output is I420 (yuv420p), or with 16-bit samples (yuv420p10le) for
 * 10 bit, each frame at its own size, as FFmpeg writes them with
 * -autoscale 0. --dxva-only hands frame_context_idx on as FFmpeg and
 * Chromium do (0 on intra frames). Every frame's address registers are
 * checked against the buffers as the driver checks them, on --null too.
 *
 * AV1: output as for VP9. The decoder is handed each frame's OBUs as they
 * are in the file. --dxva-only gives the builder only DXVA_PicParams_AV1
 * and the tile data, as a D3D11 driver has them, and wraps the tiles in an
 * OBU_TILE_GROUP (--av1-synth 0, default) or in an OBU_FRAME with 16 zero
 * bytes of frame header (--av1-synth 1): which of the two the decoder
 * takes is for the board to tell.
 *
 * HEVC: every picture's reference sets are also derived from its slice
 * headers as the decoder derives them, once with the SPS's sets (the
 * parser's) and once with the table the builder rebuilds from DXVA alone,
 * and compared with what DXVA states ("rps check" at the end).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir_p(d) _mkdir(d)
static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
#else
#include <sys/stat.h>
#include <time.h>
#define mkdir_p(d) mkdir(d, 0755)
static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
#endif

#include "dev.h"
#include "../../drivers/video/vdpu383/vdpu383_check.h"

/* --session: decode as a session of the driver (dev_null_sessions) */
static int g_session_mode;
#include "h264parse.h"
#include "hevcparse.h"
#include "vp9parse.h"
#include "av1parse.h"
#include "md5.h"

/* ------------------------------------------------------------- helpers */

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    if (d && n > 0 && fread(d, 1, (size_t)n, f) != (size_t)n) {
        free(d);
        d = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return d;
}

static int write_file(const char *path, const void *d, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fwrite(d, 1, len, f);
    fclose(f);
    return 0;
}

static void md5_hex(const void *d, size_t len, char hex[33])
{
    Md5 m;
    md5_init(&m);
    md5_update(&m, d, len);
    md5_final(&m, hex);
}

static const char *run_result_name(unsigned r)
{
    switch (r) {
    case VDPU383_RUN_OK: return "ok";
    case VDPU383_RUN_TIMEOUT: return "TIMEOUT";
    case VDPU383_RUN_HW_ERROR: return "HW-ERROR";
    case VDPU383_RUN_BAD_ADDRESS: return "BAD-ADDRESS";
    case VDPU383_RUN_BUSY: return "BUSY";
    case VDPU383_RUN_MMU_FAULT: return "MMU-FAULT";
    default: return "?";
    }
}

static void print_status_bits(unsigned s)
{
    static const char *names[16] = {
        "frame_rdy", "strm_error", "core_timeout", "ip_timeout", "bus_error",
        "buffer_empty", "colmv_ref_error", "error_spread", "create_core_timeout",
        "wlast_miss_match", "core_rst_rdy", "ip_rst_rdy", "force_busidle_rdy",
        "ltb_pause_rdy", "ltb_end", "unsupport_decmode"
    };
    unsigned i, any = 0;
    for (i = 0; i < 16; i++)
        if (s & (1u << i)) {
            printf("%s%s", any ? "|" : "", names[i]);
            any = 1;
        }
    if (!any)
        printf("none");
}

/* --------------------------------------------------------------- info */

/* The driver's note of the last failed import (README.md), from its
 * Device Parameters; nothing printed when there is none. */
static void print_last_import(void)
{
#ifdef _WIN32
    static const wchar_t key[] = L"SYSTEM\\CurrentControlSet\\Enum\\ACPI\\RKCP0383\\0\\Device Parameters";
    static const wchar_t *const names[] = { L"LastImportStage", L"LastImportStatus", L"LastImportPages",
                                            L"LastImportExporterStatus", L"LastImportExporterPages",
                                            L"LastImportHandle", L"LastImportPid" };
    static const char *const stages[] = { "?", "parameters or IOMMU off", "no memory", "refresh of an unknown import",
                                          "no free IO addresses", "no exporter (maliwddm did not start it)",
                                          "exporter refused", "page count differs", "IOMMU entries" };
    DWORD v[7], i;

    for (i = 0; i < 7; i++) {
        DWORD n = sizeof(v[i]);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, names[i], RRF_RT_REG_DWORD, NULL, &v[i], &n) != ERROR_SUCCESS)
            return;
    }
    printf("last failed import  stage %lu (%s), status 0x%08lx; %lu pages asked, exporter 0x%08lx with %lu pages; "
           "handle %lu, process %lu\n", v[0], stages[v[0] < 9 ? v[0] : 0], v[1], v[2], v[3], v[4], v[5], v[6]);
#endif
}

static int cmd_info(void)
{
    VDPU383_INFO in;
    unsigned i;

    if (dev_info(&in)) {
        fprintf(stderr, "%s\n", dev_error());
        return 1;
    }
    printf("interface version   %u\n", in.interface_version);
    printf("version register    0x%08x  (0x27B00100)\n", in.reg_version);
    printf("link registers (0x27B00000):\n");
    for (i = 0; i < 24; i++)
        printf("  +0x%02x 0x%08x%s", i * 4, in.link[i], (i % 4 == 3) ? "\n" : "");
    printf("IOMMU 0x800: status 0x%08x dte 0x%08x rawstat 0x%08x  (paging %s)\n",
           in.mmu_status[0], in.mmu_dte[0], in.mmu_int_rawstat[0], (in.mmu_status[0] & 1) ? "ON" : "off");
    printf("IOMMU 0x900: status 0x%08x dte 0x%08x rawstat 0x%08x  (paging %s)\n",
           in.mmu_status[1], in.mmu_dte[1], in.mmu_int_rawstat[1], (in.mmu_status[1] & 1) ? "ON" : "off");
    printf("SRAM                %s 0x%08x size 0x%x\n", in.sram_present ? "yes" : "no", in.sram_dma, in.sram_size);
    printf("interrupts          decoder %u, IOMMU %u; last irq 0x%08x status 0x%08x\n",
           in.irq_count, in.mmu_irq_count, in.last_irq, in.last_status);
    printf("IOMMU last fault    status 0x%08x/0x%08x addr 0x%08x/0x%08x\n",
           in.mmu_last_status[0], in.mmu_last_status[1], in.mmu_last_fault_addr[0], in.mmu_last_fault_addr[1]);
    printf("runs %u, timeouts %u\n", in.runs, in.timeouts);
    if (in.interface_version >= 2)
        printf("buffers             %s (Iommu setting %u); directory 0x%08x, %u pages mapped, "
               "%u IOMMU faults, paging programmed %u times, %u failed allocations\n",
               in.iommu ? "IO addresses, IOMMU paging on" : "physical, contiguous", in.iommu_setting,
               in.iommu_dt_dma, in.iova_pages, in.mmu_faults, in.mmu_enables, in.alloc_failures);
    if (in.interface_version >= 3)
        printf("imports             %u made or refreshed, %u taken back by the display driver, %u failed\n",
               in.imports, in.revokes, in.import_failures);
    if (in.interface_version >= 3 && in.import_failures)
        print_last_import();
    if (in.interface_version >= 4)
        printf("sessions            %u open, %u made, %u decodes through them, %u refused; "
               "IOMMU directory switched %u times; UserAccess %u\n",
               in.sessions, in.sessions_made, in.session_runs, in.session_refused, in.domain_switches,
               in.user_access);
    printf("warm-up at D0 entry: ran %u timed_out %u irq 0x%08x status 0x%08x (", in.warmup.ran,
           in.warmup.timed_out, in.warmup.irq, in.warmup.status);
    print_status_bits(in.warmup.status & 0xFFFF);
    printf(") polls %u buffer 0x%08x\n", in.warmup.polls, in.warmup.buffer_dma);
    if (!in.warmup.ran)
        printf("warm-up at D0 entry: not run (Device Parameters WarmupAtD0 is 0, or an older driver); "
               "`vdpu383test warmup` runs it now\n");
    if (in.warmup.ran) {
        int ok = !in.warmup.timed_out && (in.warmup.status & V383_STA_FRAME_RDY) &&
                 !(in.warmup.status & V383_STA_ERROR_MASK);
        printf("warm-up verdict: %s\n", ok ? "PASS (frame_rdy, no error bits)" : "FAIL");
    }
    return 0;
}

static int cmd_warmup(void)
{
    VDPU383_WARMUP_RESULT r;
    int ok;

    if (dev_warmup(&r)) {
        fprintf(stderr, "%s\n", dev_error());
        return 1;
    }
    printf("warm-up: ran %u timed_out %u irq 0x%08x status 0x%08x (", r.ran, r.timed_out, r.irq, r.status);
    print_status_bits(r.status & 0xFFFF);
    printf(") polls %u\n", r.polls);
    ok = r.ran && !r.timed_out && (r.status & V383_STA_FRAME_RDY) && !(r.status & V383_STA_ERROR_MASK);
    printf("verdict: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 2;
}

static int cmd_regs(int argc, char **argv)
{
    unsigned off = 0, count = 0x400, i;
    unsigned *v;

    if (argc > 0)
        off = (unsigned)strtoul(argv[0], NULL, 0);
    if (argc > 1)
        count = (unsigned)strtoul(argv[1], NULL, 0);
    v = (unsigned *)calloc(count, 4);
    if (!v || dev_read_regs(off, count, v)) {
        fprintf(stderr, "%s\n", dev_error());
        return 1;
    }
    for (i = 0; i < count; i++)
        printf("0x%08x 0x%08x\n", 0x27B00000u + off + i * 4, v[i]);
    free(v);
    return 0;
}

static int cmd_reset(void)
{
    unsigned st;
    if (dev_reset(&st)) {
        fprintf(stderr, "%s\n", dev_error());
        return 1;
    }
    printf("IP reset: status after 0x%08x (ip_rst_rdy %s)\n", st, (st & V383_STA_IP_RST_RDY) ? "set" : "NOT set");
    return 0;
}

/* The warm-up picture of mpp_hack_rk3576.c, through IOCTL_VDPU383_RUN in
 * direct mode instead of the link-table mode the driver uses at D0. */
static const uint8_t warm_rlc[32] = {
    0x00, 0x00, 0x01, 0x65, 0x88, 0x82, 0x0b, 0x01, 0x2f, 0x08, 0xc5, 0x00, 0x01, 0x51, 0x78, 0xe0,
    0x00, 0x24, 0xf7, 0x1c, 0x00, 0x04, 0xcc, 0xeb, 0x89, 0xd7, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t warm_pps[28] = {
    0x40, 0x26, 0x00, 0x10, 0x04, 0x08, 0x00, 0x08, 0x80, 0x01, 0x00, 0x00, 0x00, 0x40,
    0x01, 0xd8, 0x07, 0x7c, 0x7a, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
};

static int cmd_warmup_direct(void)
{
    DevBuf b;
    VDPU383_RUN_IN *in;
    VDPU383_RUN_OUT out;
    char hex[33];
    unsigned i;

    if (dev_alloc(8192, &b)) {
        fprintf(stderr, "alloc: %s\n", dev_error());
        return 1;
    }
    memset(b.cpu, 0, 8192);
    memcpy(b.cpu + 0, warm_rlc, sizeof(warm_rlc));
    memcpy(b.cpu + 64, warm_pps, sizeof(warm_pps));

    in = (VDPU383_RUN_IN *)calloc(1, sizeof(*in));
    in->regs[8] = 0x00000001;
    in->regs[13] = 0x0000ffff;
    in->regs[16] = 0x00000101;
    in->regs[20] = 0xffffffff;
    in->regs[21] = 0x3ff3ffff;
    in->regs[64] = 0x76543210;
    in->regs[66] = 0x00000020;
    in->regs[67] = 0x000000a8;
    in->regs[68] = 0x00000002;
    in->regs[69] = 0x00000002;
    in->regs[70] = 0x00000040;
    in->regs[128] = b.dma + 0;
    in->regs[129] = b.dma + 320;
    in->regs[131] = b.dma + 64;
    in->regs[140] = b.dma + 576;
    in->regs[141] = 0x000000c0;
    in->regs[148] = b.dma + 832;
    in->regs[149] = 0x00000200;
    in->regs[168] = b.dma + 1344;
    in->regs[216] = b.dma + 2880;
    for (i = 8; i <= 31; i++) in->write_mask[i / 32] |= 1u << (i % 32);
    for (i = 64; i <= 107; i++) in->write_mask[i / 32] |= 1u << (i % 32);
    for (i = 128; i <= 235; i++) in->write_mask[i / 32] |= 1u << (i % 32);
    in->link_ip_timeout = 0x7ffff;
    in->link_ip_enable = 0;
    in->timeout_ms = 200;
    in->flags = VDPU383_RUN_READBACK;

    if (dev_run(in, &out)) {
        fprintf(stderr, "%s\n", dev_error());
        return 1;
    }
    md5_hex(b.cpu + 1344, 1536, hex);
    printf("warm-up (direct): %s irq 0x%08x status 0x%08x (", run_result_name(out.result), out.irq, out.status);
    print_status_bits(out.status & 0xFFFF);
    printf(") %u us\n", out.elapsed_us);
    printf("output 32x32 NV12 at +1344, md5 %s\n", hex);
    printf("register 15 after: 0x%08x\n", out.regs[15]);
    free(in);
    dev_free(&b);
    return (out.result == VDPU383_RUN_OK) ? 0 : 2;
}

/* -------------------------------------------------------------- decode */

typedef struct Dec {
    /* options */
    const char *out_path, *ref_path, *dump_dir;
    unsigned max_pictures;
    int frame_md5, readback, print_regs, dxva_only, stride_mpp, link_vendor;

    /* surfaces */
    DevBuf   surf[H264_MAX_SLOTS];
    V383_Surface vs[H264_MAX_SLOTS];
    unsigned width, height, hor, ver, surf_size, colmv_off;
    DevBuf   aux, rcb, strm;

    /* output */
    FILE    *out;
    uint8_t *ref;
    size_t   ref_len;
    unsigned crop_l, crop_r, crop_t, crop_b;
    uint8_t *frame;
    size_t   frame_size;
    unsigned out_count, mismatches, first_mismatch;
    Md5      total;

    unsigned pictures, hw_errors;
    double   hw_ms;

    /* HEVC */
    V383_HevcRpsTable rebuilt;
    unsigned rps_real_bad, rps_rebuilt_bad, rps_unsupported, rps_logged;

    /* VP9 */
    V383_Vp9State vp9;
    unsigned vp9_options, bit_depth, addr_bad;
    DevBuf   vp9aux, colmv[2];
    size_t   ref_off;

    /* AV1 */
    V383_Av1State av1;
    V383_Av1Surface as[AV1_MAX_SLOTS];
    unsigned av1_synth, av1_synth_hdr;
} Dec;

static unsigned popcount(unsigned v)
{
    unsigned n = 0;
    for (; v; v &= v - 1)
        n++;
    return n;
}

static unsigned align_up(unsigned v, unsigned a)
{
    return (v + a - 1) & ~(a - 1);
}

static unsigned mpp_stride(unsigned w)
{
    unsigned v = align_up(w, 64);
    if (((v - 64) % 256) == 128)
        return v;
    return (align_up(v, 128) | 128) + 64;
}

static void free_surfaces(Dec *d)
{
    unsigned i;
    for (i = 0; i < H264_MAX_SLOTS; i++)
        dev_free(&d->surf[i]);
    dev_free(&d->rcb);
}

static int setup_surfaces(Dec *d, const H264Picture *pic)
{
    unsigned i, colmv;

    if (d->width == pic->width && d->height == pic->height && d->surf[0].handle)
        return 0;
    free_surfaces(d);
    d->width = pic->width;
    d->height = pic->height;
    d->hor = d->stride_mpp ? mpp_stride(pic->width) : align_up(pic->width, 64);
    d->ver = align_up(pic->height, 16);
    colmv = v383_h264_colmv_size(pic->width, pic->height, V383_PP_GET(&pic->pp, V383_PP_FRAME_MBS_ONLY, 1));
    /* MPP sizes a frame buffer for 4:2:2 (SLOTS_LEN_ALIGN) whatever the format */
    d->colmv_off = align_up(d->hor * d->ver * 2, 4096);
    d->surf_size = d->colmv_off + align_up(colmv, 4096);
    for (i = 0; i < H264_MAX_SLOTS; i++) {
        if (dev_alloc(d->surf_size, &d->surf[i])) {
            fprintf(stderr, "surface alloc (%u bytes): %s\n", d->surf_size, dev_error());
            return -1;
        }
        memset(d->surf[i].cpu, 0, d->surf_size);
        d->vs[i].dma = d->surf[i].dma;
        d->vs[i].colmv_dma = d->surf[i].dma + d->colmv_off;
    }
    if (dev_alloc(v383_h264_rcb_size(pic->width, pic->height, 1) + 4096, &d->rcb)) {
        fprintf(stderr, "rcb alloc: %s\n", dev_error());
        return -1;
    }
    memset(d->rcb.cpu, 0, d->rcb.size);
    printf("# %ux%u, stride %u x %u, %u surfaces of %u bytes, colmv at +0x%x\n",
           pic->width, pic->height, d->hor, d->ver, H264_MAX_SLOTS, d->surf_size, d->colmv_off);
    return 0;
}

static void write_replay(Dec *d, const H264Picture *pic, const V383_RegFile *rf, const VDPU383_RUN_IN *in,
                         const char *out_md5)
{
    char dir[512], path[600];
    FILE *f;
    unsigned i, r, s;
    int used[H264_MAX_SLOTS] = { 0 };

    snprintf(dir, sizeof(dir), "%s/pic_%04u", d->dump_dir, pic->decode_index);
    mkdir_p(d->dump_dir);
    mkdir_p(dir);

    snprintf(path, sizeof(path), "%s/stream.bin", dir);
    write_file(path, d->strm.cpu, rf->regs[66]);
    snprintf(path, sizeof(path), "%s/aux.bin", dir);
    write_file(path, d->aux.cpu, V383_H264_AUX_SIZE);
    snprintf(path, sizeof(path), "%s/pp.bin", dir);
    write_file(path, &pic->pp, sizeof(pic->pp));
    snprintf(path, sizeof(path), "%s/qm.bin", dir);
    write_file(path, &pic->qm, sizeof(pic->qm));

    /* reference surfaces as they were before this decode is not known any
     * more; save them now (they are not written by the decode) */
    used[V383_PIC_INDEX(pic->pp.CurrPic)] = 1;
    for (i = 0; i < 16; i++)
        if (!V383_PIC_UNUSED(pic->pp.RefFrameList[i]))
            used[V383_PIC_INDEX(pic->pp.RefFrameList[i])] = 1;
    for (s = 0; s < H264_MAX_SLOTS; s++) {
        if (!used[s] || s == V383_PIC_INDEX(pic->pp.CurrPic))
            continue;
        snprintf(path, sizeof(path), "%s/surf%02u.bin", dir, s);
        write_file(path, d->surf[s].cpu, d->surf_size);
    }

    snprintf(path, sizeof(path), "%s/replay.txt", dir);
    f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "# vdpu383 replay v1: picture %u, poc %d, slices %u\n", pic->decode_index, pic->poc, pic->num_slices);
    fprintf(f, "buf strm %u stream.bin\n", d->strm.size);
    fprintf(f, "buf aux %u aux.bin\n", V383_H264_AUX_SIZE);
    fprintf(f, "buf rcb %u\n", d->rcb.size);
    for (s = 0; s < H264_MAX_SLOTS; s++) {
        if (!used[s])
            continue;
        if (s == V383_PIC_INDEX(pic->pp.CurrPic))
            fprintf(f, "buf surf%02u %u\n", s, d->surf_size);
        else
            fprintf(f, "buf surf%02u %u surf%02u.bin\n", s, d->surf_size, s);
    }
    fprintf(f, "link_ip_timeout 0x%x\nlink_ip_enable 0x%x\n", in->link_ip_timeout, in->link_ip_enable);
    for (r = 0; r < V383_NUM_REGS; r++) {
        const char *name = NULL;
        uint32_t base = 0, v = rf->regs[r];

        if (!(rf->write_mask[r / 32] & (1u << (r % 32))))
            continue;
        /* express addresses relative to a buffer */
        if (v >= d->strm.dma && v < d->strm.dma + d->strm.size) { name = "strm"; base = d->strm.dma; }
        else if (v >= d->aux.dma && v < d->aux.dma + d->aux.size) { name = "aux"; base = d->aux.dma; }
        else if (v >= d->rcb.dma && v < d->rcb.dma + d->rcb.size && r >= 140 && r <= 161 && !(r & 1)) { name = "rcb"; base = d->rcb.dma; }
        else {
            for (s = 0; s < H264_MAX_SLOTS && r >= 168; s++)
                if (used[s] && v >= d->surf[s].dma && v < d->surf[s].dma + d->surf_size) {
                    static char nm[16];
                    snprintf(nm, sizeof(nm), "surf%02u", s);
                    name = nm;
                    base = d->surf[s].dma;
                    break;
                }
        }
        if (name)
            fprintf(f, "reg %u @%s+0x%x\n", r, name, v - base);
        else
            fprintf(f, "reg %u 0x%08x\n", r, v);
    }
    if (out_md5)
        fprintf(f, "expect surf%02u 0 %u %s\n", V383_PIC_INDEX(pic->pp.CurrPic), d->hor * d->ver * 3 / 2, out_md5);
    fclose(f);
}

static int decode_cb(void *ctx, const H264Picture *pic)
{
    Dec *d = (Dec *)ctx;
    V383_H264Input bi;
    V383_RegFile rf;
    VDPU383_RUN_IN *in;
    VDPU383_RUN_OUT out;
    unsigned need, r;
    int rc;
    double t0;
    char hex[33];

    if (setup_surfaces(d, pic))
        return -1;
    d->crop_l = pic->crop_left;
    d->crop_r = pic->crop_right;
    d->crop_t = pic->crop_top;
    d->crop_b = pic->crop_bottom;

    need = align_up((unsigned)pic->stream_len, 16) + 128;
    if (need > d->strm.size) {
        dev_free(&d->strm);
        if (dev_alloc(need > (4u << 20) ? need : (4u << 20), &d->strm)) {
            fprintf(stderr, "stream alloc: %s\n", dev_error());
            return -1;
        }
    }
    memcpy(d->strm.cpu, pic->stream, pic->stream_len);
    memset(d->strm.cpu + pic->stream_len, 0, need - pic->stream_len);

    memset(&bi, 0, sizeof(bi));
    bi.pp = &pic->pp;
    bi.qm = &pic->qm;
    bi.scaling_matrix_present = pic->scaling_matrix_present;
    bi.extra = d->dxva_only ? NULL : &pic->extra;
    bi.pps_id = d->dxva_only ? V383_PPS_ID_FROM_STREAM : pic->pps_id;
    bi.stream_cpu = d->strm.cpu;
    bi.stream_dma = d->strm.dma;
    bi.stream_len = align_up((unsigned)pic->stream_len, 16);
    bi.aux_cpu = d->aux.cpu;
    bi.aux_dma = d->aux.dma;
    bi.rcb_dma = d->rcb.dma;
    bi.surfaces = d->vs;
    bi.num_surfaces = H264_MAX_SLOTS;
    bi.hor_stride = d->hor;
    bi.ver_stride = d->ver;

    rc = v383_h264_build(&bi, &rf);
    if (rc) {
        fprintf(stderr, "picture %u: register builder failed (%d)\n", pic->decode_index, rc);
        return -1;
    }
    if (d->dxva_only && v383_h264_parse_pps_id(pic->stream, (unsigned)pic->stream_len) != (int)pic->pps_id)
        fprintf(stderr, "picture %u: pps id from the slice header disagrees with the parser\n", pic->decode_index);

    in = (VDPU383_RUN_IN *)calloc(1, sizeof(*in));
    if (!in)
        return -1;
    memcpy(in->regs, rf.regs, sizeof(in->regs));
    memcpy(in->write_mask, rf.write_mask, sizeof(in->write_mask));
    in->link_ip_timeout = d->link_vendor ? 0x7FFFFF : rf.core_timeout;
    in->link_ip_enable = d->link_vendor ? 0x01000000 : 0;
    in->timeout_ms = 2000;
    in->flags = (d->readback ? VDPU383_RUN_READBACK : 0) | VDPU383_RUN_RESET_ON_ERROR;

    if (d->print_regs) {
        printf("# picture %u registers\n", pic->decode_index);
        for (r = 0; r < V383_NUM_REGS; r++)
            if (rf.write_mask[r / 32] & (1u << (r % 32)))
                printf("reg %3u 0x%08x\n", r, rf.regs[r]);
    }

    t0 = now_ms();
    if (dev_run(in, &out)) {
        fprintf(stderr, "picture %u: %s\n", pic->decode_index, dev_error());
        free(in);
        return -1;
    }
    d->hw_ms += now_ms() - t0;
    d->pictures++;

    hex[0] = 0;
    {
        uint8_t *o = d->surf[V383_PIC_INDEX(pic->pp.CurrPic)].cpu;
        md5_hex(o, (size_t)d->hor * d->ver * 3 / 2, hex);
    }
    printf("pic %4u slot %2u poc %5d %s%s%s slices %u refs %u  %s irq 0x%08x status 0x%08x (",
           pic->decode_index, V383_PIC_INDEX(pic->pp.CurrPic), pic->poc,
           (pic->slice_types & 4) ? "I" : "", (pic->slice_types & 1) ? "P" : "", (pic->slice_types & 2) ? "B" : "",
           pic->num_slices, popcount(pic->pp.UsedForReferenceFlags) / 2,
           run_result_name(out.result), out.irq, out.status);
    print_status_bits(out.status & 0xFFFF);
    printf(") %u us\n", out.elapsed_us);
    if (out.result != VDPU383_RUN_OK) {
        d->hw_errors++;
        if (out.result == VDPU383_RUN_BAD_ADDRESS)
            printf("    rejected: register %u = 0x%08x is not in a buffer of this process\n",
                   out.bad_reg, out.bad_reg < V383_NUM_REGS ? in->regs[out.bad_reg] : 0);
        if (out.result == VDPU383_RUN_MMU_FAULT)
            printf("    IOMMU fault at IO address 0x%08x\n", out.bad_reg);
        printf("    mmu status 0x%08x/0x%08x rawstat 0x%08x/0x%08x reset_status 0x%08x\n",
               out.mmu_status[0], out.mmu_status[1], out.mmu_int_rawstat[0], out.mmu_int_rawstat[1],
               out.reset_status);
    }
    if (d->readback && d->print_regs) {
        printf("# picture %u readback\n", pic->decode_index);
        for (r = 0; r < V383_NUM_REGS; r++)
            printf("rb %3u 0x%08x\n", r, out.regs[r]);
    }
    if (d->dump_dir)
        write_replay(d, pic, &rf, in, hex);
    free(in);
    return 0;
}

static void output_cb(void *ctx, int slot, unsigned decode_index, int poc)
{
    Dec *d = (Dec *)ctx;
    const uint8_t *s = d->surf[slot].cpu;
    unsigned w = d->width - d->crop_l - d->crop_r;
    unsigned h = d->height - d->crop_t - d->crop_b;
    unsigned x, y;
    uint8_t *p;
    char hex[33];
    size_t fsize = (size_t)w * h * 3 / 2;

    if (!s)
        return;
    if (d->frame_size != fsize) {
        free(d->frame);
        d->frame = (uint8_t *)malloc(fsize);
        d->frame_size = fsize;
    }
    p = d->frame;
    for (y = 0; y < h; y++, p += w)
        memcpy(p, s + (size_t)(y + d->crop_t) * d->hor + d->crop_l, w);
    /* NV12 -> I420 */
    {
        const uint8_t *c = s + (size_t)d->hor * d->ver;
        uint8_t *u = p, *v = p + (w / 2) * (h / 2);
        for (y = 0; y < h / 2; y++)
            for (x = 0; x < w / 2; x++) {
                const uint8_t *q = c + (size_t)(y + d->crop_t / 2) * d->hor + (x + d->crop_l / 2) * 2;
                u[y * (w / 2) + x] = q[0];
                v[y * (w / 2) + x] = q[1];
            }
    }
    md5_update(&d->total, d->frame, fsize);
    if (d->out)
        fwrite(d->frame, 1, fsize, d->out);
    if (d->frame_md5) {
        md5_hex(d->frame, fsize, hex);
        printf("frame %4u (pic %u, poc %d) md5 %s\n", d->out_count, decode_index, poc, hex);
    }
    if (d->ref) {
        size_t off = (size_t)d->out_count * fsize;
        if (off + fsize > d->ref_len) {
            if (!d->mismatches++)
                d->first_mismatch = d->out_count;
            printf("frame %4u: past the end of the reference\n", d->out_count);
        } else if (memcmp(d->ref + off, d->frame, fsize)) {
            size_t i, bad[3] = { 0, 0, 0 }, ysz = (size_t)w * h;
            for (i = 0; i < fsize; i++)
                if (d->ref[off + i] != d->frame[i])
                    bad[i < ysz ? 0 : (i < ysz + ysz / 4 ? 1 : 2)]++;
            if (!d->mismatches++)
                d->first_mismatch = d->out_count;
            printf("frame %4u (pic %u, poc %d): MISMATCH Y %zu U %zu V %zu bytes differ\n",
                   d->out_count, decode_index, poc, bad[0], bad[1], bad[2]);
        }
    }
    d->out_count++;
}

/* ---------------------------------------------------------------- HEVC */

static int setup_surfaces_hevc(Dec *d, const HevcPicture *pic)
{
    unsigned i, colmv, cb = 1u << (pic->pp.log2_min_luma_coding_block_size_minus3 + 3);

    if (d->width == pic->width && d->height == pic->height && d->surf[0].handle)
        return 0;
    free_surfaces(d);
    d->width = pic->width;
    d->height = pic->height;
    d->hor = d->stride_mpp ? mpp_stride(pic->width) : align_up(pic->width, 64);
    d->ver = align_up(pic->height, 16);
    colmv = v383_hevc_colmv_size(pic->width, pic->height, cb);
    d->colmv_off = align_up(d->hor * d->ver * 2, 4096);
    d->surf_size = d->colmv_off + align_up(colmv, 4096);
    for (i = 0; i < HEVC_MAX_SLOTS; i++) {
        if (dev_alloc(d->surf_size, &d->surf[i])) {
            fprintf(stderr, "surface alloc (%u bytes): %s\n", d->surf_size, dev_error());
            return -1;
        }
        memset(d->surf[i].cpu, 0, d->surf_size);
        d->vs[i].dma = d->surf[i].dma;
        d->vs[i].colmv_dma = d->surf[i].dma + d->colmv_off;
    }
    if (dev_alloc(v383_hevc_rcb_size(pic->width, pic->height) + 4096, &d->rcb)) {
        fprintf(stderr, "rcb alloc: %s\n", dev_error());
        return -1;
    }
    memset(d->rcb.cpu, 0, d->rcb.size);
    printf("# HEVC %ux%u, stride %u x %u, %u surfaces of %u bytes, colmv at +0x%x\n",
           pic->width, pic->height, d->hor, d->ver, HEVC_MAX_SLOTS, d->surf_size, d->colmv_off);
    return 0;
}

static void rps_checks(Dec *d, const HevcPicture *pic)
{
    char why[160];
    int rc;

    if (v383_hevc_check_rps(&pic->pp, pic->stream, (unsigned)pic->stream_len, &pic->extra.rps, why, sizeof(why))) {
        d->rps_real_bad++;
        if (d->rps_logged++ < 10)
            printf("    rps check, the SPS's sets: picture %u poc %d: %s\n", pic->decode_index, pic->poc, why);
    }
    rc = v383_hevc_rebuild_rps(&pic->pp, pic->stream, (unsigned)pic->stream_len, &d->rebuilt);
    if (rc) {
        d->rps_unsupported++;
        if (d->rps_logged++ < 10)
            printf("    rps check, rebuilt from DXVA: picture %u poc %d: no table (%d)\n", pic->decode_index,
                   pic->poc, rc);
    } else if (v383_hevc_check_rps(&pic->pp, pic->stream, (unsigned)pic->stream_len, &d->rebuilt, why, sizeof(why))) {
        d->rps_rebuilt_bad++;
        if (d->rps_logged++ < 10)
            printf("    rps check, rebuilt from DXVA: picture %u poc %d: %s\n", pic->decode_index, pic->poc, why);
    }
}

static int decode_cb_hevc(void *ctx, const HevcPicture *pic)
{
    Dec *d = (Dec *)ctx;
    V383_HevcInput bi;
    V383_RegFile rf;
    VDPU383_RUN_IN *in;
    VDPU383_RUN_OUT out;
    unsigned need, r, i, refs = 0;
    int rc;
    double t0;
    char hex[33];

    if (setup_surfaces_hevc(d, pic))
        return -1;
    d->crop_l = pic->crop_left;
    d->crop_r = pic->crop_right;
    d->crop_t = pic->crop_top;
    d->crop_b = pic->crop_bottom;

    need = v383_hevc_stream_read_len((unsigned)pic->stream_len);
    if (need > d->strm.size) {
        dev_free(&d->strm);
        if (dev_alloc(need > (4u << 20) ? need : (4u << 20), &d->strm)) {
            fprintf(stderr, "stream alloc: %s\n", dev_error());
            return -1;
        }
    }
    memcpy(d->strm.cpu, pic->stream, pic->stream_len);
    memset(d->strm.cpu + pic->stream_len, 0, need - pic->stream_len);

    rps_checks(d, pic);

    memset(&bi, 0, sizeof(bi));
    bi.pp = &pic->pp;
    bi.qm = &pic->qm;
    bi.extra = d->dxva_only ? NULL : &pic->extra;
    bi.stream_cpu = d->strm.cpu;
    bi.stream_dma = d->strm.dma;
    bi.stream_len = (unsigned)pic->stream_len;
    bi.aux_cpu = d->aux.cpu;
    bi.aux_dma = d->aux.dma;
    bi.rcb_dma = d->rcb.dma;
    bi.surfaces = d->vs;
    bi.num_surfaces = HEVC_MAX_SLOTS;
    bi.hor_stride = d->hor;
    bi.ver_stride = d->ver;
    bi.rebuild_scratch = &d->rebuilt;

    rc = v383_hevc_build(&bi, &rf);
    if (rc) {
        fprintf(stderr, "picture %u: register builder failed (%d)\n", pic->decode_index, rc);
        return -1;
    }
    if (d->dxva_only && v383_hevc_parse_pps_id(&pic->pp, pic->stream, (unsigned)pic->stream_len) !=
                            (int)pic->extra.pps_id)
        fprintf(stderr, "picture %u: pps id from the slice header disagrees with the parser\n", pic->decode_index);

    in = (VDPU383_RUN_IN *)calloc(1, sizeof(*in));
    if (!in)
        return -1;
    memcpy(in->regs, rf.regs, sizeof(in->regs));
    memcpy(in->write_mask, rf.write_mask, sizeof(in->write_mask));
    in->link_ip_timeout = d->link_vendor ? 0x7FFFFF : rf.core_timeout;
    in->link_ip_enable = d->link_vendor ? 0x01000000 : 0;
    in->timeout_ms = 2000;
    in->flags = (d->readback ? VDPU383_RUN_READBACK : 0) | VDPU383_RUN_RESET_ON_ERROR;

    if (d->print_regs) {
        printf("# picture %u registers\n", pic->decode_index);
        for (r = 0; r < V383_NUM_REGS; r++)
            if (rf.write_mask[r / 32] & (1u << (r % 32)))
                printf("reg %3u 0x%08x\n", r, rf.regs[r]);
    }

    t0 = now_ms();
    if (dev_run(in, &out)) {
        fprintf(stderr, "picture %u: %s\n", pic->decode_index, dev_error());
        free(in);
        return -1;
    }
    d->hw_ms += now_ms() - t0;
    d->pictures++;

    md5_hex(d->surf[V383_PIC_INDEX(pic->pp.CurrPic)].cpu, (size_t)d->hor * d->ver * 3 / 2, hex);
    for (i = 0; i < 15; i++)
        if (pic->pp.RefPicList[i].bPicEntry != 0xFF)
            refs++;
    printf("pic %4u slot %2u poc %5d nal %2u %s%s%s slices %u refs %u  %s irq 0x%08x status 0x%08x (",
           pic->decode_index, V383_PIC_INDEX(pic->pp.CurrPic), pic->poc, pic->nal_type,
           (pic->slice_types & 4) ? "I" : "", (pic->slice_types & 2) ? "P" : "", (pic->slice_types & 1) ? "B" : "",
           pic->num_slices, refs, run_result_name(out.result), out.irq, out.status);
    print_status_bits(out.status & 0xFFFF);
    printf(") %u us\n", out.elapsed_us);
    if (out.result != VDPU383_RUN_OK) {
        d->hw_errors++;
        if (out.result == VDPU383_RUN_BAD_ADDRESS)
            printf("    rejected: register %u = 0x%08x is not in a buffer of this process\n",
                   out.bad_reg, out.bad_reg < V383_NUM_REGS ? in->regs[out.bad_reg] : 0);
        if (out.result == VDPU383_RUN_MMU_FAULT)
            printf("    IOMMU fault at IO address 0x%08x\n", out.bad_reg);
        printf("    mmu status 0x%08x/0x%08x rawstat 0x%08x/0x%08x reset_status 0x%08x\n",
               out.mmu_status[0], out.mmu_status[1], out.mmu_int_rawstat[0], out.mmu_int_rawstat[1],
               out.reset_status);
    }
    if (d->readback && d->print_regs) {
        printf("# picture %u readback\n", pic->decode_index);
        for (r = 0; r < V383_NUM_REGS; r++)
            printf("rb %3u 0x%08x\n", r, out.regs[r]);
    }
    free(in);
    return 0;
}

/* ----------------------------------------------------------------- VP9 */

static void free_vp9(Dec *d)
{
    dev_free(&d->vp9aux);
    dev_free(&d->colmv[0]);
    dev_free(&d->colmv[1]);
}

/* Surfaces for the largest frame of the stream (all frames share them, as
 * a D3D11 decoder's texture array does), 64-aligned in both directions. */
static int setup_surfaces_vp9(Dec *d, unsigned width, unsigned height, unsigned bit_depth)
{
    unsigned i, bytes = (width * bit_depth + 7) / 8, colmv = v383_vp9_colmv_size(width, height);

    free_surfaces(d);
    free_vp9(d);
    d->width = width;
    d->height = height;
    d->bit_depth = bit_depth;
    d->hor = d->stride_mpp ? mpp_stride(bytes) : align_up(bytes, 64);
    d->ver = align_up(height, 64);
    d->surf_size = align_up(d->hor * d->ver * 3 / 2, 4096);
    for (i = 0; i < VP9_MAX_SLOTS; i++) {
        if (dev_alloc(d->surf_size, &d->surf[i])) {
            fprintf(stderr, "surface alloc (%u bytes): %s\n", d->surf_size, dev_error());
            return -1;
        }
        memset(d->surf[i].cpu, 0, d->surf_size);
        d->vs[i].dma = d->surf[i].dma;
        d->vs[i].colmv_dma = 0;
    }
    if (dev_alloc(v383_vp9_rcb_size(width, height) + 4096, &d->rcb) ||
        dev_alloc(V383_VP9_AUX_SIZE, &d->vp9aux) ||
        dev_alloc(colmv, &d->colmv[0]) || dev_alloc(colmv, &d->colmv[1])) {
        fprintf(stderr, "buffer alloc: %s\n", dev_error());
        return -1;
    }
    memset(d->rcb.cpu, 0, d->rcb.size);
    memset(d->vp9aux.cpu, 0, d->vp9aux.size);
    memset(d->colmv[0].cpu, 0, d->colmv[0].size);
    memset(d->colmv[1].cpu, 0, d->colmv[1].size);
    printf("# VP9 up to %ux%u, %u bit, stride %u x %u, %u surfaces of %u bytes, colmv 2 x %u\n",
           width, height, bit_depth, d->hor, d->ver, VP9_MAX_SLOTS, d->surf_size, colmv);
    return 0;
}

/* The driver's check (V383IsAddrReg/V383AddrOk), on the null device too:
 * every address register written must point into one of this decode's
 * buffers, and what the decoder reads from the stream must be in it. */
static int vp9_in_buf(const DevBuf *b, uint32_t a, uint32_t len)
{
    return b->handle && a >= b->dma && (uint64_t)a + len <= (uint64_t)b->dma + b->size;
}

static unsigned check_vp9_addrs(Dec *d, const V383_RegFile *rf, char *why, size_t why_len)
{
    unsigned r, i, bad = 0;

    for (r = 0; r < V383_NUM_REGS; r++) {
        uint32_t a = rf->regs[r];
        int ok = 0, addr = 0;

        if (!(rf->write_mask[r / 32] & (1u << (r % 32))))
            continue;
        addr = (r >= 128 && r <= 134) || (r >= 140 && r <= 160 && !(r & 1)) || (r >= 168 && r <= 185) ||
               r == 192 || (r >= 194 && r <= 210) || (r >= 216 && r <= 232);
        if (!addr || !a)
            continue;
        ok = vp9_in_buf(&d->strm, a, 1) || vp9_in_buf(&d->rcb, a, 1) || vp9_in_buf(&d->vp9aux, a, 1) ||
             vp9_in_buf(&d->colmv[0], a, 1) || vp9_in_buf(&d->colmv[1], a, 1);
        for (i = 0; i < VP9_MAX_SLOTS && !ok; i++)
            ok = vp9_in_buf(&d->surf[i], a, 1);
        if (!ok) {
            if (!bad++)
                snprintf(why, why_len, "register %u = 0x%08x is in no buffer", r, a);
        }
    }
    if (!vp9_in_buf(&d->strm, rf->regs[128], rf->regs[66])) {
        if (!bad++)
            snprintf(why, why_len, "stream read 0x%08x + %u past the buffer", rf->regs[128], rf->regs[66]);
    }
    {
        unsigned last = 0;
        for (i = 0; i < 11; i++)
            if (rf->regs[141 + 2 * i])
                last = rf->regs[140 + 2 * i] + rf->regs[141 + 2 * i];
        if (last && !vp9_in_buf(&d->rcb, d->rcb.dma, last - d->rcb.dma)) {
            if (!bad++)
                snprintf(why, why_len, "row buffers end at 0x%08x, past the buffer", last);
        }
    }
    return bad;
}

static int decode_cb_vp9(void *ctx, const Vp9Picture *pic)
{
    Dec *d = (Dec *)ctx;
    const V383_DXVA_PicParams_VP9 *pp = &pic->pp;
    V383_Vp9Input bi;
    V383_RegFile rf;
    VDPU383_RUN_IN *in;
    VDPU383_RUN_OUT out;
    unsigned need, r, cur = V383_PIC_INDEX(pp->CurrPic);
    unsigned intra = !V383_VP9_FRAME_TYPE(pp) || V383_VP9_INTRA_ONLY(pp);
    int rc;
    double t0;
    char hex[33], why[160];

    need = (pp->uncompressed_header_size_byte_aligned & ~15u) + v383_vp9_stream_read_len((unsigned)pic->len) + 16;
    if (need > d->strm.size) {
        dev_free(&d->strm);
        if (dev_alloc(need > (4u << 20) ? need : (4u << 20), &d->strm)) {
            fprintf(stderr, "stream alloc: %s\n", dev_error());
            return -1;
        }
    }
    memcpy(d->strm.cpu, pic->data, pic->len);
    memset(d->strm.cpu + pic->len, 0, need - pic->len);

    memset(&bi, 0, sizeof(bi));
    bi.pp = pp;
    bi.state = &d->vp9;
    bi.options = d->vp9_options;
    bi.stream_dma = d->strm.dma;
    bi.stream_len = (unsigned)pic->len;
    bi.aux_cpu = d->vp9aux.cpu;
    bi.aux_dma = d->vp9aux.dma;
    bi.rcb_dma = d->rcb.dma;
    bi.colmv_dma[0] = d->colmv[0].dma;
    bi.colmv_dma[1] = d->colmv[1].dma;
    bi.surfaces = d->vs;
    bi.num_surfaces = VP9_MAX_SLOTS;
    bi.hor_stride = d->hor;
    bi.ver_stride = d->ver;

    rc = v383_vp9_build(&bi, &rf);
    if (rc) {
        fprintf(stderr, "frame %u: register builder failed (%d)\n", pic->decode_index, rc);
        return -1;
    }
    if (check_vp9_addrs(d, &rf, why, sizeof(why))) {
        d->addr_bad++;
        printf("    frame %u: %s\n", pic->decode_index, why);
    }

    in = (VDPU383_RUN_IN *)calloc(1, sizeof(*in));
    if (!in)
        return -1;
    memcpy(in->regs, rf.regs, sizeof(in->regs));
    memcpy(in->write_mask, rf.write_mask, sizeof(in->write_mask));
    in->link_ip_timeout = d->link_vendor ? 0x7FFFFF : rf.core_timeout;
    in->link_ip_enable = d->link_vendor ? 0x01000000 : 0;
    in->timeout_ms = 2000;
    in->flags = (d->readback ? VDPU383_RUN_READBACK : 0) | VDPU383_RUN_RESET_ON_ERROR;

    if (d->print_regs) {
        printf("# frame %u registers\n", pic->decode_index);
        for (r = 0; r < V383_NUM_REGS; r++)
            if (rf.write_mask[r / 32] & (1u << (r % 32)))
                printf("reg %3u 0x%08x\n", r, rf.regs[r]);
    }

    t0 = now_ms();
    if (dev_run(in, &out)) {
        fprintf(stderr, "frame %u: %s\n", pic->decode_index, dev_error());
        free(in);
        return -1;
    }
    d->hw_ms += now_ms() - t0;
    d->pictures++;

    md5_hex(d->surf[cur].cpu, (size_t)d->hor * d->ver * 3 / 2, hex);
    printf("pic %4u slot %2u %ux%u %s%s%s q %3d ctx %u%s refresh 0x%02x %u bytes (headers %u + %u)  %s irq 0x%08x status 0x%08x (",
           pic->decode_index, cur, pp->width, pp->height,
           !V383_VP9_FRAME_TYPE(pp) ? "K" : (intra ? "I" : "P"), V383_VP9_SHOW_FRAME(pp) ? "" : " hidden",
           V383_VP9_ERROR_RESILIENT(pp) ? " er" : "", pp->base_qindex, pic->coded_ctx_idx,
           V383_VP9_REFRESH_CTX(pp) ? (V383_VP9_PARALLEL_MODE(pp) ? "fw" : "bw") : "--",
           pic->refresh_frame_flags, (unsigned)pic->len, pp->uncompressed_header_size_byte_aligned,
           pp->first_partition_size, run_result_name(out.result), out.irq, out.status);
    print_status_bits(out.status & 0xFFFF);
    printf(") %u us\n", out.elapsed_us);
    if (out.result != VDPU383_RUN_OK) {
        d->hw_errors++;
        if (out.result == VDPU383_RUN_BAD_ADDRESS)
            printf("    rejected: register %u = 0x%08x is not in a buffer of this process\n",
                   out.bad_reg, out.bad_reg < V383_NUM_REGS ? in->regs[out.bad_reg] : 0);
        if (out.result == VDPU383_RUN_MMU_FAULT)
            printf("    IOMMU fault at IO address 0x%08x\n", out.bad_reg);
        printf("    mmu status 0x%08x/0x%08x rawstat 0x%08x/0x%08x reset_status 0x%08x\n",
               out.mmu_status[0], out.mmu_status[1], out.mmu_int_rawstat[0], out.mmu_int_rawstat[1],
               out.reset_status);
    }
    if (d->readback && d->print_regs) {
        printf("# frame %u readback\n", pic->decode_index);
        for (r = 0; r < V383_NUM_REGS; r++)
            printf("rb %3u 0x%08x\n", r, out.regs[r]);
    }
    free(in);
    return 0;
}

/* One sample of a surface row: 8 bit, or the decoder's 10-bit packing
 * (4 samples in 5 bytes, least significant bits first). */
static unsigned vp9_sample(const uint8_t *row, unsigned x, unsigned bit_depth)
{
    unsigned bit, v;

    if (bit_depth == 8)
        return row[x];
    bit = x * 10;
    v = row[bit >> 3] | ((unsigned)row[(bit >> 3) + 1] << 8);
    return (v >> (bit & 7)) & 0x3FF;
}

/* A shown frame: cropped to its own size, I420 (8 bit) or I420 with 16-bit
 * little-endian samples (10 bit), as FFmpeg writes yuv420p / yuv420p10le. */
static void output_cb_vp9(void *ctx, int slot, unsigned w, unsigned h, unsigned decode_index, int existing)
{
    Dec *d = (Dec *)ctx;
    const uint8_t *s = d->surf[slot].cpu;
    unsigned cw = (w + 1) / 2, ch = (h + 1) / 2, bps = d->bit_depth > 8 ? 2 : 1;
    size_t fsize = ((size_t)w * h + 2 * (size_t)cw * ch) * bps;
    unsigned x, y, pl;
    uint8_t *p;
    char hex[33];

    if (!s)
        return;
    if (d->frame_size < fsize) {
        free(d->frame);
        d->frame = (uint8_t *)malloc(fsize);
        d->frame_size = fsize;
    }
    p = d->frame;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            unsigned v = vp9_sample(s + (size_t)y * d->hor, x, d->bit_depth);
            *p++ = (uint8_t)v;
            if (bps == 2)
                *p++ = (uint8_t)(v >> 8);
        }
    for (pl = 0; pl < 2; pl++)
        for (y = 0; y < ch; y++)
            for (x = 0; x < cw; x++) {
                unsigned v = vp9_sample(s + (size_t)d->hor * d->ver + (size_t)y * d->hor, 2 * x + pl, d->bit_depth);
                *p++ = (uint8_t)v;
                if (bps == 2)
                    *p++ = (uint8_t)(v >> 8);
            }
    md5_update(&d->total, d->frame, fsize);
    if (d->out)
        fwrite(d->frame, 1, fsize, d->out);
    if (d->frame_md5) {
        md5_hex(d->frame, fsize, hex);
        printf("frame %4u (pic %u%s) %ux%u md5 %s\n", d->out_count, decode_index,
               existing ? ", shown again" : "", w, h, hex);
    }
    if (d->ref) {
        if (d->ref_off + fsize > d->ref_len) {
            if (!d->mismatches++)
                d->first_mismatch = d->out_count;
            printf("frame %4u: past the end of the reference\n", d->out_count);
        } else if (memcmp(d->ref + d->ref_off, d->frame, fsize)) {
            size_t i, bad[3] = { 0, 0, 0 }, ysz = (size_t)w * h * bps;
            for (i = 0; i < fsize; i++)
                if (d->ref[d->ref_off + i] != d->frame[i])
                    bad[i < ysz ? 0 : (i < ysz + (size_t)cw * ch * bps ? 1 : 2)]++;
            if (!d->mismatches++)
                d->first_mismatch = d->out_count;
            printf("frame %4u (pic %u): MISMATCH Y %zu U %zu V %zu bytes differ\n",
                   d->out_count, decode_index, bad[0], bad[1], bad[2]);
        }
        d->ref_off += fsize;
    }
    d->out_count++;
}

static int run_vp9(Dec *d, const uint8_t *data, size_t len)
{
    Vp9Parser *p = (Vp9Parser *)calloc(1, sizeof(*p));
    int rc;

    if (!p)
        return -1;
    /* pass 1: the largest frame and the bit depth, for the surfaces */
    vp9_init(p, NULL, NULL);
    rc = vp9_run(p, data, len, d->max_pictures, NULL, NULL);
    if (rc) {
        fprintf(stderr, "error: %s\n", p->err);
        free(p);
        return -1;
    }
    if (setup_surfaces_vp9(d, p->max_width, p->max_height, p->max_bit_depth)) {
        free(p);
        return -1;
    }
    v383_vp9_reset(&d->vp9);
    vp9_init(p, output_cb_vp9, d);
    p->dxva_only = d->dxva_only;
    rc = vp9_run(p, data, len, d->max_pictures, decode_cb_vp9, d);
    if (rc)
        fprintf(stderr, "error: %s\n", p->err);
    printf("vp9: %u packets, %u superframes, %u frames decoded (%u key, %u intra-only, %u hidden), "
           "%u shown again, %u size changes, %u scaled references, %u dropped (no reference)\n",
           p->packets, p->superframes, p->decode_index, p->keyframes, p->intra_only, p->hidden, p->existing,
           p->resized, p->scaled_refs, p->dropped);
    printf("address check: %u frames with an address outside the buffers\n", d->addr_bad);
    free(p);
    return rc;
}

/* ----------------------------------------------------------------- AV1 */

/* Surfaces for the largest frame, each with its co-located motion vectors
 * and its CDF and segment map buffer behind the picture. */
static int setup_surfaces_av1(Dec *d, unsigned width, unsigned height, unsigned bit_depth)
{
    unsigned i, bytes = (width * bit_depth + 7) / 8;
    unsigned colmv = v383_av1_colmv_size(width, height), cdf = v383_av1_cdf_size(width, height), pic;

    free_surfaces(d);
    free_vp9(d);
    d->width = width;
    d->height = height;
    d->bit_depth = bit_depth;
    d->hor = d->stride_mpp ? mpp_stride(bytes) : align_up(bytes, 64);
    d->ver = align_up(height, 64);
    pic = align_up(d->hor * d->ver * 3 / 2, 4096);
    d->colmv_off = pic;
    d->surf_size = pic + colmv + cdf;
    for (i = 0; i < AV1_MAX_SLOTS; i++) {
        if (dev_alloc(d->surf_size, &d->surf[i])) {
            fprintf(stderr, "surface alloc (%u bytes): %s\n", d->surf_size, dev_error());
            return -1;
        }
        memset(d->surf[i].cpu, 0, d->surf_size);
        d->as[i].dma = d->surf[i].dma;
        d->as[i].colmv_dma = d->surf[i].dma + pic;
        d->as[i].cdf_dma = d->surf[i].dma + pic + colmv;
    }
    if (dev_alloc(v383_av1_aux_size(width, height), &d->vp9aux)) {
        fprintf(stderr, "buffer alloc: %s\n", dev_error());
        return -1;
    }
    memset(d->vp9aux.cpu, 0, d->vp9aux.size);
    printf("# AV1 up to %ux%u, %u bit, stride %u x %u, %u surfaces of %u bytes (colmv %u, cdf %u)\n",
           width, height, bit_depth, d->hor, d->ver, AV1_MAX_SLOTS, d->surf_size, colmv, cdf);
    return 0;
}

static unsigned check_av1_addrs(Dec *d, const V383_RegFile *rf, char *why, size_t why_len)
{
    unsigned r, i, bad = 0, last = 0;

    for (r = 0; r < V383_NUM_REGS; r++) {
        uint32_t a = rf->regs[r];
        int ok, addr;

        if (!(rf->write_mask[r / 32] & (1u << (r % 32))))
            continue;
        addr = (r >= 128 && r <= 134) || (r >= 140 && r <= 160 && !(r & 1)) || (r >= 168 && r <= 185) ||
               r == 192 || (r >= 194 && r <= 210) || (r >= 216 && r <= 232);
        if (!addr || !a)
            continue;
        ok = vp9_in_buf(&d->strm, a, 1) || vp9_in_buf(&d->rcb, a, 1) || vp9_in_buf(&d->vp9aux, a, 1);
        for (i = 0; i < AV1_MAX_SLOTS && !ok; i++)
            ok = vp9_in_buf(&d->surf[i], a, 1);
        if (!ok && !bad++)
            snprintf(why, why_len, "register %u = 0x%08x is in no buffer", r, a);
    }
    if (!vp9_in_buf(&d->strm, rf->regs[128], rf->regs[66]) && !bad++)
        snprintf(why, why_len, "stream read 0x%08x + %u past the buffer", rf->regs[128], rf->regs[66]);
    for (i = 0; i < 11; i++)
        if (rf->regs[141 + 2 * i])
            last = rf->regs[140 + 2 * i] + rf->regs[141 + 2 * i];
    if (last && !vp9_in_buf(&d->rcb, d->rcb.dma, last - d->rcb.dma) && !bad++)
        snprintf(why, why_len, "row buffers end at 0x%08x, past the buffer", last);
    /* the CDF reads: default tables plus a segment map, or a surface's */
    if (rf->regs[181] && !vp9_in_buf(&d->vp9aux, rf->regs[181], 1)) {
        for (i = 0; i < AV1_MAX_SLOTS; i++)
            if (vp9_in_buf(&d->surf[i], rf->regs[181], 1))
                break;
        if (i == AV1_MAX_SLOTS && !bad++)
            snprintf(why, why_len, "segment map read 0x%08x in no buffer", rf->regs[181]);
    }
    return bad;
}

static int decode_cb_av1(void *ctx, const Av1Picture *pic)
{
    Dec *d = (Dec *)ctx;
    const V383_DXVA_PicParams_AV1 *pp = &pic->pp;
    V383_Av1Input bi;
    V383_RegFile rf;
    VDPU383_RUN_IN *in;
    VDPU383_RUN_OUT out;
    unsigned need, r, len, cur = pp->CurrPicTextureIndex, ft = V383_AV1_FRAME_TYPE(pp);
    int rc;
    double t0;
    char hex[33], why[160];
    static const char *types[4] = { "K", "P", "I", "S" };

    if (d->dxva_only) {
        unsigned tiles = 0, i;
        for (i = 0; i < pic->num_tiles; i++)
            tiles += pic->tiles[i].DataSize + 4;
        need = v383_av1_stream_read_len(tiles + 16 + d->av1_synth_hdr) + 64;
    } else {
        need = v383_av1_stream_read_len((unsigned)pic->obu_len) + 64;
    }
    if (need > d->strm.size) {
        dev_free(&d->strm);
        if (dev_alloc(need > (4u << 20) ? need : (4u << 20), &d->strm)) {
            fprintf(stderr, "stream alloc: %s\n", dev_error());
            return -1;
        }
    }
    memset(d->strm.cpu, 0, need);
    if (d->dxva_only) {
        len = v383_av1_synth_stream(pp, pic->tiles, pic->num_tiles, pic->tile_data, d->av1_synth,
                                    d->av1_synth_hdr, d->strm.cpu, d->strm.size);
        if (!len) {
            fprintf(stderr, "frame %u: no room for the stream\n", pic->decode_index);
            return -1;
        }
    } else {
        memcpy(d->strm.cpu, pic->obu, pic->obu_len);
        len = (unsigned)pic->obu_len;
    }

    memset(&bi, 0, sizeof(bi));
    bi.pp = pp;
    bi.extra = d->dxva_only ? NULL : &pic->extra;
    bi.state = &d->av1;
    bi.stream_dma = d->strm.dma;
    bi.stream_len = len;
    bi.tile_size_bytes = 4;
    bi.frame_header_bits = d->av1_synth ? d->av1_synth_hdr * 8 : 0;
    bi.aux_cpu = d->vp9aux.cpu;
    bi.aux_dma = d->vp9aux.dma;
    bi.surfaces = d->as;
    bi.num_surfaces = AV1_MAX_SLOTS;
    bi.hor_stride = d->hor;
    bi.ver_stride = d->ver;
    if (d->rcb.size < v383_av1_rcb_size(pp) + 4096) {
        dev_free(&d->rcb);
        if (dev_alloc(v383_av1_rcb_size(pp) + 4096, &d->rcb)) {
            fprintf(stderr, "rcb alloc: %s\n", dev_error());
            return -1;
        }
        memset(d->rcb.cpu, 0, d->rcb.size);
    }
    bi.rcb_dma = d->rcb.dma;

    rc = v383_av1_build(&bi, &rf);
    if (rc) {
        fprintf(stderr, "frame %u: register builder failed (%d)\n", pic->decode_index, rc);
        return -1;
    }
    if (check_av1_addrs(d, &rf, why, sizeof(why))) {
        d->addr_bad++;
        printf("    frame %u: %s\n", pic->decode_index, why);
    }

    in = (VDPU383_RUN_IN *)calloc(1, sizeof(*in));
    if (!in)
        return -1;
    memcpy(in->regs, rf.regs, sizeof(in->regs));
    memcpy(in->write_mask, rf.write_mask, sizeof(in->write_mask));
    in->link_ip_timeout = d->link_vendor ? 0x7FFFFF : rf.core_timeout;
    in->link_ip_enable = d->link_vendor ? 0x01000000 : 0;
    in->timeout_ms = 2000;
    in->flags = (d->readback ? VDPU383_RUN_READBACK : 0) | VDPU383_RUN_RESET_ON_ERROR;
    if (d->print_regs) {
        printf("# frame %u registers\n", pic->decode_index);
        for (r = 0; r < V383_NUM_REGS; r++)
            if (rf.write_mask[r / 32] & (1u << (r % 32)))
                printf("reg %3u 0x%08x\n", r, rf.regs[r]);
    }
    t0 = now_ms();
    if (dev_run(in, &out)) {
        fprintf(stderr, "frame %u: %s\n", pic->decode_index, dev_error());
        free(in);
        return -1;
    }
    d->hw_ms += now_ms() - t0;
    d->pictures++;

    md5_hex(d->surf[cur].cpu, (size_t)d->hor * d->ver * 3 / 2, hex);
    printf("pic %4u slot %2u %ux%u %s%s q %3u oh %3u prim %u refresh 0x%02x tiles %u %u bytes (header %u bits)  "
           "%s irq 0x%08x status 0x%08x (",
           pic->decode_index, cur, pp->width, pp->height, types[ft & 3], V383_AV1_SHOW_FRAME(pp) ? "" : " hidden",
           pp->quantization.base_qindex, pp->order_hint, pp->primary_ref_frame, pic->refresh_frame_flags,
           pic->num_tiles, len, pic->frame_header_bits, run_result_name(out.result), out.irq, out.status);
    print_status_bits(out.status & 0xFFFF);
    printf(") %u us\n", out.elapsed_us);
    if (out.result != VDPU383_RUN_OK) {
        d->hw_errors++;
        if (out.result == VDPU383_RUN_BAD_ADDRESS)
            printf("    rejected: register %u = 0x%08x is not in a buffer of this process\n",
                   out.bad_reg, out.bad_reg < V383_NUM_REGS ? in->regs[out.bad_reg] : 0);
        if (out.result == VDPU383_RUN_MMU_FAULT)
            printf("    IOMMU fault at IO address 0x%08x\n", out.bad_reg);
        printf("    mmu status 0x%08x/0x%08x rawstat 0x%08x/0x%08x reset_status 0x%08x\n",
               out.mmu_status[0], out.mmu_status[1], out.mmu_int_rawstat[0], out.mmu_int_rawstat[1],
               out.reset_status);
    }
    if (d->readback && d->print_regs) {
        printf("# frame %u readback\n", pic->decode_index);
        for (r = 0; r < V383_NUM_REGS; r++)
            printf("rb %3u 0x%08x\n", r, out.regs[r]);
    }
    free(in);
    return 0;
}

static int run_av1(Dec *d, const uint8_t *data, size_t len)
{
    Av1Parser *p = (Av1Parser *)calloc(1, sizeof(*p));
    int rc;

    if (!p)
        return -1;
    av1_init(p, NULL, NULL);
    rc = av1_run(p, data, len, d->max_pictures, NULL, NULL);
    if (rc) {
        fprintf(stderr, "error: %s\n", p->err);
        av1_free(p);
        free(p);
        return -1;
    }
    if (setup_surfaces_av1(d, p->max_width, p->max_height, p->max_bit_depth)) {
        av1_free(p);
        free(p);
        return -1;
    }
    v383_av1_reset(&d->av1);
    av1_init(p, output_cb_vp9, d);
    rc = av1_run(p, data, len, d->max_pictures, decode_cb_av1, d);
    if (rc)
        fprintf(stderr, "error: %s\n", p->err);
    printf("av1: %u temporal units, %u frames decoded (%u key, %u intra-only, %u switch, %u hidden), "
           "%u shown again, %u lower layers not shown, %u superres, %u with film grain, %u with loop restoration, "
           "%u scaled references, %u short ref signalling, up to %u tiles\n",
           p->packets, p->decode_index, p->keyframes, p->intra_only, p->switch_frames, p->frames_hidden,
           p->existing, p->dropped_layers, p->superres, p->grain_frames, p->lr_frames, p->scaled_refs,
           p->short_signaling, p->tiles_max);
    printf("address check: %u frames with an address outside the buffers\n", d->addr_bad);
    av1_free(p);
    free(p);
    return rc;
}

static int cmd_decode(int argc, char **argv)
{
    Dec d;
    H264Parser p;
    HevcParser *hp = NULL;
    int codec = 0;              /* 0 by the stream, 1 H.264, 2 HEVC, 3 VP9, 4 AV1 */
    uint8_t *data;
    size_t len;
    const char *in_path = NULL;
    int i, rc;
    char hex[33];

    memset(&d, 0, sizeof(d));
    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) d.out_path = argv[++i];
        else if (!strcmp(argv[i], "--ref") && i + 1 < argc) d.ref_path = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) d.max_pictures = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) d.dump_dir = argv[++i];
        else if (!strcmp(argv[i], "--frame-md5")) d.frame_md5 = 1;
        else if (!strcmp(argv[i], "--readback")) d.readback = 1;
        else if (!strcmp(argv[i], "--regs")) d.print_regs = 1;
        else if (!strcmp(argv[i], "--dxva-only")) d.dxva_only = 1;
        else if (!strcmp(argv[i], "--stride") && i + 1 < argc) d.stride_mpp = !strcmp(argv[++i], "mpp");
        else if (!strcmp(argv[i], "--link-vendor")) d.link_vendor = 1;
        else if (!strcmp(argv[i], "--null") || !strcmp(argv[i], "--session")) ;
        else if (!strcmp(argv[i], "--hevc")) codec = 2;
        else if (!strcmp(argv[i], "--h264")) codec = 1;
        else if (!strcmp(argv[i], "--av1-synth") && i + 1 < argc) {
            d.av1_synth = (unsigned)atoi(argv[++i]);
            d.av1_synth_hdr = d.av1_synth ? 16 : 0;
        }
        else if (!strcmp(argv[i], "--vp9-mpp-intra")) d.vp9_options |= V383_VP9_OPT_MPP_INTRA_RESET;
        else if (!strcmp(argv[i], "--vp9-mpp-update")) d.vp9_options |= V383_VP9_OPT_MPP_UPDATE_BUF;
        else if (!strcmp(argv[i], "--vp9-mpp")) d.vp9_options |= V383_VP9_OPT_MPP_INTRA_RESET | V383_VP9_OPT_MPP_UPDATE_BUF;
        else if (argv[i][0] != '-' && !in_path) in_path = argv[i];
        else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 1;
        }
    }
    if (!in_path) {
        fprintf(stderr, "decode: no input file\n");
        return 1;
    }
    data = read_file(in_path, &len);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", in_path);
        return 1;
    }
    if (d.ref_path) {
        d.ref = read_file(d.ref_path, &d.ref_len);
        if (!d.ref) {
            fprintf(stderr, "cannot read %s\n", d.ref_path);
            return 1;
        }
    }
    if (d.out_path) {
        d.out = fopen(d.out_path, "wb");
        if (!d.out) {
            fprintf(stderr, "cannot write %s\n", d.out_path);
            return 1;
        }
    }
    if (dev_alloc(V383_H264_AUX_SIZE, &d.aux)) {
        fprintf(stderr, "aux alloc: %s\n", dev_error());
        return 1;
    }
    md5_init(&d.total);
    if (!codec)
        codec = av1_probe(data, len) ? 4 : vp9_probe(data, len) ? 3 : hevc_probe(data, len) ? 2 : 1;
    if (codec == 4) {
        if (d.dump_dir)
            fprintf(stderr, "--dump: H.264 only, ignored\n");
        d.dump_dir = NULL;
        rc = run_av1(&d, data, len);
    } else if (codec == 3) {
        if (d.dump_dir)
            fprintf(stderr, "--dump: H.264 only, ignored\n");
        d.dump_dir = NULL;
        rc = run_vp9(&d, data, len);
    } else if (codec == 2) {
        if (d.dump_dir)
            fprintf(stderr, "--dump: H.264 only, ignored\n");
        d.dump_dir = NULL;
        hp = (HevcParser *)calloc(1, sizeof(*hp));
        if (!hp)
            return 1;
        hevc_init(hp, output_cb, &d);
        rc = hevc_run(hp, data, len, d.max_pictures, decode_cb_hevc, &d);
        if (rc)
            fprintf(stderr, "error: %s\n", hp->err);
        if (hp->skipped)
            printf("RASL pictures skipped (their IRAP starts the stream): %u\n", hp->skipped);
        printf("rps check: %u pictures; with the SPS's sets %u differ; rebuilt from DXVA %u differ, %u had no table\n",
               d.pictures, d.rps_real_bad, d.rps_rebuilt_bad, d.rps_unsupported);
    } else {
        h264_init(&p, output_cb, &d);
        rc = h264_run(&p, data, len, d.max_pictures, decode_cb, &d);
        if (rc)
            fprintf(stderr, "error: %s\n", p.err);
    }
    md5_final(&d.total, hex);
    printf("pictures %u, frames out %u, hardware errors %u, %.1f ms in the decoder calls\n",
           d.pictures, d.out_count, d.hw_errors, d.hw_ms);
    printf("output md5 %s\n", hex);
    if (d.ref && codec >= 3) {
        printf("reference: %s", (d.mismatches || d.ref_off != d.ref_len) ? "MISMATCH" : "MATCH");
        if (d.mismatches)
            printf(", %u frames differ, first at output frame %u", d.mismatches, d.first_mismatch);
        else if (d.ref_off != d.ref_len)
            printf(" (but the reference has %zu bytes more)", d.ref_len - d.ref_off);
        printf("\n");
    } else if (d.ref) {
        size_t fs = d.frame_size ? d.frame_size : 1;
        printf("reference: %zu frames; %s", d.ref_len / fs, d.mismatches ? "MISMATCH" : "MATCH");
        if (d.mismatches)
            printf(", %u frames differ, first at output frame %u", d.mismatches, d.first_mismatch);
        else if (d.out_count != d.ref_len / fs)
            printf(" (but frame counts differ)");
        printf("\n");
    }
    if (hp) {
        hevc_free(hp);
        free(hp);
    } else if (codec == 1) {
        h264_free(&p);
    }
    if (d.out)
        fclose(d.out);
    free_surfaces(&d);
    free_vp9(&d);
    dev_free(&d.aux);
    dev_free(&d.strm);
    free(data);
    free(d.ref);
    free(d.frame);
    if (dev_is_null() && g_session_mode)
        printf("session check: %u decodes refused\n", dev_null_refused());
    return (rc || d.hw_errors || d.mismatches || d.rps_real_bad || d.rps_rebuilt_bad || d.addr_bad ||
            (d.ref && codec >= 3 && d.ref_off != d.ref_len) ||
            (d.dxva_only && d.rps_unsupported)) ? 2 : 0;
}

/* -------------------------------------------------------------- replay */

#define MAX_BUFS 32

static int cmd_replay(const char *path)
{
    char dir[512], line[512], name[64], file[256], full[800];
    struct { char name[64]; DevBuf b; } bufs[MAX_BUFS];
    unsigned nbufs = 0, i;
    VDPU383_RUN_IN *in;
    VDPU383_RUN_OUT out;
    FILE *f;
    const char *slash;
    int rc = 0, expects = 0, expect_fail = 0;

    slash = strrchr(path, '/');
    if (!slash)
        slash = strrchr(path, '\\');
    if (slash) {
        size_t n = (size_t)(slash - path);
        memcpy(dir, path, n);
        dir[n] = 0;
    } else {
        strcpy(dir, ".");
    }
    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    in = (VDPU383_RUN_IN *)calloc(1, sizeof(*in));
    in->timeout_ms = 2000;
    in->flags = VDPU383_RUN_READBACK | VDPU383_RUN_RESET_ON_ERROR;
    memset(bufs, 0, sizeof(bufs));

    /* pass 1: buffers and registers */
    while (fgets(line, sizeof(line), f)) {
        unsigned size, reg, off;
        if (line[0] == '#' || line[0] == '\n')
            continue;
        file[0] = 0;
        if (sscanf(line, "buf %63s %u %255s", name, &size, file) >= 2) {
            size_t flen;
            uint8_t *d;
            if (nbufs >= MAX_BUFS || dev_alloc(size, &bufs[nbufs].b)) {
                fprintf(stderr, "buffer %s: %s\n", name, dev_error());
                rc = 1;
                goto out;
            }
            snprintf(bufs[nbufs].name, sizeof(bufs[nbufs].name), "%s", name);
            memset(bufs[nbufs].b.cpu, 0, bufs[nbufs].b.size);
            if (file[0]) {
                snprintf(full, sizeof(full), "%s/%s", dir, file);
                d = read_file(full, &flen);
                if (!d) {
                    fprintf(stderr, "cannot read %s\n", full);
                    rc = 1;
                    goto out;
                }
                memcpy(bufs[nbufs].b.cpu, d, flen < bufs[nbufs].b.size ? flen : bufs[nbufs].b.size);
                free(d);
            }
            nbufs++;
        } else if (sscanf(line, "reg %u @%63[^+]+0x%x", &reg, name, &off) == 3) {
            for (i = 0; i < nbufs; i++)
                if (!strcmp(bufs[i].name, name))
                    break;
            if (i == nbufs || reg >= V383_NUM_REGS) {
                fprintf(stderr, "bad line: %s", line);
                rc = 1;
                goto out;
            }
            in->regs[reg] = bufs[i].b.dma + off;
            in->write_mask[reg / 32] |= 1u << (reg % 32);
        } else if (sscanf(line, "reg %u 0x%x", &reg, &off) == 2 && reg < V383_NUM_REGS) {
            in->regs[reg] = off;
            in->write_mask[reg / 32] |= 1u << (reg % 32);
        } else if (sscanf(line, "link_ip_timeout 0x%x", &off) == 1) {
            in->link_ip_timeout = off;
        } else if (sscanf(line, "link_ip_enable 0x%x", &off) == 1) {
            in->link_ip_enable = off;
        }
    }

    if (dev_run(in, &out)) {
        fprintf(stderr, "%s\n", dev_error());
        rc = 1;
        goto out;
    }
    printf("replay: %s irq 0x%08x status 0x%08x (", run_result_name(out.result), out.irq, out.status);
    print_status_bits(out.status & 0xFFFF);
    printf(") %u us\n", out.elapsed_us);

    /* pass 2: checks */
    rewind(f);
    while (fgets(line, sizeof(line), f)) {
        unsigned off, len;
        char want[64];
        if (sscanf(line, "expect %63s %u %u %63s", name, &off, &len, want) == 4) {
            char hex[33];
            for (i = 0; i < nbufs; i++)
                if (!strcmp(bufs[i].name, name))
                    break;
            if (i == nbufs || off + len > bufs[i].b.size)
                continue;
            md5_hex(bufs[i].b.cpu + off, len, hex);
            expects++;
            if (strcmp(hex, want)) {
                expect_fail++;
                printf("  %s md5 %s, expected %s: MISMATCH\n", name, hex, want);
            } else {
                printf("  %s md5 %s: MATCH\n", name, hex);
            }
        }
    }
    if (out.result != VDPU383_RUN_OK || expect_fail)
        rc = 2;
    if (!expects)
        printf("  (no expect lines)\n");
out:
    fclose(f);
    for (i = 0; i < nbufs; i++)
        dev_free(&bufs[i].b);
    free(in);
    return rc;
}

/* ------------------------------------------------------------ selftest */

/* an inter frame with most of the record's fields set */
static void av1_kat_pp(V383_DXVA_PicParams_AV1 *pp, V383_Av1State *s)
{
    unsigned i, j;

    memset(pp, 0, sizeof(*pp));
    v383_av1_reset(s);
    for (i = 0; i < 8; i++) {
        V383_Av1SurfState *t = &s->surf[i];
        t->valid = 1;
        t->frame_type = (unsigned char)(i ? 1 : 0);
        t->order_hint = (unsigned char)(10 + 3 * i);
        for (j = 0; j < 7; j++)
            t->ref_hints[j] = (unsigned char)(i + j);
        t->width = t->upscaled_width = 1280;
        t->height = 720;
        t->cdf_valid = 1;
        pp->RefFrameMapTextureIndex[i] = (unsigned char)i;
    }
    s->surf[5].width = 960;
    s->frames = 9;
    pp->width = 1280;
    pp->height = 720;
    pp->max_width = 1920;
    pp->max_height = 1080;
    pp->CurrPicTextureIndex = 8;
    pp->superres_denom = 8;
    pp->bitdepth = 8;
    pp->tiles.cols = 4;
    pp->tiles.rows = 2;
    pp->tiles.context_update_id = 5;
    for (i = 0; i < 4; i++)
        pp->tiles.widths[i] = 5;
    pp->tiles.heights[0] = 6;
    pp->tiles.heights[1] = 6;
    pp->CodingParamToolFlags = 0x7FFFFFFu & ~(1u << 12) & ~(1u << 21) & ~(1u << 11);
    pp->FormatAndPictureInfoFlags = 1 | (1 << 2) | (1 << 4) | (1 << 5);
    pp->primary_ref_frame = 2;
    pp->order_hint = 30;
    pp->order_hint_bits = 7;
    for (i = 0; i < 7; i++) {
        pp->frame_refs[i].Index = (unsigned char)((i * 3) % 8);
        pp->frame_refs[i].width = 1280;
        pp->frame_refs[i].height = 720;
        for (j = 0; j < 6; j++)
            pp->frame_refs[i].wmmat[j] = (j % 3 == 2) ? 1 << 16 : 0;
    }
    pp->frame_refs[0].GlobalMotionFlags = 2 << 1;                /* ROTZOOM */
    pp->frame_refs[0].wmmat[2] = (1 << 16) + 2 * 2;
    pp->frame_refs[0].wmmat[3] = -6;
    pp->frame_refs[0].wmmat[4] = 6;
    pp->frame_refs[0].wmmat[5] = (1 << 16) + 4;
    pp->frame_refs[0].wmmat[0] = 64 * 1024;
    pp->frame_refs[0].wmmat[1] = -3 * 1024;
    pp->frame_refs[1].GlobalMotionFlags = 1 | (0 << 1);
    pp->loop_filter.filter_level[0] = 20;
    pp->loop_filter.filter_level[1] = 18;
    pp->loop_filter.filter_level_u = 9;
    pp->loop_filter.filter_level_v = 7;
    pp->loop_filter.sharpness_level = 3;
    pp->loop_filter.ControlFlags = 0xF;
    pp->loop_filter.ref_deltas[0] = 1;
    pp->loop_filter.ref_deltas[4] = -1;
    pp->loop_filter.ref_deltas[6] = -1;
    pp->loop_filter.ref_deltas[7] = -1;
    pp->loop_filter.mode_deltas[1] = 2;
    pp->loop_filter.delta_lf_res = 1;
    pp->loop_filter.frame_restoration_type[0] = 3;
    pp->loop_filter.frame_restoration_type[1] = 1;
    pp->loop_filter.log2_restoration_unit_size[0] = 7;
    pp->loop_filter.log2_restoration_unit_size[1] = 6;
    pp->loop_filter.log2_restoration_unit_size[2] = 6;
    pp->quantization.ControlFlags = 1 | (2 << 1);
    pp->quantization.base_qindex = 120;
    pp->quantization.y_dc_delta_q = -3;
    pp->quantization.u_ac_delta_q = 2;
    pp->quantization.qm_y = 5;
    pp->quantization.qm_u = 6;
    pp->quantization.qm_v = 6;
    pp->cdef.ControlFlags = 2 | (2 << 2);
    for (i = 0; i < 4; i++) {
        pp->cdef.y_strengths[i] = (unsigned char)(i * 3 | ((i & 3) << 6));
        pp->cdef.uv_strengths[i] = (unsigned char)(i | (1 << 6));
    }
    pp->interp_filter = 4;
    pp->segmentation.ControlFlags = 1 | 2 | 4;
    pp->segmentation.feature_mask[1] = 1 | 2 | 32;
    pp->segmentation.feature_data[1][0] = -20;
    pp->segmentation.feature_data[1][1] = 5;
    pp->segmentation.feature_data[1][5] = 3;
    pp->film_grain.ControlFlags = 1 | (1 << 1) | (2 << 4) | (1 << 10);
    pp->film_grain.grain_seed = 0x1234;
    pp->film_grain.num_y_points = 2;
    pp->film_grain.scaling_points_y[0][0] = 10;
    pp->film_grain.scaling_points_y[0][1] = 40;
    pp->film_grain.scaling_points_y[1][0] = 200;
    pp->film_grain.scaling_points_y[1][1] = 60;
    for (i = 0; i < 12; i++)
        pp->film_grain.ar_coeffs_y[i] = (unsigned char)(120 + i);
    pp->film_grain.cb_offset = 256;
}

static unsigned ceil_div(unsigned a, unsigned b)
{
    return (a + b - 1) / b;
}

static int cmd_selftest(void)
{
    static const uint8_t sl[] = { 0, 0, 0, 1, 0x65, 0x88, 0x84, 0x00 };  /* first_mb 0, I, pps 0 */
    static const uint8_t sl3[] = { 0, 0, 1, 0x41, 0x9A, 0x20 };         /* first_mb 0, P(5), pps 3? */
    V383_DXVA_PicParams_H264 pp;
    V383_H264RefLists l;
    int fails = 0;
    unsigned i;

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

    {
        /* compile-time: the DXVA layouts */
        typedef char pp_size_is_1040[(sizeof(V383_DXVA_PicParams_H264) == 1040) ? 1 : -1];
        typedef char qm_size_is_224[(sizeof(V383_DXVA_Qmatrix_H264) == 224) ? 1 : -1];
        (void)sizeof(pp_size_is_1040);
        (void)sizeof(qm_size_is_224);
    }
    CHECK(v383_h264_parse_pps_id(sl, sizeof(sl)) == 0);
    (void)sl3;

    /* P list: frame_num 5 current; refs frame_num 4, 3, 1 (short), lt idx 0 */
    memset(&pp, 0, sizeof(pp));
    for (i = 0; i < 16; i++)
        pp.RefFrameList[i].bPicEntry = 0xFF;
    pp.log2_max_frame_num_minus4 = 0;
    pp.frame_num = 5;
    pp.RefFrameList[0].bPicEntry = V383_PIC_ENTRY(2, 0); pp.FrameNumList[0] = 3;
    pp.RefFrameList[1].bPicEntry = V383_PIC_ENTRY(7, 0); pp.FrameNumList[1] = 4;
    pp.RefFrameList[2].bPicEntry = V383_PIC_ENTRY(1, 1); pp.FrameNumList[2] = 0;
    pp.RefFrameList[3].bPicEntry = V383_PIC_ENTRY(3, 0); pp.FrameNumList[3] = 14;   /* wraps to -2 */
    pp.UsedForReferenceFlags = 0xFF;
    pp.FieldOrderCntList[0][0] = pp.FieldOrderCntList[0][1] = 6;
    pp.FieldOrderCntList[1][0] = pp.FieldOrderCntList[1][1] = 8;
    pp.FieldOrderCntList[3][0] = pp.FieldOrderCntList[3][1] = 12;
    pp.CurrFieldOrderCnt[0] = pp.CurrFieldOrderCnt[1] = 10;
    v383_h264_build_reflists(&pp, &l);
    CHECK(l.num_p == 4 && l.p[0] == 1 && l.p[1] == 0 && l.p[2] == 3 && l.p[3] == 2);
    CHECK(l.num_b0 == 4 && l.b0[0] == 1 && l.b0[1] == 0 && l.b0[2] == 3 && l.b0[3] == 2);
    CHECK(l.num_b1 == 4 && l.b1[0] == 3 && l.b1[1] == 1 && l.b1[2] == 0 && l.b1[3] == 2);

    /* RCB sizes for 1920x1088, against the float formulas of MPP */
    {
        unsigned w = 1920, total = v383_h264_rcb_size(1920, 1088, 0);
        double inter = ((w + 3) / 4) * 92.0, intra = ((w * 12 * 2 + 511) / 512) * 512.0;
        double fltd = w * (1.6 * 10 + 0.5) * ((6 + 3) * 1 + 2 + 1.5);
        unsigned expect = 0;
#define RB(b) ((unsigned)((((unsigned)(b) + 7) / 8 + 63) & ~63u))
        expect = 2 * RB(inter) + 2 * RB(intra) + 2 * RB(fltd / 2 + 864 * 16 * 8) + RB(fltd);
        CHECK(total == expect);
        printf("rcb 1920x1088: %u bytes\n", total);
    }
    /* VP9: the packers against MPP's output for the same input (MD5s of
     * the bytes MPP's hal_vp9d_prob_default(), hal_vp9d_prob_kf() and
     * vdpu38x_vp9d_uncomp_hdr() write, taken off-board) */
    {
        static unsigned char probs[V383_VP9_PROB_BYTES], kf[V383_VP9_KFPROB_BYTES], gbl[V383_VP9_GBL_BYTES];
        V383_DXVA_PicParams_VP9 vp;
        V383_Vp9State vs;
        char hex[33];

        typedef char vp9_pp_size_208[(sizeof(V383_DXVA_PicParams_VP9) == 208) ? 1 : -1];
        typedef char vp9_seg_size_83[(sizeof(V383_DXVA_segmentation_VP9) == 83) ? 1 : -1];
        (void)sizeof(vp9_pp_size_208);
        (void)sizeof(vp9_seg_size_83);

        v383_vp9_write_probs(&v383_vp9_default_probs, probs);
        md5_hex(probs, sizeof(probs), hex);
        CHECK(!strcmp(hex, "04f9835c8526b0735efd068f1262c3a0"));
        v383_vp9_write_kf_probs(kf);
        md5_hex(kf, sizeof(kf), hex);
        CHECK(!strcmp(hex, "54ce69eba3fb07e02d00e60045c109c4"));

        /* an inter frame with everything set: scaled references, switchable
         * filter, deltas, segmentation, 4 x 2 tiles */
        memset(&vp, 0, sizeof(vp));
        v383_vp9_reset(&vs);
        vp.wFormatAndPictureInfoFlags = (1 << V383_VP9_FLAG_FRAME_TYPE) | (1 << V383_VP9_FLAG_SHOW_FRAME) |
                                        (1 << V383_VP9_FLAG_REFRESH_CTX);
        vp.width = 1920;
        vp.height = 1080;
        vp.interp_filter = V383_VP9_SWITCHABLE;
        for (i = 0; i < 8; i++) {
            vp.ref_frame_map[i].bPicEntry = (unsigned char)i;
            vp.ref_frame_coded_width[i] = 1280;
            vp.ref_frame_coded_height[i] = 720;
        }
        vp.frame_refs[0].bPicEntry = 1;
        vp.frame_refs[1].bPicEntry = 2;
        vp.frame_refs[2].bPicEntry = 5;
        vp.ref_frame_sign_bias[3] = 1;
        vp.filter_level = 36;
        vp.sharpness_level = 2;
        vp.wControlInfoFlags = 3;
        vp.ref_deltas[0] = 1;
        vp.ref_deltas[2] = -1;
        vp.ref_deltas[3] = -1;
        vp.mode_deltas[1] = -3;
        vp.base_qindex = 120;
        vp.y_dc_delta_q = -2;
        vp.uv_ac_delta_q = 3;
        vp.stVP9Segments.wSegmentInfoFlags = 0xF;
        for (i = 0; i < 7; i++)
            vp.stVP9Segments.tree_probs[i] = (unsigned char)(10 + i);
        for (i = 0; i < 3; i++)
            vp.stVP9Segments.pred_probs[i] = (unsigned char)(200 + i);
        vp.stVP9Segments.feature_mask[3] = 0xB;
        vp.stVP9Segments.feature_data[3][0] = -40;
        vp.stVP9Segments.feature_data[3][1] = 12;
        vp.stVP9Segments.feature_data[3][2] = 2;
        vp.log2_tile_cols = 2;
        vp.log2_tile_rows = 1;
        vp.first_partition_size = 777;
        vs.last_width = 1920;
        vs.last_height = 1080;
        vs.last_show_frame = 1;
        vs.last_frame_type = 1;
        vs.last_ref_deltas[0] = 1;
        vs.last_mode_deltas[0] = -1;
        vs.last_seg_enabled = 1;
        v383_vp9_write_gbl(&vp, &vs, gbl);
        md5_hex(gbl, sizeof(gbl), hex);
        CHECK(!strcmp(hex, "010f3e418d3ab15045316aeaef5395e3"));
        printf("vp9 packers: default and key-frame tables and a global record as MPP writes them\n");

        /* RCB sizes for 1920x1080 (aligned to 1920x1088), MPP's float formulas */
        {
            double w = 1920, h = 1088, fl = w * (1.6 * 10 + 0.5) * (17.5 + 8);
            unsigned expect = 2 * RB(ceil_div(1920, 64) * 2368.0) + 2 * RB(((1920 * 24 + 511) / 512) * 512.0) +
                              2 * RB(fl / 2 + 864 * 16 * 8) + RB(fl) + RB(h * 16.5 * (17.75 + 8));
            CHECK(v383_vp9_rcb_size(1920, 1080) == expect);
        }
    }
    /* AV1: the global record against MPP's vdpu38x_av1d_uncomp_hdr() for
     * the same frame (MD5 taken off-board), the RCB sizes against MPP's
     * float formulas, and the stream wrapper */
    {
        static unsigned char gbl[V383_AV1_GBL_BYTES], obu[64];
        static const unsigned char tile_data[6] = { 1, 2, 3, 4, 5, 6 };
        V383_DXVA_PicParams_AV1 ap;
        V383_DXVA_Tile_AV1 t[2];
        V383_Av1State as;
        V383_Av1Hdr ah;
        char hex[33];
        unsigned n;

        typedef char av1_pp_size_912[(sizeof(V383_DXVA_PicParams_AV1) == 912) ? 1 : -1];
        typedef char av1_entry_size_36[(sizeof(V383_DXVA_PicEntry_AV1) == 36) ? 1 : -1];
        typedef char av1_tile_size_16[(sizeof(V383_DXVA_Tile_AV1) == 16) ? 1 : -1];
        (void)sizeof(av1_pp_size_912);
        (void)sizeof(av1_entry_size_36);
        (void)sizeof(av1_tile_size_16);

        av1_kat_pp(&ap, &as);
        v383_av1_derive(&ap, NULL, &as, &ah);
        CHECK(ah.skip_ref0 == 3 && ah.skip_ref1 == 6 && ah.last_active == 1 && ah.preskip == 1);
        v383_av1_write_gbl(&ah, gbl);
        md5_hex(gbl, sizeof(gbl), hex);
        CHECK(!strcmp(hex, "720bb65f84f66b1126b7940ab7bcfa88"));
        {
            double w = 1280, h = 768, row = w * 16.5 * (12.5 + 6 + 1.5), col = h * 16.5 * (14 + 7 + 26.5 + 6);
            unsigned expect = RB(ceil_div(1280, 8) * 100.0) + 2 * RB(ceil_div(1280, 64) * 2752.0) +
                              2 * RB(((1280 * 24 + 511) / 512) * 512.0) + 2 * RB(row / 2 + 864 * 16 * 8) + RB(row) +
                              RB(col) + RB(ceil_div(720, 64) * 220.0);
            CHECK(v383_av1_rcb_size(&ap) == expect);
        }
        memset(t, 0, sizeof(t));
        t[0].DataSize = 4;
        t[1].DataOffset = 4;
        t[1].DataSize = 2;
        n = v383_av1_synth_stream(&ap, t, 2, tile_data, 0, 0, obu, sizeof(obu));
        CHECK(n == 13 && obu[0] == 0x22 && obu[1] == 11 && obu[2] == 0 && obu[3] == 3 && obu[7] == 1 && obu[12] == 6);
        printf("av1: global record as MPP writes it, row buffers, OBU wrapper checked\n");
    }
    printf("selftest: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}

/* ---------------------------------------------------------- sessions */

/*
 * The driver's session rules (vdpu383_check.h) on the null device: two
 * sessions (two processes' display driver sessions), their buffers, an
 * imported texture, revocation, closing. Each line names what it checks.
 */
static unsigned st_fails;
#define ST_CHECK(c, what) do { int ok_ = (c); printf("sessiontest: %-62s %s\n", what, ok_ ? "ok" : "FAILED"); \
                               if (!ok_) st_fails++; } while (0)

static void st_set(VDPU383_RUN_IN *in, unsigned r, unsigned v)
{
    in->regs[r] = v;
    in->write_mask[r / 32] |= 1u << (r % 32);
}

static unsigned st_run(const VDPU383_RUN_IN *in, unsigned *bad)
{
    VDPU383_RUN_OUT *out = (VDPU383_RUN_OUT *)calloc(1, sizeof(*out));
    unsigned r;

    if (!out || dev_run(in, out)) {
        free(out);
        return 0xFFFFFFFFu;
    }
    r = out->result;
    *bad = out->bad_reg;
    free(out);
    return r;
}

static int cmd_sessiontest(void)
{
    VDPU383_RUN_IN *in = (VDPU383_RUN_IN *)calloc(1, sizeof(*in));
    VDPU383_INFO info;
    VDPU383_IMPORT_OUT imp, imp2;
    DevBuf strm, aux, pic, other, stolen;
    uint64_t tok, tok2, tokb;
    unsigned bad = 0, r, in_sz, out_sz;
    const VDPU383_RUN_IN *seen;

    if (!in)
        return 1;
    dev_null_sessions(1);

    /* which calls a session has, and their sizes */
    ST_CHECK(v383_session_ioctl_ok(IOCTL_VDPU383_RUN) && v383_session_ioctl_ok(IOCTL_VDPU383_ALLOC) &&
             v383_session_ioctl_ok(IOCTL_VDPU383_FREE) && v383_session_ioctl_ok(IOCTL_VDPU383_IMPORT) &&
             v383_session_ioctl_ok(IOCTL_VDPU383_GET_INFO), "a session may run, allocate, free, import, ask");
    ST_CHECK(!v383_session_ioctl_ok(IOCTL_VDPU383_WRITE_REGS) && !v383_session_ioctl_ok(IOCTL_VDPU383_READ_REGS) &&
             !v383_session_ioctl_ok(IOCTL_VDPU383_WARMUP) && !v383_session_ioctl_ok(IOCTL_VDPU383_RESET) &&
             !v383_session_ioctl_ok(0x83830000u), "no register access, warm-up, reset or unknown call");
    ST_CHECK(v383_session_sizes(IOCTL_VDPU383_RUN, &in_sz, &out_sz) && in_sz == sizeof(VDPU383_RUN_IN) &&
             out_sz == sizeof(VDPU383_RUN_OUT) && !v383_session_sizes(IOCTL_VDPU383_WRITE_REGS, &in_sz, &out_sz),
             "the sizes of a session's calls");

    /* session 1: a decode of its own buffers */
    dev_null_select(1);
    if (dev_alloc(64 << 10, &strm) || dev_alloc(64 << 10, &aux) || dev_alloc(256 << 10, &pic))
        return 1;
    st_set(in, 8, 0x1);                 /* a mode register, not an address */
    st_set(in, 66, 1000);
    st_set(in, 128, strm.dma);
    st_set(in, 131, aux.dma + 0x100);
    st_set(in, 168, pic.dma);
    st_set(in, 216, pic.dma + (192 << 10));
    in->flags = VDPU383_RUN_RESET_ON_ERROR;
    in->timeout_ms = 2000;
    ST_CHECK(st_run(in, &bad) == VDPU383_RUN_OK, "session 1 decodes into its own buffers");

    /* an address just past its picture buffer */
    st_set(in, 168, pic.dma + pic.size);
    r = st_run(in, &bad);
    ST_CHECK(r == VDPU383_RUN_BAD_ADDRESS && bad == 168, "an address outside its buffers is refused (register 168)");
    in->flags |= VDPU383_RUN_NO_ADDR_CHECK;
    r = st_run(in, &bad);
    ST_CHECK(r == VDPU383_RUN_BAD_ADDRESS && bad == 168, "and is refused with VDPU383_RUN_NO_ADDR_CHECK too");
    st_set(in, 168, pic.dma);

    /* what the driver makes of a session's run flags */
    in->flags = VDPU383_RUN_NO_ADDR_CHECK | VDPU383_RUN_READBACK | VDPU383_RUN_RESET_ON_ERROR;
    in->timeout_ms = 600000;
    in->link_ip_enable = 0x01000000;
    r = st_run(in, &bad);
    seen = dev_null_last_run();
    ST_CHECK(r == VDPU383_RUN_OK && seen->flags == VDPU383_RUN_RESET_ON_ERROR &&
             seen->timeout_ms == V383_SESSION_TIMEOUT_MS && seen->link_ip_enable == 0,
             "no read-back, no skipped check, a bounded wait, link enable 0");
    in->flags = VDPU383_RUN_RESET_ON_ERROR;
    in->timeout_ms = 0;
    in->link_ip_enable = 0;
    (void)st_run(in, &bad);
    ST_CHECK(dev_null_last_run()->timeout_ms == V383_SESSION_TIMEOUT_MS, "timeout 0 becomes the session bound");

    /* session 2 (another process) names session 1's buffers */
    dev_null_select(2);
    if (dev_alloc(64 << 10, &other))
        return 1;
    r = st_run(in, &bad);
    ST_CHECK(r == VDPU383_RUN_BAD_ADDRESS && bad == 128, "another session's buffers are refused");
    stolen = pic;
    dev_free(&stolen);
    dev_null_select(1);
    ST_CHECK(st_run(in, &bad) == VDPU383_RUN_OK, "another session cannot free them");

    /* an imported texture of session 1's process */
    tok = dev_null_export(1, 64);
    tokb = dev_null_export(2, 64);
    dev_null_select(2);
    ST_CHECK(dev_import(tok, 64, 0, &imp) != 0, "another process's export token is refused");
    dev_null_select(1);
    ST_CHECK(dev_import(tokb, 64, 0, &imp) != 0, "and the other way round");
    ST_CHECK(dev_import(tok, 64, 0, &imp) == 0 && imp.handle, "session 1 imports its own texture");
    st_set(in, 168, imp.dma);
    ST_CHECK(st_run(in, &bad) == VDPU383_RUN_OK, "and decodes into it");
    ST_CHECK(dev_import(tok, 64, 0, &imp2) != 0, "a second import of the same export is refused");
    dev_null_select(2);
    r = st_run(in, &bad);
    ST_CHECK(r == VDPU383_RUN_BAD_ADDRESS, "session 2 cannot decode into it");
    dev_null_select(1);

    /* the display driver takes the pages back (eviction, destruction) */
    dev_null_revoke(tok);
    r = st_run(in, &bad);
    ST_CHECK(r == VDPU383_RUN_REVOKED && bad == 168, "after revocation a decode naming it is refused (revoked)");
    ST_CHECK(dev_import(tok, 64, imp.handle, &imp2) != 0, "the old token is gone");
    tok2 = dev_null_export(1, 64);
    ST_CHECK(dev_import(tok2, 64, imp.handle, &imp2) == 0 && imp2.dma == imp.dma, "a new export refreshes it in place");
    ST_CHECK(st_run(in, &bad) == VDPU383_RUN_OK, "and the decode runs again");

    /* session 1's process exits: its session closes */
    dev_null_close(1);
    dev_null_select(2);
    ST_CHECK(st_run(in, &bad) == VDPU383_RUN_BAD_ADDRESS, "a closed session's buffers stay refused to others");
    dev_null_select(3);
    ST_CHECK(st_run(in, &bad) == VDPU383_RUN_BAD_ADDRESS, "and to a new session");

    /* what a session is told */
    memset(&info, 0x5a, sizeof(info));
    v383_session_scrub_info(&info);
    ST_CHECK(info.iommu_dt_dma == 0 && info.mmu_dte[0] == 0 && info.mmu_dte[1] == 0 && info.sram_dma == 0 &&
             info.link[0] == 0 && info.link[23] == 0 && info.warmup.buffer_dma == 0 &&
             info.mmu_last_fault_addr[0] == 0 && info.runs == 0x5a5a5a5au,
             "no physical addresses or registers in GET_INFO, counters kept");

    free(in);
    printf("sessiontest: %s (%u refused decodes counted)\n", st_fails ? "FAILED" : "passed", dev_null_refused());
    return st_fails ? 1 : 0;
}

/* ---------------------------------------------------------------- main */

static void usage(void)
{
    printf("usage: vdpu383test [--null] info | warmup | warmup-direct | regs [off] [count] | reset |\n"
           "                   decode IN.264 [-o out.yuv] [--ref ref.yuv] [-n N] [--frame-md5]\n"
           "                          [--dump DIR] [--readback] [--regs] [--dxva-only]\n"
           "                          [--stride mpp] [--link-vendor] [--vp9-mpp[-intra|-update]] [--av1-synth 0|1] |\n"
           "                   replay DIR/replay.txt | selftest | --null sessiontest\n"
           "       --session (with --null): decode as a session of the driver\n");
}

int main(int argc, char **argv)
{
    int null_dev = 0, i, rc;
    const char *cmd = NULL;
    int first = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--null"))
            null_dev = 1;
        else if (!strcmp(argv[i], "--session"))
            g_session_mode = 1;
        else if (!cmd) {
            cmd = argv[i];
            first = i + 1;
        }
    }
    if (!cmd) {
        usage();
        return 1;
    }
    if (!strcmp(cmd, "selftest"))
        return cmd_selftest();
    if (dev_open(null_dev)) {
        fprintf(stderr, "%s\n", dev_error());
        return 1;
    }
    if (null_dev && g_session_mode)
        dev_null_sessions(1);
    if (!strcmp(cmd, "sessiontest"))
        rc = null_dev ? cmd_sessiontest() : 1;
    else if (!strcmp(cmd, "info"))
        rc = cmd_info();
    else if (!strcmp(cmd, "warmup"))
        rc = cmd_warmup();
    else if (!strcmp(cmd, "warmup-direct"))
        rc = cmd_warmup_direct();
    else if (!strcmp(cmd, "regs"))
        rc = cmd_regs(argc - first, argv + first);
    else if (!strcmp(cmd, "reset"))
        rc = cmd_reset();
    else if (!strcmp(cmd, "decode"))
        rc = cmd_decode(argc - first, argv + first);
    else if (!strcmp(cmd, "replay") && first < argc)
        rc = cmd_replay(argv[first]);
    else {
        usage();
        rc = 1;
    }
    dev_close();
    return rc;
}
