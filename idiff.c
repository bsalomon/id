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
static size_t glen, blen, dlen;

static size_t nwork = 0;
static struct {
    char *gpath, *bpath, *dpathA, *dpathB;  // on heap, clean up with free()
    size_t pixels, diffs;
    uint32_t max;

    int padding;
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

struct bitmap {
    uint32_t* pixels;
    size_t w,h;
};

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

static void diff_pngs(const uint8_t* gpng, size_t gsize, const uint8_t* bpng, size_t bsize,
                      struct bitmap* dA, struct bitmap* dB) {
    struct bitmap g = read_png(gpng, gsize),
                  b = read_png(bpng, bsize);
    if (!g.pixels || !b.pixels || g.w != b.w || g.h != b.h) {
        _mm_free(g.pixels);
        _mm_free(b.pixels);
        *dA = (struct bitmap) { NULL, 0, 0 };
        *dB = (struct bitmap) { NULL, 0, 0 };
        return;
    }

    size_t p = 0, n = g.w*g.h;
    for (; p < n/4*4; p += 4) {
        __m128i g4 = _mm_load_si128((const __m128i*)(g.pixels+p)),
                b4 = _mm_load_si128((const __m128i*)(b.pixels+p)),
                d4 = _mm_or_si128(_mm_subs_epu8(g4,b4), _mm_subs_epu8(b4,g4));
        _mm_store_si128((__m128i*)(g.pixels+p), d4);
        //_mm_store_si128((__m128i*)(b.pixels+p), d4);
    }
    for (; p < n; p++) {
        __m128i g1 = _mm_cvtsi32_si128((int)g.pixels[p]),
                b1 = _mm_cvtsi32_si128((int)b.pixels[p]),
                d1 = _mm_or_si128(_mm_subs_epu8(g1,b1), _mm_subs_epu8(b1,g1));
        g.pixels[p] = (uint32_t)_mm_cvtsi128_si32(d1);
        //b.pixels[p] = (uint32_t)_mm_cvtsi128_si32(d1);
    }
    *dA = g;
    *dB = b;
}

static uint32_t hash(const uint32_t* pix, size_t n) {
    uint32_t h = 0;
    for (size_t p = 0; p < n; p++) {
        h = _mm_crc32_u32(h, pix[p]);
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
            struct bitmap dA, dB;
            diff_pngs(g, gsize, b, bsize, &dA, &dB);

            if (dA.pixels) {
                __m128i zero  = _mm_setzero_si128(),
                        diffs = zero,
                        max   = zero;
                size_t p = 0, n = dA.w*dA.h;
                for (; p < n/4*4; p += 4) {
                    __m128i d4 = _mm_load_si128((const __m128i*)(dA.pixels+p));

                    diffs = _mm_add_epi32(diffs,
                            _mm_add_epi32(_mm_set1_epi32(1), _mm_cmpeq_epi32(d4, zero)));
                    max = _mm_max_epu8(max, d4);

                    _mm_store_si128((__m128i*)(dA.pixels+p),
                            _mm_xor_si128(_mm_set1_epi32((int)0xff000000), d4));
                    _mm_store_si128((__m128i*)(dB.pixels+p),
                            _mm_xor_si128(_mm_set1_epi32(0x00ffffff), _mm_cmpeq_epi8(d4, zero)));
                }
                for (; p < n; p++) {
                    __m128i d1 = _mm_cvtsi32_si128((int)dA.pixels[p]);

                    diffs = _mm_add_epi32(diffs,
                            _mm_add_epi32(_mm_set1_epi32(1), _mm_cmpeq_epi32(d1, zero)));
                    max = _mm_max_epu8(max, d1);

                    dA.pixels[p] = (uint32_t)_mm_cvtsi128_si32(
                            _mm_xor_si128(_mm_set1_epi32((int)0xff000000), d1));
                    dB.pixels[p] = (uint32_t)_mm_cvtsi128_si32(
                            _mm_xor_si128(_mm_set1_epi32(0x00ffffff), _mm_cmpeq_epi8(d1, zero)));
                }
                diffs = _mm_add_epi32(diffs, _mm_srli_si128(diffs, 8));
                diffs = _mm_add_epi32(diffs, _mm_srli_si128(diffs, 4));
                max = _mm_max_epu8(max, _mm_srli_si128(max, 8));
                max = _mm_max_epu8(max, _mm_srli_si128(max, 4));

                work[i].pixels = n;
                work[i].diffs  = (  size_t)_mm_cvtsi128_si32(diffs);
                work[i].max    = (uint32_t)_mm_cvtsi128_si32(max);

                uint32_t h;
                char hashpng[dlen + 1 + 8 + 4 + 1];

                h = hash(dA.pixels, n);
                sprintf(hashpng, "%s/%08x.png", diff, h);
                work[i].dpathA = strdup(hashpng);
                if (0 != stat(hashpng, &st)) {
                    dump_png(dA, work[i].dpathA);
                }

                h = hash(dB.pixels, n);
                sprintf(hashpng, "%s/%08x.png", diff, h);
                work[i].dpathB = strdup(hashpng);
                if (0 != stat(hashpng, &st)) {
                    dump_png(dB, work[i].dpathB);
                }
            }
            _mm_free(dA.pixels);
            _mm_free(dB.pixels);
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
    dlen = strlen(diff);

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
        if (work[i].diffs == 0) {
            continue;
        }
        double diffpercent = 100.0 * work[i].diffs / work[i].pixels;
        printf("%5.2f%% %08x %s %s\n",
                diffpercent, work[i].max, work[i].dpathB, work[i].dpathA);
        fprintf(u, "<tr><th>%5.2f%%"
                       "<th>%08x"
                       "<th>%s"
                       "<th>%s"
                   "<tr><td><a href=%s><img src=%s></a>"
                       "<td><a href=%s><img src=%s></a>"
                       "<td><a href=%s><img src=%s></a>"
                       "<td><a href=%s><img src=%s></a>",
                diffpercent, work[i].max, work[i].gpath, work[i].bpath,
                work[i].dpathB, work[i].dpathB,
                work[i].dpathA, work[i].dpathA,
                work[i].gpath, work[i].gpath,
                work[i].bpath, work[i].bpath);
    }
    fclose(u);
    printf("%s\n", ugly);

    for (size_t i = 0; i < nwork; i++) {
        free(work[i].gpath);
        free(work[i].bpath);
        free(work[i].dpathA);
        free(work[i].dpathB);
    }

    return 0;
}
