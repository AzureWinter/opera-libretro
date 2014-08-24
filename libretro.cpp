#ifndef _MSC_VER
#include <stdbool.h>
#include <sched.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fstream>
#include <sstream>
#include <string.h>

#ifdef _MSC_VER
#define snprintf _snprintf
#pragma pack(1)
#endif

#include "libretro.h"

#include "freedocore.h"
#include "frame.h"

#define TEMP_BUFFER_SIZE 512
#define ROM1_SIZE 1 * 1024 * 1024
#define ROM2_SIZE 933636 //was 1 * 1024 * 1024,
#define NVRAM_SIZE 32 * 1024

#define INPUTBUTTONL     (1<<4)
#define INPUTBUTTONR     (1<<5)
#define INPUTBUTTONX     (1<<6)
#define INPUTBUTTONP     (1<<7)
#define INPUTBUTTONC     (1<<8)
#define INPUTBUTTONB     (1<<9)
#define INPUTBUTTONA     (1<<10)
#define INPUTBUTTONLEFT  (1<<11)
#define INPUTBUTTONRIGHT (1<<12)
#define INPUTBUTTONUP    (1<<13)
#define INPUTBUTTONDOWN  (1<<14)

typedef struct{
	int buttons; // buttons bitfield
}inputState;

inputState internal_input_state[6];

static char biosPath[PATH_MAX];
static void *nvramCopy;
static VDLFrame *frame;

FILE *fcdrom;
static int currentSector;
static bool isSwapFrameSignaled;

static int fver1,fver2;

static uint32_t *videoBuffer;
static int videoWidth, videoHeight;
//uintptr_t sampleBuffer[TEMP_BUFFER_SIZE];
static int32_t sampleBuffer[TEMP_BUFFER_SIZE];
static unsigned int sampleCurrent;

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;

void retro_set_environment(retro_environment_t cb) { environ_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

// File/ISO helper functions
unsigned char nvramhead[]=
{
   0x01,0x5a,0x5a,0x5a,0x5a,0x5a,0x02,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0x6e,0x76,0x72,0x61,0x6d,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0xff,0xff,0xff,0xff,0,0,0,1,
   0,0,0x80,0,0xff,0xff,0xff,0xfe,0,0,0,0,0,0,0,1,
   0,0,0,0,0,0,0,0x84,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0x85,0x5a,2,0xb6,0,0,0,0x98,0,0,0,0x98,
   0,0,0,0x14,0,0,0,0x14,0x7A,0xa5,0x65,0xbd,0,0,0,0x84,
   0,0,0,0x84,0,0,0x76,0x68,0,0,0,0x14
};

void fsReadBios(const char *biosFile, void *prom)
{
   FILE *bios1;
   long fsize;
   int readcount;

   bios1 = fopen(biosFile, "rb");
   fseek(bios1, 0, SEEK_END);
   fsize = ftell(bios1);
   rewind(bios1);

   readcount = fread(prom, 1, fsize, bios1);
	fclose(bios1);
}

static int fsOpenIso(const char *path)
{
   fcdrom = fopen(path, "rb");

   if(!fcdrom)
	   return 0;

   return 1;
}

static int fsCloseIso()
{
	fclose(fcdrom);
	return 1;
}

static int fsReadBlock(void *buffer, int sector)
{
   fseek(fcdrom, 2048 * sector, SEEK_SET);
   fread(buffer, 1, 2048, fcdrom);
   rewind(fcdrom);

	return 1;
}

static char *fsReadSize()
{
   char *buffer = (char *)malloc(sizeof(char) * 4);
   rewind(fcdrom);
   fseek(fcdrom, 80, SEEK_SET);
   fread(buffer, 1, 4, fcdrom);
   rewind(fcdrom);

   return buffer;
}

static unsigned int fsReadDiscSize()
{
   unsigned int size;
   char sectorZero[2048];
   unsigned int temp;
   char *ssize;
   ssize = fsReadSize();

   memcpy(&temp, ssize, 4);
   size = (temp & 0x000000FFU) << 24 | (temp & 0x0000FF00U) << 8 |
	   (temp & 0x00FF0000U) >> 8 | (temp & 0xFF000000U) >> 24;
   //printf("disc size: %d sectors\n", size);
   return size;
}

void initVideo()
{
   if(videoBuffer)
      free(videoBuffer);

   //TODO core option or always enable if CPU hit isn't too high?
   /*
   extern int HightResMode;
   HightResMode = 1;
   videoWidth = 640;
   videoHeight = 480;
   */
   videoWidth = 320;
   videoHeight = 240;
   videoBuffer = (uint32_t*)malloc(videoWidth * videoHeight * 4);
   frame = (VDLFrame*)malloc(sizeof(VDLFrame));
   memset(frame, 0, sizeof(VDLFrame));
   fver2=fver1=0;
}

void initNVRAM()
{
   nvramCopy = malloc(65536/2);
   memset(nvramCopy, 0, 65536/2);
   memcpy(nvramCopy, nvramhead, sizeof(nvramhead));
}

// Input helper functions
static int CheckDownButton(int deviceNumber,int button)
{
   if(internal_input_state[deviceNumber].buttons&button)
      return 1;
   else
      return 0;
}

static char CalculateDeviceLowByte(int deviceNumber)
{
   char returnValue = 0;

   returnValue |= 0x01 & 0; // unknown
   returnValue |= 0x02 & 0; // unknown
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONL) ? (char)0x04 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONR) ? (char)0x08 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONX) ? (char)0x10 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONP) ? (char)0x20 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONC) ? (char)0x40 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONB) ? (char)0x80 : (char)0;

   return returnValue;
}

