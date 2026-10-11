/*
 * unzip_test.cpp -- a host test of the launcher's zip reader (ot_unzip, the
 * step of an update that turns the downloaded release into a folder).
 *
 *   cmake -S port/launcher/app -B <build dir> -G "MinGW Makefiles" -DOT_BUILD_TESTS=ON
 *   cmake --build <build dir> --target ot_unzip_test
 *   ot_unzip_test <zip> <empty output folder>
 *
 * Prints "OK" (exit 0) when the zip was unpacked, "REFUSED: <reason>" (exit 1)
 * when ot_unzip said no; 2 = bad arguments. The caller compares the folder
 * (sha256 of each file) and checks that nothing was written outside it.
 */
#include "ot_install.h"

#include <cstdio>
#include <string>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: ot_unzip_test <zip> <output folder>\n");
        return 2;
    }
    std::string error;
    if (!ot_unzip(argv[1], argv[2], error)) {
        printf("REFUSED: %s\n", error.c_str());
        return 1;
    }
    printf("OK\n");
    return 0;
}
