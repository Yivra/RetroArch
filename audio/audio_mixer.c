/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2017 - Andre Leiradella
 * 
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "audio_mixer.h"

#include "audio_driver.h"
#include <audio/audio_resampler.h>
#include <streams/file_stream.h>
#include <formats/rwav.h>
#include <memalign.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>

/*---------------------------------------------------------------------------*/
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_CRT

#define assert( x )
#undef NULL

#ifdef __MINGW32__
#undef __forceinline
#define __forceinline __inline__ __attribute__((__always_inline__,__gnu_inline__))
#endif

#include "stb_vorbis.c"
/*---------------------------------------------------------------------------*/

#define AUDIO_MIXER_MAX_VOICES      8
#define AUDIO_MIXER_TEMP_OGG_BUFFER 8192


#define AUDIO_MIXER_TYPE_NONE 0
#define AUDIO_MIXER_TYPE_WAV  1
#define AUDIO_MIXER_TYPE_OGG  2

struct audio_mixer_sound_t
{
   unsigned type;
   
   union
   {
      struct
      {
         /* wav */
         unsigned frames;
         const float* pcm;
      } wav;
      
      struct
      {
         /* ogg */
         unsigned size;
         const void* data;
      } ogg;
   } types;
};

struct audio_mixer_voice_t
{
   unsigned type;
   bool     repeat;
   float    volume;
   audio_mixer_sound_t*  sound;
   audio_mixer_stop_cb_t stop_cb;
   
   union
   {
      struct
      {
         /* wav */
         unsigned position;
      } wav;
      
      struct
      {
         /* ogg */
         unsigned    position;
         unsigned    samples;
         stb_vorbis* stream;
         float*      buffer;
         unsigned    buf_samples;
         void*       resampler_data;
         float       ratio;
         const retro_resampler_t* resampler;
      } ogg;
   } types;
};

static unsigned s_rate;
static audio_mixer_voice_t s_voices[AUDIO_MIXER_MAX_VOICES];

static bool wav2float(const rwav_t* wav, float** pcm, size_t* samples_out)
{
   float* f;
   float sample;
   const uint8_t* u8;
   const int16_t* s16;
   size_t i;

   /* Allocate on a 16-byte boundary, and pad to a multiple of 16 bytes */
   *samples_out = wav->numsamples * 2;
   f = (float*)memalign_alloc(16, ((*samples_out + 15) & ~15) * sizeof(float));
   
   if (f == NULL)
      return false;
   
   *pcm = f;
   
   if (wav->numchannels == 1)
   {
      if (wav->bitspersample == 8)
      {
         u8 = (const uint8_t*)wav->samples;
         
         for (i = wav->numsamples; i != 0; i--)
         {
            sample = (float)*u8++ / 255.0f;
            sample = sample * 2.0f - 1.0f;
            *f++ = sample;
            *f++ = sample;
         }
      }
      else
      {
         s16 = (const int16_t*)wav->samples;
         
         for (i = wav->numsamples; i != 0; i--)
         {
            sample = (float)((int)*s16++ + 32768) / 65535.0f;
            sample = sample * 2.0f - 1.0f;
            *f++ = sample;
            *f++ = sample;
         }
      }
   }
   else if (wav->numchannels == 2)
   {
      if (wav->bitspersample == 8)
      {
         u8 = (const uint8_t*)wav->samples;
         
         for (i = wav->numsamples; i != 0; i--)
         {
            sample = (float)*u8++ / 255.0f;
            sample = sample * 2.0f - 1.0f;
            *f++ = sample;
            sample = (float)*u8++ / 255.0f;
            sample = sample * 2.0f - 1.0f;
            *f++ = sample;
         }
      }
      else
      {
         s16 = (const int16_t*)wav->samples;
         
         for (i = wav->numsamples; i != 0; i--)
         {
            sample = (float)((int)*s16++ + 32768) / 65535.0f;
            sample = sample * 2.0f - 1.0f;
            *f++ = sample;
            sample = (float)((int)*s16++ + 32768) / 65535.0f;
            sample = sample * 2.0f - 1.0f;
            *f++ = sample;
         }
      }
   }
   
   return true;
}

