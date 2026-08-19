#include <dirent.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "app.h"
#include "codec.h"

static s32 rd32(SaveCtx *ctx, u32 off) { s32 v; memcpy(&v, ctx->plain + off, 4); return v; }
static void wr32(SaveCtx *ctx, u32 off, s32 v) { memcpy(ctx->plain + off, &v, 4); }
static s16 rd16(SaveCtx *ctx, u32 off) { s16 v; memcpy(&v, ctx->plain + off, 2); return v; }
static void wr16(SaveCtx *ctx, u32 off, s16 v) { memcpy(ctx->plain + off, &v, 2); }

#define PICK_MAX 4096
static const void *pick_ptr[PICK_MAX];
static char pick_lab[PICK_MAX][40];
static const char *pick_lines[PICK_MAX];

static bool name_match(const char *name, const char *filt)
{
    if (!filt[0]) return true;
    size_t fl = strlen(filt);
    for (const char *p = name; *p; p++) {
        size_t i = 0;
        while (i < fl && p[i] &&
               ((p[i] | 0x20) == (filt[i] | 0x20))) i++;
        if (i == fl) return true;
    }
    return false;
}

static int pick_cmp(const void *a, const void *b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    while (*x && *y) {
        int cx = (*x >= 'a' && *x <= 'z') ? *x - 32 : *x;
        int cy = (*y >= 'a' && *y <= 'z') ? *y - 32 : *y;
        if (cx != cy) return cx - cy;
        x++; y++;
    }
    return (int)(u8)*x - (int)(u8)*y;
}

static int pick_cmp_idx(const void *a, const void *b)
{
    return pick_cmp(&pick_lines[*(const int *)a], &pick_lines[*(const int *)b]);
}

/* labels start with a group tag, so sorting groups then orders by name */
static void pick_sort(int n)
{
    static int order[PICK_MAX];
    static const char *tmp_lines[PICK_MAX];
    static const void *tmp_ptr[PICK_MAX];
    for (int i = 0; i < n; i++) order[i] = i;
    qsort(order, n, sizeof(int), pick_cmp_idx);
    for (int i = 0; i < n; i++) {
        tmp_lines[i] = pick_lines[order[i]];
        tmp_ptr[i] = pick_ptr[order[i]];
    }
    memcpy(pick_lines, tmp_lines, (size_t)n * sizeof(*pick_lines));
    memcpy(pick_ptr, tmp_ptr, (size_t)n * sizeof(*pick_ptr));
}

/* ---- secret link ---- */

bool link_apply(SaveCtx *ctx, int sel)
{
    const GameDef *g = ctx->game;
    int current = ctx->plain[g->link_off];
    if (sel == current) return false;
    char msg[160];
    snprintf(msg, sizeof(msg), "Set link level %d -> %d?%s", current, sel,
             (sel == 3) ? "\n\nLevel 3 REQUIRES the version-exclusive\nteam beaten. Glitched save otherwise." : "");
    if (!ui_dialog((sel == 3) ? "I beat it, proceed" : "confirm", msg, sel == 3)) return false;
    ui_header();
    ctx->plain[g->link_off] = (u8)sel;
    if (apply_changes(ctx)) return true;
    ctx->plain[g->link_off] = (u8)current;
    return false;
}

