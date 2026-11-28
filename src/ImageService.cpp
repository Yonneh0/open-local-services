#include "ImageService.h"
#include "OpenLocalServices.h"
#include "Common.h"
#include "EngineLoader.h"
#include "VerifiedRoom.h"

#include <stable-diffusion.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_IMPLEMENTATION
// stb_image's reader is carried here, beside the writer: the image engine
// ships as an import library, so its own copy of the reader is not in sight
// of this link, and the server's binary holds its own.
#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <cstddef>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "Platform.h"

namespace ols
{
  namespace imagegen
  {

    using json = nlohmann::json;

    struct ImageModelDef
    {
      std::string id;
      std::string name;
      std::string repo;
      std::string negative;
      std::string sigmas;
      std::string wtype;
      // A row that stands wholly on int8-convrot weights carries its denoiser
      // math on INT8 GEMM, which this engine's CUDA backend answers only from
      // compute 7.5 up - below that the engine places every matmul of the run
      // on the cpu. The row says it here so the card can be asked before the
      // weights are ever opened.
      bool int8_convrot = false;
      std::string prompt;
      std::string specialty;
      bool edit = false;
      bool redraw = false;
      int refs = 0;
      int steps = 0;
      float guidance = 1.0f;
      // The size the model's own card names. A run that asks for no side draws
      // this one, and the page offers it as the size the model stands at home at:
      // a model asked to draw at a size it was never trained at costs the square
      // of the ratio in compute and gives back a picture it cannot fill.
      int width = 0;
      int height = 0;
      // The sampler and scheduler the card names, written in the engine's own
      // words. Empty leaves the engine's choice for this model's family.
      std::string sampler;
      std::string scheduler;
      // Whether this model runs with a step cache. A row that names nothing runs
      // without one: the cache starts at fifteen percent of a run's steps and
      // stops at ninety, which in a few distilled steps is the whole sampling.
      bool cache = false;
      // How many CLIP text-blocks to stop after. -1 leaves the engine's own
      // default standing, which is 2 for SD 2 and SDXL and 1 for SD 1.5
      // (src/conditioning/conditioner.hpp:459). A card that names the number says
      // it here, because the default answers for the family, not for the model.
      int clip_skip = -1;
      // The time shift a flow-matching model samples on. INFINITY leaves the
      // engine's family default standing - set_flow_shift answers with
      // default_flow_shift when it is handed INFINITY
      // (src/pipeline/diffusion_engine.cpp:3187-3188) - and that default is 3.0
      // for z-image and the Qwen image flow families, 6.93 for FLUX.3, and for
      // older FLUX families a 1.0 the engine's own source marks "TODO: validate"
      // (diffusion_engine.cpp:1531). A model that ships a scheduler config
      // naming its shift says it here, because the family default is a guess
      // the engine has not validated.
      float flow_shift = INFINITY;
      int civitaiVersion = 0;
      std::vector<std::pair<std::string, std::string>> files;
      // Whether this model's work is a moving picture rather than a still one.
      // Stated by the card: MiniMax H3's card is a video model ("image-text-to-video",
      // "up to 15 seconds at 24 FPS with 32 kHz stereo audio"), and the engine says
      // the same of a context built from its files (sd_ctx_supports_video_generation).
      // A run of a video model goes through the engine's video door and leaves a clip
      // behind it, not a picture, so the gallery and the pages read this flag rather
      // than guessing from what a file is named.
      bool video = false;
      // The clip length the model's card states it runs at home at, in frames, and
      // the frame rate it runs at. The engine keeps its own grid (frame counts are
      // aligned up to the 17k+5 grid, and H3 overrides any frame rate other than its
      // 24), so a run that names no clip length draws these numbers and the page
      // offers them as the length the model stands at home at.
      int frames = 0;
      int fps = 0;
    };

    // What a model is good at is one line here, stated by the card that names
    // the model. The words a run asks with are typed on the page.
    static const std::vector<std::pair<const char *, const char *>> &kSpecialty()
    {
      static const std::vector<std::pair<const char *, const char *>> rows = {
          {"flux-3-klein", "photoreal scenes, diagrams, and crisp rendered words in a small fast package"},
          {"z-image-turbo", "cinematic photoreal shots in eight steps, with legible signage"},
          {"qwen-image-2.1", "clean illustration, comics, and long legible text, including Chinese"},
          {"qwen-image-2.1-turbo", "the same legible text and illustration in eight steps"},

          {"sdxl-turbo", "fast product, food, and cartoon shapes in one or four steps"},
          {"realvisxl-v5-lightning", "photoreal portraits and candid photos in five steps, honest skin"},
          {"pony-diffusion-v6", "anime and game illustration written in tag style"},
          {"cyberrealistic-xl", "photoreal people, portraits and full body"},
          {"animagine-xl-3.1", "anime illustration in several genres, with score tags"},
          {"lustify-sdxl", "photoreal women; guided, SFW photography, objects, animals, fantasy"},

          {"juggernaut-xl-v9", "photoreal portrait, architecture, car, food, interior, landscape"},

          // MiniMax H3's card is a video model: "MiniMax H3 is an omni-modal
          // generative system ... up to 15 seconds at 24 FPS with 32 kHz stereo
          // audio" (the words unsloth carries beside the very bytes here), and the
          // engine's own page states the same. What it makes is a moving picture
          // with its own sound, so its line says that rather than a still-life
          // strength.
          {"minimax-h3", "moving pictures with their own stereo sound, from words and from frames"},
      };
      return rows;
    }

    // how a model uses pictures, and how many it takes: an editing model reads
    // the pictures in the order they are handed to it, and the caps below are
    // the numbers the model's own card states - the Qwen Image 2.1 card says
    // "up to 10 reference images", the FLUX.3 Image Klein docs say the same,
    // and a redraw model takes one picture and
    // changes it.
    static const std::vector<std::tuple<std::string, std::string, int>> &kCapability()
    {
      static const std::vector<std::tuple<std::string, std::string, int>> rows = {
          {"flux-3-klein", "edit", 10},
          {"qwen-image-2.1", "edit", 10},
          {"qwen-image-2.1-turbo", "edit", 10},

          {"z-image-turbo", "redraw", 1},
          {"sdxl-turbo", "redraw", 1},
          {"realvisxl-v5-lightning", "redraw", 1},
          {"pony-diffusion-v6", "redraw", 1},
          {"cyberrealistic-xl", "redraw", 1},
          {"animagine-xl-3.1", "redraw", 1},
          {"lustify-sdxl", "redraw", 1},

          {"juggernaut-xl-v9", "redraw", 1},

          // MiniMax H3's fl2va checkpoint takes "text, plus zero, one or two frames"
          // (unsloth's own words beside the files), and the engine reads them in
          // order: the first as the frame the clip starts from, the second as the
          // frame it ends on. That is the ordering the reference strip already keeps
          // pictures by, so it is stated as an editing model that takes two frames.
          {"minimax-h3", "edit", 2},
      };
      return rows;
    }

    // What a model's own card and its own weight files say about running it,
    // stated in the engine's words: the size it was trained at, the sampler and
    // scheduler its release names, and whether a step cache has room in the
    // steps it asks for. One row per model; a model with no row keeps the
    // engine's own choice for its family, which is the right answer for the
    // families the engine knows by itself.
    struct ImageModelTuning
    {
      const char *id;
      int width;
      int height;
      const char *sampler;
      const char *scheduler;
      bool cache;
      // trailing, so a row that has nothing to say about it writes five fields and
      // the -1 stands.
      int clip_skip = -1;
      // trailing too: INFINITY means the row says nothing about the shift and the
      // engine's family default stands.
      float flow_shift = INFINITY;
      // trailing: a model whose work is a moving picture says so here, with the
      // clip length and frame rate its own card states it runs at home at.
      bool video = false;
      int frames = 0;
      int fps = 0;
    };
    static const std::vector<ImageModelTuning> &kTuning()
    {
      static const std::vector<ImageModelTuning> rows = {
          // SDXL Turbo: the card at 512x512 with guidance disabled
          // ("guidance_scale=0.0"), one to four steps; the repo ships an
          // Euler-Ancestral trailing-spacing scheduler, which is the grid
          // sgm_uniform draws. Four steps leave a step cache nothing to skip.
          {"sdxl-turbo", 512, 512, "euler_a", "sgm_uniform", false},
          // FLUX.3 Klein: the card's table states the distilled checkpoint at 16
          // steps with no guidance ("Klein | distilled, bfloat16 | 16 | none"),
          // at 1024x1024. The engine's FLUX.3 page names euler and carries the
          // upstream time shift of 6.93 in its own default scheduler, so no
          // scheduler is named here. No step cache for a transformer.
          {"flux-3-klein", 1024, 1024, "euler", "", false},
          // Z-Image Turbo: the card's Turbo row at Step 8 with CFG crossed out
          // ("Guidance should be 0 for the Turbo models"), stated at 1024. The
          // engine's page names no sampler for this family, so none is written.
          {"z-image-turbo", 1024, 1024, "", "", false},
          // Qwen Image 2.1 and its Turbo: the engine's page runs both with euler -
          // the base at cfg 6.0 (the released ComfyUI workflow ships 25 steps and
          // 1024x1024), the Turbo at 8 steps and cfg 1.0 with its own sigmas.
          {"qwen-image-2.1", 1024, 1024, "euler", "", false},
          {"qwen-image-2.1-turbo", 1024, 1024, "euler", "", false},
          // RealVisXL V5 Lightning: the card's parameters - 5 steps, DPM++ SDE
          // Karras, CFG 1.0-2.0 - at the square an SDXL is trained at. Five
          // distilled steps leave the engine's cache window (15%-95% of the steps)
          // with three steps inside it, so the cache is off.
          {"realvisxl-v5-lightning", 1024, 1024, "dpm++2m_sde", "karras", false},
          // Pony Diffusion V6 XL: the card recommends Euler a at 25 steps and
          // 1024px, with no negative prompt. A plain SDXL UNet at 25 steps is the
          // caching case the engine's page documents, so the cache stays on.
          {"pony-diffusion-v6", 1024, 1024, "euler_a", "", true},
          // CyberRealistic XL: the Recommended Settings table names 25-30 steps,
          // CFG 7.0-8.0, 1024x1024 (native), and "DPM++ 3M SDE Karras, Euler A" -
          // the middle of each range is taken. That sampler is this engine's
          // dpm++2m_sde_bt, the bit-trick 2M SDE ComfyUI ships as 3M.
          {"cyberrealistic-xl", 1024, 1024, "dpm++2m_sde_bt", "karras", true},
          // Animagine XL 3.1: the card's CFG "around 5-7", "sampling steps below
          // 30", Euler a, and the square of its Multi Aspect Resolution table.
          {"animagine-xl-3.1", 1024, 1024, "euler_a", "", true},
          // Lustify NSFW Checkpoint: CivitAI version 638929 - a VERSION id, so it
          // resolves at /api/v1/model-versions/638929, not the model endpoint. The
          // showcase photos carry the model hash of this very file, and their
          // metadata is the evidence for its numbers: steps 30, cfg most 4.5-5,
          // DPM++ 2M SDE on the karras grid, negative "airbrushed, blurry", sizes
          // about 832x1152. The recommended-parameters block on the page belongs to
          // the LIGHTNING files, another checkpoint, and is not used. The VAE is
          // baked into the file; SDXL's family clip skip of 2 stands.
          {"lustify-sdxl", 832, 1152, "dpm++2m_sde", "karras", true},
          // Juggernaut XL v9: the card's table: 832x1216 portrait, DPM++ 2M Karras,
          // 30-40 steps, CFG 3-7 - so 35 and 5.0, the numbers its own example
          // passes, at the size the card sends people to rather than SDXL's
          // training square. "Start with none" for negatives; the VAE is baked in.
          {"juggernaut-xl-v9", 832, 1216, "dpm++2m", "karras", true},
          // MiniMax H3: unsloth's own example run beside the very GGUF bytes this
          // machine downloads - 960x544, 124 frames at 24 FPS, 8 steps, guidance
          // 1.0, the length and shape the model's maker runs it at. 960 and 544
          // are multiples of 32 and 124 = 17*7 + 5 sits on the engine's frame grid
          // (docs/minimax_h3.md), so nothing is aligned upward on the way in.
          // Neither card names a sampler, so the family choice stands; the video
          // flow shift 12 is written from that same page, whose family default is
          // not it. No step cache: those are scoped to UNet models, and eight
          // distilled steps leave one nothing to skip.
          {"minimax-h3", 960, 544, "", "", false, -1, 12.0f, true, 124, 24},
      };
      return rows;
    }

    // What to ask of a model that changes a picture is written beside the model
    // itself: the words a page opens its prompt field with, naming the change and
    // the starting picture it belongs to.
    static const std::vector<ImageModelDef> &kModelList()
    {
      static const std::vector<ImageModelDef> models = []()
      {
        std::vector<ImageModelDef> out;
        const auto add = [](std::vector<ImageModelDef> &out, const std::string &id,
                            const std::string &name, const std::string &repo, int steps,
                            float guidance, const std::string &prompt, const std::string &negative,
                            const std::string &sigmas, int civitaiVersion,
                            const std::vector<std::pair<std::string, std::string>> &files,
                            const std::string &wtype = std::string(),
                            bool int8_convrot = false)
        {
          ImageModelDef def;
          def.id = id;
          def.name = name;
          def.repo = repo;
          def.negative = negative;
          def.sigmas = sigmas;
          def.wtype = wtype;
          def.int8_convrot = int8_convrot;
          def.prompt = prompt;
          def.steps = steps;
          def.guidance = guidance;
          def.civitaiVersion = civitaiVersion;
          def.files = files;
          out.push_back(def);
        };

        add(out, "flux-3-klein", "FLUX.3 Klein", "black-forest-labs/flux-3-image-klein", 16, 1.0f,
            "a grey tabby cat sitting upright holding a small handwritten card that reads "
            "'flux3-image-klein' with one paw, plain warm-grey studio backdrop, soft side "
            "light, the words crisp and legible",
            "", "", 0,
            {{"diffusion_model", "model.safetensors"},
             {"vae", "ae.safetensors"},
             {"llm", "hf:Qwen/Qwen3-VL-4B-Instruct-GGUF/Qwen3VL-4B-Instruct-Q4_K_M.gguf"}},
            "q8_0");
        add(out, "z-image-turbo", "Z-Image Turbo", "Comfy-Org/z_image_turbo", 8, 1.0f,
            "a cinematic photograph of a lone hooded figure standing centered at the far end of "
            "a rain-slicked city street at night, neon spilling in pink and cyan down the wet "
            "asphalt, a single neon sign reading 'CIRCUIT BOARD' crisp and legible above the "
            "doorway, 2.39:1 framing",
            "", "", 0,
            {{"diffusion_model", "split_files/diffusion_models/z_image_turbo_int8_convrot.safetensors"},
             {"vae", "split_files/vae/ae.safetensors"},
             {"llm", "hf:unsloth/Qwen3-4B-Instruct-2507-GGUF/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"}},
             std::string(), true);
        add(out, "qwen-image-2.1", "Qwen Image 2.1", "Comfy-Org/Qwen-Image-2.1", 25, 6.0f,
            "a grey tabby cat sitting upright holding a small handwritten card that reads "
            "'qwen2.1' with one paw, plain warm-grey studio backdrop, soft side light, the "
            "words crisp and legible, clean vector illustration",
            "", "", 0,
            {{"diffusion_model", "diffusion_models/qwen_image_2.1_int8_convrot.safetensors"},
             {"vae", "vae/qwen_image_2.1_vae_bf16.safetensors"},
             {"llm", "text_encoders/qwen3vl_8b_int8_convrot.safetensors"}},
             std::string(), true);
        add(out, "qwen-image-2.1-turbo", "Qwen Image 2.1 Turbo", "Comfy-Org/Qwen-Image-2.1", 8,
            1.0f, "a brass telescope on a velvet-draped table beside a small upright card that "
                  "reads 'qwen2.1 turbo', one warm lamp light, dark quiet background, the words "
                  "crisp "
                  "and legible",
            "",
            "1.0,0.978453,0.95418,0.926626,0.89508,0.845148,0.704534,0.414568,0.0", 0,
            {{"diffusion_model", "diffusion_models/qwen_image_2.1_turbo_int8_convrot.safetensors"},
             {"vae", "vae/qwen_image_2.1_vae_bf16.safetensors"},
             {"llm", "text_encoders/qwen3vl_8b_int8_convrot.safetensors"}},
             std::string(), true);
        add(out, "sdxl-turbo", "SDXL Turbo", "stabilityai/sdxl-turbo", 4, 0.0f,
            "a single red apple centered on a rustic wooden table, macro photo, one soft window "
            "light from the left, dark neutral background, shallow depth of field, no other "
            "objects",
            "", "", 0,
            {{"model", "sd_xl_turbo_1.0_fp16.safetensors"}});
        // The card names 5 steps, DPM++ SDE Karras, CFG 1.0-2.0, and a recommended
        // negative prompt, all shipped here as it writes them.
        add(out, "realvisxl-v5-lightning", "RealVisXL V5 Lightning", "SG161222/RealVisXL_V5.0_Lightning", 5, 1.5f,
            "portrait photo of a young woman with freckles in natural window light, 85mm lens, "
            "detailed skin texture, plain plaster wall behind her, shoulders and face only in "
            "frame",
            "(octane render, render, drawing, anime, bad photo, bad photography:1.3), (worst "
            "quality, low quality, blurry:1.2), (bad teeth, deformed teeth, deformed lips), "
            "(bad anatomy, bad proportions:1.1), (deformed iris, deformed pupils), (deformed "
            "eyes, bad eyes), (deformed face, ugly face, bad face), (deformed hands, bad hands, "
            "fused fingers), morbid, mutilated, mutation, disfigured",
            "", 0, {{"model", "RealVisXL_V5.0_Lightning_fp16.safetensors"}});
        // The card's own recommendation: Euler a, 25 steps, 1024px, no negative
        // prompt, default guidance 7. Its clip skip 2 is the engine's SDXL family
        // default already. The prompt carries the card's own quality template.
        add(out, "pony-diffusion-v6", "Pony Diffusion V6 XL", "LyliaEngine/Pony_Diffusion_V6_XL", 25,
            7.0f, "score_9, score_8_up, score_7_up, score_6_up, score_5_up, score_4_up, "
                  "source_anime, a knight in ornate silver armour standing alone in a stone "
                  "hall, torchlight on the breastplate, detailed face, dramatic lighting",
            "", "", 0,
            {{"model", "ponyDiffusionV6XL_v6StartWithThisOne.safetensors"}, {"vae", "sdxl_vae.safetensors"}});
        // The card's Recommended Settings: steps 25-30, "DPM++ 3M SDE Karras,
        // Euler A", 1024x1024 (native), CFG 7.0-8.0 - the middle of each range.
        // It ships its own VAE ("Built-in VAE"), so the row names none.
        add(out, "cyberrealistic-xl", "CyberRealistic XL", "cyberdelia/CyberRealisticXL", 28, 7.5f,
            "(masterpiece, best quality), RAW photo figure study of an unclothed woman seated with "
            "her back to the camera on a wooden stool against a plain grey backdrop, bare shoulders "
            "and back, one soft key light from the upper left, skin texture visible, no "
            "retouching, tasteful framing, RAW photo, 85mm",
            "", "", 0, {{"model", "CyberRealisticXLPlay_V10.0_FP16.safetensors"}});
        // The card's own settings: CFG "around 5-7", "sampling steps below 30",
        // Euler a, and the square of its Multi Aspect Resolution table. It is
        // "optimized for Danbooru-style tags rather than natural language prompts",
        // so the prompt is tags and the negative is the card's, verbatim.
        add(out, "animagine-xl-3.1", "Animagine XL 3.1", "cagliostrolab/animagine-xl-3.1", 28, 6.0f,
            "1girl, green hair, sweater, looking at viewer, upper body, beanie, outdoors, "
            "night, turtleneck, masterpiece, best quality, very aesthetic, absurdres",
            "nsfw, lowres, (bad), text, error, fewer, extra, missing, worst quality, jpeg "
            "artifacts, low quality, watermark, unfinished, displeasing, oldest, early, "
            "chromatic aberration, signature, extra digits, artistic error, username, scan, "
            "[abstract]",
            "", 0, {{"model", "animagine-xl-3.1.safetensors"}});
        add(out, "lustify-sdxl", "Lustify NSFW Checkpoint", "", 30, 5.0f,
            "score_9, 4k, highres, best quality, boudoir photograph at dawn of an unclothed woman "
            "reclining on an unmade bed, one arm resting across her body, a sheer linen sheet "
            "slipped to her hip, pale window light from the left, hair loose, natural skin, "
            "(cinematic lighting:1.1), 50mm film photograph",
            "airbrushed, blurry", "", 638929, {{"model", "lustifyNSFWCheckpoint_v10Alpha.safetensors"}});
        add(out, "juggernaut-xl-v9", "Juggernaut XL v9", "RunDiffusion/Juggernaut-XL-v9", 35, 5.0f,
            "Cinematic mid shot photo of an astronaut walking through a neon-lit Tokyo alley at "
            "night, hyperdetailed photography, skin details, shallow depth of field",
            "", "", 0, {{"model", "Juggernaut-XL_v9_RunDiffusionPhoto_v2.safetensors"}});
        // MiniMax H3 - the first video model this machine offers, so its row states
        // what a clip costs rather than what a picture costs. Every file is named
        // where it is actually kept: the denoiser and the text encoder come from
        // unsloth's GGUF repo (the bytes this project downloads), and the two VAEs
        // are not duplicated there - "The VAEs are not duplicated here, take them
        // from Comfy-Org/MiniMax-H3" (the words on that same README) - so they are
        // read from Comfy-Org/MiniMax-H3/vae, which is also what the engine's own
        // page names (docs/minimax_h3.md:33-36). The four components are passed
        // separately, as that page states (docs/minimax_h3.md:7: "Pass the four
        // MiniMax-H3 components separately"), and the audio VAE is one of them:
        // without it "Omitting --audio-vae still runs the joint diffusion model but
        // produces video without a decoded audio track" (docs line 46), which would
        // leave half of what this model makes unmade.
        // The row names the Q4_K denoiser: unsloth pairs "the Q4_K_M one with
        // everything else" above the two smallest denoisers, and the engine's own
        // example runs a Q4_K-class denoiser beside the Q4_K_M encoder - the pairing
        // both sources name. Which form a run opens is the forms table below, and a
        // form may replace the denoiser or the text encoder, since the two are
        // separate checkpoints ("They are separate checkpoints, not settings").
        add(out, "minimax-h3", "MiniMax H3", "unsloth/MiniMax-H3-GGUF", 8, 1.0f,
            "a cute silver tabby kitten surfing a tropical ocean wave on a white surfboard, "
            "cinematic tracking shot, realistic water, bright sunlight, smooth motion. Add "
            "upbeat tropical surf-rock background music with cheerful drums and guitar.",
            "", "", 0,
            {{"diffusion_model", "hf:unsloth/MiniMax-H3-GGUF/minimax_h3_fl2va_pruned-Q4_K.gguf"},
             {"llm", "hf:unsloth/MiniMax-H3-GGUF/qwen3vl_32b_minimax_h3-Q4_K_M.gguf"},
             {"vae", "hf:Comfy-Org/MiniMax-H3/vae/minimax_h3_video_vae_fp16.safetensors"},
             {"audio_vae", "hf:Comfy-Org/MiniMax-H3/vae/minimax_h3_audio_vae_fp32.safetensors"}});
        for (ImageModelDef &def : out)
        {
          for (const auto &row : kSpecialty())
          {
            if (row.first == def.id)
              def.specialty = row.second;
          }
          for (const auto &row : kCapability())
          {
            if (std::get<0>(row) != def.id)
              continue;
            if (std::get<1>(row) == "edit")
              def.edit = true;
            if (std::get<1>(row) == "redraw")
              def.redraw = true;
            def.refs = std::get<2>(row);
          }
          for (const ImageModelTuning &row : kTuning())
          {
            if (std::string(row.id) != def.id)
              continue;
            def.width = row.width;
            def.height = row.height;
            def.sampler = row.sampler;
            def.scheduler = row.scheduler;
            def.cache = row.cache;
            def.clip_skip = row.clip_skip;
            def.flow_shift = row.flow_shift;
            def.video = row.video;
            def.frames = row.frames;
            def.fps = row.fps;
          }
          // A model's one documented strength stays as one line: the words a run
          // asks with are typed on the page, and nothing about scenes is carried
          // with the model table.
        }
        return out;
      }();
      return models;
    }

    static const ImageModelDef *findModel(const std::string &id)
    {
      for (const ImageModelDef &def : kModelList())
      {
        if (def.id == id)
          return &def;
      }
      return nullptr;
    }

    static std::mutex gMutex;
    static std::condition_variable gCondition;
    static std::map<std::string, sd_ctx_t *> gCtx;
    static std::mutex gCtxMutex;
    // The card is one worker. The engine stages a run's weights against the
    // card's whole budget, and it cannot see a second run staging the same
    // bytes for itself: sixty pictures failed in four seconds on one 8 GiB
    // card when six runs sampled at once, each reading the others' leftovers
    // as free memory. A run that asks the card while another holds it waits
    // here, job standing, in the order it came. The gate is the machine's own
    // (see the card in Common.h): every engine in this binary builds its
    // graphs on the same device, and the device does not know which module
    // asked.
    static std::mutex &gCardGate = ols::deviceCard();
    // Resident models are charged with the bytes of weights they hold and stamped
    // with the tick of their last use, so a load can keep room by closing the
    // least recently used idle model first.
    static std::map<std::string, uint64_t> gCtxUse;
    static std::map<std::string, long long> gCtxBytes;
    // Which device each open context was opened on. A run that asks for the
    // other one gets its weights moved: drawing on the context that is standing
    // would answer the run with a device it did not ask for.
    static std::map<std::string, bool> gCtxCpu;
    // The form each standing context was opened as. A later ask for another form
    // must get those files moved, not be answered from the weights already there.
    static std::map<std::string, std::string> gCtxForm;
    static uint64_t gUseTick = 0;

    // The card answers two questions about itself - the whole bytes of device
    // memory it is, and the numbers of its compute capability - through the
    // engine loader's reading of nvml, which stands with the driver on every
    // machine that runs one of these cards. Asking nvml costs no CUDA context
    // and no toolkit, and it answers whether or not an engine library stands
    // or has opened the card. A machine where nothing answers - a card of
    // another maker, no driver - reads as no card (0, -1) and the engine is
    // left alone. The probe answering nothing while the engine burns the card
    // anyway is not innocent: measured on MSI 2026-11-19, the Windows binary
    // asked its 8 GiB card for a 6.4 GB graph segment against an unlimited
    // budget and died on it, because both the weight budget and the clip
    // window read this one answer.
    struct CardAnswer
    {
      long long totalBytes = 0;
      int computeMajor = -1;
      int computeMinor = 0;
    };
    static const CardAnswer &cardAnswer()
    {
      static const CardAnswer answer = []()
      {
        CardAnswer out;
        long long freeBytes = 0;
        cardFacts(out.totalBytes, freeBytes, out.computeMajor, out.computeMinor);
        return out;
      }();
      return answer;
    }

    // The working-set ceiling for image models: a standing three quarters of the
    // largest GPU heap, so the compute buffers and activations still fit beside
    // the resident weights. what can actually be held is answered when a context
    // is opened, where the engine asks its own allocator for room.
    static long long gpuTotalBytes()
    {
      return cardAnswer().totalBytes;
    }

