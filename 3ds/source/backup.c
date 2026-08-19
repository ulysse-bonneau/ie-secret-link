#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "app.h"
#include "codec.h"

/* one-time move of .bak files from the pre-rename directory */
void migrate_backups(void)
{
    mkdir(BACKUP_DIR, 0777);
    DIR *d = opendir(OLD_BACKUP_DIR);
    if (!d) return;
    struct dirent *e;
    int moved = 0;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".bak")) continue;
        char from[0x300], to[0x300];
        snprintf(from, sizeof(from), OLD_BACKUP_DIR "/%s", e->d_name);
        snprintf(to, sizeof(to), BACKUP_DIR "/%s", e->d_name);
        if (rename(from, to) == 0) moved++;
    }
    closedir(d);
    if (moved) logline("migrated %d backup(s) to sd:" BACKUP_DIR, moved);
}

bool backup_save(SaveCtx *ctx, const char *name)
{
    char path[0x300];
    if (name) {
        snprintf(path, sizeof(path), BACKUP_DIR "/%s.bak", name);
    } else {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        snprintf(path, sizeof(path), BACKUP_DIR "/auto-%04d%02d%02d-%02d%02d%02d.bak",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec);
    }
    FILE *out = fopen(path, "wb");
    if (!out) return false;
    bool ok = fwrite(ctx->raw, 1, ctx->size, out) == ctx->size;
    fclose(out);
    if (ok) logline("backup: sd:%s", path);
    else remove(path);
    return ok;
}

static bool restore_file(SaveCtx *ctx, const char *bakname)
{
    char full[0x300];
    snprintf(full, sizeof(full), BACKUP_DIR "/%s", bakname);
    FILE *in = fopen(full, "rb");
    if (!in) return false;
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    u8 *buf = malloc(size);
    bool rok = fread(buf, 1, size, in) == (size_t)size;
    fclose(in);
    if (!rok || size < 0x100) { free(buf); return false; }

    logline("restoring %s (%ld b) into %s", bakname, size, ctx->filepath);
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
        /* refresh in-memory copies so the menu shows the restored state */
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

void backup_manager(SaveCtx *ctx)
{
    int cursor = 0;
    while (aptMainLoop()) {
        static char names[MAX_BAKS][48];
        int n = 0;
        DIR *d = opendir(BACKUP_DIR);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) && n < MAX_BAKS) {
                size_t l = strlen(e->d_name);
                if (l > 4 && l < 48 && !strcmp(e->d_name + l - 4, ".bak"))
                    snprintf(names[n++], 48, "%s", e->d_name);
            }
            closedir(d);
        }
        qsort(names, n, 48, cmp_desc);

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
            char def[40];
            snprintf(def, sizeof(def), "manual-%04d%02d%02d-%02d%02d",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
            if (!ui_text("Backup name", def, name, 40)) continue;
            ui_header();
            ui_notice(backup_save(ctx, name) ? "Backup written." : "Backup FAILED.",
                      true);
            continue;
        }

        char *bak = names[pick - 1];
        const char *actions[] = { "Restore over current save", "Rename", "Delete", "Back" };
        int act = ui_list(bak, actions, 4, 0);
        if (act == 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Restore %s\nover the current save?", bak);
            if (!ui_dialog("restore", msg, false)) continue;
            ui_header();
            ui_notice(restore_file(ctx, bak) ? "Restored and committed."
                                             : "RESTORE FAILED, see log.",
                      true);
        } else if (act == 1) {
            char base[40];
            snprintf(base, sizeof(base), "%.*s", (int)(strlen(bak) - 4), bak);
            char name[40];
            if (!ui_text("New name", base, name, 40)) continue;
            char from[0x300], to[0x300];
            snprintf(from, sizeof(from), BACKUP_DIR "/%s", bak);
            snprintf(to, sizeof(to), BACKUP_DIR "/%s.bak", name);
            ui_header();
            ui_notice(rename(from, to) == 0 ? "Renamed." : "Rename failed.", true);
        } else if (act == 2) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Delete %s permanently?", bak);
            if (!ui_dialog("delete", msg, true)) continue;
            char full[0x300];
            snprintf(full, sizeof(full), BACKUP_DIR "/%s", bak);
            ui_header();
            ui_notice(remove(full) == 0 ? "Deleted." : "Delete failed.", true);
        }
    }
}