void link_level_editor(SaveCtx *ctx)
{
    const GameDef *g = ctx->game;

    if (g->link_kind == LINK_GO_WORD) {
        /* GO: single "link unlocked" flag, from the reference editor */
        if (!ui_dialog("unlock", "Unlock the secret link (level 3)?\n\nUndo only via backup restore.", false))
            return;
        ui_header();
        wr16(ctx, g->link_off, 0x01C0);
        wr16(ctx, g->link_off + 2, 0x0000);
        apply_changes(ctx);
        return;
    }

    int chapter = g->chapter_off ? ctx->plain[g->chapter_off] : 99;
    int current = ctx->plain[g->link_off];
    int max = (chapter < 10) ? 2 : 3;
    int sel = (current > max) ? max : current;
    bool dirty = true;

    while (aptMainLoop()) {
        if (dirty) {
            ui_header();
            printf(C_KEY " Secret link level" C_RESET "\n\n");
            printf("  current: %d\n", current);
            printf("  new:   %s " C_VAL "%d" C_RESET " %s\n\n",
                   (sel > 0) ? "<" : " ", sel, (sel < max) ? ">" : " ");
            if (chapter < 10)
                printf(C_DIM " Level 3 locked: chapter < 10." C_RESET "\n");
            else
                printf(C_DIM " Level 3 needs the version-exclusive\n team beaten, else the save glitches." C_RESET "\n");
            printf("\x1b[28;1H" C_DIM " LEFT/RIGHT adjust   A apply   B back" C_RESET);
            dirty = false;
        }
        hidScanInput();
        u32 k = hidKeysDown();
        u32 kr = hidKeysDownRepeat();
        if (k & (KEY_B | KEY_START)) return;
        if (kr & KEY_LEFT)  { if (sel > 0) sel--; dirty = true; }
        if (kr & KEY_RIGHT) { if (sel < max) sel++; dirty = true; }
        if ((k & KEY_A) && sel != current) {
            if (link_apply(ctx, sel)) return;
            current = ctx->plain[g->link_off];
            dirty = true;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
}

void sdlink_unlock(SaveCtx *ctx)
{
    const GameDef *g = ctx->game;
    ui_header();
    if (g->chapter_off && ctx->plain[g->chapter_off] < 2) {
        ui_notice("Refused: requires chapter >= 2.", false);
        return;
    }
    if (ctx->size < g->unlock_end + 8) {
        ui_notice("Refused: save smaller than expected.", false);
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "%s?\n\nUndo only via backup restore.", g->unlock_label);
    if (!ui_dialog("unlock", msg, false)) return;
    ui_header();
    for (int i = 0; i < g->unlock_n; i++)
        memcpy(ctx->plain + g->unlock[i].offset, g->unlock[i].data, g->unlock[i].len);
    apply_changes(ctx);
}

/* ---- save info ---- */

static void read_name_buf(const u8 *buf, u32 off, char *out, size_t outsz)
{
    size_t o = 0, i = 0;
    while (o < outsz - 1 && i < NAME_FIELD_LEN && buf[off + i]) {
        u8 c = buf[off + i];
        if (c < 0x80) {
            out[o++] = (char)c;
            i++;
        } else {
            /* one placeholder per UTF-8 codepoint (JP names can't render) */
            out[o++] = '?';
            i++;
            while (i < NAME_FIELD_LEN && (buf[off + i] & 0xC0) == 0x80) i++;
        }
    }
    out[o] = 0;
}

static void read_name(SaveCtx *ctx, u32 off, char *out, size_t outsz)
{
    read_name_buf(ctx->plain, off, out, outsz);
}

static void write_name(SaveCtx *ctx, u32 off, const char *name)
{
    memset(ctx->plain + off, 0, NAME_FIELD_LEN);
    size_t l = strlen(name);
    if (l > NAME_FIELD_LEN - 2) l = NAME_FIELD_LEN - 2;
    memcpy(ctx->plain + off, name, l);
    ctx->plain[off + l] = 0x00;
    ctx->plain[off + l + 1] = 0x88;
}

enum { F_NAME, F_TEAM, F_TIME, F_PRESTIGE, F_FRIEND, F_COIN0 };

/* records are stored one byte per group; record j of a group with cnt
 * entries sits at bit (cnt-1-j), mirroring the reference editor's BitArray */
static int record_bit(const GameDef *g, int idx)
{
    int grp = g->rdb[idx].group;
    int cnt = 0, j = -1;
    for (int i = 0; i < g->rdb_count; i++) {
        if (g->rdb[i].group != grp) continue;
        if (i == idx) j = cnt;
        cnt++;
    }
    return cnt - 1 - j;
}

void records_unlock(SaveCtx *ctx)
{
    const GameDef *g = ctx->game;
    if (!g->records_n || !g->rdb_count || ctx->size < g->records_off + (u32)g->records_n) {
        ui_header();
        ui_notice("Play records unknown for this game.", false);
        return;
    }

    u8 *snap = malloc((size_t)g->records_n);
    memcpy(snap, ctx->plain + g->records_off, (size_t)g->records_n);

    int cursor = 0;
    while (aptMainLoop()) {
        static char rows[128][48];
        const char *lines[128];
        snprintf(rows[0], 48, "[ Unlock all ]");
        lines[0] = rows[0];
        int n = 1;
        for (int i = 0; i < g->rdb_count && n < 128; i++) {
            u8 byte = ctx->plain[g->records_off + g->rdb[i].group];
            bool on = (byte >> record_bit(g, i)) & 1;
            snprintf(rows[n], 48, "[%c] %s", on ? 'x' : ' ', g->rdb[i].name);
            lines[n] = rows[n];
            n++;
        }
        int pick = ui_list("Play records (A: toggle)", lines, n, cursor);
        if (pick < 0) break;
        cursor = pick;
        if (pick == 0) {
            memset(ctx->plain + g->records_off, 0xFF, (size_t)g->records_n);
            continue;
        }
        int i = pick - 1;
        ctx->plain[g->records_off + g->rdb[i].group] ^= (u8)(1 << record_bit(g, i));
    }

    if (memcmp(snap, ctx->plain + g->records_off, (size_t)g->records_n) != 0) {
        if (!apply_changes(ctx))
            memcpy(ctx->plain + g->records_off, snap, (size_t)g->records_n);
    }
    free(snap);
}

void saveinfo_editor(SaveCtx *ctx)
{
    const GameDef *g = ctx->game;
    static const char *coin_names[5] = { "Bronze", "Silver", "Gold", "Platinum", "Rainbow" };
    bool modified = false;
    int cursor = 0;
    u8 *si_snap = malloc(ctx->size);
    memcpy(si_snap, ctx->plain, ctx->size);

    while (aptMainLoop()) {
        char name[32], team[32];
        read_name(ctx, g->name_off, name, sizeof(name));
        read_name(ctx, g->team_off, team, sizeof(team));
        int secs = rd32(ctx, g->time_off);

        char rows[10][48];
        int fields[10];
        int n = 0;
        snprintf(rows[n], 48, "Name       %s", name); fields[n++] = F_NAME;
        snprintf(rows[n], 48, "Team name  %s", team); fields[n++] = F_TEAM;
        snprintf(rows[n], 48, "Play time  %dh %02dm", secs / 3600, secs / 60 % 60); fields[n++] = F_TIME;
        snprintf(rows[n], 48, "Prestige   %ld", (long)rd32(ctx, g->money_off)); fields[n++] = F_PRESTIGE;
        if (g->has_friendship) {
            snprintf(rows[n], 48, "Friendship %ld", (long)rd32(ctx, g->money_off + 4));
            fields[n++] = F_FRIEND;
        }
        if (g->coin_off)
            for (int i = 0; i < 5; i++) {
                snprintf(rows[n], 48, "%-8s   %d", coin_names[i], rd16(ctx, g->coin_off + i * 2));
                fields[n++] = F_COIN0 + i;
            }
        const char *lines[10];
        for (int i = 0; i < n; i++) lines[i] = rows[i];

        int delta = 0;
        int pick = ui_list_adj("Save info (B: back)", lines, n, cursor, &delta);
        if (pick < 0) break;
        cursor = pick;

        int v;
        char text[32];
        switch (fields[pick]) {
        case F_NAME:
            if (!delta && ui_text("Player name", name, text, 22)) { write_name(ctx, g->name_off, text); modified = true; }
            break;
        case F_TEAM:
            if (!delta && ui_text("Team name", team, text, 22)) { write_name(ctx, g->team_off, text); modified = true; }
            break;
        case F_TIME:
            v = secs / 3600 + delta;
            if (delta) {
                if (v < 0 || v > 999) break;
            } else if (!ui_number("Play time: hours", secs / 3600, 0, 999, &v)) {
                break;
            }
            wr32(ctx, g->time_off, v * 3600 + (secs / 60 % 60) * 60);
            modified = true;
            break;
        case F_PRESTIGE:
            v = rd32(ctx, g->money_off) + delta * 100;
            if (delta) {
                if (v < 0 || v > 9999999) break;
            } else if (!ui_number("Prestige points", rd32(ctx, g->money_off), 0, 9999999, &v)) {
                break;
            }
            wr32(ctx, g->money_off, v);
            modified = true;
            break;
        case F_FRIEND:
            v = rd32(ctx, g->money_off + 4) + delta * 100;
            if (delta) {
                if (v < 0 || v > 9999999) break;
            } else if (!ui_number("Friendship points", rd32(ctx, g->money_off + 4), 0, 9999999, &v)) {
                break;
            }
            wr32(ctx, g->money_off + 4, v);
            modified = true;
            break;
        default: {
            int i = fields[pick] - F_COIN0;
            v = rd16(ctx, g->coin_off + i * 2) + delta;
            if (delta) {
                if (v < 0 || v > 9999) break;
            } else if (!ui_number(coin_names[i], rd16(ctx, g->coin_off + i * 2), 0, 9999, &v)) {
                break;
            }
            wr16(ctx, g->coin_off + i * 2, (s16)v);
            modified = true;
            break;
        }
        }
    }

    if (modified && !apply_changes(ctx)) {
        /* reverted below from the snapshot */
        memcpy(ctx->plain, si_snap, ctx->size);
    }
    free(si_snap);
}

/* ---- players ---- */

static const PlayerInfo *player_info(const GameDef *g, u32 id)
{
    int lo = 0, hi = g->db_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g->db[mid].id == id) return &g->db[mid];
        if (g->db[mid].id < id) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* invested-point order mapped to Stat[2..9] per the reference editor */
static const char *STAT_NAMES[8] = { "Kick", "Dribble", "Technique", "Block",
                                     "Speed", "Stamina", "Catch", "Luck" };

static void level_gp_tp(SaveCtx *ctx, u32 blk, const PlayerInfo *pi, int level)
{
    const GameDef *g = ctx->game;
    ctx->plain[blk + g->p_gp_off + 6] = (u8)level;
    if (pi && pi->gp) {
        /* linear growth approximation (exact at 99); never lowers stored values,
         * since real in-game growth outpaces the linear curve */
        int gp = pi->gp * level / 99;
        int tp = pi->tp * level / 99;
        if (gp > rd16(ctx, blk + g->p_gp_off)) wr16(ctx, blk + g->p_gp_off, (s16)gp);
        if (tp > rd16(ctx, blk + g->p_gp_off + 2)) wr16(ctx, blk + g->p_gp_off + 2, (s16)tp);
    }
}

/* seesaw pattern: which stat drops when training past freedom, per position.
 * Bidirectional pairs, stat order: Kick Dribble Technique Block Speed Stamina
 * Catch Luck. GK: Kick<->Luck, Dribble<->Block, Catch<->Technique, Speed<->Stamina;
 * DF: Kick<->Catch, Dribble<->Luck, Block<->Technique; MF: Kick<->Catch,
 * Dribble<->Technique, Block<->Luck; FW: Kick<->Technique, Dribble<->Block,
 * Catch<->Luck; Speed<->Stamina for all. */
static const signed char SEESAW[4][8] = {
    /* GK */ { 7, 3, 6, 1, 5, 4, 2, 0 },
    /* DF */ { 6, 7, 3, 2, 5, 4, 0, 1 },
    /* MF */ { 6, 2, 1, 7, 5, 4, 0, 3 },
    /* FW */ { 2, 3, 0, 1, 5, 4, 7, 6 },
};

/* the reference data only contains each stat's value at Lv 99 (untrained);
 * real growth curves live in the game ROM, so no per-level estimate is shown */
static int stat_base(const PlayerInfo *pi, int i, int level)
{
    (void)level;
    return pi->st[i];
}

static int pos_index(const PlayerInfo *pi)
{
    if (!strcmp(pi->pos, "GK")) return 0;
    if (!strcmp(pi->pos, "DF")) return 1;
    if (!strcmp(pi->pos, "MF")) return 2;
    if (!strcmp(pi->pos, "FW")) return 3;
    return -1;
}

/* +1 on stat i following game rules; returns false if cancelled */
static bool train_plus(SaveCtx *ctx, u32 blk, const PlayerInfo *pi, int i)
{
    const GameDef *g = ctx->game;
    int freedom = rd16(ctx, blk + g->p_gp_off + 4);
    if (freedom > 0) {
        wr16(ctx, blk + g->p_invest_off + i * 2, (s16)(rd16(ctx, blk + g->p_invest_off + i * 2) + 1));
        wr16(ctx, blk + g->p_gp_off + 4, (s16)(freedom - 1));
        return true;
    }
    int p = pos_index(pi);
    int victim = (p >= 0) ? SEESAW[p][i] : -1;
    if (victim >= 0 &&
        stat_base(pi, victim, ctx->plain[blk + g->p_gp_off + 6]) +
            rd16(ctx, blk + g->p_invest_off + victim * 2) - 1 < 1)
        return false;
    if (victim < 0) {
        /* pair unknown: let the user choose the stat to lower */
        const char *lines[8];
        for (int s = 0; s < 8; s++) lines[s] = STAT_NAMES[s];
        int pick = ui_list("No freedom left: lower which stat?", lines, 8, (i + 1) % 8);
        if (pick < 0 || pick == i) return false;
        victim = pick;
    }
    wr16(ctx, blk + g->p_invest_off + i * 2, (s16)(rd16(ctx, blk + g->p_invest_off + i * 2) + 1));
    wr16(ctx, blk + g->p_invest_off + victim * 2, (s16)(rd16(ctx, blk + g->p_invest_off + victim * 2) - 1));
    return true;
}


/* write a fresh player block: DB defaults, chosen level, given roster index */
static void write_player_block(SaveCtx *ctx, u32 blk, const PlayerInfo *pi,
                               int level, int index)
{
    const GameDef *g = ctx->game;
    memset(ctx->plain + blk, 0, (size_t)g->pblock);
    wr32(ctx, blk + g->p_id_off - 4, index);
    memcpy(ctx->plain + blk + g->p_id_off, &pi->id, 4);
    wr16(ctx, blk + g->p_gp_off + 4, (s16)pi->freedom);
    level_gp_tp(ctx, blk, pi, level);
    for (int m = 0; m < 4; m++) {
        u32 e = blk + g->p_moves_off + (u32)m * 12;
        memcpy(ctx->plain + e, &pi->moves[m], 4);
        ctx->plain[e + 4] = 1;   /* move level */
        ctx->plain[e + 6] = 1;   /* learned (4-byte bool, LE) */
    }
}

/* next roster index following the PC editor's scheme: both 16-bit halves
 * of the last index are incremented until unique */
static int next_player_index(SaveCtx *ctx, int count)
{
    const GameDef *g = ctx->game;
    int idx = rd32(ctx, g->pdata_off + (u32)(count - 1) * g->pblock + g->p_id_off - 4);
    bool taken = true;
    while (taken) {
        s16 lo = (s16)(idx & 0xFFFF), hi = (s16)((idx >> 16) & 0xFFFF);
        lo++; hi++;
        idx = (int)(u16)lo | ((int)(u16)hi << 16);
        taken = false;
        for (int i = 0; i < count; i++)
            if (rd32(ctx, g->pdata_off + (u32)i * g->pblock + g->p_id_off - 4) == idx)
                taken = true;
    }
    return idx;
}

static const PlayerInfo *player_db_picker(SaveCtx *ctx, bool skip_owned)
{
    const GameDef *g = ctx->game;
    char filt[24] = "";
    int cursor = 0;
    while (aptMainLoop()) {
        int n = 0;
        for (int i = 0; i < g->db_count && n < PICK_MAX; i++) {
            const PlayerInfo *pi = &g->db[i];
            if (!name_match(pi->name, filt)) continue;
            if (skip_owned) {
                bool owned = false;
                for (int k = 0; k < g->pmax && !owned; k++) {
                    u32 id;
                    memcpy(&id, ctx->plain + g->pdata_off + (u32)k * g->pblock + g->p_id_off, 4);
                    if (id == pi->id) owned = true;
                }
                if (owned) continue;
            }
            pick_ptr[n] = pi;
            snprintf(pick_lab[n], 40, "%-2s %-4s %s", pi->pos, pi->elem, pi->name);
            pick_lines[n] = pick_lab[n];
            n++;
        }
        if (!n) {
            ui_header();
            ui_notice(filt[0] ? "No match; search again." : "No player available.", false);
            if (!filt[0]) return NULL;
            filt[0] = 0;
            continue;
        }
        pick_sort(n);
        int delta = 0;
        int pick = ui_list_adj(filt[0] ? filt : "Pick a player (Y: search)", pick_lines, n, cursor, &delta);
        if (pick < 0) return NULL;
        cursor = pick;
        if (delta == 3) { ui_text_opt("Search", filt, sizeof(filt)); cursor = 0; continue; }
        if (delta) continue;
        return (const PlayerInfo *)pick_ptr[pick];
    }
    return NULL;
}

static const MoveInfo *move_info(const GameDef *g, u32 id)
{
    int lo = 0, hi = g->mdb_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g->mdb[mid].id == id) return &g->mdb[mid];
        if (g->mdb[mid].id < id) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static const MoveInfo *move_db_picker(SaveCtx *ctx)
{
    const GameDef *g = ctx->game;
    char filt[24] = "";
    int cursor = 0;
    while (aptMainLoop()) {
        int n = 0;
        for (int i = 0; i < g->mdb_count && n < PICK_MAX; i++) {
            const MoveInfo *mi = &g->mdb[i];
            if (!name_match(mi->name, filt)) continue;
            pick_ptr[n] = mi;
            snprintf(pick_lab[n], 40, "%-2s %s", mi->kind, mi->name);
            pick_lines[n] = pick_lab[n];
            n++;
        }
        if (!n) {
            ui_header();
            ui_notice("No match; search again.", false);
            filt[0] = 0;
            continue;
        }
        pick_sort(n);
        int delta = 0;
        int pick = ui_list_adj(filt[0] ? filt : "Pick a move (Y: search)", pick_lines, n, cursor, &delta);
        if (pick < 0) return NULL;
        cursor = pick;
        if (delta == 3) { ui_text_opt("Search", filt, sizeof(filt)); cursor = 0; continue; }
        if (delta) continue;
        return (const MoveInfo *)pick_ptr[pick];
    }
    return NULL;
}

static void moves_editor(SaveCtx *ctx, u32 blk, const char *pname)
{
    const GameDef *g = ctx->game;
    int cursor = 0;
    while (aptMainLoop()) {
        char rows[6][48];
        const char *lines[6];
        for (int s = 0; s < 6; s++) {
            u32 e = blk + g->p_moves_off + (u32)s * 12;
            u32 id;
            memcpy(&id, ctx->plain + e, 4);
            const MoveInfo *mi = id ? move_info(g, id) : NULL;
            if (!id)
                snprintf(rows[s], 48, "%d  (empty%s)", s + 1, (s < 4) ? "" : " extra slot");
            else if (mi)
                snprintf(rows[s], 48, "%d  %-2s %-22s Lv%d", s + 1, mi->kind, mi->name, ctx->plain[e + 4]);
            else
                snprintf(rows[s], 48, "%d  ?? %08lX Lv%d", s + 1, (unsigned long)id, ctx->plain[e + 4]);
            lines[s] = rows[s];
        }

        int delta = 0;
        int pick = ui_list_adj(pname, lines, 6, cursor, &delta);
        if (pick < 0) return;
        cursor = pick;
        u32 e = blk + g->p_moves_off + (u32)pick * 12;
        u32 id;
        memcpy(&id, ctx->plain + e, 4);

        if (delta == 2) {
            /* X: clear an extra slot; innate moves stay */
            if (pick < 4 || !id) continue;
            if (ui_dialog("clear slot", "Remove this extra move?", false))
                memset(ctx->plain + e, 0, 12);
            continue;
        }
        if (delta) {
            if (!id) continue;
            int lv = ctx->plain[e + 4] + delta;
            if (lv >= 1 && lv <= 5) ctx->plain[e + 4] = (u8)lv;
            continue;
        }
        bool replace = !id;
        if (!replace) {
            char lstate[24];
            snprintf(lstate, sizeof(lstate), "Learned: %s", ctx->plain[e + 6] ? "yes" : "no");
            const char *acts[] = { "Set move level", "Replace move", lstate, "Set usage count" };
            int a = ui_list(lines[pick], acts, 4, 0);
            if (a < 0) continue;
            if (a == 0) {
                int v;
                if (ui_number("Move level (1-5)", ctx->plain[e + 4], 1, 5, &v))
                    ctx->plain[e + 4] = (u8)v;
                continue;
            }
            if (a == 2) { ctx->plain[e + 6] = !ctx->plain[e + 6]; continue; }
            if (a == 3) {
                int v;
                if (ui_number("Usage count", ctx->plain[e + 5], 0, 255, &v))
                    ctx->plain[e + 5] = (u8)v;
                continue;
            }
            replace = true;
        }
        {
            const MoveInfo *mi = move_db_picker(ctx);
            if (!mi) continue;
            memset(ctx->plain + e, 0, 12);
            memcpy(ctx->plain + e, &mi->id, 4);
            ctx->plain[e + 4] = 1;
            ctx->plain[e + 6] = 1; /* learned */
        }
    }
}

static void avatar_editor(SaveCtx *ctx, u32 blk, const char *pname)
{
    const GameDef *g = ctx->game;
    char filt[24] = "";
    int cursor = 0;
    const AvatarInfo *ai = NULL;
    while (aptMainLoop()) {
        pick_lines[0] = "[ None (remove avatar) ]";
        pick_ptr[0] = NULL;
        int n = 1;
        for (int i = 0; i < g->adb_count && n < PICK_MAX; i++) {
            const AvatarInfo *a = &g->adb[i];
            if (!a->spirit && !g->totem_off) continue; /* totems are Galaxy-only */
            if (!name_match(a->name, filt)) continue;
            pick_ptr[n] = a;
            snprintf(pick_lab[n], 40, "%-6s %s", a->spirit ? "Spirit" : "Totem", a->name);
            pick_lines[n] = pick_lab[n];
            n++;
        }
        if (n > 2) {
            /* keep [None] on top, sort the rest */
            static const char *save0;
            save0 = pick_lines[0];
            memmove(pick_lines, pick_lines + 1, (size_t)(n - 1) * sizeof(*pick_lines));
            memmove(pick_ptr, pick_ptr + 1, (size_t)(n - 1) * sizeof(*pick_ptr));
            pick_sort(n - 1);
            memmove(pick_lines + 1, pick_lines, (size_t)(n - 1) * sizeof(*pick_lines));
            memmove(pick_ptr + 1, pick_ptr, (size_t)(n - 1) * sizeof(*pick_ptr));
            pick_lines[0] = save0;
            pick_ptr[0] = NULL;
        }
        int delta = 0;
        int pick = ui_list_adj(filt[0] ? filt : pname, pick_lines, n, cursor, &delta);
        if (pick < 0) return;
        cursor = pick;
        if (delta == 3) { ui_text_opt("Search", filt, sizeof(filt)); cursor = 0; continue; }
        if (delta) continue;
        ai = (const AvatarInfo *)pick_ptr[pick];
        break;
    }

    u32 av = blk + g->p_avatar_off;
    if (!ai) {
        memset(ctx->plain + av, 0, 6);
        if (g->totem_off) wr32(ctx, blk, 0);
        return;
    }
    if (ai->spirit) {
        int lv = 1;
        if (!ui_number("Avatar level (1-5)", 1, 1, 5, &lv)) return;
        memcpy(ctx->plain + av, &ai->id, 4);
        ctx->plain[av + 4] = (u8)lv;
        ctx->plain[av + 5] = 0;
        if (g->totem_off) wr32(ctx, blk, 0);
    } else {
        memset(ctx->plain + av, 0, 6);
        wr32(ctx, blk, (s32)ai->id);
    }
}


/* keep group-2 equipped counters consistent with player equipment fields */
static void recompute_equipped(SaveCtx *ctx)
{
    const GameDef *g = ctx->game;
    for (int i = 0; i < g->g2_n; i++) {
        u32 e = g->g2_off + (u32)i * 16;
        u32 id;
        memcpy(&id, ctx->plain + e + 4, 4);
        if (!id) continue;
        int idx = rd32(ctx, e);
        int count = 0;
        for (int p = 0; p < g->pmax; p++) {
            u32 blk = g->pdata_off + (u32)p * g->pblock;
            u32 pid;
            memcpy(&pid, ctx->plain + blk + g->p_id_off, 4);
            if (!pid) continue;
            for (int s = 0; s < 4; s++)
                if (rd32(ctx, blk + g->p_equip_off + (u32)s * 4) == idx) count++;
        }
        wr32(ctx, e + 12, count);
        if (rd32(ctx, e + 8) < count) wr32(ctx, e + 8, count);
    }
}

static void equipment_editor(SaveCtx *ctx, u32 blk, const char *pname)
{
    const GameDef *g = ctx->game;
    static const char *slot_names[4] = { "Boots", "Bracelet", "Pendant", "Gloves" };
    static const int slot_subs[4] = { 1, 3, 4, 2 };
    int cursor = 0;
    while (aptMainLoop()) {
        char rows[4][48];
        const char *lines[4];
        for (int s = 0; s < 4; s++) {
            const ItemInfo *ii = item_by_index(ctx, rd32(ctx, blk + g->p_equip_off + (u32)s * 4));
            snprintf(rows[s], 48, "%-9s %s", slot_names[s], ii ? ii->name : "(none)");
            lines[s] = rows[s];
        }
        int delta = 0;
        int pick = ui_list_adj(pname, lines, 4, cursor, &delta);
        if (pick < 0) { recompute_equipped(ctx); return; }
        cursor = pick;
        if (delta == 2) { wr32(ctx, blk + g->p_equip_off + (u32)pick * 4, 0); continue; }
        if (delta) continue;
        int idx = owned_item_picker(ctx, slot_subs[pick], slot_names[pick]);
        if (idx) wr32(ctx, blk + g->p_equip_off + (u32)pick * 4, idx);
    }
}

/* per-player flags and counters */
static void flags_editor(SaveCtx *ctx, u32 blk, const char *pname)
{
    const GameDef *g = ctx->game;
    int cursor = 0;
    while (aptMainLoop()) {
        char rows[6][48];
        const char *lines[6];
        int ids[6], n = 0;
        u8 inv = ctx->plain[blk + g->p_invoke_off];
        if (g->p_style_off) {
            snprintf(rows[n], 48, "Style          %d", (ctx->plain[blk + g->p_style_off] >> 4) & 0xF);
            ids[n] = 0; lines[n] = rows[n]; n++;
            snprintf(rows[n], 48, "Invoke spirit  %s", (inv & 8) ? "yes" : "no");
            ids[n] = 1; lines[n] = rows[n]; n++;
            snprintf(rows[n], 48, "Armed          %s", (inv & 16) ? "yes" : "no");
            ids[n] = 2; lines[n] = rows[n]; n++;
        } else {
            snprintf(rows[n], 48, "Can invoke     %s", inv ? "yes" : "no");
            ids[n] = 5; lines[n] = rows[n]; n++;
        }
        if (g->p_key_off) {
            snprintf(rows[n], 48, "Key player     %s", ctx->plain[blk + g->p_key_off] ? "yes" : "no");
            ids[n] = 6; lines[n] = rows[n]; n++;
        }
        snprintf(rows[n], 48, "Participation  %d", rd16(ctx, blk + g->p_part_off));
        ids[n] = 3; lines[n] = rows[n]; n++;
        snprintf(rows[n], 48, "Goals scored   %d", rd16(ctx, blk + g->p_part_off + 2));
        ids[n] = 4; lines[n] = rows[n]; n++;

        int delta = 0;
        int pick = ui_list_adj(pname, lines, n, cursor, &delta);
        if (pick < 0) return;
        cursor = pick;
        int v;
        switch (ids[pick]) {
        case 0: {
            int st = (ctx->plain[blk + g->p_style_off] >> 4) & 0xF;
            if (delta == 1 || delta == -1) v = st + delta;
            else if (!delta && ui_number("Style (0-15)", st, 0, 15, &v)) {}
            else break;
            if (v >= 0 && v <= 15)
                ctx->plain[blk + g->p_style_off] = (u8)((v << 4) & 0xF0);
            break;
        }
        case 1: if (!delta || delta == 1 || delta == -1) ctx->plain[blk + g->p_invoke_off] ^= 8; break;
        case 2: if (!delta || delta == 1 || delta == -1) ctx->plain[blk + g->p_invoke_off] ^= 16; break;
        case 5: if (!delta || delta == 1 || delta == -1) ctx->plain[blk + g->p_invoke_off] = !ctx->plain[blk + g->p_invoke_off]; break;
        case 6: if (!delta || delta == 1 || delta == -1) ctx->plain[blk + g->p_key_off] = !ctx->plain[blk + g->p_key_off]; break;
        case 3:
            v = rd16(ctx, blk + g->p_part_off) + delta;
            if (delta == 1 || delta == -1) { if (v >= 0 && v <= 9999) wr16(ctx, blk + g->p_part_off, (s16)v); }
            else if (!delta && ui_number("Participation", rd16(ctx, blk + g->p_part_off), 0, 9999, &v))
                wr16(ctx, blk + g->p_part_off, (s16)v);
            break;
        case 4:
            v = rd16(ctx, blk + g->p_part_off + 2) + delta;
            if (delta == 1 || delta == -1) { if (v >= 0 && v <= 9999) wr16(ctx, blk + g->p_part_off + 2, (s16)v); }
            else if (!delta && ui_number("Goals scored", rd16(ctx, blk + g->p_part_off + 2), 0, 9999, &v))
                wr16(ctx, blk + g->p_part_off + 2, (s16)v);
            break;
        }
    }
}


/* ---- player bank (sd:/IESM/players) ---- */

#define PLAYERS_DIR "/IESM/players"

typedef struct {
    char magic[4];   /* "IEPL" */
    u8 ver, pad;
    u16 game_magic;
    u32 id;
    u8 level, pad2;
    s16 gp, tp, freedom;
    s16 invested[8];
    u8 moves[72];
    u32 avatar_id;
    u8 av_level, av_usage, pad3[2];
    u32 totem_id;
} PlayerBankFile;

static void player_bank_store(SaveCtx *ctx, u32 blk, const PlayerInfo *pi)
{
    const GameDef *g = ctx->game;
    PlayerBankFile f;
    memset(&f, 0, sizeof(f));
    memcpy(f.magic, "IEPL", 4);
    f.ver = 1;
    f.game_magic = g->magic;
    memcpy(&f.id, ctx->plain + blk + g->p_id_off, 4);
    f.level = ctx->plain[blk + g->p_gp_off + 6];
    f.gp = rd16(ctx, blk + g->p_gp_off);
    f.tp = rd16(ctx, blk + g->p_gp_off + 2);
    f.freedom = rd16(ctx, blk + g->p_gp_off + 4);
    for (int i = 0; i < 8; i++) f.invested[i] = rd16(ctx, blk + g->p_invest_off + i * 2);
    memcpy(f.moves, ctx->plain + blk + g->p_moves_off, 72);
    memcpy(&f.avatar_id, ctx->plain + blk + g->p_avatar_off, 4);
    f.av_level = ctx->plain[blk + g->p_avatar_off + 4];
    f.av_usage = ctx->plain[blk + g->p_avatar_off + 5];
    if (g->totem_off) f.totem_id = (u32)rd32(ctx, blk);

    char def[40];
    snprintf(def, sizeof(def), "%.24s-L%d", pi ? pi->name : "player", f.level);
    for (char *p = def; *p; p++) if (*p == ' ') *p = '_';
    char name[40];
    if (!ui_text("Bank entry name", def, name, sizeof(name))) return;
    mkdir(PLAYERS_DIR, 0777);
    char path[0x300];
    snprintf(path, sizeof(path), PLAYERS_DIR "/%s.iep", name);
    FILE *out = fopen(path, "wb");
    bool ok = out && fwrite(&f, 1, sizeof(f), out) == sizeof(f);
    if (out) fclose(out);
    if (ok) logline("player banked: sd:%s", path);
    ui_header();
    ui_notice(ok ? "Player saved to the bank." : "Bank write FAILED.", ok);
}

/* import as a new roster member; returns true if added */
static bool player_bank_import(SaveCtx *ctx, const PlayerBankFile *f, int count)
{
    const GameDef *g = ctx->game;
    if (count >= g->pmax) {
        ui_header();
        ui_notice("Roster is full.", false);
        return false;
    }
    u32 blk = g->pdata_off + (u32)count * g->pblock;
    int idx = next_player_index(ctx, count);
    memset(ctx->plain + blk, 0, (size_t)g->pblock);
    wr32(ctx, blk + g->p_id_off - 4, idx);
    memcpy(ctx->plain + blk + g->p_id_off, &f->id, 4);
    ctx->plain[blk + g->p_gp_off + 6] = f->level;
    wr16(ctx, blk + g->p_gp_off, f->gp);
    wr16(ctx, blk + g->p_gp_off + 2, f->tp);
    wr16(ctx, blk + g->p_gp_off + 4, f->freedom);
    for (int i = 0; i < 8; i++) wr16(ctx, blk + g->p_invest_off + i * 2, f->invested[i]);
    memcpy(ctx->plain + blk + g->p_moves_off, f->moves, 72);
    memcpy(ctx->plain + blk + g->p_avatar_off, &f->avatar_id, 4);
    ctx->plain[blk + g->p_avatar_off + 4] = f->av_level;
    ctx->plain[blk + g->p_avatar_off + 5] = f->av_usage;
    if (g->totem_off) wr32(ctx, blk, (s32)f->totem_id);
    for (int i = 0; i < g->pmax; i++)
        if (rd32(ctx, g->pindex_off + (u32)i * 4) == 0) {
            wr32(ctx, g->pindex_off + (u32)i * 4, idx);
            break;
        }
    return true;
}

/* returns number of players added */
static int player_bank_browser(SaveCtx *ctx, int count)
{
    mkdir(PLAYERS_DIR, 0777);
    int added = 0, cursor = 0;
    while (aptMainLoop()) {
        static char names[64][48];
        int n = 0;
        DIR *d = opendir(PLAYERS_DIR);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) && n < 64) {
                size_t l = strlen(e->d_name);
                if (l > 4 && l < 48 && !strcmp(e->d_name + l - 4, ".iep"))
                    snprintf(names[n++], 48, "%s", e->d_name);
            }
            closedir(d);
        }
        if (!n) {
            ui_header();
            ui_notice("Bank empty. In a player's screen,\nuse [ Store in player bank ].", false);
            return added;
        }
        const char *lines[64];
        for (int i = 0; i < n; i++) lines[i] = names[i];
        int pick = ui_list("Player bank", lines, n, cursor);
        if (pick < 0) return added;
        cursor = pick;

        char path[0x300];
        snprintf(path, sizeof(path), PLAYERS_DIR "/%s", names[pick]);
        PlayerBankFile f;
        FILE *in = fopen(path, "rb");
        bool ok = in && fread(&f, 1, sizeof(f), in) == sizeof(f) && !memcmp(f.magic, "IEPL", 4);
        if (in) fclose(in);
        if (!ok) {
            ui_header();
            ui_notice("Unreadable bank file.", false);
            continue;
        }

        const char *acts[] = { "Import into this save", "Delete", "Back" };
        int a = ui_list(names[pick], acts, 3, 0);
        if (a == 0) {
            if (f.game_magic != ctx->game->magic) {
                ui_header();
                ui_notice("Refused: player from another game.", false);
                continue;
            }
            if (player_bank_import(ctx, &f, count + added)) {
                added++;
                ui_header();
                ui_notice("Player imported.", true);
            }
        } else if (a == 1) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Delete %.32s permanently?", names[pick]);
            if (ui_dialog("delete", msg, true)) remove(path);
        }
    }
    return added;
}