    static long long memoryBudgetBytes()
    {
      const long long heap = gpuTotalBytes();
      return heap > 0 ? heap / 4 * 3 : 19327352832LL; // three quarters of 24 GiB
    }
    static std::map<std::string, std::vector<std::string>> gEvents;
    static std::map<std::string, bool> gEnded;
    static std::map<std::string, bool> gCancel;
    // One record per job: kind, model, state, timings, progress, and the file it made.
    static std::map<std::string, json> gJobs;
    // What stands proved about the bytes of this weights room is the room's own
    // record, read and written through VerifiedRoom - see the room beside it.
    // Which precision of a model a person last asked for. The choice is a
    // machine-wide fact about a model, so it stands beside the weights room and
    // outlives a restart.
    static std::map<std::string, std::string> gChosenQuant;
    static std::map<std::string, std::shared_ptr<std::mutex>> gLoadGuards;
    static std::map<std::string, int> gGenerating;
    static std::map<std::string, bool> gPendingErase;
    static std::mutex gGenerationMutex;
    static std::deque<std::string> gJobOrder;
    // One render works on the card at a time, whatever model it stands for.
    // The order a generate job keeps in gJobOrder is the line it stands in:
    // whoever reaches the head of the standing generate jobs while the card
    // is free is the one that goes.
    static bool gCardBusy = false;
    // Why the last call this thread made was refused. It is read on the thread
    // that made the call - the door asks for the reason in the breath after the
    // work was refused - so it stands one per thread: held in one place for the
    // whole machine, two requests refused at the same moment wrote over each
    // other's words while a third was still copying them.
    static thread_local std::string gEngineError;

    // Every ask of the image engine goes through the table the loader filled
    // from the library standing beside this server (see EngineLoader.h). These
    // forwarders wear the engine's own names so the work below reads the same
    // as it always did; what they add is the one honest answer a machine
    // without a standing library has to give - refused, in words, nowhere a
    // null jumps through.
    static bool engineRefused(const char *name)
    {
      gEngineError = std::string("no image engine library stands beside this server to answer ") +
                     name + (engineWords().empty() ? std::string() : " (" + engineWords() + ")");
      return false;
    }
    static sd_ctx_t *new_sd_ctx(const sd_ctx_params_t *params)
    {
      if (!engineStands())
      {
        engineRefused("new_sd_ctx");
        return nullptr;
      }
      return engineApi().new_sd_ctx(params);
    }
    static void free_sd_ctx(sd_ctx_t *ctx)
    {
      if (engineStands())
        engineApi().free_sd_ctx(ctx);
    }
    static bool generate_image(sd_ctx_t *ctx, const sd_img_gen_params_t *params,
                               sd_image_t **images, int *produced)
    {
      if (!engineStands())
        return engineRefused("generate_image");
      return engineApi().generate_image(ctx, params, images, produced);
    }
    static bool generate_video(sd_ctx_t *ctx, const sd_vid_gen_params_t *params,
                               sd_image_t **frames, int *count, sd_audio_t **audio, int *fps)
    {
      if (!engineStands())
        return engineRefused("generate_video");
      return engineApi().generate_video(ctx, params, frames, count, audio, fps);
    }
    static void sd_cancel_generation(sd_ctx_t *ctx, enum sd_cancel_mode_t mode)
    {
      if (engineStands())
        engineApi().sd_cancel_generation(ctx, mode);
    }
    static size_t sd_list_devices(char *buffer, size_t size)
    {
      if (!engineStands())
        return 0;
      return engineApi().sd_list_devices(buffer, size);
    }
    static void sd_set_log_callback(sd_log_cb_t cb, void *data)
    {
      if (engineStands())
        engineApi().sd_set_log_callback(cb, data);
    }
    static void sd_set_progress_callback(sd_progress_cb_t cb, void *data)
    {
      if (engineStands())
        engineApi().sd_set_progress_callback(cb, data);
    }
    static void sd_ctx_params_init(sd_ctx_params_t *params)
    {
      // the parameter records keep the defaults inside the engine's own
      // headers, so a machine without a standing library still fills the
      // struct the same way the engine would - the refusal comes at the call.
      *params = {};
      if (engineStands())
        engineApi().sd_ctx_params_init(params);
    }
    static void sd_sample_params_init(sd_sample_params_t *params)
    {
      *params = {};
      if (engineStands())
        engineApi().sd_sample_params_init(params);
    }
    static void sd_img_gen_params_init(sd_img_gen_params_t *params)
    {
      *params = {};
      if (engineStands())
        engineApi().sd_img_gen_params_init(params);
    }
    static void sd_vid_gen_params_init(sd_vid_gen_params_t *params)
    {
      *params = {};
      if (engineStands())
        engineApi().sd_vid_gen_params_init(params);
    }
    static enum sample_method_t sd_get_default_sample_method(const sd_ctx_t *ctx)
    {
      if (!engineStands())
      {
        engineRefused("sd_get_default_sample_method");
        return EULER_SAMPLE_METHOD;
      }
      return engineApi().sd_get_default_sample_method(ctx);
    }
    static enum scheduler_t sd_get_default_scheduler(const sd_ctx_t *ctx,
                                                     enum sample_method_t method)
    {
      if (!engineStands())
      {
        engineRefused("sd_get_default_scheduler");
        return DISCRETE_SCHEDULER;
      }
      return engineApi().sd_get_default_scheduler(ctx, method);
    }
    static void free_sd_audio(sd_audio_t *audio)
    {
      if (audio && engineStands())
        engineApi().free_sd_audio(audio);
    }
    static void free_sd_images(sd_image_t *images, int count)
    {
      if (images && engineStands())
        engineApi().free_sd_images(images, count);
    }
    static enum sd_type_t str_to_sd_type(const char *text)
    {
      // the name lookups answer from tables of words when no engine stands:
      // the words this machine knows are its own, and a name no engine reads
      // is refused at the open, as ever.
      if (engineStands())
        return engineApi().str_to_sd_type(text);
      return SD_TYPE_COUNT;
    }
    static enum sample_method_t str_to_sample_method(const char *text)
    {
      if (engineStands())
        return engineApi().str_to_sample_method(text);
      return SAMPLE_METHOD_COUNT;
    }
    static enum scheduler_t str_to_scheduler(const char *text)
    {
      if (engineStands())
        return engineApi().str_to_scheduler(text);
      return SCHEDULER_COUNT;
    }

    // Every call below wears the qualified name, because the engine's own
    // header declares these names at the global scope: an unqualified call
    // would see both this file's forwarder and that header's prototype (the
    // argument-dependent lookup reaches the global one through the engine's
    // own types) and stand undecided between them. The header's prototypes
    // describe a library this binary no longer links; these names go through
    // the table, and only through the table.
#define new_sd_ctx ols::imagegen::new_sd_ctx
#define free_sd_ctx ols::imagegen::free_sd_ctx
#define generate_image ols::imagegen::generate_image
#define generate_video ols::imagegen::generate_video
#define sd_cancel_generation ols::imagegen::sd_cancel_generation
#define sd_list_devices ols::imagegen::sd_list_devices
#define sd_set_log_callback ols::imagegen::sd_set_log_callback
#define sd_set_progress_callback ols::imagegen::sd_set_progress_callback
#define sd_ctx_params_init ols::imagegen::sd_ctx_params_init
#define sd_sample_params_init ols::imagegen::sd_sample_params_init
#define sd_img_gen_params_init ols::imagegen::sd_img_gen_params_init
#define sd_vid_gen_params_init ols::imagegen::sd_vid_gen_params_init
#define sd_get_default_sample_method ols::imagegen::sd_get_default_sample_method
#define sd_get_default_scheduler ols::imagegen::sd_get_default_scheduler
#define free_sd_audio ols::imagegen::free_sd_audio
#define free_sd_images ols::imagegen::free_sd_images

    // where a running job began, in whole milliseconds: the home page measures
    // a run's steps-per-second against how long it took, and whole seconds of a
    // run are too coarse to be that rate. the start sits beside the job, not in
    // its record, and is read once when the job settles.
    static std::map<std::string, long long> gStartedMs;

    // the newest jobs this engine keeps: past this its oldest record is dropped,
    // so the merged status answer stays a few kilobytes however long the machine
    // has been up. the cap is ols::kKeptJobs, the one number the shared ledger
    // states for every service.
    static void pruneJobs()
    {
      ols::forgetPastKept(gJobOrder, ols::kKeptJobs, gJobs, gEvents, gEnded, gCancel, gStartedMs);
    }
    static std::string gWeightsDir;
    static std::string gOutputDir;
    static std::string gRefsDir;

    // Called with gMutex held. Read the chosen precisions back once per process.
    static void ensureChosenLoadedLocked()
    {
      static bool loaded = false;
      if (loaded)
        return;
      loaded = true;
      std::string body;
      if (!readFileText(gWeightsDir + "/.chosen-quants.json", body))
        return;
      const json doc = json::parse(body, nullptr, false);
      if (!doc.is_object())
        return;
      for (const auto &entry : doc.items())
      {
        if (entry.value().is_string())
          gChosenQuant[entry.key()] = entry.value().get<std::string>();
      }
    }

    // Called with gMutex held. The few short rows are rewritten whole: as cheap
    // as merging one into them, and a half-written file never stands.
    static bool saveChosenLocked()
    {
      json doc = json::object();
      for (const auto &entry : gChosenQuant)
        doc[entry.first] = entry.second;
      return writeFileText(gWeightsDir + "/.chosen-quants.json", doc.dump());
    }

    // What a settled run cost, filed beside the settings it ran at: the honest
    // answer to "what will this draw take" is what these same settings cost here
    // before. A generation is keyed by the model and by everything that changes
    // the work - the sides, the steps, the guidance, the sampler and scheduler as
    // this run resolved them, the step cache, the device, the sage-attention
    // switch, the picture count and the reference count. An open is keyed by the
    // model and the precision it was opened as. The seed is in no key: another
    // seed is the same work. A run that was cancelled or failed is not filed.
    // The ledger stands behind its own lock: filing and reading it reach files,
    // and that work must never happen while gMutex - the lock every page answer
    // passes through - is held.
    static std::map<std::string, json> gTimings;
    static std::mutex gTimingsMutex;
    // One row per set of settings ever run to the end; past this depth the
    // oldest-touched rows come off, so a machine that has been reconfigured for
    // years cannot grow the ledger or the file past a page's patience.
    static const std::size_t kMaxTimingRows = 400;

    // Called with gTimingsMutex held. Read the ledger back once per process.
    static void ensureTimingsLoaded()
    {
      static bool loaded = false;
      if (loaded)
        return;
      loaded = true;
      std::string body;
      if (!readFileText(gWeightsDir + "/.run-timings.json", body))
        return;
      const json doc = json::parse(body, nullptr, false);
      if (!doc.is_object())
        return;
      for (const auto &entry : doc.items())
      {
        if (entry.value().is_object() && entry.value().contains("count"))
          gTimings[entry.key()] = entry.value();
      }
    }

    // Called with gTimingsMutex held.
    static bool saveTimings()
    {
      json doc = json::object();
      for (const auto &entry : gTimings)
        doc[entry.first] = entry.second;
      return writeFileText(gWeightsDir + "/.run-timings.json", doc.dump());
    }

    // From the one place a job settles, once the file it describes is settled
    // too: a run that finished every step of its window says what these settings
    // cost, and nothing else does. Called without gMutex held.
    static void noteTiming(const json &record)
    {
      if (!record.is_object() || !record.contains("kind") || !record.contains("model"))
        return;
      const std::string kind = record.value("kind", std::string());
      if (kind != "generate" && kind != "load")
        return;
      if (record.value("state", std::string()) != ols::kStateDone)
        return;
      if (!record.contains("elapsed_ms") || !record["elapsed_ms"].is_number())
        return;
      const long long ms = record["elapsed_ms"].get<long long>();
      if (ms <= 0)
        return;
      json settings = json::object();
      if (kind == "load")
      {
        std::string precision = record.value("quant", std::string());
        settings["precision"] = precision.empty() ? std::string("as shipped") : precision;
      }
      else
      {
        // the settings are the key, and an ask that was refused never had any
        if (!record.contains("settings") || !record["settings"].is_object())
          return;
        settings = record["settings"];
      }
      // the key is the settings themselves, printed with its keys in order, so
      // the same ask from any door lands on one row
      const std::string key = kind + "|" + record.value("model", std::string()) + "|" + settings.dump();
      const long long now = nowSeconds();
      std::lock_guard<std::mutex> lock(gTimingsMutex);
      ensureTimingsLoaded();
      auto at = gTimings.find(key);
      if (at == gTimings.end())
      {
        json row = settings;
        row["kind"] = kind;
        row["model"] = record.value("model", std::string());
        row["count"] = 1;
        row["total_ms"] = ms;
        row["min_ms"] = ms;
        row["max_ms"] = ms;
        row["last_ms"] = ms;
        row["updated"] = now;
        gTimings[key] = row;
      }
      else
      {
        json &row = at->second;
        const long long count = row.value("count", 0LL);
        const long long total = row.value("total_ms", 0LL);
        row["count"] = count + 1;
        row["total_ms"] = total + ms;
        if (ms < row.value("min_ms", ms))
          row["min_ms"] = ms;
        if (ms > row.value("max_ms", ms))
          row["max_ms"] = ms;
        row["last_ms"] = ms;
        row["updated"] = now;
      }
      while (gTimings.size() > kMaxTimingRows)
      {
        auto oldest = gTimings.begin();
        for (auto it = gTimings.begin(); it != gTimings.end(); ++it)
        {
          if (it->second.value("updated", 0LL) < oldest->second.value("updated", 0LL))
            oldest = it;
        }
        gTimings.erase(oldest);
      }
      saveTimings();
    }
    // How many of this machine's own renders stand on disk. The newest that many
    // are kept, and that many is also the deepest a gallery page can be asked
    // for, so one number states both ends: a reader cannot reach past what the
    // machine keeps, and the room cannot grow past it either. The count is a
    // default the entry point may move through a setting; a machine that was set
    // for a smaller room keeps that many and no more.
    static const int kGalleryCeilingDefault = 10000;
    static int gKeptMax = kGalleryCeilingDefault;
    // Reaching the folder to prune it walks every file in it, so the walk is not
    // repeated on every picture that lands: the room is trimmed only once it stands
    // a slack past its ceiling, which takes the oldest slack off in one pass rather
    // than the oldest one file at a time.
    static const int kPruneSlack = 128;
    static int32_t gThreads = 4;
    // The download keys stand behind their own lock: the entry point writes them
    // once at startup and curl reads them on every job thread after that.
    static std::mutex gKeysMutex;
    static std::string gHfToken;
    static std::string gCivitaiKey;

    static std::string lowerText(const std::string &text)
    {
      std::string out = text;
      for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return out;
    }

    static std::string contentTypeForName(const std::string &name)
    {
      const std::string key = lowerText(name);
      if (key.size() > 4 && key.compare(key.size() - 4, 4, ".png") == 0)
        return "image/png";
      if (key.size() > 4 && key.compare(key.size() - 4, 4, ".jpg") == 0)
        return "image/jpeg";
      if (key.size() > 5 && key.compare(key.size() - 5, 5, ".jpeg") == 0)
        return "image/jpeg";
      if (key.size() > 5 && key.compare(key.size() - 5, 5, ".webp") == 0)
        return "image/webp";
      // a clip and the parts of it a page plays on its own: the clip is the
      // model's own MJPEG AVI (an MPEG-J track with the stereo audio muxed in),
      // and its plain-pixel form is the same frames with no codec to know - both
      // are AVI files, so both are answered with that container. The preview is an
      // APNG a browser draws as an image, and the track is the clip's sound on its
      // own. Said apart so a page can reach either.
      if (key.size() > 5 && key.compare(key.size() - 5, 5, ".apng") == 0)
        return "image/apng";
      if (key.size() > 4 && key.compare(key.size() - 4, 4, ".avi") == 0)
        return "video/x-msvideo";
      if (key.size() > 4 && key.compare(key.size() - 4, 4, ".wav") == 0)
        return "audio/wav";
      return "application/octet-stream";
    }

    static void pushEvent(const std::string &job, const std::string &event)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      gEvents[job].push_back(event);
      gCondition.notify_all();
    }

    static bool busyModel(const std::string &modelId)
    {
      const auto found = gGenerating.find(modelId);
      return found != gGenerating.end() && found->second > 0;
    }

    static void touchContext(const std::string &modelId, const std::string &form, long long bytes);
    static std::string lruVictimLocked(const std::string &keep);
    static void closeContextLocked(const std::string &modelId);

    static void finishPendingLocked()
    {
      for (auto it = gPendingErase.begin(); it != gPendingErase.end();)
      {
        if (busyModel(it->first))
        {
          ++it;
          continue;
        }
        // closeContextLocked drops the model's own pending-erase entry with it,
        // which takes this iterator with it: take the name and step past the
        // entry before anything can erase it.
        const std::string modelId = it->first;
        it = gPendingErase.erase(it);
        closeContextLocked(modelId);
      }
    }

    static void endJob(const std::string &job)
    {
      {
        std::lock_guard<std::mutex> lock(gMutex);
        gEnded[job] = true;
      }
      {
        // what waits here may close a context, and a close hands the device
        // back its memory: it stands on the card, as every road that touches
        // the device does.
        std::lock_guard<std::mutex> device(ols::deviceCard());
        std::lock_guard<std::mutex> lock(gCtxMutex);
        finishPendingLocked();
      }
      gCondition.notify_all();
    }

    static void noteError(const std::string &job, const std::string &reason)
    {
      {
        std::lock_guard<std::mutex> lock(gMutex);
        gJobs[job]["error"] = reason;
      }
      pushEvent(job, json{{"type", "error"}, {"message", reason}}.dump());
    }

    static void markState(const std::string &job, const std::string &state)
    {
      json timingRecord;
      bool willFile = false;
      bool wake = false;
      {
        std::lock_guard<std::mutex> lock(gMutex);
        json &record = gJobs[job];
        wake = ols::jobStanding(record);
        record["state"] = state;
        if (ols::stateEnded(state))
        {
          // the card lets go with the run that held it, and the line wakes to
          // whoever stands next; a job that ended before its turn never held
          // the card and lets nothing go.
          if (gCardBusy && record.contains("on_card") && record["on_card"].is_boolean() &&
              record["on_card"].get<bool>())
          {
            gCardBusy = false;
            record.erase("on_card");
          }
          record["ended_at"] = nowSeconds();
          const auto started = gStartedMs.find(job);
          if (started != gStartedMs.end())
          {
            record["elapsed_ms"] = steadyMs() - started->second;
            gStartedMs.erase(job);
          }
          // the record now carries its own settings and its whole cost: what
          // those settings cost is filed below, after the lock is let go,
          // because filing reaches a file
          willFile = true;
          timingRecord = record;
          // A finished job finished every step of its own count, so a row that
          // reads "0 of 1" under a job that just completed would be a lie; a
          // stopped or failed job keeps the part it really did.
          if (state == ols::kStateDone)
          {
            const auto max = record.find("max");
            const auto done = record.find("done");
            if (max != record.end() && done != record.end())
              *done = *max;
          }
        }
      }
      if (willFile)
        noteTiming(timingRecord);
      if (wake)
        gCondition.notify_all();
      if (state == ols::kStateCancelled)
        pushEvent(job, json{{"type", "cancelled"}}.dump());
    }

    static void noteProgress(const std::string &job, int done, int total)
    {
      if (total > 0 && done > total)
        done = total;
      {
        std::lock_guard<std::mutex> lock(gMutex);
        json &record = gJobs[job];
        // a run's progress only moves forward: the engine calls its progress hook
        // for its own passes in whatever order it likes, and a page would draw the
        // bar of a three-step run shrinking back to two. One tick is believed
        // backwards: the engine's own "sampling starts" call at zero, which is
        // what lifts the bar off whatever the loading and the encoding left it at.
        const int kept = record.contains("done") ? record["done"].get<int>() : 0;
        const int window = record.contains("max") ? record["max"].get<int>() : 0;
        if (window == total && done < kept && done > 0)
          return;
        record["done"] = done;
        record["max"] = total;
      }
      pushEvent(job, json{{"type", "progress"}, {"done", done}, {"max", total}}.dump());
    }

