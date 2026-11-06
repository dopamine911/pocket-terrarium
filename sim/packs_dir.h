/* packs_dir.h - the simulator's folder of packs (docs/ptpk.md "The loader"; issue #4 phase 1).
 *
 * The built-in packs are encoded from their const definitions when the registry first loads (common/packs/builtin.c,
 * ptpk_write.c: packs_load_builtin, through packs_add); a pack file found in a folder is offered to the
 * same loader (packs_add, PACKS_FILE) and, with a uid the registry does not have, is available to every tank:
 *   POCKET_PACKS_DIR   the folder (default ../packs: the sim runs in sim/; the repo's packs/ holds the built-in packs' own
 *                      files, which the registry already has - they are dropped without a word)
 * Every *.ptpk is read (habitats first, a species lists its habitats by uid) and refused or added: a corrupt, short, long or
 * unknown file gets one log line - `packs: <file>: REFUSED: <why>` - and nothing else happens. The firmware has no folder: it
 * encodes and loads its built-in packs at boot through the same entry (the card is phase 4). */
#ifndef PACKS_DIR_H
#define PACKS_DIR_H
#include <stdbool.h>

const char *sim_packs_dir(void);                  /* POCKET_PACKS_DIR, else ../packs */
/* offer the folder's files to the loader: how many packs were added. log: say what was added and refused (a refusal once per
   file as it is: a rescan of the same bytes is quiet). A folder that is not there adds nothing and says nothing */
int sim_packs_scan(const char *dir, bool log);
/* the frame loop's: while a tank waits for a pack (tank_t.packs_missing), the folder is read again every few seconds, so a pack
   copied in while the sim runs is found (progression_tick then resumes the tank) */
void sim_packs_poll(bool waiting, float dt);
#endif