static bool one_shot_resample(const float* in, size_t samples_in, unsigned rate, float** out, size_t* samples_out)
{
   void* data = NULL;
   const retro_resampler_t* resampler = NULL;
   struct resampler_data info = {0};
   float ratio = (double)s_rate / (double)rate;

   if (!retro_resampler_realloc(&data, &resampler, NULL, ratio))
      return false;
   
   /* Allocate on a 16-byte boundary, and pad to a multiple of 16 bytes */
   *samples_out = samples_in * ratio;
   *out = (float*)memalign_alloc(16, ((*samples_out + 15) & ~15) * sizeof(float));
   
   if (*out == NULL)
      return false;

   info.data_in = in;
   info.data_out = *out;
   info.input_frames = samples_in / 2;
   info.ratio = ratio;

   resampler->process(data, &info);
   resampler->free(data);
   return true;
}

void audio_mixer_init(unsigned rate)
{
   unsigned i;
   
   s_rate = rate;
   
   for (i = 0; i < AUDIO_MIXER_MAX_VOICES; i++)
      s_voices[i].type = AUDIO_MIXER_TYPE_NONE;
}

void audio_mixer_done(void)
{
   unsigned i;
   
   for (i = 0; i < AUDIO_MIXER_MAX_VOICES; i++)
      s_voices[i].type = AUDIO_MIXER_TYPE_NONE;
}

audio_mixer_sound_t* audio_mixer_load_wav(const char* path)
{
   /* Raw WAV bytes */
   void* buffer;
   ssize_t size;
   /* WAV data */
   rwav_t wav;
   /* WAV samples converted to float */
   float* pcm;
   float* resampled;
   size_t samples;
   /* Result */
   audio_mixer_sound_t* sound;
   
   if (filestream_read_file(path, &buffer, &size) == 0)
      return NULL;
   
   if (rwav_load(&wav, buffer, size) != RWAV_ITERATE_DONE)
   {
      free(buffer);
      return NULL;
   }
   
   free(buffer);
   
   if (!wav2float(&wav, &pcm, &samples))
      return NULL;
   
   if (wav.samplerate != s_rate)
   {
      if (!one_shot_resample(pcm, samples, wav.samplerate, &resampled, &samples))
         return NULL;
      
      memalign_free((void*)pcm);
      pcm = resampled;
   }
   
   sound = (audio_mixer_sound_t*)malloc(sizeof(audio_mixer_sound_t));
   
   if (sound == NULL)
   {
      memalign_free((void*)pcm);
      return NULL;
   }
   
   sound->type = AUDIO_MIXER_TYPE_WAV;
   sound->types.wav.frames = samples / 2;
   sound->types.wav.pcm = pcm;
   
   rwav_free(&wav);
   return sound;
}

audio_mixer_sound_t* audio_mixer_load_ogg(const char* path)
{
   void* buffer;
   ssize_t size;
   audio_mixer_sound_t* sound;
   
   if (filestream_read_file(path, &buffer, &size) == 0)
      return NULL;
   
   sound = (audio_mixer_sound_t*)malloc(sizeof(audio_mixer_sound_t));
   
   if (sound == NULL)
   {
      free(buffer);
      return NULL;
   }
   
   sound->type = AUDIO_MIXER_TYPE_OGG;
   sound->types.ogg.size = size;
   sound->types.ogg.data = buffer;
   
   return sound;
}

void audio_mixer_destroy(audio_mixer_sound_t* sound)
{
   if (sound->type == AUDIO_MIXER_TYPE_WAV)
      memalign_free((void*)sound->types.wav.pcm);
   else if (sound->type == AUDIO_MIXER_TYPE_OGG)
      memalign_free((void*)sound->types.ogg.data);
   
   free(sound);
}

