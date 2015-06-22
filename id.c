#include <png.h>

// Directory names: 2 input, 1 output.
static const char *good = "good",
                  *bad  = "bad",
                  *ugly = "ugly";

int main(int argc, char** argv) {

    switch (argc) {
        case 4: ugly = argv[3];
        case 3: bad  = argv[2];
        case 2: good = argv[1];
        case 1: break;
        default: fprintf(stderr, "usage: %s [good] [bad] [ugly]\n", argv[0]); return 1;
    }


    return 0;
}
