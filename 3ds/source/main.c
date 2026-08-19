/* ie-secret-link: set the secret link level in Inazuma Eleven GO Galaxy
 * (Big Bang / Supernova) saves, directly on console.
 * Format ported from Tiniifan/InazumaElevenSaveEditor — see NOTES.md. */

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "codec.h"
#include "unlock_data.h"

#define VERSION        "v0.3.0"
#define GALAXY_MAGIC   0x40F1
#define LINK_OFFSET    0x90B4
#define CHAPTER_OFFSET 0x9F1C
#define BACKUP_DIR     "/ie-secret-link"

#define TID_BIG_BANG  0x000400000010BA00ULL
#define TID_SUPERNOVA 0x000400000010BB00ULL

static FILE *logfp;
static PrintConsole topcon, botcon;

static void logline(const char *fmt, ...)
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

static const char *title_name(u64 tid)
{
    if (tid == TID_BIG_BANG)  return "Big Bang";
    if (tid == TID_SUPERNOVA) return "Supernova";
    return "unknown title";
}

typedef struct {
    FS_Archive arch;
    char filepath[0x220]; /* path inside the save archive, leading '/' */
    u8 *raw;    /* encrypted, as on disk */
    u8 *plain;  /* decrypted copy */
    u32 size;
    u64 tid;
    const char *media;
    int matches;              /* total files that decrypt to Galaxy magic */
    char extra[4][0x220];     /* additional matches beyond the first */
} SaveCtx;

static bool try_load_file(SaveCtx *ctx, const char *path, u64 fsize)
{
    /* must at least contain the fields we patch, plus the 8-byte trailer */
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
        if (ctx->matches - 2 < 4)
            snprintf(ctx->extra[ctx->matches - 2], 0x220, "%s", path);
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

static bool find_save(SaveCtx *ctx)
{
    const FS_MediaType medias[2] = { MEDIATYPE_GAME_CARD, MEDIATYPE_SD };
    const char *media_names[2] = { "card", "sd" };
    const u64 known[2] = { TID_BIG_BANG, TID_SUPERNOVA };

    for (int m = 0; m < 2; m++)
        for (int t = 0; t < 2; t++)
            if (try_title(ctx, medias[m], media_names[m], known[t]))
                return true;

    /* fallback: scan every installed game title for a Galaxy-format save */
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
                if ((tids[i] >> 32) != 0x00040000) continue; /* games only */
                if (tids[i] == TID_BIG_BANG || tids[i] == TID_SUPERNOVA) continue;
                found = try_title(ctx, medias[m], media_names[m], tids[i]);
            }
        }
        free(tids);
    }
    amExit();
    return found;
}

static bool backup_save(SaveCtx *ctx)
{
    char base[0x220];
    snprintf(base, sizeof(base), "%s", ctx->filepath + 1);
    for (char *p = base; *p; p++)
        if (*p == '/') *p = '_';
    char path[0x300];
    for (int i = 0; i < 1000; i++) {
        snprintf(path, sizeof(path), BACKUP_DIR "/%s.%03d.bak", base, i);
        FILE *probe = fopen(path, "rb");
        if (probe) { fclose(probe); continue; }
        FILE *out = fopen(path, "wb");
        if (!out) return false;
        bool ok = fwrite(ctx->raw, 1, ctx->size, out) == ctx->size;
        fclose(out);
        if (ok) logline("Backup: sd:%s", path);
        return ok;
    }
    return false;
}

/* ponytail: picks the newest .bak by name (counter suffix sorts), assumes it
 * belongs to the currently found save file; per-file selection if ever needed */