static char CalculateDeviceHighByte(int deviceNumber)
{
   char returnValue = 0;

   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONA)     ? (char)0x01 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONLEFT)  ? (char)0x02 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONRIGHT) ? (char)0x04 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONUP)    ? (char)0x08 : (char)0;
   returnValue |= CheckDownButton(deviceNumber, INPUTBUTTONDOWN)  ? (char)0x10 : (char)0;
   returnValue |= 0x20 & 0; // unknown
   returnValue |= 0x40 & 0; // unknown
   returnValue |= 0x80; // This last bit seems to indicate power and/or connectivity.

   return returnValue;
}

// libfreedo callback
static void *fdcCallback(int procedure, void *data)
{
   switch(procedure)
   {
      case EXT_READ_ROMS:
      {
         fsReadBios(biosPath, data);
         break;
      }
      case EXT_READ_NVRAM:
         memcpy(data, nvramCopy, NVRAM_SIZE);
         break;
      case EXT_WRITE_NVRAM:
         // ?
         break;
      case EXT_SWAPFRAME:
      {
         isSwapFrameSignaled = true;
         return frame;
      }
      case EXT_PUSH_SAMPLE:
      {
         //TODO: fix all this, not right
         sampleBuffer[sampleCurrent] = *((unsigned int*)&data);
         sampleCurrent++;
         if(sampleCurrent >= TEMP_BUFFER_SIZE)
         {
            sampleCurrent = 0;
            audio_batch_cb((int16_t *)sampleBuffer, TEMP_BUFFER_SIZE);
         }
         break;
      }
      case EXT_GET_PBUSLEN:
         return (void*)16;
      case EXT_GETP_PBUSDATA:
      {
         // Set up raw data to return
         unsigned char *pbusData;
         pbusData = (unsigned char *)malloc(sizeof(unsigned char) * 16);

         pbusData[0x0] = 0x00;
         pbusData[0x1] = 0x48;
         pbusData[0x2] = CalculateDeviceLowByte(0);
         pbusData[0x3] = CalculateDeviceHighByte(0);
         pbusData[0x4] = CalculateDeviceLowByte(2);
         pbusData[0x5] = CalculateDeviceHighByte(2);
         pbusData[0x6] = CalculateDeviceLowByte(1);
         pbusData[0x7] = CalculateDeviceHighByte(1);
         pbusData[0x8] = CalculateDeviceLowByte(4);
         pbusData[0x9] = CalculateDeviceHighByte(4);
         pbusData[0xA] = CalculateDeviceLowByte(3);
         pbusData[0xB] = CalculateDeviceHighByte(3);
         pbusData[0xC] = 0x00;
         pbusData[0xD] = 0x80;
         pbusData[0xE] = CalculateDeviceLowByte(5);
         pbusData[0xF] = CalculateDeviceHighByte(5);

         return pbusData;
      }
      case EXT_KPRINT:
         break;
      case EXT_FRAMETRIGGER_MT:
      {
         isSwapFrameSignaled = true;
         _freedo_Interface(FDP_DO_FRAME_MT, frame);
         break;
      }
      case EXT_READ2048:
         fsReadBlock(data, currentSector);
         break;
      case EXT_GET_DISC_SIZE:
         return (void *)(intptr_t)fsReadDiscSize();
      case EXT_ON_SECTOR:
         currentSector = *((int*)&data);
         break;
      case EXT_ARM_SYNC:
         //printf("fdcCallback EXT_ARM_SYNC\n");
         break;

      default:
         break;
   }
   return (void*)0;
}