static void god_mode(SaveCtx *ctx, u32 blk, const char *pname)
{
    const GameDef *g = ctx->game;
    u32 gp = g->p_gp_off;
    int cursor = 0;
    while (aptMainLoop()) {
        char rows[12][48];
        snprintf(rows[0], 48, "Level      %d", ctx->plain[blk + gp + 6]);
        snprintf(rows[1], 48, "GP         %d", rd16(ctx, blk + gp));
        snprintf(rows[2], 48, "TP         %d", rd16(ctx, blk + gp + 2));
        snprintf(rows[3], 48, "Freedom    %d", rd16(ctx, blk + gp + 4));
        for (int i = 0; i < 8; i++)
            snprintf(rows[4 + i], 48, "+%-9s %d", STAT_NAMES[i], rd16(ctx, blk + g->p_invest_off + i * 2));
        const char *lines[12];
        for (int i = 0; i < 12; i++) lines[i] = rows[i];

        int delta = 0;
        int pick = ui_list_adj(pname, lines, 12, cursor, &delta);
        if (pick < 0) return;
        cursor = pick;

        static const struct { u32 rel; int min, max; bool byte; } F[4] = {
            { 6, 1, 99, true }, { 0, 1, 999, false }, { 2, 1, 999, false }, { 4, 0, 9999, false },
        };
        int v;
        if (pick < 4) {
            u32 off = blk + gp + F[pick].rel;
            int cur = F[pick].byte ? ctx->plain[off] : rd16(ctx, off);
            if (delta) {
                cur += delta;
                if (cur < F[pick].min || cur > F[pick].max) continue;
            } else {
                static const char *hints[4] = { "Level (1-99)", "GP", "TP", "Freedom points" };
                if (!ui_number(hints[pick], cur, F[pick].min, F[pick].max, &cur)) continue;
            }
            if (F[pick].byte) ctx->plain[off] = (u8)cur;
            else wr16(ctx, off, (s16)cur);
        } else {
            int i = pick - 4;
            int cur = rd16(ctx, blk + g->p_invest_off + i * 2);
            if (delta) {
                cur += delta;
                if (cur < -255 || cur > 255) continue;
                wr16(ctx, blk + g->p_invest_off + i * 2, (s16)cur);
            } else if (ui_number(STAT_NAMES[i], cur, 0, 255, &v)) {
                wr16(ctx, blk + g->p_invest_off + i * 2, (s16)v);
            }
        }
    }
}

