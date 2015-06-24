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
static size_t glen, blen;

struct bitmap {
    uint32_t* pixels;
    size_t w,h;
};

static size_t nwork = 0;
static struct {
    char *gpath, *bpath, *dpath;  // on heap, clean up with free()
    struct bitmap diff;

    int diffs;
    uint32_t max;
} work[MANY];

static int find_work(const char* fpath, const struct stat* sb UNUSED, int type) {
    if (type == FTW_F) {
        size_t len = strlen(fpath);
        if (len > 4 && 0 == strcmp(".png", fpath+len-4)) {
            work[nwork].gpath = strdup(fpath);
            work[nwork].bpath = malloc(len + blen - glen + 1);
            strcat(strcpy(work[nwork].bpath, bad), fpath+glen);
            nwork++;
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

static struct bitmap diff_pngs(const uint8_t* gpng, size_t gsize,
                               const uint8_t* bpng, size_t bsize) {
    struct bitmap g = read_png(gpng, gsize),
                  b = read_png(bpng, bsize),
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
    _mm_free(g.pixels);
    _mm_free(b.pixels);
    return d;
}

static uint32_t hash(const uint32_t* p, size_t len) {
    uint32_t h = 0;
    for (size_t i = 0; i < len; i++) {
        h = _mm_crc32_u32(h, p[i]);
    }
    return h;
}

static void do_work(void* ctx UNUSED, size_t i) {
    int gfd = open(work[i].gpath, O_RDONLY),
        bfd = open(work[i].bpath, O_RDONLY);
    if (gfd >= 0 || bfd >= 0) {
        struct stat st;
        fstat(gfd, &st);
        size_t gsize = (size_t)st.st_size;
        fstat(bfd, &st);
        size_t bsize = (size_t)st.st_size;

        const uint8_t *g = mmap(0, gsize, PROT_READ, MAP_FILE|MAP_PRIVATE, gfd, 0),
                      *b = mmap(0, bsize, PROT_READ, MAP_FILE|MAP_PRIVATE, bfd, 0);
        assert (g != MAP_FAILED && b != MAP_FAILED);
        if (gsize != bsize || 0 != memcmp(g, b, gsize)) {
            work[i].diff = diff_pngs(g, gsize, b, bsize);
            struct bitmap* d = &work[i].diff;

            if (d->pixels) {
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

                work[i].diffs =           _mm_cvtsi128_si32(diffs);
                work[i].max   = (uint32_t)_mm_cvtsi128_si32(max);

                uint32_t h = hash(d->pixels, d->w*d->h);

                char hashpng[strlen(diff) + 1 + 8 + 4 + 1];
                sprintf(hashpng, "%s/%08x.png", diff, h);

                work[i].dpath = strdup(hashpng);
                dump_png(work[i].diff, work[i].dpath);
            }
        }
        munmap((void*)g, gsize);
        munmap((void*)b, bsize);
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

    glen = strlen(good);
    blen = strlen(bad);

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
        "body { background-size: 16px 16px; "
        "       background-color: rgb(230,230,230); "
        "       background-image: "
        "   linear-gradient(45deg, rgba(255,255,255,.2) 25%, transparent 25%, transparent 50%, "
        "   rgba(255,255,255,.2) 50%, rgba(255,255,255,.2) 75%, transparent 75%, transparent) "
        "}"
        "table { table-layout:fixed; width:100% }"
        "img {max-width:100%; max-height:320 }";
    fprintf(u, "<html><style>%s</style><table>", style);

    for (size_t i = 0; i < nwork; i++) {
        if (!work[i].diffs) {
            continue;
        }
        double diffpercent = 100.0 * work[i].diffs / work[i].diff.w / work[i].diff.h;
        printf("%.2f%% %08x %s\n%s\n%s\n\n",
                diffpercent, work[i].max, work[i].dpath, work[i].gpath, work[i].bpath);
        fprintf(u, "<tr><th>%.2f%% %08x"
                       "<th>%s"
                       "<th>%s"
                   "<tr><td><a href=%s><img src=%s></a>"
                       "<td><a href=%s><img src=%s></a>"
                       "<td><a href=%s><img src=%s></a>",
                diffpercent, work[i].max, work[i].gpath, work[i].bpath,
                work[i].dpath, work[i].dpath,
                work[i].gpath, work[i].gpath,
                work[i].bpath, work[i].bpath);
    }
    fclose(u);
    printf("%s\n", ugly);

    return 0;
}
