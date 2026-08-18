/* ie-secret-link: set the secret link level in Inazuma Eleven GO Galaxy
 * (Big Bang / Supernova) saves, directly on console.
 * Format ported from Tiniifan/InazumaElevenSaveEditor — see NOTES.md. */

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define GALAXY_MAGIC   0x40F1
#define LINK_OFFSET    0x90B4
#define CHAPTER_OFFSET 0x9F1C
#define BACKUP_DIR     "/ie-secret-link"

static const u64 TITLE_IDS[2] = {
    0x000400000010BA00ULL, /* Big Bang */
    0x000400000010BB00ULL, /* Supernova */
};
static const char *TITLE_NAMES[2] = { "Big Bang", "Supernova" };

static const u16 odd_primes[256] = {
       3,    5,    7,   11,   13,   17,   19,   23,   29,   31,   37,   41,   43,   47,   53,   59,
      61,   67,   71,   73,   79,   83,   89,   97,  101,  103,  107,  109,  113,  127,  131,  137,
     139,  149,  151,  157,  163,  167,  173,  179,  181,  191,  193,  197,  199,  211,  223,  227,
     229,  233,  239,  241,  251,  257,  263,  269,  271,  277,  281,  283,  293,  307,  311,  313,
     317,  331,  337,  347,  349,  353,  359,  367,  373,  379,  383,  389,  397,  401,  409,  419,
     421,  431,  433,  439,  443,  449,  457,  461,  463,  467,  479,  487,  491,  499,  503,  509,
     521,  523,  541,  547,  557,  563,  569,  571,  577,  587,  593,  599,  601,  607,  613,  617,
     619,  631,  641,  643,  647,  653,  659,  661,  673,  677,  683,  691,  701,  709,  719,  727,
     733,  739,  743,  751,  757,  761,  769,  773,  787,  797,  809,  811,  821,  823,  827,  829,
     839,  853,  857,  859,  863,  877,  881,  883,  887,  907,  911,  919,  929,  937,  941,  947,
     953,  967,  971,  977,  983,  991,  997, 1009, 1013, 1019, 1021, 1031, 1033, 1039, 1049, 1051,
    1061, 1063, 1069, 1087, 1091, 1093, 1097, 1103, 1109, 1117, 1123, 1129, 1151, 1153, 1163, 1171,
    1181, 1187, 1193, 1201, 1213, 1217, 1223, 1229, 1231, 1237, 1249, 1259, 1277, 1279, 1283, 1289,
    1291, 1297, 1301, 1303, 1307, 1319, 1321, 1327, 1361, 1367, 1373, 1381, 1399, 1409, 1423, 1427,
    1429, 1433, 1439, 1447, 1451, 1453, 1459, 1471, 1481, 1483, 1487, 1489, 1493, 1499, 1511, 1523,
    1531, 1543, 1549, 1553, 1559, 1567, 1571, 1579, 1583, 1597, 1601, 1607, 1609, 1613, 1619, 1621
};

static void build_table(u32 seed, u8 table[256])
{
    u64 s = seed;
    u32 st[4];
    for (int i = 0; i < 3; i++) {
        s ^= s >> 30;
        s = (u32)((u64)(i + 1) + s * 0x6C078965ULL);
        st[i] = (u32)s;
    }
    st[3] = 0x03DF95B3;
    for (int i = 0; i < 256; i++) table[i] = i;
    for (int i = 0; i < 4096; i++) {
        u32 x = st[0], y = st[3];
        st[0] = st[1]; st[1] = st[2]; st[2] = st[3];
        x ^= x << 11;
        x ^= x >> 8;
        y ^= y >> 19;
        st[3] = x ^ y;
        u32 r = st[3] % 0x10000;
        u8 r1 = r & 0xFF, r2 = (r >> 8) & 0xFF;
        if (r1 != r2) {
            u8 a = table[r1], b = table[r2];
            u8 t = table[a]; table[a] = table[b]; table[b] = t;
        }
    }
}

/* symmetric: encrypts and decrypts everything but the 8-byte trailer */
static void xor_body(u8 *buf, u32 len)
{
    u8 table[256];
    u32 seed;
    memcpy(&seed, buf + len - 4, 4);
    build_table(seed, table);
    u32 ka = 0;
    for (u32 i = 0; i < len - 8; i++) {
        if ((i & 0xFF) == 0) ka = odd_primes[table[(i & 0xFF00) >> 8]];
        buf[i] ^= table[(ka * (i + 1)) & 0xFF];
    }
}

static u32 crc32_zlib(const u8 *buf, u32 len)
{
    u32 c = 0xFFFFFFFF;
    for (u32 i = 0; i < len; i++) {
        c ^= buf[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320 & (0 - (c & 1)));
    }
    return ~c;
}

typedef struct {
    FS_Archive arch;
    char filename[0x107];
    u8 *raw;    /* encrypted, as on disk */
    u8 *plain;  /* decrypted copy */
    u32 size;
    int title;  /* index into TITLE_IDS */
    const char *media;
} SaveCtx;

