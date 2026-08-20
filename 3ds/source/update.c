/* self-updater: checks the GitHub latest release and installs the new CIA
 * over this title (FBI-style AM streaming install) */
#include <stdlib.h>
#include <string.h>
#include "app.h"

#define REPO "ulysse-bonneau/iesm"
#define LATEST_URL "https://github.com/" REPO "/releases/latest"
#define CIA_URL "https://github.com/" REPO "/releases/latest/download/iesm.cia"

/* open a context following redirects; caller closes */
static Result http_open(httpcContext *ctx, const char *url)
{
    char loc[0x400];
    snprintf(loc, sizeof(loc), "%s", url);
    for (int hop = 0; hop < 6; hop++) {
        Result rc = httpcOpenContext(ctx, HTTPC_METHOD_GET, loc, 1);
        if (R_FAILED(rc)) return rc;
        httpcSetSSLOpt(ctx, SSLCOPT_DisableVerify);
        httpcAddRequestHeaderField(ctx, "User-Agent", "IESM");
        rc = httpcBeginRequest(ctx);
        u32 status = 0;
        if (R_SUCCEEDED(rc)) rc = httpcGetResponseStatusCode(ctx, &status);
        if (R_FAILED(rc)) {
            httpcCloseContext(ctx);
            return rc;
        }
        if (status >= 300 && status < 400) {
            rc = httpcGetResponseHeader(ctx, "Location", loc, sizeof(loc));
            httpcCloseContext(ctx);
            if (R_FAILED(rc)) return rc;
            continue;
        }
        if (status != 200) {
            httpcCloseContext(ctx);
            return -1;
        }
        return 0;
    }
    return -1;
}

/* latest tag from the /releases/latest redirect Location header
 * (github.com works from the 3DS TLS stack; api.github.com does not) */
static bool fetch_latest_tag(char *out, size_t outsz)
{
    httpcContext ctx;
    if (R_FAILED(httpcOpenContext(&ctx, HTTPC_METHOD_GET, LATEST_URL, 1))) return false;
    httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
    httpcAddRequestHeaderField(&ctx, "User-Agent", "IESM");
    bool ok = false;
    char loc[0x200] = "";
    if (R_SUCCEEDED(httpcBeginRequest(&ctx))) {
        u32 status = 0;
        if (R_SUCCEEDED(httpcGetResponseStatusCode(&ctx, &status)) &&
            status >= 300 && status < 400 &&
            R_SUCCEEDED(httpcGetResponseHeader(&ctx, "Location", loc, sizeof(loc))))
            ok = true;
    }
    httpcCloseContext(&ctx);
    if (!ok) return false;
    const char *p = strstr(loc, "/tag/");
    if (!p) return false;
    p += 5;
    size_t l = strlen(p);
    while (l && (p[l - 1] == '\r' || p[l - 1] == '\n' || p[l - 1] == '/')) l--;
    if (!l || l >= outsz) return false;
    memcpy(out, p, l);
    out[l] = 0;
    return true;
}

static bool install_cia(void)
{
    httpcContext ctx;
    if (R_FAILED(http_open(&ctx, CIA_URL))) return false;
    u32 total = 0;
    httpcGetDownloadSizeState(&ctx, NULL, &total);

    if (R_FAILED(amInit())) {
        httpcCloseContext(&ctx);
        return false;
    }
    Handle cia;
    if (R_FAILED(AM_StartCiaInstall(MEDIATYPE_SD, &cia))) {
        amExit();
        httpcCloseContext(&ctx);
        return false;
    }

    static u8 buf[0x20000];
    u64 off = 0;
    bool ok = true;
    while (ok) {
        u32 got = 0;
        Result rc = httpcDownloadData(&ctx, buf, sizeof(buf), &got);
        if (got) {
            u32 written = 0;
            if (R_FAILED(FSFILE_Write(cia, &written, off, buf, got, 0)) || written != got) {
                ok = false;
                break;
            }
            off += got;
            printf("\r %lu / %lu KB", (unsigned long)(off / 1024), (unsigned long)(total / 1024));
        }
        if (rc == 0) break;
        if (rc != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) {
            ok = false;
            break;
        }
    }
    printf("\n");
    httpcCloseContext(&ctx);
    if (ok && total && off != total) ok = false;
    if (ok)
        ok = R_SUCCEEDED(AM_FinishCiaInstall(cia));
    else
        AM_CancelCIAInstall(cia);
    amExit();
    return ok;
}

void self_update(void)
{
    ui_header();
    if (envIsHomebrew()) {
        ui_notice("Running as .3dsx: update by copying\nthe new file to the SD card.", false);
        return;
    }
    printf(" Checking latest release...\n");
    if (R_FAILED(httpcInit(0))) {
        ui_notice("Network unavailable.", false);
        return;
    }
    char tag[32];
    bool got = fetch_latest_tag(tag, sizeof(tag));
    if (!got) {
        httpcExit();
        ui_notice("Could not reach GitHub.", false);
        return;
    }
    if (!strcmp(tag, VERSION)) {
        httpcExit();
        char msg[64];
        snprintf(msg, sizeof(msg), "Already up to date (%s).", VERSION);
        ui_notice(msg, true);
        return;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "Update available: %s (installed %s).\n\nDownload and install?", tag, VERSION);
    if (!ui_dialog("update", msg, false)) {
        httpcExit();
        return;
    }
    ui_header();
    printf(" Downloading %s...\n", tag);
    bool ok = install_cia();
    httpcExit();
    if (ok) {
        logline("self-updated to %s", tag);
        ui_notice("Installed. Close IESM and relaunch\nto use the new version.", true);
    } else {
        ui_notice("Update FAILED. Install manually\nvia FBI if this persists.", false);
    }
}

