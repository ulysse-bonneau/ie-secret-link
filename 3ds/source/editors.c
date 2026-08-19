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
        if (k & (KEY_B | KEY_START)) return;
        if (k & KEY_LEFT)  { if (sel > 0) sel--; dirty = true; }
        if (k & KEY_RIGHT) { if (sel < max) sel++; dirty = true; }
        if ((k & KEY_A) && sel != current) {
            char msg[160];
            snprintf(msg, sizeof(msg), "Set link level %d -> %d?%s", current, sel,
                     (sel == 3) ? "\n\nLevel 3 REQUIRES the version-exclusive\nteam beaten. Glitched save otherwise." : "");
            if (ui_dialog((sel == 3) ? "I beat it, proceed" : "confirm", msg, sel == 3)) {
                ui_header();
                ctx->plain[g->link_off] = (u8)sel;
                if (apply_changes(ctx)) return;
                ctx->plain[g->link_off] = (u8)current;
            }
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
    size_t i = 0;
    for (; i < outsz - 1 && i < NAME_FIELD_LEN && ctx->plain[off + i]; i++)
        out[i] = (ctx->plain[off + i] < 0x80) ? (char)ctx->plain[off + i] : '?';
    out[i] = 0;
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

        int pick = ui_list("Save info (B: back)", lines, n, cursor);
        if (pick < 0) break;
        cursor = pick;

        int v;
        char text[32];
        switch (fields[pick]) {
        case F_NAME:
            if (ui_text("Player name", name, text, 22)) { write_name(ctx, g->name_off, text); modified = true; }
            break;
        case F_TEAM:
            if (ui_text("Team name", team, text, 22)) { write_name(ctx, g->team_off, text); modified = true; }
            break;
        case F_TIME:
            if (ui_number("Play time: hours", secs / 3600, 0, 999, &v)) {
                wr32(ctx, g->time_off, v * 3600 + (secs / 60 % 60) * 60);
                modified = true;
            }
            break;
        case F_PRESTIGE:
            if (ui_number("Prestige points", rd32(ctx, g->money_off), 0, 9999999, &v)) {
                wr32(ctx, g->money_off, v);
                modified = true;
            }
            break;
        case F_FRIEND:
            if (ui_number("Friendship points", rd32(ctx, g->money_off + 4), 0, 9999999, &v)) {
                wr32(ctx, g->money_off + 4, v);
                modified = true;
            }
            break;
        default: {
            int i = fields[pick] - F_COIN0;
            if (ui_number(coin_names[i], rd16(ctx, g->coin_off + i * 2), 0, 9999, &v)) {
                wr16(ctx, g->coin_off + i * 2, (s16)v);
                modified = true;
            }
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

static void edit_player(SaveCtx *ctx, u32 blk, const char *pname)
{
    const GameDef *g = ctx->game;
    static const char *inv_names[8] = { "Kick", "Dribble", "Block", "Catch",
                                        "Technique", "Speed", "Stamina", "Lucky" };
    u32 gp = g->p_gp_off;
    int cursor = 0;
    while (aptMainLoop()) {
        char rows[12][48];
        snprintf(rows[0], 48, "Level      %d", ctx->plain[blk + gp + 6]);
        snprintf(rows[1], 48, "GP         %d", rd16(ctx, blk + gp));
        snprintf(rows[2], 48, "TP         %d", rd16(ctx, blk + gp + 2));
        snprintf(rows[3], 48, "Freedom    %d", rd16(ctx, blk + gp + 4));
        for (int i = 0; i < 8; i++)
            snprintf(rows[4 + i], 48, "+%-9s %d", inv_names[i], rd16(ctx, blk + g->p_invest_off + i * 2));
        const char *lines[12];
        for (int i = 0; i < 12; i++) lines[i] = rows[i];

        int pick = ui_list(pname, lines, 12, cursor);
        if (pick < 0) return;
        cursor = pick;

        int v;
        if (pick == 0) {
            if (ui_number("Level (1-99)", ctx->plain[blk + gp + 6], 1, 99, &v))
                ctx->plain[blk + gp + 6] = (u8)v;
        } else if (pick == 1) {
            if (ui_number("GP", rd16(ctx, blk + gp), 1, 999, &v)) wr16(ctx, blk + gp, (s16)v);
        } else if (pick == 2) {
            if (ui_number("TP", rd16(ctx, blk + gp + 2), 1, 999, &v)) wr16(ctx, blk + gp + 2, (s16)v);
        } else if (pick == 3) {
            if (ui_number("Freedom points", rd16(ctx, blk + gp + 4), 0, 9999, &v)) wr16(ctx, blk + gp + 4, (s16)v);
        } else {
            int i = pick - 4;
            if (ui_number(inv_names[i], rd16(ctx, blk + g->p_invest_off + i * 2), 0, 255, &v))
                wr16(ctx, blk + g->p_invest_off + i * 2, (s16)v);
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

    /* player count = non-zero entries in the index table (mirrors the C# code);
     * blocks are contiguous from pdata_off for that count */
    int count = 0;
    for (int i = 0; i < g->pmax; i++)
        if (rd32(ctx, g->pindex_off + (u32)i * 4) != 0) count++;
    if (!count) {
        ui_header();
        ui_notice("No players found in save.", false);
        return;
    }

    static u32 blocks[336];
    static char labels[336][48];
    const char *lines[336];
    int cursor = 0;

    u32 region = (u32)g->pmax * g->pblock;
    u8 *snap = malloc(region);
    memcpy(snap, ctx->plain + g->pdata_off, region);

    while (aptMainLoop()) {
        int n = 0;
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

        u32 id;
        memcpy(&id, ctx->plain + blocks[pick] + g->p_id_off, 4);
        const PlayerInfo *pi = player_info(g, id);
        edit_player(ctx, blocks[pick], pi ? pi->name : "Unknown player");
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
