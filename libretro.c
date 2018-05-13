#ifndef _MSC_VER
#include <sched.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <boolean.h>

#include <libretro.h>
#include <retro_inline.h>
#include <streams/file_stream.h>
#include <file/file_path.h>
#include <retro_miscellaneous.h>

#include "nvram.h"
#include "retro_callbacks.h"
#include "retro_cdimage.h"

#include "libfreedo/freedo_3do.h"
#include "libfreedo/freedo_arm.h"
#include "libfreedo/freedo_cdrom.h"
#include "libfreedo/freedo_core.h"
#include "libfreedo/freedo_frame.h"
#include "libfreedo/freedo_madam.h"
#include "libfreedo/freedo_quarz.h"
#include "libfreedo/freedo_vdlp.h"
#include "libfreedo/hack_flags.h"

#define CDIMAGE_SECTOR_SIZE 2048
#define SAMPLE_BUFFER_SIZE 512
#define ROM1_SIZE 1 * 1024 * 1024
#define ROM2_SIZE 933636 /* was 1 * 1024 * 1024, */
#define ROM_FILENAME_PANASONIC_FZ10 "panafz10.bin"

static vdlp_frame_t *FRAME = NULL;

static cdimage_t  CDIMAGE;
static uint32_t   CDIMAGE_SECTOR;
static uint32_t  *VIDEO_BUFFER = NULL;
static uint32_t   VIDEO_WIDTH;
static uint32_t   VIDEO_HEIGHT;
static uint32_t   SAMPLE_IDX;
static int32_t    SAMPLE_BUFFER[SAMPLE_BUFFER_SIZE];

static bool x_button_also_p;
static int  controller_count;

void
retro_set_environment(retro_environment_t cb_)
{
  struct retro_vfs_interface_info vfs_iface_info;
  static const struct retro_variable vars[] =
    {
      { "4do_cpu_overclock",        "CPU overclock; "
        "1.0x (12.50Mhz)|"
        "1.1x (13.75Mhz)|"
        "1.2x (15.00Mhz)|"
        "1.5x (18.75Mhz)|"
        "1.6x (20.00Mhz)|"
        "1.8x (22.50Mhz)|"
        "2.0x (25.00Mhz)" },
      { "4do_high_resolution",      "High Resolution; disabled|enabled" },
      { "4do_nvram_storage",        "NVRAM Storage; per game|shared" },
      { "4do_x_button_also_p",      "Button X also acts as P; disabled|enabled" },
      { "4do_controller_count",     "Controller Count; 1|2|3|4|5|6|7|8|0" },
      { "4do_hack_timing_1",        "Timing Hack 1 (Crash 'n Burn); disabled|enabled" },
      { "4do_hack_timing_3",        "Timing Hack 3 (Dinopark Tycoon); disabled|enabled" },
      { "4do_hack_timing_5",        "Timing Hack 5 (Microcosm); disabled|enabled" },
      { "4do_hack_timing_6",        "Timing Hack 6 (Alone in the Dark); disabled|enabled" },
      { "4do_hack_graphics_step_y", "Graphics Step Y Hack (Samurai Shodown); disabled|enabled" },
      { NULL, NULL },
    };

  retro_set_environment_cb(cb_);

  retro_environment_cb(RETRO_ENVIRONMENT_SET_VARIABLES,(void*)vars);

  vfs_iface_info.required_interface_version = 1;
  vfs_iface_info.iface                      = NULL;
  if(retro_environment_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE,&vfs_iface_info))
    filestream_vfs_init(&vfs_iface_info);
}

void
retro_set_video_refresh(retro_video_refresh_t cb_)
{
  retro_set_video_refresh_cb(cb_);
}

void
retro_set_audio_sample(retro_audio_sample_t cb_)
{

}

void
retro_set_audio_sample_batch(retro_audio_sample_batch_t cb_)
{
  retro_set_audio_sample_batch_cb(cb_);
}

void
retro_set_input_poll(retro_input_poll_t cb_)
{
  retro_set_input_poll_cb(cb_);
}

void
retro_set_input_state(retro_input_state_t cb_)
{
  retro_set_input_state_cb(cb_);
}

static
void
video_init(void)
{
  if(VIDEO_BUFFER == NULL)
    VIDEO_BUFFER = (uint32_t*)malloc(640 * 480 * sizeof(uint32_t));

  if(FRAME == NULL)
    FRAME = (vdlp_frame_t*)malloc(sizeof(vdlp_frame_t));

  memset(FRAME,0,sizeof(vdlp_frame_t));
  memset(VIDEO_BUFFER,0,(640 * 480 * sizeof(uint32_t)));
}

