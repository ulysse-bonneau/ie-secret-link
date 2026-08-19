/* IESM — Inazuma Eleven Save Manager: edits Inazuma Eleven GO-series saves
 * (GO / Chrono Stones / Galaxy) directly on console.
 * Format ported from Tiniifan/InazumaElevenSaveEditor — see NOTES.md. */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "codec.h"

FILE *logfp;

void logline(const char *fmt, ...)
{
    va_list ap;
    consoleSelect(&botcon);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    consoleSelect(&topcon);
    if (logfp) {
        va_start(ap, fmt);
        vfprintf(logfp, fmt, ap);
        va_end(ap);
        fputc('\n', logfp);
        fflush(logfp);
    }
}

const char *tid_variant(u64 tid)
{
    if (tid == 0x000400000010BA00ULL) return "Big Bang";
    if (tid == 0x000400000010BB00ULL) return "Supernova";
    return NULL;
}

/* ---- discovery ---- */

#define MAX_SAVES 16

typedef struct {
    u64 tid;
    FS_MediaType media;
    const char *media_name;
    char filepath[0x220];
    u32 size;
    const GameDef *game;
} SaveEntry;

static SaveEntry entries[MAX_SAVES];
static int n_entries;

static const GameDef *probe_game(FS_Archive arch, const char *path, u64 fsize)
{
    if (fsize < 0x1000 || fsize > 0x800000) return NULL;
    Handle f;
    if (R_FAILED(FSUSER_OpenFile(&f, arch, fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0)))
        return NULL;
    u8 head[6];
    u32 seed = 0, r1 = 0, r2 = 0;
    Result rc1 = FSFILE_Read(f, &r1, 0, head, 6);
    Result rc2 = FSFILE_Read(f, &r2, fsize - 4, &seed, 4);
    FSFILE_Close(f);
    if (R_FAILED(rc1) || R_FAILED(rc2) || r1 != 6 || r2 != 4) return NULL;
    u16 magic = ie_magic(head, seed);
    for (int i = 0; i < GAMES_N; i++)
        if (GAMES[i].magic == magic) return &GAMES[i];
    /* an already-decrypted save (magic in plaintext) also counts */
    u16 plainmagic;
    memcpy(&plainmagic, head + 4, 2);
    for (int i = 0; i < GAMES_N; i++)
        if (GAMES[i].magic == plainmagic) return &GAMES[i];
    return NULL;
}

static void scan_archive_dir(FS_Archive arch, const char *dirpath, int depth,
                             u64 tid, FS_MediaType media, const char *mname)
{
    Handle dir;
    if (R_FAILED(FSUSER_OpenDirectory(&dir, arch, fsMakePath(PATH_ASCII, dirpath))))
        return;
    FS_DirectoryEntry ent;
    u32 n;
    while (n_entries < MAX_SAVES && R_SUCCEEDED(FSDIR_Read(dir, &n, 1, &ent)) && n == 1) {
        char name[0x107];
        int j = 0;
        for (; j < 0x106 && ent.name[j]; j++)
            name[j] = (ent.name[j] < 0x80) ? (char)ent.name[j] : '?';
        name[j] = 0;
        char full[0x220];
        snprintf(full, sizeof(full), "%s%s%s",
                 dirpath, (dirpath[strlen(dirpath) - 1] == '/') ? "" : "/", name);
        if (ent.attributes & FS_ATTRIBUTE_DIRECTORY) {
            if (depth > 0) scan_archive_dir(arch, full, depth - 1, tid, media, mname);
        } else {
            const GameDef *g = probe_game(arch, full, ent.fileSize);
            if (g) {
                SaveEntry *e = &entries[n_entries++];
                e->tid = tid;
                e->media = media;
                e->media_name = mname;
                snprintf(e->filepath, sizeof(e->filepath), "%s", full);
                e->size = (u32)ent.fileSize;
                e->game = g;
                logline("  %s: %s %s", g->name, mname, full);
            }
        }
    }
    FSDIR_Close(dir);
}

static void scan_title(FS_MediaType media, const char *mname, u64 tid)
{
    u32 path[3] = { media, (u32)tid, (u32)(tid >> 32) };
    FS_Archive arch;
    FS_Path bpath = { PATH_BINARY, sizeof(path), path };
    if (R_FAILED(FSUSER_OpenArchive(&arch, ARCHIVE_USER_SAVEDATA, bpath)))
        return;
    scan_archive_dir(arch, "/", 3, tid, media, mname);
    FSUSER_CloseArchive(arch);
}

