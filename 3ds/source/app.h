#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stdio.h>
#include "unlock_data.h"
#include "players_db.h"
#include "items_db.h"
#include "moves_db.h"
#include "avatars_db.h"
#include "records_db.h"

#define VERSION "v0.13.2"

#define BACKUP_DIR     "/IESM"
#define OLD_BACKUP_DIR "/ie-secret-link"
#define NAME_FIELD_LEN 0x20

typedef enum { LINK_NONE, LINK_LEVEL, LINK_GO_WORD } LinkKind;

typedef struct {
    const char *name;      /* shown in the save picker */
    const char *shortname; /* backup name prefix */
    u16 magic;
    /* save info */
    u32 time_off, name_off, team_off;
    u32 money_off;         /* prestige i32; +4 = friendship when has_friendship */
    bool has_friendship;
    u32 coin_off;          /* 5 x s16, Galaxy only (0 = none) */
    u32 chapter_off;       /* 0 = not known for this game */
    /* secret link */
    LinkKind link_kind;
    u32 link_off;
    /* players */
    u32 pdata_off, pindex_off;
    int pmax, pblock;      /* block stride; 0 = players unsupported */
    u32 p_id_off, p_gp_off, p_invest_off; /* TP = gp+2, Freedom = gp+4, Level = gp+6 */
    u32 p_moves_off;       /* 6 x 12-byte MoveBlock inside the player block */
    u32 p_avatar_off;      /* u32 id + u8 level + u8 usage */
    u32 p_equip_off;       /* 4 x i32 inventory indexes: boots,bracelet,pendant,gloves */
    u32 p_invoke_off;      /* invoke/armed bit byte (GO: CanInvoke byte) */
    u32 p_style_off;       /* 0 = no style field (GO) */
    u32 p_part_off;        /* participation s16, score = +2 */
    u32 p_key_off;         /* GO only: key-player byte (0 = none) */
    bool has_seesaw;       /* GO/CS trade stats at 0 freedom; Galaxy cannot */
    u32 totem_off;         /* Galaxy only: totem id at block start (0 = none) */
    u32 records_off; int records_n; /* play-record bitfield bytes (0 = unknown) */
    const RecordInfo *rdb; int rdb_count;
    const MoveInfo *mdb;   int mdb_count;
    const AvatarInfo *adb; int adb_count;
    const PlayerInfo *db;
    int db_count;
    /* inventory: group1 (idx,id,qty = 12 B), group2 (idx,id,qty,equipped = 16 B) */
    u32 g1_off; int g1_n;
    u32 g2_off; int g2_n;
    u32 g3_off; int g3_n;  /* ownership-only entries (idx,id = 8 B), no quantity */
    const ItemInfo *idb;
    int idb_count;
    /* custom teams (tactics) */
    u32 t_info, t_info_str;    /* coach/formation/kit/emblem i32 + 16+16 bytes */
    u32 t_name, t_name_str;    /* 0 = teams are unnamed (GO) */
    u32 t_players;             /* 16 x i32 roster indexes, stride 0x40 */
    int t_count;
    /* unlock-all-data */
    const UnlockRegion *unlock;
    int unlock_n;
    u32 unlock_end;        /* highest offset written, for a size guard */
    const char *unlock_label;
} GameDef;

extern const GameDef GAMES[];
extern const int GAMES_N;

typedef struct {
    FS_Archive arch;
    char filepath[0x220];
    u8 *raw;
    u8 *plain;
    u32 size;
    u64 tid;
    const char *media;
    const GameDef *game;
} SaveCtx;

/* main.c */
extern FILE *logfp;
const char *tid_variant(u64 tid); /* "Big Bang"/"Supernova" or NULL */
void logline(const char *fmt, ...);
bool commit_plain(SaveCtx *ctx);

/* ui.c */
extern PrintConsole topcon, botcon;
u32  wait_key(void);
void ui_header(void);
bool ui_dialog(const char *yes, const char *text, bool warn);
void ui_notice(const char *text, bool ok);
int  ui_list(const char *title, const char *const *lines, int n, int cursor);
int  ui_list_adj(const char *title, const char *const *lines, int n, int cursor, int *delta);
bool ui_text(const char *hint, const char *initial, char *out, size_t outsz);
bool ui_text_opt(const char *hint, char *out, size_t outsz); /* empty allowed */
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
bool backup_save(SaveCtx *ctx, const char *name); /* NULL = auto date name */
void backup_manager(SaveCtx *ctx);
void export_import(SaveCtx *ctx);

/* editors.c */
void link_level_editor(SaveCtx *ctx);
bool link_apply(SaveCtx *ctx, int sel);
void sdlink_unlock(SaveCtx *ctx);
void saveinfo_editor(SaveCtx *ctx);
void player_editor(SaveCtx *ctx);
void inventory_editor(SaveCtx *ctx);
void records_unlock(SaveCtx *ctx);
void teams_editor(SaveCtx *ctx);
bool apply_changes(SaveCtx *ctx);
