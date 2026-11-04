/* hdatest: checks the Intel HDA output planner (kernel/drivers/audio/hda_graph.c) against invented codec descriptions.
 * The test itself is tests/host/hdatest.c; this file only builds it as a guest program (the host here has no C library
 * headers to build it with). Run `hdatest`: it prints each fixture's plan and "N checks, 0 failed". */
#include "../../../tests/host/hdatest.c"
