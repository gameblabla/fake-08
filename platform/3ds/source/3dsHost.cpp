#include <3ds.h>

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
using namespace std;

#include "../../../source/host.h"
#include "../../../source/hostVmShared.h"
#include "../../../source/nibblehelpers.h"
#include "../../../source/logger.h"

// sdl
#include <SDL/SDL.h>
//#include <SDL/SDL_gfxBlitFunc.h>

#define SCREEN_WIDTH 400
#define SCREEN_HEIGHT 240

#define SCREEN_2_WIDTH 320
#define SCREEN_2_HEIGHT 240

#define SAMPLERATE 22050
#define SAMPLESPERBUF (SAMPLERATE / 30)
#define NUM_BUFFERS 2

const int __3ds_TopScreenWidth = SCREEN_WIDTH;
const int __3ds_TopScreenHeight = SCREEN_HEIGHT;

const int __3ds_BottomScreenWidth = SCREEN_2_WIDTH;
const int __3ds_BottomScreenHeight = SCREEN_2_HEIGHT;


const int PicoScreenWidth = 128;
const int PicoScreenHeight = 128;

const int PicoFbLength = 128 * 64;


StretchOption stretch = StretchAndOverflow;
u64 last_time;
u64 now_time;
u64 frame_time;
double targetFrameTimeMs;

u32 currKDown;
u32 currKHeld;

Color* _paletteColors;
Bgr24Col _bgrColors[16];
Audio* _audio;

//SDL_Window* window;
SDL_Event event;
SDL_Surface *window;
SDL_Surface *texture;
//SDL_Renderer *renderer;
//SDL_Texture *texture = NULL;
SDL_bool done = SDL_FALSE;
SDL_AudioSpec want, have;
//SDL_AudioDeviceID dev;
void *pixels;
uint8_t *base;
int pitch;

SDL_Rect SrcR;
SDL_Rect DestR;


uint8_t ConvertInputToP8(u32 input){
	uint8_t result = 0;
	if (input & KEY_LEFT){
		result |= P8_KEY_LEFT;
	}

	if (input & KEY_RIGHT){
		result |= P8_KEY_RIGHT;
	}

	if (input & KEY_UP){
		result |= P8_KEY_UP;
	}

	if (input & KEY_DOWN){
		result |= P8_KEY_DOWN;
	}

	if (input & KEY_B){
		result |= P8_KEY_O;
	}

	if (input & KEY_A){
		result |= P8_KEY_X;
	}

	if (input & KEY_START){
		result |= P8_KEY_PAUSE;
	}

	if (input & KEY_SELECT){
		result |= P8_KEY_7;
	}

	return result;
}

void postFlipFunction(){
    // We're done rendering, so we end the frame here.

    //this function doesn't stretch
    SDL_BlitSurface(texture, NULL, window, &DestR);

    //this function is supposed to stretch, but its doing nothing
    //int res = SDL_gfxBlitRGBA(texture, NULL, window, NULL);

    //if (res != 1){
    //    printf("blit failed : %d\n", res);
    //}

    SDL_UpdateRect(window, 0, 0, 0, 0);
}


void init_fill_buffer(void *audioBuffer,size_t offset, size_t size) {

	u32 *dest = (u32*)audioBuffer;

	for (size_t i=0; i<size; i++) {
		dest[i] = 0;
	}

	DSP_FlushDataCache(audioBuffer,size);

}

bool audioInitialized = false;
u32 *audioBuffer;
u32 audioBufferSize;
ndspWaveBuf waveBuf[2];
bool fillBlock = false;
u32 currPos;


void audioCleanup(){
    audioInitialized = false;

    ndspExit();

    if(audioBuffer != nullptr) {
        linearFree(audioBuffer);
        audioBuffer = nullptr;
    }
}

void audioSetup(){
    if(R_FAILED(ndspInit())) {
        return;
    }

	//audio setup
	audioBufferSize = SAMPLESPERBUF * NUM_BUFFERS * sizeof(u32);
	audioBuffer = (u32*)linearAlloc(audioBufferSize);
	if(audioBuffer == nullptr) {
        audioCleanup();
        return;
    }
	

	ndspSetOutputMode(NDSP_OUTPUT_STEREO);

	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetRate(0, SAMPLERATE);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

	float mix[12];
	memset(mix, 0, sizeof(mix));
	mix[0] = 1.0;
	mix[1] = 1.0;
	ndspChnSetMix(0, mix);

	memset(waveBuf,0,sizeof(waveBuf));
	waveBuf[0].data_vaddr = &audioBuffer[0];
	waveBuf[0].nsamples = SAMPLESPERBUF;
	waveBuf[1].data_vaddr = &audioBuffer[SAMPLESPERBUF];
	waveBuf[1].nsamples = SAMPLESPERBUF;


	size_t stream_offset = 0;

	//not sure if this is necessary? if it is, memset might be better?
	init_fill_buffer(audioBuffer,stream_offset, SAMPLESPERBUF * 2);

	stream_offset += SAMPLESPERBUF;

	ndspChnWaveBufAdd(0, &waveBuf[0]);
	ndspChnWaveBufAdd(0, &waveBuf[1]);

	audioInitialized = true;
}



