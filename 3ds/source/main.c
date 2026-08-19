/* IESM — Inazuma Eleven Save Manager: edits Inazuma Eleven GO Galaxy
 * (Big Bang / Supernova) saves directly on console.
 * Format ported from Tiniifan/InazumaElevenSaveEditor — see NOTES.md. */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "app.h"
#include "codec.h"

#define TID_BIG_BANG  0x000400000010BA00ULL
#define TID_SUPERNOVA 0x000400000010BB00ULL

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

const char *title_name(u64 tid)
{
    if (tid == TID_BIG_BANG)  return "Big Bang";
    if (tid == TID_SUPERNOVA) return "Supernova";
    return "unknown title";
}

static bool try_load_file(SaveCtx *ctx, const char *path, u64 fsize)
{
    if (fsize < CHAPTER_OFFSET + 0x10 || fsize > 0x800000) return false;
    Handle f;
    if (R_FAILED(FSUSER_OpenFile(&f, ctx->arch, fsMakePath(PATH_ASCII, path),
                                 FS_OPEN_READ, 0))) return false;
    u32 size = (u32)fsize, read = 0;
    u8 *raw = malloc(size);
    Result rc = FSFILE_Read(f, &read, 0, raw, size);
    FSFILE_Close(f);
    if (R_FAILED(rc) || read != size) { free(raw); return false; }
    u8 *plain = malloc(size);
    memcpy(plain, raw, size);
    ie_xor_body(plain, size);
    u16 magic;
    memcpy(&magic, plain + 4, 2);
    logline("  %s (%lu b) magic 0x%04X", path, (unsigned long)size, magic);
    if (magic != GALAXY_MAGIC) { free(raw); free(plain); return false; }
    ctx->matches++;
    if (ctx->raw) {
        logline("  ^ ADDITIONAL Galaxy save file!");
        free(raw);
        free(plain);
        return false;
    }
    ctx->raw = raw;
    ctx->plain = plain;
    ctx->size = size;
    snprintf(ctx->filepath, sizeof(ctx->filepath), "%s", path);
    return true;
}

static bool scan_dir(SaveCtx *ctx, const char *dirpath, int depth)
{
    Handle dir;
    if (R_FAILED(FSUSER_OpenDirectory(&dir, ctx->arch, fsMakePath(PATH_ASCII, dirpath))))
        return false;
    FS_DirectoryEntry ent;
    u32 n;
    bool found = false;
    while (R_SUCCEEDED(FSDIR_Read(dir, &n, 1, &ent)) && n == 1) {
        char name[0x107];
        int j = 0;
        for (; j < 0x106 && ent.name[j]; j++)
            name[j] = (ent.name[j] < 0x80) ? (char)ent.name[j] : '?';
        name[j] = 0;
        char full[0x220];
        snprintf(full, sizeof(full), "%s%s%s",
                 dirpath, (dirpath[strlen(dirpath) - 1] == '/') ? "" : "/", name);
        if (ent.attributes & FS_ATTRIBUTE_DIRECTORY) {
            if (depth > 0) found |= scan_dir(ctx, full, depth - 1);
        } else {
            found |= try_load_file(ctx, full, ent.fileSize);
        }
    }
    FSDIR_Close(dir);
    return found;
}

static bool try_title(SaveCtx *ctx, FS_MediaType media, const char *mname, u64 tid)
{
    u32 path[3] = { media, (u32)tid, (u32)(tid >> 32) };
    FS_Archive arch;
    FS_Path bpath = { PATH_BINARY, sizeof(path), path };
    Result rc = FSUSER_OpenArchive(&arch, ARCHIVE_USER_SAVEDATA, bpath);
    logline("%s %08lX%08lX: %s%08lX", mname,
            (unsigned long)(tid >> 32), (unsigned long)tid,
            R_FAILED(rc) ? "err " : "open ", (unsigned long)rc);
    if (R_FAILED(rc)) return false;
    ctx->arch = arch;
    ctx->tid = tid;
    ctx->media = mname;
    scan_dir(ctx, "/", 3);
    if (ctx->raw) return true;
    FSUSER_CloseArchive(arch);
    ctx->arch = 0;
    return false;
}