    static void noteResult(const std::string &job, const std::string &url)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      json &record = gJobs[job];
      // every file a run made, in the order the stream carried them; "url" stays
      // the first, which is the one a page with room for one picture shows.
      if (!record.contains("urls") || !record["urls"].is_array())
        record["urls"] = json::array();
      record["urls"].push_back(url);
      if (!record.contains("url"))
        record["url"] = url;
    }

    // A word about one step that did not stop the job: a file that came by a
    // different road than its card, a link that did not answer, a copy the two
    // boxes do not agree on. The record keeps the words in order, so what a person
    // reads after the fact is what really happened and not only the state the job
    // settled on - and an error field stays what it means, a job that stopped.
    static void noteStep(const std::string &job, const std::string &words)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      json &record = gJobs[job];
      if (!record.contains("notes"))
        record["notes"] = json::array();
      record["notes"].push_back(words);
    }

    static bool cancelledRequested(const std::string &job)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      const auto found = gCancel.find(job);
      return found != gCancel.end() && found->second;
    }

    // Stand in the line until the card is free and no standing generate job was
    // asked for before this one: gJobOrder is the order the machine took its
    // asks in, and a reordering moves a job inside that line. The job holds the
    // card the moment it sees its turn - before it lets the lock go - so two
    // waiters never cross the head together. A stop asked while standing is
    // believed here: the engine is never started for a picture nobody wants.
    static bool waitForTurn(const std::string &job)
    {
      std::unique_lock<std::mutex> lock(gMutex);
      while (true)
      {
        const auto stop = gCancel.find(job);
        if (stop != gCancel.end() && stop->second)
          return false;
        bool clear = !gCardBusy;
        if (clear)
          for (const auto &name : gJobOrder)
          {
            if (name == job)
              break;
            const auto row = gJobs.find(name);
            if (row == gJobs.end() || row->second.value("kind", std::string()) != "generate")
              continue;
            if (ols::jobStanding(row->second))
            {
              clear = false;
              break;
            }
          }
        if (clear)
        {
          gCardBusy = true;
          gJobs[job]["on_card"] = true;
          break;
        }
        gCondition.wait_for(lock, std::chrono::seconds(1));
      }
      lock.unlock();
      markState(job, ols::kStateRunning);
      return true;
    }

    // A job runs on a thread of its own: an exception that reached the top of
    // that thread would end the process, so the failure becomes the job's own
    // error state, which is what the pages read.
    template <class Fn>
    static void runJobGuarded(const std::string &job, const Fn &body)
    {
      try
      {
        body();
      }
      catch (const std::exception &e)
      {
        const std::string words = e.what();
        noteError(job, words.empty() ? std::string("the run stopped without words") : words);
        markState(job, ols::kStateError);
      }
      catch (...)
      {
        noteError(job, "the run stopped in a way it does not state");
        markState(job, ols::kStateError);
      }
    }

    static int runCurl(const std::string &url, const std::string &dest, int maxSeconds = 0)
    {
      // read the keys under their lock: a job thread reaches here while the
      // entry point may still be standing the keys up
      std::string token;
      {
        std::lock_guard<std::mutex> lock(gKeysMutex);
        token = url.rfind("https://civitai.com/", 0) == 0 ? gCivitaiKey : gHfToken;
      }
      return ols::platform::curlFetch(dest, url, token, maxSeconds, maxSeconds > 0 ? 0 : 3);
    }

    // How long one round of an internet fetch is allowed to run: one curl for the
    // whole of a file answers when the file is done or the link died, and nothing
    // in between - so cancel would wait for all of it, and the bar would state
    // nothing while thirty gigabytes arrive. Each round resumes from the bytes
    // already down and ends by telling the watching job what it has.
    constexpr int kFetchSliceSeconds = 20;

    // A file from the internet, brought down in rounds. True when the address
    // answered the whole of it - either curl said it finished, or the size the card
    // states now stands on the disk. False when the caller stopped asking, or when
    // the link stopped carrying the file; which of those it was is the caller's to
    // ask, because it is the one that knows whether cancel was pressed.
    // Where a caller leaves word (stated), each round tells it the bytes that
    // stand on the disk and the rate the round just carried them at - once before
    // the first round too, so a bar watching the file starts from where the bytes
    // already stand rather than from nothing.
    static bool fetchWhole(const std::string &url, const std::string &dest, long long want,
                           const std::function<bool()> &cancelled, int &answerOut,
                           const std::function<void(long long, double)> &stated = std::function<void(long long, double)>())
    {
      const long long goal = want > 0 ? want : 0;
      int stalls = 0;
      bool told = false;
      for (;;)
      {
        if (cancelled && cancelled())
          return false;
        const long long before = ols::fileSize(dest);
        if (stated && !told)
        {
          told = true;
          stated(before, 0.0);
        }
        const long long beganAt = ols::steadyMs();
        const int answer = runCurl(url, dest, kFetchSliceSeconds);
        answerOut = answer;
        // This machine would not ask for the file at all, because one of its
        // names is not something a command line can carry. No further round
        // changes that, so the file is given up on here.
        if (answer == -1)
          return false;
        const long long after = ols::fileSize(dest);
        if (stated)
        {
          const long long took = ols::steadyMs() - beganAt;
          stated(after, took > 0 ? double(after > before ? after - before : 0) * 1000.0 / double(took) : 0.0);
        }
        if (answer == 0 || (goal > 0 && after >= goal))
          return true;
        stalls = after > before ? 0 : stalls + 1;
        if (stalls >= 5)
          return false;
      }
    }

    static bool sharded(const std::string &slot)
    {
      return slot.find("-00001-of-") != std::string::npos;
    }

    static std::string treePathOf(const std::string &slot)
    {
      if (slot.rfind("hf:", 0) != 0)
        return slot;
      const std::string rest = slot.substr(3);
      const size_t first = rest.find('/');
      if (first == std::string::npos)
        return slot;
      const size_t second = rest.find('/', first + 1);
      if (second == std::string::npos)
        return slot;
      return rest.substr(second + 1);
    }

    static std::string localNameOf(const std::string &slot)
    {
      if (slot.rfind("hf:", 0) != 0)
        return slot;
      const size_t first = slot.find('/');
      if (first == std::string::npos)
        return slot;
      const size_t second = slot.find('/', first + 1);
      if (second == std::string::npos)
        return slot;
      return slot.substr(second + 1);
    }

    static std::string slotRepo(const std::string &slot)
    {
      if (slot.rfind("hf:", 0) != 0)
        return std::string();
      const std::string rest = slot.substr(3);
      const size_t first = rest.find('/');
      const size_t second = rest.find('/', first + 1);
      if (first == std::string::npos || second == std::string::npos)
        return std::string();
      return rest.substr(0, second);
    }

    static std::string downloadUrl(const std::string &repo, const std::string &slot, int civitaiVersion)
    {
      if (civitaiVersion > 0)
      {
        return "https://civitai.com/api/download/models/" + std::to_string(civitaiVersion);
      }
      if (slot.rfind("hf:", 0) == 0)
      {
        const std::string rest = slot.substr(3);
        const size_t first = rest.find('/');
        const size_t second = rest.find('/', first + 1);
        if (first == std::string::npos || second == std::string::npos)
          return std::string();
        return "https://huggingface.co/" + rest.substr(0, second) + "/resolve/main/" + rest.substr(second + 1);
      }
      return "https://huggingface.co/" + repo + "/resolve/main/" + slot;
    }

    static json fetchListing(const std::string &repo)
    {
      // A card that names no repo has no tree to ask about: the address built
      // from an empty name asks for the root of huggingface and answers a 404
      // every single time.
      if (repo.empty())
        return json();
      static std::map<std::string, json> gListings;
      static std::map<std::string, std::shared_ptr<std::mutex>> gListingGuards;
      std::shared_ptr<std::mutex> guard;
      {
        std::lock_guard<std::mutex> lock(gMutex);
        const auto found = gListings.find(repo);
        if (found != gListings.end())
          return found->second;
        const auto existing = gListingGuards.find(repo);
        if (existing != gListingGuards.end())
          guard = existing->second;
        else
        {
          guard = std::make_shared<std::mutex>();
          gListingGuards[repo] = guard;
        }
      }
      std::lock_guard<std::mutex> fetchLock(*guard);
      {
        std::lock_guard<std::mutex> lock(gMutex);
        const auto found = gListings.find(repo);
        if (found != gListings.end())
          return found->second;
      }
      std::string flat = repo;
      for (char &c : flat)
      {
        if (c == '/')
          c = '_';
      }
      const std::string listingPath = gWeightsDir + "/.hf-tree-" + flat + ".json";
      const std::string url = "https://huggingface.co/api/models/" + repo + "/tree/main?recursive=true";
      if (runCurl(url, listingPath) != 0)
        return json();
      std::string body;
      if (!readFileText(listingPath, body))
        return json();
      const json tree = json::parse(body, nullptr, false);
      if (tree.is_discarded())
      {
        // A listing that stopped halfway down cannot answer anything: a file cut
        // off there would keep saying "no such shard is in that repo" for as long
        // as it stood, so it is taken away rather than left to answer wrong.
        std::error_code broken{};
        std::filesystem::remove(listingPath, broken);
        return json();
      }
      std::lock_guard<std::mutex> lock(gMutex);
      gListings[repo] = tree;
      return tree;
    }

    // The listing a repo's file beside the weights states, kept parsed until that
    // file is written again. A model table that states a size for a form whose
    // bytes are not here reads that size out of the listing, and it asks for a row
    // of its own per form: reading the file and parsing it again on every read of
    // the table is work paid for a listing that did not change.
    static json repoListingCached(const std::string &flatRepo)
    {
      static std::mutex parsedMutex;
      static std::map<std::string, std::pair<std::string, json>> parsed;
      const std::string path = gWeightsDir + "/.hf-tree-" + flatRepo + ".json";
      std::error_code ec{};
      const auto when = std::filesystem::last_write_time(path, ec);
      if (ec)
        return json();
      const std::string stamp = std::to_string(when.time_since_epoch().count());
      {
        std::lock_guard<std::mutex> lock(parsedMutex);
        const auto found = parsed.find(path);
        if (found != parsed.end() && found->second.first == stamp)
          return found->second.second;
      }
      // the read and the parse stand outside every shared lock: the tree can be
      // long, and gMutex is what every page answer passes through
      std::string body;
      if (!readFileText(path, body))
        return json();
      json tree = json::parse(body, nullptr, false);
      if (tree.is_discarded())
        tree = json();
      std::lock_guard<std::mutex> lock(parsedMutex);
      parsed[path] = {stamp, tree};
      return tree;
    }

    static std::vector<std::string> expandShards(const std::string &repo, const std::string &slot)
    {
      if (!sharded(slot))
        return {slot};
      const json tree = fetchListing(repo);
      if (!tree.is_array())
        return {slot};
      const std::string prefix = slot.substr(0, slot.find("-00001-of-"));
      std::vector<std::string> out;
      for (const auto &entry : tree)
      {
        const std::string path = ols::statedString(entry, "path");
        if (path.rfind(prefix, 0) != 0 || path.find("-of-", prefix.size()) == std::string::npos)
          continue;
        // shards come in the clothing the weights keep: a safetensors set and a
        // gguf set are both split the same way, and a listing should find the
        // brothers of either, not only of one
        const bool dress = (path.size() >= 12 && path.compare(path.size() - 12, 12, ".safetensors") == 0) ||
                           (path.size() >= 5 && path.compare(path.size() - 5, 5, ".gguf") == 0);
        if (dress)
          out.push_back(path);
      }
      if (out.size() < 2)
        return {slot};
      std::sort(out.begin(), out.end());
      return out;
    }

    static bool safetensorsComplete(const std::string &path)
    {
      std::ifstream in{path, std::ios::binary};
      if (!in)
        return false;
      uint64_t headerSize = 0;
      for (int i = 0; i < 8; ++i)
      {
        const int byte = in.get();
        if (byte == std::ifstream::traits_type::eof())
          return false;
        headerSize |= static_cast<uint64_t>(static_cast<unsigned char>(byte)) << (i * 8);
      }
      if (headerSize == 0 || headerSize > 0x4000000ull)
        return false;
      std::string header(static_cast<size_t>(headerSize), '\0');
      in.read(header.data(), static_cast<std::streamsize>(headerSize));
      if (static_cast<size_t>(in.gcount()) != headerSize)
        return false;
      const json meta = json::parse(header, nullptr, false);
      if (meta.is_discarded() || !meta.is_object())
        return false;
      uint64_t highest = 0;
      for (const auto &entry : meta.items())
      {
        if (entry.key() == "__metadata__")
          continue;
        const json value = entry.value();
        if (!value.is_object())
          continue;
        const json offsets = value.value("data_offsets", json::array());
        if (!offsets.is_array() || offsets.size() < 2)
          continue;
        // A header that words an offset as anything but a whole number says
        // nothing about where its bytes end: this asks the question on a thread
        // serving a page, and an answer that throws takes the whole page down.
        if (!offsets[1].is_number_unsigned())
          continue;
        const uint64_t to = offsets[1].get<uint64_t>();
        if (to > highest)
          highest = to;
      }
      std::error_code measured{};
      const std::uintmax_t fileSize = std::filesystem::file_size(path, measured);
      if (measured)
        return false;
      // An offset is added to the header's own length before it is weighed
      // against the bytes on disk, so a figure past any file this machine could
      // hold is refused here: added to, one that big becomes a small one.
      constexpr uint64_t kPastAnyFile = 1ull << 48;
      if (highest > kPastAnyFile)
        return false;
      return highest + 8ull + headerSize <= static_cast<uint64_t>(fileSize);
    }

    // The same question, answered for a file that keeps its bytes in a GGUF
    // header. A quantized text encoder or transformer is shipped this way, and
    // the safetensors dialect above simply does not speak it: a card that states
    // no sum left those files unvouched, and a model whose every file was here
    // still read as missing. Measured on X1 2026-10-31: the Q4 text encoder of
    // flux-3-klein took exactly this path - read, hashed, and left standing with
    // nothing behind it, its job ending at "the address to fetch from" because
    // the header question had answered no.
    //
    // A GGUF file is: magic "GGUF", a version, counts, then metadata key-values,
    // then one record per tensor giving its offset into the data block, and then
    // the data itself, which starts at the end of the header padded up to the
    // alignment the writer uses. This file states no size of its data block and
    // names no alignment; its records' offsets are the only sentence it makes, so
    // whole is read from them: the furthest offset the header names must end
    // inside the file - the same question the safetensors check above asks, in
    // this format's own words. A tensor record's type says only how its bytes
    // are packed; nothing here needs to know.
    static bool ggufComplete(const std::string &path)
    {
      std::ifstream in{path, std::ios::binary};
      if (!in)
        return false;
      char magic[4] = {};
      in.read(magic, 4);
      if (!in || std::string(magic, 4) != "GGUF")
        return false;
      uint32_t version = 0;
      uint64_t nTensors = 0;
      uint64_t nKV = 0;
      in.read(reinterpret_cast<char *>(&version), 4);
      in.read(reinterpret_cast<char *>(&nTensors), 8);
      in.read(reinterpret_cast<char *>(&nKV), 8);
      if (!in || version < 2 || nTensors == 0)
        return false;
      // A GGUF header is little-endian on every platform; on one that does not
      // read that way, the question answers honestly that nothing was stated.
      static const bool kLittleEndianHost =
          []()
      {
        const uint32_t one = 1u;
        const unsigned char *raw = reinterpret_cast<const unsigned char *>(&one);
        return raw[0] == 1u && raw[1] == 0u && raw[2] == 0u && raw[3] == 0u;
      }();
      if (!kLittleEndianHost)
        return false;
      // The one field this file words as four bytes rather than one: the tag
      // naming a value's type. Read from Qwen3VL-4B-Instruct-Q4_K_M.gguf on X1,
      // 2026-10-31: general.architecture is followed by 08 00 00 00 (the string tag,
      // four bytes) and then a length of 7 and "qwen3vl"; qwen3vl.block_count by
      // 04 00 00 00 and the 36 blocks as a uint32; tokenizer.ggml.add_bos_token by
      // 07 00 00 00 and one byte, no; general.tags by 09 00 00 00, the element tag
      // in the same four bytes and a count of one. Reading any of these as a single
      // byte loses three bytes at every entry and answers about the wrong ones.
      // The bytes each type is carried in. GGUF's third wording adds three wide
      // types (uint64, int64, float64), and a value of one of them is a value: a
      // header that met one used to be answered as a file that was not whole.
      const uint64_t widths[13] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};
      auto readU64 = [&in](uint64_t &v)
      {
        in.read(reinterpret_cast<char *>(&v), 8);
        return static_cast<bool>(in);
      };
      auto readString = [&in, &readU64](std::string &out)
      {
        uint64_t len = 0;
        if (!readU64(len))
          return false;
        if (len > 0x100000ull)
          return false;
        out.assign(static_cast<std::string::size_type>(len), '\0');
        if (len && !in.read(out.data(), static_cast<std::streamsize>(len)))
          return false;
        return static_cast<bool>(in);
      };
      auto readTag = [&in](uint32_t &tag)
      {
        in.read(reinterpret_cast<char *>(&tag), 4);
        return static_cast<bool>(in);
      };
      // Walk a metadata value of every type this wording states; the values only
      // walk, nothing here needs what any of them says.
      std::function<bool(uint32_t)> skipValue =
          [&in, &readString, &readU64, &readTag, &widths, &skipValue](uint32_t type) -> bool
      {
        switch (type)
        {
        case 8:
        {
          std::string s;
          return readString(s);
        } // string
        case 9:
        {
          // the array's wording: the type of its elements, then how many, in the
          // same four-byte and eight-byte fields this file uses.
          uint32_t elem = 0;
          uint64_t count = 0;
          if (!readTag(elem) || !readU64(count) || count > 0x100000ull)
            return false;
          for (uint64_t i = 0; i < count; ++i)
          {
            if (!skipValue(elem))
              return false;
          }
          return true;
        }
        default:
        {
          if (type > 12)
            return false;
          const uint64_t w = widths[type];
          if (w == 0)
            return false;
          in.ignore(static_cast<std::streamsize>(w));
          return static_cast<bool>(in);
        }
        }
      };
      // The metadata keys and the counts of every array are read as they are
      // written; the values only walk. One of them is read for what it says: the
      // bytes of a file begin at the end of its header padded up to the alignment
      // the file names, and a file that names none stands on 32.
      uint64_t alignment = 32;
      for (uint64_t i = 0; i < nKV; ++i)
      {
        std::string key;
        if (!readString(key))
          return false;
        uint32_t type = 0;
        if (!readTag(type))
          return false;
        if (key == "general.alignment" && type == 4)
        {
          uint32_t stated = 0;
          in.read(reinterpret_cast<char *>(&stated), 4);
          if (!in || stated == 0)
            return false;
          alignment = stated;
          continue;
        }
        if (!skipValue(type))
          return false;
      }
      // The tensor records answer one question here: where the furthest bytes
      // the header names would begin. Shapes and packing are not needed for it;
      // the type field is the same four-byte wording the metadata tags use.
      uint64_t highestOffset = 0;
      for (uint64_t i = 0; i < nTensors; ++i)
      {
        std::string name;
        if (!readString(name))
          return false;
        uint32_t nDims = 0;
        in.read(reinterpret_cast<char *>(&nDims), 4);
        if (!in || nDims == 0 || nDims > 16)
          return false;
        for (uint32_t d = 0; d < nDims; ++d)
        {
          uint64_t dim = 0;
          in.read(reinterpret_cast<char *>(&dim), 8);
          if (!in)
            return false;
        }
        uint32_t ttype = 0;
        uint64_t offset = 0;
        in.read(reinterpret_cast<char *>(&ttype), 4);
        in.read(reinterpret_cast<char *>(&offset), 8);
        if (!in)
          return false;
        if (offset > highestOffset)
          highestOffset = offset;
      }
      if (!in)
        return false;
      const uint64_t headerEnd = static_cast<uint64_t>(in.tellg());
      std::error_code ec{};
      const auto fileSize = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
      if (ec || headerEnd == 0 || fileSize <= headerEnd)
        return false;
      // The bytes of the file stand after the header, padded up to the alignment
      // the file itself named above (32 when it named none). Reading a file that
      // stands on 64 or 418 as one that stands on 32 starts the data early, and a
      // file truncated at the end still answers as whole.
      const uint64_t dataStart = (headerEnd + alignment - 1) / alignment * alignment;
      return highestOffset > 0 && dataStart + highestOffset < fileSize;
    }

    // The switches the web interface offers: kept on the server so a page reload
    // shows the same page. Nothing here is about what a model may draw: a
    // picture is shown whole, always. "cpu" names which part of the machine
    // works: the card is left to whatever else the machine is busy with, and
    // every weight and every step is worked where the threads are.
    static const std::vector<std::string> &kPrefKeys()
    {
      static const std::vector<std::string> keys = {"tiling", "sage_attn", "cpu"};
      return keys;
    }
    static bool prefKeyNamed(const std::string &key)
    {
      if (key == "model")
        return true;
      for (const std::string &want : kPrefKeys())
      {
        if (want == key)
          return true;
      }
      return false;
    }
    static std::map<std::string, std::string> gPrefs;
    static bool gPrefsLoaded = false;

    static void loadPrefs()
    {
      std::lock_guard<std::mutex> lock(gMutex);
      if (gPrefsLoaded)
        return;
      gPrefsLoaded = true;
      std::string body;
      if (readFileText(gWeightsDir + "/.ui-prefs.json", body))
      {
        const json doc = json::parse(body, nullptr, false);
        if (!doc.is_discarded() && doc.is_object())
        {
          for (const auto &entry : doc.items())
          {
            if (entry.value().is_string() && prefKeyNamed(entry.key()))
              gPrefs[entry.key()] = entry.value().get<std::string>();
          }
        }
      }
    }

    // One switch as this machine remembers it. A name nobody has turned answers
    // "off", which is what every switch on the page opens as.
    static bool prefOn(const std::string &key)
    {
      loadPrefs();
      std::lock_guard<std::mutex> lock(gMutex);
      return gPrefs.count(key) && gPrefs[key] == "1";
    }

    // Caller holds gMutex.
    static bool savePrefsLocked()
    {
      json doc = json::object();
      for (const auto &entry : gPrefs)
        doc[entry.first] = entry.second;
      return writeFileText(gWeightsDir + "/.ui-prefs.json", doc.dump());
    }

    // The weights room this module works in, asked as one thing everywhere: the
    // rows that stand proved, and the proof of a file a download has just laid
    // down, all live in that room's own records.
    static ols::room::VerifiedRoom &room()
    {
      return ols::room::roomFor(gWeightsDir);
    }

    // Is this file here, and do its bytes stand proved as they are right now?
    // Every table a page reads asks this, and it reads the room's records plus
    // the file's length and date - never its content. A weight is read once per
    // machine, when a job is about to open it: nothing a page asks costs a
    // reading of a hundred gigabytes.
    static bool provedHere(const std::string &path)
    {
      return room().proved(path);
    }

    static bool integrityOk(const std::string &path, const json &spec, const std::string &got)
    {
      std::error_code measured{};
      const std::uintmax_t onDisk = std::filesystem::file_size(path, measured);
      const long long wantSize = spec.value("size", 0LL);
      if (measured)
        return false;
      if (wantSize > 0 && wantSize != static_cast<long long>(onDisk))
      {
        return false;
      }
      const std::string want = spec.value("sha", std::string());
      if (want.empty())
      {
        // A file with no sum on its card answers with its own header, in
        // whichever dialect it keeps: safetensors and GGUF state the same
        // sentence in different shapes, and both of them say a file is whole
        // before a byte of its data is read.
        //
        // And a file that keeps neither dialect - a config, a tokenizer, the
        // vocabulary a pipeline reads beside the weights - has no header to read
        // that sentence from. Asking it whether it is a whole safetensors file
        // answers no, and a model with such a side file on its card, from a
        // source that states a sum only for the big files, then never reads as
        // installed. What such a file can be asked is its length, and that was
        // asked above.
        std::ifstream probe{path, std::ios::binary};
        if (!probe)
          return false;
        char head[9] = {};
        const std::streamsize headBytes = probe.read(head, 9).gcount();
        if (headBytes < 4)
          return false;
        if (std::string(head, 4) == "GGUF")
          return ggufComplete(path);
        uint64_t headerSize = 0;
        if (headBytes == 9)
        {
          for (int i = 0; i < 8; ++i)
            headerSize |= static_cast<uint64_t>(static_cast<unsigned char>(head[i])) << (i * 8);
        }
        // The eight bytes that open a safetensors file count its own json header,
        // which then opens with a brace. A small json file that is not weights at
        // all reads as one of these only if its first eight bytes say a length and
        // its ninth is a brace - and then the reading below answers about it
        // honestly, as about any file whose bytes are shorter than its header.
        if (headerSize >= 1 && headerSize <= 0x4000000ull && head[8] == '{')
          return safetensorsComplete(path);
        return true;
      }
      // a card states its sum in upper-case hex and some spell the kind before
      // it; the sum computed here is lower-case hex. compare what they say.
      std::string expected = lowerText(want);
      if (expected.rfind("sha256:", 0) == 0)
        expected.erase(0, 7);
      return lowerText(got) == expected;
    }

    // What a model can be taken as: the same model with some of its parts stood
    // under other files, each named by the form it carries. A form is a set of
    // slot substitutions, because for most models the one part that comes in
    // rungs is the transformer - "the autoencoder and the text encoder keep their
    // own files, since a quantized package states it does not re-quantize them" -
    // while a model whose parts are separate checkpoints can be offered in forms
    // that move either of them: MiniMax H3's denoiser and its text encoder are
    // "separate checkpoints, not settings" (unsloth's words beside those files),
    // so a form may replace the denoiser's rung or the shared text encoder. A card
    // lists these names with the sizes its own source states beside them; a
    // download or an open takes the one a person chose, and with nothing chosen it
    // takes the default named below.
    struct ModelForm
    {
      std::string label;
      // the slots this form stands files under, and the files: a name a page
      // offers, and what opening it would load.
      std::vector<std::pair<std::string, std::string>> files;
      // whether this form's files carry their denoiser math as int8-convrot
      // weights - the row's own fact, kept true of the form that stands it.
      bool int8_convrot = false;
    };
    static const std::vector<std::pair<std::string, std::vector<ModelForm>>> &
    kQuantOffers()
    {
      static const std::vector<std::pair<std::string, std::vector<ModelForm>>> rows = []()
      {
        std::vector<std::pair<std::string, std::vector<ModelForm>>> out;
        const char *kleinNames[] = {"Q3_K_M", "Q4_K_M", "Q5_K_M", "Q6_K", "Q8_0",
                                    "UD-Q2_K_XL", "UD-Q3_K_XL", "UD-Q4_K_XL", "UD-Q5_K_XL", "BF16"};
        std::vector<ModelForm> klein;
        for (const char *name : kleinNames)
        {
          klein.push_back(ModelForm{
              name,
              {{"diffusion_model",
                std::string("hf:unsloth/flux-3-image-klein-GGUF/flux-3-image-klein-") + name + ".gguf"}}});
        }
        out.push_back({"flux-3-klein", klein});
        // Z-Image Turbo comes from Comfy-Org in two precisions of the same
        // denoiser: int8_convrot, whose math rides INT8 GEMM (the engine's CUDA
        // backend answers it only from compute 7.5 up), and bf16, which answers
        // on any card that can hold the weights. Both forms are named so the
        // machine can drive the one its card carries - and say, when it cannot,
        // exactly which form to fetch instead.
        out.push_back({"z-image-turbo",
                       {ModelForm{"int8_convrot",
                                  {{"diffusion_model",
                                    "split_files/diffusion_models/z_image_turbo_int8_convrot.safetensors"}},
                                   true},
                        ModelForm{"bf16",
                                  {{"diffusion_model",
                                    "split_files/diffusion_models/z_image_turbo_bf16.safetensors"}}}}});
        // MiniMax H3 comes in forms that move one of two separate checkpoints. The
        // denoiser rungs are unsloth's own uniform ones - its table names
        // minimax_h3_fl2va_pruned-{Q2_K,Q3_K,Q4_K,Q5_0,Q6_K,Q8_0} and more, and the
        // Q4 rung of that table is spelled "Q4_K": there is no Q4_K_M transformer in
        // that repo. "Q4_K_M" there is the name of the shared text encoder, which
        // pairs with everything above the two smallest denoisers
        // ("Pair the Q2_K_M text encoder with the two smallest denoisers and the Q4_K_M
        // one with everything else"). The fourth form is the text encoder half:
        // webmp3/Sakura-MiniMax-H3-Uncensored-GGUF is an abliterated ("uncensored")
        // Qwen3-VL-32B text encoder for this same model - "Sakura-MiniMax-H3-TextEncoder-
        // Uncensored-Q4_K_M-14.05GiB.gguf", which its page says to pass with --llm
        // beside a fl2va denoiser - so it is offered as a form that replaces the
        // encoder while the denoiser stays the row's. It is not-for-all-audiences,
        // which is said on the card rather than hidden here. That page names two
        // rungs of the encoder: the Q4_K_M it exemplifies and the Q8_0 -
        // "Sakura-MiniMax-H3-TextEncoder-Uncensored-Q8_0-25.60GiB.gguf" - the rung
        // nearest the encoder's original weights, offered beside it for a machine
        // with the room to hold it.
        std::vector<ModelForm> h3;
        for (const char *rung : {"Q2_K", "Q3_K", "Q4_K", "Q5_0", "Q6_K", "Q8_0"})
        {
          const bool smallRung = std::string(rung) == "Q2_K" || std::string(rung) == "Q3_K";
          std::vector<std::pair<std::string, std::string>> form{
              {"diffusion_model",
               std::string("hf:unsloth/MiniMax-H3-GGUF/minimax_h3_fl2va_pruned-") + rung + ".gguf"}};
          if (smallRung)
          {
            form.push_back({"llm", "hf:unsloth/MiniMax-H3-GGUF/qwen3vl_32b_minimax_h3-Q2_K_M.gguf"});
          }
          h3.push_back(ModelForm{rung, form});
        }
        h3.push_back(ModelForm{"Sakura-Q4_K_M",
                               {{"llm", "hf:webmp3/Sakura-MiniMax-H3-Uncensored-GGUF/"
                                        "Sakura-MiniMax-H3-TextEncoder-Uncensored-Q4_K_M-14.05GiB.gguf"}}});
        h3.push_back(ModelForm{"Sakura-Q8_0",
                               {{"llm", "hf:webmp3/Sakura-MiniMax-H3-Uncensored-GGUF/"
                                        "Sakura-MiniMax-H3-TextEncoder-Uncensored-Q8_0-25.60GiB.gguf"}}});
        out.push_back({"minimax-h3", h3});
        return out;
      }();
      return rows;
    }

    // what a download takes when nothing is chosen: the form the model's own card
    // gives a default by - for FLUX.3 Klein the closest-to-original file, and for
    // MiniMax H3 the pairing both of its makers name: the engine's page runs a
    // Q4-class denoiser beside the qwen3vl Q4_K_M encoder, and unsloth pairs that
    // encoder "with everything else" above the two smallest denoisers.
    static const std::vector<std::pair<std::string, std::string>> &kQuantDefaults()
    {
      static const std::vector<std::pair<std::string, std::string>> rows = {
          {"flux-3-klein", "Q8_0"},
          {"minimax-h3", "Q4_K"},
      };
      return rows;
    }

    static const std::vector<ModelForm> *findOffers(const std::string &id)
    {
      for (const auto &row : kQuantOffers())
      {
        if (row.first == id)
          return &row.second;
      }
      return nullptr;
    }

    static std::string defaultQuantFor(const std::string &id)
    {
      for (const auto &row : kQuantDefaults())
      {
        if (row.first == id)
          return row.second;
      }
      return std::string();
    }

    // the form a name asks for, when that name is one this model is offered as.
    static const ModelForm *formNamed(const std::string &id, const std::string &label)
    {
      const auto offers = findOffers(id);
      if (!offers)
        return nullptr;
      for (const ModelForm &form : *offers)
      {
        if (form.label == label)
          return &form;
      }
      return nullptr;
    }

    // whether a model's file for one slot is already here and already checked. one
    // answer for every table that asks, so a card and a choice cannot disagree.
    static bool slotOnDisk(const ImageModelDef *def, const std::string &slot)
    {
      if (!def || slot.empty())
        return false;
      const std::string repo = slotRepo(slot).empty() ? def->repo : slotRepo(slot);
      for (const std::string &part : expandShards(repo, slot))
      {
        const std::string dest = ols::modelFileDir(gWeightsDir, def->id) + localNameOf(part);
        if (!ols::fileExists(dest) || !provedHere(dest))
          return false;
      }
      return true;
    }

    // the form a model is taken as: what a person last asked for, else the first
    // form whose files all stand proved here, else the package that came with the
    // model, else the default the next download would fetch. A null answer means
    // the model's own files, unchanged. Nothing is invented, so a form with no
    // files behind it is never chosen over files that are here.
    static const ModelForm *formFor(const ImageModelDef *def, const std::string &chosen)
    {
      const auto offers = findOffers(def->id);
      if (!offers)
        return nullptr;
      std::string wanted = chosen;
      if (wanted.empty())
      {
        const std::string held = [&]()
        {
          std::lock_guard<std::mutex> lock(gMutex);
          ensureChosenLoadedLocked();
          const auto found = gChosenQuant.find(def->id);
          return found == gChosenQuant.end() ? std::string() : found->second;
        }();
        wanted = held;
      }
      if (!wanted.empty())
      {
        if (const ModelForm *form = formNamed(def->id, wanted))
          return form;
      }
      // A card that cannot carry the math of the row's quantization takes the
      // first offered form it can carry - even when the row's own files stand
      // on disk: the machine drives the road that ends somewhere rather than
      // standing on the one that does not. A held choice above still wins: a
      // machine that was told what it runs stays told.
      if (def->int8_convrot)
      {
        const CardAnswer &card = cardAnswer();
        if (card.computeMajor >= 0 &&
            (card.computeMajor < 7 || (card.computeMajor == 7 && card.computeMinor < 5)))
        {
          for (const ModelForm &form : *offers)
          {
            if (form.int8_convrot)
              continue;
            bool here = true;
            for (const auto &file : form.files)
            {
              if (!slotOnDisk(def, file.second))
              {
                here = false;
                break;
              }
            }
            if (here)
              return &form;
          }
        }
      }
      bool baseHere = true;
      for (const auto &file : def->files)
      {
        if (!slotOnDisk(def, file.second))
        {
          baseHere = false;
          break;
        }
      }
      if (baseHere)
        return nullptr;
      for (const ModelForm &form : *offers)
      {
        bool here = true;
        for (const auto &file : form.files)
        {
          if (!slotOnDisk(def, file.second))
          {
            here = false;
            break;
          }
        }
        if (here)
          return &form;
      }
      const std::string fallback = defaultQuantFor(def->id);
      return fallback.empty() ? nullptr : formNamed(def->id, fallback);
    }

    // a model's files as one form stands them: the row's files with that form's
    // parts replaced. A form may name a slot the row never carried - an encoder
    // the model can be taken with where it shipped none - so that slot is added.
    static std::vector<std::pair<std::string, std::string>> filesAsForm(const ImageModelDef *def,
                                                                        const ModelForm *form)
    {
      std::vector<std::pair<std::string, std::string>> out = def->files;
      if (!form)
        return out;
      for (const auto &file : form->files)
      {
        bool named = false;
        for (auto &held : out)
        {
          if (held.first != file.first)
            continue;
          held.second = file.second;
          named = true;
          break;
        }
        if (!named)
          out.push_back(file);
      }
      return out;
    }

    // a model's files with the form it is taken as applied.
    static std::vector<std::pair<std::string, std::string>> modelFiles(const ImageModelDef *def,
                                                                       const std::string &chosen)
    {
      return filesAsForm(def, formFor(def, chosen));
    }

    // every file of one set of files stands proved here.
    static bool filesProved(const ImageModelDef *def,
                            const std::vector<std::pair<std::string, std::string>> &files)
    {
      for (const auto &file : files)
      {
        const std::string repo = slotRepo(file.second).empty() ? def->repo : slotRepo(file.second);
        for (const std::string &part : expandShards(repo, file.second))
        {
          const std::string dest = ols::modelFileDir(gWeightsDir, def->id) + localNameOf(part);
          if (!ols::fileExists(dest) || !provedHere(dest))
            return false;
        }
      }
      return true;
    }

    // whether a form of this model stands proved here: its files, each one, or -
    // for a model offered in forms - the files of any one form. The choice is
    // which form, not whether the model is usable, so one form here is enough to
    // call the model here.
    static bool modelInstalled(const std::string &id)
    {
      const ImageModelDef *def = findModel(id);
      if (!def || def->files.empty())
        return false;
      if (const auto offers = findOffers(id))
      {
        for (const ModelForm &form : *offers)
        {
          if (filesProved(def, filesAsForm(def, &form)))
            return true;
        }
      }
      return filesProved(def, def->files);
    }

    static std::string gpuDeviceName()
    {
      size_t needed = sd_list_devices(nullptr, 0);
      if (needed == 0)
        return std::string();
      std::vector<char> buffer(needed + 1, 0);
      sd_list_devices(buffer.data(), buffer.size());
      std::string fallback;
      const std::string text{buffer.data()};
      std::istringstream lines(text);
      std::string line;
      while (std::getline(lines, line))
      {
        const size_t tab = line.find('\t');
        const std::string name = tab == std::string::npos ? line : line.substr(0, tab);
        if (name.empty() || lowerText(name).rfind("cpu", 0) == 0)
          continue;
        if (lowerText(name).rfind("hip", 0) == 0 || lowerText(name).rfind("vulkan", 0) == 0)
          return name;
        if (fallback.empty())
          fallback = name;
      }
      return fallback;
    }

    // what a model's files weigh, shard for shard: a set that came apart on the
    // way to this machine costs every piece of it, not just the first.
    static long long weightsBytesOf(const std::string &modelId, const std::string &form)
    {
      const ImageModelDef *def = findModel(modelId);
      long long bytes = 0;
      if (!def)
        return 0;
      for (const auto &file : modelFiles(def, form))
      {
        const std::string repo = slotRepo(file.second).empty() ? def->repo : slotRepo(file.second);
        for (const std::string &part : expandShards(repo, file.second))
        {
          const std::string path = ols::modelFilePath(gWeightsDir, modelId, localNameOf(part));
          std::error_code ec{};
          const auto size = std::filesystem::file_size(path, ec);
          if (!ec)
            bytes += static_cast<long long>(size);
        }
      }
      return bytes;
    }

    // Caller holds gCtxMutex, and holds no lock that weightsBytesOf or the form
    // tables need: the bytes and the form are counted before this, never inside
    // it, because this stamp lands on a path that already holds gCtxMutex and
    // gMutex must only ever be taken before it, never under it.
    static void touchContext(const std::string &modelId, const std::string &form, long long bytes)
    {
      ++gUseTick;
      gCtxUse[modelId] = gUseTick;
      gCtxBytes[modelId] = bytes;
      gCtxForm[modelId] = form;
    }

    // Caller holds gCtxMutex. The idle model whose weights were used least recently
    // and that is not generating; empty when nothing can be closed.
    static std::string lruVictimLocked(const std::string &keep)
    {
      std::string victim;
      uint64_t oldest = 0;
      for (const auto &entry : gCtx)
      {
        if (entry.first == keep || busyModel(entry.first))
          continue;
        const uint64_t use = gCtxUse.count(entry.first) ? gCtxUse[entry.first] : 0;
        if (victim.empty() || use < oldest)
        {
          oldest = use;
          victim = entry.first;
        }
      }
      return victim;
    }

    // Caller holds gCtxMutex. After a load, close idle models the least recently
    // used first until the resident weights fit the memory budget. Closing before a
    // load leaves the engine short of heap, so this runs only once the new context
    // exists.
    static void trimOverBudgetLocked(const std::string &keep)
    {
      long long resident = 0;
      for (const auto &entry : gCtxBytes)
        resident += entry.second;
      const long long budget = memoryBudgetBytes();
      if (budget <= 0)
        return;
      while (resident > budget)
      {
        const std::string victim = lruVictimLocked(keep);
        if (victim.empty())
          return;
        const long long freed = gCtxBytes.count(victim) ? gCtxBytes[victim] : 0;
        closeContextLocked(victim);
        resident -= freed;
      }
    }

    static void closeContextLocked(const std::string &modelId)
    {
      const auto found = gCtx.find(modelId);
      if (found == gCtx.end())
        return;
      free_sd_ctx(found->second);
      gCtx.erase(found);
      gCtxUse.erase(modelId);
      gCtxBytes.erase(modelId);
      gCtxCpu.erase(modelId);
      gCtxForm.erase(modelId);
      gPendingErase.erase(modelId);
    }

    // The engine's own last complaint, kept where a failed run can speak it.
    // Runs are made one at a time, so one slot tells the truth for the run in
    // front of it; the log callback fills it as the engine talks.
    static std::mutex gSdWordsMutex;
    static std::string gSdWords;

    // Whole bytes of device memory the card states, or 0 when no card answers.
    static long long cudaDeviceTotalBytes()
    {
      return cardAnswer().totalBytes;
    }

    static std::string currentChosenQuant(const std::string &modelId)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      ensureChosenLoadedLocked();
      const auto found = gChosenQuant.find(modelId);
      return found == gChosenQuant.end() ? std::string() : found->second;
    }

    // Open a model, or take the one already standing. openedForm and openedBytes
    // come back from here telling what the weights really stood as and weighed:
    // resolved before the context lock, because the tables they read stand behind
    // the request lock, and gMutex may only ever be taken before gCtxMutex, never
    // under it - the page answers take them in that order and an open taking them
    // the other way is a deadlock waiting for its two halves.
    static sd_ctx_t *openContext(const std::string &modelId, bool sage, bool cpu,
                                 std::string &error, std::string *openedFormOut = nullptr,
                                 long long *openedBytesOut = nullptr)
    {
      const ImageModelDef *def = findModel(modelId);
      if (!def)
      {
        error = "that model is not one this machine knows: " + modelId;
        return nullptr;
      }
      if (openedFormOut)
        openedFormOut->clear();
      if (openedBytesOut)
        *openedBytesOut = 0;
      const std::string chosen = currentChosenQuant(modelId);
      const std::string base = ols::modelFileDir(gWeightsDir, modelId);
      // the form is resolved once, out here: what the ask named when it names a
      // form, else what the files standing here answer with.
      const ModelForm *form = formFor(def, chosen);
      std::string openedForm = form ? form->label : std::string();
      if (!form)
      {
        // the row's own files are, for a model whose default form is the file the
        // row already names, one of the forms it is offered as - say that form,
        // the same words the model card uses, so the form a run is filed under
        // and the form the card names are never two answers to one question.
        const auto offers = findOffers(modelId);
        if (offers)
        {
          for (const ModelForm &offer : *offers)
          {
            if (filesAsForm(def, &offer) == def->files)
            {
              openedForm = offer.label;
              break;
            }
          }
        }
      }
      const long long openedBytes = weightsBytesOf(modelId, chosen);
      sd_ctx_params_t params;
      sd_ctx_params_init(&params);
      const std::vector<std::pair<std::string, std::string>> wantedFiles = filesAsForm(def, form);
      // every file a slot stands under has to be here before the engine is asked
      // for anything - a shard set is more files than its name says - and the
      // engine is handed the first of them, which is the one it reads the rest
      // from.
      std::vector<std::string> paths;
      paths.reserve(wantedFiles.size());
      for (const auto &file : wantedFiles)
      {
        const std::string repo = slotRepo(file.second).empty() ? def->repo : slotRepo(file.second);
        const std::vector<std::string> parts = expandShards(repo, file.second);
        for (const std::string &part : parts)
        {
          if (!ols::fileExists(base + localNameOf(part)))
          {
            error = "the file for " + modelId + " (" + localNameOf(part) + ") is not here";
            return nullptr;
          }
        }
        paths.push_back(base + localNameOf(parts.front()));
      }
      for (size_t i = 0; i < wantedFiles.size(); ++i)
      {
        const std::string slot = wantedFiles[i].first;
        const char *path = paths[i].c_str();
        bool handedOn = true;
        if (slot == "model")
          params.model_path = path;
        else if (slot == "clip_l")
          params.clip_l_path = path;
        else if (slot == "clip_g")
          params.clip_g_path = path;
        else if (slot == "t5xxl")
          params.t5xxl_path = path;
        else if (slot == "diffusion_model")
          params.diffusion_model_path = path;
        else if (slot == "vae")
          params.vae_path = path;
        else if (slot == "llm")
          params.llm_path = path;
        else if (slot == "llm_vision")
          params.llm_vision_path = path;
        // the audio half of a joint video+audio model: the engine's page passes
        // it as its own component and says a run without it "produces video
        // without a decoded audio track" (docs/minimax_h3.md).
        else if (slot == "audio_vae")
          params.audio_vae_path = path;
        else
          handedOn = false;
        // a table naming a part the engine has no door for would otherwise load
        // the model short one weight and never say so.
        if (!handedOn)
        {
          std::fprintf(stderr, "[image] %s names a part this machine cannot hand on: %s\n",
                       modelId.c_str(), slot.c_str());
          std::fflush(stderr);
        }
      }
      params.n_threads = gThreads;
      params.enable_mmap = true;
      params.sage_attn = sage;
      // both pages that name how these weights are run take the noise from the
      // cpu ("--rng cpu" - docs/minimax_h3.md and the weight maker's own usage
      // page), where the engine's standing answer is the card's generator.
      if (modelId == "minimax-h3")
        params.rng_type = CPU_RNG;
      if (!def->wtype.empty())
        params.wtype = str_to_sd_type(def->wtype.c_str());
      // On some GPUs the engine's flash-attention kernel has no device code for
      // that card (it aborts the whole process, which took the server down
      // mid-suite on the compute-7.0 box). The engine's own probe cannot see
      // this, so the card is asked before the model is opened and flash-attention
      // is turned off here. The rule is held to the cards the abort was ever seen
      // on: the weight maker's own run of MiniMax H3 carries --diffusion-fa on
      // ordinary cards, and 2026-11-19 a compute-7.5 card (RTX 2080) held by a
      // blanket under-8.0 rule ran a grainy clip without it while the same
      // settings on a card the rule did not touch ran clean. The card of this
      // machine does not change while the server stands, so it is asked once.
      static const CardAnswer &kCard = cardAnswer();
      if (kCard.computeMajor >= 0 &&
          (kCard.computeMajor < 7 || (kCard.computeMajor == 7 && kCard.computeMinor < 5)))
      {
        static const std::string kFlashOff = "off";
        params.flash_attn_mode = kFlashOff.c_str();
        std::fprintf(stderr, "[image] cuda device is compute %d.%d: flash attention was not built for it, so it is off\n",
                     kCard.computeMajor, kCard.computeMinor);
        std::fflush(stderr);
      }
      // The card of this machine, named in the engine's words, is what a model
      // opens on; "cpu" is a name the engine resolves as well, and every weight
      // and every step then works where the threads are. A library with no card
      // in front of it offers the cpu and nothing else - the engine's own plan
      // for that case is the default backend too - so a run asks the cpu and
      // the log says so once, rather than every run refusing over a card no
      // one ever asked for.
      const std::string card = cpu ? std::string() : gpuDeviceName();
      const bool onCpu = cpu || card.empty();
      if (card.empty())
      {
        static std::once_flag loggedOnce;
        std::call_once(loggedOnce, []
                       {
          std::fprintf(stderr, "[image] no card answers on this machine: image models open on the cpu\n");
          std::fflush(stderr); });
      }
      const std::string device = onCpu ? std::string("cpu") : card;
      params.backend = device.c_str();
      // The denoiser math of an int8-convrot form rides INT8 GEMM, which this
      // engine's CUDA backend answers only from compute 7.5 up (the turing gate
      // in its ggml): below that the engine places every matmul of the run on
      // the cpu, and a picture that costs seconds on a Turing card crawls with
      // the card idle - seen on a compute-7.0 machine on 2026-11-27, where an
      // eight-step run burned 42 minutes of cpu across its 26-minute life, the
      // card at 0 percent and the journal silent from the first graph line,
      // until a person stopped it. The form resolution above already steers a
      // card below that floor onto a form it can carry; a refusal arrives only
      // when no such form is on disk (or a held choice insists), and it names
      // the form the weights room can fetch.
      const bool runConvrot = form ? form->int8_convrot : def->int8_convrot;
      if (runConvrot && !onCpu && kCard.computeMajor >= 0 &&
          (kCard.computeMajor < 7 || (kCard.computeMajor == 7 && kCard.computeMinor < 5)))
      {
        std::string carryable;
        if (const auto offers = findOffers(def->id))
        {
          for (const ModelForm &offer : *offers)
          {
            if (offer.int8_convrot)
              continue;
            if (!carryable.empty())
              carryable += ", ";
            carryable += offer.label;
          }
        }
        std::fprintf(stderr, "[image] %s: card compute %d.%d cannot carry int8-convrot math; the run is refused before opening\n",
                     def->id.c_str(), kCard.computeMajor, kCard.computeMinor);
        std::fflush(stderr);
        error = def->name + " would open as int8-convrot weights, whose math needs a compute-7.5 card or newer; "
                            "this card is compute " +
                std::to_string(kCard.computeMajor) + "." + std::to_string(kCard.computeMinor) +
                (carryable.empty()
                     ? std::string(". No other form of these weights is known here; the cpu road remains for an ask "
                                   "made knowingly.")
                     : std::string(" - but this card carries them as ") + carryable +
                           ", which the weights room offers to fetch. The cpu road remains for an ask made knowingly.");
        return nullptr;
      }
      // A clip's weights ask the card first like every other model: the steps
      // and the weights standing on the same part of the machine is what makes
      // the steps fast, and one machine's card is another machine's whole
      // memory. A card that runs out of room answers the open with a refusal,
      // and only then does the run fall back to the engine's streaming recipe -
      // weights on the cpu, the card holding the working set - which is what a
      // small card has to be asked with.
      const bool videoWantsCard = def->video && !onCpu;
      params.params_backend = device.c_str();
      params.auto_fit = true;

      std::shared_ptr<std::mutex> guard;
      {
        std::lock_guard<std::mutex> lock(gCtxMutex);
        const auto standing = gCtx.find(modelId);
        if (standing != gCtx.end())
        {
          const bool standsOn = gCtxCpu.count(modelId) ? gCtxCpu[modelId] : false;
          const auto opened = gCtxForm.find(modelId);
          const std::string standsAs = opened == gCtxForm.end() ? std::string() : opened->second;
          if (standsOn != onCpu || standsAs != openedForm)
          {
            std::fprintf(stderr, "[image] %s stands open on %s and as %s, and this asks for %s and as %s: the weights are moved\n",
                         modelId.c_str(),
                         standsOn ? "cpu" : "card",
                         standsAs.empty() ? "its default" : standsAs.c_str(),
                         onCpu ? "cpu" : "card",
                         openedForm.empty() ? "its default" : openedForm.c_str());
            std::fflush(stderr);
            closeContextLocked(modelId);
          }
        }
      }
      {
        std::lock_guard<std::mutex> lock(gCtxMutex);
        const auto found = gCtx.find(modelId);
        if (found != gCtx.end())
        {
          touchContext(modelId, openedForm, openedBytes);
          // Room is asked for on reuse too, never only at an open: a model
          // that stands open answers from here, and the neighbour whose
          // weights moved onto the card since it last drew would else stand
          // there unevicted - a juggernaut then sampling segments beside flux
          // weights took three times as long, and beside a second sampler,
          // it failed outright.
          trimOverBudgetLocked(modelId);
          if (openedFormOut)
            *openedFormOut = openedForm;
          if (openedBytesOut)
            *openedBytesOut = openedBytes;
          return found->second;
        }
        const auto existing = gLoadGuards.find(modelId);
        if (existing != gLoadGuards.end())
          guard = existing->second;
        else
        {
          guard = std::make_shared<std::mutex>();
          gLoadGuards[modelId] = guard;
        }
      }
      std::lock_guard<std::mutex> loadLock(*guard);
      {
        std::lock_guard<std::mutex> lock(gCtxMutex);
        const auto found = gCtx.find(modelId);
        if (found != gCtx.end())
        {
          touchContext(modelId, openedForm, openedBytes);
          trimOverBudgetLocked(modelId);
          if (openedFormOut)
            *openedFormOut = openedForm;
          if (openedBytesOut)
            *openedBytesOut = openedBytes;
          return found->second;
        }
      }
      static std::once_flag sdLogOnce;
      std::call_once(sdLogOnce, []()
                     { sd_set_log_callback(
                           [](sd_log_level_t level, const char *text, void *)
                           {
                             const bool bad = level == SD_LOG_ERROR || level == SD_LOG_WARN;
                             if (bad && text)
                             {
                               std::lock_guard<std::mutex> lock(gSdWordsMutex);
                               gSdWords = text;
                               const auto cut = gSdWords.find(" --- ");
                               if (cut != std::string::npos)
                                 gSdWords.erase(cut);
                               while (!gSdWords.empty() && (gSdWords.back() == '\n' || gSdWords.back() == '\r'))
                                 gSdWords.pop_back();
                             }
                             std::cerr << "[sd " << (level == SD_LOG_DEBUG ? "debug" : level == SD_LOG_INFO ? "info"
                                                                                   : level == SD_LOG_WARN   ? "warn"
                                                                                                            : "error")
                                       << "] " << text;
                           },
                           nullptr); });
      static const std::string kVramBudget = []()
      {
        // The engine cuts its graphs and settles weights against this budget;
        // without it the manager asks the card for the whole run at once and a
        // big clip dies where a spill to memory would have served. Four GiB
        // stay outside the budget for what the engine does not track.
        const long long total = cudaDeviceTotalBytes();
        if (total <= 0)
          return std::string();
        long long gib = (total - 4LL * 1024 * 1024 * 1024) / (1024LL * 1024 * 1024);
        if (gib < 4)
          gib = 4;
        // Measured on MSI 2026-11-19: an 8 GiB card under this rule was handed
        // four GiB, and the manager counts the runtime buffers it prepares in
        // its every ask - one segment of a twelve-frame clip wanted 4341 MB and
        // was refused against a 3904 MB budget while the card itself stood
        // 6689 MB free. Such a card carries six whole GiB of the clip with the
        // driver's own share left standing outside the budget; where the
        // subtraction would leave the floor at four, six is what the card has.
        if (gib <= 4 && total >= 7500LL * 1024 * 1024)
          gib = 6;
        return std::to_string(gib);
      }();
      params.max_vram = kVramBudget.empty() ? nullptr : kVramBudget.c_str();
      sd_ctx_t *ctx = new_sd_ctx(&params);
      bool freed = false;
      bool streamed = false;
      while (!ctx)
      {
        std::string victim;
        {
          std::lock_guard<std::mutex> lock(gCtxMutex);
          victim = lruVictimLocked(modelId);
          if (!victim.empty())
            closeContextLocked(victim);
        }
        if (!victim.empty())
        {
          freed = true;
          ctx = new_sd_ctx(&params);
          continue;
        }
        // Nothing else stands in the way and the card still refused: a clip is
        // where the streaming recipe earns its keep, so ask that once before
        // saying the machine cannot hold the model at all.
        if (!streamed && videoWantsCard)
        {
          streamed = true;
          params.params_backend = "*=cpu";
          std::fprintf(stderr, "[image] %s does not stand on the card whole: its weights will stream from the cpu\n",
                       modelId.c_str());
          std::fflush(stderr);
          ctx = new_sd_ctx(&params);
          continue;
        }
        break;
      }
      if (!ctx)
      {
        error = freed ? "No room for " + modelId + " on this machine: closing the loaded models did not free enough"
                      : "This machine cannot hold " + modelId + " even with nothing else loaded";
        return nullptr;
      }
      {
        std::lock_guard<std::mutex> lock(gCtxMutex);
        gCtx[modelId] = ctx;
        gCtxCpu[modelId] = onCpu;
        touchContext(modelId, openedForm, openedBytes);
        // room is made after the new weights stand, never before: closing an
        // idle neighbour that pushed the room past its ceiling while the new
        // model opened would leave the engine short of heap at the one moment
        // it needs the most.
        trimOverBudgetLocked(modelId);
      }
      if (openedFormOut)
        *openedFormOut = openedForm;
      if (openedBytesOut)
        *openedBytesOut = openedBytes;
      return ctx;
    }

    static bool doDownload(const std::string &job, const ImageModelDef *def)
    {
      const std::string base = ols::modelFileDir(gWeightsDir, def->id);
      std::vector<std::string> slots;
      for (const auto &file : modelFiles(def, std::string()))
        slots.push_back(file.second);
      int done = 0;
      const int total = static_cast<int>(slots.size());
      for (const std::string &slot : slots)
      {
        if (cancelledRequested(job))
        {
          markState(job, ols::kStateCancelled);
          return false;
        }
        const std::string repo = slotRepo(slot).empty() ? def->repo : slotRepo(slot);
        const std::vector<std::string> parts = expandShards(repo, slot);
        for (const std::string &part : parts)
        {
          const std::string local = localNameOf(part);
          const std::string dest = base + local;
          std::error_code ec{};
          std::filesystem::create_directories(std::filesystem::path(dest).parent_path(), ec);
          if (provedHere(dest))
            continue;
          json spec = json::object();
          const json tree = fetchListing(repo);
          if (tree.is_array())
          {
            const std::string treePath = treePathOf(part);
            for (const auto &entry : tree)
            {
              if (ols::statedString(entry, "path") == treePath)
              {
                spec["size"] = ols::statedCount(entry, "size");
                spec["sha"] = ols::statedString(entry, "oid");
                const json lfs = entry.value("lfs", json::object());
                if (lfs.is_object())
                {
                  // the block that ships a file states its size and its hash; a
                  // plain listing entry states neither, and what the listing
                  // already gave stays.
                  const long long shipped = ols::statedCount(lfs, "size");
                  if (shipped > 0)
                    spec["size"] = shipped;
                  const std::string shippedSha = ols::statedString(lfs, "oid");
                  if (!shippedSha.empty())
                    spec["sha"] = shippedSha;
                }
                break;
              }
            }
          }
          const std::string cardWhole = spec.value("sha", std::string());
          std::string got;
          std::string url;
          int transferred = 0;
          std::error_code seen{};
          // One pass over a file that is already here answers both questions at
          // once: whether the sum its card states answers to these bytes, and the
          // rows every later answer is read from. A weight is read once on a
          // machine, and never again while its bytes stand.
          if (std::filesystem::exists(dest, seen) &&
              room().prove(dest, cardWhole, &got) && integrityOk(dest, spec, got))
          {
            // The voucher a fetched file is given two pages below belongs here too.
            // A card that states no sum leaves the file's own header to say it is
            // whole, and without that word stood under them these rows sit with
            // nothing behind them: proved() says no, modelInstalled says no, and the
            // file is reported as weights not downloaded while it occupies the disk.
            // Measured on X1 2026-10-28: 35 of the 55 rows in .verified.json had been
            // written this way - read, hashed, and left unvouched - so a model whose
            // every file was here read on the picture page as missing, and with it
            // every quant of a model offered in quants, since a quant is asked the
            // same question.
            if (cardWhole.empty())
              room().vouch(dest, ols::room::kVoucherWhole());
            continue;
          }
          url = downloadUrl(repo, part, def->civitaiVersion);
          if (url.empty())
          {
            noteError(job,
                      "the address to fetch this file from is not one this machine can read");
            markState(job, ols::kStateError);
            return false;
          }
          // Rounds, not one long ask: see kFetchSliceSeconds. What arrived stays
          // where it arrived, since the next round - today or after a restart -
          // resumes from those bytes.
          const std::function<bool()> stopAskedNow = [job]()
          { return cancelledRequested(job); };
          // each round of the fetch stands its numbers on the job's record -
          // the bytes on the disk, the size the card states, the rate of the
          // round just ended and the average over this fetch - because a page
          // watching the bar has nothing else to read them from, and "download"
          // alone is no answer while twenty-five gigabytes arrive.
          const long long totalBytes = spec.value("size", 0LL);
          const long long fromBytes = ols::fileSize(dest);
          const long long beganFetch = ols::steadyMs();
          const std::function<void(long long, double)> told =
              [job, totalBytes, fromBytes, beganFetch, local](long long bytes, double speed)
          {
            {
              std::lock_guard<std::mutex> lock(gMutex);
              json &record = gJobs[job];
              record["bytes"] = bytes;
              if (totalBytes > 0)
                record["total_bytes"] = totalBytes;
              record["speed"] = speed;
              const long long took = ols::steadyMs() - beganFetch;
              record["avg"] = took > 0
                                  ? double(bytes > fromBytes ? bytes - fromBytes : 0) * 1000.0 / double(took)
                                  : 0.0;
              record["file"] = local;
            }
            pushEvent(job, json{{"type", "fetch"},
                                {"bytes", bytes},
                                {"total", totalBytes},
                                {"speed", speed}}
                               .dump());
          };
          int fetchAnswer = 0;
          const bool cameWhole = fetchWhole(url, dest, totalBytes, stopAskedNow, fetchAnswer, told);
          transferred = fetchAnswer;
          if (!cameWhole && cancelledRequested(job))
          {
            noteStep(job, ols::fileExists(dest)
                              ? "the fetch was stopped; what is down stands where it is and the "
                                "next fetch continues from that byte"
                              : "the fetch was stopped before anything of it arrived");
            markState(job, ols::kStateCancelled);
            return false;
          }
          std::string fetched;
          const bool answered = room().prove(dest, cardWhole, &fetched);
          if (!answered && !cardWhole.empty())
          {
            noteError(job, "the file that came down does not match the sum its card states");
            markState(job, ols::kStateError);
            return false;
          }
          if (fetched.empty())
          {
            noteError(job, "this machine could not read the file that came down");
            markState(job, ols::kStateError);
            return false;
          }
          if (!integrityOk(dest, spec, fetched))
          {
            noteError(job, transferred == -1
                               ? "this machine would not ask for that file: its name or its address "
                                 "holds characters a command line cannot carry"
                           : transferred != 0 ? "the fetch did not finish for " + url
                                              : "the file that came down is not whole");
            markState(job, ols::kStateError);
            return false;
          }
          // A card that states no sum leaves the file's own header to say it is
          // whole, and that is the voucher the rows stand on - written down as
          // what it is, rather than as a sum nobody read.
          if (cardWhole.empty())
            room().vouch(dest, ols::room::kVoucherWhole());
        }
        ++done;
        noteProgress(job, done, total);
      }
      markState(job, ols::kStateDone);
      return true;
    }

    static void doLoad(const std::string &job, const std::string &modelId)
    {
      const ImageModelDef *def = findModel(modelId);
      if (!def)
      {
        noteError(job, "that model is not one this machine knows");
        markState(job, ols::kStateError);
        return;
      }
      if (!modelInstalled(modelId))
      {
        if (!doDownload(job, def))
          return;
      }
      // an open cannot be taken back: a stop asked for now is honoured by
      // stopping here, before the weights are moved onto a device
      if (cancelledRequested(job))
      {
        markState(job, ols::kStateCancelled);
        return;
      }
      std::string error;
      std::string openedForm;
      // The load hand opens the model the way the page's switches name it, so a
      // model loaded by hand stands on the same device the next run would put it
      // on - and a run that names another of them moves the weights. Standing
      // weights on the device is the device's work: this waits its turn on the
      // card like every other hand that touches it.
      sd_ctx_t *ctx = nullptr;
      {
        std::lock_guard<std::mutex> device(ols::deviceCard());
        ctx = openContext(modelId, prefOn("sage_attn"), prefOn("cpu"), error, &openedForm);
      }
      if (!ctx)
      {
        noteError(job, error);
        markState(job, ols::kStateError);
        return;
      }
      if (cancelledRequested(job))
      {
        // the weights stand now; pulling them back out from under a run that may
        // already be working with them costs more than the stop is worth, so the
        // load says it stopped and says what it leaves standing.
        noteStep(job, "the model was open before the stop was asked for; it stands open");
        markState(job, ols::kStateCancelled);
        return;
      }
      {
        // the timing row for an open is keyed by the precision the model was
        // really opened as, not by what the page had asked for at best
        std::lock_guard<std::mutex> lock(gMutex);
        if (!openedForm.empty())
          gJobs[job]["quant"] = openedForm;
      }
      markState(job, ols::kStateDone);
    }

    // What kind of file an output's own file is, and the endings that say so. A
    // picture stands as one PNG. A clip stands as an MJPEG AVI - the frames with
    // the sound muxed in -, beside four parts named from the clip with a word that
    // says what each is: ".raw.avi" for the same frames with no codec to know,
    // ".poster.png" for the still first frame, ".preview.apng" for the small
    // animated copy, ".wav" for the sound on its own. Those parts are not outputs
    // of their own, so nothing here lists them as gallery items and every removal
    // that drops a clip drops them with it.
    static const std::vector<std::string> kOutputEndings{".png", ".avi"};

    static bool endsWithText(const std::string &text, const std::string &ending)
    {
      return text.size() >= ending.size() &&
             text.compare(text.size() - ending.size(), ending.size(), ending) == 0;
    }

    // whether a file in the outputs folder is an output's own file rather than a
    // part of one. The poster is the only part that carries a picture's ending,
    // and the word before it says what it is.
    static bool outputItemNamed(const std::string &name)
    {
      if (endsWithText(name, ".poster.png") || endsWithText(name, ".preview.apng") ||
          endsWithText(name, ".wav") || endsWithText(name, ".raw.avi"))
        return false;
      for (const std::string &ending : kOutputEndings)
      {
        if (endsWithText(name, ending))
          return true;
      }
      return false;
    }

    // every file one output is stood as, given the name of its own file: the file
    // itself, the record that hangs off it, and the parts of a clip.
    static std::vector<std::string> outputArtifactNames(const std::string &itemName)
    {
      std::vector<std::string> names{itemName, itemName + ".json"};
      for (const std::string &ending : kOutputEndings)
      {
        if (!endsWithText(itemName, ending))
          continue;
        if (ending == ".avi")
        {
          const std::string base = itemName.substr(0, itemName.size() - ending.size());
          names.push_back(base + ".poster.png");
          names.push_back(base + ".preview.apng");
          names.push_back(base + ".wav");
          names.push_back(base + ".raw.avi");
        }
        break;
      }
      return names;
    }

    static void removeOutputArtifacts(const std::string &itemName)
    {
      std::error_code ec{};
      for (const std::string &name : outputArtifactNames(itemName))
        std::filesystem::remove(gOutputDir + "/" + name, ec);
    }

    static void pruneOutputs()
    {
      std::error_code ec{};
      if (!std::filesystem::exists(gOutputDir, ec))
        return;
      std::vector<std::pair<std::filesystem::file_time_type, std::string>> files;
      for (const auto &entry : std::filesystem::directory_iterator(gOutputDir, ec))
      {
        const std::string name = entry.path().filename().string();
        if (!outputItemNamed(name))
          continue;
        files.push_back({std::filesystem::last_write_time(entry.path(), ec), name});
      }
      if (files.size() <= static_cast<size_t>(gKeptMax) + static_cast<size_t>(kPruneSlack))
        return;
      std::sort(files.begin(), files.end(),
                [](const std::pair<std::filesystem::file_time_type, std::string> &a,
                   const std::pair<std::filesystem::file_time_type, std::string> &b)
                {
                  return a.first > b.first;
                });
      for (size_t i = static_cast<size_t>(gKeptMax); i < files.size(); ++i)
        removeOutputArtifacts(files[i].second);
    }

    // Kept pictures are a small working set: the newest that many stay, the rest are
    // dropped so the folder cannot grow without bound. The number is also what the
    // page offers a person to keep in front of them, so the strip and the room
    // agree on one depth.
    static const int kKeptRefs = 24;
    static void pruneRefs()
    {
      std::error_code ec{};
      if (!std::filesystem::exists(gRefsDir, ec))
        return;
      std::vector<std::pair<std::filesystem::file_time_type, std::string>> files;
      for (const auto &entry : std::filesystem::directory_iterator(gRefsDir, ec))
      {
        const std::string name = entry.path().filename().string();
        if (name.size() < 5 || name.compare(name.size() - 5, 5, ".json") != 0)
          continue;
        files.push_back({std::filesystem::last_write_time(entry.path(), ec),
                         name.substr(0, name.size() - 5)});
      }
      std::sort(files.begin(), files.end(),
                [](const std::pair<std::filesystem::file_time_type, std::string> &a,
                   const std::pair<std::filesystem::file_time_type, std::string> &b)
                {
                  return a.first > b.first;
                });
      for (size_t i = static_cast<size_t>(kKeptRefs); i < files.size(); ++i)
      {
        // The collected name is the record name without ".json"; a record's
        // picture keeps its real extension, so this name is the picture and
        // name + ".json" is the record. Remove both together.
        std::filesystem::remove(gRefsDir + "/" + files[i].second + ".json", ec);
        std::filesystem::remove(gRefsDir + "/" + files[i].second, ec);
      }
    }

    // A reference picture for a render: a kept reference, a render from the gallery,
    // or any image file on this machine.
    static std::string resolveReference(const std::string &source)
    {
      if (source.empty())
        return std::string();
      std::error_code ec{};
      for (const auto &ext : {".png", ".jpg", ".jpeg", ".webp"})
      {
        const std::string candidate = gRefsDir + "/" + source + ext;
        if (std::filesystem::exists(candidate, ec))
          return candidate;
        const std::string output = gOutputDir + "/" + source + ext;
        if (std::filesystem::exists(output, ec))
          return output;
      }
      // a clip cannot be read as a picture, and no hand here can take one - but
      // every clip this machine stood has a still beside it, the poster the
      // gallery draws and the preview a browser moves, and a reference named by
      // a clip's id asks for the first of those that stands. without this, a
      // page could keep a clip's picture for later and be told, when it asked
      // for it, that the machine could not find it.
      for (const auto &still : {".poster.png", ".preview.apng"})
      {
        if (std::filesystem::exists(gRefsDir + "/" + source + ".avi", ec) ||
            std::filesystem::exists(gOutputDir + "/" + source + ".avi", ec))
        {
          const std::string kept = gRefsDir + "/" + source + still;
          if (std::filesystem::exists(kept, ec))
            return kept;
          const std::string made = gOutputDir + "/" + source + still;
          if (std::filesystem::exists(made, ec))
            return made;
        }
      }
      if (ols::fileExists(source))
        return source;
      return std::string();
    }

    static bool loadImageFile(const std::string &path, sd_image_t &image)
    {
      int width = 0;
      int height = 0;
      int channel = 0;
      unsigned char *pixels = stbi_load(path.c_str(), &width, &height, &channel, 0);
      if (!pixels)
        return false;
      image.width = static_cast<uint32_t>(width);
      image.height = static_cast<uint32_t>(height);
      image.channel = static_cast<uint32_t>(channel);
      image.data = pixels;
      return true;
    }

    // ---------- what a clip is stood as on disk ----------
    //
    // A clip is written by this machine rather than by a program it calls: one
    // executable stands here, and what the engine hands back from a video run is
    // frames and a sound, not bytes. Five files come out of one run, and each
    // one is the thing a different hand needs:
    //
    //   the clip    an MJPEG AVI - the frames as they came out, with the stereo
    //               audio muxed in. This is what a person saves.
    //   the plain   the same frames with no codec to know, as an AVI whose
    //               pictures are raw bottom-up rows. A reader that does not speak
    //               MPEG-J can still be shown what the model made.
    //   the poster  the first frame as one PNG, so a room of clips can be named by
    //               its pictures without one being played.
    //   the preview an APNG - a small, thinned copy of the clip that a browser
    //               draws as an image. A gallery card shows this instead of the
    //               clip: a picture a browser moves on its own, at a fraction of
    //               the clip's bytes, with no video decoder and no byte-range
    //               walk over a file a person may never watch.
    //   the track   the clip's audio on its own, as WAV. Browsers play WAV
    //               natively, so a clip's sound is reachable from a page even
    //               where the container above is not played by one.
    //
    // The AVI byte layout follows the one the vendored engine's own example writer
    // uses for this same kind of clip (examples/common/media_io.cpp:874), and the
    // first word of that layout is not spelled in that file: the codec fourcc goes
    // in its own slot of the video stream's header, which is what both outside
    // readers asked for and what this machine writes now (see writeAviClip).
    //
    // A clip's frames and audio are the model's own output: the engine states the
    // audio is "part of the model output rather than something added afterwards"
    // (unsloth's words beside these files), so the track is written rather than
    // dropped, and a clip that came without sound says so in its record instead
    // of standing as a silent one.
    static void putBE32(std::string &out, std::uint32_t value)
    {
      for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<char>((value >> shift) & 0xFFu));
    }
    static void putBE16(std::string &out, std::uint16_t value)
    {
      out.push_back(static_cast<char>((value >> 8) & 0xFFu));
      out.push_back(static_cast<char>(value & 0xFFu));
    }
    static void putLE32(std::string &out, std::uint32_t value)
    {
      for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<char>((value >> shift) & 0xFFu));
    }
    static void putLE16(std::string &out, std::uint16_t value)
    {
      out.push_back(static_cast<char>(value & 0xFFu));
      out.push_back(static_cast<char>((value >> 8) & 0xFFu));
    }

    static std::uint32_t pngCrc(const std::string &bytes)
    {
      // The sum a PNG chunk stands under, bit at a time, with the reversed
      // polynomial the format names. A bit set stands for "add the polynomial",
      // which is what the mask below turns the low bit into.
      constexpr std::uint32_t polynomial = 0xEDB88320u;
      std::uint32_t crc = 0xFFFFFFFFu;
      for (char c : bytes)
      {
        crc ^= static_cast<unsigned char>(c);
        for (int bit = 0; bit < 8; ++bit)
        {
          const std::uint32_t mask = (crc & 1u) ? polynomial : 0u;
          crc = (crc >> 1) ^ mask;
        }
      }
      return ~crc;
    }

    // a PNG chunk: the length, the kind, the bytes, and the sum the kind and the
    // bytes are stood under.
    static void pngChunk(std::string &out, const char *kind, const std::string &body)
    {
      const std::string tagged = std::string(kind) + body;
      putBE32(out, static_cast<std::uint32_t>(body.size()));
      out += tagged;
      putBE32(out, pngCrc(tagged));
    }

    // a PNG image stream that carries no compression: stored deflate blocks. A
    // preview's frames are pictures a browser will decode itself, and nothing here
    // needs a compressor in this machine - a stored block is a legal zlib stream,
    // which is what a small preview costs.
    static std::string zlibStored(const std::string &raw)
    {
      std::string out;
      out.push_back('\x78');
      out.push_back('\x01');
      size_t at = 0;
      std::uint32_t a = 1, b = 0;
      for (;;)
      {
        const size_t take = raw.size() - at < 65535u ? raw.size() - at : 65535u;
        const bool last = at + take >= raw.size();
        out.push_back(static_cast<char>(last ? 0x01 : 0x00));
        putLE16(out, static_cast<std::uint16_t>(take));
        putLE16(out, static_cast<std::uint16_t>(~take));
        out.append(raw.data() + at, take);
        for (size_t i = 0; i < take; ++i)
        {
          a += static_cast<unsigned char>(raw[at + i]);
          a %= 65521u;
          b += a;
          b %= 65521u;
        }
        at += take;
        if (last)
          break;
      }
      putBE32(out, (b << 16) | a);
      return out;
    }

    // the clip's sound on its own: PCM 16-bit, interleaved, at the rate the
    // engine carried it at. The engine's own example writer states what the
    // audio's fields mean (sample_count is a channel's frames, and the samples
    // interleave channels), so the same reading is used here.
    static std::string pcm16Of(const sd_audio_t *audio)
    {
      if (!audio || !audio->data || audio->sample_count == 0 || audio->channels == 0 || audio->sample_rate == 0)
        return std::string();
      const std::size_t frames = static_cast<std::size_t>(audio->sample_count);
      const std::uint32_t channels = audio->channels;
      std::string out;
      out.reserve(frames * channels * 2u);
      for (std::size_t i = 0; i < frames * channels; ++i)
      {
        const float sample = audio->data[i] < -1.0f ? -1.0f : (audio->data[i] > 1.0f ? 1.0f : audio->data[i]);
        const int value = static_cast<int>(std::lrint(sample * 32767.0f));
        putLE16(out, static_cast<std::uint16_t>(value));
      }
      return out;
    }

    static bool writeWavTrack(const std::string &path, const sd_audio_t *audio)
    {
      const std::string pcm = pcm16Of(audio);
      if (pcm.empty())
        return false;
      std::string out = "RIFF";
      putLE32(out, 0u);
      out += "WAVE";
      out += "fmt ";
      putLE32(out, 16u);
      putLE16(out, 1u); // uncompressed PCM
      putLE16(out, static_cast<std::uint16_t>(audio->channels));
      putLE32(out, audio->sample_rate);
      const std::uint32_t align = static_cast<std::uint32_t>(audio->channels * 2u);
      putLE32(out, align * audio->sample_rate);
      putLE16(out, static_cast<std::uint16_t>(align));
      putLE16(out, 16u);
      out += "data";
      putLE32(out, static_cast<std::uint32_t>(pcm.size()));
      out += pcm;
      // the whole file stood under its header, written where the header left room
      const std::uint32_t trackSize = static_cast<std::uint32_t>(out.size() - 8u);
      for (std::size_t at = 0; at < 4u; ++at)
        out[4u + at] = static_cast<char>((trackSize >> (at * 8u)) & 0xFFu);
      return writeFileText(path, out);
    }

    // how big, and how many frames, the copy a page plays on its own is: a
    // preview is a small picture a browser may be drawing many of at once, so its
    // own size, not the clip's, decides what it holds. The rate is low on
    // purpose - the motion of a clip is still readable at it - and the frames are
    // spread across the whole clip rather than taken from its start, so a preview
    // says what a clip is rather than what its first second is.
    static const int kPreviewFrames = 32;
    static const int kPreviewSide = 256;
    static const int kPreviewFps = 8;

    // the preview: a small, thinned copy of the clip as an animated PNG.
    // The frame layout is the one the machine's own picture reader writes for an
    // animated PNG - checked against it here, since nothing else on this machine
    // speaks the format. The rectangle of every frame is the whole picture, every
    // frame is complete, and nothing is left to the previous frame, so a browser
    // that draws one frame at a time draws the clip.
    static bool writeApngPreview(const std::string &path, const sd_image_t *frames, int count,
                                 int previewFps, int maxSide)
    {
      if (count < 1 || previewFps < 1 || maxSide < 8)
        return false;
      const std::uint32_t srcW = frames[0].width;
      const std::uint32_t srcH = frames[0].height;
      if (srcW == 0 || srcH == 0 || (frames[0].channel != 3u && frames[0].channel != 4u))
        return false;
      double ratio = 1.0;
      const double byWide = static_cast<double>(maxSide) / srcW;
      const double byTall = static_cast<double>(maxSide) / srcH;
      if (byWide < ratio)
        ratio = byWide;
      if (byTall < ratio)
        ratio = byTall;
      const auto wide = static_cast<std::uint32_t>(static_cast<double>(srcW) * ratio);
      const auto tall = static_cast<std::uint32_t>(static_cast<double>(srcH) * ratio);
      if (wide < 1u || tall < 1u)
        return false;
      // Frames are thinned evenly across the clip rather than only every Nth one,
      // so a preview shows the whole of a clip's motion at its own length rather
      // than its first second. A frame that does not stand at the size the others
      // do is left out here rather than in the writing below: the count the header
      // of the animation states is the number of frames it goes on to carry, and a
      // frame passed over after that number was written leaves the two disagreeing.
      const int shown = count < kPreviewFrames ? count : kPreviewFrames;
      std::vector<int> picks;
      picks.reserve(static_cast<std::size_t>(shown));
      for (int at = 0; at < shown; ++at)
      {
        const sd_image_t &frame = frames[static_cast<std::size_t>((static_cast<long long>(at) * count) / shown)];
        if (frame.width == srcW && frame.height == srcH && frame.data)
          picks.push_back(static_cast<int>((static_cast<long long>(at) * count) / shown));
      }
      if (picks.empty())
        return false;

      std::string out = "\x89PNG\r\n\x1a\n";
      std::string head;
      putBE32(head, wide);
      putBE32(head, tall);
      head.push_back('\x08');
      head.push_back('\x02'); // RGB, no alpha
      head.push_back('\x00'); // the default compression
      head.push_back('\x00'); // filtering: the rows stand as they are
      head.push_back('\x00'); // interlace: none
      pngChunk(out, "IHDR", head);
      std::string control;
      putBE32(control, static_cast<std::uint32_t>(picks.size()));
      putBE32(control, 0u); // forever
      pngChunk(out, "acTL", control);

      std::uint32_t sequence = 0u;
      for (int index : picks)
      {
        const sd_image_t &frame = frames[index];
        // fcTL and fdAT share one ascending sequence with no gaps (the words the
        // format carries: "the sequence numbers in the remaining fcTL and fdAT
        // chunks shall be in ascending order, with no gaps or duplicates"), so the
        // number here is a count of the frame chunks written so far, not of the
        // frames themselves.
        std::string control2;
        putBE32(control2, sequence);
        putBE32(control2, wide);
        putBE32(control2, tall);
        putBE32(control2, 0u);
        putBE32(control2, 0u);
        control2.push_back('\x00');                                // the frame stands for one part of a second:
        control2.push_back('\x01');                                // a numerator of one over the rate below
        putBE16(control2, static_cast<std::uint16_t>(previewFps)); // the denominator
        putBE16(control2, 0u);                                     // dispose: leave what was drawn; blend: source -
                                                                   // every frame is the whole picture, so what a
                                                                   // reader draws over what it says stands
        pngChunk(out, "fcTL", control2);
        ++sequence;

        std::string raw;
        raw.reserve(static_cast<std::size_t>(tall) * (static_cast<std::size_t>(wide) * 3u + 1u));
        const unsigned char *src = frame.data;
        const std::uint32_t channels = frame.channel;
        for (std::uint32_t y = 0; y < tall; ++y)
        {
          const std::uint32_t sy = static_cast<std::uint32_t>(
              (static_cast<long long>(y) * srcH) / tall);
          raw.push_back('\x00'); // this row stands as it is
          for (std::uint32_t x = 0; x < wide; ++x)
          {
            const std::uint32_t sx = static_cast<std::uint32_t>(
                (static_cast<long long>(x) * srcW) / wide);
            const unsigned char *pixel = src + (static_cast<std::size_t>(sy) * srcW + sx) * channels;
            raw.append(reinterpret_cast<const char *>(pixel), 3u);
          }
        }
        const std::string image = zlibStored(raw);
        if (sequence == 1u)
          pngChunk(out, "IDAT", image); // the first frame stands as the base image
        else
        {
          // a frame after the first carries its bytes as fdAT, numbered in the same
          // sequence its fcTL was numbered in
          std::string body;
          putBE32(body, sequence);
          body += image;
          pngChunk(out, "fdAT", body);
          ++sequence;
        }
      }
      std::string end;
      pngChunk(out, "IEND", end);
      return writeFileText(path, out);
    }

    // one frame of a clip, stood as a JPEG: the frames of an MJPEG AVI are plain
    // JPEG files, one after another. The engine's own example writer says why the
    // quality is capped (media_io.cpp:888: "stb_image_write changes JPEG sampling
    // behavior above quality 90. MJPG AVI playback is more compatible when we
    // keep the encoder on the <= 90 path").
    static void appendJpegBytes(void *context, void *bytes, int size)
    {
      auto *out = static_cast<std::vector<std::uint8_t> *>(context);
      const unsigned char *src = static_cast<const unsigned char *>(bytes);
      out->insert(out->end(), src, src + size);
    }

    // A RIFF chunk: the four letters, the number of bytes that stand under them,
    // and the bytes. Every size below is read off the bytes that were built, so
    // a field count cannot drift away from the size a reader is told.
    static void aviChunk(std::string &out, const char *fourcc, const std::string &body)
    {
      out += fourcc;
      putLE32(out, static_cast<std::uint32_t>(body.size()));
      out += body;
    }

    // a size that was written as a placeholder before all of the bytes it counts
    // were in, stood with the real number now that they are.
    static void patchLE32(std::string &out, std::size_t at, std::uint32_t value)
    {
      for (std::size_t byte = 0u; byte < 4u; ++byte)
        out[at + byte] = static_cast<char>((value >> (byte * 8u)) & 0xFFu);
    }

    // one frame as the plain pixels a plainest player reads: rows bottom up, three
    // bytes a pixel, and every row a whole number of four.
    static void appendRawFrameBytes(std::string &into, const sd_image_t &frame,
                                    std::uint32_t rowStride)
    {
      const std::uint32_t wide = frame.width;
      const std::uint32_t tall = frame.height;
      for (std::uint32_t line = 0u; line < tall; ++line)
      {
        const std::uint32_t row = tall - 1u - line;
        for (std::uint32_t column = 0u; column < wide; ++column)
        {
          const std::uint8_t *pixel =
              frame.data + (static_cast<std::size_t>(row) * wide + column) * frame.channel;
          into.push_back(static_cast<unsigned char>(pixel[2]));
          into.push_back(static_cast<unsigned char>(pixel[1]));
          into.push_back(static_cast<unsigned char>(pixel[0]));
        }
        into.insert(into.end(), rowStride - wide * 3u, '\0');
      }
    }

    // The clip's own file: a RIFF AVI that carries the frames as they came and the
    // sound the model carried with them. With keepTheFramesWhole the frames are
    // written as plain pixels instead of as JPEGs, which no player has to know a
    // codec for - a much bigger file that even the plainest player reads. A frame
    // that does not stand at the size of the others is left out, so framesWritten
    // comes back with the number the file really carries.
    static bool writeAviClip(const std::string &path, const sd_image_t *frames, int count, int fps,
                             const sd_audio_t *audio, bool keepTheFramesWhole = false,
                             int *framesWrittenOut = nullptr)
    {
      if (framesWrittenOut)
        *framesWrittenOut = 0;
      if (count < 1 || fps < 1)
        return false;
      const std::uint32_t wide = frames[0].width;
      const std::uint32_t tall = frames[0].height;
      if (wide == 0u || tall == 0u || (frames[0].channel != 3u && frames[0].channel != 4u))
        return false;
      const std::string pcm = pcm16Of(audio);
      const bool hasAudio = !pcm.empty();
      const std::uint32_t channels = hasAudio ? audio->channels : 1u;
      const std::uint32_t rate = hasAudio ? audio->sample_rate : 0u;
      const std::uint32_t align = channels * 2u;
      const bool rawFrames = keepTheFramesWhole;
      const std::uint32_t rowStride = (wide * 3u + 3u) & ~3u;
      const std::uint32_t frameBytes = rawFrames ? rowStride * tall : wide * tall * 3u;

      // Every frame is stood for itself before a single header byte is written. The
      // headers name how many frames the clip carries, so a frame that cannot be
      // made has to be known about first: passed over after the headers were
      // written, it would leave the clip saying one thing and holding another.
      std::vector<std::string> payloads;
      payloads.reserve(static_cast<std::size_t>(count));
      for (int at = 0; at < count; ++at)
      {
        const sd_image_t &frame = frames[at];
        if (frame.width != wide || frame.height != tall || !frame.data)
          continue;
        std::string body;
        if (rawFrames)
        {
          appendRawFrameBytes(body, frame, rowStride);
        }
        else
        {
          std::vector<std::uint8_t> jpeg;
          if (!stbi_write_jpg_to_func(appendJpegBytes, &jpeg, static_cast<int>(wide),
                                      static_cast<int>(tall), static_cast<int>(frame.channel),
                                      frame.data, 90))
            return false;
          body.append(reinterpret_cast<const char *>(jpeg.data()), jpeg.size());
        }
        payloads.push_back(std::move(body));
      }
      if (payloads.empty())
        return false;
      const std::uint32_t made = static_cast<std::uint32_t>(payloads.size());
      if (framesWrittenOut)
        *framesWrittenOut = static_cast<int>(made);
      // A frame of plain pixels is written as an uncompressed chunk: that is what
      // tells a reader to take the bytes as they stand rather than hand them to a
      // codec, which is the whole point of the plain copy.
      const char *const chunkKind = rawFrames ? "00db" : "00dc";
      const std::uint32_t chunkFlags = rawFrames ? 0u : 0x10u;
      // the biggest chunk either stream holds, and what the whole clip costs a
      // second: these two fields are read by players that size their buffers from
      // the header, and an audio-only number in a field that counts both streams
      // says a compressed clip needs no room for its pictures.
      std::uint64_t clipBytes = 0;
      std::uint32_t biggestVideo = 0;
      for (const std::string &body : payloads)
      {
        clipBytes += body.size();
        if (body.size() > biggestVideo)
          biggestVideo = static_cast<std::uint32_t>(body.size());
      }
      std::uint32_t biggestChunk = biggestVideo;
      if (hasAudio && pcm.size() > biggestChunk)
        biggestChunk = static_cast<std::uint32_t>(pcm.size());
      const std::uint32_t bytesPerSecond = static_cast<std::uint32_t>(
          (clipBytes + (hasAudio ? pcm.size() : 0u)) * static_cast<std::uint64_t>(fps) /
          static_cast<std::uint64_t>(made));

      std::string avih;
      putLE32(avih, static_cast<std::uint32_t>(1000000 / fps));
      putLE32(avih, bytesPerSecond);
      putLE32(avih, 0u);
      putLE32(avih, 0x110u);
      putLE32(avih, made);
      putLE32(avih, 0u);
      putLE32(avih, hasAudio ? 2u : 1u);
      putLE32(avih, biggestChunk);
      putLE32(avih, wide);
      putLE32(avih, tall);
      for (int at = 0; at < 4; ++at)
        putLE32(avih, 0u);

      // A stream header names what the stream is, so that a reader can ask for one
      // part of a clip without reading the rest. The frames travel whole, one per
      // frame: as JPEGs when a codec is agreed on, or as plain pixels when none is.
      std::string video;
      video += "vids";
      if (rawFrames)
        video += std::string(4, '\0');
      else
        video += "MJPG";
      putLE32(video, 0u);
      putLE16(video, 0u);
      putLE16(video, 0u);
      putLE32(video, 0u);
      putLE32(video, 1u);
      putLE32(video, static_cast<std::uint32_t>(fps));
      putLE32(video, 0u);
      putLE32(video, made);
      putLE32(video, rawFrames ? frameBytes : biggestVideo);
      putLE32(video, static_cast<std::uint32_t>(-1));
      // a sample is a fixed size only when no codec sits between: a compressed
      // frame is as long as it is, and the index says where each one starts.
      putLE32(video, rawFrames ? frameBytes : 0u);
      // the picture's rectangle, four whole numbers, as the header ends.
      putLE32(video, 0u);
      putLE32(video, 0u);
      putLE32(video, wide);
      putLE32(video, tall);

      std::string videoFormat;
      putLE32(videoFormat, 0x28u);
      putLE32(videoFormat, wide);
      // Plain pixels are written from the bottom row up, which is what a positive
      // height says; a JPEG frame carries its own order and its height is the
      // picture's, not a statement about the rows.
      putLE32(videoFormat, tall);
      putLE16(videoFormat, 1u);
      putLE16(videoFormat, 24u);
      if (rawFrames)
        videoFormat += std::string(4, '\0');
      else
        videoFormat += "MJPG";
      putLE32(videoFormat, frameBytes);
      for (int at = 0; at < 4; ++at)
        putLE32(videoFormat, 0u);

      std::string hdrl = "hdrl";
      aviChunk(hdrl, "avih", avih);

      std::string videoStream = "strl";
      aviChunk(videoStream, "strh", video);
      aviChunk(videoStream, "strf", videoFormat);
      aviChunk(hdrl, "LIST", videoStream);

      if (hasAudio)
      {
        // The sound stream is PCM at the rate the model carried it at, and each
        // number stands where the container says it stands. A block is one frame of
        // sound across every channel, so `align` bytes pass for one sample of the
        // stream: that is what a reader multiplies to reach a moment of the clip,
        // and how many of them there are is how long the clip's sound runs. Say the
        // rate where the rate belongs and the count where the count belongs, and a
        // reader that asks the header for a length is told one.
        std::string audioHead;
        audioHead += "auds";
        putLE32(audioHead, 0u);                                             // no codec: the words say uncompressed PCM
        putLE32(audioHead, 0u);                                             // flags
        putLE16(audioHead, 0u);                                             // priority
        putLE16(audioHead, 0u);                                             // language
        putLE32(audioHead, 0u);                                             // nothing stands before the first sample
        putLE32(audioHead, align);                                          // one block of sound
        putLE32(audioHead, align * rate);                                   // passes every second
        putLE32(audioHead, 0u);                                             // starts at the beginning
        putLE32(audioHead, static_cast<std::uint32_t>(pcm.size() / align)); // runs this long
        putLE32(audioHead, static_cast<std::uint32_t>(pcm.size()));         // one chunk holds it all
        putLE32(audioHead, static_cast<std::uint32_t>(-1));                 // quality: the default
        putLE32(audioHead, align);                                          // a block is this wide
        // a sound stream has no picture rectangle, but the sixteen bytes where it
        // stands are still part of every stream header: a reader that walks the
        // header stops at the wrong letter without them.
        for (int at = 0; at < 4; ++at)
          putLE32(audioHead, 0u);

        std::string audioFormat;
        putLE16(audioFormat, 1u);
        putLE16(audioFormat, static_cast<std::uint16_t>(channels));
        putLE32(audioFormat, rate);
        putLE32(audioFormat, align * rate);
        putLE16(audioFormat, static_cast<std::uint16_t>(align));
        putLE16(audioFormat, 16u);

        std::string audioStream = "strl";
        aviChunk(audioStream, "strh", audioHead);
        aviChunk(audioStream, "strf", audioFormat);
        aviChunk(hdrl, "LIST", audioStream);
      }

      std::string out = "RIFF";
      putLE32(out, 0u);
      out += "AVI ";
      const std::size_t hdrlSizeAt = out.size() + 4u;
      out += "LIST";
      putLE32(out, 0u);
      out += hdrl;
      patchLE32(out, hdrlSizeAt, static_cast<std::uint32_t>(out.size() - hdrlSizeAt - 4u));

      out += "LIST";
      const std::size_t moviSizeAt = out.size();
      putLE32(out, 0u);
      out += "movi";

      struct ClipIndexEntry
      {
        const char *kind;
        std::uint32_t flags;
        std::uint32_t offset;
        std::uint32_t size;
      };
      std::vector<ClipIndexEntry> index;
      index.reserve(static_cast<std::size_t>(made) + 1u);
      // An index entry names its chunk from where the clip's own bytes begin, not
      // from where the file begins: the header, the list that carries it, the four
      // letters naming the list and the four naming its contents all stand before
      // that. A reader adds an entry to the start of the clip's bytes to reach a
      // frame, so a number measured from the file walks it past the clip and leaves
      // it to hunt through the bytes for itself.
      const std::size_t moviDataAt = moviSizeAt + 8u;

      for (const std::string &body : payloads)
      {
        ClipIndexEntry entry{chunkKind, chunkFlags,
                             static_cast<std::uint32_t>(out.size() - moviDataAt),
                             static_cast<std::uint32_t>(body.size())};
        out += entry.kind;
        putLE32(out, entry.size);
        out += body;
        index.push_back(entry);
        if (entry.size % 2u != 0u)
          out.push_back('\0');
      }

      if (hasAudio)
      {
        ClipIndexEntry entry{"01wb", 0u, static_cast<std::uint32_t>(out.size() - moviDataAt),
                             static_cast<std::uint32_t>(pcm.size())};
        out += entry.kind;
        putLE32(out, entry.size);
        out += pcm;
        index.push_back(entry);
        if (entry.size % 2u != 0u)
          out.push_back('\0');
      }

      // the sizes the file is stood under, written down now that all of the
      // clip's bytes are in: the length of the clip's chunks and of the file.
      const std::uint32_t moviSize = static_cast<std::uint32_t>(out.size() - moviSizeAt - 4u);
      patchLE32(out, moviSizeAt, moviSize);

      out += "idx1";
      putLE32(out, static_cast<std::uint32_t>(index.size() * 16u));
      for (const ClipIndexEntry &entry : index)
      {
        out += entry.kind;
        putLE32(out, entry.flags);
        putLE32(out, entry.offset);
        putLE32(out, entry.size);
      }

      const std::uint32_t fileSize = static_cast<std::uint32_t>(out.size() - 8u);
      patchLE32(out, 4u, fileSize);

      return writeFileText(path, out);
    }

    static void doGenerate(const std::string &job, const std::string &modelId,
                           const std::string &payloadJson)
    {
      const json request = json::parse(payloadJson, nullptr, false);
      if (request.is_discarded())
      {
        noteError(job, "the request was not readable");
        markState(job, ols::kStateError);
        return;
      }
      const ImageModelDef *def = findModel(modelId);
      if (!def)
      {
        noteError(job, "that model is not one this machine knows");
        markState(job, ols::kStateError);
        return;
      }
      // the bounds of one run, named here once so a message cannot drift from the
      // check it describes: the machine keeps a run inside what one model fits at a
      // time, and the words a run answers with are built from these same numbers.
      // kDefaultSide is the last resort and not the rule: a model whose card names
      // a size draws at that size when the run asks for none.
      static const int kDefaultSide = 768;
      static const int kMinSide = 64;
      static const int kMaxSide = 4096;
      static const int kMinSteps = 1;
      static const int kMaxSteps = 150;
      // One run draws its pictures back to back inside one model open, and a
      // stop between pictures keeps the ones already written - so the ceiling
      // bounds a run's silence between landings rather than the ask. The page
      // hands its batches in runs of its own fill; a number above that only
      // stands where another client asks deeper.
      static const int kMaxBatchPerRun = 16;
      static const int kMaxReferenceImages = 10;
      const int askedWidth = request.value("width", def->width > 0 ? def->width : kDefaultSide);
      const int askedHeight = request.value("height", def->height > 0 ? def->height : kDefaultSide);
      // the sides a clip is finally drawn at: the ones asked, less when the
      // card's window bends a moved-from clip smaller below. what was asked
      // stays in the run's ask; what is left is what the engine and the file
      // speak of.
      int width = askedWidth;
      int height = askedHeight;
      const int steps = request.value("steps", def->steps);
      const double guidanceAsked = request.value("guidance", static_cast<double>(def->guidance));
      const float guidance = static_cast<float>(guidanceAsked);
      const int batch = request.value("count", 1);
      if (width < kMinSide || width > kMaxSide || height < kMinSide || height > kMaxSide)
      {
        noteError(job, "the side asked for is outside " + std::to_string(kMinSide) + ".." +
                           std::to_string(kMaxSide));
        markState(job, ols::kStateError);
        return;
      }
      if (steps < kMinSteps || steps > kMaxSteps)
      {
        noteError(job, "the steps asked for are outside " + std::to_string(kMinSteps) + ".." +
                           std::to_string(kMaxSteps));
        markState(job, ols::kStateError);
        return;
      }
      // guidance answers in the engine's own units, and a number it was never
      // meant - not-a-number among them - is refused here rather than walked
      // into the sampler, where no check would notice it: every comparison a
      // range check leans on lets a not-a-number through.
      if (!std::isfinite(guidanceAsked) || guidanceAsked < 0.0 || guidanceAsked > 100.0)
      {
        noteError(job, "the guidance asked for is outside 0..100");
        markState(job, ols::kStateError);
        return;
      }
      // how long a clip is, in frames rather than in seconds: a model whose card
      // says its work moves names a length beside it, and the bounds here are what
      // this machine will run. The engine keeps the count on the grid its temporal
      // compression works on - 17 frames and 5, aligned upward - so the ceiling is
      // the last grid point under it, 17*14+5: a count between grid points is
      // raised to the next one and would then stand past the ceiling it was
      // refused by.
      static const int kMinFrames = 5;
      static const int kMaxFrames = 243;
      int framesAsked = request.value("frames", def->frames > 0 ? def->frames : kMinFrames);
      if (def->video && (framesAsked < kMinFrames || framesAsked > kMaxFrames))
      {
        noteError(job, "the clip asked for is outside " + std::to_string(kMinFrames) + ".." +
                           std::to_string(kMaxFrames) + " frames");
        markState(job, ols::kStateError);
        return;
      }
      // what was asked before any bending below: a record that says twelve
      // frames where a hundred and twenty-four were asked is the card speaking,
      // and only the two numbers side by side say so.
      const int framesWanted = framesAsked;
      // the pictures this run starts from, in the order they were handed over:
      // an editing model reads each of them and keeps them apart by position,
      // a redraw model takes one picture and changes it. naming a single
      // picture with "reference" is the same thing as a list of one.
      std::vector<std::string> sources;
      if (request.contains("references") && request["references"].is_array())
      {
        for (const auto &entry : request["references"])
        {
          if (!entry.is_string())
          {
            noteError(job, "each reference has to name a kept picture or a path");
            markState(job, ols::kStateError);
            return;
          }
          sources.push_back(entry.get<std::string>());
        }
      }
      const std::string reference = request.value("reference", std::string());
      if (!reference.empty())
        sources.push_back(reference);
      if (static_cast<int>(sources.size()) > kMaxReferenceImages)
      {
        noteError(job, "more reference pictures were given than this machine takes (at most " +
                           std::to_string(kMaxReferenceImages) + ")");
        markState(job, ols::kStateError);
        return;
      }
      // A card only holds so many picture-frames at once. The engine keeps no
      // window over them: the whole clip is one working set, and an ask past
      // the card dies inside it after every weight has moved. The maker's own
      // recipe runs 56 frames of 864x480 on a 24 GiB card with the weights
      // streaming, so that many picture-frames are taken as what such a card
      // holds, and the window a card of this size holds follows from it.
      // A clip that starts from a picture carries that picture's features
      // beside its own frames: measured on MSI 2026-11-19, the same 960x544
      // clip asked 4341 MB of the manager from words alone and 6355 MB from an
      // image - and a working model of the cost says why: the picture adds
      // about 159 bytes per picture-frame on top of the clip's own 37, over a
      // floor of 4111 MB. The engine lifts a frame count up to the next point
      // of the grid its temporal model walks - every seventeen frames and
      // five - and the one step above five is twenty-two: five frames is a
      // fifth of a second, which reads as a still no matter what the model
      // drew. So a window too small for twenty-two whole moved-from frames
      // bends the PICTURE down before it bends the length, rather than
      // standing a five-frame clip that looks broken; measured the next way
      // was eight clips of 0.2 s that a person correctly called static.
      if (def->video)
      {
        const long long cardBytes = cudaDeviceTotalBytes();
        if (cardBytes > 0)
        {
          const long long kVendorPixelFrames = 864LL * 480 * 56;
          long long window = kVendorPixelFrames * (cardBytes / (1024LL * 1024 * 1024)) / 24;
          if (!sources.empty())
            window = window * 2 / 3;
          const long long asked = static_cast<long long>(framesAsked) * width * height;
          if (asked > window)
          {
            int fits = static_cast<int>(window / (static_cast<long long>(width) * height));
            if (sources.empty())
            {
              fits -= fits % 4;
              if (fits < kMinFrames)
                fits = kMinFrames;
              std::fprintf(stderr, "[image] %s asked for %d frames of %dx%d: this card's window holds %d, the clip will be made that long\n",
                           modelId.c_str(), framesWanted, width, height, fits);
            }
            else
            {
              if (fits < 22)
              {
                // the window cannot hold one whole grid step of frames at the
                // asked size: trade the side for the seconds, quarter-limit
                // held (a 256-wide picture is the smallest one worth moving).
                double scale = std::sqrt((window / 22.0) / (static_cast<double>(width) * height)) * 0.96;
                int bentW = static_cast<int>(width * scale) & ~15;
                int bentH = static_cast<int>(height * scale) & ~15;
                if (bentW < 256)
                  bentW = 256;
                if (bentH < 256)
                  bentH = 256;
                while (static_cast<long long>(bentW) * bentH * 22 > window &&
                       (bentW > 256 || bentH > 256))
                {
                  bentW = (bentW * 9 / 10) & ~15;
                  bentH = (bentH * 9 / 10) & ~15;
                  if (bentW < 256)
                    bentW = 256;
                  if (bentH < 256)
                    bentH = 256;
                }
                if (static_cast<long long>(bentW) * bentH * 22 <= window && bentW != width)
                {
                  std::fprintf(stderr, "[image] %s asked for %d frames of %dx%d: that card's window holds fewer than twenty-two of those, so the picture bends to %dx%d to keep the clip a clip\n",
                               modelId.c_str(), framesWanted, width, height, bentW, bentH);
                  width = bentW;
                  height = bentH;
                }
              }
              fits = static_cast<int>(window / (static_cast<long long>(width) * height));
              fits -= (fits - kMinFrames) % 17;
              if (fits < kMinFrames)
                fits = kMinFrames;
              std::fprintf(stderr, "[image] %s: the clip will be %d frames of %dx%d\n",
                           modelId.c_str(), fits, width, height);
            }
            std::fflush(stderr);
            framesAsked = fits;
          }
        }
      }
      struct FreeRefs
      {
        std::vector<sd_image_t> *images;
        ~FreeRefs()
        {
          for (sd_image_t &image : *images)
          {
            if (image.data)
            {
              std::free(image.data);
              image.data = nullptr;
            }
          }
        }
      };
      std::vector<sd_image_t> refImages;
      FreeRefs freeRefs{&refImages};
      bool hasRef = false;
      if (!sources.empty())
      {
        if (def->refs < 1)
        {
          noteError(job, "this model does not take reference pictures");
          markState(job, ols::kStateError);
          return;
        }
        if (static_cast<int>(sources.size()) > def->refs)
        {
          noteError(job, "this model takes at most " + std::to_string(def->refs) +
                             " reference pictures; " + std::to_string(sources.size()) +
                             " were asked for");
          markState(job, ols::kStateError);
          return;
        }
        for (const std::string &source : sources)
        {
          const std::string path = resolveReference(source);
          if (path.empty())
          {
            noteError(job, "no kept picture stands under that name: " + source);
            markState(job, ols::kStateError);
            return;
          }
          sd_image_t image{};
          if (!loadImageFile(path, image))
          {
            noteError(job, "the reference picture could not be read: " + source);
            markState(job, ols::kStateError);
            return;
          }
          refImages.push_back(image);
        }
        hasRef = true;
      }
      // The whole of a run that stands on the card - opening what it draws
      // with, staging weights, and every step - happens one run at a time,
      // behind the gate beside the contexts. Standing for the turn first means
      // the line is the order the machine took its asks in, and not the order
      // threads happened to wake; counting the run busy before it is crossed
      // would let a queued run cast the standing one out of the card's memory
      // while that one still holds it.
      if (!waitForTurn(job))
      {
        markState(job, ols::kStateCancelled);
        return;
      }
      std::lock_guard<std::mutex> cardGate(gCardGate);
      {
        std::lock_guard<std::mutex> lock(gCtxMutex);
        ++gGenerating[modelId];
      }
      // from here to the end of the run this model is busy, and a good deal of
      // what lies ahead can still throw: a counter let go by hand on every road
      // out is a counter some road will forget.
      struct GenerationCount
      {
        std::string model;
        ~GenerationCount()
        {
          std::lock_guard<std::mutex> lock(gCtxMutex);
          const auto now = gGenerating.find(model);
          if (now != gGenerating.end() && --now->second <= 0)
            gGenerating.erase(model);
          finishPendingLocked();
        }
      } generationCount{modelId};
      std::string error;
      std::string openedForm;
      // "cpu" names which part of the machine this run works on; a run that
      // names nothing takes the switch the page left standing. the switches are
      // named here once because the run's own record carries them: what a run
      // cost is filed under the switches it ran with, and a name read twice can
      // drift apart from the one that was used.
      const bool sageAsked = request.value("sage_attn", prefOn("sage_attn"));
      const bool cpuAsked = request.value("cpu", prefOn("cpu"));
      const long long openBegan = ols::steadyMs();
      sd_ctx_t *ctx = openContext(modelId, sageAsked, cpuAsked, error, &openedForm);
      // the open says how much of a run was moving weights rather than drawing:
      // zero when the model was already standing, whole seconds when it was not.
      const long long openMs = ols::steadyMs() - openBegan;
      std::string deviceLabel;
      if (!ctx)
      {
        noteError(job, error);
        markState(job, ols::kStateError);
        return;
      }

      sd_sample_params_t sample;
      sd_sample_params_init(&sample);
      // a model trained without guidance states 0 on its card; 1.0 is the same
      // sentence in the engine's words (its own note reads "use cfg-scale=1 for
      // distilled models"), so 0 is answered with 1.0 and nothing more.
      sample.guidance.txt_cfg = guidance == 0.0f ? 1.0f : guidance;
      sample.sample_steps = steps;
      // The sampler and scheduler the model's own card names, asked in the
      // engine's words: the engine's choice for the model's family stands where
      // the card names nothing, and a name the engine does not know is kept as
      // the engine's own choice - and said - rather than answered as a failure
      // of the run. Which solver this run really sampled with is kept beside it,
      // named in the words the picture table uses, so the run's cost is filed
      // under the solver that did the work and not under a name the engine
      // quietly replaced.
      sample.sample_method = sd_get_default_sample_method(ctx);
      std::string samplerUsed = "engine default";
      std::string schedulerUsed = "engine default";
      if (!def->sampler.empty())
      {
        const enum sample_method_t asked = str_to_sample_method(def->sampler.c_str());
        if (asked != SAMPLE_METHOD_COUNT)
        {
          sample.sample_method = asked;
          samplerUsed = def->sampler;
        }
        else
          noteStep(job, "the card names a sampler the engine does not know (" + def->sampler +
                            "); the engine's own choice stands");
      }
      // The grid a solver walks belongs to the solver, so its default is asked
      // after the solver above is settled - not before, which keyed every row's
      // grid to the family's solver instead of to the one the card named.
      sample.scheduler = sd_get_default_scheduler(ctx, sample.sample_method);
      if (!def->scheduler.empty())
      {
        const enum scheduler_t asked = str_to_scheduler(def->scheduler.c_str());
        if (asked != SCHEDULER_COUNT)
        {
          sample.scheduler = asked;
          schedulerUsed = def->scheduler;
        }
        else
          noteStep(job, "the card names a scheduler the engine does not know (" + def->scheduler +
                            "); the solver's own grid stands");
      }
      std::vector<float> sigmas;
      if (!def->sigmas.empty())
      {
        // The steps a model asks for stand in this machine's own table, and a
        // table is a thing hands edit. A row that words one of them as something
        // other than a number says nothing about the steps it meant; it must not
        // end the run, nor the server, on a thread that is nobody's to end.
        std::istringstream parts(def->sigmas);
        std::string part;
        while (std::getline(parts, part, ','))
        {
          const char *where = part.c_str();
          char *stops = nullptr;
          const float value = std::strtof(where, &stops);
          if (stops == where)
            break;
          sigmas.push_back(value);
        }
        sample.custom_sigmas = sigmas.empty() ? nullptr : sigmas.data();
        sample.custom_sigmas_count = static_cast<int>(sigmas.size());
      }

      sd_img_gen_params_t img;
      sd_img_gen_params_init(&img);
      const std::string prompt = request.value("prompt", std::string("a quiet street at dusk"));
      std::string negative = request.value("negative_prompt", std::string());
      if (negative.empty())
        negative = def->negative;
      img.prompt = prompt.c_str();
      img.negative_prompt = negative.empty() ? nullptr : negative.c_str();
      img.width = width;
      img.height = height;
      img.sample_params = sample;
      img.strength = 1.0f;
      img.seed = request.value("seed", -1);
      // one clip is one run's work: a clip is already a long window, and this
      // machine keeps the work of a run inside one model at a time, so asking a
      // model whose work moves for several clips in one run is not something it
      // answers with several windows for.
      img.batch_count = def->video ? 1
                                   : (batch < 1 ? 1 : (batch > kMaxBatchPerRun ? kMaxBatchPerRun : batch));
      // vae tiling follows the page's switch when the run names nothing, the way
      // the device switches do - a switch that only works when a run happens to
      // name it is a switch the page should not be asked to leave standing.
      // Three cards are clip DiTs whose untiled vae decode crawls the conv path
      // at a small fraction of its speed on this gpu's driver - measured here at
      // 725 s against the same decode tiled at 241, and at never coming back
      // against one that came back in 912 - so tiling stands on for them unless
      // a run says it wants the untiled road after all.
      static const char *const kTiledCards[] = {"z-image-turbo", "qwen-image-2.1",
                                                "qwen-image-2.1-turbo"};
      bool tilingOn = request.value("tiling", prefOn("tiling"));
      if (!request.contains("tiling") && !tilingOn)
        for (const char *const tiled : kTiledCards)
          if (def->id == tiled)
          {
            tilingOn = true;
            break;
          }
      img.vae_tiling_params.enabled = tilingOn;
      // The text encoder stops after this many blocks. Left at -1 the engine
      // answers for the model's family - 2 for SD 2 and SDXL, 1 for SD 1.5
      // (src/conditioning/conditioner.hpp:459) - which is right for the families
      // the engine knows and wrong for a model whose card names a number the
      // family default does not give it.
      img.clip_skip = def->clip_skip;
      // The shift travels with the sample settings; INFINITY is the engine's own
      // word for "answer with this family's default", so a row that says nothing
      // changes nothing (src/pipeline/image.cpp:818 feeds this straight to
      // set_flow_shift).
      img.sample_params.flow_shift = def->flow_shift;
      // a step whose latent barely moved reuses the step before it: the vendored
      // library names this path for UNET models and refuses it for every other
      // model type on its own (src/runtime/sample-cache.cpp prints "UCache
      // requested but not supported for this model type"), and its own docs say
      // the UNet step caches do not apply to the DiT families - so one ask here
      // covers a run, and the library decides per model whether anything engages.
      // the numbers are the ones that documentation gives: sd_cache_params_init
      // leaves reuse_threshold at INFINITY, which caches nothing, so naming the
      // mode alone would change no work at all. a run may say "cache": false to
      // compare the same generation with and without it, which is what those
      // docs ask for before trusting a cache. a model whose card names only the
      // few steps a distillation was trained for names "cache": false with them
      // (see kTuning): that cache starts at fifteen percent of the steps and
      // stops at ninety, so in four steps it is offered the work the sampling is.
      const bool cacheOn = def->cache && request.value("cache", true);
      if (cacheOn)
      {
        img.cache.mode = SD_CACHE_UCACHE;
        img.cache.reuse_threshold = 1.0f;
      }
      if (hasRef)
      {
        if (def->edit)
        {
          img.ref_images = refImages.data();
          img.ref_images_count = static_cast<int>(refImages.size());
        }
        else
        {
          const float strength = static_cast<float>(request.value("strength", 0.6));
          img.init_image = refImages[0];
          img.strength = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);
        }
      }
      // What this run resolved to, kept with the job so that the machine can say,
      // once the run settles, what a run of these settings cost. The seed is not
      // among them: another seed is the same work at these settings, which is why
      // repeats of one ask agree on one estimate. The quant is the form the
      // weights really stood as, and the guidance the number the run used.
      {
        std::lock_guard<std::mutex> lock(gMutex);
        json &jobRecord = gJobs[job];
        json asked = json::object();
        asked["width"] = width;
        asked["height"] = height;
        asked["steps"] = steps;
        asked["guidance"] = guidanceAsked == 0.0 ? 1.0 : guidanceAsked;
        asked["quant"] = openedForm.empty() ? std::string("as shipped") : openedForm;
        asked["sampler"] = samplerUsed;
        asked["scheduler"] = schedulerUsed;
        asked["cache"] = cacheOn;
        // The vae road this run really took, as the run was answered rather
        // than as it was asked: a tiled decode and an untiled one are different
        // work on several of these cards, and one cost row for both would let a
        // 241 s tiled row price a 725 s untiled ask.
        asked["tiling"] = img.vae_tiling_params.enabled;
        // Which part of the machine this run really worked on, in that part's own
        // name - "cpu", or the device the engine named ("Vulkan0 AMD Radeon(TM)
        // 890M Graphics", tomorrow a CUDA row). A plain "card" here filed work
        // done on different cards and drivers under one row, and an estimate
        // carried from a faster card onto a slower one is a wrong number, not a
        // near one. The open's own record answers, because that is where a card
        // asked for and a cpu delivered is settled.
        const bool stoodOnCpu = [&]()
        {
          std::lock_guard<std::mutex> ctxLock(gCtxMutex);
          return gCtxCpu.count(modelId) ? gCtxCpu[modelId] : cpuAsked;
        }();
        std::string deviceName;
        if (!stoodOnCpu)
          deviceName = gpuDeviceName();
        deviceLabel = stoodOnCpu || deviceName.empty() ? std::string(stoodOnCpu ? "cpu" : "card") : deviceName;
        asked["device"] = deviceLabel;
        asked["sage_attn"] = sageAsked;
        asked["batch"] = img.batch_count;
        asked["refs"] = static_cast<int>(sources.size());
        if (def->video)
        {
          // the length of a clip is most of the work of a run, so it belongs to
          // what that run's cost is filed under, the way the side of a picture
          // does above.
          asked["frames"] = framesAsked;
          asked["fps"] = def->fps;
        }
        jobRecord["settings"] = asked;
      }

      // what this run draws when the model's own card says its work moves: the
      // words, the size, the settings and the stop of a run are the ones built
      // above - a clip is that same work, stood as frames rather than as one
      // picture. What a clip carries beyond it is its length, the rate it is
      // encoded at, and the frames it was handed to move from. A run that starts
      // from pictures starts a clip at those frames rather than passing them as
      // references, because that is how this kind of model is read: its card
      // says the first checkpoint takes a first frame and, beside it, a last one
      // (docs/minimax_h3.md names --init-img and --end-img), while a checkpoint
      // that works from references keeps the reference path above. The rate the
      // run answers with is the one the engine reports back rather than the one
      // that was asked, which is what the engine's notes say a model of this kind
      // does: it keeps the rate its weights were trained at.
      sd_vid_gen_params_t vid;
      sd_vid_gen_params_init(&vid);
      vid.prompt = img.prompt;
      vid.negative_prompt = img.negative_prompt;
      vid.clip_skip = img.clip_skip;
      vid.width = width;
      vid.height = height;
      vid.sample_params = sample;
      vid.sample_params.flow_shift = def->flow_shift;
      vid.seed = img.seed;
      vid.video_frames = framesAsked;
      vid.fps = def->fps;
      vid.cache = img.cache;
      vid.vae_tiling_params = img.vae_tiling_params;
      // how far an opening frame is followed is the one ask a run may name; left
      // unasked the engine's own starting strength is kept, which is the number
      // its command-line example leaves these checkpoints at.
      const double strengthAsked = request.value("strength", -1.0);
      if (strengthAsked >= 0.0)
        vid.strength = strengthAsked < 1.0 ? static_cast<float>(strengthAsked) : 1.0f;
      if (hasRef)
      {
        if (def->edit)
        {
          vid.init_image = refImages[0];
          if (refImages.size() > 1u)
            vid.end_image = refImages[1];
        }
        else
        {
          vid.ref_images = refImages.data();
          vid.ref_images_count = static_cast<int>(refImages.size());
        }
      }

      struct CancelProbe
      {
        std::string job;
        sd_ctx_t *ctx;
        int sampleSteps;
        int pictures;
        int picturesDone;
        int anchor; // the engine's own tick total for the trusted window
        bool full;  // the current picture's window was drawn whole
        int last;   // the last progress a page was told
      };
      CancelProbe probe{job, ctx, sample.sample_steps, img.batch_count, 0, 0, false, 0};
      auto thunk = [](int step, int stepsTotal, float, void *userData)
      {
        auto *probe = static_cast<CancelProbe *>(userData);
        const std::string jobId = probe->job;
        const int window = probe->sampleSteps > 0 ? probe->sampleSteps : 1;
        const int pictures = probe->pictures > 0 ? probe->pictures : 1;
        const int nominal = probe->sampleSteps > 0 ? probe->sampleSteps : 1;
        // This one hook hears every phase the engine reports, and every phase
        // climbs a ladder of its own: weight loading counts tensors by the
        // hundred and seventy, tile decoding counts tile passes, text encoding
        // counts its own. A bar that believed them all blinks to its last step
        // before the first real one, then hangs there until the file lands -
        // and one ladder looks exactly like the sampling from the inside: it
        // opens at zero and climbs to its whole. What sets the sampling apart
        // is the size of the ladder it climbs: it is the phase the run's
        // steps count, so its engine total reads the number of steps the ask
        // named, while the readying phases count bytes and tiles and end on no
        // such number. A ladder that long is the window and is believed from
        // its first tick; a ladder of any other count never teaches the page.
        // And when no ladder ever matches the steps - which the readying
        // phases never do and no scheduler seen here either - the honest
        // floor is a bar that holds at zero and turns to the picture when it
        // lands, rather than one that lies about a step it never took.
        if (step <= 0)
        {
          if (probe->anchor <= 0)
            noteProgress(jobId, 0, window * pictures); // the wait has a size
          else if (probe->full && stepsTotal == nominal &&
                   probe->picturesDone + 1 < pictures)
          {
            // A whole window behind it, this count opened at zero again: the
            // run's next picture.
            probe->picturesDone += 1;
            probe->full = false;
            probe->last = probe->picturesDone * window;
            noteProgress(jobId, probe->picturesDone * window, window * pictures);
          }
          // A stop asked after this run has already drawn pictures of its batch
          // is believed from the start of the next picture, so what came out
          // stays; a stop before the first picture is believed at once, there
          // being nothing yet to keep.
          if (cancelledRequested(jobId))
            sd_cancel_generation(probe->ctx, probe->picturesDone > 0 ? SD_CANCEL_NEW_LATENTS
                                                                     : SD_CANCEL_ALL);
          return;
        }
        if (probe->anchor <= 0)
        {
          // A window as long as the steps named, and seen from its first step:
          // an engine pass seen here opens at zero, completes to the step count
          // whole inside a heartbeat, and never walks the middle - a bar that
          // believed it jumped to its end seconds before the first real step
          // was drawn. Only a ladder that starts counting at one is the
          // sampling, for the sampler reports every step as it takes it; a
          // phase that skips its first step never teaches the page at all.
          if (stepsTotal == nominal && step == 1)
            probe->anchor = stepsTotal;
          else
          {
            if (cancelledRequested(jobId))
              sd_cancel_generation(probe->ctx, probe->picturesDone > 0 ? SD_CANCEL_NEW_LATENTS
                                                                       : SD_CANCEL_ALL);
            return; // some other phase's ladder, or one that skipped its start
          }
        }
        if (stepsTotal != probe->anchor || probe->full)
        {
          // A whole window takes no more ticks until its zero, and ladders
          // of another count are other phases - tiles and bytes are not steps.
          if (cancelledRequested(jobId))
            sd_cancel_generation(probe->ctx, probe->picturesDone > 0 ? SD_CANCEL_NEW_LATENTS
                                                                     : SD_CANCEL_ALL);
          return;
        }
        int within = static_cast<int>((static_cast<long long>(step) * window) /
                                      probe->anchor);
        if (within > window)
          within = window;
        if (within < 1)
          within = 1;
        const int done = probe->picturesDone * window + within;
        if (done > probe->last)
        {
          probe->last = done;
          noteProgress(jobId, done, window * pictures);
        }
        if (within >= window)
          probe->full = true;
        if (cancelledRequested(jobId))
          sd_cancel_generation(probe->ctx, probe->picturesDone > 0 ? SD_CANCEL_NEW_LATENTS
                                                                   : SD_CANCEL_ALL);
      };
      // Before the engine speaks at all, a page may have been waiting on this
      // job through the whole of its open: the line below names the window so
      // the bar stripes at zero rather than saying nothing.
      noteProgress(job, 0,
                   (sample.sample_steps > 0 ? sample.sample_steps : 1) *
                       (img.batch_count > 0 ? img.batch_count : 1));
      std::lock_guard<std::mutex> generationLock(gGenerationMutex);
      // A stop asked of this job while it waited behind the run before it is
      // believed here: the engine is not started for a picture nobody wants any
      // more. An ask of several pictures queues several jobs deep behind this
      // lock, and the cancel hand speaks to every job of the ask, so the ones
      // that never reached the engine end as stopped rather than as work nobody
      // asked for.
      if (cancelledRequested(job))
      {
        markState(job, ols::kStateCancelled);
        return;
      }
      sd_set_progress_callback(thunk, &probe);

      sd_image_t *images = nullptr;
      int produced = 0;
      bool ok = false;
      // a clip's sound, when one came out: part of what the model made rather
      // than something this machine adds afterwards, so it is kept beside the
      // frames and freed with them.
      sd_audio_t *clipAudio = nullptr;
      int encodedFps = 0;
      // the busy count lets go with its own guard above; what is freed here is
      // what the run allocated.
      struct Release
      {
        sd_image_t **images;
        int *produced;
        sd_audio_t **audio;
        ~Release()
        {
          sd_set_progress_callback(nullptr, nullptr);
          if (*audio)
            free_sd_audio(*audio);
          // What a run made is allocated inside the library, and its header says
          // plainly that the caller must hand it back through free_sd_images
          // ("the memory is allocated in the library but freed in the caller"),
          // include/stable-diffusion.h.
          if (*images)
            free_sd_images(*images, *produced);
        }
      } release{&images, &produced, &clipAudio};
      if (def->video)
        ok = generate_video(ctx, &vid, &images, &produced, &clipAudio, &encodedFps) && produced > 0;
      else
        ok = generate_image(ctx, &img, &images, &produced) && produced > 0;
      if (!ok)
      {
        if (cancelledRequested(job))
        {
          markState(job, ols::kStateCancelled);
          return;
        }
        // the engine said no. it now carries its own words - what it last
        // complained of is what this run died of, and a page that reads the
        // job should say so instead of a shrug.
        std::string words;
        {
          std::lock_guard<std::mutex> lock(gSdWordsMutex);
          words.swap(gSdWords);
        }
        const std::string bare = def->video ? "the clip did not come out" : "the picture did not come out";
        noteError(job, words.empty() ? bare : bare + ": " + words);
        markState(job, ols::kStateError);
        return;
      }

      const long long created = nowSeconds();
      // What a run cost and what it stood on, kept beside the file itself: the
      // wall a person waited from the moment the ask was taken, what opening
      // the model took of it, the card the work was done on, the card's whole
      // memory, and the quant the weights really ran as. The words are the
      // same readings the job record carries, so a file and its record can
      // never tell two different stories about one run.
      long long beganRun = 0;
      {
        std::lock_guard<std::mutex> lock(gMutex);
        const auto started = gStartedMs.find(job);
        if (started != gStartedMs.end())
          beganRun = started->second;
      }
      const json msValue = beganRun > 0 ? json(steadyMs() - beganRun) : json(nullptr);
      const long long vramBytes = cudaDeviceTotalBytes();
      const json vramValue = vramBytes > 0 ? json(vramBytes) : json(nullptr);
      const json quantValue = openedForm.empty() ? json("as shipped") : json(openedForm);
      if (def->video)
      {
        // the clip and the parts of it. The clip is the run's output and what a
        // person saves; the plain copy is the same frames with no codec to know,
        // for a reader that does not speak MPEG-J; the poster, the animated preview
        // and the track are what let a gallery full of clips say what each one is
        // without asking for all of a clip - one picture, a picture a browser moves
        // on its own, a sound a browser plays. They are named from the clip rather
        // than as outputs of their own, which is why the gallery never lists them as
        // things of their own (see outputItemNamed above).
        const int rate = encodedFps > 0 ? encodedFps : (vid.fps > 0 ? vid.fps : 1);
        const std::string base = "gen-" + job + "-0";
        const std::string clipName = base + ".avi";
        const std::string posterName = base + ".poster.png";
        const std::string previewName = base + ".preview.apng";
        const std::string trackName = base + ".wav";
        const std::string rawName = base + ".raw.avi";
        if (!ols::ensureParentDir(gOutputDir + "/" + clipName))
        {
          noteError(job, "the folder for the clip could not be made");
          markState(job, ols::kStateError);
          return;
        }
        int clipFrames = 0;
        const bool clipWritten =
            writeAviClip(gOutputDir + "/" + clipName, images, produced, rate, clipAudio, false,
                         &clipFrames);
        bool posterWritten = false;
        if (images[0].data)
          posterWritten = stbi_write_png((gOutputDir + "/" + posterName).c_str(),
                                         static_cast<int>(images[0].width), static_cast<int>(images[0].height),
                                         static_cast<int>(images[0].channel), images[0].data,
                                         static_cast<int>(images[0].width * images[0].channel)) != 0;
        const bool previewWritten =
            writeApngPreview(gOutputDir + "/" + previewName, images, produced, kPreviewFps, kPreviewSide);
        // The one number that says whether a clip moves: the mean change of a
        // picture between the frames it stands between, read off the pixels
        // the run made rather than off anyone's hopes. It is recorded because
        // a card that ends with five frames of a asked hundred and twenty-four
        // reads as a still to the eye whatever the model drew, and the file
        // should say what it holds. Measured 2026-11-19: clips that move stand
        // at three to twelve of 255 between neighbours; a still sits at zero.
        json motionValue = json(nullptr);
        if (produced > 1 && images[0].data)
        {
          double sum = 0.0;
          int pairs = 0;
          const long long pixels = static_cast<long long>(images[0].width) * images[0].height;
          const int chan = images[0].channel == 4u ? 4 : 3;
          for (int at = 1; at < produced; ++at)
          {
            if (!images[at].data || images[at].width != images[0].width ||
                images[at].height != images[0].height)
              continue;
            double here = 0.0;
            long long taken = 0;
            for (long long px = 0; px < pixels; px += 7)
            {
              const unsigned char *a = images[at - 1].data + px * chan;
              const unsigned char *b = images[at].data + px * chan;
              here += std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]);
              ++taken;
            }
            if (taken > 0)
            {
              sum += here / taken / 3.0;
              ++pairs;
            }
          }
          if (pairs > 0)
            motionValue = json(std::round(sum / pairs * 100.0) / 100.0);
        }
        // the plain-pixel clip costs a whole picture per frame, so it is only stood
        // where a person can still carry the file.
        const unsigned long long rawBytes =
            static_cast<unsigned long long>(produced) *
            (((images[0].width * 3u + 3u) & ~3u) * images[0].height);
        const bool rawWritten =
            rawBytes < 48ull * 1024ull * 1024ull &&
            writeAviClip(gOutputDir + "/" + rawName, images, produced, rate, clipAudio, true);
        const bool trackWritten = clipAudio ? writeWavTrack(gOutputDir + "/" + trackName, clipAudio) : false;
        if (!clipWritten)
        {
          noteError(job, "the clip did not come out as a file");
          markState(job, ols::kStateError);
          return;
        }
        // the file holds what it holds: a frame left out because it did not stand
        // at the size of the others is said here and carried into the record,
        // rather than the record naming frames the clip never got.
        if (clipFrames < produced)
        {
          noteStep(job, "the clip was stood with " + std::to_string(clipFrames) + " of the " +
                            std::to_string(produced) +
                            " frames the run made: a frame that did not stand at the size of the others is left out");
        }
        // the record of a clip says what it is made of, in the words this
        // machine's own file door answers with, so a page that holds the record
        // can reach the part it wants rather than the whole clip: a card draws
        // the poster, moves the preview, and offers the clip and the sound to a
        // person. What is absent is said as absent rather than guessed at.
        const json posterValue = posterWritten ? json(posterName) : json(nullptr);
        const json previewValue = previewWritten ? json(previewName) : json(nullptr);
        const json audioValue = trackWritten ? json(trackName) : json(nullptr);
        const json rawValue = rawWritten ? json(rawName) : json(nullptr);
        json record = {{"id", base},
                       {"file", clipName},
                       {"kind", "video"},
                       {"created", created},
                       {"model", modelId},
                       {"name", def->name},
                       {"width", images[0].width},
                       {"height", images[0].height},
                       {"prompt", prompt},
                       {"steps", steps},
                       {"seed", vid.seed},
                       {"frames", clipFrames},
                       {"motion", motionValue},
                       {"fps", rate},
                       {"duration_ms", static_cast<long long>(clipFrames) * 1000 / rate},
                       {"poster", posterValue},
                       {"preview", previewValue},
                       {"audio", audioValue},
                       {"raw", rawValue},
                       {"channels", clipAudio ? static_cast<long long>(clipAudio->channels) : 0},
                       {"sample_rate", clipAudio ? static_cast<long long>(clipAudio->sample_rate) : 0},
                       {"references", json(sources)},
                       {"quant", quantValue},
                       {"device", deviceLabel},
                       {"machine", ols::platform::hostName()},
                       {"vram_bytes", vramValue},
                       {"sampler", samplerUsed},
                       {"scheduler", schedulerUsed},
                       {"tiling", img.vae_tiling_params.enabled},
                       {"frames_asked", framesWanted},
                       {"ms", msValue},
                       {"open_ms", openMs}};
        writeFileText(gOutputDir + "/" + clipName + ".json", record.dump());
        noteResult(job, "/files/outputs/" + clipName);
        json event = {{"type", "video"},
                      {"clip", json("/files/outputs/" + clipName)},
                      {"poster", posterWritten ? json("/files/outputs/" + posterName) : json(nullptr)},
                      {"preview", previewWritten ? json("/files/outputs/" + previewName) : json(nullptr)},
                      {"audio", trackWritten ? json("/files/outputs/" + trackName) : json(nullptr)},
                      {"raw", rawWritten ? json("/files/outputs/" + rawName) : json(nullptr)},
                      {"frames", clipFrames},
                      {"fps", rate}};
        pushEvent(job, event.dump());
      }
      // the pictures of a run, when this run made pictures: a clip was stood as
      // its own files above.
      for (int i = 0; !def->video && i < produced; ++i)
      {
        sd_image_t &image = images[i];
        const std::string name = "gen-" + job + "-" + std::to_string(i) + ".png";
        const std::string png = gOutputDir + "/" + name;
        if (!ols::ensureParentDir(png))
          continue;
        if (!stbi_write_png(png.c_str(), static_cast<int>(image.width), static_cast<int>(image.height),
                            static_cast<int>(image.channel), image.data,
                            static_cast<int>(image.width * image.channel)))
        {
          continue;
        }
        json record = {{"id", name.substr(0, name.size() - 4)},
                       {"file", name},
                       {"kind", "image"},
                       {"created", created},
                       {"model", modelId},
                       {"name", def->name},
                       {"width", image.width},
                       {"height", image.height},
                       {"prompt", prompt},
                       {"steps", steps},
                       {"seed", img.seed},
                       {"references", json(sources)},
                       {"quant", quantValue},
                       {"device", deviceLabel},
                       {"machine", ols::platform::hostName()},
                       {"vram_bytes", vramValue},
                       {"sampler", samplerUsed},
                       {"scheduler", schedulerUsed},
                       {"tiling", img.vae_tiling_params.enabled},
                       {"ms", msValue},
                       {"open_ms", openMs}};
        writeFileText(gOutputDir + "/" + name + ".json", record.dump());
        noteResult(job, "/files/outputs/" + name);
        pushEvent(job, json{{"type", "image"}, {"url", "/files/outputs/" + name}}.dump());
      }
      pruneOutputs();
      markState(job, ols::kStateDone);
    }

    std::string ImageService::startJob(const std::string &kind, const std::string &modelId,
                                       const std::string &payloadJson)
    {
      std::string wanted = modelId;
      std::string quant;
      json ask;
      if (kind == "generate")
      {
        const json probe = json::parse(payloadJson, nullptr, false);
        if (probe.is_discarded())
        {
          gEngineError = "the body of the ask is not one this machine can read";
          return std::string();
        }
        ask = probe;
        // read the two names without trusting their types: this runs on the
        // thread that serves the request, and a body that words "model" as a
        // number must be answered with a refusal, not with an exception carried
        // up through the server.
        if (wanted.empty())
          wanted = ols::statedString(probe, "model");
        quant = ols::statedString(probe, "quant");
      }
      else if (!payloadJson.empty())
      {
        // a download and an open carry the same shape: which precision of the
        // model is meant, when a page named one.
        const json probe = json::parse(payloadJson, nullptr, false);
        if (!probe.is_discarded())
        {
          if (wanted.empty())
            wanted = ols::statedString(probe, "model");
          quant = ols::statedString(probe, "quant");
        }
      }
      if (kind != "download" && kind != "load" && kind != "generate")
      {
        gEngineError = "that kind of work is not one this machine runs: " + kind;
        return std::string();
      }
      if (wanted.empty())
      {
        gEngineError = "no model was named: the picture tables list what this machine holds";
        return std::string();
      }
      if (!findModel(wanted))
      {
        gEngineError = "that model is not one this machine knows: " + wanted;
        return std::string();
      }
      if (!quant.empty() && !formNamed(wanted, quant))
      {
        std::string names;
        if (const auto offers = findOffers(wanted))
        {
          for (const ModelForm &offer : *offers)
          {
            if (!names.empty())
              names += ", ";
            names += offer.label;
          }
        }
        gEngineError = names.empty() ? "that model is not offered in quants: it comes as one package"
                                     : "that name is not one " + wanted + " is offered as: it can be opened as " + names;
        return std::string();
      }
      if (!quant.empty())
      {
        std::lock_guard<std::mutex> lock(gMutex);
        ensureChosenLoadedLocked();
        gChosenQuant[wanted] = quant;
        // the next page, and the next start of this machine, keep the choice
        saveChosenLocked();
      }
      const std::string job = makeId("img");
      {
        std::lock_guard<std::mutex> lock(gMutex);
        // a render stands queued until its turn at the card comes; a download
        // and an open work the moment they are named.
        gJobs[job] = {{"id", job},
                      {"kind", kind},
                      {"model", wanted},
                      {"state", kind == "generate" ? std::string(ols::kStateQueued)
                                                   : std::string(ols::kStateRunning)},
                      {"started_at", nowSeconds()}};
        if (!quant.empty())
          gJobs[job]["quant"] = quant;
        if (kind == "generate" && ask.is_object())
        {
          // the ask as it was made, so a page can draw the picture's card -
          // its words, its settings, the backs it leans on - while its run
          // still stands in the line.
          json standing;
          for (const char *word : {"prompt", "negative_prompt"})
            if (ask.contains(word) && ask[word].is_string() && !ask[word].get<std::string>().empty())
              standing[word] = ask[word];
          for (const char *word : {"width", "height", "steps", "guidance", "seed", "frames",
                                   "fps", "strength", "count"})
            if (ask.contains(word) && ask[word].is_number())
              standing[word] = ask[word];
          if (ask.contains("references") && ask["references"].is_array())
          {
            json sources = json::array();
            json doors = json::array();
            for (const auto &source : ask["references"])
            {
              if (!source.is_string())
                continue;
              sources.push_back(source);
              const std::string found = resolveReference(source.get<std::string>());
              if (found.rfind(gRefsDir + "/", 0) == 0)
                doors.push_back("/files/refs/" + found.substr(gRefsDir.size() + 1));
              else if (found.rfind(gOutputDir + "/", 0) == 0)
                doors.push_back("/files/outputs/" + found.substr(gOutputDir.size() + 1));
            }
            if (!sources.empty())
              standing["refs"] = sources;
            if (!doors.empty())
              standing["ref_urls"] = doors;
          }
          if (!standing.empty())
            gJobs[job]["ask"] = standing;
        }
        gEvents.try_emplace(job);
        gCancel[job] = false;
        gJobOrder.push_back(job);
        gStartedMs[job] = steadyMs();
        pruneJobs();
      }
      if (kind == "download")
      {
        std::thread([job, wanted]()
                    {
      runJobGuarded(job, [&]() { doDownload(job, findModel(wanted)); });
      endJob(job); })
            .detach();
        return job;
      }
      if (kind == "load")
      {
        std::thread([job, wanted]()
                    {
      runJobGuarded(job, [&]() { doLoad(job, wanted); });
      endJob(job); })
            .detach();
        return job;
      }
      std::thread([job, wanted, payloadJson]()
                  {
    runJobGuarded(job, [&]() { doGenerate(job, wanted, payloadJson); });
    endJob(job); })
          .detach();
      return job;
    }

    std::string ImageService::jobsListJson() const
    {
      std::lock_guard<std::mutex> lock(gMutex);
      json out = json::array();
      for (const auto &entry : gJobs)
        out.push_back(entry.second);
      std::sort(out.begin(), out.end(), [](const json &a, const json &b)
                { return a.value("started_at", 0LL) > b.value("started_at", 0LL); });
      // the line at the card, as the machine now holds it: zero stands on the
      // card, and the queued ones are numbered from one in the order they will
      // go, whether or not anyone is on the card this instant.
      std::string holder;
      std::vector<std::string> line;
      for (const auto &name : gJobOrder)
      {
        const auto row = gJobs.find(name);
        if (row == gJobs.end() || row->second.value("kind", std::string()) != "generate")
          continue;
        const std::string state = ols::jobState(row->second);
        if (state == ols::kStateRunning && holder.empty())
          holder = name;
        else if (state == ols::kStateQueued)
          line.push_back(name);
      }
      const auto stamp = [&out](const std::string &name, int place)
      {
        for (json &entry : out)
          if (entry.value("id", std::string()) == name)
          {
            entry["queue_pos"] = place;
            return;
          }
      };
      if (!holder.empty())
        stamp(holder, 0);
      for (size_t n = 0; n < line.size(); ++n)
        stamp(line[n], static_cast<int>(n) + 1);
      return out.dump();
    }

    std::string ImageService::jobStateJson(const std::string &id) const
    {
      std::lock_guard<std::mutex> lock(gMutex);
      const auto found = gJobs.find(id);
      if (found == gJobs.end())
      {
        return json{{"id", id}, {"state", ols::kStateUnknown}}.dump();
      }
      return found->second.dump();
    }

    // What this machine's own work costs, as it stands on disk: every set of
    // settings it has run a job through to the end, with how many times, the mean
    // of those runs, their fastest and slowest, and what one step or one picture
    // costs at those settings. Nothing here is guessed - a set of settings nobody
    // has run is simply not in the answer, and a page says that rather than
    // inventing a number. Rows come newest first, so a page that wants only the
    // recent cost of one model can stop when it has it.
    std::string ImageService::timingsJson() const
    {
      std::lock_guard<std::mutex> lock(gTimingsMutex);
      ensureTimingsLoaded();
      json runs = json::array();
      json opens = json::array();
      for (const auto &entry : gTimings)
      {
        json row = entry.second;
        const long long count = row.value("count", 0LL);
        if (count <= 0)
          continue;
        const long long total = row.value("total_ms", 0LL);
        row["mean_ms"] = total / count;
        // what one sampling step costs is the whole run over the steps the run
        // really sampled: a run asked for its steps for every picture it was
        // asked to make in one call, so the window of one step is the mean
        // divided by the steps across all of them - dividing by one picture's
        // steps stated a step four times its cost for a run of four, and a
        // page's estimate of a batch was wrong by that same factor. the
        // division belongs here, where both counts are known, not where a page
        // would have to guess either of them.
        const long long steps = row.value("steps", 0LL);
        const long long pictures = row.value("batch", 1LL);
        const long long samples = steps * (pictures > 0 ? pictures : 1);
        if (samples > 0)
          row["mean_ms_per_step"] = row["mean_ms"].get<long long>() / samples;
        if (pictures > 1)
          row["mean_ms_per_picture"] = row["mean_ms"].get<long long>() / pictures;
        row["key"] = entry.first;
        if (row.value("kind", std::string()) == "load")
          opens.push_back(row);
        else
          runs.push_back(row);
      }
      const auto byNewest = [](const json &a, const json &b)
      { return a.value("updated", 0LL) > b.value("updated", 0LL); };
      std::sort(runs.begin(), runs.end(), byNewest);
      std::sort(opens.begin(), opens.end(), byNewest);
      return json{{"generations", runs}, {"loads", opens}, {"row_count", static_cast<int>(gTimings.size())}}.dump();
    }

    bool ImageService::cancelJob(const std::string &id)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      const auto found = gJobs.find(id);
      if (found == gJobs.end() || !ols::jobStanding(found->second))
        return false;
      gCancel[id] = true;
      // a job standing in the line for the card is woken to read its own stop;
      // one on the card reads it at its next step.
      gCondition.notify_all();
      return true;
    }

    bool ImageService::reorderJob(const std::string &id, int shift)
    {
      if (shift == 0)
        return false;
      std::lock_guard<std::mutex> lock(gMutex);
      const auto found = gJobs.find(id);
      if (found == gJobs.end() || found->second.value("kind", std::string()) != "generate" ||
          ols::jobState(found->second) != ols::kStateQueued)
        return false;
      // the line is the standing generate jobs in gJobOrder's order; a move
      // trades places with the one beside it and the line keeps its shape.
      std::vector<std::string> line;
      for (const auto &name : gJobOrder)
      {
        const auto row = gJobs.find(name);
        if (row == gJobs.end() || row->second.value("kind", std::string()) != "generate")
          continue;
        if (ols::jobState(row->second) == ols::kStateQueued)
          line.push_back(name);
      }
      const auto at = std::find(line.begin(), line.end(), id);
      if (at == line.end())
        return false;
      const int moved = static_cast<int>(at - line.begin()) + shift;
      if (moved < 0 || moved >= static_cast<int>(line.size()))
        return false;
      std::iter_swap(at, line.begin() + moved);
      auto next = line.begin();
      for (auto &name : gJobOrder)
      {
        const auto row = gJobs.find(name);
        if (row == gJobs.end() || row->second.value("kind", std::string()) != "generate")
          continue;
        if (ols::jobState(row->second) != ols::kStateQueued)
          continue;
        name = *next++;
      }
      gCondition.notify_all();
      return true;
    }

    void ImageService::streamEvents(void *client, const std::string &id)
    {
      std::unique_lock<std::mutex> lock(gMutex);
      if (!gJobs.count(id))
      {
        // A page that follows an id nothing holds is told so: a stream that
        // ends without a word reads as a job that finished with nothing to say.
        OpenLocalServices::writeStreamChunk(client,
                                            "data: " + json{{"type", "error"}, {"message", "no such job: " + id}}.dump() + "\n\n");
        return;
      }
      size_t sent = 0;
      while (true)
      {
        // look the job up afresh for every chunk: the room lets old jobs go
        // while this one waits and while a chunk is being written, and either
        // an iterator or a reference carried across the wait would be reading
        // a row that no longer stands. and looking it up with find, never with
        // [], which would mint an empty row for a job that has been swept away.
        bool alive = true;
        while (true)
        {
          const auto frames = gEvents.find(id);
          if (frames == gEvents.end())
            return;
          if (sent >= frames->second.size())
            break;
          const std::string frame = "data: " + frames->second[sent] + "\n\n";
          lock.unlock();
          alive = OpenLocalServices::writeStreamChunk(client, frame);
          lock.lock();
          if (!alive)
            return;
          ++sent;
        }
        const auto ended = gEnded.find(id);
        if (ended != gEnded.end() && ended->second)
          return;
        gCondition.wait_for(lock, std::chrono::seconds(1));
      }
    }

    // The name a page should mark as the form in use. A model's forms are named by
    // their label and opened by their slot, so the slot this machine would open
    // with is what says which label that is. A card that says only how many forms
    // a model comes in leaves a person pressing buttons to find out which one the
    // machine answers with.
    static std::string formInUse(const ImageModelDef &def, const std::string &chosen)
    {
      const auto offers = findOffers(def.id);
      if (!offers)
        return std::string();
      const ModelForm *form = formFor(&def, chosen);
      if (form)
        return form->label;
      // The row's own files are, for a model whose default form is the file the
      // row already names, one of the forms it is offered as - MiniMax H3's row
      // stands a Q4_K denoiser, and that is what the Q4_K form stands under. Say
      // that form, so a page never reads "no form in use" about bytes that are
      // one of the named forms.
      for (const ModelForm &offer : *offers)
      {
        if (filesAsForm(&def, &offer) == def.files)
          return offer.label;
      }
      return std::string();
    }

    std::string ImageService::modelsJson() const
    {
      json out = json::array();
      for (const ImageModelDef &def : kModelList())
      {
        // Every model a machine knows is offered whatever it may draw: nothing
        // here hides a card because of what a model tends to produce.
        std::string held;
        {
          std::lock_guard<std::mutex> lock(gMutex);
          ensureChosenLoadedLocked();
          const auto found = gChosenQuant.find(def.id);
          if (found != gChosenQuant.end())
            held = found->second;
        }
        // the names this model's files stand under, in the same words every other
        // table gives them: a path beside the weights room. on_disk is then said
        // of these very files - a card that lists one precision and reports on
        // the bytes of another is the disagreement a page cannot argue with.
        const std::vector<std::pair<std::string, std::string>> listedFiles =
            modelFiles(&def, held);
        const bool present = filesProved(&def, listedFiles);
        long long bytes = 0;
        // what of this model physically sits in the room, proved or not: a model
        // whose files were copied in by hand has its bytes on this machine, and a
        // page that only ever heard on_disk: false called that not downloaded and
        // offered to fetch them again.
        long long lyingHere = 0;
        bool unchecked = false;
        json files = json::array();
        for (const auto &file : listedFiles)
        {
          const std::string path = ols::modelFilePath(gWeightsDir, def.id, localNameOf(file.second));
          files.push_back(def.id + "/" + localNameOf(file.second));
          std::error_code ec{};
          const auto size = std::filesystem::file_size(path, ec);
          if (ec)
            continue;
          lyingHere += static_cast<long long>(size);
          if (!provedHere(path))
            unchecked = true;
          else
            bytes += static_cast<long long>(size);
        }
        // what else this model's parts come as. The sizes are read from
        // what is on this machine, and for a file that is not here from the
        // listing this machine already holds beside its weights; where neither
        // states a size, none is named.
        json quants = json::array();
        const auto offers = findOffers(def.id);
        if (offers)
        {
          // the bytes one file of a form costs: these bytes if they stand here,
          // else the size the listing beside the weights states, else nothing.
          const std::string modelRoom = ols::modelFileDir(gWeightsDir, def.id);
          const auto bytesOf = [&modelRoom, &def](const std::string &slot) -> long long
          {
            std::error_code ec{};
            const auto size = std::filesystem::file_size(modelRoom + localNameOf(slot), ec);
            if (!ec)
              return static_cast<long long>(size);
            const std::string repo = slotRepo(slot).empty() ? def.repo : slotRepo(slot);
            std::string flat = repo;
            for (char &c : flat)
            {
              if (c == '/')
                c = '_';
            }
            const std::string treePath = treePathOf(slot);
            for (const auto &entry : repoListingCached(flat))
            {
              if (ols::statedString(entry, "path") != treePath)
                continue;
              const long long stated = ols::statedCount(entry, "size");
              if (stated > 0)
                return stated;
            }
            return 0;
          };
          for (const ModelForm &offer : *offers)
          {
            // only the files a form stands under say whether that form is here:
            // the rest of its files are the row's own, and those are asked about
            // by the model's own row already.
            bool here = true;
            bool standing = true;
            long long total = 0;
            for (const auto &file : offer.files)
            {
              if (!slotOnDisk(&def, file.second))
                here = false;
              std::error_code ec{};
              if (std::filesystem::file_size(modelRoom + localNameOf(file.second), ec))
                standing = false;
              total += bytesOf(file.second);
            }
            json one = {{"name", offer.label}, {"here", here}};
            one["present"] = standing;
            if (total > 0)
              one["bytes"] = total;
            quants.push_back(one);
          }
        }
        out.push_back({{"id", def.id},
                       {"name", def.name},
                       {"repo", def.repo},
                       {"steps", def.steps},
                       {"guidance", def.guidance},
                       // The size, sampler, scheduler and cache policy this
                       // model's own card names (a size of 0 states none), so a
                       // page asks for nothing and draws what the machine says.
                       {"width", def.width},
                       {"height", def.height},
                       {"sampler", def.sampler},
                       {"scheduler", def.scheduler},
                       {"cache", def.cache},
                       // Whether this machine's card can carry the math these
                       // weights are made of: int8-convrot below compute 7.5
                       // means weights the card cannot run at a usable speed,
                       // and a page should say that plainly rather than call an
                       // hour-long crawl ready. A machine with no cuda card to
                       // answer (the cpu shape) holds its own road.
                       {"int8_convrot", def.int8_convrot},
                       {"card_holds",
                        !(def.int8_convrot && cardAnswer().computeMajor >= 0 &&
                          (cardAnswer().computeMajor < 7 ||
                           (cardAnswer().computeMajor == 7 && cardAnswer().computeMinor < 5)))},
                       {"clip_skip", def.clip_skip},
                       {"flow_shift", def.flow_shift == INFINITY ? -1.0f : def.flow_shift},
                       {"prompt", def.prompt},
                       {"specialty", def.specialty},
                       {"negative", def.negative},
                       {"sigmas", def.sigmas},
                       {"edit", def.edit},
                       {"redraw", def.redraw},
                       {"refs", def.refs},
                       {"video", def.video},
                       {"frames", def.frames},
                       {"fps", def.fps},
                       {"files", files},
                       {"on_disk", present},
                       {"bytes", bytes},
                       // present_bytes: what of this model sits in the room whether or
                       // not anything has proved it. unchecked: some of it is here with
                       // nothing stood behind it. Both are said apart from on_disk so a
                       // machine holding the bytes is never called a machine without them.
                       {"present_bytes", lyingHere},
                       {"unchecked", unchecked},
                       {"quants", quants},
                       {"default_quant", defaultQuantFor(def.id)},
                       {"asked_quant", held}});
        out.back()["uses_quant"] = formInUse(def, held);
      }
      return json{{"models", out}}.dump();
    }

    std::string ImageService::statusJson() const
    {
      int active = 0;
      json generating = json::array();
      json loaded = json::array();
      json resident = json::array();
      long long bytes = 0;
      std::lock_guard<std::mutex> jobLock(gMutex);
      for (const auto &entry : gJobs)
      {
        if (ols::jobStanding(entry.second))
          ++active;
      }
      std::lock_guard<std::mutex> lock(gCtxMutex);
      std::vector<std::pair<uint64_t, std::string>> order;
      for (const auto &entry : gCtx)
      {
        if (!entry.second)
          continue;
        loaded.push_back(entry.first);
        order.push_back({gCtxUse.count(entry.first) ? gCtxUse[entry.first] : 0, entry.first});
        const long long charge = gCtxBytes.count(entry.first) ? gCtxBytes[entry.first] : 0;
        bytes += charge;
        // Which part of the machine this open model works on, named beside its
        // bytes: a page that says a model is open can say where it stands. And
        // the quant it stands open as: the form this very context was opened
        // under (gCtxForm, resolved by every open - a chosen form, else the
        // model's default, else the form its own files answer with), and where
        // no form resolves at all the same two words the job records use:
        // "as shipped". An open model always states what it holds open.
        const auto stands = gCtxForm.find(entry.first);
        const std::string openQuant = stands == gCtxForm.end() || stands->second.empty()
                                          ? std::string("as shipped")
                                          : stands->second;
        resident.push_back({{"model", entry.first},
                            {"bytes", charge},
                            {"device", (gCtxCpu.count(entry.first) && gCtxCpu[entry.first]) ? "cpu" : "card"},
                            {"quant", openQuant}});
        const auto busy = gGenerating.find(entry.first);
        if (busy != gGenerating.end() && busy->second > 0)
          generating.push_back(entry.first);
      }
      std::sort(order.begin(), order.end());
      json lru = json::array();
      for (const auto &entry : order)
        lru.push_back(entry.second);
      return json{{"loaded", loaded},
                  {"generating", generating},
                  {"active", active},
                  {"resident_bytes", bytes},
                  {"resident", resident},
                  {"budget_bytes", memoryBudgetBytes()},
                  {"lru", lru}}
          .dump();
    }

    std::string ImageService::devicesJson() const
    {
      size_t needed = sd_list_devices(nullptr, 0);
      if (needed == 0)
        return "[]";
      std::vector<char> buffer(needed + 1, 0);
      sd_list_devices(buffer.data(), buffer.size());
      json out = json::array();
      std::istringstream lines(std::string{buffer.data()});
      std::string line;
      while (std::getline(lines, line))
      {
        const size_t tab = line.find('\t');
        const std::string name = tab == std::string::npos ? line : line.substr(0, tab);
        const std::string description = tab == std::string::npos ? std::string() : line.substr(tab + 1);
        if (name.empty())
          continue;
        const std::string key = lowerText(name);
        out.push_back({{"name", name},
                       {"description", description},
                       {"type", key.rfind("cpu", 0) == 0 ? "cpu" : "gpu"}});
      }
      return out.dump();
    }

    bool ImageService::known(const std::string &modelId) const
    {
      for (const ImageModelDef &def : kModelList())
      {
        if (def.id == modelId)
          return true;
      }
      return false;
    }

    bool ImageService::unload(const std::string &modelId)
    {
      // letting a model go hands its weights back to the device: this road
      // stands on the card first, whatever run is drawing with it now.
      std::lock_guard<std::mutex> device(ols::deviceCard());
      std::lock_guard<std::mutex> ctxLock(gCtxMutex);
      const auto found = gCtx.find(modelId);
      if (found == gCtx.end() || !found->second)
        return false;
      const auto busy = gGenerating.find(modelId);
      if (busy != gGenerating.end() && busy->second > 0)
      {
        gPendingErase[modelId] = true;
        return true;
      }
      closeContextLocked(modelId);
      return true;
    }

    std::string ImageService::prefsJson() const
    {
      loadPrefs();
      std::lock_guard<std::mutex> lock(gMutex);
      json doc = json::object();
      for (const std::string &key : kPrefKeys())
      {
        doc[key] = gPrefs.count(key) ? gPrefs[key] == "1" : false;
      }
      doc["model"] = gPrefs.count("model") ? gPrefs["model"] : std::string();
      return doc.dump();
    }

    bool ImageService::setPrefs(const std::string &bodyJson)
    {
      const json doc = json::parse(bodyJson, nullptr, false);
      if (doc.is_discarded() || !doc.is_object())
        return false;
      loadPrefs();
      std::lock_guard<std::mutex> lock(gMutex);
      bool touched = false;
      for (const auto &entry : doc.items())
      {
        const std::string key = entry.key();
        if (key == "model")
        {
          if (!entry.value().is_string())
            continue;
          if (!known(entry.value().get<std::string>()))
            continue;
          gPrefs["model"] = entry.value().get<std::string>();
          touched = true;
          continue;
        }
        // a switch this machine does not keep is passed over, not answered as a
        // failure: a page sends the switches it keeps standing, and one of them
        // growing old on this machine must not cost the rest of them.
        if (!prefKeyNamed(key))
          continue;
        // a switch says what it means as a boolean, a number or the words one
        // and zero; a value worded no way a switch speaks is passed over too,
        // rather than taken for the answer that is not it.
        if (entry.value().is_boolean())
        {
          gPrefs[key] = entry.value().get<bool>() ? "1" : "0";
          touched = true;
        }
        else if (entry.value().is_number())
        {
          gPrefs[key] = entry.value().get<double>() != 0.0 ? "1" : "0";
          touched = true;
        }
        else if (entry.value().is_string())
        {
          const std::string word = entry.value().get<std::string>();
          if (word == "1" || word == "true")
          {
            gPrefs[key] = "1";
            touched = true;
          }
          else if (word == "0" || word == "false")
          {
            gPrefs[key] = "0";
            touched = true;
          }
        }
      }
      if (!touched)
        return false;
      return savePrefsLocked();
    }

    // The gallery page, newest first, one window of it at a time. The records are
    // ordered by the moment each was written, so a page that starts deep in the
    // room still costs one directory listing and only the reads of the window it
    // asked for: a gallery that holds ten thousand pictures costs the same handful
    // of file reads per page as one that holds forty. What the answer states is the
    // window it served and how many records the machine holds in all, so a reader
    // that took a shorter window can say what it left out rather than look like it
    // had seen everything.
    std::string ImageService::galleryJson(int limit, int offset) const
    {
      if (limit < 0)
        limit = 0;
      if (limit > gKeptMax)
        limit = gKeptMax;
      if (offset < 0)
        offset = 0;
      json items = json::array();
      std::error_code ec{};
      if (!std::filesystem::exists(gOutputDir, ec))
        return json{{"total", 0}, {"offset", 0}, {"items", items}}.dump();
      struct Kept
      {
        std::uintmax_t written;
        std::string path;
        std::string name;
      };
      std::vector<Kept> newest;
      for (const auto &entry : std::filesystem::directory_iterator(gOutputDir, ec))
      {
        const std::string name = entry.path().filename().string();
        if (name.size() < 5 || name.compare(name.size() - 5, 5, ".json") != 0)
          continue;
        std::error_code when{};
        const auto written = std::filesystem::last_write_time(entry.path(), when);
        if (when)
          continue;
        newest.push_back(Kept{static_cast<std::uintmax_t>(written.time_since_epoch().count()),
                              entry.path().string(), name});
      }
      std::sort(newest.begin(), newest.end(), [](const Kept &a, const Kept &b)
                { return a.written > b.written; });
      // how many records this machine holds, counted from the listing that just
      // ran, so a page that took a shorter window can say what it left out rather
      // than look like it had seen everything.
      const long long total = static_cast<long long>(newest.size());
      // A little headroom, so a record that cannot be read does not leave the
      // page short of the pictures that can.
      const size_t from = static_cast<size_t>(offset) < newest.size()
                              ? static_cast<size_t>(offset)
                              : newest.size();
      const size_t past = from + static_cast<size_t>(limit) + 16u;
      const size_t to = past < newest.size() ? past : newest.size();
      for (size_t i = from; i < to; ++i)
      {
        const Kept &kept = newest[i];
        std::ifstream in{kept.path, std::ios::binary};
        json record = json::parse(std::string{(std::istreambuf_iterator<char>{in}),
                                              std::istreambuf_iterator<char>{}},
                                  nullptr, false);
        if (record.is_discarded())
          continue;
        record["url"] = "/files/outputs/" + kept.name.substr(0, kept.name.size() - 5);
        items.push_back(record);
      }
      // a record's moment is whole seconds, and a run's pictures can all be
      // written inside one of them: the sort keeps the newest-file-first order
      // the walk already gave these records behind a tie, so every page of the
      // gallery is a cut of one settled order and no picture lands on two pages
      // - or on none.
      std::stable_sort(items.begin(), items.end(), [](const json &a, const json &b)
                       { return a.value("created", 0LL) > b.value("created", 0LL); });
      json trimmed = json::array();
      for (int i = 0; i < limit && i < static_cast<int>(items.size()); ++i)
      {
        trimmed.push_back(items[i]);
      }
      return json{{"total", total},
                  {"offset", static_cast<long long>(from)},
                  {"items", trimmed}}
          .dump();
    }

    // Whether a name a page gave can stand as one of this machine's files: the
    // door reads inside the folder its kind names and nowhere else. No step out
    // through "..", no backslash, no colon - a drive letter or an alternate
    // stream hides behind one - and no dot at the start, which is how the room's
    // own records (its chosen precisions, its switches) are named. A slash may
    // stand inside a name, for a thing that lives in a folder of its own: a
    // model's files are named model/file, and it is the model's folder all the
    // same. One rule serves every door that names a file.
    static bool safeItemName(const std::string &name)
    {
      if (name.empty() || name.front() == '/' || name.front() == '\\' || name.front() == '.')
        return false;
      return name.find("..") == std::string::npos && name.find('\\') == std::string::npos &&
             name.find(':') == std::string::npos;
    }

    // one gallery record by its id, for a page that already holds the id and
    // wants that record alone rather than the whole list
    std::string ImageService::galleryItemJson(const std::string &id) const
    {
      if (!safeItemName(id))
        return std::string();
      // a record hangs off the file it describes, so the id of an output is
      // looked up at each ending a record can hang behind: a page that holds an
      // id does not say whether it named a picture or a clip.
      for (const std::string &ending : kOutputEndings)
      {
        std::ifstream in{gOutputDir + "/" + id + ending + ".json", std::ios::binary};
        json record = json::parse(std::string{(std::istreambuf_iterator<char>{in}),
                                              std::istreambuf_iterator<char>{}},
                                  nullptr, false);
        if (record.is_discarded())
          continue;
        record["url"] = "/files/outputs/" + id + ending;
        return record.dump();
      }
      return std::string();
    }

    bool ImageService::removeOutput(const std::string &id)
    {
      if (!safeItemName(id))
        return false;
      for (const std::string &ending : kOutputEndings)
      {
        std::error_code ec{};
        const std::string itemName = id + ending;
        if (!std::filesystem::exists(gOutputDir + "/" + itemName + ".json", ec))
          continue;
        for (const std::string &name : outputArtifactNames(itemName))
          std::filesystem::remove(gOutputDir + "/" + name, ec);
        return true;
      }
      return false;
    }

    std::string ImageService::refsJson() const
    {
      json items = json::array();
      std::error_code ec{};
      if (std::filesystem::exists(gRefsDir, ec))
      {
        for (const auto &entry : std::filesystem::directory_iterator(gRefsDir, ec))
        {
          const std::string name = entry.path().filename().string();
          if (name.size() < 5 || name.compare(name.size() - 5, 5, ".json") != 0)
            continue;
          std::ifstream in{entry.path(), std::ios::binary};
          json record = json::parse(std::string{(std::istreambuf_iterator<char>{in}),
                                                std::istreambuf_iterator<char>{}},
                                    nullptr, false);
          if (record.is_discarded())
            continue;
          record["url"] = "/files/refs/" + name.substr(0, name.size() - 5);
          items.push_back(record);
        }
      }
      // pictures kept in the same second stand in the order the room gave them;
      // a sort free to shuffle ties would move them between two answers.
      std::stable_sort(items.begin(), items.end(), [](const json &a, const json &b)
                       { return a.value("created", 0LL) > b.value("created", 0LL); });
      return json{{"items", items}}.dump();
    }

    // Keep a picture on disk for later renders to look at: either a render from the
    // gallery, or any image file on this machine.
    // a kept picture can fail for two different reasons, and they need
    // different words: the source was not there, or this folder belongs to
    // somebody else and the copy had nowhere to go
    static std::mutex gRefErrorMutex;
    static std::string gRefError;

    bool ImageService::addRef(const std::string &source, const std::string &name)
    {
      {
        std::lock_guard<std::mutex> lock(gRefErrorMutex);
        gRefError.clear();
      }
      if (source.empty())
      {
        std::lock_guard<std::mutex> lock(gRefErrorMutex);
        gRefError = "what to keep was not named";
        return false;
      }
      std::error_code ec{};
      std::string from;
      // What a person keeps as a reference is the thing a later run can start
      // from. A picture is that thing itself. A clip is not: what a model that
      // moves can start from is the still first frame the machine wrote beside it
      // - the poster - so keeping a clip keeps that frame, and the record names
      // the clip it came from. Only where no such still was written does the
      // clip itself stand, and a run that was then given it says what it did.
      const std::string picture = gOutputDir + "/" + source + ".png";
      if (std::filesystem::exists(picture, ec))
        from = picture;
      if (from.empty())
      {
        // the still a run wrote beside a clip is named from the run itself
        // (`<run>.poster.png`), which is what the clip record stores; the older
        // spelling that carries the container is tried only so a room written
        // before this change still gives its keep a still to stand on.
        for (const std::string &suffix : {std::string(".poster.png"), std::string(".avi.poster.png")})
        {
          const std::string still = gOutputDir + "/" + source + suffix;
          if (std::filesystem::exists(still, ec))
          {
            from = still;
            break;
          }
        }
      }
      if (from.empty())
      {
        for (const std::string &ending : kOutputEndings)
        {
          const std::string item = gOutputDir + "/" + source + ending;
          if (std::filesystem::exists(item, ec))
          {
            from = item;
            break;
          }
        }
      }
      if (from.empty() && ols::fileExists(source))
        from = source;
      if (from.empty())
      {
        std::lock_guard<std::mutex> lock(gRefErrorMutex);
        gRefError = "nothing by that name is here to keep";
        return false;
      }
      // keep the real extension of the picture (.png, .jpg, .jpeg, .webp):
      // cutting at a fixed four characters drops the dot of .jpeg/.webp and
      // writes a name the ref list cannot read back
      std::string ext = ".png";
      const size_t dot = from.rfind('.');
      const size_t sep = from.find_last_of("/\\");
      if (dot != std::string::npos && (sep == std::string::npos || dot > sep) &&
          from.size() - dot >= 4 && from.size() - dot <= 6)
      {
        const std::string cand = lowerText(from.substr(dot));
        bool sane = cand[0] == '.';
        for (size_t i = 1; i < cand.size() && sane; ++i)
          sane = cand[i] >= 'a' && cand[i] <= 'z';
        if (sane)
          ext = cand;
      }
      const long long created = nowSeconds();
      const std::string id = makeId("ref");
      const std::string kept = gRefsDir + "/" + id + ext;
      if (!ols::ensureParentDir(kept))
      {
        {
          std::lock_guard<std::mutex> lock(gRefErrorMutex);
          gRefError = "this machine could not make a place for the kept picture";
        }
        return false;
      }
      std::error_code copyEc{};
      if (!std::filesystem::copy_file(from, kept,
                                      std::filesystem::copy_options::overwrite_existing, copyEc))
      {
        {
          std::lock_guard<std::mutex> lock(gRefErrorMutex);
          gRefError = "this machine could not carry the picture into the kept-picture room: " +
                      copyEc.message();
        }
        return false;
      }
      const json record = {{"id", id},
                           {"file", id + ext},
                           {"name", name.empty() ? id : name},
                           {"created", created},
                           {"source", source}};
      if (!writeFileText(gRefsDir + "/" + id + ext + ".json", record.dump()))
      {
        {
          std::lock_guard<std::mutex> lock(gRefErrorMutex);
          gRefError = "this machine could not write the record that stands beside the kept picture";
        }
        return false;
      }
      pruneRefs();
      return true;
    }

    // a picture the browser is holding - a file dragged onto the page, a pasted
    // screenshot - has no path on this machine, so it arrives as its own bytes
    // in image_b64, with the format the browser says the file is. the studio
    // shrinks it before sending, so what arrives here fits one request body.
    bool ImageService::addRefBytes(const std::string &name, const std::string &imageB64,
                                   const std::string &format)
    {
      {
        std::lock_guard<std::mutex> lock(gRefErrorMutex);
        gRefError.clear();
      }
      std::string ext = ".png";
      const std::string want = lowerText(format);
      if (want.empty() || want == "png")
        ext = ".png";
      else if (want == "jpg" || want == "jpeg")
        ext = ".jpg";
      else if (want == "webp")
        ext = ".webp";
      else
      {
        {
          std::lock_guard<std::mutex> lock(gRefErrorMutex);
          gRefError = "a kept picture comes as a png, a jpg or a webp";
        }
        return false;
      }
      const std::string bytes = ols::decodeBase64(imageB64);
      if (bytes.empty())
      {
        {
          std::lock_guard<std::mutex> lock(gRefErrorMutex);
          gRefError = "no picture came with the ask";
        }
        return false;
      }
      // the name of one kept picture is its id, and the bytes are what the
      // browser sent: past what a request body may carry there is nothing to
      // keep, and the page says so rather than storing half a picture
      static const unsigned long long kMaxKeptBytes = 2ull * 1024ull * 1024ull;
      if (bytes.size() > kMaxKeptBytes)
      {
        {
          std::lock_guard<std::mutex> lock(gRefErrorMutex);
          gRefError = "Picture exceeds the keep limit (2 MB)";
        }
        return false;
      }
      const long long created = nowSeconds();
      const std::string id = makeId("ref");
      std::error_code ec{};
      const std::string file = gRefsDir + "/" + id + ext;
      if (!writeFileText(file, bytes))
      {
        {
          std::lock_guard<std::mutex> lock(gRefErrorMutex);
          gRefError = "this machine could not write the kept picture";
        }
        return false;
      }
      const json record = {{"id", id},
                           {"file", id + ext},
                           {"name", name.empty() ? id : name},
                           {"created", created},
                           {"source", "kept in the browser"}};
      if (!writeFileText(file + ".json", record.dump()))
      {
        std::filesystem::remove(file, ec);
        {
          std::lock_guard<std::mutex> lock(gRefErrorMutex);
          gRefError = "this machine could not write the record that stands beside the kept picture";
        }
        return false;
      }
      pruneRefs();
      return true;
    }

    std::string ImageService::lastRefError() const
    {
      std::lock_guard<std::mutex> lock(gRefErrorMutex);
      return gRefError;
    }

    bool ImageService::removeRef(const std::string &id)
    {
      if (!safeItemName(id))
        return false;
      std::error_code ec{};
      bool removed = false;
      // a kept picture carries the ending of what it is, and a clip that stood
      // with no still of its own was kept as itself - so the removal tries every
      // ending this machine can write a kept thing under, not just the pictures.
      std::vector<std::string> endings{".png", ".jpg", ".jpeg", ".webp"};
      for (const std::string &ending : kOutputEndings)
      {
        if (std::find(endings.begin(), endings.end(), ending) == endings.end())
          endings.push_back(ending);
      }
      for (const std::string &ext : endings)
      {
        const std::string path = gRefsDir + "/" + id + ext;
        if (std::filesystem::remove(path + ".json", ec))
          removed = true;
        std::filesystem::remove(path, ec);
      }
      return removed;
    }

    bool ImageService::file(const std::string &kind, const std::string &name, std::string &path,
                            std::string &contentType) const
    {
      if (!safeItemName(name))
        return false;
      // the doors this one opens, named: any other word names no folder, and a
      // request for it is refused rather than walked into the weights room.
      std::string base;
      if (kind == "outputs")
        base = gOutputDir;
      else if (kind == "refs")
        base = gRefsDir;
      else if (kind == "weights")
        base = gWeightsDir;
      else
        return false;
      path = base + "/" + name;
      if (!ols::fileExists(path))
        return false;
      contentType = contentTypeForName(name);
      return true;
    }

    std::string ImageService::engineError() const
    {
      return gEngineError;
    }

    ImageService::ImageService(const std::string &weightsDir, const std::string &outputDir, int32_t threads,
                               const std::string &hfToken, const std::string &civitaiKey, int32_t keptMax)
    {
      gWeightsDir = weightsDir;
      gOutputDir = outputDir;
      {
        const std::filesystem::path out{outputDir};
        const std::filesystem::path data = out.parent_path();
        gRefsDir = (data / "refs").string();
      }
      gThreads = threads;
      {
        std::lock_guard<std::mutex> lock(gKeysMutex);
        gHfToken = hfToken;
        gCivitaiKey = civitaiKey;
      }
      // A room was set for a size; a number that is not a size leaves the default
      // standing rather than a room that holds nothing.
      gKeptMax = keptMax > 0 ? keptMax : kGalleryCeilingDefault;
      // Nothing here makes a folder: a machine that never writes a picture, a
      // kept reference, or a record leaves no tree behind, and a mistaken launch
      // costs nothing on disk. The folders come into being with the first thing
      // that is written into them - and so do the weights room's records, which
      // the room itself reads the first time anything asks it a question.
    }

    void ImageService::setHfToken(const std::string &token)
    {
      std::lock_guard<std::mutex> lock(gKeysMutex);
      gHfToken = token;
    }

    void ImageService::setCivitaiKey(const std::string &token)
    {
      std::lock_guard<std::mutex> lock(gKeysMutex);
      gCivitaiKey = token;
    }

  } // namespace imagegen
} // namespace ols