static void find_saves(void)
{
    const FS_MediaType medias[2] = { MEDIATYPE_GAME_CARD, MEDIATYPE_SD };
    const char *media_names[2] = { "card", "sd" };
    /* known GO-series title IDs, tried even if the AM title list fails */
    const u64 known[] = { 0x000400000010BA00ULL, 0x000400000010BB00ULL };

    bool am = R_SUCCEEDED(amInit());
    for (int m = 0; m < 2; m++) {
        u32 count = 0;
        u64 *tids = NULL;
        u32 read = 0;
        if (am && R_SUCCEEDED(AM_GetTitleCount(medias[m], &count)) && count) {
            tids = malloc(count * sizeof(u64));
            if (R_FAILED(AM_GetTitleList(&read, medias[m], count, tids))) read = 0;
        }
        for (u32 i = 0; i < read; i++)
            if ((tids[i] >> 32) == 0x00040000)
                scan_title(medias[m], media_names[m], tids[i]);
        free(tids);
        if (!read)
            for (u32 i = 0; i < sizeof(known) / sizeof(*known); i++)
                scan_title(medias[m], media_names[m], known[i]);
    }
    if (am) amExit();
}

static bool load_entry(SaveCtx *ctx, const SaveEntry *e)
{
    u32 path[3] = { e->media, (u32)e->tid, (u32)(e->tid >> 32) };
    FS_Path bpath = { PATH_BINARY, sizeof(path), path };
    if (R_FAILED(FSUSER_OpenArchive(&ctx->arch, ARCHIVE_USER_SAVEDATA, bpath)))
        return false;
    Handle f;
    if (R_FAILED(FSUSER_OpenFile(&f, ctx->arch, fsMakePath(PATH_ASCII, e->filepath),
                                 FS_OPEN_READ, 0))) {
        FSUSER_CloseArchive(ctx->arch);
        ctx->arch = 0;
        return false;
    }
    u32 read = 0;
    ctx->raw = malloc(e->size);
    Result rc = FSFILE_Read(f, &read, 0, ctx->raw, e->size);
    FSFILE_Close(f);
    if (R_FAILED(rc) || read != e->size) {
        free(ctx->raw);
        ctx->raw = NULL;
        FSUSER_CloseArchive(ctx->arch);
        ctx->arch = 0;
        return false;
    }
    ctx->size = e->size;
    ctx->plain = malloc(e->size);
    memcpy(ctx->plain, ctx->raw, e->size);
    ie_xor_body(ctx->plain, e->size);
    snprintf(ctx->filepath, sizeof(ctx->filepath), "%s", e->filepath);
    ctx->tid = e->tid;
    ctx->media = e->media_name;
    ctx->game = e->game;
    return true;
}

static void unload(SaveCtx *ctx)
{
    if (ctx->arch) FSUSER_CloseArchive(ctx->arch);
    free(ctx->raw);
    free(ctx->plain);
    memset(ctx, 0, sizeof(*ctx));
}

bool commit_plain(SaveCtx *ctx)
{
    u8 *enc = malloc(ctx->size);
    memcpy(enc, ctx->plain, ctx->size);
    ie_xor_body(enc, ctx->size);
    u32 crc = ie_crc32(enc, ctx->size - 8);
    memcpy(enc + ctx->size - 8, &crc, 4);

    Handle f;
    bool ok = false;
    if (R_SUCCEEDED(FSUSER_OpenFile(&f, ctx->arch, fsMakePath(PATH_ASCII, ctx->filepath),
                                    FS_OPEN_WRITE, 0))) {
        u32 written = 0;
        ok = R_SUCCEEDED(FSFILE_Write(f, &written, 0, enc, ctx->size, FS_WRITE_FLUSH))
             && written == ctx->size;
        FSFILE_Close(f);
        if (ok)
            ok = R_SUCCEEDED(FSUSER_ControlArchive(ctx->arch, ARCHIVE_ACTION_COMMIT_SAVE_DATA,
                                                   NULL, 0, NULL, 0));
    }
    if (ok) memcpy(ctx->raw, enc, ctx->size);
    free(enc);
    return ok;
}

