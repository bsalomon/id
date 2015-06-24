#include <assert.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <immintrin.h>
#include <png.h>

#if defined(__BLOCKS__) && defined(__APPLE__)
    #include <dispatch/dispatch.h>
#endif

#define UNUSED __attribute__((unused))

#define MANY 1024*1024
#define NOPENFD 100

static const char *good = "good",
                  *bad  = "bad",
                  *ugly = "ugly.html",
                  *diff  = "/tmp";

enum state { BYTE_EQ, PIXEL_EQ, MISSING, INCOMPARABLE, DIFF };
static const char* state_name[] = {
    "byte-equal", "pixel-equal", "missing", "incomparable", "diff"
};

struct bitmap {
    uint32_t* pixels;
    size_t w,h;
};
static void free_bitmap(struct bitmap b) { _mm_free(b.pixels); }

static size_t nwork = 0;
static struct {
    const char* suffix;  // on heap, cleanup with free()
    struct bitmap diff;

    const char *gpath, *bpath, *dpath;

    size_t diffs;
    uint32_t max;
    enum state state;
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

    png_read_info(png, info);
    // Force ARGB-8888.
    png_set_strip_16(png);
    png_set_expand(png);
    png_set_add_alpha(png, 0xFF, PNG_FILLER_BEFORE);
    png_read_update_info(png, info);

    size_t w = png_get_image_width (png, info),
           h = png_get_image_height(png, info);
    uint32_t* pix = _mm_malloc(w*h*4, 16);
    png_bytep rows[h];
    for (size_t j = 0; j < h; j++) {
        rows[j] = (png_bytep)(pix+j*w);
    }
    png_read_image(png, rows);

    png_destroy_read_struct(&png, &info, NULL);
    return (struct bitmap) { pix, w, h };
}