static void update_input()
{
   if (!input_poll_cb)
      return;

   input_poll_cb();
   
   // Can possibly support up to 6 players but is currently set for 2
   for (unsigned i = 0; i < 2; i++)
   {
      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))
         internal_input_state[i].buttons|=INPUTBUTTONUP;
      else
         internal_input_state[i].buttons&=~INPUTBUTTONUP;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))
         internal_input_state[i].buttons|=INPUTBUTTONDOWN;
      else
         internal_input_state[i].buttons&=~INPUTBUTTONDOWN;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
         internal_input_state[i].buttons|=INPUTBUTTONLEFT;
      else
         internal_input_state[i].buttons&=~INPUTBUTTONLEFT;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
         internal_input_state[i].buttons|=INPUTBUTTONRIGHT;
      else
         internal_input_state[i].buttons&=~INPUTBUTTONRIGHT;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))
         internal_input_state[i].buttons|=INPUTBUTTONA;
      else
         internal_input_state[i].buttons&=~INPUTBUTTONA;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))
         internal_input_state[i].buttons|=INPUTBUTTONB;
      else
         internal_input_state[i].buttons&=~INPUTBUTTONB;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))
         internal_input_state[i].buttons|=INPUTBUTTONC;
      else
         internal_input_state[i].buttons&=~INPUTBUTTONC;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L))
         internal_input_state[i].buttons|=INPUTBUTTONL;
      else
         internal_input_state[i].buttons&=~INPUTBUTTONL;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R))
         internal_input_state[i].buttons|=INPUTBUTTONR;
      else
         internal_input_state[i].buttons&=~INPUTBUTTONR;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))
         internal_input_state[i].buttons|=INPUTBUTTONP;
      else
         internal_input_state[i].buttons&=~INPUTBUTTONP;

      if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT))
         internal_input_state[i].buttons|=INPUTBUTTONX;
      else
         internal_input_state[i].buttons&=~INPUTBUTTONX;
   }
}

/************************************
 * libretro implementation
 ************************************/

static struct retro_system_av_info g_av_info;

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
	info->library_name = "4DO";
	info->library_version = "1.3.2.3";
	info->need_fullpath = false;
	info->valid_extensions = "iso";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->timing.fps            = 60;
   info->timing.sample_rate    = 44100;
   info->geometry.base_width   = videoWidth;
   info->geometry.base_height  = videoHeight;
   info->geometry.max_width    = videoWidth;
   info->geometry.max_height   = videoHeight;
   info->geometry.aspect_ratio = 4.0 / 3.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

size_t retro_serialize_size(void) 
{ 
   //TODO
	//return STATE_SIZE;
	return 0;
}

bool retro_serialize(void *data, size_t size)
{
   //TODO
	return false;
}

bool retro_unserialize(const void *data, size_t size)
{
   //TODO
   return false;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

bool retro_load_game(const struct retro_game_info *info)
{
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
    {
        if (log_cb)
            log_cb(RETRO_LOG_INFO, "[4DO]: XRGB8888 is not supported.\n");
        return false;
    }

    currentSector = 0;
    sampleCurrent = 0;
    memset(sampleBuffer, 0, sizeof(int32_t) * TEMP_BUFFER_SIZE);

    const char *full_path;
    full_path = info->path;
    const char *system_directory_c = NULL;

    *biosPath = '\0';
    if (fsOpenIso(full_path))
    {
       environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_directory_c);
       if (!system_directory_c)
       {
          if (log_cb)
             log_cb(RETRO_LOG_WARN, "[4DO]: no system directory defined, unable to look for panafz10.bin\n");
       }
       else
       {
          std::string system_directory(system_directory_c);
          std::string bios_file_path = system_directory + "/panafz10.bin";
          std::ifstream bios_file(bios_file_path.c_str());
          if (!bios_file.is_open())
          {
             if (log_cb)
                log_cb(RETRO_LOG_WARN, "[4DO]: panafz10.bin not found, cannot load BIOS\n");
          }
          else
             strcpy(biosPath, bios_file_path.c_str());
       }
       
       // Initialize libfreedo
       initNVRAM();
       initVideo();
       _freedo_Interface(FDP_INIT, (void*)*fdcCallback);

       return true;
    }

   return false;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
   (void)game_type;
   (void)info;
   (void)num_info;
   return false;
}

void retro_unload_game(void) 
{
   _freedo_Interface(FDP_DESTROY, (void*)0);
   fsCloseIso();
}

unsigned retro_get_region(void)
{
    return RETRO_REGION_NTSC;
}

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

void *retro_get_memory_data(unsigned id)
{
    return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
    return 0;
}

void retro_init(void)
{
   struct retro_log_callback log;
   unsigned level = 5;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

void retro_deinit(void)
{
}

void retro_reset(void)
{
}

void retro_run(void)
{
   update_input();

   _freedo_Interface(FDP_DO_EXECFRAME, frame); // FDP_DO_EXECFRAME_MT ?
   
   if(isSwapFrameSignaled)
   {
      if(fver2==fver1)
      {
         isSwapFrameSignaled = false;
         struct BitmapCrop bmpcrop;
         ScalingAlgorithm sca;
         int rw, rh;
         Get_Frame_Bitmap((VDLFrame *)frame, videoBuffer, 0, &bmpcrop, videoWidth, videoHeight, false, true, false, sca, &rw, &rh);
         fver1++;
      }
   }
   fver2=fver1;

   video_cb(videoBuffer, videoWidth, videoHeight, videoWidth << 2);
}
