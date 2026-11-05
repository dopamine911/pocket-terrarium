/* sim_tmp.h - the sim's scratch files, one directory per run (pocket-terrarium P2c).
 *
 * Every selftest, shot and snapshot that needs a scratch save, a shelf, a frame directory or a wifi file asks for it
 * here instead of naming a fixed /tmp path: sim_tmp("name") is "<the run's directory>/name", the directory made with
 * mkdtemp under $TMPDIR (else /tmp) the first time anything asks, and removed - with all in it - when the run exits.
 * So two checkouts' `make check-all` at once (or two boards' `make check`, or the sim beside the tests) never share a
 * file. A test's own name for a file is all it chooses; names may hold a '/' (the directory under it is the caller's,
 * as with any path: the sim's persist port makes a save's parents itself).
 *
 * sim_tmp_keep("name") is for what a FAILING test leaves for the keeper to look at (a frame that differs): a path that
 * survives the run, $TMPDIR/pocket-terrarium-<pid>-name - unique to the run too, written only on a failure. */
#ifndef SIM_TMP_H
#define SIM_TMP_H
const char *sim_tmp_dir(void);              /* the run's directory (made on the first call) */
const char *sim_tmp(const char *name);      /* a path in it; the same name gives the same pointer for the whole run */
const char *sim_tmp_keep(const char *name); /* a path beside it that outlives the run (failure artifacts) */
#endif