static bool try_load_file(SaveCtx *ctx, const char *name, u64 fsize)
{
    if (fsize < 0x30000 || fsize > 0x400000) return false;
    Handle f;
    char path[0x110];
    snprintf(path, sizeof(path), "/%s", name);
    if (R_FAILED(FSUSER_OpenFile(&f, ctx->arch, fsMakePath(PATH_ASCII, path),
                                 FS_OPEN_READ, 0))) return false;
    u32 size = (u32)fsize, read = 0;
    u8 *raw = malloc(size);
    Result rc = FSFILE_Read(f, &read, 0, raw, size);
    FSFILE_Close(f);
    if (R_FAILED(rc) || read != size) { free(raw); return false; }
    u8 *plain = malloc(size);
    memcpy(plain, raw, size);
    xor_body(plain, size);
    u16 magic;
    memcpy(&magic, plain + 4, 2);
    if (magic != GALAXY_MAGIC) { free(raw); free(plain); return false; }
    ctx->raw = raw;
    ctx->plain = plain;
    ctx->size = size;
    strncpy(ctx->filename, name, sizeof(ctx->filename) - 1);
    return true;
}

static bool find_save(SaveCtx *ctx)
{
    const FS_MediaType medias[2] = { MEDIATYPE_GAME_CARD, MEDIATYPE_SD };
    const char *media_names[2] = { "game card", "SD (installed)" };

    for (int m = 0; m < 2; m++) {
        for (int t = 0; t < 2; t++) {
            u32 path[3] = { medias[m], (u32)TITLE_IDS[t], (u32)(TITLE_IDS[t] >> 32) };
            FS_Archive arch;
            FS_Path bpath = { PATH_BINARY, sizeof(path), path };
            if (R_FAILED(FSUSER_OpenArchive(&arch, ARCHIVE_USER_SAVEDATA, bpath)))
                continue;
            ctx->arch = arch;
            ctx->title = t;
            ctx->media = media_names[m];

            Handle dir;
            if (R_SUCCEEDED(FSUSER_OpenDirectory(&dir, arch, fsMakePath(PATH_ASCII, "/")))) {
                FS_DirectoryEntry ent;
                u32 n;
                while (R_SUCCEEDED(FSDIR_Read(dir, &n, 1, &ent)) && n == 1) {
                    char name[0x107];
                    int j = 0;
                    for (; j < 0x106 && ent.name[j]; j++)
                        name[j] = (ent.name[j] < 0x80) ? (char)ent.name[j] : '?';
                    name[j] = 0;
                    if (ent.attributes & FS_ATTRIBUTE_DIRECTORY) continue;
                    if (try_load_file(ctx, name, ent.fileSize)) {
                        FSDIR_Close(dir);
                        return true;
                    }
                }
                FSDIR_Close(dir);
            }
            FSUSER_CloseArchive(arch);
        }
    }
    return false;
}

static bool backup_save(SaveCtx *ctx)
{
    mkdir(BACKUP_DIR, 0777);
    char path[256];
    for (int i = 0; i < 1000; i++) {
        snprintf(path, sizeof(path), BACKUP_DIR "/%s.%03d.bak", ctx->filename, i);
        FILE *probe = fopen(path, "rb");
        if (probe) { fclose(probe); continue; }
        FILE *out = fopen(path, "wb");
        if (!out) return false;
        bool ok = fwrite(ctx->raw, 1, ctx->size, out) == ctx->size;
        fclose(out);
        if (ok) printf("Backup: sd:%s\n", path);
        return ok;
    }
    return false;
}

static bool write_save(SaveCtx *ctx, u8 level)
{
    ctx->plain[LINK_OFFSET] = level;
    u8 *enc = malloc(ctx->size);
    memcpy(enc, ctx->plain, ctx->size);
    xor_body(enc, ctx->size);
    u32 crc = crc32_zlib(enc, ctx->size - 8);
    memcpy(enc + ctx->size - 8, &crc, 4);

    char path[0x110];
    snprintf(path, sizeof(path), "/%s", ctx->filename);
    Handle f;
    bool ok = false;
    if (R_SUCCEEDED(FSUSER_OpenFile(&f, ctx->arch, fsMakePath(PATH_ASCII, path),
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

    printf("ie-secret-link\n\n");

    SaveCtx ctx = {0};
    if (!find_save(&ctx)) {
        printf("No GO Galaxy save found (game card or SD).\n");
        printf("Insert the cartridge and restart.\n\nPress any key to exit.\n");
        wait_key();
        goto out;
    }

    int chapter = ctx.plain[CHAPTER_OFFSET];
    int current = ctx.plain[LINK_OFFSET];
    int sel = current;
    int max = (chapter < 10) ? 2 : 3;
    if (sel > max) sel = max;
    bool dirty = true;

    while (aptMainLoop()) {
        if (dirty) {
            consoleClear();
            printf("ie-secret-link\n\n");
            printf("Game:       GO Galaxy %s (%s)\n", TITLE_NAMES[ctx.title], ctx.media);
            printf("Save file:  %s (%lu bytes)\n", ctx.filename, (unsigned long)ctx.size);
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
    free(ctx.raw);
    free(ctx.plain);
    fsExit();
    gfxExit();
    return 0;
}
