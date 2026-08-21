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

static bool arch_write(SaveCtx *ctx, const char *name, const u8 *buf, u32 size)
{
    char path[0x40];
    snprintf(path, sizeof(path), "/%s", name);
    Handle f;
    if (R_FAILED(FSUSER_OpenFile(&f, ctx->arch, fsMakePath(PATH_ASCII, path),
                                 FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) return false;
    u32 w = 0;
    bool ok = R_SUCCEEDED(FSFILE_SetSize(f, size))
           && R_SUCCEEDED(FSFILE_Write(f, &w, 0, buf, size, FS_WRITE_FLUSH)) && w == size;
    FSFILE_Close(f);
    return ok;
}

/* copy every archive file other than the loaded game save into sidecar files
 * named "<base>.<archivename>"; returns count written */
static int backup_sidecars(SaveCtx *ctx, const char *dir, const char *base)
{
    const char *self = ctx->filepath[0] == '/' ? ctx->filepath + 1 : ctx->filepath;
    Handle d;
    if (R_FAILED(FSUSER_OpenDirectory(&d, ctx->arch, fsMakePath(PATH_ASCII, "/")))) return 0;
    FS_DirectoryEntry e; u32 n; int cnt = 0;
    while (R_SUCCEEDED(FSDIR_Read(d, &n, 1, &e)) && n == 1) {
        if (e.attributes & FS_ATTRIBUTE_DIRECTORY) continue;
        char nm[0x40]; int j = 0;
        for (; j < 0x3F && e.name[j]; j++) nm[j] = (e.name[j] < 0x80) ? (char)e.name[j] : '_';
        nm[j] = 0;
        if (!strcmp(nm, self)) continue;               /* game.ie handled separately */
        if (e.fileSize == 0 || e.fileSize > 0x100000) continue;
        Handle f; char ap[0x40]; snprintf(ap, sizeof(ap), "/%s", nm);
        if (R_FAILED(FSUSER_OpenFile(&f, ctx->arch, fsMakePath(PATH_ASCII, ap), FS_OPEN_READ, 0))) continue;
        u32 sz = (u32)e.fileSize, rd = 0; u8 *b = malloc(sz);
        bool rok = R_SUCCEEDED(FSFILE_Read(f, &rd, 0, b, sz)) && rd == sz;
        FSFILE_Close(f);
        if (rok) {
            char sp[0x340]; snprintf(sp, sizeof(sp), "%s/%s.%s", dir, base, nm);
            FILE *o = fopen(sp, "wb");
            if (o) { if (fwrite(b, 1, sz, o) == sz) cnt++; fclose(o); }
        }
        free(b);
    }
    FSDIR_Close(d);
    return cnt;
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
    if (!ok) { remove(path); return false; }
    /* derive base (strip ".bak") and copy companion archive files (head.ie) */
    char base[0x80];
    const char *fn = strrchr(path, '/'); fn = fn ? fn + 1 : path;
    snprintf(base, sizeof(base), "%.*s", (int)(strlen(fn) - 4), fn);
    int sc = backup_sidecars(ctx, dir, base);
    logline("backup: sd:%s (+%d companion file(s))", path, sc);
    return true;
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
    const char *self = ctx->filepath[0] == '/' ? ctx->filepath + 1 : ctx->filepath;
    bool ok = arch_write(ctx, self, buf, (u32)size);
    /* restore companion files: "<full-without-.bak>.<name>" */
    if (ok) {
        char stem[0x340];
        snprintf(stem, sizeof(stem), "%.*s", (int)(strlen(full) - 4), full);
        Handle d;
        if (R_SUCCEEDED(FSUSER_OpenDirectory(&d, ctx->arch, fsMakePath(PATH_ASCII, "/")))) {
            FS_DirectoryEntry e; u32 n;
            while (R_SUCCEEDED(FSDIR_Read(d, &n, 1, &e)) && n == 1) {
                if (e.attributes & FS_ATTRIBUTE_DIRECTORY) continue;
                char nm[0x40]; int j = 0;
                for (; j < 0x3F && e.name[j]; j++) nm[j] = (e.name[j] < 0x80) ? (char)e.name[j] : '_';
                nm[j] = 0;
                if (!strcmp(nm, self)) continue;
                char sp[0x360]; snprintf(sp, sizeof(sp), "%s.%s", stem, nm);
                FILE *sf = fopen(sp, "rb");
                if (!sf) continue;
                fseek(sf, 0, SEEK_END); long ss = ftell(sf); fseek(sf, 0, SEEK_SET);
                u8 *sb = malloc(ss);
                if (fread(sb, 1, ss, sf) == (size_t)ss) {
                    if (arch_write(ctx, nm, sb, (u32)ss)) logline("  companion %s (%ld b)", nm, ss);
                }
                free(sb); fclose(sf);
            }
            FSDIR_Close(d);
        }
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


/* decrypt a backup fully; caller frees */
static u8 *load_bak_plain(const char *full, long *size_out)
{
    FILE *in = fopen(full, "rb");
    if (!in) return NULL;
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    u8 *buf = malloc(size);
    bool ok = fread(buf, 1, size, in) == (size_t)size;
    fclose(in);
    if (!ok || size < 0x1000) { free(buf); return NULL; }
    ie_xor_body(buf, (u32)size);
    *size_out = size;
    return buf;
}

static const char *region_of(const GameDef *g, u32 i)
{
    if (i >= g->pdata_off && i < g->pdata_off + (u32)g->pmax * g->pblock) return "players";
    if (i >= g->pindex_off && i < g->pindex_off + (u32)g->pmax * 4) return "roster idx";
    if (i >= g->g1_off && i < g->g1_off + (u32)g->g1_n * 12) return "items g1";
    if (i >= g->g2_off && i < g->g2_off + (u32)g->g2_n * 16) return "items g2";
    if (i >= g->g3_off && i < g->g3_off + (u32)g->g3_n * 8) return "items g3";
    if (g->t_count && i >= g->t_info && i < g->t_players + (u32)g->t_count * 0x40) return "teams";
    if (i >= g->link_off && i < g->link_off + 4) return "link";
    if (g->records_n && i >= g->records_off && i < g->records_off + (u32)g->records_n) return "records";
    if (i >= g->money_off && i < g->money_off + 8) return "money";
    if (g->coin_off && i >= g->coin_off && i < g->coin_off + 10) return "coins";
    if (i < 0x100) return "header";
    return "?";
}

/* byte-diff two decrypted backups, merged into runs, on screen and log */
static void diff_backups(SaveCtx *ctx, const char *dir, const char *na, const char *nb)
{
    char full[0x300];
    long sa = 0, sb = 0;
    snprintf(full, sizeof(full), "%s/%s", dir, na);
    u8 *a = load_bak_plain(full, &sa);
    snprintf(full, sizeof(full), "%s/%s", dir, nb);
    u8 *b = load_bak_plain(full, &sb);
    if (!a || !b) {
        free(a); free(b);
        ui_header();
        ui_notice("Could not read both backups.", false);
        return;
    }
    long len = (sa < sb) ? sa : sb;

    static char rows[200][48];
    const char *lines[201];
    int n = 0;
    logline("diff %s vs %s:", na, nb);
    if (sa != sb) {
        snprintf(rows[n], 48, "sizes differ: %ld vs %ld", sa, sb);
        logline("%s", rows[n]);
        lines[n] = rows[n];
        n++;
    }
    long i = 0;
    while (i < len - 8 && n < 200) {
        if (a[i] == b[i]) { i++; continue; }
        long start = i, last = i;
        while (i < len - 8 && i - last < 16) {
            if (a[i] != b[i]) last = i;
            i++;
        }
        long rl = last - start + 1;
        if (rl <= 4) {
            char va[12], vb[12];
            int o = 0;
            for (long k = 0; k < rl; k++) {
                snprintf(va + o, 3, "%02X", a[start + k]);
                snprintf(vb + o, 3, "%02X", b[start + k]);
                o += 2;
            }
            snprintf(rows[n], 48, "0x%06lX %s->%s %s", start, va, vb,
                     region_of(ctx->game, (u32)start));
        } else {
            snprintf(rows[n], 48, "0x%06lX +%-4ld %s", start, rl,
                     region_of(ctx->game, (u32)start));
        }
        logline("  %s", rows[n]);
        lines[n] = rows[n];
        n++;
    }
    if (!n) {
        snprintf(rows[n], 48, "identical (outside trailer)");
        lines[n] = rows[n];
        n++;
    }
    logline("diff done: %d run(s)", n);
    ui_list("Diff (also in log.txt)", lines, n, 0);
    free(a);
    free(b);
}


/* test whether the u32 at 0x28 is a running CRC32 or byte-sum over [a..b) */
static void hunt_checksum(SaveCtx *ctx, const char *dir, const char *bakname)
{
    char full[0x300];
    snprintf(full, sizeof(full), "%s/%s", dir, bakname);
    long size = 0;
    u8 *p = load_bak_plain(full, &size);
    if (!p) {
        ui_header();
        ui_notice("Could not read the backup.", false);
        return;
    }
    u32 target;
    memcpy(&target, p + 0x28, 4);

    static char rows[24][48];
    const char *lines[24];
    int n = 0;
    snprintf(rows[n], 48, "target @0x28 = %08lX", (unsigned long)target);
    logline("%s", rows[n]);
    lines[n] = rows[n];
    n++;

    static const u32 starts[] = { 0x2C, 0x30, 0x34, 0x40, 0x80, 0x100, 0x200, 0x400 };
    u32 end = (u32)size - 8;
    for (u32 s = 0; s < sizeof(starts) / sizeof(*starts) && n < 22; s++) {
        u32 a = starts[s];
        u32 crc = 0xFFFFFFFF;
        u32 sum = 0;
        for (u32 i = a; i < end; i++) {
            crc ^= p[i];
            for (int k = 0; k < 8; k++)
                crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
            sum += p[i];
            u32 cv = ~crc;
            if (cv == target && n < 22) {
                snprintf(rows[n], 48, "CRC32 [%05lX..%05lX] MATCH", (unsigned long)a, (unsigned long)(i + 1));
                logline("%s", rows[n]);
                lines[n] = rows[n];
                n++;
            }
            if (sum == target && target > 0x10000 && n < 22) {
                snprintf(rows[n], 48, "SUM   [%05lX..%05lX] MATCH", (unsigned long)a, (unsigned long)(i + 1));
                logline("%s", rows[n]);
                lines[n] = rows[n];
                n++;
            }
        }
    }
    free(p);
    if (n == 1) {
        snprintf(rows[n], 48, "no CRC32/sum match found");
        logline("%s", rows[n]);
        lines[n] = rows[n];
        n++;
    }
    ui_list("Checksum hunt (also in log)", lines, n, 0);
}


/* hunt a u16 checksum: target read at 0x9F16, algorithms x range grid;
 * also locates every other occurrence of the same u16 nearby (mirror) */
static void hunt_checksum16(SaveCtx *ctx, const char *dir, const char *bakname)
{
    char full[0x300];
    snprintf(full, sizeof(full), "%s/%s", dir, bakname);
    long size = 0;
    u8 *p = load_bak_plain(full, &size);
    if (!p) {
        ui_header();
        ui_notice("Could not read the backup.", false);
        return;
    }
    u16 target;
    memcpy(&target, p + 0x9F16, 2);
    u16 target_be = (u16)((target >> 8) | (target << 8));

    static char rows[40][48];
    const char *lines[40];
    int n = 0;
    snprintf(rows[n], 48, "target u16 @9F16 = %04X", target);
    logline("%s", rows[n]);
    lines[n] = rows[n];
    n++;

    /* mirrors: same u16 in 0x8000..0xA400 */
    for (u32 i = 0x8000; i < 0xA400 && n < 8; i += 1) {
        if (i == 0x9F16) continue;
        u16 v;
        memcpy(&v, p + i, 2);
        if (v == target) {
            snprintf(rows[n], 48, "mirror u16 at 0x%05lX", (unsigned long)i);
            logline("%s", rows[n]);
            lines[n] = rows[n];
            n++;
        }
    }

    static const u32 starts[] = { 0x9F18, 0x9F1C, 0x9F20, 0x9F56, 0x9F6C, 0xA000, 0xA394 };
    static const u32 ends[]   = { 0xA394, 0xC020, 0xDC6C, 0xF82C, 0xF83C, 0x26E28, 0x27400 };
    for (u32 s = 0; s < sizeof(starts) / sizeof(*starts) && n < 38; s++) {
        u32 a = starts[s];
        u32 sum = 0;
        u16 ccitt = 0xFFFF, ccitt0 = 0, ibm = 0;
        u32 crc = 0xFFFFFFFF;
        u32 emax = (u32)size - 8;
        for (u32 i = a; i < emax; i++) {
            u8 c = p[i];
            sum += c;
            ccitt ^= (u16)(c << 8);
            ccitt0 ^= (u16)(c << 8);
            for (int k = 0; k < 8; k++) {
                ccitt = (ccitt & 0x8000) ? (u16)((ccitt << 1) ^ 0x1021) : (u16)(ccitt << 1);
                ccitt0 = (ccitt0 & 0x8000) ? (u16)((ccitt0 << 1) ^ 0x1021) : (u16)(ccitt0 << 1);
            }
            ibm ^= c;
            for (int k = 0; k < 8; k++)
                ibm = (ibm & 1) ? (u16)((ibm >> 1) ^ 0xA001) : (u16)(ibm >> 1);
            crc ^= c;
            for (int k = 0; k < 8; k++)
                crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
            /* check at each candidate end (i+1) */
            for (u32 e2 = 0; e2 < sizeof(ends) / sizeof(*ends); e2++) {
                if (i + 1 != ends[e2] || ends[e2] <= a) continue;
                struct { const char *nm; u16 v; } cand[] = {
                    { "sum16", (u16)sum }, { "ccittF", ccitt }, { "ccitt0", ccitt0 },
                    { "crc16", ibm }, { "crc32lo", (u16)~crc },
                };
                for (u32 c2 = 0; c2 < 5 && n < 38; c2++) {
                    if (cand[c2].v != target && cand[c2].v != target_be) continue;
                    snprintf(rows[n], 48, "%s [%05lX..%05lX] MATCH%s", cand[c2].nm,
                             (unsigned long)a, (unsigned long)(i + 1),
                             (cand[c2].v == target_be) ? " (BE)" : "");
                    logline("%s", rows[n]);
                    lines[n] = rows[n];
                    n++;
                }
            }
        }
    }
    free(p);
    if (n <= 1) {
        snprintf(rows[n], 48, "no u16 match found");
        lines[n] = rows[n];
        n++;
    }
    logline("hunt16 done");
    ui_list("Checksum hunt v2 (also in log)", lines, n, 0);
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
            bool bok = backup_save(ctx, name); ui_notice(bok ? "Backup written." : "Backup failed.", bok);
            continue;
        }

        char *bak = names[pick - 1];
        const char *actions[] = { "Restore over current save", "Rename", "Delete",
                                  "Advanced (debug tools)", "Back" };
        int act = ui_list(bak, actions, 5, 0);
        if (act == 3) {
            const char *dbg[] = { "Diff against another backup",
                                  "Hunt checksum @0x28", "Hunt u16 checksum @0x9F16", "Back" };
            int d = ui_list(bak, dbg, 4, 0);
            if (d == 1) { hunt_checksum(ctx, dir, bak); continue; }
            if (d == 2) { hunt_checksum16(ctx, dir, bak); continue; }
            if (d == 0) {
                const char *lines2[MAX_BAKS];
                int m = 0;
                for (int i = 0; i < n; i++)
                    if (i != pick - 1) lines2[m++] = names[i];
                if (!m) continue;
                int other = ui_list("Diff against...", lines2, m, 0);
                if (other < 0) continue;
                int oi = (other >= pick - 1) ? other + 1 : other;
                diff_backups(ctx, dir, bak, names[oi]);
            }
            continue;
        }
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
            bool rok2 = restore_from_path(ctx, full);
            ui_notice(rok2 ? "Restored and committed." : "Restore failed, see log.", rok2);
        } else if (act == 1) {
            char base[40];
            snprintf(base, sizeof(base), "%.*s", (int)(strlen(bak) - 4), bak);
            char name[40];
            if (!ui_text("New name", base, name, 40)) continue;
            char from[0x300], to[0x300];
            snprintf(from, sizeof(from), "%s/%s", dir, bak);
            snprintf(to, sizeof(to), "%s/%s.bak", dir, name);
            ui_header();
            bool mok = (rename(from, to) == 0); ui_notice(mok ? "Renamed." : "Rename failed.", mok);
        } else if (act == 2) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Delete %s permanently?", bak);
            if (!ui_dialog("delete", msg, true)) continue;
            char full[0x300];
            snprintf(full, sizeof(full), "%s/%s", dir, bak);
            ui_header();
            bool dok = (remove(full) == 0); ui_notice(dok ? "Deleted." : "Delete failed.", dok);
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
            ui_notice("Refused: backup is not a save of\nthis game.", false);
            continue;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "Import %s\nover the current save?", names[f]);
        if (!ui_dialog("import", msg, true)) continue;
        ui_header();
        bool iok = restore_from_path(ctx, full);
        ui_notice(iok ? "Imported and committed." : "Import failed, see log.", iok);
    }
}