static void edit_player(SaveCtx *ctx, u32 blk, const PlayerInfo *pi)
{
    const GameDef *g = ctx->game;
    u32 gp = g->p_gp_off;

    if (!pi) {
        god_mode(ctx, blk, "Unknown player (god mode)");
        return;
    }

    int cursor = 0;
    while (aptMainLoop()) {
        int inv[8], inv_sum = 0;
        for (int i = 0; i < 8; i++) {
            inv[i] = rd16(ctx, blk + g->p_invest_off + i * 2);
            inv_sum += inv[i];
        }
        int freedom = rd16(ctx, blk + gp + 4);
        int budget = freedom + inv_sum;
        int level = ctx->plain[blk + gp + 6];

        char rows[20][48];
        snprintf(rows[0], 48, "Level      %d", level);
        snprintf(rows[1], 48, "GP         %d", rd16(ctx, blk + gp));
        snprintf(rows[2], 48, "TP         %d", rd16(ctx, blk + gp + 2));
        snprintf(rows[3], 48, "Freedom left    %d", freedom);
        for (int i = 0; i < 8; i++) {
            int b = stat_base(pi, i, level);
            snprintf(rows[4 + i], 48, "%-9s %3d %+4d = %d", STAT_NAMES[i], b, inv[i], b + inv[i]);
        }
        snprintf(rows[12], 48, "[ God mode (free edit) ]");
        snprintf(rows[13], 48, "[ Reset freedom & invested points ]");
        snprintf(rows[14], 48, "[ Replace with another player ]");
        snprintf(rows[15], 48, "[ Moves ]");
        snprintf(rows[16], 48, "[ Avatar ]");
        snprintf(rows[17], 48, "[ Equipment ]");
        snprintf(rows[18], 48, "[ Flags & counters ]");
        snprintf(rows[19], 48, "[ Store in player bank ]");
        const char *lines[20];
        for (int i = 0; i < 20; i++) lines[i] = rows[i];

        char ptitle[48];
        snprintf(ptitle, sizeof(ptitle), "%s%s", pi->name,
                 (level < 99) ? " (bases = Lv99 values)" : "");
        int delta = 0;
        int pick = ui_list_adj(ptitle, lines, 20, cursor, &delta);
        if (pick < 0) return;
        cursor = pick;

        int v;
        if (pick == 0) {
            int lvl = ctx->plain[blk + gp + 6];
            if (delta) {
                lvl += delta;
                if (lvl >= 1 && lvl <= 99) level_gp_tp(ctx, blk, pi, lvl);
            } else if (ui_number("Level (1-99)", lvl, 1, 99, &v)) {
                level_gp_tp(ctx, blk, pi, v);
            }
        } else if (pick == 1 || pick == 2) {
            u32 off = blk + gp + (pick == 1 ? 0 : 2);
            int cur = rd16(ctx, off);
            if (delta) {
                cur += delta;
                if (cur >= 1 && cur <= 999) wr16(ctx, off, (s16)cur);
            } else if (ui_number(pick == 1 ? "GP" : "TP", cur, 1, 999, &v)) {
                wr16(ctx, off, (s16)v);
            }
        } else if (pick == 3) {
            ui_header();
            char msg[96];
            snprintf(msg, sizeof(msg), "Freedom left: %d (total budget %d).\nSpend it by raising stats.", freedom, budget);
            ui_notice(msg, true);
        } else if (pick >= 4 && pick < 12) {
            int i = pick - 4;
            if (delta > 0) {
                train_plus(ctx, blk, pi, i);
            } else if (delta < 0) {
                if (stat_base(pi, i, level) + inv[i] - 1 < 1) continue;
                int p = pos_index(pi);
                int pair_inv = 0;
                int victim = -1;
                if (p >= 0) {
                    victim = SEESAW[p][i];
                    pair_inv = rd16(ctx, blk + g->p_invest_off + victim * 2);
                }
                if (freedom == 0 && victim >= 0 && pair_inv != 0) {
                    /* seesaw down: the paired stat gains the point */
                    wr16(ctx, blk + g->p_invest_off + i * 2, (s16)(inv[i] - 1));
                    wr16(ctx, blk + g->p_invest_off + victim * 2, (s16)(pair_inv + 1));
                } else {
                    /* untrain: give the point back to freedom (pair at +0 also
                     * refunds, so points can move to another stat pair) */
                    if (freedom >= pi->freedom) continue;
                    wr16(ctx, blk + g->p_invest_off + i * 2, (s16)(inv[i] - 1));
                    wr16(ctx, blk + gp + 4, (s16)(freedom + 1));
                }
            } else {
                int base = stat_base(pi, i, level);
                char hint[64];
                snprintf(hint, sizeof(hint), "%s target (base %d)", STAT_NAMES[i], base);
                int hi = base + inv[i] + freedom;
                if (!ui_number(hint, base + inv[i], 1, hi, &v)) continue;
                int ninv = v - base;
                int nsum = inv_sum - inv[i] + ninv;
                if (nsum > budget) {
                    ui_header();
                    char msg[96];
                    snprintf(msg, sizeof(msg), "Not enough freedom points (left %d).\nLower another stat first.", freedom);
                    ui_notice(msg, false);
                    continue;
                }
                int nfree = budget - nsum;
                if (nfree > pi->freedom) nfree = pi->freedom;
                wr16(ctx, blk + g->p_invest_off + i * 2, (s16)ninv);
                wr16(ctx, blk + gp + 4, (s16)nfree);
            }
        } else if (pick == 12) {
            god_mode(ctx, blk, pi->name);
        } else if (pick == 13) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Reset freedom to %d and all\ninvested points to 0?", pi->freedom);
            if (ui_dialog("reset", msg, false)) {
                wr16(ctx, blk + gp + 4, (s16)pi->freedom);
                for (int i = 0; i < 8; i++)
                    wr16(ctx, blk + g->p_invest_off + i * 2, 0);
            }
        } else if (pick == 14) {
            const PlayerInfo *np = player_db_picker(ctx, false);
            if (!np) continue;
            char msg[128];
            snprintf(msg, sizeof(msg), "Replace %s with %s?\n\nLevel kept; moves, avatar, equipment\nand training are reset.",
                     pi->name, np->name);
            if (!ui_dialog("replace", msg, false)) continue;
            int idx = rd32(ctx, blk + g->p_id_off - 4);
            write_player_block(ctx, blk, np, level, idx);
            return;
        } else if (pick == 15) {
            moves_editor(ctx, blk, pi->name);
        } else if (pick == 16) {
            avatar_editor(ctx, blk, pi->name);
        } else if (pick == 17) {
            equipment_editor(ctx, blk, pi->name);
        } else if (pick == 18) {
            flags_editor(ctx, blk, pi->name);
        } else if (pick == 19) {
            player_bank_store(ctx, blk, pi);
        }
    }
}

