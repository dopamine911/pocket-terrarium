/* ptpk_tags.h - the records' ids (docs/ptpk.md), the one list the reader (ptpk.c) and the writer (ptpk_write.c) share. Private to
 * those two: the names are short on purpose, so no other file includes it. A tag is the id with bit 15 (PTPK_REQUIRED) set when
 * a loader that does not know the id must refuse the pack. 0x0003 (phase 1's WORLD) and 0x0105 (its WALK) are retired: the ids
 * stay unused - a world is a filter (WORLDS), and a walk is an entry of LOCOMOTION. */
#ifndef PTPK_TAGS_H
#define PTPK_TAGS_H

#define PTPK_REQUIRED 0x8000u

enum {
    T_NAME = 0x0001, T_UID_NS, T_LEGACY_ID = 0x0004, T_TEST_ONLY, T_WORLDS = 0x0010,
    T_HABITATS = 0x0101, T_ACTIVITY, T_HEAT, T_DRIVES, T_LIFE = 0x0106, T_SHED, T_DIET, T_BODY, T_MORPHS, T_PATTERNS, T_GOALS,
    T_LOCOMOTION = 0x0110, T_MEDIA_NEEDS,
    T_CLIMATE = 0x0201, T_BASK, T_HIDES, T_ROCKS, T_RAMPS, T_MOISTURE, T_DISH, T_AREAS = 0x0210, T_PROVIDES,
    T_MODEL_TAG = 0x0301, T_MODEL_GOALS
};
#endif
