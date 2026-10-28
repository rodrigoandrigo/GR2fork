#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <orbis/ImeDialog.h>
#include <orbis/UserService.h>

extern int sceKernelUsleep(unsigned int);

int main(void) {
    sceUserServiceInitialize(NULL);
    int32_t user = -1;
    if (sceUserServiceGetInitialUser(&user) < 0)
        return 1;
    FILE* log = fopen("/data/ime-dialog.txt", "w");
    if (!log)
        return 2;
    const uint16_t title[] = {'I', 'M', 'E', 0};
    for (unsigned pass = 0; pass < 3; ++pass) {
        struct {
            uint32_t before;
            uint16_t text[17];
            uint16_t after;
        } buffer = {0x12345678, {'S', 'e', 'e', 'd', 0}, 0x5678};
        OrbisImeDialogSetting param = {0};
        param.userId = user;
        param.type = ORBIS_TYPE_DEFAULT;
        param.maxTextLength = 16;
        param.inputTextBuffer = (wchar_t*)buffer.text;
        param.title = (const wchar_t*)title;
        int status = sceImeDialogInit(&param, NULL);
        fprintf(log, "pass=%u init=%d\n", pass, status);
        fflush(log);
        if (status < 0)
            break;
        unsigned tick;
        for (tick = 0; tick < 30000; ++tick) {
            if (sceImeDialogGetStatus() == ORBIS_DIALOG_STATUS_STOPPED)
                break;
            sceKernelUsleep(10000);
        }
        if (tick == 30000) {
            sceImeDialogAbort();
            fprintf(log, "timeout\n");
        }
        OrbisDialogResult result = {0};
        status = sceImeDialogGetResult(&result);
        fprintf(log, "result=%d end=%u guards=%08x/%04x text=", status,
                result.endstatus, buffer.before, buffer.after);
        for (unsigned i = 0; i < 17; ++i)
            fprintf(log, "%04x,", buffer.text[i]);
        fprintf(log, " term=%d\n", sceImeDialogTerm());
        fflush(log);
        sceKernelUsleep(500000);
    }
    fclose(log);
    return 0;
}
