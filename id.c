#include <assert.h>
#include <ftw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <immintrin.h>

#include <CommonCrypto/CommonDigest.h>
#include <dispatch/dispatch.h>
#include <png.h>

#define UNUSED __attribute__((unused))

#define MANY 1024*1024
#define NOPENFD 100

static const char *good = "good",
                  *bad  = "bad",
                  *ugly = "ugly";

enum state { BYTE_EQ, PIXEL_EQ, MISSING, INCOMPARABLE, DIFF };
static const char* state_name[] = {
    "byte-equal", "pixel-equal", "missing", "incomparable", "diff"
};

struct bitmap {
    uint32_t* pixels;
    size_t w,h;
};
static void free_bitmap(struct bitmap b) { free(b.pixels); }

static size_t nwork = 0;
static struct {
    const char* suffix;  // on heap, cleanup with free()
    struct bitmap ugly;
    int diffs;
    uint32_t max;
    enum state state;
    uint8_t md5[16];

    int padding;
} work[MANY];

static int find_work(const char* fpath, const struct stat* sb UNUSED, int typeflag) {
    if (typeflag == FTW_F) {
        const char* suffix = fpath + strlen(good);
        size_t len = strlen(suffix);
        if (len > 4 && 0 == strcmp(".png", suffix+len-4)) {
            work[nwork++].suffix = strdup(suffix);
        }
    }
    return 0;
}

struct png_io {
    const uint8_t* buf;
    size_t len;
};

static void read_png_io(png_structp png, png_bytep data, png_size_t len) {
    struct png_io* io = png_get_io_ptr(png);
    size_t l = len < io->len ? len : io->len;
    memcpy(data, io->buf, l);
    io->buf += l;
    io->len -= l;
}

static struct bitmap read_png(const uint8_t* buf, size_t len) {
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    assert (png);
    png_infop info = png_create_info_struct(png);
    assert (info);

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        return (struct bitmap) { NULL, 0, 0 };
    }

    struct png_io io = { buf, len };
    png_set_read_fn(png, &io, read_png_io);

    png_read_png(png, info, PNG_TRANSFORM_EXPAND, NULL);
    png_bytepp rows = png_get_rows(png, info);

    assert (                       8 == png_get_bit_depth (png, info));
    assert (PNG_COLOR_TYPE_RGB_ALPHA == png_get_color_type(png, info));

    size_t w = png_get_image_width (png, info),
           h = png_get_image_height(png, info);

    uint32_t* pix = calloc(w*h, 4);
    for (size_t j = 0; j < h; j++) {
        memcpy(pix+(j*w), rows[j], w*4);
    }

    png_destroy_read_struct(&png, &info, NULL);
    return (struct bitmap) { pix, w, h };
}

static struct bitmap diff_pngs(const uint8_t* gpng, size_t glen,
                               const uint8_t* bpng, size_t blen) {
    struct bitmap g = read_png(gpng, glen),
                  b  = read_png(bpng, blen),
                  u = { NULL, 0, 0 };

    if (g.pixels && b.pixels && g.w == b.w && g.h == b.h) {
        size_t w = g.w, h = g.h;
        u = (struct bitmap) { calloc(w*h, 4), w, h };
        for (size_t j = 0; j < h; j++) {
        for (size_t i = 0; i < w; i++) {
            __m128i g4 = _mm_cvtsi32_si128((int)g.pixels[j*w+i]),
                    b4 = _mm_cvtsi32_si128((int)b.pixels[j*w+i]);
            u.pixels[j*w+i] = (uint32_t)_mm_cvtsi128_si32(_mm_or_si128(_mm_subs_epu8(g4,b4),
                                                                       _mm_subs_epu8(b4,g4)));
        }
        }
    }
    free_bitmap(g);
    free_bitmap(b);
    return u;
}

static void do_work(void* ctx UNUSED, size_t i) {
    size_t len = strlen(work[i].suffix);
    char gpath[strlen(good) + len + 1],
         bpath[strlen(bad)  + len + 1];
    strcat(strcpy(gpath, good), work[i].suffix);
    strcat(strcpy(bpath,  bad), work[i].suffix);

    int gfd = open(gpath, O_RDONLY),
        bfd = open(bpath, O_RDONLY);
    if (gfd >= 0 || bfd >= 0) {
        struct stat st;
        fstat(gfd, &st);
        size_t glen = (size_t)st.st_size;
        fstat(bfd, &st);
        size_t blen = (size_t)st.st_size;

        const uint8_t *g = mmap(0, glen, PROT_READ, MAP_FILE|MAP_PRIVATE, gfd, 0),
                      *b = mmap(0, blen, PROT_READ, MAP_FILE|MAP_PRIVATE, bfd, 0);
        assert (g != MAP_FAILED && b != MAP_FAILED);
        if (glen == blen && 0 == memcmp(g, b, glen)) {
            work[i].state = BYTE_EQ;
        } else {
            struct bitmap u = diff_pngs(g, glen, b, blen);
            work[i].ugly = u;
            if (!u.pixels) {
                work[i].state = INCOMPARABLE;
            } else {
                __m128i max = _mm_setzero_si128();
                for (size_t p = 0; p < u.w*u.h; p++) {
                    max = _mm_max_epu8(max, _mm_cvtsi32_si128((int)u.pixels[p]));
                    work[i].diffs += (u.pixels[p] != 0);
                }
                work[i].state = work[i].diffs ? DIFF : PIXEL_EQ;
                work[i].max   = (uint32_t)_mm_cvtsi128_si32(max);

                CC_MD5(u.pixels, (CC_LONG)(4*u.w*u.h), work[i].md5);
            }
        }
        munmap((void*)g, glen);
        munmap((void*)b, blen);
    } else {
        work[i].state = MISSING;
    }
    if (gfd >= 0) close(gfd);
    if (bfd >= 0) close(bfd);
}

int main(int argc, char** argv) {
    switch (argc) {
        case 4: ugly = argv[3];
        case 3: bad  = argv[2];
        case 2: good = argv[1];
        case 1: break;
        default: fprintf(stderr, "usage: %s [good] [bad] [ugly]\n", argv[0]); return 1;
    }

    ftw(good, find_work, NOPENFD);

    dispatch_queue_t queue = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
    dispatch_apply_f(nwork, queue, NULL, do_work);

    for (int state = 0; state <= DIFF; state++) {
        int n = 0;
        for (size_t i = 0; i < nwork; i++) {
            n += (work[i].state == state && work[i].diffs != 0);
        }
        if (n > 0) {
            printf("%s:\n", state_name[state]);
            for (size_t i = 0; i < nwork; i++) {
                if (work[i].state == state) {
                    printf("\t%d\t0x%08x\t", work[i].diffs, work[i].max);
                    for (int j = 0; j < 16; j++) {
                        printf("%02x", work[i].md5[j]);
                    }
                    printf("\t%s\n", work[i].suffix);
                }
            }
        }
    }
    return 0;
}
