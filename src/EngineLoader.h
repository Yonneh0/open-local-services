#pragma once
// The image engine stands OUTSIDE this binary.
//
// A library beside the program - stable-diffusion-cuda, -vulkan, or -cpu -
// carries the engine and its ggml whole. This server links neither: it reads
// the machine (the card answers through nvml, the vulkan loader through its
// own name), opens the one library of the kind it found, and takes the table
// of calls from there. The CUDA library is one file for all the card
// generations its build was pointed at: its loader picks the card's own
// kernels when the library is opened, so a binary and a library built once
// serve every machine of their kind - which is why the shape read at start is
// three kinds, not one per generation.
//
// warmEngine loads one library, once, and prints what the machine answered
// with. Where no library stands, every call through the table is refused with
// words, and nothing else about the server is harmed: the doors that draw
// pictures say plainly that no engine stands, and the rest of the machine
// keeps serving.
#ifndef OLS_ENGINELOADER_H
#define OLS_ENGINELOADER_H

#include <string>

#include <stable-diffusion.h>

namespace ols
{
  namespace imagegen
  {
    // The names this server asks of the engine. Every one is filled from the
    // library opened at start; a library that does not answer one of these
    // names is not opened at all - the next kind is tried, and the last word
    // says so.
    struct EngineApi
    {
      sd_ctx_t *(*new_sd_ctx)(const sd_ctx_params_t *) = nullptr;
      void (*free_sd_ctx)(sd_ctx_t *) = nullptr;
      bool (*generate_image)(sd_ctx_t *, const sd_img_gen_params_t *, sd_image_t **, int *) = nullptr;
      bool (*generate_video)(sd_ctx_t *, const sd_vid_gen_params_t *, sd_image_t **, int *,
                             sd_audio_t **, int *) = nullptr;
      void (*sd_cancel_generation)(sd_ctx_t *, enum sd_cancel_mode_t) = nullptr;
      size_t (*sd_list_devices)(char *, size_t) = nullptr;
      void (*sd_set_log_callback)(sd_log_cb_t, void *) = nullptr;
      void (*sd_set_progress_callback)(sd_progress_cb_t, void *) = nullptr;
      void (*sd_ctx_params_init)(sd_ctx_params_t *) = nullptr;
      void (*sd_sample_params_init)(sd_sample_params_t *) = nullptr;
      void (*sd_img_gen_params_init)(sd_img_gen_params_t *) = nullptr;
      void (*sd_vid_gen_params_init)(sd_vid_gen_params_t *) = nullptr;
      enum sample_method_t (*sd_get_default_sample_method)(const sd_ctx_t *) = nullptr;
      enum scheduler_t (*sd_get_default_scheduler)(const sd_ctx_t *, enum sample_method_t) = nullptr;
      void (*free_sd_audio)(sd_audio_t *) = nullptr;
      void (*free_sd_images)(sd_image_t *, int) = nullptr;
      // the engine's own word tables: a name for a model kind, a sampler or a
      // scheduler is read by the engine, not guessed at by the server.
      enum sd_type_t (*str_to_sd_type)(const char *) = nullptr;
      enum sample_method_t (*str_to_sample_method)(const char *) = nullptr;
      enum scheduler_t (*str_to_scheduler)(const char *) = nullptr;
    };

    // Load one engine library and print the machine's answer, once. A named
    // OLS_ENGINE (cpu|vulkan|cuda) stands over what the machine says; auto,
    // the default, asks the card first, then the vulkan loader, then the cpu.
    void warmEngine();

    // Whether a library stands with all its names in hand.
    bool engineStands();
    // "cuda", "vulkan", "cpu" - or empty while none stands.
    std::string engineShape();
    // The one line this server started with: shape, file, and the card's word
    // where one answered. Empty before the warm, words after - in both cases
    // only what was measured, never what was hoped.
    std::string engineWords();
    // The table. Every member is null until an engine stands.
    const EngineApi &engineApi();

    // What the card answers about itself through nvml - without opening a CUDA
    // context, whether or not an engine stands. totalBytes 0 with major -1
    // means no card answered.
    void cardFacts(long long &totalBytes, long long &freeBytes, int &computeMajor,
                   int &computeMinor);
    // The card's generation in the word the pages show ("86"), empty when no
    // card answers.
    std::string engineCardWord();
    // The machine's own memory, whole and free: the numbers the pages show
    // beside the processor's name, read from the machine itself.
    void systemMemory(long long &totalBytes, long long &freeBytes);

  } // namespace imagegen
} // namespace ols

#endif // OLS_ENGINELOADER_H
