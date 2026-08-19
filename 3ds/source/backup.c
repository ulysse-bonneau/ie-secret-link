#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "app.h"
#include "codec.h"

static void game_dir(SaveCtx *ctx, char *out, size_t outsz)
{
    snprintf(out, outsz, BACKUP_DIR "/%s", ctx->game->shortname);
    mkdir(out, 0777);
}

/* sort a directory's stray .bak files into per-game subfolders; files with an
 * unknown prefix predate multi-game support and were always Galaxy */
static int sort_dir_into_games(const char *dirpath)
{
    DIR *d = opendir(dirpath);
    if (!d) return 0;
    struct dirent *e;
    int moved = 0;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".bak")) continue;
        const char *sub = "galaxy";
        for (int i = 0; i < GAMES_N; i++) {
            size_t sl = strlen(GAMES[i].shortname);
            if (!strncmp(e->d_name, GAMES[i].shortname, sl) && e->d_name[sl] == '-') {
                sub = GAMES[i].shortname;
                break;
            }
        }
        char to[0x300];
        snprintf(to, sizeof(to), BACKUP_DIR "/%s", sub);
        mkdir(to, 0777);
        char from[0x300];
        snprintf(from, sizeof(from), "%s/%s", dirpath, e->d_name);
        snprintf(to, sizeof(to), BACKUP_DIR "/%s/%s", sub, e->d_name);
        if (rename(from, to) == 0) moved++;
    }
    closedir(d);
    return moved;
}

void migrate_backups(void)
{
    mkdir(BACKUP_DIR, 0777);
    int moved = sort_dir_into_games(OLD_BACKUP_DIR) + sort_dir_into_games(BACKUP_DIR);
    if (moved) logline("sorted %d backup(s) into game folders", moved);
}

bool backup_save(SaveCtx *ctx, const char *name)
{
    char dir[0x40], path[0x300];
    game_dir(ctx, dir, sizeof(dir));
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    const char *var = tid_variant(ctx->tid);
    /* BB/Supernova share one format+folder: tag the filename with the version */
    const char *tag = var ? ((var[0] == 'B') ? "bb-" : "sn-") : "";
    if (name)
        snprintf(path, sizeof(path), "%s/%s.bak", dir, name);
    else
        snprintf(path, sizeof(path), "%s/%sauto-%04d-%02d-%02d-%02d%02d%02d.bak",
                 dir, tag, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec);
    FILE *out = fopen(path, "wb");
    if (!out) return false;
    bool ok = fwrite(ctx->raw, 1, ctx->size, out) == ctx->size;
    fclose(out);
    if (ok) logline("backup: sd:%s", path);
    else remove(path);
    return ok;
}

/* the backup's magic must match the currently loaded game */
static bool bak_matches_game(SaveCtx *ctx, const char *full, long size)
{
    if (size < 0x1000) return false;
    FILE *in = fopen(full, "rb");
    if (!in) return false;
    u8 head[6];
    u32 seed = 0;
    bool ok = fread(head, 1, 6, in) == 6 &&
              fseek(in, size - 4, SEEK_SET) == 0 &&
              fread(&seed, 1, 4, in) == 4;
    fclose(in);
    return ok && ie_magic(head, seed) == ctx->game->magic;
}

static bool restore_from_path(SaveCtx *ctx, const char *full)
{
    FILE *in = fopen(full, "rb");
    if (!in) return false;
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    u8 *buf = malloc(size);
    bool rok = fread(buf, 1, size, in) == (size_t)size;
    fclose(in);
    if (!rok || size < 0x1000) { free(buf); return false; }

    logline("restoring %s (%ld b) into %s", full, size, ctx->filepath);
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
    if (ok) {
        free(ctx->raw);
        free(ctx->plain);
        ctx->size = (u32)size;
        ctx->raw = buf;
        ctx->plain = malloc(size);
        memcpy(ctx->plain, buf, size);
        ie_xor_body(ctx->plain, ctx->size);
    } else {
        free(buf);
    }
    return ok;
}

static int cmp_desc(const void *a, const void *b)
{
    return strcmp((const char *)b, (const char *)a);
}

#define MAX_BAKS 100
#define BAKNAME 56