static bool restore_backup(SaveCtx *ctx)
{
    DIR *d = opendir(BACKUP_DIR);
    if (!d) { logline("no backup dir on SD"); return false; }
    char best[0x280] = "";
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".bak") && strcmp(e->d_name, best) > 0)
            snprintf(best, sizeof(best), "%s", e->d_name);
    }
    closedir(d);
    if (!best[0]) { logline("no .bak files in sd:" BACKUP_DIR); return false; }

    char full[0x300];
    snprintf(full, sizeof(full), BACKUP_DIR "/%s", best);
    FILE *in = fopen(full, "rb");
    if (!in) return false;
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    u8 *buf = malloc(size);
    bool rok = fread(buf, 1, size, in) == (size_t)size;
    fclose(in);
    if (!rok) { free(buf); return false; }

    logline("restoring %s (%ld b)", best, size);
    logline("into %s", ctx->filepath);

    Handle f;
    bool ok = false;
    if (R_SUCCEEDED(FSUSER_OpenFile(&f, ctx->arch, fsMakePath(PATH_ASCII, ctx->filepath),
                                    FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
        u32 written = 0;
        ok = R_SUCCEEDED(FSFILE_SetSize(f, (u64)size))
          && R_SUCCEEDED(FSFILE_Write(f, &written, 0, buf, (u32)size, FS_WRITE_FLUSH))
          && written == (u32)size;
        FSFILE_Close(f);
        if (ok)
            ok = R_SUCCEEDED(FSUSER_ControlArchive(ctx->arch, ARCHIVE_ACTION_COMMIT_SAVE_DATA,
                                                   NULL, 0, NULL, 0));
    }
    free(buf);
    return ok;
}

static bool commit_plain(SaveCtx *ctx)
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

static u32 wait_key(void)
{
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k) return k;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    return 0;
}

#define C_RESET  "\x1b[0m"
#define C_TITLE  "\x1b[30;46m"   /* black on cyan */
#define C_KEY    "\x1b[36m"      /* cyan  */
#define C_VAL    "\x1b[33m"      /* yellow */
#define C_SEL    "\x1b[30;47m"   /* black on white */
#define C_WARN   "\x1b[31m"      /* red */
#define C_OK     "\x1b[32m"      /* green */
#define C_DIM    "\x1b[35m"      /* magenta */

static void header(void)
{
    consoleClear();
    printf(C_TITLE " IESM - Inazuma Eleven Save Manager   " VERSION " " C_RESET "\n\n");
}

/* modal on the top screen; returns true if `yes` key pressed */
static bool dialog(const char *yes, const char *text, bool warn)
{
    header();
    printf("%s%s" C_RESET "\n\n", warn ? C_WARN : "", text);
    printf(C_KEY " A " C_RESET "%s   " C_KEY " B " C_RESET "cancel\n", yes);
    while (aptMainLoop()) {
        u32 k = wait_key();
        if (k & KEY_A) return true;
        if (k & KEY_B) return false;
    }
    return false;
}

static void notice(const char *text, bool ok)
{
    printf("\n%s%s" C_RESET "\n\n" C_DIM "Press any key." C_RESET "\n",
           ok ? C_OK : C_WARN, text);
    wait_key();
}

#define MENU_ITEMS 4

