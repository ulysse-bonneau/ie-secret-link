#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stdio.h>

#define VERSION        "v0.4.0"
#define GALAXY_MAGIC   0x40F1

/* decrypted-save offsets (see NOTES.md) */
#define TIME_OFFSET     0x20
#define NAME_OFFSET     0x3C
#define TEAMNAME_OFFSET 0x5C
#define NAME_FIELD_LEN  0x20
#define LINK_OFFSET     0x90B4
#define CHAPTER_OFFSET  0x9F1C
#define PLAYERS_OFFSET  0xF83C
#define PLAYER_BLOCK    250
#define MAX_PLAYERS     336
#define MONEY_OFFSET    0x268D0
#define COIN_OFFSET     0x26CC8

#define BACKUP_DIR     "/IESM"
#define OLD_BACKUP_DIR "/ie-secret-link"

typedef struct {
    FS_Archive arch;
    char filepath[0x220]; /* path inside the save archive, leading '/' */
    u8 *raw;    /* encrypted, as on disk */
    u8 *plain;  /* decrypted copy */
    u32 size;
    u64 tid;
    const char *media;
    int matches;
} SaveCtx;

/* main.c */
extern FILE *logfp;
void logline(const char *fmt, ...);
const char *title_name(u64 tid);
bool find_save(SaveCtx *ctx);
bool commit_plain(SaveCtx *ctx);

/* ui.c */
extern PrintConsole topcon, botcon;
u32  wait_key(void);
void ui_header(void);
bool ui_dialog(const char *yes, const char *text, bool warn);
void ui_notice(const char *text, bool ok);
int  ui_list(const char *title, const char *const *lines, int n, int cursor);
bool ui_text(const char *hint, const char *initial, char *out, size_t outsz);
bool ui_number(const char *hint, int initial, int min, int max, int *out);

#define C_RESET  "\x1b[0m"
#define C_TITLE  "\x1b[30;46m"
#define C_KEY    "\x1b[36m"
#define C_VAL    "\x1b[33m"
#define C_SEL    "\x1b[30;47m"
#define C_WARN   "\x1b[31m"
#define C_OK     "\x1b[32m"
#define C_DIM    "\x1b[35m"

/* backup.c */
void migrate_backups(void);
bool backup_save(SaveCtx *ctx, const char *name); /* NULL = timestamped auto name */
void backup_manager(SaveCtx *ctx);

/* editors.c */
void link_level_editor(SaveCtx *ctx);
void sdlink_unlock(SaveCtx *ctx);
void saveinfo_editor(SaveCtx *ctx);
void player_editor(SaveCtx *ctx);

/* guarded commit used by all editors: backup, write, report */
bool apply_changes(SaveCtx *ctx);