/* returns picked entry or NULL */
static const SaveEntry *save_picker(void)
{
    if (n_entries == 0) return NULL;
    if (n_entries == 1) return &entries[0];
    static char labels[MAX_SAVES][48];
    const char *lines[MAX_SAVES];
    for (int i = 0; i < n_entries; i++) {
        const char *var = tid_variant(entries[i].tid);
        snprintf(labels[i], 48, "%-21s %-4s %s", var ? var : entries[i].game->name,
                 entries[i].media_name, entries[i].filepath);
        lines[i] = labels[i];
    }
    int pick = ui_list("Select a save", lines, n_entries, 0);
    return (pick < 0) ? NULL : &entries[pick];
}

int main(void)
{
    gfxInitDefault();
    hidSetRepeatParameters(18, 4);
    consoleInit(GFX_TOP, &topcon);
    consoleInit(GFX_BOTTOM, &botcon);
    consoleSelect(&topcon);
    fsInit();

    migrate_backups();
    logfp = fopen(BACKUP_DIR "/log.txt", "w");

    ui_header();
    printf(" Scanning for Inazuma Eleven saves...\n" C_DIM " (log on the bottom screen)" C_RESET "\n");
    logline("IESM " VERSION);
    find_saves();
    logline("%d save(s) found", n_entries);

    SaveCtx ctx = {0};
    while (aptMainLoop()) {
        const SaveEntry *e = save_picker();
        if (!e) {
            if (n_entries == 0) {
                ui_header();
                printf(C_WARN " No Inazuma Eleven GO save found.\n" C_RESET
                       "\n Insert the cartridge or install the\n game, then relaunch."
                       "\n\n Full log: sd:" BACKUP_DIR "/log.txt\n");
                ui_notice("Exiting.", false);
            }
            break;
        }
        if (!load_entry(&ctx, e)) {
            ui_header();
            ui_notice("Failed to load the save, see log.", false);
            continue;
        }

        int cursor = 0;
        int staged = -1; /* pending link level picked with LEFT/RIGHT on the menu row */
        bool quit = false;
        while (aptMainLoop()) {
            const GameDef *g = ctx.game;
            char linkrow[48], chrow[48];
            int link_now = (g->link_kind == LINK_LEVEL) ? ctx.plain[g->link_off] : 0;
            if (g->link_kind == LINK_LEVEL) {
                if (staged >= 0 && staged != link_now)
                    snprintf(linkrow, sizeof(linkrow), "Secret link level  %d -> %d  (A: apply)",
                             link_now, staged);
                else
                    snprintf(linkrow, sizeof(linkrow), "Secret link level        (now: %d)", link_now);
            } else
                snprintf(linkrow, sizeof(linkrow), "Unlock secret link (level 3)");
            const char *var = tid_variant(ctx.tid);
            if (g->chapter_off)
                snprintf(chrow, sizeof(chrow), "%s - chapter %d", var ? var : g->name,
                         ctx.plain[g->chapter_off]);
            else
                snprintf(chrow, sizeof(chrow), "%s", var ? var : g->name);

            const char *items[] = {
                linkrow,
                g->unlock_label,
                "Save info (name, money, time)",
                "Players (level, stats)",
                "Inventory (item quantities)",
                "Backups (new, restore, rename)",
                "Switch save",
                "Quit",
            };
            int delta = 0;
            int pick = ui_list_adj(chrow, items, 8, cursor, &delta);
            if (pick < 0 || pick == 7) { quit = true; break; }
            cursor = pick;
            if (delta) {
                if (pick == 0 && g->link_kind == LINK_LEVEL) {
                    int chapter = g->chapter_off ? ctx.plain[g->chapter_off] : 99;
                    int max = (chapter < 10) ? 2 : 3;
                    int v = (staged < 0 ? link_now : staged) + delta;
                    if (v >= 0 && v <= max) staged = v;
                }
                continue;
            }
            switch (pick) {
            case 0:
                if (g->link_kind == LINK_LEVEL && staged >= 0 && staged != link_now)
                    link_apply(&ctx, staged);
                else
                    link_level_editor(&ctx);
                staged = -1;
                break;
            case 1: sdlink_unlock(&ctx); break;
            case 2: saveinfo_editor(&ctx); break;
            case 3: player_editor(&ctx); break;
            case 4: inventory_editor(&ctx); break;
            case 5: backup_manager(&ctx); break;
            case 6: break;
            }
            if (pick == 6) break;
        }
        unload(&ctx);
        if (quit || n_entries <= 1) break;
    }

    if (logfp) fclose(logfp);
    fsExit();
    gfxExit();
    return 0;
}
