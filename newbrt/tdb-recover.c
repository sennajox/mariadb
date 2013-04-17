/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "$Id: brt.c 11200 2009-04-10 22:28:41Z yfogel $"
#ident "Copyright (c) 2007, 2008, 2009 Tokutek Inc.  All rights reserved."
#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."

/* Recover an env.  The logs are in argv[1].  The new database is created in the cwd. */

// Test:
//    cd ../src/tests/tmpdir
//    ../../../newbrt/recover ../dir.test_log2.c.tdb

#include "includes.h"

static int recovery_main(int argc, const char *argv[]);

static void dummy(void) {}

int
main(int argc, const char *argv[]) {
    toku_brt_init(dummy, dummy);
    int r = recovery_main(argc, argv);
    toku_brt_destroy();
    return r;
}

int recovery_main (int argc, const char *argv[]) {
    const char *data_dir, *log_dir;
    if (argc==3) {
	data_dir = argv[1];
	log_dir  = argv[2];
    } else if (argc==2) {
	data_dir = log_dir = argv[1];
    } else {
	printf("Usage: %s <datadir> [ <logdir> ]\n", argv[0]);
	return(1);
    }

    int r=tokudb_recover(data_dir, log_dir);
    if (r!=0) {
	fprintf(stderr, "Recovery failed\n");
	return(1);
    }
    toku_malloc_cleanup();
    return 0;
}