/* ---- send a backup to a PC over the local network ---- */

#define IP_FILE BACKUP_DIR "/pc_ip.txt"

void send_file_to_pc(const char *path, const char *name)
{
    FILE *in = fopen(path, "rb");
    if (!in) {
        ui_header();
        ui_notice("Could not read the file.", false);
        return;
    }
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    u8 *buf = malloc(size);
    bool rok = fread(buf, 1, size, in) == (size_t)size;
    fclose(in);
    if (!rok) {
        free(buf);
        return;
    }

    char ip[40] = "";
    FILE *f = fopen(IP_FILE, "r");
    if (f) {
        if (!fgets(ip, sizeof(ip), f)) ip[0] = 0;
        fclose(f);
        for (char *p = ip; *p; p++)
            if (*p == '\n' || *p == '\r') *p = 0;
    }
    char entered[40];
    if (!ui_text("PC IP (run tools/receive_saves.py)", ip[0] ? ip : "192.168.1.", entered, sizeof(entered))) {
        free(buf);
        return;
    }
    f = fopen(IP_FILE, "w");
    if (f) {
        fputs(entered, f);
        fclose(f);
    }

    ui_header();
    printf(" Sending %s (%ld b) to %s...\n", name, size, entered);
    bool ok = false;
    Result step = httpcInit(0);
    logline("send: httpcInit %08lX", (unsigned long)step);
    if (R_SUCCEEDED(step)) {
        char url[0x100];
        snprintf(url, sizeof(url), "http://%s:8123/%s", entered, name);
        httpcContext ctx;
        step = httpcOpenContext(&ctx, HTTPC_METHOD_POST, url, 1);
        logline("send: open %08lX", (unsigned long)step);
        if (R_SUCCEEDED(step)) {
            httpcAddRequestHeaderField(&ctx, "User-Agent", "IESM");
            step = httpcAddPostDataRaw(&ctx, (const u32 *)buf, (u32)size);
            logline("send: postdata %08lX", (unsigned long)step);
            step = httpcBeginRequest(&ctx);
            logline("send: begin %08lX", (unsigned long)step);
            if (R_SUCCEEDED(step)) {
                u32 status = 0;
                step = httpcGetResponseStatusCode(&ctx, &status);
                logline("send: status rc=%08lX http=%lu", (unsigned long)step, (unsigned long)status);
                ok = (status == 200);
            }
            httpcCloseContext(&ctx);
        }
        httpcExit();
    }
    free(buf);
    ui_notice(ok ? "Sent." : "Send FAILED - error codes are on\nthe bottom screen and in log.txt.", ok);
}

/* upload a file to bashupload.com; shows the one-time download URL */
void send_file_to_internet(const char *path, const char *name)
{
    FILE *in = fopen(path, "rb");
    if (!in) {
        ui_header();
        ui_notice("Could not read the file.", false);
        return;
    }
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    u8 *buf = malloc(size);
    bool rok = fread(buf, 1, size, in) == (size_t)size;
    fclose(in);
    if (!rok) {
        free(buf);
        return;
    }

    ui_header();
    printf(" Uploading %s (%ld b)...\n", name, size);
    static char body[0x800];
    u32 total = 0;
    bool ok = false;
    if (R_SUCCEEDED(httpcInit(0))) {
        char url[0x120];
        snprintf(url, sizeof(url), "https://bashupload.com/%s", name);
        httpcContext ctx;
        if (R_SUCCEEDED(httpcOpenContext(&ctx, HTTPC_METHOD_PUT, url, 1))) {
            httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
            httpcAddRequestHeaderField(&ctx, "User-Agent", "IESM");
            httpcAddPostDataRaw(&ctx, (const u32 *)buf, (u32)size);
            if (R_SUCCEEDED(httpcBeginRequest(&ctx))) {
                u32 status = 0;
                httpcGetResponseStatusCode(&ctx, &status);
                if (status == 200) {
                    u32 got = 0;
                    while (total < sizeof(body) - 1) {
                        Result rc = httpcDownloadData(&ctx, (u8 *)body + total,
                                                      sizeof(body) - 1 - total, &got);
                        if (got) total += got;
                        if (rc == 0) break;
                        if (rc != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) break;
                    }
                    body[total] = 0;
                    ok = true;
                }
            }
            httpcCloseContext(&ctx);
        }
        httpcExit();
    }
    free(buf);
    ui_header();
    if (ok) {
        printf(C_OK " Uploaded. Download URL (one use):" C_RESET "\n\n%s\n", body);
        logline("uploaded %s:", name);
        logline("%s", body);
        printf("\n" C_DIM " (also written to log.txt)\n Press any key." C_RESET "\n");
        wait_key();
    } else {
        ui_notice("Upload FAILED.", false);
    }
}
