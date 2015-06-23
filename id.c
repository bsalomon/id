#include <dispatch/dispatch.h>
#include <ftw.h>
#include <png.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define UNUSED __attribute__((unused))

#define MANY 1024*1024
#define NOPENFD 100

static const char *good = "good", *bad  = "bad", *ugly = "ugly";

static size_t nwork = 0;
static const char* work[MANY];  // each pointer on heap, cleanup with free()

static int find_work(const char* fpath, const struct stat* sb UNUSED, int typeflag) {
    if (typeflag == FTW_F) {
        const char* suffix = fpath + strlen(good);
        size_t len = strlen(suffix);
        if (len > 4 && 0 == strcmp(".png", suffix+len-4)) {
            work[nwork++] = strdup(suffix);
        }
    }
    return 0;
}

static void do_work(void* ctx UNUSED, size_t i) {
    size_t len = strlen(work[i]);
    char gpath[strlen(good) + len + 1],
         bpath[strlen(bad)  + len + 1];
    strcat(strcpy(gpath, good), work[i]);
    strcat(strcpy(bpath,  bad), work[i]);

    int gfd = open(gpath, O_RDONLY),
        bfd = open(bpath, O_RDONLY);
    if (gfd >= 0 && bfd >= 0) {
        struct stat st;
        fstat(gfd, &st);
        size_t glen = (size_t)st.st_size;
        fstat(bfd, &st);
        size_t blen = (size_t)st.st_size;

        const char *g = mmap(0, glen, PROT_READ, MAP_FILE|MAP_PRIVATE, gfd, 0),
                   *b = mmap(0, blen, PROT_READ, MAP_FILE|MAP_PRIVATE, bfd, 0);
        if (g != MAP_FAILED && b != MAP_FAILED) {
            if (glen != blen || 0 != memcmp(g, b, glen)) {
                printf("%s %zu %zu\n", work[i], glen, blen);
            }
        }
        if (g != MAP_FAILED) munmap((void*)g, glen);
        if (b != MAP_FAILED) munmap((void*)b, blen);
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

    (void)ftw(good, find_work, NOPENFD);

    dispatch_queue_t queue = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
    dispatch_apply_f(nwork, queue, NULL, do_work);

    return 0;
}
