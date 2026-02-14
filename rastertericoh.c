/*
 * rastertericoh - CUPS raster filter for Ricoh SP100/SP200 family GDI printers
 *
 * Converts CUPS raster input to Ricoh GDI format (PJL + JBIG1 bitmap).
 * Designed to work within the macOS CUPS sandbox as a compiled binary
 * with libjbig statically linked.
 *
 * CUPS filter chain: PDF -> cgpdftoraster -> rastertericoh -> USB backend
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <cups/cups.h>
#include <cups/raster.h>
#include <jbig.h>

/* Write PJL line with CR+LF ending */
static void pjl_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\r', stdout);
    fputc('\n', stdout);
}

/* Write raw bytes to stdout */
static void write_bytes(const unsigned char *data, size_t len)
{
    fwrite(data, 1, len, stdout);
}

/* Convert CUPS raster page to packed PBM (1-bit) format.
 * CUPS raster for a B&W printer should already be 1-bit,
 * but we handle 8-bit grayscale too just in case. */
static unsigned char *raster_to_pbm(cups_page_header2_t *header,
                                     cups_raster_t *ras,
                                     unsigned int *out_width,
                                     unsigned int *out_height,
                                     size_t *out_size)
{
    unsigned int width = header->cupsWidth;
    unsigned int height = header->cupsHeight;
    unsigned int bpl = header->cupsBytesPerLine;
    /* PBM row stride: ceil(width/8) */
    unsigned int pbm_stride = (width + 7) / 8;
    size_t pbm_size = (size_t)pbm_stride * height;
    unsigned char *pbm = calloc(1, pbm_size);
    unsigned char *line = malloc(bpl);

    if (!pbm || !line) {
        syslog(LOG_ERR, "rastertericoh: memory allocation failed");
        free(pbm);
        free(line);
        return NULL;
    }

    for (unsigned int y = 0; y < height; y++) {
        if (cupsRasterReadPixels(ras, line, bpl) != bpl) {
            syslog(LOG_ERR, "rastertericoh: short read at line %u", y);
            break;
        }

        unsigned char *dst = pbm + (size_t)y * pbm_stride;

        if (header->cupsBitsPerPixel == 1) {
            /* Already 1-bit packed - but CUPS uses 0=white, 1=black
             * which matches PBM convention. Just copy. */
            memcpy(dst, line, pbm_stride);
        } else if (header->cupsBitsPerPixel == 8) {
            /* 8-bit grayscale: threshold at 128
             * CUPS: 0=black, 255=white for COLORSPACE_W
             * PBM: 1=black, 0=white
             * So: if pixel < 128 -> black (1), else white (0) */
            memset(dst, 0, pbm_stride);
            for (unsigned int x = 0; x < width; x++) {
                int black;
                if (header->cupsColorSpace == CUPS_CSPACE_W ||
                    header->cupsColorSpace == CUPS_CSPACE_SW) {
                    /* White colorspace: 0=black, 255=white */
                    black = (line[x] < 128);
                } else {
                    /* K colorspace: 0=white, 255=black */
                    black = (line[x] >= 128);
                }
                if (black) {
                    dst[x / 8] |= (0x80 >> (x % 8));
                }
            }
        } else {
            syslog(LOG_WARNING, "rastertericoh: unsupported bpp=%u, treating as 1-bit",
                   header->cupsBitsPerPixel);
            memcpy(dst, line, pbm_stride < bpl ? pbm_stride : bpl);
        }
    }

    free(line);
    *out_width = width;
    *out_height = height;
    *out_size = pbm_size;
    return pbm;
}

/* JBIG output callback - collect compressed data */
typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} jbig_buffer_t;

static void jbig_data_cb(unsigned char *start, size_t len, void *file)
{
    jbig_buffer_t *buf = (jbig_buffer_t *)file;
    if (buf->size + len > buf->capacity) {
        size_t new_cap = buf->capacity * 2;
        if (new_cap < buf->size + len)
            new_cap = buf->size + len + 65536;
        unsigned char *new_data = realloc(buf->data, new_cap);
        if (!new_data) {
            syslog(LOG_ERR, "rastertericoh: JBIG buffer realloc failed");
            return;
        }
        buf->data = new_data;
        buf->capacity = new_cap;
    }
    memcpy(buf->data + buf->size, start, len);
    buf->size += len;
}

/* Compress PBM data to JBIG1 with the specific parameters for Ricoh printers */
static unsigned char *pbm_to_jbig(const unsigned char *pbm,
                                   unsigned int width,
                                   unsigned int height,
                                   size_t *out_size)
{
    struct jbg_enc_state enc;
    jbig_buffer_t buf = {NULL, 0, 0};

    buf.capacity = 65536;
    buf.data = malloc(buf.capacity);
    if (!buf.data) return NULL;

    unsigned char *pbm_mut = (unsigned char *)pbm;
    jbg_enc_init(&enc, width, height, 1, &pbm_mut, jbig_data_cb, &buf);

    /* Parameters matching the original driver:
     * -p 72  -> l0 = 72 (lines per stripe)
     * -o 3   -> order = JBG_HITOLO | JBG_SEQ = 3
     * -m 0   -> mx = 0 (no AT moves)
     * -q     -> options = JBG_TPBON (typical prediction) */
    jbg_enc_options(&enc, JBG_HITOLO | JBG_SEQ, JBG_TPBON, 72, 0, 0);
    jbg_enc_out(&enc);
    jbg_enc_free(&enc);

    *out_size = buf.size;
    return buf.data;
}

