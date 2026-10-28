#include <stdint.h>
#include <stdio.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>

extern int sceKernelUsleep(unsigned int);

int main(int argc, char** argv) {
    sceUserServiceInitialize(NULL);
    int32_t user = -1;
    if (sceUserServiceGetInitialUser(&user) < 0 || scePadInit() < 0)
        return 1;
    int handle = scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
    if (handle < 0)
        return 2;
    FILE* output = fopen(argc > 1 ? argv[1] : "/data/shadps4-touchpad.csv", "w");
    if (!output) {
        scePadClose(handle);
        return 3;
    }
    OrbisPadInformation info = {0};
    int result = scePadGetControllerInformation(handle, &info);
    fprintf(output, "controller_info,%d,resolution,%u,%u\n", result,
            info.touchResolutionX, info.touchResolutionY);
    fprintf(output, "sample,connected,click,l3,lx,ly,fingers,id0,x0,y0,id1,x1,y1,touch_aux\n");
    unsigned violations = 0;
    for (unsigned sample = 0; sample < 15000; ++sample) {
        OrbisPadData pad = {0};
        if (scePadReadState(handle, &pad) < 0) {
            fprintf(output, "read_error,%u\n", sample);
            ++violations;
            break;
        }
        if (pad.touch.fingers > ORBIS_PAD_MAX_TOUCH_NUM)
            ++violations;
        for (unsigned finger = 0; finger < pad.touch.fingers && finger < ORBIS_PAD_MAX_TOUCH_NUM; ++finger)
            if (result == 0 && (pad.touch.touch[finger].x >= info.touchResolutionX ||
                               pad.touch.touch[finger].y >= info.touchResolutionY))
                ++violations;
        fprintf(output, "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", sample,
                pad.connected, !!(pad.buttons & ORBIS_PAD_BUTTON_TOUCH_PAD),
                !!(pad.buttons & ORBIS_PAD_BUTTON_L3), pad.leftStick.x, pad.leftStick.y,
                pad.touch.fingers, pad.touch.touch[0].finger, pad.touch.touch[0].x,
                pad.touch.touch[0].y, pad.touch.touch[1].finger, pad.touch.touch[1].x,
                pad.touch.touch[1].y, pad.touch.pad2);
        if (sample % 125 == 0)
            fflush(output);
        sceKernelUsleep(8000);
    }
    fprintf(output, "coordinate_violations,%u\n", violations);
    fclose(output);
    scePadClose(handle);
    return violations ? 4 : 0;
}