void player_editor(SaveCtx *ctx)
{
    const GameDef *g = ctx->game;
    if (!g->pblock ||
        ctx->size < g->pindex_off + (u32)g->pmax * 4 ||
        ctx->size < g->pdata_off + (u32)g->pmax * g->pblock) {
        ui_header();
        ui_notice("Players not supported for this save.", false);
        return;
    }

    int count = 0;
    for (int i = 0; i < g->pmax; i++)
        if (rd32(ctx, g->pindex_off + (u32)i * 4) != 0) count++;
    if (!count) {
        ui_header();
        ui_notice("No players found in save.", false);
        return;
    }

    static u32 blocks[337];
    static char labels[337][48];
    const char *lines[337];
    int cursor = 0;

    u32 region = (u32)g->pmax * g->pblock;
    u32 iregion = (u32)g->pmax * 4;
    u8 *snap = malloc(region + iregion);
    memcpy(snap, ctx->plain + g->pdata_off, region);
    memcpy(snap + region, ctx->plain + g->pindex_off, iregion);

    while (aptMainLoop()) {
        int n = 3;
        snprintf(labels[0], 48, "[ Set ALL players to Lv 99 ]");
        lines[0] = labels[0];
        snprintf(labels[1], 48, "[ Recruit player ]  (%d/%d)", count, g->pmax);
        lines[1] = labels[1];
        snprintf(labels[2], 48, "[ Player bank ]");
        lines[2] = labels[2];
        for (int i = 0; i < count && i < g->pmax; i++) {
            u32 blk = g->pdata_off + (u32)i * g->pblock;
            u32 id;
            memcpy(&id, ctx->plain + blk + g->p_id_off, 4);
            if (!id) continue;
            const PlayerInfo *pi = player_info(g, id);
            blocks[n] = blk;
            (void)0;
            if (pi)
                snprintf(labels[n], 48, "L%-3d %-2s %-4s %s",
                         ctx->plain[blk + g->p_gp_off + 6], pi->pos, pi->elem, pi->name);
            else
                snprintf(labels[n], 48, "L%-3d ?? %08lX",
                         ctx->plain[blk + g->p_gp_off + 6], (unsigned long)id);
            lines[n] = labels[n];
            n++;
        }

        int delta = 0;
        int pick = ui_list_adj("Players (X: dismiss)", lines, n, cursor, &delta);
        if (pick < 0) break;
        cursor = pick;
        if (delta == 2 && pick >= 3) {
            u32 id;
            memcpy(&id, ctx->plain + blocks[pick] + g->p_id_off, 4);
            const PlayerInfo *pi = player_info(g, id);
            char msg[96];
            snprintf(msg, sizeof(msg), "Dismiss %.24s from the save?\n\nTeam slots using them are cleared.",
                     pi ? pi->name : "this player");
            if (ui_dialog("dismiss", msg, true)) {
                dismiss_player(ctx, (int)((blocks[pick] - g->pdata_off) / g->pblock), count);
                count--;
                if (cursor >= n - 1) cursor = n - 2;
            }
            continue;
        }
        if (delta) continue;

        if (pick == 0) {
            if (ui_dialog("set all Lv 99", "Set every player to level 99?\n\nGP/TP are raised to each player's\nbase maximum.", false))
                for (int i = 3; i < n; i++) {
                    u32 id;
                    memcpy(&id, ctx->plain + blocks[i] + g->p_id_off, 4);
                    level_gp_tp(ctx, blocks[i], player_info(g, id), 99);
                }
            continue;
        }
        if (pick == 1) {
            if (count >= g->pmax) {
                ui_header();
                ui_notice("Roster is full.", false);
                continue;
            }
            const PlayerInfo *np = player_db_picker(ctx, true);
            if (!np) continue;
            u32 blk = g->pdata_off + (u32)count * g->pblock;
            int idx = next_player_index(ctx, count);
            write_player_block(ctx, blk, np, 1, idx);
            /* roster order lives in the index table: append at the first free slot */
            for (int i = 0; i < g->pmax; i++)
                if (rd32(ctx, g->pindex_off + (u32)i * 4) == 0) {
                    wr32(ctx, g->pindex_off + (u32)i * 4, idx);
                    break;
                }
            count++;
            ui_header();
            char rmsg[64];
            snprintf(rmsg, sizeof(rmsg), "Recruited %.24s at Lv 1.", np->name);
            ui_notice(rmsg, true);
            continue;
        }
        if (pick == 2) {
            count += player_bank_browser(ctx, count);
            continue;
        }
        u32 id;
        memcpy(&id, ctx->plain + blocks[pick] + g->p_id_off, 4);
        edit_player(ctx, blocks[pick], player_info(g, id));
    }

    if (memcmp(snap, ctx->plain + g->pdata_off, region) != 0 ||
        memcmp(snap + region, ctx->plain + g->pindex_off, iregion) != 0) {
        if (!apply_changes(ctx)) {
            memcpy(ctx->plain + g->pdata_off, snap, region);
            memcpy(ctx->plain + g->pindex_off, snap + region, iregion);
        }
    }
    free(snap);
}

/* ---- inventory ---- */

static const ItemInfo *item_info(const GameDef *g, u32 id)
{
    int lo = 0, hi = g->idb_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g->idb[mid].id == id) return &g->idb[mid];
        if (g->idb[mid].id < id) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}



/* highest inventory index across all three groups */
static int max_item_index(SaveCtx *ctx)
{
    const GameDef *g = ctx->game;
    int mx = 0;
    for (int grp = 0; grp < 3; grp++) {
        u32 base = (grp == 0) ? g->g1_off : (grp == 1) ? g->g2_off : g->g3_off;
        u32 stride = (grp == 0) ? 12 : (grp == 1) ? 16 : 8;
        int cnt = (grp == 0) ? g->g1_n : (grp == 1) ? g->g2_n : g->g3_n;
        for (int i = 0; i < cnt; i++) {
            u32 id;
            memcpy(&id, ctx->plain + base + (u32)i * stride + 4, 4);
            if (!id) continue;
            int idx = rd32(ctx, base + (u32)i * stride);
            if (idx > mx) mx = idx;
        }
    }
    return mx;
}

static bool item_owned(SaveCtx *ctx, u32 id)
{
    const GameDef *g = ctx->game;
    for (int grp = 0; grp < 3; grp++) {
        u32 base = (grp == 0) ? g->g1_off : (grp == 1) ? g->g2_off : g->g3_off;
        u32 stride = (grp == 0) ? 12 : (grp == 1) ? 16 : 8;
        int cnt = (grp == 0) ? g->g1_n : (grp == 1) ? g->g2_n : g->g3_n;
        for (int i = 0; i < cnt; i++) {
            u32 v;
            memcpy(&v, ctx->plain + base + (u32)i * stride + 4, 4);
            if (v == id) return true;
        }
    }
    return false;
}

/* add an item to the group matching its category; returns false if full */
static bool item_add(SaveCtx *ctx, const ItemInfo *ii, int qty)
{
    const GameDef *g = ctx->game;
    int grp = (ii->cat == 1) ? 0 : (ii->cat == 2) ? 1 : 2;
    u32 base = (grp == 0) ? g->g1_off : (grp == 1) ? g->g2_off : g->g3_off;
    u32 stride = (grp == 0) ? 12 : (grp == 1) ? 16 : 8;
    int cnt = (grp == 0) ? g->g1_n : (grp == 1) ? g->g2_n : g->g3_n;
    for (int i = 0; i < cnt; i++) {
        u32 e = base + (u32)i * stride;
        u32 id;
        memcpy(&id, ctx->plain + e + 4, 4);
        if (id) continue;
        wr32(ctx, e, max_item_index(ctx) + 1);
        memcpy(ctx->plain + e + 4, &ii->id, 4);
        if (grp != 2) wr32(ctx, e + 8, qty);
        if (grp == 1) wr32(ctx, e + 12, 0);
        return true;
    }
    return false;
}

/* remove entry k of a group and compact the array like the PC editor does */
static void item_remove(SaveCtx *ctx, int grp, int k)
{
    const GameDef *g = ctx->game;
    u32 base = (grp == 0) ? g->g1_off : (grp == 1) ? g->g2_off : g->g3_off;
    u32 stride = (grp == 0) ? 12 : (grp == 1) ? 16 : 8;
    int cnt = (grp == 0) ? g->g1_n : (grp == 1) ? g->g2_n : g->g3_n;
    memmove(ctx->plain + base + (u32)k * stride,
            ctx->plain + base + (u32)(k + 1) * stride,
            (size_t)(cnt - 1 - k) * stride);
    memset(ctx->plain + base + (u32)(cnt - 1) * stride, 0, stride);
}

/* picker over the item DB restricted to one subcategory; browse first,
 * Y = search; returns the picked ItemInfo or NULL */
static const ItemInfo *item_db_picker(SaveCtx *ctx, int sub)
{
    const GameDef *g = ctx->game;
    char filt[24] = "";
    int cursor = 0;
    while (aptMainLoop()) {
        int n = 0;
        for (int i = 0; i < g->idb_count && n < PICK_MAX; i++) {
            const ItemInfo *ii = &g->idb[i];
            if (ii->sub != sub) continue;
            if (item_owned(ctx, ii->id)) continue;
            if (!name_match(ii->name, filt)) continue;
            pick_ptr[n] = ii;
            snprintf(pick_lab[n], 40, "%s", ii->name);
            pick_lines[n] = pick_lab[n];
            n++;
        }
        if (!n) {
            ui_header();
            ui_notice(filt[0] ? "No match; Y to search again." : "No unowned item here.", false);
            if (!filt[0]) return NULL;
            filt[0] = 0;
            continue;
        }
        pick_sort(n);
        int delta = 0;
        int pick = ui_list_adj(filt[0] ? filt : "Add item (Y: search)", pick_lines, n, cursor, &delta);
        if (pick < 0) return NULL;
        cursor = pick;
        if (delta == 3) { ui_text_opt("Search", filt, sizeof(filt)); cursor = 0; continue; }
        if (delta) continue;
        return (const ItemInfo *)pick_ptr[pick];
    }
    return NULL;
}

#define MAX_ITEMS 1100

static const char *SUBCAT_NAMES[24] = {
    "Other", "Boots", "Gloves", "Bracelets", "Pendants", "Celebrations",
    "Consumables", "Shoot moves", "Dribble moves", "Block moves", "Save moves",
    "Skills", "Key items", "PalPack", "Topics", "Photos", "Formations",
    "Coaches", "Tactics", "Kits", "Emblems", "Spirits", "Totems", "PalPack Cards",
};

static void inventory_items(SaveCtx *ctx, int sub)
{
    const GameDef *g = ctx->game;
    static u32 qty_offs[MAX_ITEMS];
    static u32 eq_offs[MAX_ITEMS];   /* 0 = no equipped counter (group 1) */
    static u16 slot_grp[MAX_ITEMS], slot_i[MAX_ITEMS];
    static char labels[MAX_ITEMS][48];
    const char *lines[MAX_ITEMS];
    int cursor = 0;

    while (aptMainLoop()) {
        int n = 1;
        snprintf(labels[0], 48, "[ Add item ]");
        lines[0] = labels[0];
        qty_offs[0] = 0;
        eq_offs[0] = 0;
        for (int grp = 0; grp < 3; grp++) {
            u32 base = (grp == 0) ? g->g1_off : (grp == 1) ? g->g2_off : g->g3_off;
            u32 stride = (grp == 0) ? 12 : (grp == 1) ? 16 : 8;
            int cnt = (grp == 0) ? g->g1_n : (grp == 1) ? g->g2_n : g->g3_n;
            for (int i = 0; i < cnt && n < MAX_ITEMS; i++) {
                u32 e = base + (u32)i * stride;
                u32 id;
                memcpy(&id, ctx->plain + e + 4, 4);
                if (!id) continue;
                const ItemInfo *ii = item_info(g, id);
                if (!ii || ii->sub != sub) continue;
                slot_grp[n] = (u16)grp;
                slot_i[n] = (u16)i;
                if (grp == 2) {
                    qty_offs[n] = 0;
                    eq_offs[n] = 0;
                    snprintf(labels[n], 48, "    %-26s (owned)", ii->name);
                } else {
                    s32 qty;
                    memcpy(&qty, ctx->plain + e + 8, 4);
                    qty_offs[n] = e + 8;
                    eq_offs[n] = (grp == 1) ? e + 12 : 0;
                    if (grp == 1) {
                        s32 eq;
                        memcpy(&eq, ctx->plain + e + 12, 4);
                        snprintf(labels[n], 48, "%3ld %-26s (%ld eq)", (long)qty, ii->name, (long)eq);
                    } else {
                        snprintf(labels[n], 48, "%3ld %-26s", (long)qty, ii->name);
                    }
                }
                lines[n] = labels[n];
                n++;
            }
        }
        int delta = 0;
        int pick = ui_list_adj(SUBCAT_NAMES[(sub >= 0 && sub < 24) ? sub : 0], lines, n, cursor, &delta);
        if (pick < 0) return;
        cursor = pick;
        if (pick == 0) {
            if (delta) continue;
            const ItemInfo *ii = item_db_picker(ctx, sub);
            if (!ii) continue;
            int q = 1;
            if (ii->cat != 3 && !ui_number("Quantity", 1, 1, 99, &q)) continue;
            ui_header();
            ui_notice(item_add(ctx, ii, q) ? "Item added." : "No free slot in this group.",
                      true);
            continue;
        }
        if (delta == 2) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Remove this item from the save?\n\n%.40s", lines[pick] + 4);
            if (ui_dialog("remove", msg, true))
                item_remove(ctx, slot_grp[pick], slot_i[pick]);
            continue;
        }
        if (!qty_offs[pick]) {
            if (!delta) {
                ui_header();
                ui_notice("Ownership-only entry, no quantity.", false);
            }
            continue;
        }

        s32 qty, eq = 0;
        memcpy(&qty, ctx->plain + qty_offs[pick], 4);
        if (eq_offs[pick]) memcpy(&eq, ctx->plain + eq_offs[pick], 4);
        int lo = (eq > 0) ? eq : 0;
        int v;
        if (delta) {
            v = qty + delta;
            if (v < lo || v > 99) continue;
            memcpy(ctx->plain + qty_offs[pick], &v, 4);
        } else if (ui_number("Quantity", qty, lo, 99, &v)) {
            memcpy(ctx->plain + qty_offs[pick], &v, 4);
        }
    }
}

