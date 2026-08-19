#include <string.h>
#include <stdlib.h>
#include "app.h"

PrintConsole topcon, botcon;

u32 wait_key(void)
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

void ui_header(void)
{
    consoleClear();
    printf(C_TITLE " IESM - Inazuma Eleven Save Manager   " VERSION " " C_RESET "\n\n");
}

bool ui_dialog(const char *yes, const char *text, bool warn)
{
    ui_header();
    printf("%s%s" C_RESET "\n\n", warn ? C_WARN : "", text);
    printf(C_KEY " A " C_RESET "%s   " C_KEY " B " C_RESET "cancel\n", yes);
    while (aptMainLoop()) {
        u32 k = wait_key();
        if (k & KEY_A) return true;
        if (k & KEY_B) return false;
    }
    return false;
}

void ui_notice(const char *text, bool ok)
{
    printf("\n%s%s" C_RESET "\n\n" C_DIM "Press any key." C_RESET "\n",
           ok ? C_OK : C_WARN, text);
    wait_key();
}

#define LIST_ROWS 20

int ui_list(const char *title, const char *const *lines, int n, int cursor)
{
    if (n == 0) return -1;
    if (cursor < 0 || cursor >= n) cursor = 0;
    int top = 0;
    bool dirty = true;
    while (aptMainLoop()) {
        if (cursor < top) top = cursor;
        if (cursor >= top + LIST_ROWS) top = cursor - LIST_ROWS + 1;
        if (dirty) {
            ui_header();
            printf(C_KEY " %s " C_RESET "(%d/%d)\n\n", title, cursor + 1, n);
            for (int i = top; i < top + LIST_ROWS && i < n; i++)
                printf(" %s %-46.46s " C_RESET "\n", (i == cursor) ? C_SEL : " ", lines[i]);
            printf("\x1b[28;1H" C_DIM " D-Pad move (L/R page)   A select   B back" C_RESET);
            dirty = false;
        }
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_A) return cursor;
        if (k & (KEY_B | KEY_START)) return -1;
        if (k & KEY_UP)    { cursor = (cursor + n - 1) % n; dirty = true; }
        if (k & KEY_DOWN)  { cursor = (cursor + 1) % n; dirty = true; }
        if (k & KEY_LEFT)  { cursor -= LIST_ROWS; if (cursor < 0) cursor = 0; dirty = true; }
        if (k & KEY_RIGHT) { cursor += LIST_ROWS; if (cursor >= n) cursor = n - 1; dirty = true; }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    return -1;
}

bool ui_text(const char *hint, const char *initial, char *out, size_t outsz)
{
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_QWERTY, 2, outsz - 1);
    swkbdSetHintText(&kb, hint);
    if (initial) swkbdSetInitialText(&kb, initial);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (swkbdInputText(&kb, out, outsz) != SWKBD_BUTTON_RIGHT) return false;
    for (char *p = out; *p; p++)
        if (*p == '/' || *p == '\\') *p = '_';
    return out[0] != 0;
}

bool ui_number(const char *hint, int initial, int min, int max, int *out)
{
    char buf[16], init[16];
    snprintf(init, sizeof(init), "%d", initial);
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_NUMPAD, 2, sizeof(buf) - 1);
    swkbdSetHintText(&kb, hint);
    swkbdSetInitialText(&kb, init);
    if (swkbdInputText(&kb, buf, sizeof(buf)) != SWKBD_BUTTON_RIGHT) return false;
    long v = strtol(buf, NULL, 10);
    if (v < min) v = min;
    if (v > max) v = max;
    *out = (int)v;
    return true;
}