static
void
video_destroy(void)
{
  if(VIDEO_BUFFER != NULL)
    free(VIDEO_BUFFER);
  VIDEO_BUFFER = NULL;

  if(FRAME != NULL)
    free(FRAME);
  FRAME = NULL;
}

static
INLINE
void
audio_reset_sample_buffer(void)
{
  SAMPLE_IDX = 0;
  memset(SAMPLE_BUFFER,0,(sizeof(int32_t) * SAMPLE_BUFFER_SIZE));
}

static
INLINE
void
audio_push_sample(const int32_t sample_)
{
  SAMPLE_BUFFER[SAMPLE_IDX++] = sample_;
  if(SAMPLE_IDX >= SAMPLE_BUFFER_SIZE)
    {
      SAMPLE_IDX = 0;
      retro_audio_sample_batch_cb((int16_t*)SAMPLE_BUFFER,SAMPLE_BUFFER_SIZE);
    }
}

static
INLINE
uint32_t
cdimage_get_size(void)
{
  return retro_cdimage_get_number_of_logical_blocks(&CDIMAGE);
}

static
INLINE
void
cdimage_set_sector(const uint32_t sector_)
{
  CDIMAGE_SECTOR = sector_;
}

static
INLINE
void
cdimage_read_sector(void *buf_)
{
  retro_cdimage_read(&CDIMAGE,CDIMAGE_SECTOR,buf_,CDIMAGE_SECTOR_SIZE);
}

static
void*
libfreedo_callback(int   cmd_,
                   void *data_)
{
  switch(cmd_)
    {
    case EXT_SWAPFRAME:
      freedo_frame_get_bitmap_xrgb_8888(FRAME,VIDEO_BUFFER,VIDEO_WIDTH,VIDEO_HEIGHT);
      return FRAME;
    case EXT_PUSH_SAMPLE:
      /* TODO: fix all this, not right */
      audio_push_sample((intptr_t)data_);
      break;
    default:
      break;
    }

  return NULL;
}

/* See Madam.c for details on bitfields being set below */
static
INLINE
uint8_t
retro_poll_joypad(const int port_,
                  const int id_)
{
  return retro_input_state_cb(port_,RETRO_DEVICE_JOYPAD,0,id_);
}

static
INLINE
void
retro_poll_input(const int port_,
                 uint8_t   buttons_[2])
{
  buttons_[0] =
    ((retro_poll_joypad(port_,RETRO_DEVICE_ID_JOYPAD_L)      << MADAM_PBUS_BYTE0_SHIFT_L)     |
     (retro_poll_joypad(port_,RETRO_DEVICE_ID_JOYPAD_R)      << MADAM_PBUS_BYTE0_SHIFT_R)     |
     (retro_poll_joypad(port_,RETRO_DEVICE_ID_JOYPAD_SELECT) << MADAM_PBUS_BYTE0_SHIFT_X)     |
     (retro_poll_joypad(port_,RETRO_DEVICE_ID_JOYPAD_START)  << MADAM_PBUS_BYTE0_SHIFT_P)     |
     ((x_button_also_p &&
       retro_poll_joypad(port_,RETRO_DEVICE_ID_JOYPAD_X))    << MADAM_PBUS_BYTE0_SHIFT_P)     |
     (retro_poll_joypad(port_,RETRO_DEVICE_ID_JOYPAD_A)      << MADAM_PBUS_BYTE0_SHIFT_C)     |
     (retro_poll_joypad(port_,RETRO_DEVICE_ID_JOYPAD_B)      << MADAM_PBUS_BYTE0_SHIFT_B));
  buttons_[1] =
    ((retro_poll_joypad(port_,RETRO_DEVICE_ID_JOYPAD_Y)      << MADAM_PBUS_BYTE1_SHIFT_A)     |
     (retro_poll_joypad(port_,RETRO_DEVICE_ID_JOYPAD_LEFT)   << MADAM_PBUS_BYTE1_SHIFT_LEFT)  |
     (retro_poll_joypad(port_,RETRO_DEVICE_ID_JOYPAD_RIGHT)  << MADAM_PBUS_BYTE1_SHIFT_RIGHT) |
     (retro_poll_joypad(port_,RETRO_DEVICE_ID_JOYPAD_UP)     << MADAM_PBUS_BYTE1_SHIFT_UP)    |
     (retro_poll_joypad(port_,RETRO_DEVICE_ID_JOYPAD_DOWN)   << MADAM_PBUS_BYTE1_SHIFT_DOWN)  |
     MADAM_PBUS_BYTE1_CONNECTED_MASK);
}