Host::Host() { }


void Host::oneTimeSetup(Color* paletteColors, Audio* audio){
    osSetSpeedupEnable(true);

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        fprintf(stderr, "SDL could not initialize\n");
        return;
    }

    SDL_WM_SetCaption("FAKE-08", NULL);

    int flags = SDL_HWSURFACE;

    window = SDL_SetVideoMode(SCREEN_WIDTH, SCREEN_HEIGHT, 0, flags);

    SDL_Surface* temp = SDL_CreateRGBSurface(flags, PicoScreenWidth, PicoScreenHeight, 0, 0, 0, 0, 0);

    texture = SDL_DisplayFormat(temp);

    SDL_FreeSurface(temp);

    _audio = audio;
    //audioSetup();
    
    last_time = 0;
    now_time = 0;
    frame_time = 0;
    targetFrameTimeMs = 0;

    _paletteColors = paletteColors;

    SrcR.x = 0;
    SrcR.y = 0;
    SrcR.w = PicoScreenWidth;
    SrcR.h = PicoScreenHeight;

    DestR.x = 0;
    DestR.y = 0;
    DestR.w = SCREEN_WIDTH;
    DestR.h = SCREEN_HEIGHT;
}

void Host::oneTimeCleanup(){
    audioCleanup();

    //SDL_DestroyRenderer(renderer);
    //SDL_DestroyWindow(window);

    SDL_FreeSurface(texture);

    SDL_Quit();
}

void Host::setTargetFps(int targetFps){
    targetFrameTimeMs = 1000.0 / (double)targetFps;
}

void Host::changeStretch(){
    if (currKDown & KEY_R) {
        if (stretch == PixelPerfect) {
            stretch = StretchToFit;
        }
        else if (stretch == StretchToFit) {
            stretch = StretchAndOverflow;
        }
        else if (stretch == StretchAndOverflow) {
            stretch = PixelPerfect;
        }
    }
}

InputState_t Host::scanInput(){
    hidScanInput();

    currKDown = hidKeysDown();
    currKHeld = hidKeysHeld();

    return InputState_t {
        ConvertInputToP8(currKDown),
        ConvertInputToP8(currKHeld)
    };
}

bool Host::shouldQuit() {
    bool lpressed = currKHeld & KEY_L;
	bool rpressed = currKDown & KEY_R;

	return lpressed && rpressed;
}


void Host::waitForTargetFps(){
    now_time = SDL_GetTicks();
    frame_time = now_time - last_time;
	last_time = now_time;


	//sleep for remainder of time
	if (frame_time < targetFrameTimeMs) {
		uint32_t msToSleep = targetFrameTimeMs - frame_time;
        
        SDL_Delay(msToSleep);

		last_time += msToSleep;
	}
}


void Host::drawFrame(uint8_t* picoFb, uint8_t* screenPaletteMap){
    pixels = texture->pixels;

    for (int y = 0; y < PicoScreenHeight; y ++){
        for (int x = 0; x < PicoScreenWidth; x ++){
            uint8_t c = getPixelNibble(x, y, picoFb);
            Color col = _paletteColors[screenPaletteMap[c]];

            base = ((Uint8 *)pixels) + (4 * ( y * PicoScreenHeight + x));
            base[0] = col.Alpha;
            base[1] = col.Blue;
            base[2] = col.Green;
            base[3] = col.Red;
        }
    }
    

    postFlipFunction();
}

bool Host::shouldFillAudioBuff(){
    return waveBuf[fillBlock].status == NDSP_WBUF_DONE;
}

void* Host::getAudioBufferPointer(){
    return waveBuf[fillBlock].data_pcm16;
}

size_t Host::getAudioBufferSize(){
    return waveBuf[fillBlock].nsamples;
}

void Host::playFilledAudioBuffer(){
    DSP_FlushDataCache(waveBuf[fillBlock].data_pcm16, waveBuf[fillBlock].nsamples);

	ndspChnWaveBufAdd(0, &waveBuf[fillBlock]);

	fillBlock = !fillBlock;
}

bool Host::shouldRunMainLoop(){
    return aptMainLoop();
}

vector<string> Host::listcarts(){
    vector<string> carts;

    //force to SD card root
    chdir("sdmc:/");

    DIR* dir = opendir("/p8carts");
    struct dirent *ent;
    std::string fullCartDir = "/p8carts/";

    if (dir) {
        /* print all the files and directories within directory */
        while ((ent = readdir (dir)) != NULL) {
            carts.push_back(fullCartDir + ent->d_name);
        }
        closedir (dir);
    }
    
    return carts;
}

const char* Host::logFilePrefix() {
    return "";
}

std::string Host::customBiosLua() {
    return "";
}