static void draw_menu(SaveCtx *ctx, int cursor, int sel, int chapter, int current, int max)
{
    header();
    printf(C_KEY "  Game    " C_RESET "GO Galaxy %s (%s)\n", title_name(ctx->tid), ctx->media);
    printf(C_KEY "  Save    " C_RESET "%s (%lu b)\n", ctx->filepath, (unsigned long)ctx->size);
    printf(C_KEY "  Chapter " C_RESET "%d\n", chapter);
    printf(C_KEY "  Link lv " C_RESET "%d\n", current);
    if (ctx->matches > 1)
        printf(C_WARN "  %d save files found, patching first!" C_RESET "\n", ctx->matches);
    printf("\n");

    const char *labels[MENU_ITEMS] = {
        NULL, "Unlock SD-Link content", "Restore latest backup", "Quit" };
    for (int i = 0; i < MENU_ITEMS; i++) {
        const char *cur = (i == cursor) ? C_SEL : "";
        if (i == 0)
            printf(" %s Secret link level   %s %d %s%s " C_RESET "\n",
                   cur, (sel > 0) ? "<" : " ", sel, (sel < max) ? ">" : " ", cur);
        else
            printf(" %s %s " C_RESET "\n", cur, labels[i]);
    }

    printf("\n");
    switch (cursor) {
    case 0:
        if (chapter < 10)
            printf(C_DIM " Level 3 locked: chapter < 10." C_RESET "\n");
        else
            printf(C_DIM " Level 3 needs the version-exclusive\n team beaten, else the save glitches." C_RESET "\n");
        break;
    case 1:
        printf(C_DIM " Data download + QR + GO/CS link\n rewards, all at once." C_RESET "\n");
        break;
    case 2:
        printf(C_DIM " Newest .bak from sd:" BACKUP_DIR "." C_RESET "\n");
        break;
    default:
        break;
    }
    printf("\x1b[28;1H" C_DIM " D-Pad move/adjust   A select   START quit" C_RESET);
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, &topcon);
    consoleInit(GFX_BOTTOM, &botcon);
    consoleSelect(&topcon);
    fsInit();

    mkdir(BACKUP_DIR, 0777);
    logfp = fopen(BACKUP_DIR "/log.txt", "w");

    header();
    printf(" Searching for a GO Galaxy save...\n" C_DIM " (log on the bottom screen)" C_RESET "\n");
    logline("IESM " VERSION);

    SaveCtx ctx = {0};
    if (!find_save(&ctx)) {
        header();
        printf(C_WARN " No GO Galaxy save found.\n" C_RESET
               "\n Insert the cartridge or install the\n game, then relaunch."
               "\n\n Full log: sd:" BACKUP_DIR "/log.txt\n");
        notice("Exiting.", false);
        goto out;
    }
    logline("found save, tid %08lX%08lX, %d match(es)",
            (unsigned long)(ctx.tid >> 32), (unsigned long)ctx.tid, ctx.matches);

    int chapter = ctx.plain[CHAPTER_OFFSET];
    int current = ctx.plain[LINK_OFFSET];
    int sel = current;
    int max = (chapter < 10) ? 2 : 3;
    if (sel > max) sel = max;
    int cursor = 0;
    bool dirty = true;

    while (aptMainLoop()) {
        if (dirty) {
            draw_menu(&ctx, cursor, sel, chapter, current, max);
            dirty = false;
        }
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_START) break;
        if (k & KEY_UP)    { cursor = (cursor + MENU_ITEMS - 1) % MENU_ITEMS; dirty = true; }
        if (k & KEY_DOWN)  { cursor = (cursor + 1) % MENU_ITEMS; dirty = true; }
        if (cursor == 0) {
            if (k & KEY_LEFT)  { if (sel > 0) sel--; dirty = true; }
            if (k & KEY_RIGHT) { if (sel < max) sel++; dirty = true; }
        }
        if (k & KEY_A) {
            dirty = true;
            if (cursor == 3) break;
            if (cursor == 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Set link level %d -> %d?%s", current, sel,
                         (sel == 3) ? "\n\nLevel 3 REQUIRES the version-exclusive\nteam beaten. Glitched save otherwise." : "");
                if (!dialog((sel == 3) ? "I beat it, proceed" : "confirm", msg, sel == 3))
                    continue;
                printf("\n Backing up original save to SD...\n");
                if (!backup_save(&ctx)) {
                    notice("Backup FAILED, save untouched.", false);
                } else if ((ctx.plain[LINK_OFFSET] = (u8)sel), commit_plain(&ctx)) {
                    current = sel;
                    notice("Patched. Check the Inalink in-game.", true);
                } else {
                    notice("WRITE FAILED. Backup is on SD.", false);
                }
            } else if (cursor == 1) {
                if (chapter < 2) {
                    header();
                    notice("Refused: requires chapter >= 2.", false);
                    continue;
                }
                if (ctx.size < 0x2F064) {
                    header();
                    notice("Refused: save too small (?).", false);
                    continue;
                }
                if (!dialog("unlock", "Unlock ALL data download + QR +\nGO/CS link (SD Link) content?\n\nUndo only via backup restore.", false))
                    continue;
                printf("\n Backing up original save to SD...\n");
                if (!backup_save(&ctx)) {
                    notice("Backup FAILED, save untouched.", false);
                } else {
                    for (u32 i = 0; i < sizeof(UNLOCK_REGIONS) / sizeof(*UNLOCK_REGIONS); i++)
                        memcpy(ctx.plain + UNLOCK_REGIONS[i].offset,
                               UNLOCK_REGIONS[i].data, UNLOCK_REGIONS[i].len);
                    if (commit_plain(&ctx))
                        notice("Unlocked. Check the Inalink in-game.", true);
                    else
                        notice("WRITE FAILED. Backup is on SD.", false);
                }
            } else if (cursor == 2) {
                if (!dialog("restore", "Restore newest backup from\nsd:" BACKUP_DIR "\nover the current save?", false))
                    continue;
                if (restore_backup(&ctx)) {
                    notice("Restored. Relaunch before patching.", true);
                    break;
                }
                notice("RESTORE FAILED, see log.", false);
            }
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
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
