/* Round avatars for the login and lock screens: the residents everyone knows get their own colours, everyone else a
 * colour from a hash of the name, and the initial letter in the middle. All procedural; no artwork is borrowed. */
#include <kappa.h>
#include <string.h>

void k_avatar_colors(const char *name, uint32_t *bg, uint32_t *fg) {
    static const struct { const char *name; uint32_t bg, fg; } known[] = {
        { "reimu", 0xC8102E, 0xFFFFFF },    { "marisa", 0x16161F, 0xE8B84A },   { "sakuya", 0x28376E, 0xDCE3F2 },
        { "alice", 0x2F5FA8, 0xF2E6A0 },    { "patchouli", 0x5B3C8C, 0xF0E4FF }, { "remilia", 0x8E0B21, 0xFFD6D6 },
        { "flandre", 0xB5122B, 0xFFE9A8 },  { "youmu", 0x2E6B4A, 0xF0F0F0 },    { "yuyuko", 0xD98CB3, 0xFFFFFF },
        { "yukari", 0x6A3FA0, 0xFFD966 },   { "cirno", 0x6EC1F2, 0xFFFFFF },    { "nitori", 0x1E8C8C, 0xFFF6C8 },
        { "satori", 0xA0306A, 0xFFE0F0 },   { "koishi", 0x3E8F5A, 0xF7F2C0 },   { "aya", 0x22222A, 0xE8452C },
        { "eiki", 0x1C5C3A, 0xF2E6A0 },     { "keine", 0x3A5FBF, 0xFFFFFF },    { "root", 0x303030, 0xD9A441 },
        { "sumireko", 0x5A2D82, 0xF4EFD8 },
    };
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++) {
        if (!strcmp(name, known[i].name)) { *bg = known[i].bg; *fg = known[i].fg; return; }
    }
    static const uint32_t palette[] = { 0x3B6EA5, 0x7A4E9E, 0x2E8B6F, 0xB5651D, 0x8E3B46, 0x4F6D7A, 0x9E7A1F, 0x5A7D2B };
    unsigned h = 5381;
    for (const char *p = name; *p; p++) h = h * 33 + (unsigned char)*p;
    *bg = palette[h % (sizeof palette / sizeof palette[0])];
    *fg = 0xFFF6E0;
}

/* ring: 0 plain, 1 hovered, 2 selected (gold). The avatar is 62 px across, 70 with the ring. */
void k_avatar(KCanvas *cv, int cx, int cy, const char *name, int ring) {
    uint32_t bg, fg;
    k_avatar_colors(name, &bg, &fg);
    kc_circle_fill(cv, cx, cy, 35, ring == 2 ? KC_GOLD : ring == 1 ? 0x6C77B8 : 0x2A3566);
    kc_circle_fill(cv, cx, cy, 31, bg);
    char c = name[0];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    char initial[2] = { c, 0 };
    kc_text_scaled(cv, cx - 8, cy - 16, initial, fg, 2);
}
