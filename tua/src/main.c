#include "tuac_cli.h"
#include "rt/rt_config.h"

int main(int argc, char* argv[]) {
    tua_rt_configure(NULL);
    return tuac_main(argc, argv);
}