static
void
update_input(void)
{
  uint8_t *buttons;

  retro_input_poll_cb();

  buttons = freedo_madam_pbus_data_reset();

  buttons[0x00] = 0x00;
  buttons[0x01] = 0x48;
  buttons[0x0C] = 0x00;
  buttons[0x0D] = 0x80;
  switch(controller_count)
    {
    case 8:
      retro_poll_input(7,&buttons[MADAM_PBUS_CONTROLLER8_OFFSET]);
    case 7:
      retro_poll_input(6,&buttons[MADAM_PBUS_CONTROLLER7_OFFSET]);
    case 6:
      retro_poll_input(5,&buttons[MADAM_PBUS_CONTROLLER6_OFFSET]);
    case 5:
      retro_poll_input(4,&buttons[MADAM_PBUS_CONTROLLER5_OFFSET]);
    case 4:
      retro_poll_input(3,&buttons[MADAM_PBUS_CONTROLLER4_OFFSET]);
    case 3:
      retro_poll_input(2,&buttons[MADAM_PBUS_CONTROLLER3_OFFSET]);
    case 2:
      retro_poll_input(1,&buttons[MADAM_PBUS_CONTROLLER2_OFFSET]);
    case 1:
      retro_poll_input(0,&buttons[MADAM_PBUS_CONTROLLER1_OFFSET]);
    }
}

#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
void
retro_get_system_info(struct retro_system_info *info_)
{
  memset(info_,0,sizeof(*info_));

  info_->library_name     = "4DO";
  info_->library_version  = "1.3.2.4-" GIT_VERSION;
  info_->need_fullpath    = true;
  info_->valid_extensions = "iso|bin|chd|cue";
}

void
retro_get_system_av_info(struct retro_system_av_info *info_)
{
  memset(info_,0,sizeof(*info_));

  info_->timing.fps            = 60;
  info_->timing.sample_rate    = 44100;
  info_->geometry.base_width   = VIDEO_WIDTH;
  info_->geometry.base_height  = VIDEO_HEIGHT;
  info_->geometry.max_width    = VIDEO_WIDTH;
  info_->geometry.max_height   = VIDEO_HEIGHT;
  info_->geometry.aspect_ratio = 4.0 / 3.0;
}

void
retro_set_controller_port_device(unsigned port_,
                                 unsigned device_)
{
  (void)port_;
  (void)device_;
}

size_t
retro_serialize_size(void)
{
  return freedo_3do_state_size();
}

bool
retro_serialize(void   *data_,
                size_t  size_)
{
  if(size_ != freedo_3do_state_size())
    return false;

  freedo_3do_state_save(data_);

  return true;
}

bool
retro_unserialize(const void *data_,
                  size_t      size_)
{
  if(size_ != freedo_3do_state_size())
    return false;

  freedo_3do_state_load(data_);

  return true;
}

void
retro_cheat_reset(void)
{

}

void
retro_cheat_set(unsigned    index_,
                bool        enabled_,
                const char *code_)
{
  (void)index_;
  (void)enabled_;
  (void)code_;
}

static
bool
option_enabled(const char *key_)
{
  int rv;
  struct retro_variable var;

  var.key   = key_;
  var.value = NULL;
  rv = retro_environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE,&var);
  if(rv && var.value)
    return (strcmp(var.value,"enabled") == 0);

  return false;
}

static
void
check_option_4do_high_resolution(void)
{
  if(option_enabled("4do_high_resolution"))
    {
      HIRESMODE    = 1;
      VIDEO_WIDTH  = 640;
      VIDEO_HEIGHT = 480;
    }
  else
    {
      HIRESMODE    = 0;
      VIDEO_WIDTH  = 320;
      VIDEO_HEIGHT = 240;
    }
}

static
void
check_option_4do_cpu_overclock(void)
{
  int rv;
  struct retro_variable var;

  var.key   = "4do_cpu_overclock";
  var.value = NULL;

  rv = retro_environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE,&var);
  if(rv && var.value)
    {
      float mul;

      mul = atof(var.value);

      freedo_quarz_cpu_set_freq_mul(mul);
    }
}

static
void
check_option_4do_x_button_also_p(void)
{
  x_button_also_p = option_enabled("4do_x_button_also_p");
}

static
void
check_option_4do_controller_count(void)
{
  int rv;
  struct retro_variable var;

  controller_count = 0;

  var.key   = "4do_controller_count";
  var.value = NULL;

  rv = retro_environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE,&var);
  if(rv && var.value)
    controller_count = atoi(var.value);

  if((controller_count < 0) || (controller_count > 8))
    controller_count = 1;
}

