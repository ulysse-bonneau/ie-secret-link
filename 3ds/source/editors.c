#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "unlock_data.h"
#include "players_db.h"

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

void link_level_editor(SaveCtx *ctx)
{
    int chapter = ctx->plain[CHAPTER_OFFSET];
    int current = ctx->plain[LINK_OFFSET];
    int max = (chapter < 10) ? 2 : 3;
    int sel = current;

    char msg[160];
    snprintf(msg, sizeof(msg), "Secret link level (0-%d)", max);
    if (!ui_number(msg, current, 0, max, &sel) || sel == current) return;

    snprintf(msg, sizeof(msg), "Set link level %d -> %d?%s", current, sel,
             (sel == 3) ? "\n\nLevel 3 REQUIRES the version-exclusive\nteam beaten. Glitched save otherwise." : "");
    if (!ui_dialog((sel == 3) ? "I beat it, proceed" : "confirm", msg, sel == 3)) return;
    ui_header();
    ctx->plain[LINK_OFFSET] = (u8)sel;
    if (!apply_changes(ctx)) ctx->plain[LINK_OFFSET] = (u8)current;
}

void sdlink_unlock(SaveCtx *ctx)
{
    ui_header();
    if (ctx->plain[CHAPTER_OFFSET] < 2) { ui_notice("Refused: requires chapter >= 2.", false); return; }
    if (ctx->size < 0x2F064)            { ui_notice("Refused: save too small (?).", false); return; }
    if (!ui_dialog("unlock", "Unlock ALL data download + QR +\nGO/CS link (SD Link) content?\n\nUndo only via backup restore.", false))
        return;
    ui_header();
    for (u32 i = 0; i < sizeof(UNLOCK_REGIONS) / sizeof(*UNLOCK_REGIONS); i++)
        memcpy(ctx->plain + UNLOCK_REGIONS[i].offset, UNLOCK_REGIONS[i].data, UNLOCK_REGIONS[i].len);
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

static s32 rd32(SaveCtx *ctx, u32 off) { s32 v; memcpy(&v, ctx->plain + off, 4); return v; }
static void wr32(SaveCtx *ctx, u32 off, s32 v) { memcpy(ctx->plain + off, &v, 4); }
static s16 rd16(SaveCtx *ctx, u32 off) { s16 v; memcpy(&v, ctx->plain + off, 2); return v; }
static void wr16(SaveCtx *ctx, u32 off, s16 v) { memcpy(ctx->plain + off, &v, 2); }

void saveinfo_editor(SaveCtx *ctx)
{
    static const char *coin_names[5] = { "Bronze", "Silver", "Gold", "Platinum", "Rainbow" };
    bool modified = false;
    int cursor = 0;

    while (aptMainLoop()) {
        char name[32], team[32];
        read_name(ctx, NAME_OFFSET, name, sizeof(name));
        read_name(ctx, TEAMNAME_OFFSET, team, sizeof(team));
        int secs = rd32(ctx, TIME_OFFSET);
        int prestige = rd32(ctx, MONEY_OFFSET);

        char rows[9][48];
        snprintf(rows[0], 48, "Name       %s", name);
        snprintf(rows[1], 48, "Team name  %s", team);
        snprintf(rows[2], 48, "Play time  %dh %02dm", secs / 3600, secs / 60 % 60);
        snprintf(rows[3], 48, "Prestige   %d", prestige);
        for (int i = 0; i < 5; i++)
            snprintf(rows[4 + i], 48, "%-8s   %d", coin_names[i], rd16(ctx, COIN_OFFSET + i * 2));
        const char *lines[9];
        for (int i = 0; i < 9; i++) lines[i] = rows[i];

        int pick = ui_list("Save info (B: back)", lines, 9, cursor);
        if (pick < 0) break;
        cursor = pick;

        int v;
        char text[32];
        switch (pick) {
        case 0:
            if (ui_text("Player name", name, text, 22)) { write_name(ctx, NAME_OFFSET, text); modified = true; }
            break;
        case 1:
            if (ui_text("Team name", team, text, 22)) { write_name(ctx, TEAMNAME_OFFSET, text); modified = true; }
            break;
        case 2:
            if (ui_number("Play time: hours", secs / 3600, 0, 999, &v)) {
                wr32(ctx, TIME_OFFSET, v * 3600 + (secs / 60 % 60) * 60);
                modified = true;
            }
            break;
        case 3:
            if (ui_number("Prestige points", prestige, 0, 9999999, &v)) { wr32(ctx, MONEY_OFFSET, v); modified = true; }
            break;
        default:
            if (ui_number(coin_names[pick - 4], rd16(ctx, COIN_OFFSET + (pick - 4) * 2), 0, 9999, &v)) {
                wr16(ctx, COIN_OFFSET + (pick - 4) * 2, (s16)v);
                modified = true;
            }
            break;
        }
    }

    if (modified) {
        if (ui_dialog("save changes", "Commit save info changes?", false)) {
            ui_header();
            apply_changes(ctx);
        }
    }
}

/* ---- players ---- */

static const PlayerInfo *player_info(u32 id)
{
    int lo = 0, hi = PLAYERS_DB_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (PLAYERS_DB[mid].id == id) return &PLAYERS_DB[mid];
        if (PLAYERS_DB[mid].id < id) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* offsets inside a 250-byte player block */
#define P_ID     8
#define P_GP     28
#define P_TP     30
#define P_FREE   32
#define P_LEVEL  34
#define P_INVEST 52

static void edit_player(SaveCtx *ctx, u32 blk, const char *pname)
{
    static const char *inv_names[8] = { "Kick", "Dribble", "Block", "Catch",
                                        "Technique", "Speed", "Stamina", "Lucky" };
    int cursor = 0;
    while (aptMainLoop()) {
        char rows[12][48];
        snprintf(rows[0], 48, "Level      %d", ctx->plain[blk + P_LEVEL]);
        snprintf(rows[1], 48, "GP         %d", rd16(ctx, blk + P_GP));
        snprintf(rows[2], 48, "TP         %d", rd16(ctx, blk + P_TP));
        snprintf(rows[3], 48, "Freedom    %d", rd16(ctx, blk + P_FREE));
        for (int i = 0; i < 8; i++)
            snprintf(rows[4 + i], 48, "+%-9s %d", inv_names[i], rd16(ctx, blk + P_INVEST + i * 2));
        const char *lines[12];
        for (int i = 0; i < 12; i++) lines[i] = rows[i];

        int pick = ui_list(pname, lines, 12, cursor);
        if (pick < 0) return;
        cursor = pick;

        int v;
        if (pick == 0) {
            if (ui_number("Level (1-99)", ctx->plain[blk + P_LEVEL], 1, 99, &v))
                ctx->plain[blk + P_LEVEL] = (u8)v;
        } else if (pick == 1) {
            if (ui_number("GP", rd16(ctx, blk + P_GP), 1, 999, &v)) wr16(ctx, blk + P_GP, (s16)v);
        } else if (pick == 2) {
            if (ui_number("TP", rd16(ctx, blk + P_TP), 1, 999, &v)) wr16(ctx, blk + P_TP, (s16)v);
        } else if (pick == 3) {
            if (ui_number("Freedom points", rd16(ctx, blk + P_FREE), 0, 9999, &v)) wr16(ctx, blk + P_FREE, (s16)v);
        } else {
            int i = pick - 4;
            if (ui_number(inv_names[i], rd16(ctx, blk + P_INVEST + i * 2), 0, 255, &v))
                wr16(ctx, blk + P_INVEST + i * 2, (s16)v);
        }
    }
}

void player_editor(SaveCtx *ctx)
{
    if (ctx->size < PLAYERS_OFFSET + (u32)MAX_PLAYERS * PLAYER_BLOCK) {
        ui_header();
        ui_notice("Save too small for player table (?).", false);
        return;
    }

    static u32 blocks[MAX_PLAYERS];
    static char labels[MAX_PLAYERS][48];
    const char *lines[MAX_PLAYERS];
    int cursor = 0;
    bool before = false;

    /* snapshot to detect edits (players span a large region; compare on exit) */
    u32 region = (u32)MAX_PLAYERS * PLAYER_BLOCK;
    u8 *snap = malloc(region);
    memcpy(snap, ctx->plain + PLAYERS_OFFSET, region);
    (void)before;

    while (aptMainLoop()) {
        int n = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            u32 blk = PLAYERS_OFFSET + (u32)i * PLAYER_BLOCK;
            u32 id;
            memcpy(&id, ctx->plain + blk + P_ID, 4);
            if (!id) continue;
            const PlayerInfo *pi = player_info(id);
            blocks[n] = blk;
            if (pi)
                snprintf(labels[n], 48, "L%-3d %-2s %-4s %s",
                         ctx->plain[blk + P_LEVEL], pi->pos, pi->elem, pi->name);
            else
                snprintf(labels[n], 48, "L%-3d ?? %08lX",
                         ctx->plain[blk + P_LEVEL], (unsigned long)id);
            lines[n] = labels[n];
            n++;
        }
        if (!n) {
            ui_header();
            ui_notice("No players found in save.", false);
            break;
        }

        int pick = ui_list("Players (B: back)", lines, n, cursor);
        if (pick < 0) break;
        cursor = pick;

        u32 id;
        memcpy(&id, ctx->plain + blocks[pick] + P_ID, 4);
        const PlayerInfo *pi = player_info(id);
        edit_player(ctx, blocks[pick], pi ? pi->name : "Unknown player");
    }

    if (memcmp(snap, ctx->plain + PLAYERS_OFFSET, region) != 0) {
        if (ui_dialog("save changes", "Commit player changes?", false)) {
            ui_header();
            if (!apply_changes(ctx))
                memcpy(ctx->plain + PLAYERS_OFFSET, snap, region);
        } else {
            memcpy(ctx->plain + PLAYERS_OFFSET, snap, region);
        }
    }
    free(snap);
}