/* Map CUPS page size name to PJL paper name */
static const char *cups_to_pjl_paper(const char *cups_size)
{
    if (!cups_size) return "A4";
    if (strcasecmp(cups_size, "A4") == 0) return "A4";
    if (strcasecmp(cups_size, "Letter") == 0) return "LETTER";
    if (strcasecmp(cups_size, "Legal") == 0) return "LEGAL";
    if (strcasecmp(cups_size, "A5") == 0) return "A5";
    if (strcasecmp(cups_size, "A6") == 0) return "A6";
    if (strcasecmp(cups_size, "B5") == 0) return "B5";
    if (strcasecmp(cups_size, "B6") == 0) return "B6";
    if (strcasecmp(cups_size, "Monarch") == 0) return "MONARCH";
    return "A4";
}

int main(int argc, char *argv[])
{
    cups_raster_t *ras;
    cups_page_header2_t header;
    int fd;
    int page_count = 0;
    const char *user = argc > 2 ? argv[2] : "unknown";

    openlog("rastertericoh", LOG_PID, LOG_LPR);
    syslog(LOG_INFO, "starting, argc=%d", argc);

    /* Open raster input */
    if (argc >= 7) {
        fd = open(argv[6], O_RDONLY);
        if (fd < 0) {
            syslog(LOG_ERR, "cannot open input file %s", argv[6]);
            return 1;
        }
    } else {
        fd = 0; /* stdin */
    }

    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!ras) {
        syslog(LOG_ERR, "cannot open raster stream");
        return 1;
    }

    /* Timestamp for PJL */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y/%m/%d %H:%M:%S", tm);

    /* Process pages */
    while (cupsRasterReadHeader2(ras, &header)) {
        unsigned int width, height;
        size_t pbm_size, jbig_size;

        if (header.cupsBytesPerLine == 0 || header.cupsHeight == 0) {
            syslog(LOG_WARNING, "empty page, skipping");
            continue;
        }

        syslog(LOG_INFO, "page %d: %ux%u, %u bpp, %u bpl, colorspace=%u",
               page_count + 1, header.cupsWidth, header.cupsHeight,
               header.cupsBitsPerPixel, header.cupsBytesPerLine,
               header.cupsColorSpace);

        /* Read and convert raster to PBM */
        unsigned char *pbm = raster_to_pbm(&header, ras, &width, &height, &pbm_size);
        if (!pbm) {
            syslog(LOG_ERR, "failed to convert raster page %d", page_count + 1);
            continue;
        }

        /* Compress to JBIG */
        unsigned char *jbig = pbm_to_jbig(pbm, width, height, &jbig_size);
        free(pbm);
        if (!jbig) {
            syslog(LOG_ERR, "failed to JBIG-compress page %d", page_count + 1);
            continue;
        }

        syslog(LOG_INFO, "page %d: JBIG compressed %zu -> %zu bytes",
               page_count + 1, pbm_size, jbig_size);

        /* Emit PJL job header before first page */
        if (page_count == 0) {
            fprintf(stdout, "\033%%-12345X@PJL\r\n");
            pjl_printf("@PJL SET TIMESTAMP=%s", timestamp);
            pjl_printf("@PJL SET FILENAME=Document");
            pjl_printf("@PJL SET COMPRESS=JBIG");
            pjl_printf("@PJL SET USERNAME=%s", user);
            pjl_printf("@PJL SET COVER=OFF");
            pjl_printf("@PJL SET HOLD=OFF");
        }

        /* Page header */
        const char *paper = cups_to_pjl_paper(header.cupsPageSizeName);
        int resolution = header.HWResolution[0];
        const char *mediasource = "TRAY1";
        if (header.MediaPosition == 1)
            mediasource = "MANUALFEED";

        pjl_printf("@PJL SET PAGESTATUS=START");
        pjl_printf("@PJL SET COPIES=1");
        pjl_printf("@PJL SET MEDIASOURCE=%s", mediasource);
        pjl_printf("@PJL SET MEDIATYPE=PLAINRECYCLE");
        pjl_printf("@PJL SET PAPER=%s", paper);
        pjl_printf("@PJL SET PAPERWIDTH=%u", width);
        pjl_printf("@PJL SET PAPERLENGTH=%u", height);
        pjl_printf("@PJL SET RESOLUTION=%d", resolution);
        pjl_printf("@PJL SET IMAGELEN=%zu", jbig_size);

        /* JBIG raster data */
        write_bytes(jbig, jbig_size);
        free(jbig);

        /* Page footer */
        pjl_printf("@PJL SET DOTCOUNT=1132782");
        pjl_printf("@PJL SET PAGESTATUS=END");

        page_count++;
    }

    /* Job footer */
    if (page_count > 0) {
        pjl_printf("@PJL EOJ");
        fprintf(stdout, "\033%%-12345X");
        fflush(stdout);
        syslog(LOG_INFO, "job complete, %d page(s)", page_count);
    } else {
        syslog(LOG_WARNING, "no pages processed");
    }

    cupsRasterClose(ras);
    if (fd > 0) close(fd);
    closelog();

    return page_count > 0 ? 0 : 1;
}