/* set quantity for every owned item whose subcategory is in `subs` */
static int batch_qty(SaveCtx *ctx, const u8 *subs, int nsubs, int qty)
{
    const GameDef *g = ctx->game;
    int changed = 0;
    for (int grp = 0; grp < 2; grp++) {
        u32 base = grp ? g->g2_off : g->g1_off;
        u32 stride = grp ? 16 : 12;
        int cnt = grp ? g->g2_n : g->g1_n;
        for (int i = 0; i < cnt; i++) {
            u32 e = base + (u32)i * stride;
            u32 id;
            memcpy(&id, ctx->plain + e + 4, 4);
            if (!id) continue;
            const ItemInfo *ii = item_info(g, id);
            if (!ii) continue;
            bool hit = false;
            for (int s = 0; s < nsubs; s++)
                if (ii->sub == subs[s]) { hit = true; break; }
            if (!hit) continue;
            s32 v = qty;
            if (grp) {
                s32 eq;
                memcpy(&eq, ctx->plain + e + 12, 4);
                if (v < eq) v = eq;
            }
            memcpy(ctx->plain + e + 8, &v, 4);
            changed++;
        }
    }
    return changed;
}

static void inventory_batch(SaveCtx *ctx)
{
    static const u8 s_all[]   = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 21, 22 };
    static const u8 s_equip[] = { 1, 2, 3, 4 };
    static const u8 s_cons[]  = { 6 };
    static const u8 s_moves[] = { 7, 8, 9, 10, 11 };
    static const u8 s_spir[]  = { 21, 22 };
    static const u8 s_cards[] = { 23 };
    static const struct { const char *label; const u8 *subs; int n; int qty; } ACTIONS[] = {
        { "ALL items -> x99 (except cards)", s_all,   13, 99 },
        { "Equipment -> x99",                s_equip,  4, 99 },
        { "Consumables -> x99",              s_cons,   1, 99 },
        { "Moves & skills -> x99",           s_moves,  5, 99 },
        { "Spirits & totems -> x99",         s_spir,   2, 99 },
        { "PalPack cards -> x1 (cleanup)",   s_cards,  1,  1 },
    };
    const char *lines[6];
    for (int i = 0; i < 6; i++) lines[i] = ACTIONS[i].label;
    int cursor = 0;
    while (aptMainLoop()) {
        int pick = ui_list("Batch actions", lines, 6, cursor);
        if (pick < 0) return;
        cursor = pick;
        char msg[96];
        snprintf(msg, sizeof(msg), "%s\n\nApply to all owned items?", ACTIONS[pick].label);
        if (!ui_dialog("apply", msg, false)) continue;
        int c = batch_qty(ctx, ACTIONS[pick].subs, ACTIONS[pick].n, ACTIONS[pick].qty);
        ui_header();
        char res[64];
        snprintf(res, sizeof(res), "%d item slot(s) updated.", c);
        ui_notice(res, true);
    }
}

static void inventory_subcats(SaveCtx *ctx, const char *gname, const u8 *gsubs, int gn, const int *counts)
{
    int cursor = 0;
    while (aptMainLoop()) {
        char rows[24][48];
        int subs[24];
        const char *lines[24];
        int n = 0;
        for (int k = 0; k < gn; k++) {
            int s = gsubs[k];
            if (!counts[s]) continue;
            snprintf(rows[n], 48, "%-16s (%d)", SUBCAT_NAMES[s], counts[s]);
            subs[n] = s;
            lines[n] = rows[n];
            n++;
        }
        if (!n) {
            ui_header();
            ui_notice("Nothing owned in this group.", false);
            return;
        }
        if (n == 1) { inventory_items(ctx, subs[0]); return; }
        int pick = ui_list(gname, lines, n, cursor);
        if (pick < 0) return;
        cursor = pick;
        inventory_items(ctx, subs[pick]);
    }
}

void inventory_editor(SaveCtx *ctx)
{
    const GameDef *g = ctx->game;
    u32 g3_end = g->g3_off + (u32)g->g3_n * 8;
    u32 g2_end = g->g2_off + (u32)g->g2_n * 16;
    u32 inv_end = (g3_end > g2_end) ? g3_end : g2_end;
    if (!g->g1_off || ctx->size < inv_end) {
        ui_header();
        ui_notice("Inventory not supported for this save.", false);
        return;
    }

    u32 snap_start = g->g1_off;
    u32 snap_len = inv_end - snap_start;
    u8 *snap = malloc(snap_len);
    memcpy(snap, ctx->plain + snap_start, snap_len);

    static const u8 grp_equip[] = { 1, 2, 3, 4 };
    static const u8 grp_moves[] = { 7, 8, 9, 10, 11, 21, 22 };
    static const u8 grp_items[] = { 5, 6, 12, 13, 14, 15, 23 };
    static const u8 grp_team[]  = { 16, 17, 18, 19, 20 };
    static const struct { const char *name; const u8 *subs; int n; } GROUPS[4] = {
        { "Equipment",       grp_equip, 4 },
        { "Moves & Spirits", grp_moves, 7 },
        { "Items",           grp_items, 7 },
        { "Team",            grp_team,  5 },
    };

    int cursor = 0;
    while (aptMainLoop()) {
        int counts[24] = {0};
        for (int grp = 0; grp < 3; grp++) {
            u32 base = (grp == 0) ? g->g1_off : (grp == 1) ? g->g2_off : g->g3_off;
            u32 stride = (grp == 0) ? 12 : (grp == 1) ? 16 : 8;
            int cnt = (grp == 0) ? g->g1_n : (grp == 1) ? g->g2_n : g->g3_n;
            for (int i = 0; i < cnt; i++) {
                u32 id;
                memcpy(&id, ctx->plain + base + (u32)i * stride + 4, 4);
                if (!id) continue;
                const ItemInfo *ii = item_info(g, id);
                if (ii) counts[(ii->sub < 24) ? ii->sub : 0]++;
            }
        }

        char rows[5][48];
        const char *lines[5];
        snprintf(rows[0], 48, "[ Batch actions (x99, cleanup) ]");
        lines[0] = rows[0];
        int n = 1;
        int gidx[5];
        for (int k = 0; k < 4; k++) {
            int c = 0;
            for (int i = 0; i < GROUPS[k].n; i++) c += counts[GROUPS[k].subs[i]];
            if (!c) continue;
            snprintf(rows[n], 48, "%-16s (%d)", GROUPS[k].name, c);
            gidx[n] = k;
            lines[n] = rows[n];
            n++;
        }
        if (n == 1) {
            ui_header();
            ui_notice("No known items in this save.", false);
            break;
        }

        int pick = ui_list("Inventory (B: back)", lines, n, cursor);
        if (pick < 0) break;
        cursor = pick;
        if (pick == 0) inventory_batch(ctx);
        else inventory_subcats(ctx, GROUPS[gidx[pick]].name, GROUPS[gidx[pick]].subs, GROUPS[gidx[pick]].n, counts);
    }

    if (memcmp(snap, ctx->plain + snap_start, snap_len) != 0) {
        if (!apply_changes(ctx))
            memcpy(ctx->plain + snap_start, snap, snap_len);
    }
    free(snap);
}



/* remove player at contiguous block position k; compacts blocks and the
 * roster index table, and clears team roster references */
static void dismiss_player(SaveCtx *ctx, int k, int count)
{
    const GameDef *g = ctx->game;
    u32 blk = g->pdata_off + (u32)k * g->pblock;
    int ridx = rd32(ctx, blk + g->p_id_off - 4);

    memmove(ctx->plain + blk, ctx->plain + blk + g->pblock,
            (size_t)(count - 1 - k) * g->pblock);
    memset(ctx->plain + g->pdata_off + (u32)(count - 1) * g->pblock, 0, (size_t)g->pblock);

    /* index table: drop the entry, keep order, zero the tail */
    int w = 0;
    for (int i = 0; i < g->pmax; i++) {
        int v = rd32(ctx, g->pindex_off + (u32)i * 4);
        if (v == 0 || v == ridx) continue;
        wr32(ctx, g->pindex_off + (u32)w * 4, v);
        w++;
    }
    for (; w < g->pmax; w++) wr32(ctx, g->pindex_off + (u32)w * 4, 0);

    /* clear from custom teams */
    for (int t = 0; t < g->t_count; t++)
        for (int s = 0; s < 16; s++) {
            u32 off = g->t_players + (u32)t * 0x40 + (u32)s * 4;
            if (rd32(ctx, off) == ridx) wr32(ctx, off, 0);
        }
}

/* ---- teams / tactics ---- */

/* find the item whose inventory Index field equals `idx` */
static const ItemInfo *item_by_index(SaveCtx *ctx, int idx)
{
    const GameDef *g = ctx->game;
    if (!idx) return NULL;
    for (int grp = 0; grp < 3; grp++) {
        u32 base = (grp == 0) ? g->g1_off : (grp == 1) ? g->g2_off : g->g3_off;
        u32 stride = (grp == 0) ? 12 : (grp == 1) ? 16 : 8;
        int cnt = (grp == 0) ? g->g1_n : (grp == 1) ? g->g2_n : g->g3_n;
        for (int i = 0; i < cnt; i++) {
            u32 e = base + (u32)i * stride;
            if (rd32(ctx, e) != idx) continue;
            u32 id;
            memcpy(&id, ctx->plain + e + 4, 4);
            return id ? item_info(g, id) : NULL;
        }
    }
    return NULL;
}

/* pick an owned item of a subcategory; returns its inventory Index (0 = keep) */
static int owned_item_picker(SaveCtx *ctx, int sub, const char *title)
{
    const GameDef *g = ctx->game;
    static int idxs[PICK_MAX];
    int n = 0;
    for (int grp = 0; grp < 3 && n < PICK_MAX; grp++) {
        u32 base = (grp == 0) ? g->g1_off : (grp == 1) ? g->g2_off : g->g3_off;
        u32 stride = (grp == 0) ? 12 : (grp == 1) ? 16 : 8;
        int cnt = (grp == 0) ? g->g1_n : (grp == 1) ? g->g2_n : g->g3_n;
        for (int i = 0; i < cnt && n < PICK_MAX; i++) {
            u32 e = base + (u32)i * stride;
            u32 id;
            memcpy(&id, ctx->plain + e + 4, 4);
            if (!id) continue;
            const ItemInfo *ii = item_info(g, id);
            if (!ii || ii->sub != sub) continue;
            idxs[n] = rd32(ctx, e);
            snprintf(pick_lab[n], 40, "%s", ii->name);
            pick_lines[n] = pick_lab[n];
            n++;
        }
    }
    if (!n) {
        ui_header();
        ui_notice("You own nothing of that type.", false);
        return 0;
    }
    int pick = ui_list(title, pick_lines, n, 0);
    return (pick < 0) ? 0 : idxs[pick];
}

/* pick a player from the reserve; returns roster index (0 = cancel) */
static int reserve_picker(SaveCtx *ctx)
{
    const GameDef *g = ctx->game;
    static int idxs[PICK_MAX];
    int n = 0;
    for (int i = 0; i < g->pmax && n < PICK_MAX; i++) {
        u32 blk = g->pdata_off + (u32)i * g->pblock;
        u32 id;
        memcpy(&id, ctx->plain + blk + g->p_id_off, 4);
        if (!id) continue;
        int ridx = rd32(ctx, blk + g->p_id_off - 4);
        if (!ridx) continue;
        const PlayerInfo *pi = player_info(g, id);
        idxs[n] = ridx;
        if (pi)
            snprintf(pick_lab[n], 40, "L%-3d %-2s %s", ctx->plain[blk + g->p_gp_off + 6], pi->pos, pi->name);
        else
            snprintf(pick_lab[n], 40, "L%-3d ?? %08lX", ctx->plain[blk + g->p_gp_off + 6], (unsigned long)id);
        pick_lines[n] = pick_lab[n];
        n++;
    }
    if (!n) return 0;
    int pick = ui_list("Pick a player", pick_lines, n, 0);
    return (pick < 0) ? 0 : idxs[pick];
}

static const char *roster_name(SaveCtx *ctx, int ridx, char *buf, size_t bufsz)
{
    const GameDef *g = ctx->game;
    if (!ridx) return "(empty)";
    for (int i = 0; i < g->pmax; i++) {
        u32 blk = g->pdata_off + (u32)i * g->pblock;
        if (rd32(ctx, blk + g->p_id_off - 4) != ridx) continue;
        u32 id;
        memcpy(&id, ctx->plain + blk + g->p_id_off, 4);
        const PlayerInfo *pi = id ? player_info(g, id) : NULL;
        if (pi) return pi->name;
        snprintf(buf, bufsz, "%08lX", (unsigned long)id);
        return buf;
    }
    snprintf(buf, bufsz, "idx %d ?", ridx);
    return buf;
}


/* ---- team bank (sd:/IESM/teams) ---- */

#define TEAMS_DIR "/IESM/teams"

typedef struct {
    char magic[4];   /* "IETM" */
    u8 ver, pad;
    u16 game_magic;
    char name[32];
    u32 item_ids[4]; /* coach, formation, kit, emblem (0 = none) */
    u8 positions[16];
    u8 kitnums[16];
    u32 player_ids[16];
} TeamBankFile;

