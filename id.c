#include <assert.h>
#include <dispatch/dispatch.h>
#include <ftw.h>
#include <png.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define UNUSED __attribute__((unused))

#define MANY 1024*1024
#define NOPENFD 100

static const char *good = "good",
                  *bad  = "bad",
                  *ugly = "ugly";

enum state { BYTE_EQ, PIXEL_EQ, MISSING, DIMENSION_DIFF, PIXEL_DIFF };
static const char* state_name[] = {
    "byte-equal", "pixel-equal", "missing", "dimension-diff", "pixel-diff"
};

static size_t nwork = 0;
static struct {
    const char* suffix;  // on heap, cleanup with free()
    enum state state;
    int diffs;
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

static uint32_t* read_png(const uint8_t* buf, size_t len, size_t* x, size_t* y) {
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    assert (png);
    png_infop info = png_create_info_struct(png);
    assert (info);

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        return NULL;
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
    *x = w;
    *y = h;
    for (size_t j = 0; j < h; j++) {
        memcpy(pix+(j*w), rows[j], w*4);
    }

    png_destroy_read_struct(&png, &info, NULL);
    return pix;
}

static int diff_pngs(const uint8_t* gpng, size_t gpnglen,
                     const uint8_t* bpng, size_t bpnglen) {
    int diffs = 0;
    size_t gx,gy, bx,by;
    uint32_t *g = read_png(gpng, gpnglen, &gx, &gy),
             *b = read_png(bpng, bpnglen, &bx, &by);
    if (g && b) {
        if (gx == bx && gy == by) {
            size_t w = gx, h = gy;
            for (size_t j = 0; j < h; j++) {
            for (size_t i = 0; i < w; i++) {
                if (g[j*w+i] != b[j*w+i]) {
                    diffs++;
                }
            }
            }
        } else {
            diffs = -1;
        }
    }
    free(g);
    free(b);
    return diffs;
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
            int diffs = diff_pngs(g, glen, b, blen);
            switch (diffs) {
                case -1: work[i].state = DIMENSION_DIFF; break;
                case  0: work[i].state = PIXEL_EQ;       break;
                default: work[i].state = PIXEL_DIFF;     break;
            }
            work[i].diffs = diffs;
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

    for (int state = 0; state <= PIXEL_DIFF; state++) {
        int n = 0;
        for (size_t i = 0; i < nwork; i++) {
            n += (work[i].state == state && work[i].diffs != 0);
        }
        if (n > 0) {
            printf("%s:\n", state_name[state]);
            for (size_t i = 0; i < nwork; i++) {
                if (work[i].state == state) {
                    printf("\t%d\t%s\n", work[i].diffs, work[i].suffix);
                }
            }
        }
    }
    return 0;
}
