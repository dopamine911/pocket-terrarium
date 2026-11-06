/* ptpk_tags.h - the records' ids (docs/ptpk.md), the one list the reader (ptpk.c) and the writer (ptpk_write.c) share. Private to
 * those two: the names are short on purpose, so no other file includes it. A tag is the id with bit 15 (PTPK_REQUIRED) set when
 * a loader that does not know the id must refuse the pack. */
#ifndef PTPK_TAGS_H
#define PTPK_TAGS_H

#define PTPK_REQUIRED 0x8000u

enum {
    T_NAME = 0x0001, T_UID_NS, T_WORLD, T_LEGACY_ID, T_TEST_ONLY,
    T_HABITATS = 0x0101, T_ACTIVITY, T_HEAT, T_DRIVES, T_WALK, T_LIFE, T_SHED, T_DIET, T_BODY, T_MORPHS, T_PATTERNS, T_GOALS,
    T_CLIMATE = 0x0201, T_BASK, T_HIDES, T_ROCKS, T_RAMPS, T_MOISTURE, T_DISH
};
#endif
