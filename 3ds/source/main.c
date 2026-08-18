/* ie-secret-link: set the secret link level in Inazuma Eleven GO Galaxy
 * (Big Bang / Supernova) saves, directly on console.
 * Format ported from Tiniifan/InazumaElevenSaveEditor — see NOTES.md. */

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "codec.h"

#define VERSION        "v0.1.1"
#define GALAXY_MAGIC   0x40F1
#define LINK_OFFSET    0x90B4
#define CHAPTER_OFFSET 0x9F1C
#define BACKUP_DIR     "/ie-secret-link"

#define TID_BIG_BANG  0x000400000010BA00ULL
#define TID_SUPERNOVA 0x000400000010BB00ULL

static FILE *logfp;

static void logline(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
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
    while (!found && R_SUCCEEDED(FSDIR_Read(dir, &n, 1, &ent)) && n == 1) {
        char name[0x107];
        int j = 0;
        for (; j < 0x106 && ent.name[j]; j++)
            name[j] = (ent.name[j] < 0x80) ? (char)ent.name[j] : '?';
        name[j] = 0;
        char full[0x220];
        snprintf(full, sizeof(full), "%s%s%s",
                 dirpath, (dirpath[strlen(dirpath) - 1] == '/') ? "" : "/", name);
        if (ent.attributes & FS_ATTRIBUTE_DIRECTORY) {
            if (depth > 0) found = scan_dir(ctx, full, depth - 1);
        } else {
            found = try_load_file(ctx, full, ent.fileSize);
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
    if (scan_dir(ctx, "/", 3)) return true;
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

static bool write_save(SaveCtx *ctx, u8 level)
{
    ctx->plain[LINK_OFFSET] = level;
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

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    fsInit();

    mkdir(BACKUP_DIR, 0777);
    logfp = fopen(BACKUP_DIR "/log.txt", "w");

    logline("ie-secret-link " VERSION);
    logline("");

    SaveCtx ctx = {0};
    if (!find_save(&ctx)) {
        logline("");
        logline("No GO Galaxy save found.");
        logline("Full log: sd:" BACKUP_DIR "/log.txt");
        logline("");
        logline("Press any key to exit.");
        wait_key();
        goto out;
    }
    logline("found save, tid %08lX%08lX",
            (unsigned long)(ctx.tid >> 32), (unsigned long)ctx.tid);

    int chapter = ctx.plain[CHAPTER_OFFSET];
    int current = ctx.plain[LINK_OFFSET];
    int sel = current;
    int max = (chapter < 10) ? 2 : 3;
    if (sel > max) sel = max;
    bool dirty = true;

    while (aptMainLoop()) {
        if (dirty) {
            consoleClear();
            printf("ie-secret-link " VERSION "\n\n");
            printf("Game:       GO Galaxy %s (%s)\n", title_name(ctx.tid), ctx.media);
            printf("Save file:  %s (%lu bytes)\n", ctx.filepath, (unsigned long)ctx.size);
            printf("Chapter:    %d\n", chapter);
            printf("Link level: %d\n\n", current);
            printf("Select new link level: < %d >\n\n", sel);
            if (chapter < 10)
                printf("Level 3 locked: chapter < 10.\n");
            else
                printf("Level 3 only if the version-exclusive\nteam is beaten, else the save glitches.\n");
            printf("\nLEFT/RIGHT: change  A: apply  START: quit\n");
            dirty = false;
        }
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_START) break;
        if (k & KEY_LEFT)  { if (sel > 0) sel--; dirty = true; }
        if (k & KEY_RIGHT) { if (sel < max) sel++; dirty = true; }
        if (k & KEY_A) {
            consoleClear();
            printf("Set link level %d -> %d\n\n", current, sel);
            if (sel == 3) {
                printf("Level 3 REQUIRES the version-exclusive\nteam beaten. Glitched save otherwise.\n\n");
                printf("A: I beat it, proceed   B: cancel\n");
                if (!(wait_key() & KEY_A)) { dirty = true; continue; }
            } else {
                printf("A: confirm   B: cancel\n");
                if (!(wait_key() & KEY_A)) { dirty = true; continue; }
            }
            printf("\nBacking up original save to SD...\n");
            if (!backup_save(&ctx)) {
                printf("Backup FAILED, not touching the save.\n");
            } else if (write_save(&ctx, (u8)sel)) {
                current = sel;
                printf("Patched and committed. Start the game\nand check the Inalink.\n");
            } else {
                printf("WRITE FAILED. Save may be untouched;\nbackup is on SD either way.\n");
            }
            printf("\nPress any key.\n");
            wait_key();
            dirty = true;
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
