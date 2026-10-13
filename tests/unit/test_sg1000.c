#define main gm0_main
#include "../../gm0.c"
#undef main
#define CHECK(x) do { if(!(x)){fprintf(stderr,"Failed at line %d: %s\n",__LINE__,#x);return 1;} }while(0)
int main(void) {
    static const uint8_t rom[]={0x3e,0x84,0xd3,0x7f,0x3e,0x10,0xd3,0x7f,
        0x3e,0x90,0xd3,0x7f,0xc3,0x0c,0x00};
    CHECK(sdl_init_all()==0);
    reset_machine();load_firmware_data(rom,sizeof(rom),"synthetic.sg");
    CHECK(fwLoaded&&emuTarget==TGT_SG1000&&SCR_W==256&&SCR_H==192);
    CHECK(key_bit(SDLK_SPACE)==BTN_A&&key_bit(SDLK_LCTRL)==BTN_B);
    CHECK(key_bit(SDLK_RETURN)==BTN_MENU&&key_bit(SDLK_ASTERISK)==BTN_HOME);
    CHECK(pad_button_mask(SDL_CONTROLLER_BUTTON_START)==BTN_MENU);
    btn_press(BTN_MENU|BTN_RIGHT);run_emulated_frame();
    CHECK(sgMachine.buttons[0]==24&&sgMachine.frames==1);
    CHECK(sgAudioSamples>360&&aqP.levels>360);
    btn_release(BTN_MENU|BTN_RIGHT);btn_press(BTN_HOME);run_emulated_frame();
    CHECK(sgMachine.pause&&!sgMachine.buttons[0]);
    btn_release(BTN_HOME);run_emulated_frame();CHECK(!sgMachine.pause);
    for(int f=0;f<FILT_COUNT;f++){dispFilter=f;blit(emuRen);CHECK(tex);}
    SDL_Event event={0};event.type=SDL_KEYUP;event.key.keysym.sym=SDLK_F8;
    SDL_PushEvent(&event);CHECK(poll_events());CHECK(dispFilter==FILT_PIXEL);
    event.key.keysym.sym=SDLK_F10;SDL_PushEvent(&event);CHECK(poll_events());CHECK(dispScale==1);
    event.type=SDL_KEYDOWN;event.key.keysym.sym=SDLK_8;event.key.keysym.scancode=SDL_SCANCODE_8;event.key.keysym.mod=KMOD_SHIFT;
    SDL_PushEvent(&event);CHECK(poll_events());CHECK(sgButtons&BTN_HOME);
    event.type=SDL_KEYUP;event.key.keysym.mod=0;
    SDL_PushEvent(&event);CHECK(poll_events());CHECK(!(sgButtons&BTN_HOME));
    fw_restart();CHECK(fwLoaded&&sgMachine.frames==0&&fwLen==sizeof(rom));
    CHECK(memcmp(fwData,rom,sizeof(rom))==0);
    uint8_t firmware[]={0,0x80,0,0x20,9,0x40,0,0};
    reset_machine();load_firmware_data(firmware,sizeof(firmware),"test.bin");
    CHECK(emuTarget==TGT_META&&SCR_W==160&&SCR_H==128);
    fw_restart();CHECK(memcmp(fwData,firmware,sizeof(firmware))==0);
    reset_machine();load_firmware_data(rom,sizeof(rom),"again.SG");CHECK(emuTarget==TGT_SG1000);
    SDL_CloseAudioDevice(audioDev);SDL_DestroyTexture(tex);SDL_DestroyRenderer(emuRen);SDL_DestroyWindow(emuWin);SDL_Quit();
    return 0;
}