static bool audio_mixer_play_wav(audio_mixer_sound_t* sound, audio_mixer_voice_t* voice, bool repeat, float volume, audio_mixer_stop_cb_t stop_cb)
{
   voice->type = AUDIO_MIXER_TYPE_WAV;
   voice->repeat = repeat;
   voice->volume = volume;
   voice->sound = sound;
   voice->stop_cb = stop_cb;
   voice->types.wav.position = 0;
   
   return true;
}

static bool audio_mixer_play_ogg(audio_mixer_sound_t* sound, audio_mixer_voice_t* voice, bool repeat, float volume, audio_mixer_stop_cb_t stop_cb)
{
   int res;
   stb_vorbis_info info;
   float ratio;
   unsigned samples;
   
   voice->repeat = repeat;
   voice->volume = volume;
   voice->sound = sound;
   voice->stop_cb = stop_cb;

   voice->types.ogg.stream = stb_vorbis_open_memory((const unsigned char*)sound->types.ogg.data, sound->types.ogg.size, &res, NULL);

   if (voice->types.ogg.stream == NULL)
      return false;

   info = stb_vorbis_get_info(voice->types.ogg.stream);
   
   if (info.channels != 2)
   {
      /* Only stereo supported for now */
      stb_vorbis_close(voice->types.ogg.stream);
      return false;
   }
   
   if (info.sample_rate != s_rate)
   {
      voice->types.ogg.ratio = ratio = (double)s_rate / (double)info.sample_rate;
      
      if (!retro_resampler_realloc(&voice->types.ogg.resampler_data, &voice->types.ogg.resampler, NULL, ratio))
      {
         stb_vorbis_close(voice->types.ogg.stream);
         return false;
      }
   }

   samples = voice->types.ogg.buf_samples = (unsigned)(AUDIO_MIXER_TEMP_OGG_BUFFER * ratio);
   voice->types.ogg.buffer = (float*)memalign_alloc(16, ((samples + 15) & ~15) * sizeof(float));

   if (voice->types.ogg.buffer == NULL)
   {
      voice->types.ogg.resampler->free(voice->types.ogg.resampler_data);
      stb_vorbis_close(voice->types.ogg.stream);
      return false;
   }

   voice->type = AUDIO_MIXER_TYPE_OGG;
   voice->types.ogg.position = voice->types.ogg.samples = 0;
   return true;
}

audio_mixer_voice_t* audio_mixer_play(audio_mixer_sound_t* sound, bool repeat, float volume, audio_mixer_stop_cb_t stop_cb)
{
   unsigned i;
   audio_mixer_voice_t* voice;
   bool res = false;
   
   for (i = 0, voice = s_voices; i < AUDIO_MIXER_MAX_VOICES; i++, voice++)
   {
      if (voice->type == AUDIO_MIXER_TYPE_NONE)
      {
         if (sound->type == AUDIO_MIXER_TYPE_WAV)
            res = audio_mixer_play_wav(sound, voice, repeat, volume, stop_cb);
         else if (sound->type == AUDIO_MIXER_TYPE_OGG)
            res = audio_mixer_play_ogg(sound, voice, repeat, volume, stop_cb);
         
         break;
      }
   }
   
   return res ? voice : NULL;
}

void audio_mixer_stop(audio_mixer_voice_t* voice)
{
   voice->stop_cb(voice, AUDIO_MIXER_SOUND_STOPPED);
   audio_mixer_destroy(voice->sound);
}