static u32 item_index_to_id(SaveCtx *ctx, int idx)
{
    const GameDef *g = ctx->game;
    if (!idx) return 0;
    for (int grp = 0; grp < 3; grp++) {
        u32 base = (grp == 0) ? g->g1_off : (grp == 1) ? g->g2_off : g->g3_off;
        u32 stride = (grp == 0) ? 12 : (grp == 1) ? 16 : 8;
        int cnt = (grp == 0) ? g->g1_n : (grp == 1) ? g->g2_n : g->g3_n;
        for (int i = 0; i < cnt; i++) {
            u32 e = base + (u32)i * stride;
            if (rd32(ctx, e) != idx) continue;
            u32 id;
            memcpy(&id, ctx->plain + e + 4, 4);
            return id;
        }
    }
    return 0;
}

static int item_id_to_index(SaveCtx *ctx, u32 id)
{
    const GameDef *g = ctx->game;
    if (!id) return 0;
    for (int grp = 0; grp < 3; grp++) {
        u32 base = (grp == 0) ? g->g1_off : (grp == 1) ? g->g2_off : g->g3_off;
        u32 stride = (grp == 0) ? 12 : (grp == 1) ? 16 : 8;
        int cnt = (grp == 0) ? g->g1_n : (grp == 1) ? g->g2_n : g->g3_n;
        for (int i = 0; i < cnt; i++) {
            u32 e = base + (u32)i * stride;
            u32 v;
            memcpy(&v, ctx->plain + e + 4, 4);
            if (v == id) return rd32(ctx, e);
        }
    }
    return 0;
}

static u32 ridx_to_player_id(SaveCtx *ctx, int ridx)
{
    const GameDef *g = ctx->game;
    if (!ridx) return 0;
    for (int i = 0; i < g->pmax; i++) {
        u32 blk = g->pdata_off + (u32)i * g->pblock;
        if (rd32(ctx, blk + g->p_id_off - 4) != ridx) continue;
        u32 id;
        memcpy(&id, ctx->plain + blk + g->p_id_off, 4);
        return id;
    }
    return 0;
}

static int player_id_to_ridx(SaveCtx *ctx, u32 id)
{
    const GameDef *g = ctx->game;
    if (!id) return 0;
    for (int i = 0; i < g->pmax; i++) {
        u32 blk = g->pdata_off + (u32)i * g->pblock;
        u32 v;
        memcpy(&v, ctx->plain + blk + g->p_id_off, 4);
        if (v == id) return rd32(ctx, blk + g->p_id_off - 4);
    }
    return 0;
}

static void bank_store(SaveCtx *ctx, int t)
{
    const GameDef *g = ctx->game;
    u32 info = g->t_info + (u32)t * g->t_info_str;
    u32 players = g->t_players + (u32)t * 0x40;

    TeamBankFile f;
    memset(&f, 0, sizeof(f));
    memcpy(f.magic, "IETM", 4);
    f.ver = 1;
    f.game_magic = g->magic;
    if (g->t_name)
        read_name_buf(ctx->plain, g->t_name + (u32)t * g->t_name_str, f.name, sizeof(f.name));
    for (int s = 0; s < 4; s++)
        f.item_ids[s] = item_index_to_id(ctx, rd32(ctx, info + (u32)s * 4));
    memcpy(f.positions, ctx->plain + info + 16, 16);
    memcpy(f.kitnums, ctx->plain + info + 32, 16);
    for (int s = 0; s < 16; s++)
        f.player_ids[s] = ridx_to_player_id(ctx, rd32(ctx, players + (u32)s * 4));

    char def[40];
    if (f.name[0]) snprintf(def, sizeof(def), "%.30s", f.name);
    else snprintf(def, sizeof(def), "team-%c", 'A' + t);
    char name[40];
    if (!ui_text("Bank entry name", def, name, sizeof(name))) return;
    mkdir(TEAMS_DIR, 0777);
    char path[0x300];
    snprintf(path, sizeof(path), TEAMS_DIR "/%s.iet", name);
    FILE *out = fopen(path, "wb");
    bool ok = out && fwrite(&f, 1, sizeof(f), out) == sizeof(f);
    if (out) fclose(out);
    if (ok) logline("team banked: sd:%s", path);
    ui_header();
    ui_notice(ok ? "Team saved to the bank." : "Bank write FAILED.", ok);
}

static void bank_restore(SaveCtx *ctx, const TeamBankFile *f, int t)
{
    const GameDef *g = ctx->game;
    u32 info = g->t_info + (u32)t * g->t_info_str;
    u32 players = g->t_players + (u32)t * 0x40;

    int miss_p = 0, miss_i = 0;
    for (int s = 0; s < 4; s++) {
        int idx = item_id_to_index(ctx, f->item_ids[s]);
        if (f->item_ids[s] && !idx) miss_i++;
        wr32(ctx, info + (u32)s * 4, idx);
    }
    memcpy(ctx->plain + info + 16, f->positions, 16);
    memcpy(ctx->plain + info + 32, f->kitnums, 16);
    for (int s = 0; s < 16; s++) {
        int ridx = player_id_to_ridx(ctx, f->player_ids[s]);
        if (f->player_ids[s] && !ridx) miss_p++;
        wr32(ctx, players + (u32)s * 4, ridx);
    }
    if (g->t_name && f->name[0]) {
        u32 off = g->t_name + (u32)t * g->t_name_str;
        memset(ctx->plain + off, 0, g->t_name_str);
        size_t l = strlen(f->name);
        if (l > g->t_name_str - 2) l = g->t_name_str - 2;
        memcpy(ctx->plain + off, f->name, l);
        ctx->plain[off + l + 1] = 0x88;
    }
    ui_header();
    char msg[96];
    if (miss_p || miss_i)
        snprintf(msg, sizeof(msg), "Restored. Missing from this save:\n%d player(s), %d item(s) left empty.", miss_p, miss_i);
    else
        snprintf(msg, sizeof(msg), "Team restored into slot %d.", t + 1);
    ui_notice(msg, true);
}

static int pick_team_slot(SaveCtx *ctx, const char *title)
{
    const GameDef *g = ctx->game;
    char rows[10][48];
    const char *lines[10];
    for (int t = 0; t < g->t_count; t++) {
        char tn[32] = "";
        if (g->t_name)
            read_name_buf(ctx->plain, g->t_name + (u32)t * g->t_name_str, tn, sizeof(tn));
        snprintf(rows[t], 48, "%d  %s", t + 1, tn[0] ? tn : "(unnamed)");
        lines[t] = rows[t];
    }
    return ui_list(title, lines, g->t_count, 0);
}

static void team_bank(SaveCtx *ctx)
{
    mkdir(TEAMS_DIR, 0777);
    int cursor = 0;
    while (aptMainLoop()) {
        static char names[64][48];
        int n = 0;
        DIR *d = opendir(TEAMS_DIR);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) && n < 64) {
                size_t l = strlen(e->d_name);
                if (l > 4 && l < 48 && !strcmp(e->d_name + l - 4, ".iet"))
                    snprintf(names[n++], 48, "%s", e->d_name);
            }
            closedir(d);
        }
        if (!n) {
            ui_header();
            ui_notice("Bank empty. In the team list,\npress X on a team to store it.", false);
            return;
        }
        const char *lines[64];
        for (int i = 0; i < n; i++) lines[i] = names[i];
        int pick = ui_list("Team bank", lines, n, cursor);
        if (pick < 0) return;
        cursor = pick;

        char path[0x300];
        snprintf(path, sizeof(path), TEAMS_DIR "/%s", names[pick]);
        TeamBankFile f;
        FILE *in = fopen(path, "rb");
        bool ok = in && fread(&f, 1, sizeof(f), in) == sizeof(f) && !memcmp(f.magic, "IETM", 4);
        if (in) fclose(in);
        if (!ok) {
            ui_header();
            ui_notice("Unreadable bank file.", false);
            continue;
        }

        const char *acts[] = { "Restore into a team slot", "Delete", "Back" };
        int a = ui_list(names[pick], acts, 3, 0);
        if (a == 0) {
            if (f.game_magic != ctx->game->magic) {
                ui_header();
                ui_notice("Refused: team from another game.", false);
                continue;
            }
            int t = pick_team_slot(ctx, "Restore into which team?");
            if (t < 0) continue;
            char msg[96];
            snprintf(msg, sizeof(msg), "Overwrite team slot %d with\n%.32s?", t + 1, names[pick]);
            if (ui_dialog("restore", msg, false)) bank_restore(ctx, &f, t);
        } else if (a == 1) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Delete %.32s permanently?", names[pick]);
            if (ui_dialog("delete", msg, true)) remove(path);
        }
    }
}

static void team_edit(SaveCtx *ctx, int t)
{
    const GameDef *g = ctx->game;
    u32 info = g->t_info + (u32)t * g->t_info_str;
    u32 players = g->t_players + (u32)t * 0x40;
    u32 nameoff = g->t_name ? g->t_name + (u32)t * g->t_name_str : 0;
    static const char *slot_names[4] = { "Coach", "Formation", "Kit", "Emblem" };
    static const int slot_subs[4] = { 17, 16, 19, 20 };
    int cursor = 0;

    while (aptMainLoop()) {
        char rows[22][48];
        const char *lines[22];
        int n = 0;
        char tname[32] = "";
        if (nameoff) {
            read_name_buf(ctx->plain, nameoff, tname, sizeof(tname));
            snprintf(rows[n], 48, "Name       %s", tname);
            lines[n] = rows[n];
            n++;
        }
        for (int s = 0; s < 4; s++) {
            const ItemInfo *ii = item_by_index(ctx, rd32(ctx, info + (u32)s * 4));
            snprintf(rows[n], 48, "%-9s  %s", slot_names[s], ii ? ii->name : "(none)");
            lines[n] = rows[n];
            n++;
        }
        for (int s = 0; s < 16; s++) {
            char nb[16];
            snprintf(rows[n], 48, "#%-2d %s", ctx->plain[info + 32 + s],
                     roster_name(ctx, rd32(ctx, players + (u32)s * 4), nb, sizeof(nb)));
            lines[n] = rows[n];
            n++;
        }
        snprintf(rows[n], 48, "[ Swap two pitch positions ]");
        lines[n] = rows[n];
        n++;

        int delta = 0;
        int pick = ui_list_adj(tname[0] ? tname : "Custom team", lines, n, cursor, &delta);
        if (pick < 0) return;
        cursor = pick;
        int base = nameoff ? 1 : 0;

        if (nameoff && pick == 0) {
            if (delta) continue;
            char text[24];
            if (ui_text("Team name", tname, text, 22)) {
                memset(ctx->plain + nameoff, 0, g->t_name_str);
                size_t l = strlen(text);
                if (l > g->t_name_str - 2) l = g->t_name_str - 2;
                memcpy(ctx->plain + nameoff, text, l);
                ctx->plain[nameoff + l + 1] = 0x88;
            }
        } else if (pick < base + 4) {
            int s = pick - base;
            if (delta == 2) { wr32(ctx, info + (u32)s * 4, 0); continue; }
            if (delta) continue;
            int idx = owned_item_picker(ctx, slot_subs[s], slot_names[s]);
            if (idx) wr32(ctx, info + (u32)s * 4, idx);
        } else if (pick == base + 4 + 16) {
            /* swap the formation-position values of two roster entries,
             * exactly what the PC editor's drag-swap does */
            if (delta) continue;
            int a = ui_list("Swap: first roster slot", lines + base + 4, 16, 0);
            if (a < 0) continue;
            int b2 = ui_list("Swap: second roster slot", lines + base + 4, 16, 0);
            if (b2 < 0 || a == b2) continue;
            u8 tmp = ctx->plain[info + 16 + a];
            ctx->plain[info + 16 + a] = ctx->plain[info + 16 + b2];
            ctx->plain[info + 16 + b2] = tmp;
        } else {
            int s = pick - base - 4;
            if (delta == 2) { wr32(ctx, players + (u32)s * 4, 0); continue; }
            if (delta == 1 || delta == -1) {
                int kn = ctx->plain[info + 32 + s] + delta;
                if (kn >= 1 && kn <= 99) ctx->plain[info + 32 + s] = (u8)kn;
                continue;
            }
            if (delta == 3) {
                int v;
                if (ui_number("Kit number (1-99)", ctx->plain[info + 32 + s], 1, 99, &v))
                    ctx->plain[info + 32 + s] = (u8)v;
                continue;
            }
            int ridx = reserve_picker(ctx);
            if (!ridx) continue;
            bool dup = false;
            for (int k = 0; k < 16; k++)
                if (rd32(ctx, players + (u32)k * 4) == ridx) dup = true;
            if (dup) {
                ui_header();
                ui_notice("Already in this team.", false);
                continue;
            }
            wr32(ctx, players + (u32)s * 4, ridx);
        }
    }
}