static
void
check_option_set_reset_bits(const char *key_,
                         int        *input_,
                         int         bitmask_)
{
  *input_ = (option_enabled(key_) ?
             (*input_ | bitmask_) :
             (*input_ & ~bitmask_));
}

static
bool
check_option_nvram_per_game(void)
{
  int rv;
  struct retro_variable var;

  var.key   = "4do_nvram_storage";
  var.value = NULL;

  rv = retro_environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE,&var);
  if(rv && var.value)
    {
      if(strcmp(var.value,"per game"))
        return false;
    }

  return true;
}

static
bool
check_option_nvram_shared(void)
{
  return !check_option_nvram_per_game();
}

static
void
check_options(void)
{
  check_option_4do_high_resolution();
  check_option_4do_cpu_overclock();
  check_option_4do_x_button_also_p();
  check_option_4do_controller_count();
  check_option_set_reset_bits("4do_hack_timing_1",&FIXMODE,FIX_BIT_TIMING_1);
  check_option_set_reset_bits("4do_hack_timing_3",&FIXMODE,FIX_BIT_TIMING_3);
  check_option_set_reset_bits("4do_hack_timing_5",&FIXMODE,FIX_BIT_TIMING_5);
  check_option_set_reset_bits("4do_hack_timing_6",&FIXMODE,FIX_BIT_TIMING_6);
  check_option_set_reset_bits("4do_hack_graphics_step_y",&FIXMODE,FIX_BIT_GRAPHICS_STEP_Y);
}

#define CONTROLLER_DESC(PORT)                                           \
  {PORT, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" }, \
  {PORT, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" }, \
  {PORT, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" }, \
  {PORT, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" }, \
  {PORT, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "A" },  \
  {PORT, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },  \
  {PORT, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "C" },  \
  {PORT, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },  \
  {PORT, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },  \
  {PORT, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "X (Stop)" }, \
  {PORT, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "P (Play/Pause)" }

static
void
retro_setup_input_descriptions(void)
{
  struct retro_input_descriptor desc[] =
    {
      CONTROLLER_DESC(0),
      CONTROLLER_DESC(1),
      CONTROLLER_DESC(2),
      CONTROLLER_DESC(3),
      CONTROLLER_DESC(4),
      CONTROLLER_DESC(5),
      CONTROLLER_DESC(6),
      CONTROLLER_DESC(7)
    };

  retro_environment_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,desc);
}


static
bool
file_exists(const char *path_)
{
  RFILE *fp;

  fp = filestream_open(path_,RETRO_VFS_FILE_ACCESS_READ,RETRO_VFS_FILE_ACCESS_HINT_NONE);

  if(fp == NULL)
    return false;

  filestream_close(fp);

  return true;
}

static
bool
file_exists_in_system_directory(const char *filename_)
{
  int rv;
  char fullpath[PATH_MAX_LENGTH];
  const char *system_path;

  system_path = NULL;
  rv = retro_environment_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY,&system_path);
  if(!rv || (system_path == NULL))
    {
      retro_log_printf_cb(RETRO_LOG_ERROR,
                          "[4DO]: no system directory defined - can't find %s\n",
                          filename_);
      return false;
    }

  fill_pathname_join(fullpath,system_path,filename_,PATH_MAX_LENGTH);

  return file_exists(fullpath);
}

static
int64_t
read_file_from_system_directory(const char *filename_,
                                uint8_t    *data_,
                                int64_t     size_)
{
  int64_t rv;
  RFILE *file;
  char fullpath[PATH_MAX_LENGTH];
  const char *system_path;

  system_path = NULL;
  rv = retro_environment_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY,&system_path);
  if(!rv || (system_path == NULL))
    {
      retro_log_printf_cb(RETRO_LOG_ERROR,
                          "[4DO]: no system directory defined - can't find %s\n",
                          filename_);
      return false;
    }

  fill_pathname_join(fullpath,system_path,filename_,PATH_MAX_LENGTH);

  file = filestream_open(fullpath,
                         RETRO_VFS_FILE_ACCESS_READ,
                         RETRO_VFS_FILE_ACCESS_HINT_NONE);
  if(file == NULL)
    return -1;

  rv = filestream_read(file,data_,size_);

  filestream_close(file);

  return rv;
}

