#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "codec.h"

bool apply_changes(SaveCtx *ctx)
{
    const GameDef *g = ctx->game;

    /* recap: diff the pending buffer against the last committed state */
    u8 *old = malloc(ctx->size);
    memcpy(old, ctx->raw, ctx->size);
    ie_xor_body(old, ctx->size);

    struct { const char *name; u32 s, e; } R[16];
    int nr = 0;
    R[nr++] = (typeof(R[0])){ "save info", g->time_off, g->team_off + NAME_FIELD_LEN };
    R[nr++] = (typeof(R[0])){ "money/coins", g->money_off, g->money_off + 8 };
    if (g->coin_off) R[nr++] = (typeof(R[0])){ "coins", g->coin_off, g->coin_off + 10 };
    R[nr++] = (typeof(R[0])){ "secret link", g->link_off, g->link_off + 4 };
    if (g->records_n) R[nr++] = (typeof(R[0])){ "play records", g->records_off, g->records_off + (u32)g->records_n };
    R[nr++] = (typeof(R[0])){ "players", g->pdata_off, g->pdata_off + (u32)g->pmax * g->pblock };
    R[nr++] = (typeof(R[0])){ "roster order", g->pindex_off, g->pindex_off + (u32)g->pmax * 4 };
    R[nr++] = (typeof(R[0])){ "items", g->g1_off, g->g1_off + (u32)g->g1_n * 12 };
    R[nr++] = (typeof(R[0])){ "equipment items", g->g2_off, g->g2_off + (u32)g->g2_n * 16 };
    R[nr++] = (typeof(R[0])){ "owned items", g->g3_off, g->g3_off + (u32)g->g3_n * 8 };

    u32 per[16] = {0}, other = 0, total = 0;
    for (u32 i = 0; i < ctx->size - 8; i++) {
        if (old[i] == ctx->plain[i]) continue;
        total++;
        bool hit = false;
        for (int r = 0; r < nr; r++)
            if (i >= R[r].s && i < R[r].e) { per[r]++; hit = true; break; }
        if (!hit) other++;
    }
    free(old);
    if (!total) {
        ui_notice("No changes to save.", false);
        return false;
    }

    ui_header();
    printf(C_KEY " Pending changes" C_RESET " (%lu byte%s)\n\n", (unsigned long)total, total > 1 ? "s" : "");
    logline("commit recap: %lu byte(s)", (unsigned long)total);
    bool players_touched = false;
    for (int r = 0; r < nr; r++) {
        if (!per[r]) continue;
        printf("  %-16s %lu byte%s\n", R[r].name, (unsigned long)per[r], per[r] > 1 ? "s" : "");
        logline("  %s: %lu", R[r].name, (unsigned long)per[r]);
        if (R[r].s == g->pdata_off) players_touched = true;
    }
    if (other) {
        printf("  %-16s %lu byte%s\n", "other flags", (unsigned long)other, other > 1 ? "s" : "");
        logline("  other: %lu", (unsigned long)other);
    }
    if (players_touched)
        printf(C_DIM "\n GP/TP level curve is approximate\n (exact at level 99 only)." C_RESET "\n");
    printf("\n An auto-backup is written first.\n\n");
    printf(C_KEY " A " C_RESET "commit   " C_KEY " B " C_RESET "cancel\n");
    while (aptMainLoop()) {
        u32 k = wait_key();
        if (k & KEY_A) break;
        if (k & KEY_B) { logline("commit cancelled"); return false; }
    }

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

static s32 rd32(SaveCtx *ctx, u32 off) { s32 v; memcpy(&v, ctx->plain + off, 4); return v; }
static void wr32(SaveCtx *ctx, u32 off, s32 v) { memcpy(ctx->plain + off, &v, 4); }
static s16 rd16(SaveCtx *ctx, u32 off) { s16 v; memcpy(&v, ctx->plain + off, 2); return v; }
static void wr16(SaveCtx *ctx, u32 off, s16 v) { memcpy(ctx->plain + off, &v, 2); }

#define PICK_MAX 900

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

static void read_name(SaveCtx *ctx, u32 off, char *out, size_t outsz)
{
    size_t o = 0, i = 0;
    while (o < outsz - 1 && i < NAME_FIELD_LEN && ctx->plain[off + i]) {
        u8 c = ctx->plain[off + i];
        if (c < 0x80) {
            out[o++] = (char)c;
            i++;
        } else {
            /* one placeholder per UTF-8 codepoint (JP names can't render) */
            out[o++] = '?';
            i++;
            while (i < NAME_FIELD_LEN && (ctx->plain[off + i] & 0xC0) == 0x80) i++;
        }
    }
    out[o] = 0;
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

void records_unlock(SaveCtx *ctx)
{
    const GameDef *g = ctx->game;
    ui_header();
    if (!g->records_n || ctx->size < g->records_off + (u32)g->records_n) {
        ui_notice("Play records unknown for this game.", false);
        return;
    }
    if (!ui_dialog("unlock", "Unlock ALL play records?\n\nUndo only via backup restore.", false))
        return;
    memset(ctx->plain + g->records_off, 0xFF, (size_t)g->records_n);
    apply_changes(ctx);
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

static int stat_base(const PlayerInfo *pi, int i, int level)
{
    int v = pi->st[i] * level / 99;
    return (v < 1) ? 1 : v;
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
    char filt[24];
    if (!ui_text_opt("Filter (empty = all)", filt, sizeof(filt))) return NULL;
    static const PlayerInfo *found[PICK_MAX];
    static char plabels[PICK_MAX][40];
    const char *lines[PICK_MAX];
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
        found[n] = pi;
        snprintf(plabels[n], 40, "%-2s %-4s %s", pi->pos, pi->elem, pi->name);
        lines[n] = plabels[n];
        n++;
    }
    if (!n) {
        ui_header();
        ui_notice("No matching player.", false);
        return NULL;
    }
    int pick = ui_list("Pick a player", lines, n, 0);
    return (pick < 0) ? NULL : found[pick];
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
    char filt[24];
    if (!ui_text_opt("Filter (empty = all)", filt, sizeof(filt))) return NULL;
    static const MoveInfo *found[PICK_MAX];
    static char mlabels[PICK_MAX][36];
    const char *lines[PICK_MAX];
    int n = 0;
    for (int i = 0; i < g->mdb_count && n < PICK_MAX; i++) {
        const MoveInfo *mi = &g->mdb[i];
        if (!name_match(mi->name, filt)) continue;
        found[n] = mi;
        snprintf(mlabels[n], 36, "%-2s %s", mi->kind, mi->name);
        lines[n] = mlabels[n];
        n++;
    }
    if (!n) {
        ui_header();
        ui_notice("No matching move.", false);
        return NULL;
    }
    int pick = ui_list("Pick a move", lines, n, 0);
    return (pick < 0) ? NULL : found[pick];
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
        if (pick < 4) {
            if (!id) continue;
            int v;
            if (ui_number("Move level (1-5)", ctx->plain[e + 4], 1, 5, &v))
                ctx->plain[e + 4] = (u8)v;
        } else {
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
    char filt[24];
    if (!ui_text_opt("Filter (empty = all)", filt, sizeof(filt))) return;
    static const AvatarInfo *found[PICK_MAX];
    static char alabels[PICK_MAX][40];
    const char *lines[PICK_MAX + 1];
    lines[0] = "[ None (remove avatar) ]";
    int n = 1;
    for (int i = 0; i < g->adb_count && n < PICK_MAX; i++) {
        const AvatarInfo *ai = &g->adb[i];
        if (!ai->spirit && !g->totem_off) continue; /* totems are Galaxy-only */
        if (!name_match(ai->name, filt)) continue;
        found[n] = ai;
        snprintf(alabels[n], 40, "%-6s %s", ai->spirit ? "Spirit" : "Totem", ai->name);
        lines[n] = alabels[n];
        n++;
    }
    int pick = ui_list(pname, lines, n, 0);
    if (pick < 0) return;

    u32 av = blk + g->p_avatar_off;
    if (pick == 0) {
        memset(ctx->plain + av, 0, 6);
        if (g->totem_off) wr32(ctx, blk, 0);
        return;
    }
    const AvatarInfo *ai = found[pick];
    if (ai->spirit) {
        int lv = 1;
        if (!ui_number("Avatar level (1-5)", 1, 1, 5, &lv)) return;
        memcpy(ctx->plain + av, &ai->id, 4);
        ctx->plain[av + 4] = (u8)lv;
        ctx->plain[av + 5] = 0;
        if (g->totem_off) wr32(ctx, blk, 0);
    } else {
        /* totem lives in the field at the start of the block */
        memset(ctx->plain + av, 0, 6);
        wr32(ctx, blk, (s32)ai->id);
    }
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

        char rows[17][48];
        snprintf(rows[0], 48, "Level      %-4d (GP/TP exact at 99 only)", level);
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
        const char *lines[17];
        for (int i = 0; i < 17; i++) lines[i] = rows[i];

        int delta = 0;
        int pick = ui_list_adj(pi->name, lines, 17, cursor, &delta);
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
        int n = 2;
        snprintf(labels[0], 48, "[ Set ALL players to Lv 99 ]");
        lines[0] = labels[0];
        snprintf(labels[1], 48, "[ Recruit player ]  (%d/%d)", count, g->pmax);
        lines[1] = labels[1];
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

        int pick = ui_list("Players (B: back)", lines, n, cursor);
        if (pick < 0) break;
        cursor = pick;

        if (pick == 0) {
            if (ui_dialog("set all Lv 99", "Set every player to level 99?\n\nGP/TP are raised to each player's\nbase maximum.", false))
                for (int i = 2; i < n; i++) {
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
            int lvl = 1;
            if (!ui_number("Recruit at level (1-99)", 1, 1, 99, &lvl)) continue;
            u32 blk = g->pdata_off + (u32)count * g->pblock;
            int idx = next_player_index(ctx, count);
            write_player_block(ctx, blk, np, lvl, idx);
            /* roster order lives in the index table: append at the first free slot */
            for (int i = 0; i < g->pmax; i++)
                if (rd32(ctx, g->pindex_off + (u32)i * 4) == 0) {
                    wr32(ctx, g->pindex_off + (u32)i * 4, idx);
                    break;
                }
            count++;
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

/* filtered picker over the item DB restricted to one subcategory;
 * returns the picked ItemInfo or NULL */
static const ItemInfo *item_db_picker(SaveCtx *ctx, int sub)
{
    const GameDef *g = ctx->game;
    char filt[24];
    if (!ui_text_opt("Filter (empty = all)", filt, sizeof(filt))) return NULL;
    static const ItemInfo *found[PICK_MAX];
    const char *lines[PICK_MAX];
    int n = 0;
    for (int i = 0; i < g->idb_count && n < PICK_MAX; i++) {
        const ItemInfo *ii = &g->idb[i];
        if (ii->sub != sub) continue;
        if (item_owned(ctx, ii->id)) continue;
        if (!name_match(ii->name, filt)) continue;
        found[n] = ii;
        lines[n] = ii->name;
        n++;
    }
    if (!n) {
        ui_header();
        ui_notice("No matching unowned item.", false);
        return NULL;
    }
    int pick = ui_list("Add item", lines, n, 0);
    return (pick < 0) ? NULL : found[pick];
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