bool find_save(SaveCtx *ctx)
{
    const FS_MediaType medias[2] = { MEDIATYPE_GAME_CARD, MEDIATYPE_SD };
    const char *media_names[2] = { "card", "sd" };
    const u64 known[2] = { TID_BIG_BANG, TID_SUPERNOVA };

    for (int m = 0; m < 2; m++)
        for (int t = 0; t < 2; t++)
            if (try_title(ctx, medias[m], media_names[m], known[t]))
                return true;

    logline("known title IDs failed, scanning all titles...");
    if (R_FAILED(amInit())) {
        logline("amInit failed, cannot scan");
        return false;
    }
    bool found = false;
    for (int m = 0; m < 2 && !found; m++) {
        u32 count = 0;
        if (R_FAILED(AM_GetTitleCount(medias[m], &count)) || count == 0) continue;
        u64 *tids = malloc(count * sizeof(u64));
        u32 read = 0;
        if (R_SUCCEEDED(AM_GetTitleList(&read, medias[m], count, tids))) {
            for (u32 i = 0; i < read && !found; i++) {
                if ((tids[i] >> 32) != 0x00040000) continue;
                if (tids[i] == TID_BIG_BANG || tids[i] == TID_SUPERNOVA) continue;
                found = try_title(ctx, medias[m], media_names[m], tids[i]);
            }
        }
        free(tids);
    }
    amExit();
    return found;
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

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, &topcon);
    consoleInit(GFX_BOTTOM, &botcon);
    consoleSelect(&topcon);
    fsInit();

    migrate_backups();
    logfp = fopen(BACKUP_DIR "/log.txt", "w");

    ui_header();
    printf(" Searching for a GO Galaxy save...\n" C_DIM " (log on the bottom screen)" C_RESET "\n");
    logline("IESM " VERSION);

    SaveCtx ctx = {0};
    if (!find_save(&ctx)) {
        ui_header();
        printf(C_WARN " No GO Galaxy save found.\n" C_RESET
               "\n Insert the cartridge or install the\n game, then relaunch."
               "\n\n Full log: sd:" BACKUP_DIR "/log.txt\n");
        ui_notice("Exiting.", false);
        goto out;
    }
    logline("found save, tid %08lX%08lX, %d match(es)",
            (unsigned long)(ctx.tid >> 32), (unsigned long)ctx.tid, ctx.matches);

    ui_header();
    printf(C_KEY "  Game    " C_RESET "GO Galaxy %s (%s)\n", title_name(ctx.tid), ctx.media);
    printf(C_KEY "  Save    " C_RESET "%s (%lu b)\n", ctx.filepath, (unsigned long)ctx.size);
    printf(C_KEY "  Chapter " C_RESET "%d\n", ctx.plain[CHAPTER_OFFSET]);
    if (ctx.matches > 1)
        printf(C_WARN "  %d save files found, editing first!" C_RESET "\n", ctx.matches);
    printf(C_DIM "\n  Close the game before editing.\n  Every write makes an auto-backup first." C_RESET "\n");
    printf("\n" C_DIM " Press any key for the menu " C_RESET "\n");
    wait_key();

    int cursor = 0;
    while (aptMainLoop()) {
        char linkrow[48];
        snprintf(linkrow, sizeof(linkrow), "Secret link level          (now: %d)",
                 ctx.plain[LINK_OFFSET]);
        const char *items[] = {
            linkrow,
            "Unlock SD-Link content",
            "Save info (name, money, coins)",
            "Players (level, stats)",
            "Backups (new, restore, rename)",
            "Quit",
        };
        int pick = ui_list("Main menu", items, 6, cursor);
        if (pick < 0 || pick == 5) break;
        cursor = pick;
        switch (pick) {
        case 0: link_level_editor(&ctx); break;
        case 1: sdlink_unlock(&ctx); break;
        case 2: saveinfo_editor(&ctx); break;
        case 3: player_editor(&ctx); break;
        case 4: backup_manager(&ctx); break;
        }
    }

out:
    if (ctx.arch) FSUSER_CloseArchive(ctx.arch);
    if (logfp) fclose(logfp);
    free(ctx.raw);
    free(ctx.plain);
    fsExit();
    gfxExit();
    return 0;
}