void backup_manager(SaveCtx *ctx)
{
    int cursor = 0;
    while (aptMainLoop()) {
        static char names[MAX_BAKS][BAKNAME];
        int n = 0;
        char dir[0x40];
        game_dir(ctx, dir, sizeof(dir));
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) && n < MAX_BAKS) {
                size_t l = strlen(e->d_name);
                if (l > 4 && l < BAKNAME && !strcmp(e->d_name + l - 4, ".bak"))
                    snprintf(names[n++], BAKNAME, "%s", e->d_name);
            }
            closedir(d);
        }
        qsort(names, n, BAKNAME, cmp_desc);

        const char *lines[MAX_BAKS + 1];
        static char newlabel[] = "[ New backup ]";
        lines[0] = newlabel;
        for (int i = 0; i < n; i++) lines[i + 1] = names[i];

        int pick = ui_list("Backups", lines, n + 1, cursor);
        if (pick < 0) return;
        cursor = pick;

        if (pick == 0) {
            char name[40] = "";
            time_t t = time(NULL);
            struct tm *tm = localtime(&t);
            const char *var = tid_variant(ctx->tid);
            char def[48];
            snprintf(def, sizeof(def), "%s%04d-%02d-%02d-%02d%02d",
                     var ? ((var[0] == 'B') ? "bb-" : "sn-") : "",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
            if (!ui_text("Backup name", def, name, 40)) continue;
            ui_header();
            ui_notice(backup_save(ctx, name) ? "Backup written." : "Backup FAILED.", true);
            continue;
        }

        char *bak = names[pick - 1];
        const char *actions[] = { "Restore over current save", "Rename", "Delete", "Back" };
        int act = ui_list(bak, actions, 4, 0);
        if (act == 0) {
            char full[0x300];
            struct stat st;
            snprintf(full, sizeof(full), "%s/%s", dir, bak);
            if (stat(full, &st) != 0 || !bak_matches_game(ctx, full, st.st_size)) {
                ui_header();
                ui_notice("Refused: backup is not a save of\nthis game.", false);
                continue;
            }
            const char *var = tid_variant(ctx->tid);
            char msg[160];
            snprintf(msg, sizeof(msg), "Restore %s\nover the current %s save?", bak,
                     var ? var : ctx->game->name);
            if (!ui_dialog("restore", msg, false)) continue;
            ui_header();
            ui_notice(restore_from_path(ctx, full) ? "Restored and committed."
                                                    : "RESTORE FAILED, see log.", true);
        } else if (act == 1) {
            char base[40];
            snprintf(base, sizeof(base), "%.*s", (int)(strlen(bak) - 4), bak);
            char name[40];
            if (!ui_text("New name", base, name, 40)) continue;
            char from[0x300], to[0x300];
            snprintf(from, sizeof(from), "%s/%s", dir, bak);
            snprintf(to, sizeof(to), "%s/%s.bak", dir, name);
            ui_header();
            ui_notice(rename(from, to) == 0 ? "Renamed." : "Rename failed.", true);
        } else if (act == 2) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Delete %s permanently?", bak);
            if (!ui_dialog("delete", msg, true)) continue;
            char full[0x300];
            snprintf(full, sizeof(full), "%s/%s", dir, bak);
            ui_header();
            ui_notice(remove(full) == 0 ? "Deleted." : "Delete failed.", true);
        }
    }
}

#define EXPORT_DIR BACKUP_DIR "/export"

void export_import(SaveCtx *ctx)
{
    int cursor = 0;
    while (aptMainLoop()) {
        const char *actions[] = { "Export save to sd:" EXPORT_DIR,
                                  "Import a file from sd:" EXPORT_DIR };
        int pick = ui_list("Export / Import", actions, 2, cursor);
        if (pick < 0) return;
        cursor = pick;

        if (pick == 0) {
            mkdir(EXPORT_DIR, 0777);
            time_t t = time(NULL);
            struct tm *tm = localtime(&t);
            char path[0x300];
            snprintf(path, sizeof(path), EXPORT_DIR "/%s-%04d-%02d-%02d-%02d%02d.sav",
                     ctx->game->shortname, tm->tm_year + 1900, tm->tm_mon + 1,
                     tm->tm_mday, tm->tm_hour, tm->tm_min);
            FILE *out = fopen(path, "wb");
            bool ok = out && fwrite(ctx->raw, 1, ctx->size, out) == ctx->size;
            if (out) fclose(out);
            if (ok) logline("exported: sd:%s", path);
            ui_header();
            ui_notice(ok ? "Exported (encrypted raw save)." : "Export FAILED.", ok);
            continue;
        }

        static char names[MAX_BAKS][BAKNAME];
        int n = 0;
        DIR *d = opendir(EXPORT_DIR);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) && n < MAX_BAKS) {
                if (e->d_name[0] == '.') continue;
                size_t l = strlen(e->d_name);
                if (l < BAKNAME) snprintf(names[n++], BAKNAME, "%s", e->d_name);
            }
            closedir(d);
        }
        if (!n) {
            ui_header();
            ui_notice("Nothing in sd:" EXPORT_DIR ".", false);
            continue;
        }
        qsort(names, n, BAKNAME, cmp_desc);
        const char *lines[MAX_BAKS];
        for (int i = 0; i < n; i++) lines[i] = names[i];
        int f = ui_list("Import file", lines, n, 0);
        if (f < 0) continue;
        char full[0x300];
        struct stat st;
        snprintf(full, sizeof(full), EXPORT_DIR "/%s", names[f]);
        if (stat(full, &st) != 0 || !bak_matches_game(ctx, full, st.st_size)) {
            ui_header();
            ui_notice("Refused: not a save of this game.", false);
            continue;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "Import %s\nover the current save?", names[f]);
        if (!ui_dialog("import", msg, true)) continue;
        ui_header();
        ui_notice(restore_from_path(ctx, full) ? "Imported and committed."
                                               : "IMPORT FAILED, see log.", true);
    }
}