static
int
load_rom(void)
{
  uint8_t *rom;
  int64_t  size;
  int64_t  rv;

  rom  = freedo_arm_rom1_get();
  size = freedo_arm_rom1_size();

  rv = read_file_from_system_directory(ROM_FILENAME_PANASONIC_FZ10,rom,size);
  if(rv < 0)
    return -1;

  freedo_arm_rom1_byteswap_if_necessary();

  return 0;
}

bool
retro_load_game(const struct retro_game_info *info_)
{
  int rv;
  enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;

  if(!info_)
    return false;

  retro_setup_input_descriptions();

  rv = retro_environment_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT,&fmt);
  if(!rv)
    {
      retro_log_printf_cb(RETRO_LOG_ERROR,"[4DO]: XRGB8888 is not supported.\n");
      return false;
    }

  cdimage_set_sector(0);
  audio_reset_sample_buffer();

  rv = retro_cdimage_open(info_->path,&CDIMAGE);
  if(rv == -1)
    return false;

  rv = file_exists_in_system_directory(ROM_FILENAME_PANASONIC_FZ10);
  if(!rv)
    {
      retro_log_printf_cb(RETRO_LOG_ERROR,
                          "[4DO]: Unable to find BIOS ROM - %s\n",
                          ROM_FILENAME_PANASONIC_FZ10);
      return false;
    }

  check_options();
  video_init();
  freedo_3do_init(libfreedo_callback);
  load_rom();

  /* XXX: Is this really a frontend responsibility? */
  nvram_init(freedo_arm_nvram_get());
  if(check_option_nvram_shared())
    retro_nvram_load(freedo_arm_nvram_get());

  return true;
}

bool
retro_load_game_special(unsigned                      game_type_,
                        const struct retro_game_info *info_,
                        size_t                        num_info_)
{
  (void)game_type_;
  (void)info_;
  (void)num_info_;

  return false;
}

void
retro_unload_game(void)
{
  if(check_option_nvram_shared())
    retro_nvram_save(freedo_arm_nvram_get());

  freedo_3do_destroy();

  retro_cdimage_close(&CDIMAGE);

  video_destroy();
}

unsigned
retro_get_region(void)
{
  return RETRO_REGION_NTSC;
}

unsigned
retro_api_version(void)
{
  return RETRO_API_VERSION;
}

void*
retro_get_memory_data(unsigned id_)
{
  switch(id_)
    {
    case RETRO_MEMORY_SAVE_RAM:
      if(check_option_nvram_shared())
        return NULL;
      return freedo_arm_nvram_get();
    case RETRO_MEMORY_SYSTEM_RAM:
      return freedo_arm_ram_get();
    case RETRO_MEMORY_VIDEO_RAM:
      return freedo_arm_vram_get();
    }

  return NULL;
}

size_t
retro_get_memory_size(unsigned id_)
{
  switch(id_)
    {
    case RETRO_MEMORY_SAVE_RAM:
      if(check_option_nvram_shared())
        return 0;
      return freedo_arm_nvram_size();
    case RETRO_MEMORY_SYSTEM_RAM:
      return freedo_arm_ram_size();
    case RETRO_MEMORY_VIDEO_RAM:
      return freedo_arm_vram_size();
    }

  return 0;
}

void
retro_init(void)
{
  unsigned level;
  uint64_t serialization_quirks;
  struct retro_log_callback log;

  level = 5;
  serialization_quirks = RETRO_SERIALIZATION_QUIRK_SINGLE_SESSION;

  if(retro_environment_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE,&log))
    retro_set_log_printf_cb(log.log);

  retro_environment_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL,&level);
  retro_environment_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS,&serialization_quirks);

  freedo_cdrom_set_callbacks(cdimage_get_size,
                             cdimage_set_sector,
                             cdimage_read_sector);
}

void
retro_deinit(void)
{

}

void
retro_reset(void)
{
  if(check_option_nvram_shared())
    retro_nvram_save(freedo_arm_nvram_get());

  freedo_3do_destroy();

  check_options();
  video_init();
  cdimage_set_sector(0);
  audio_reset_sample_buffer();
  freedo_3do_init(libfreedo_callback);
  load_rom();

  nvram_init(freedo_arm_nvram_get());
  if(check_option_nvram_shared())
    retro_nvram_load(freedo_arm_nvram_get());
}

void
retro_run(void)
{
  bool updated = false;
  if(retro_environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE,&updated) && updated)
    check_options();

  update_input();

  freedo_3do_process_frame(FRAME);

  retro_video_refresh_cb(VIDEO_BUFFER,VIDEO_WIDTH,VIDEO_HEIGHT,VIDEO_WIDTH << 2);
}