static void dump_png(struct bitmap bm, const char* path) {
    FILE* f = fopen(path, "w");
    assert(f);
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    assert(png);
    png_infop info = png_create_info_struct(png);
    assert(info);

    png_set_compression_level(png, 1);
    png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);

    png_init_io(png, f);
    png_set_IHDR(png, info, (png_uint_32)bm.w, (png_uint_32)bm.h, 8, PNG_COLOR_TYPE_RGB_ALPHA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    png_bytep rows[bm.h];
    for (size_t j = 0; j < bm.h; j++) {
        rows[j] = (png_bytep)(bm.pixels+j*bm.w);
    }
    png_set_rows(png, info, rows);
    png_write_png(png, info, 0, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(f);
}

static struct bitmap diff_pngs(const uint8_t* gpng, size_t glen,
                               const uint8_t* bpng, size_t blen) {
    struct bitmap g = read_png(gpng, glen),
                  b = read_png(bpng, blen),
                  d = { NULL, 0, 0 };

    if (g.pixels && b.pixels && g.w == b.w && g.h == b.h) {
        size_t w = g.w, h = g.h;
        d = (struct bitmap) { _mm_malloc(w*h*4, 16), w, h };
        size_t p = 0;
        for (; p < w*h/4*4; p += 4) {
            __m128i g4 = _mm_load_si128((const __m128i*)(g.pixels+p)),
                    b4 = _mm_load_si128((const __m128i*)(b.pixels+p));
            _mm_store_si128((__m128i*)(d.pixels+p), _mm_or_si128(_mm_subs_epu8(g4,b4),
                                                                 _mm_subs_epu8(b4,g4)));
        }
        for (; p < w*h; p++) {
            __m128i g1 = _mm_cvtsi32_si128((int)g.pixels[p]),
                    b1 = _mm_cvtsi32_si128((int)b.pixels[p]);
            d.pixels[p] = (uint32_t)_mm_cvtsi128_si32(_mm_or_si128(_mm_subs_epu8(g1,b1),
                                                                   _mm_subs_epu8(b1,g1)));
        }
    }
    free_bitmap(g);
    free_bitmap(b);
    return d;
}

static uint32_t hash(const uint32_t* p, size_t len) {
    uint32_t h = 0;
    for (size_t i = 0; i < len; i++) {
        uint32_t k = p[i];
        k *= 0xcc9e2d51;
        k = (k << 15) | (k >> 17);
        k *= 0x1b873593;
        h ^= k;
        h = (h << 13) | (h >> 19);
        h *= 5;
        h += 0xe6546b64;
    }
    h ^= len;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

static void do_work(void* ctx UNUSED, size_t i) {
    size_t len = strlen(work[i].suffix);
    char gpath[strlen(good) + len + 1],
         bpath[strlen(bad)  + len + 1];
    strcat(strcpy(gpath, good), work[i].suffix);
    strcat(strcpy(bpath,  bad), work[i].suffix);

    work[i].gpath = strdup(gpath);
    work[i].bpath = strdup(bpath);

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
            work[i].diff = diff_pngs(g, glen, b, blen);
            struct bitmap* d = &work[i].diff;

            if (!d->pixels) {
                work[i].state = INCOMPARABLE;
            } else {
                __m128i max = _mm_setzero_si128();
                __m128i diffs = _mm_setzero_si128();
                size_t p = 0;
                for (; p < d->w*d->h/4*4; p += 4) {
                    __m128i p4 = _mm_load_si128((const __m128i*)(d->pixels+p));
                    diffs = _mm_add_epi32(diffs,
                            _mm_add_epi32(_mm_set1_epi32(1),
                                          _mm_cmpeq_epi32(p4, _mm_setzero_si128())));
                    max = _mm_max_epu8(max, p4);
                    p4 = _mm_cmpeq_epi8(p4, _mm_setzero_si128());
                    _mm_store_si128((__m128i*)(d->pixels+p), p4);
                }
                for (; p < d->w*d->h; p++) {
                    __m128i p1 = _mm_cvtsi32_si128((int)d->pixels[p]);
                    diffs = _mm_add_epi32(diffs,
                            _mm_add_epi32(_mm_set1_epi32(1),
                                          _mm_cmpeq_epi32(p1, _mm_setzero_si128())));
                    max = _mm_max_epu8(max, p1);
                    p1 = _mm_cmpeq_epi8(p1, _mm_setzero_si128());
                    d->pixels[p] = (uint32_t)_mm_cvtsi128_si32(p1);
                }
                diffs = _mm_add_epi32(diffs, _mm_srli_si128(diffs, 8));
                diffs = _mm_add_epi32(diffs, _mm_srli_si128(diffs, 4));
                max = _mm_max_epu8(max, _mm_srli_si128(max, 8));
                max = _mm_max_epu8(max, _mm_srli_si128(max, 4));

                work[i].diffs = (uint32_t)_mm_cvtsi128_si32(diffs);
                work[i].max   = (uint32_t)_mm_cvtsi128_si32(max);
                work[i].state = work[i].diffs ? DIFF : PIXEL_EQ;

                uint32_t h = hash(d->pixels, d->w*d->h);

                char hashpng[strlen(diff) + 1 + 8 + 4 + 1];
                sprintf(hashpng, "%s/%08x.png", diff, h);

                work[i].dpath = strdup(hashpng);
                dump_png(work[i].diff, work[i].dpath);
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
        case 5: diff = argv[4];
        case 4: ugly = argv[3];
        case 3: bad  = argv[2];
        case 2: good = argv[1];
        case 1: break;
        default: fprintf(stderr, "usage: %s [good] [bad] [ugly] [diff]\n", argv[0]); return 1;
    }
    mkdir(diff, 0777);

    ftw(good, find_work, NOPENFD);

#if defined(__BLOCKS__) && defined(__APPLE__)
    dispatch_queue_t queue = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
    dispatch_apply_f(nwork, queue, NULL, do_work);
#elif defined(_OPENMP)
    #pragma omp parallel for
    for (size_t i = 0; i < nwork; i++) do_work(NULL, i);
#else
    for (size_t i = 0; i < nwork; i++) do_work(NULL, i);
#endif

    FILE* u = fopen(ugly, "w");
    assert (u);
    const char* style =
        "body { background-size: 24px 24px; "
        "       background-color: rgb(170,190,210); "
        "       background-image: "
        "   linear-gradient(45deg, rgba(255,255,255,.2) 25%, transparent 25%, transparent 50%, "
        "   rgba(255,255,255,.2) 50%, rgba(255,255,255,.2) 75%, transparent 75%, transparent) "
        "}"
        "table { table-layout:fixed; width:100% }"
        "img {max-width:100%; max-height:320 }";
    fprintf(u, "<html><style>%s</style><table>", style);

    for (enum state state = 0; state <= DIFF; state++) {
        int n = 0;
        for (size_t i = 0; i < nwork; i++) {
            n += (work[i].state == state && work[i].diffs != 0);
        }
        if (n > 0) {
            printf("%s:\n", state_name[state]);
            for (size_t i = 0; i < nwork; i++) {
                if (work[i].state == state) {
                    double diffpercent = 100.0 * work[i].diffs / work[i].diff.w / work[i].diff.h;
                    printf("\t%.2f%%\t%08x\t%s\t%s\n",
                            diffpercent, work[i].max, work[i].dpath, work[i].suffix);
                    fprintf(u, "<tr>"
                                 "<th>%.2f%% %08x"
                                 "<th>%s"
                                 "<th>%s"
                               "<tr>"
                                 "<td><a href=%s><img src=%s></a>"
                                 "<td><a href=%s><img src=%s></a>"
                                 "<td><a href=%s><img src=%s></a>",
                               diffpercent, work[i].max, work[i].gpath, work[i].bpath,
                               work[i].dpath, work[i].dpath,
                               work[i].gpath, work[i].gpath,
                               work[i].bpath, work[i].bpath);
                }
            }
        }
    }
    fclose(u);

    return 0;
}
