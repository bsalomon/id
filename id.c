#include <dispatch/dispatch.h>
#include <ftw.h>
#include <png.h>
#include <stdio.h>
#include <string.h>

#define UNUSED __attribute__((unused))

#define MANY 1024*1024
#define NOPENFD 100

static const char *good = "good",
                  *bad  = "bad",
                  *ugly = "ugly";

static size_t nwork = 0;
static struct work {
    const char* suffix;  // on heap, cleanup with free()
} work[MANY];

static int find_work(const char* fpath, const struct stat* sb UNUSED, int typeflag) {
    if (typeflag == FTW_F) {
        const char* suffix = fpath + strlen(good);
        size_t len = strlen(suffix);
        if (len > 4 && 0 == strcmp(".png", suffix+len-4)) {
            work[nwork].suffix = strdup(suffix);
            nwork++;
        }
    }
    return 0;
}

static void do_work(size_t i) {
    printf("%s\n", work[i].suffix);
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
    dispatch_apply(nwork, queue, ^(size_t i) { do_work(i); });

    return 0;
}
