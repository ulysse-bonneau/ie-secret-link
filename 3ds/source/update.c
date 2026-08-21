/* self-updater: checks the GitHub latest release and installs the new CIA
 * over this title (FBI-style AM streaming install) */
#include <stdlib.h>
#include <string.h>
#include "app.h"

#define UA "Mozilla/5.0 (Nintendo 3DS; U; ; en) IESM"

/* GetResponseStatusCode can briefly return DOWNLOADPENDING or a transient
 * InvalidState right after BeginRequest; retry a few times. */
static Result get_status(httpcContext *ctx, u32 *status)
{
    Result rc = 0;
    for (int i = 0; i < 25; i++) {
        *status = 0;
        rc = httpcGetResponseStatusCode(ctx, status);
        if (R_SUCCEEDED(rc)) return 0;
        /* httpc can report InvalidState yet still fill a valid HTTP status */
        if (*status >= 100 && *status < 600) return 0;
        svcSleepThread(20 * 1000 * 1000LL); /* 20 ms */
    }
    return rc;
}

#define REPO "ulysse-bonneau/iesm"
#define LATEST_URL "https://github.com/" REPO "/releases/latest"
#define CIA_URL "https://github.com/" REPO "/releases/latest/download/iesm.cia"
#define VERSION_URL "https://raw.githubusercontent.com/" REPO "/main/VERSION"

/* open a context following redirects; caller closes */
static Result http_open(httpcContext *ctx, const char *url)
{
    char loc[0x400];
    snprintf(loc, sizeof(loc), "%s", url);
    for (int hop = 0; hop < 6; hop++) {
        Result rc = httpcOpenContext(ctx, HTTPC_METHOD_GET, loc, 1);
        if (R_FAILED(rc)) return rc;
        httpcSetSSLOpt(ctx, SSLCOPT_DisableVerify);
        httpcSetKeepAlive(ctx, HTTPC_KEEPALIVE_ENABLED);
        httpcAddRequestHeaderField(ctx, "User-Agent", UA);
        httpcAddRequestHeaderField(ctx, "Connection", "Keep-Alive");
        rc = httpcBeginRequest(ctx);
        u32 status = 0;
        if (R_SUCCEEDED(rc)) rc = get_status(ctx, &status);
        logline("http_open hop=%d begin/status rc=%08lX http=%lu", hop, (unsigned long)rc, (unsigned long)status);
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
    Result rc = httpcOpenContext(&ctx, HTTPC_METHOD_GET, VERSION_URL, 1);
    logline("upd: open %08lX", (unsigned long)rc);
    if (R_FAILED(rc)) return false;
    httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
    httpcSetKeepAlive(&ctx, HTTPC_KEEPALIVE_ENABLED);
    httpcAddRequestHeaderField(&ctx, "User-Agent", UA);
    httpcAddRequestHeaderField(&ctx, "Connection", "Keep-Alive");
    rc = httpcBeginRequest(&ctx);
    logline("upd: begin %08lX", (unsigned long)rc);
    u32 status = 0;
    if (R_SUCCEEDED(rc)) rc = get_status(&ctx, &status);
    logline("upd: status rc=%08lX http=%lu", (unsigned long)rc, (unsigned long)status);
    if (R_FAILED(rc) || status != 200) { httpcCloseContext(&ctx); return false; }

    char body[64];
    u32 total = 0;
    while (total < sizeof(body) - 1) {
        u32 got = 0;
        Result dr = httpcDownloadData(&ctx, (u8 *)body + total, sizeof(body) - 1 - total, &got);
        total += got;
        if (dr == 0) break;
        if (dr != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) break;
    }
    httpcCloseContext(&ctx);
    body[total] = 0;
    logline("upd: body '%.20s' (%lu)", body, (unsigned long)total);
    /* trim whitespace/newlines */
    char *p = body;
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    size_t l = strlen(p);
    while (l && (p[l-1] == '\n' || p[l-1] == '\r' || p[l-1] == ' ' || p[l-1] == '\t')) l--;
    if (!l || l >= outsz) return false;
    memcpy(out, p, l); out[l] = 0;
    return true;
}

static bool install_cia(void)
{
    httpcContext ctx;
    Result rc = http_open(&ctx, CIA_URL);
    logline("update: open %08lX", (unsigned long)rc);
    if (R_FAILED(rc)) { printf(" download connect failed (see log)\n"); return false; }
    u32 total = 0;
    httpcGetDownloadSizeState(&ctx, NULL, &total);
    logline("update: size %lu", (unsigned long)total);

    rc = amInit();
    if (R_FAILED(rc)) { logline("update: amInit %08lX", (unsigned long)rc); httpcCloseContext(&ctx); return false; }
    Handle cia;
    rc = AM_StartCiaInstall(MEDIATYPE_SD, &cia);
    logline("update: StartCiaInstall %08lX", (unsigned long)rc);
    if (R_FAILED(rc)) { amExit(); httpcCloseContext(&ctx); return false; }

    static u8 buf[0x20000];
    u64 off = 0;
    bool ok = true;
    while (ok) {
        u32 got = 0;
        rc = httpcDownloadData(&ctx, buf, sizeof(buf), &got);
        if (got) {
            u32 written = 0;
            Result wr = FSFILE_Write(cia, &written, off, buf, got, 0);
            if (R_FAILED(wr) || written != got) {
                logline("update: write %08lX", (unsigned long)wr);
                ok = false;
                break;
            }
            off += got;
            printf("\r %lu / %lu KB   ", (unsigned long)(off / 1024), (unsigned long)(total / 1024));
        }
        if (rc == 0) break;
        if (rc != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) {
            logline("update: download %08lX", (unsigned long)rc);
            ok = false;
            break;
        }
    }
    printf("\n");
    httpcCloseContext(&ctx);
    logline("update: got %lu / %lu bytes", (unsigned long)off, (unsigned long)total);
    if (ok && total && off != total) ok = false;
    if (ok) {
        rc = AM_FinishCiaInstall(cia);
        logline("update: FinishCiaInstall %08lX", (unsigned long)rc);
        ok = R_SUCCEEDED(rc);
    } else {
        AM_CancelCIAInstall(cia);
    }
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
        ui_notice("Update FAILED - step codes are in\nsd:/IESM/log.txt. Install via FBI\nif this persists.", false);
    }
}
