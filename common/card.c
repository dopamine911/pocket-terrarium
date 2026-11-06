/* card.c - what is shared by the card ports (card.h): the one test a name from a card must pass before any port lists it or opens it */
#include "card.h"

#if PT_CARD
/* A card is handed to the keeper by someone else: a name on it is data, not a path. Plain characters only, nothing that moves out
   of the one folder (no separator, no drive or stream colon, no "..", no leading dot or space), the one extension, and short
   enough that the whole path fits every buffer a port builds it in */
bool card_name_ok(const char *name) {
    if (!name) return false;
    size_t n = 0;
    while (n < CARD_NAME_MAX && name[n]) n++;
    if (n < 6 || n >= CARD_NAME_MAX) return false;         /* a stem of one character and ".ptpk"; a name that fills the buffer has no end */
    if (name[0] == '.' || name[0] == ' ') return false;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)name[i];
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ' ' || c == '-';
        if (!plain) return false;
        if (c == '.' && i + 1 < n && name[i + 1] == '.') return false;
    }
    const char *e = name + n - 5;                          /* FAT keeps the case a name was given: .PTPK is a pack's too */
    return e[0] == '.' && (e[1] | 0x20) == 'p' && (e[2] | 0x20) == 't' && (e[3] | 0x20) == 'p' && (e[4] | 0x20) == 'k';
}
#endif