void teams_editor(SaveCtx *ctx)
{
    const GameDef *g = ctx->game;
    u32 span_end = g->t_players + (u32)g->t_count * 0x40;
    if (!g->t_count || ctx->size < span_end) {
        ui_header();
        ui_notice("Teams not supported for this save.", false);
        return;
    }

    u8 *snap = malloc(ctx->size);
    memcpy(snap, ctx->plain, ctx->size);

    int cursor = 0;
    while (aptMainLoop()) {
        char rows[11][48];
        const char *lines[11];
        snprintf(rows[0], 48, "[ Team bank (X on a team: store) ]");
        lines[0] = rows[0];
        for (int t = 0; t < g->t_count; t++) {
            char tname[32] = "";
            if (g->t_name)
                read_name_buf(ctx->plain, g->t_name + (u32)t * g->t_name_str, tname, sizeof(tname));
            if (tname[0])
                snprintf(rows[t + 1], 48, "%d  %s", t + 1, tname);
            else
                snprintf(rows[t + 1], 48, "%d  Team %c", t + 1, 'A' + t);
            lines[t + 1] = rows[t + 1];
        }
        int delta = 0;
        int pick = ui_list_adj("Custom teams (B: back)", lines, g->t_count + 1, cursor, &delta);
        if (pick < 0) break;
        cursor = pick;
        if (pick == 0) {
            if (!delta) team_bank(ctx);
            continue;
        }
        if (delta == 2) { bank_store(ctx, pick - 1); continue; }
        if (delta) continue;
        team_edit(ctx, pick - 1);
    }

    if (memcmp(snap, ctx->plain, ctx->size) != 0) {
        if (!apply_changes(ctx))
            memcpy(ctx->plain, snap, ctx->size);
    }
    free(snap);
}

/* ---- commit recap ---- */

static int rl_n, rl_over;
static char rl[64][48];

static void remit(const char *fmt, ...)
{
    char line[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (logfp) { fprintf(logfp, "  %s\n", line); fflush(logfp); }
    if (rl_n < 64) snprintf(rl[rl_n++], 48, "%s", line);
    else rl_over++;
}

static s32 b32(const u8 *p, u32 off) { s32 v; memcpy(&v, p + off, 4); return v; }
static s16 b16(const u8 *p, u32 off) { s16 v; memcpy(&v, p + off, 2); return v; }

/* look up an item's quantity in a buffer; -1 = not owned (g3 ownership = 1) */
static int buf_item_qty(const GameDef *g, const u8 *p, u32 id)
{
    for (int grp = 0; grp < 3; grp++) {
        u32 base = (grp == 0) ? g->g1_off : (grp == 1) ? g->g2_off : g->g3_off;
        u32 stride = (grp == 0) ? 12 : (grp == 1) ? 16 : 8;
        int cnt = (grp == 0) ? g->g1_n : (grp == 1) ? g->g2_n : g->g3_n;
        for (int i = 0; i < cnt; i++) {
            u32 v;
            memcpy(&v, p + base + (u32)i * stride + 4, 4);
            if (v == id) return (grp == 2) ? 1 : b32(p, base + (u32)i * stride + 8);
        }
    }
    return -1;
}

static void recap_items_side(SaveCtx *ctx, const u8 *a, const u8 *b, bool removed)
{
    const GameDef *g = ctx->game;
    for (int grp = 0; grp < 3; grp++) {
        u32 base = (grp == 0) ? g->g1_off : (grp == 1) ? g->g2_off : g->g3_off;
        u32 stride = (grp == 0) ? 12 : (grp == 1) ? 16 : 8;
        int cnt = (grp == 0) ? g->g1_n : (grp == 1) ? g->g2_n : g->g3_n;
        for (int i = 0; i < cnt; i++) {
            u32 id;
            memcpy(&id, a + base + (u32)i * stride + 4, 4);
            if (!id) continue;
            int qa = (grp == 2) ? 1 : b32(a, base + (u32)i * stride + 8);
            int qb = buf_item_qty(g, b, id);
            const ItemInfo *ii = item_info(g, id);
            const char *nm = ii ? ii->name : "unknown item";
            if (qb < 0)
                remit("%s %.28s%s", removed ? "-" : "+", nm,
                      (grp == 2) ? "" : "");
            else if (!removed && qa != qb)
                remit("%.26s x%d -> x%d", nm, qb, qa);
        }
    }
}

static void recap_players(SaveCtx *ctx, const u8 *old)
{
    const GameDef *g = ctx->game;
    for (int i = 0; i < g->pmax; i++) {
        u32 blk = g->pdata_off + (u32)i * g->pblock;
        if (!memcmp(old + blk, ctx->plain + blk, (size_t)g->pblock)) continue;
        u32 oid, nid;
        memcpy(&oid, old + blk + g->p_id_off, 4);
        memcpy(&nid, ctx->plain + blk + g->p_id_off, 4);
        const PlayerInfo *op = oid ? player_info(g, oid) : NULL;
        const PlayerInfo *np = nid ? player_info(g, nid) : NULL;
        const char *nm = np ? np->name : (op ? op->name : "?");
        if (oid != nid) {
            if (!oid) remit("+ recruit %.32s", nm);
            else remit("%.16s -> %.20s", op ? op->name : "?", nm);
            continue;
        }
        int olv = old[blk + g->p_gp_off + 6], nlv = ctx->plain[blk + g->p_gp_off + 6];
        if (olv != nlv) remit("%.20s Lv %d -> %d", nm, olv, nlv);
        if (b16(old, blk + g->p_gp_off) != b16(ctx->plain, blk + g->p_gp_off))
            remit("%.20s GP %d -> %d", nm, b16(old, blk + g->p_gp_off), b16(ctx->plain, blk + g->p_gp_off));
        if (b16(old, blk + g->p_gp_off + 2) != b16(ctx->plain, blk + g->p_gp_off + 2))
            remit("%.20s TP %d -> %d", nm, b16(old, blk + g->p_gp_off + 2), b16(ctx->plain, blk + g->p_gp_off + 2));
        if (b16(old, blk + g->p_gp_off + 4) != b16(ctx->plain, blk + g->p_gp_off + 4))
            remit("%.16s freedom %d -> %d", nm, b16(old, blk + g->p_gp_off + 4), b16(ctx->plain, blk + g->p_gp_off + 4));
        for (int s = 0; s < 8; s++) {
            s16 oi = b16(old, blk + g->p_invest_off + s * 2);
            s16 ni = b16(ctx->plain, blk + g->p_invest_off + s * 2);
            if (oi == ni) continue;
            remit("%.14s %s %+d -> %+d", nm, STAT_NAMES[s], oi, ni);
        }
        if (memcmp(old + blk + g->p_moves_off, ctx->plain + blk + g->p_moves_off, 72))
            remit("%.24s: moves changed", nm);
        if (memcmp(old + blk + g->p_avatar_off, ctx->plain + blk + g->p_avatar_off, 6) ||
            (g->totem_off && b32(old, blk) != b32(ctx->plain, blk)))
            remit("%.24s: avatar changed", nm);
    }
}

bool apply_changes(SaveCtx *ctx)
{
    const GameDef *g = ctx->game;

    u8 *old = malloc(ctx->size);
    memcpy(old, ctx->raw, ctx->size);
    ie_xor_body(old, ctx->size);
    if (!memcmp(old, ctx->plain, ctx->size - 8)) {
        free(old);
        ui_notice("No changes to save.", false);
        return false;
    }

    rl_n = 0;
    rl_over = 0;
    if (logfp) fprintf(logfp, "commit recap:\n");

    /* save info */
    char oa[32], nb[32];
    read_name_buf(old, g->name_off, oa, sizeof(oa));
    read_name_buf(ctx->plain, g->name_off, nb, sizeof(nb));
    if (strcmp(oa, nb)) remit("Name %.12s -> %.12s", oa, nb);
    read_name_buf(old, g->team_off, oa, sizeof(oa));
    read_name_buf(ctx->plain, g->team_off, nb, sizeof(nb));
    if (strcmp(oa, nb)) remit("Team %.12s -> %.12s", oa, nb);
    if (b32(old, g->time_off) != b32(ctx->plain, g->time_off))
        remit("Play time %dh -> %dh", b32(old, g->time_off) / 3600, b32(ctx->plain, g->time_off) / 3600);
    if (b32(old, g->money_off) != b32(ctx->plain, g->money_off))
        remit("Prestige %ld -> %ld", (long)b32(old, g->money_off), (long)b32(ctx->plain, g->money_off));
    if (g->has_friendship && b32(old, g->money_off + 4) != b32(ctx->plain, g->money_off + 4))
        remit("Friendship %ld -> %ld", (long)b32(old, g->money_off + 4), (long)b32(ctx->plain, g->money_off + 4));
    if (g->coin_off) {
        static const char *cn[5] = { "Bronze", "Silver", "Gold", "Platinum", "Rainbow" };
        for (int i = 0; i < 5; i++)
            if (b16(old, g->coin_off + i * 2) != b16(ctx->plain, g->coin_off + i * 2))
                remit("%s coins %d -> %d", cn[i], b16(old, g->coin_off + i * 2), b16(ctx->plain, g->coin_off + i * 2));
    }

    if (old[g->link_off] != ctx->plain[g->link_off] ||
        (g->link_kind == LINK_GO_WORD && memcmp(old + g->link_off, ctx->plain + g->link_off, 4)))
        remit("Secret link %d -> %d", old[g->link_off], ctx->plain[g->link_off]);

    if (g->records_n && memcmp(old + g->records_off, ctx->plain + g->records_off, (size_t)g->records_n))
        remit("Play records unlocked");

    recap_players(ctx, old);
    if (g->t_count) {
        for (int t = 0; t < g->t_count; t++) {
            bool ch = memcmp(old + g->t_info + (u32)t * g->t_info_str,
                             ctx->plain + g->t_info + (u32)t * g->t_info_str, 48) ||
                      memcmp(old + g->t_players + (u32)t * 0x40,
                             ctx->plain + g->t_players + (u32)t * 0x40, 0x40) ||
                      (g->t_name && memcmp(old + g->t_name + (u32)t * g->t_name_str,
                                           ctx->plain + g->t_name + (u32)t * g->t_name_str,
                                           g->t_name_str));
            if (ch) remit("Custom team %d changed", t + 1);
        }
    }
    recap_items_side(ctx, ctx->plain, old, false); /* added + qty changes */
    recap_items_side(ctx, old, ctx->plain, true);  /* removed */

    /* anything not covered above (unlock flag regions etc.) */
    u32 other = 0;
    for (u32 i = 0; i < ctx->size - 8; i++) {
        if (old[i] == ctx->plain[i]) continue;
        if (i >= g->time_off && i < g->team_off + NAME_FIELD_LEN) continue;
        if (i >= g->money_off && i < g->money_off + 8) continue;
        if (g->coin_off && i >= g->coin_off && i < g->coin_off + 10) continue;
        if (i >= g->link_off && i < g->link_off + 4) continue;
        if (g->records_n && i >= g->records_off && i < g->records_off + (u32)g->records_n) continue;
        if (i >= g->pdata_off && i < g->pdata_off + (u32)g->pmax * g->pblock) continue;
        if (i >= g->pindex_off && i < g->pindex_off + (u32)g->pmax * 4) continue;
        if (i >= g->g1_off && i < g->g1_off + (u32)g->g1_n * 12) continue;
        if (i >= g->g2_off && i < g->g2_off + (u32)g->g2_n * 16) continue;
        if (i >= g->g3_off && i < g->g3_off + (u32)g->g3_n * 8) continue;
        if (g->t_count) {
            if (i >= g->t_info && i < g->t_info + (u32)g->t_count * g->t_info_str) continue;
            if (i >= g->t_players && i < g->t_players + (u32)g->t_count * 0x40) continue;
            if (g->t_name && i >= g->t_name && i < g->t_name + (u32)g->t_count * g->t_name_str) continue;
        }
        other++;
    }
    bool players_touched = memcmp(old + g->pdata_off, ctx->plain + g->pdata_off,
                                  (size_t)g->pmax * g->pblock) != 0;
    free(old);
    if (other) remit("unlock/other flags: %lu byte(s)", (unsigned long)other);
    if (rl_over) remit("...and %d more (see log.txt)", rl_over);
    if (players_touched) remit("(GP/TP approx below Lv 99)");

    const char *lines[64];
    for (int i = 0; i < rl_n; i++) lines[i] = rl[i];
    if (ui_list("Commit? A = yes, B = cancel", lines, rl_n, 0) < 0) {
        logline("commit cancelled");
        return false;
    }

    ui_header();
    printf("\n Backing up original save to SD...\n");
    if (!backup_save(ctx, NULL)) {
        ui_notice("Backup FAILED, save untouched.", false);
        return false;
    }
    if (commit_plain(ctx)) {
        logline("committed OK");
        ui_notice("Saved and committed.", true);
        return true;
    }
    ui_notice("WRITE FAILED. Backup is on SD.", false);
    return false;
}