static void mix_wav(float* buffer, size_t num_frames, audio_mixer_voice_t* voice)
{
   unsigned buf_free = num_frames * 2;
   const audio_mixer_sound_t* sound = voice->sound;
   unsigned pcm_available = sound->types.wav.frames * 2 - voice->types.wav.position;
   const float* pcm = sound->types.wav.pcm + voice->types.wav.position;
   float volume = voice->volume;
   int i;
   
again:
   if (pcm_available < buf_free)
   {
      for (i = pcm_available; i != 0; i--)
         *buffer++ += *pcm++ * volume;

      if (voice->repeat)
      {
         if (voice->stop_cb)
            voice->stop_cb(voice, AUDIO_MIXER_SOUND_REPEATED);

         buf_free -= pcm_available;
         pcm_available = sound->types.wav.frames * 2;
         pcm = sound->types.wav.pcm;
         voice->types.wav.position = 0;
         goto again;
      }

      if (voice->stop_cb)
         voice->stop_cb(voice, AUDIO_MIXER_SOUND_FINISHED);

      voice->type = AUDIO_MIXER_TYPE_NONE;
   }
   else
   {
      for (i = buf_free; i != 0; i--)
         *buffer++ += *pcm++ * volume;

      voice->types.wav.position += buf_free;
   }
}

static void mix_ogg(float* buffer, size_t num_frames, audio_mixer_voice_t* voice)
{
   unsigned buf_free = num_frames * 2;
   const audio_mixer_sound_t* sound = voice->sound;
   float temp_buffer[AUDIO_MIXER_TEMP_OGG_BUFFER];
   unsigned temp_samples;
   float volume = voice->volume;
   struct resampler_data info = {0};
   float* pcm;
   int i;
   
   if (voice->types.ogg.position == voice->types.ogg.samples)
   {
   again:
      temp_samples = stb_vorbis_get_samples_float_interleaved(voice->types.ogg.stream, 2, temp_buffer, AUDIO_MIXER_TEMP_OGG_BUFFER) * 2;

      if (temp_samples == 0)
      {
         if (voice->repeat)
         {
            if (voice->stop_cb)
               voice->stop_cb(voice, AUDIO_MIXER_SOUND_REPEATED);

            stb_vorbis_seek_start(voice->types.ogg.stream);
            goto again;
         }
         else
         {
            if (voice->stop_cb)
               voice->stop_cb(voice, AUDIO_MIXER_SOUND_FINISHED);

            voice->types.ogg.resampler->free(voice->types.ogg.resampler_data);
            stb_vorbis_close(voice->types.ogg.stream);
            voice->type = AUDIO_MIXER_TYPE_NONE;
            return;
         }
      }

      info.data_in = temp_buffer;
      info.data_out = voice->types.ogg.buffer;
      info.input_frames = temp_samples / 2;
      info.ratio = voice->types.ogg.ratio;

      voice->types.ogg.resampler->process(voice->types.ogg.resampler_data, &info);
      voice->types.ogg.position = 0;
      voice->types.ogg.samples = voice->types.ogg.buf_samples;
   }

   pcm = voice->types.ogg.buffer + voice->types.ogg.position;

   if (voice->types.ogg.samples < buf_free)
   {
      for (i = voice->types.ogg.samples; i != 0; i--)
         *buffer++ += *pcm++ * volume;

      buf_free -= voice->types.ogg.samples;
      goto again;
   }
   else
   {
      for ( int i = buf_free; i != 0; --i )
         *buffer++ += *pcm++ * volume;

      voice->types.ogg.position += buf_free;
      voice->types.ogg.samples  -= buf_free;
   }
}

void audio_mixer_mix(float* buffer, size_t num_frames)
{
   unsigned i;
   audio_mixer_voice_t* voice;
   size_t j;
   float* sample;
   
   for (i = 0, voice = s_voices; i < AUDIO_MIXER_MAX_VOICES; i++, voice++)
   {
      if (voice->type == AUDIO_MIXER_TYPE_WAV)
         mix_wav(buffer, num_frames, voice);
      else if (voice->type == AUDIO_MIXER_TYPE_OGG)
         mix_ogg(buffer, num_frames, voice);
   }
   
   for (j = 0, sample = buffer; j < num_frames; j++, sample++)
   {
      if (*sample < -1.0f)
         *sample = -1.0f;
      else if (*sample > 1.0f)
         *sample = 1.0f;
   }
}
