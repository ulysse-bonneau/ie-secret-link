#include <stdlib.h>
#include <string.h>
#include "app.h"

bool apply_changes(SaveCtx *ctx)
{
    printf("\n Backing up original save to SD...\n");
    if (!backup_save(ctx, NULL)) {
        ui_notice("Backup FAILED, save untouched.", false);
        return false;
    }
    if (commit_plain(ctx)) {
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

void saveinfo_editor(SaveCtx *ctx)
{
    const GameDef *g = ctx->game;
    static const char *coin_names[5] = { "Bronze", "Silver", "Gold", "Platinum", "Rainbow" };
    bool modified = false;
    int cursor = 0;

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

    if (modified && ui_dialog("save changes", "Commit save info changes?", false)) {
        ui_header();
        apply_changes(ctx);
    }
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

        char rows[14][48];
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
        const char *lines[14];
        for (int i = 0; i < 14; i++) lines[i] = rows[i];

        int delta = 0;
        int pick = ui_list_adj(pi->name, lines, 14, cursor, &delta);
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
    u8 *snap = malloc(region);
    memcpy(snap, ctx->plain + g->pdata_off, region);

    while (aptMainLoop()) {
        int n = 1;
        snprintf(labels[0], 48, "[ Set ALL players to Lv 99 ]");
        lines[0] = labels[0];
        for (int i = 0; i < count && i < g->pmax; i++) {
            u32 blk = g->pdata_off + (u32)i * g->pblock;
            u32 id;
            memcpy(&id, ctx->plain + blk + g->p_id_off, 4);
            if (!id) continue;
            const PlayerInfo *pi = player_info(g, id);
            blocks[n] = blk;
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
                for (int i = 1; i < n; i++) {
                    u32 id;
                    memcpy(&id, ctx->plain + blocks[i] + g->p_id_off, 4);
                    level_gp_tp(ctx, blocks[i], player_info(g, id), 99);
                }
            continue;
        }
        u32 id;
        memcpy(&id, ctx->plain + blocks[pick] + g->p_id_off, 4);
        edit_player(ctx, blocks[pick], player_info(g, id));
    }

    if (memcmp(snap, ctx->plain + g->pdata_off, region) != 0) {
        if (ui_dialog("save changes", "Commit player changes?", false)) {
            ui_header();
            if (!apply_changes(ctx))
                memcpy(ctx->plain + g->pdata_off, snap, region);
        } else {
            memcpy(ctx->plain + g->pdata_off, snap, region);
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
    static char labels[MAX_ITEMS][48];
    const char *lines[MAX_ITEMS];
    int cursor = 0;

    while (aptMainLoop()) {
        int n = 0;
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
        if (!n) return;

        int delta = 0;
        int pick = ui_list_adj(SUBCAT_NAMES[(sub >= 0 && sub < 24) ? sub : 0], lines, n, cursor, &delta);
        if (pick < 0) return;
        cursor = pick;
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
        if (ui_dialog("save changes", "Commit inventory changes?", false)) {
            ui_header();
            if (!apply_changes(ctx))
                memcpy(ctx->plain + snap_start, snap, snap_len);
        } else {
            memcpy(ctx->plain + snap_start, snap, snap_len);
        }
    }
    free(snap);
}
