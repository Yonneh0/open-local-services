#include "ImageService.h"
#include "OpenLocalServices.h"
#include "Common.h"
#include "VerifiedRoom.h"

#include <stable-diffusion.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
// stb_image's code is not carried here: the voice engine's own part of this
// executable already implements it, and one definition of it serves both. Only
// the header's declarations are needed to read pictures here.
#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <condition_variable>
#include <filesystem>
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

#ifdef OLS_CUDART_LINKED
// this build links the CUDA runtime, so its own question about the GPU can be
// asked directly; the attribute numbers are part of the runtime's stable ABI
extern "C" int cudaDeviceGetAttribute(int *value, int attr, int device);
#endif

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
      // Whether a step cache has room in this model's steps. A distilled model
      // runs the few steps its distillation was trained for, and a cache that
      // starts at fifteen percent of them and stops at ninety skips work those
      // steps were the whole sampling for.
      bool cache = true;
      // How many CLIP text-blocks to stop after. -1 leaves the engine's own
      // default standing, which is 2 for SD 2 and SDXL and 1 for SD 1.5
      // (src/conditioning/conditioner.hpp:459). A card that names the number says
      // it here, because the default answers for the family, not for the model.
      int clip_skip = -1;
      // The time shift a flow-matching model samples on. INFINITY leaves the
      // engine's family default standing - set_flow_shift answers with
      // default_flow_shift when it is handed INFINITY
      // (src/pipeline/diffusion_engine.cpp:3187-3188) - and that default is 3.0
      // for qwen-image, z-image and SD3.5, 6.93 for FLUX.3, and for FLUX and
      // FLUX.2 a 1.0 the engine's own source marks "TODO: validate"
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
          {"qwen-image", "legible text, handwriting, and Chinese characters"},
          {"sdxl-turbo", "fast product, food, and cartoon shapes in one or four steps"},
          {"realvisxl-v5-lightning", "photoreal portraits and candid photos in four steps, honest skin"},
          {"pony-diffusion-v6", "anime and game illustration written in tag style"},
          {"cyberrealistic-xl", "photoreal people, portraits and full body"},
          {"animagine-xl-3.1", "anime illustration in several genres, with score tags"},
          {"lustify-sdxl", "photoreal women; guided, SFW photography, objects, animals, fantasy"},
          {"majicmix-realistic-v7", "photoreal skin and faces, dark low-key scenes"},
          {"juggernaut-xl-v9", "photoreal portrait, architecture, car, food, interior, landscape"},
          {"sd35-medium", "typography, diagrams, and short legible words"},
          {"flux-2-klein-4b", "photoreal images and multi-reference editing in four steps"},
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
    // the FLUX.2 klein card states multi-reference editing without naming a
    // number so it is held to the same ten, and the original Qwen Image Edit
    // is shown with one input picture. a redraw model takes one picture and
    // changes it.
    static const std::vector<std::tuple<std::string, std::string, int>> &kCapability()
    {
      static const std::vector<std::tuple<std::string, std::string, int>> rows = {
          {"flux-3-klein", "edit", 10},
          {"qwen-image-2.1", "edit", 10},
          {"qwen-image-2.1-turbo", "edit", 10},
          {"qwen-image", "edit", 1},
          {"flux-2-klein-4b", "edit", 10},
          {"z-image-turbo", "redraw", 1},
          {"sdxl-turbo", "redraw", 1},
          {"realvisxl-v5-lightning", "redraw", 1},
          {"pony-diffusion-v6", "redraw", 1},
          {"cyberrealistic-xl", "redraw", 1},
          {"animagine-xl-3.1", "redraw", 1},
          {"lustify-sdxl", "redraw", 1},
          {"majicmix-realistic-v7", "redraw", 1},
          {"juggernaut-xl-v9", "redraw", 1},
          {"sd35-medium", "redraw", 1},
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
          // SDXL Turbo: the card states 512x512 - "Preferably, the model
          // generates images of size 512x512", and its own limitations name "a
          // fixed resolution (512x512 pix)" - one step enough and four better,
          // and no guidance_scale and no negative_prompt at all ("we disable it
          // with guidance_scale=0.0"). The repo ships an Euler-Ancestral
          // scheduler with trailing spacing, which is the grid sgm_uniform
          // draws; four steps give a step cache nothing it may skip, so the
          // cache is off and only the free conditioning cache is kept.
          {"sdxl-turbo", 512, 512, "euler_a", "sgm_uniform", false},
          // FLUX.3 Klein: its card's own table states the distilled checkpoint at 16
          // steps with no guidance at all ("Klein | distilled, bfloat16 | 16 | none"),
          // and the engine's FLUX.3 page names euler with the default discrete
          // scheduler carrying the upstream time shift of 6.93 - so the scheduler is
          // left to the engine, which knows this family. Its samples and its memory
          // figure are stated at 1024x1024, which is the size it draws at. "Short
          // prompts produce generic output": the card asks for full scene captions,
          // which is what its prompt and the suite's runs are, and it names text
          // rendering as a strength while warning the words may distort. ucache is
          // documented for the UNet families; this is a transformer, and the engine
          // refuses that mode for it on its own, so the card states no cache.
          {"flux-3-klein", 1024, 1024, "euler", "", false},
          // Z-Image Turbo: the card's own table names the Turbo row at Step 8 with CFG
          // crossed out, and its quick start passes guidance_scale=0.0 with the words
          // "Guidance should be 0 for the Turbo models" - num_inference_steps 9 there,
          // which it notes "actually results in 8 DiT forwards", the number this
          // engine's own example asks for. Its example and its showcase are stated at
          // 1024, its strengths are photorealistic generation, "Accurate Bilingual
          // Text Rendering" and instruction adherence. The engine's page names no
          // sampler for it, so its choice for this family is left standing, and
          // ucache is a UNet mode the engine will not take for a single-stream
          // transformer.
          {"z-image-turbo", 1024, 1024, "", "", false},
          // Qwen Image 2.1 and its Turbo: the engine's own page runs both with
          // "--sampling-method euler" - the base at cfg 6.0, the Turbo with
          // "--sigmas qwen-image-2.1-turbo --steps 8 --cfg-scale 1.0" - and asks for
          // dimensions divisible by 32, leaving the flow schedule to itself. The
          // official ComfyUI workflow for the same weights ships 1024x1024, euler,
          // 25 steps and "Prefer multiples of 32", and notes the base "needs about
          // 40-50 steps with euler" on the full pipeline, so 25 is the low end of what
          // the released workflow ships. ucache is a UNet mode the engine refuses for
          // this transformer.
          {"qwen-image-2.1", 1024, 1024, "euler", "", false},
          {"qwen-image-2.1-turbo", 1024, 1024, "euler", "", false},
          // Qwen Image: its card's quick start runs num_inference_steps 50 with
          // true_cfg_scale 4.0, an empty negative prompt, and a 1:1 frame of
          // 1328x1328; the released ComfyUI workflow for the fp8 bytes on this machine
          // ships the same 1328x1328 with "euler" and "simple" and switches steps 20 /
          // cfg 4, carrying the note "Try 50 steps, if you want original qwen image's
          // setting, but it will takes longer". 20 at cfg 4 is what the released
          // workflow runs on this quantisation, and its own note frames 50 as the slow
          // option. Its strength is complex text rendering, "especially for Chinese".
          // These bytes are the heaviest here - 19.03 GB of model beside an 8.74 GB
          // encoder, past a 24 GB card - so it runs with the offload switch on.
          {"qwen-image", 1328, 1328, "euler", "simple", false},
          // RealVisXL V5 Lightning: the card's own parameters — "Sampling Steps: 5",
          // "Sampling Method: DPM++ SDE Karras / DPM++ SDE", "CFG Scale: 1.0-2.0" — at
          // the square an SDXL checkpoint is trained at. It is a Lightning distillation
          // of RealVisXL V5, whose card says the model "is aimed at photorealism". No
          // step cache: five distilled steps leave the cache window the engine
          // documents (15%-95% of the steps) with three steps inside it.
          {"realvisxl-v5-lightning", 1024, 1024, "dpm++2m_sde", "karras", false},
          // Pony Diffusion V6 XL: the card names its own pair - "Using Euler a with 25
          // steps and resolution of 1024px is recommended", run "with no negative prompt
          // and otherwise default settings". It is a plain SDXL finetune at 25 steps, so
          // the step cache stays on: ucache is the mode the engine's caching page writes
          // for UNet models, and 25 steps is the range it calls an opportunity.
          {"pony-diffusion-v6", 1024, 1024, "euler_a", "", true},
          // CyberRealistic XL: its Recommended Settings table names steps 25-30, CFG
          // 7.0-8.0, "1024x1024 (native)" and the sampler pair "DPM++ 3M SDE Karras,
          // Euler A". The middle of each range is taken. The first sampler maps to this
          // engine's dpm++2m_sde_bt, the bit-trick 2M SDE that is the solver ComfyUI
          // ships as 3M; Euler A is the card's own listed alternative. 28 steps on an
          // SDXL UNet is what the engine's caching page calls an opportunity, so the step
          // cache stays on.
          {"cyberrealistic-xl", 1024, 1024, "dpm++2m_sde_bt", "karras", true},
          // Animagine XL 3.1: the card's recommended settings are a CFG of "around 5-7",
          // "sampling steps below 30" and "Euler Ancestral (Euler a) as a sampler", with
          // the 1024x1024 square from its Multi Aspect Resolution table. An SDXL UNet at
          // 28 steps is the caching case the engine's page documents, so the step cache
          // stays on.
          {"animagine-xl-3.1", 1024, 1024, "euler_a", "", true},
          // Lustify NSFW Checkpoint, v1.0 alpha. CivitAI version 638929 - a VERSION id, so
          // it resolves at /api/v1/model-versions/638929 (the /api/v1/models/<id> endpoint
          // takes a model id and answers "No model with id" for this number; that 404 is an
          // endpoint mismatch, not a dead record). It names modelId 573152 "LUSTIFY! [NSFW
          // checkpoint]" by coyotte, "baseModel":"SDXL 1.0", and the primary file
          // lustifyNSFWCheckpoint_v10Alpha.safetensors, pruned fp16, 6.46 GB, SHA256
          // 5080D7346C13... - the "Model hash 5080d7346c" the showcase photos carry, so
          // those generations came off this very file and their metadata is evidence about
          // it: steps 30, cfg 4-6.5 (most 4.5-5), "sampler":"DPM++ 2M SDE" with
          // "scheduler":"karras", negativePrompt "airbrushed, blurry", "Size":"832x1152"
          // (also 896x1152, 1152x832), prompts opening "score_9, 4k, highres, best quality,".
          // The card itself states what the checkpoint is for and what it can also do:
          // "a photoreal checkpoint optimized for creating high-quality imagery of women in
          // explicit/sexual scenarios ... if you guide it properly, it is fully capable of
          // generating stunning SFW photography, objects, animals, and fantasy environments".
          // The version's own description is only "Initial release." and the card body says
          // "Description to be rewritten completely" - the recommended-parameters block that
          // exists on this model page (DPM++ SDE, Karras/Exponential, 4 steps, cfg 1-2) is
          // written for the ⚡LIGHTNING⚡ files, a different checkpoint, so its step and
          // guidance numbers do not apply to this one; only its note that "the lighting type
          // tags need more weight ... (neon lighting:1.3), (cinematic lighting:1.1)" is
          // carried into the prompts as a hint, marked as the author's remark on a sibling.
          // No __metadata__ is in the file, but its header proves a full SDXL set:
          // model.diffusion_model, conditioner.embedders.0/1 and first_stage_model all in
          // F16 - the VAE the showcase metas pass by hand (sdxl_vae_fp16.safetensors) is
          // already inside, so nothing extra is loaded. clip skip is not named; SDXL's
          // family default of 2 stands.
          {"lustify-sdxl", 832, 1152, "dpm++2m_sde", "karras", true},
          // MajicMix Realistic v7 (CivitAI model 43331, version v7 = 176425, baseModels
          // ["SD 1.5"], and the file on disk is that version's pruned fp16
          // majicmixRealistic_v7.safetensors). Its "推荐参数 Recommended Parameters for V7"
          // names "Sampler: Euler a, Euler, restart" and "Steps: 20~40", the CFG scale
          // paragraph gives 6-8, and the same block states "clip skip 2" - which is the
          // one setting the engine cannot answer by itself here: its family default is 2
          // for SDXL and SD2 and 1 for SD 1.5, and this is an SD 1.5 UNet, so the card's
          // number has to be written down. The size is the 512 square SD 1.5 trains at,
          // not the 1024x1536 the showcase hangs at - those went through the "Hires
          // upscale: 2+" the card names, a stage this app does not run. No scheduler is
          // named for V7, so the engine's own choice for a UNet stands. The card's
          // negative is "use ng_deepnegative_v1_75t and badhandv4 in negative prompt":
          // those are textual-inversion embeddings and this app ships neither, so no
          // negative text is invented to stand in for them.
          {"majicmix-realistic-v7", 512, 512, "euler_a", "", true, 2},
          // Juggernaut XL v9 (RunDiffusion/Juggernaut-XL-v9, base_model
          // stabilityai/stable-diffusion-xl-base-1.0, the file on disk being the card's
          // own "Juggernaut-XL_v9_RunDiffusionPhoto_v2.safetensors"). Its "Recommended
          // Settings" table is explicit: Resolution "832 × 1216 (portrait) · 1216 × 832
          // (landscape)", Sampler "DPM++ 2M Karras" - which is dpm++2m drawing on the
          // karras grid, both named in the engine's own words - Steps "30 – 40", CFG
          // "3 – 7 (lower = more realistic)", so 35 and 5.0, the numbers its own
          // from_pretrained example passes. The square 1024 this app had it at is SDXL's
          // training size, not the size the card sends people to; the card's portrait
          // column is 832x1216 and that is what a run asking for no size now draws.
          // "Negative prompts: start with none. ... Heavy negatives often hurt more than
          // they help on this model." - so it ships with none, as it already did, and
          // these runs name none. Its VAE is "Already baked in", which is what the loader
          // reads off the file. clip skip is not named for an SDXL and the engine's
          // family default is already 2, so nothing is written.
          {"juggernaut-xl-v9", 832, 1216, "dpm++2m", "karras", true},
          // Stable Diffusion 3.5 Medium. Stability's own card cannot be read from this
          // machine: https://huggingface.co/stabilityai/stable-diffusion-3.5-medium/raw/main/README.md
          // answers 401 "Access to model ... is restricted. You must have access to it and
          // be authenticated", so the settings come from the two sources that can be read
          // and that this app's files come from. ComfyUI ships the SD3.5 workflow beside
          // the very repo these text encoders were downloaded from
          // (Comfy-Org/stable-diffusion-3.5-fp8): its KSampler passes 20 steps, cfg 4.01,
          // "euler" on the "sgm_uniform" scheduler, its EmptySD3LatentImage is 1024x1024,
          // and its negative prompt is the empty string. This engine's own SD3 page
          // (docs/sd3.md:17) runs the family with --sampling-method euler at 1024x1024 and
          // a cfg in the fives, naming no scheduler, which is the engine's own default
          // (src/pipeline/request.cpp:57 - a DiT that is not flux, flux2, qwen-2.1, ltx or
          // ideogram gets discrete). euler + sgm_uniform at 1024 square is the combination
          // that ships, so it is the one written down; 4.0 sits between ComfyUI's 4.01 and
          // the doc's 4.5. No cache: docs/caching.md scopes the step caches to UNet models
          // and the library refuses them for the DiT families by itself.
          {"sd35-medium", 1024, 1024, "euler", "sgm_uniform", false},
          // FLUX.2 Klein 4B. Black Forest Labs' own card (black-forest-labs/FLUX.2-klein-4B)
          // gives the numbers in code: guidance_scale=1.0, num_inference_steps=4,
          // height=1024, width=1024 - already what this app ships - and this engine's FLUX.2
          // page (docs/flux2.md:45) runs the same checkpoint at "--cfg-scale 1.0 --steps 4",
          // naming euler in the editing example, so the sampler is written down and the
          // scheduler is left to the engine, which knows flux2 by itself
          // (src/pipeline/request.cpp:70). No cache: four steps, and docs/caching.md starts
          // a step cache at fifteen percent of them. "Runs on consumer hardware, with as
          // little as 13GB VRAM" and "accessible on NVIDIA RTX 3090/4070 and above" is why
          // no offload is asked for: the files here are the 7.22 GB bf16 transformer, the
          // 7.49 GB bf16 Qwen3 4B text encoder and the 0.31 GB flux2 VAE - 15.0 GB against
          // 24 GB - and the full-precision encoder is kept because it fits; the fp4 variant
          // in the ComfyUI repo (qwen_3_4b_fp4_flux2.safetensors) is the option if the card
          // is ever shared. What the card warns about is why the built-in prompt no longer
          // asks for words: "While the model can output text, text rendered may be
          // inaccurate or subject to distortion" - a legible sign is SD3.5's showpiece and
          // this model's weak spot, so the demo is a photograph, not a typography test. The
          // strength the card states is what the model page says it for: "Text-to-image and
          // image-to-image multi-reference editing in a single unified model", which is the
          // "edit" flag with ten reference pictures this row already carries.
          // Its time shift is written down because the family default is not trustworthy:
          // the engine answers 1.0 for FLUX and FLUX.2 with the words "TODO: validate"
          // beside it (src/pipeline/diffusion_engine.cpp:1531), raising it to 1.15 only for
          // a file carrying guidance_in.in_layer.weight - and this file's header has no
          // guidance_in tensor at all, so on the family default it would sample at 1.0.
          // The maker settles it: the repo ships scheduler/scheduler_config.json, a
          // FlowMatchEulerDiscreteScheduler with "use_dynamic_shifting": true,
          // "base_shift": 0.5, "max_shift": 1.15, "base_image_seq_len": 256,
          // "max_image_seq_len": 4096 and "time_shift_type": "exponential" (its static
          // "shift": 3.0 is what applies only when dynamic shifting is off). At this row's
          // 1024x1024 the latent sequence is (1024/16)^2 = 4096, exactly max_image_seq_len,
          // so the maker's own pipeline samples at max_shift - **1.15** - and that is what
          // this model asks for. Other models leave the field alone: the engine's defaults
          // for qwen-image, z-image and SD3.5 are 3.0, and docs/qwen_image.md:17 passes
          // --flow-shift 3, the same number, which is the engine answering its family right.
          {"flux-2-klein-4b", 1024, 1024, "euler", "", false, -1, 1.15f},
          // MiniMax H3. The numbers are unsloth's own example run beside the very GGUF
          // bytes this machine downloads: "Example UD-Q2_K_XL, the smallest rung here,
          // at 960x544, 124 frames, 24 FPS, 8 steps, guidance 1.0, seed 11, on a
          // single card" - the length and shape the model's own maker runs it at, and
          // 124 frames at 24 FPS is 5.2 s, inside the "up to 15 seconds at 24 FPS" its
          // card states. 960 and 544 are both multiples of 32, and 124 = 17*7 + 5 sits
          // exactly on the frame grid the engine keeps (docs/minimax_h3.md:93: "Frame
          // count is aligned upward to the 17k+5 grid"), so nothing here is aligned
          // upward on the way in. 8 steps is also what the repo's shorter example asks
          // ("--width 640 --height 384 --video-frames 25 --steps 4"): the few steps a
          // distilled, cfg-free checkpoint was trained for. No sampler or scheduler is
          // named by either card, so the engine's choice for the model's family stands.
          // The flow shift IS named, by the engine's own page (docs/minimax_h3.md:95:
          // "The default video flow shift is 12. The audio stream is mapped internally
          // to its shift of 3"), and the family default is not that, so 12 is written
          // down. No step cache: docs/caching.md scopes the step caches to UNet models
          // and the library refuses them for the other families on its own, and eight
          // distilled steps leave a cache nothing to skip anyway.
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
                            const std::string &wtype = std::string())
        {
          ImageModelDef def;
          def.id = id;
          def.name = name;
          def.repo = repo;
          def.negative = negative;
          def.sigmas = sigmas;
          def.wtype = wtype;
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
             {"llm", "hf:unsloth/Qwen3-4B-Instruct-2507-GGUF/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"}});
        add(out, "qwen-image-2.1", "Qwen Image 2.1", "Comfy-Org/Qwen-Image-2.1", 25, 6.0f,
            "a grey tabby cat sitting upright holding a small handwritten card that reads "
            "'qwen2.1' with one paw, plain warm-grey studio backdrop, soft side light, the "
            "words crisp and legible, clean vector illustration",
            "", "", 0,
            {{"diffusion_model", "diffusion_models/qwen_image_2.1_int8_convrot.safetensors"},
             {"vae", "vae/qwen_image_2.1_vae_bf16.safetensors"},
             {"llm", "text_encoders/qwen3vl_8b_int8_convrot.safetensors"}});
        add(out, "qwen-image-2.1-turbo", "Qwen Image 2.1 Turbo", "Comfy-Org/Qwen-Image-2.1", 8,
            1.0f, "a brass telescope on a velvet-draped table beside a small upright card that "
                  "reads 'qwen2.1 turbo', one warm lamp light, dark quiet background, the words "
                  "crisp "
                  "and legible",
            "",
            "1.0,0.978453,0.95418,0.926626,0.89508,0.845148,0.704534,0.414568,0.0", 0,
            {{"diffusion_model", "diffusion_models/qwen_image_2.1_turbo_int8_convrot.safetensors"},
             {"vae", "vae/qwen_image_2.1_vae_bf16.safetensors"},
             {"llm", "text_encoders/qwen3vl_8b_int8_convrot.safetensors"}});
        add(out, "qwen-image", "Qwen Image", "Comfy-Org/Qwen-Image_ComfyUI", 20, 4.0f,
            "a Chinese woman in a t-shirt wearing the word QWEN, smiling at the camera while "
            "holding a "
            "black marker; behind her a glass board handwritten in Chinese reading Qwen-Image "
            "复杂字体渲染",
            "", "", 0,
            {{"diffusion_model", "split_files/diffusion_models/qwen_image_fp8_e4m3fn.safetensors"},
             {"vae", "split_files/vae/qwen_image_vae.safetensors"},
             {"llm", "split_files/text_encoders/qwen_2.5_vl_7b_fp8_scaled.safetensors"}});
        add(out, "sdxl-turbo", "SDXL Turbo", "stabilityai/sdxl-turbo", 4, 0.0f,
            "a single red apple centered on a rustic wooden table, macro photo, one soft window "
            "light from the left, dark neutral background, shallow depth of field, no other "
            "objects",
            "", "", 0,
            {{"model", "sd_xl_turbo_1.0_fp16.safetensors"}});
        // The card names 5 steps, DPM++ SDE Karras, CFG 1.0-2.0, and a recommended
        // negative prompt, all shipped here as it writes them: the negative carries the
        // (word:1.3) weights the card uses, which this engine's prompt parser reads.
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
        // The card's own recommendation: "Using Euler a with 25 steps and resolution of
        // 1024px is recommended", run "with no negative prompt and otherwise default
        // settings" - the default guidance of the software it names is 7. Its required
        // clip skip 2 needs nothing from here: the engine resolves an unset clip skip to
        // 2 for SDXL on its own (conditioner.hpp). The prompt carries the card's own
        // quality template, which it says is what makes the defaults work.
        add(out, "pony-diffusion-v6", "Pony Diffusion V6 XL", "LyliaEngine/Pony_Diffusion_V6_XL", 25,
            7.0f, "score_9, score_8_up, score_7_up, score_6_up, score_5_up, score_4_up, "
                  "source_anime, a knight in ornate silver armour standing alone in a stone "
                  "hall, torchlight on the breastplate, detailed face, dramatic lighting",
            "", "", 0,
            {{"model", "ponyDiffusionV6XL_v6StartWithThisOne.safetensors"}, {"vae", "sdxl_vae.safetensors"}});
        // The card's Recommended Settings table: steps 25-30, sampler "DPM++ 3M SDE
        // Karras, Euler A", resolution "1024x1024 (native)", CFG 7.0-8.0 - the middle of
        // each range here. Its first sampler is the bit-trick 2M SDE this engine carries
        // (the solver ComfyUI names 3M), so that pair is used; Euler A is the card's own
        // equal alternative. It also says the checkpoint ships a VAE ("Built-in VAE - No
        // need for an external VAE"), which is why the row names no vae file, and its
        // example prompts open on (masterpiece, best quality) quality tags, which the
        // prompt below follows.
        add(out, "cyberrealistic-xl", "CyberRealistic XL", "cyberdelia/CyberRealisticXL", 28, 7.5f,
            "(masterpiece, best quality), RAW photo figure study of an unclothed woman seated with "
            "her back to the camera on a wooden stool against a plain grey backdrop, bare shoulders "
            "and back, one soft key light from the upper left, skin texture visible, no "
            "retouching, tasteful framing, RAW photo, 85mm",
            "", "", 0, {{"model", "CyberRealisticXLPlay_V10.0_FP16.safetensors"}});
        // The card's own settings: a CFG of "around 5-7", "sampling steps below 30", and
        // "Euler Ancestral (Euler a) as a sampler"; the square of its Multi Aspect
        // Resolution table. Its prompt template is "1girl/1boy, character name, from what
        // series, everything else in any order" with "masterpiece, best quality, very
        // aesthetic, absurdres" prepended - the widget's own example below - and the
        // negative is the card's recommended one, verbatim. It says the model is
        // "optimized for Danbooru-style tags rather than natural language prompts", so the
        // tags are the prompt here, not sentences.
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
        add(out, "majicmix-realistic-v7", "MajicMix Realistic v7", "digiplay/majicMIX_realistic_v7", 28, 7.0f,
            "Best quality, masterpiece, ultra high res, (photorealistic:1.4), close-up portrait of "
            "an elderly fisherman mending a net on a boat, weathered hands, grey sky, deep shadow",
            "", "", 0, {{"model", "majicmixRealistic_v7.safetensors"}});
        add(out, "juggernaut-xl-v9", "Juggernaut XL v9", "RunDiffusion/Juggernaut-XL-v9", 35, 5.0f,
            "Cinematic mid shot photo of an astronaut walking through a neon-lit Tokyo alley at "
            "night, hyperdetailed photography, skin details, shallow depth of field",
            "", "", 0, {{"model", "Juggernaut-XL_v9_RunDiffusionPhoto_v2.safetensors"}});
        add(out, "sd35-medium", "Stable Diffusion 3.5 Medium", "stabilityai/stable-diffusion-3.5-medium", 20, 4.0f, "a lovely cat holding a sign says \"Stable Diffusion 3.5 Medium\", "
                                                                                                                    "plain "
                                                                                                                    "warm-grey studio "
                                                                                                                    "backdrop, soft side light, the words crisp and legible",
            "",
            "", 0,
            {{"model", "sd3.5_medium.safetensors"},
             {"clip_l", "hf:Comfy-Org/stable-diffusion-3.5-fp8/text_encoders/clip_l.safetensors"},
             {"clip_g", "hf:Comfy-Org/stable-diffusion-3.5-fp8/text_encoders/clip_g.safetensors"},
             {"t5xxl", "hf:Comfy-Org/stable-diffusion-3.5-fp8/text_encoders/t5xxl_fp8_e4m3fn.safetensors"}});
        add(out, "flux-2-klein-4b", "FLUX.2 Klein 4B", "Comfy-Org/vae-text-encorder-for-flux-klein-4b", 4, 1.0f,
            "a weathered fisherman mending a net on a rolling deck at dawn, spray in the air, wool "
            "sweater, salt on the rails, natural light, photorealistic photograph, shallow depth of field",
            "", "",
            0,
            {{"diffusion_model", "split_files/diffusion_models/flux-2-klein-4b.safetensors"},
             {"vae", "split_files/vae/flux2-vae.safetensors"},
             {"llm", "split_files/text_encoders/qwen_3_4b.safetensors"}});
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
    // Resident models are charged with the bytes of weights they hold and stamped
    // with the tick of their last use, so a load can keep room by closing the
    // least recently used idle model first.
    static std::map<std::string, uint64_t> gCtxUse;
    static std::map<std::string, long long> gCtxBytes;
    // Which device each open context was opened on. A run that asks for the
    // other one gets its weights moved: drawing on the context that is standing
    // would answer the run with a device it did not ask for.
    static std::map<std::string, bool> gCtxCpu;
    static uint64_t gUseTick = 0;

    // The working-set ceiling for image models: a standing three quarters of the
    // largest GPU heap this pair of machines is built for, so the compute
    // buffers and activations still fit beside the weights. it is a fixed
    // number rather than a reading of the machine - what a machine can actually
    // hold is answered when a context is opened, where the engine asks its own
    // allocator for room and this module closes the models it must to make room.
    static long long memoryBudgetBytes() { return 25769803776LL; }
    static std::map<std::string, std::vector<std::string>> gEvents;
    static std::map<std::string, bool> gEnded;
    static std::map<std::string, bool> gCancel;
    // One record per job: kind, model, state, timings, progress, and the file it made.
    static std::map<std::string, json> gJobs;
    // What stands proved about the bytes of this weights room is the room's own
    // record, read and written through VerifiedRoom: one set of rows for the
    // whole machine, so a page and a download cannot hold two different opinions
    // about the same file.
    // which precision of a model a person last asked for, kept beside the
    // weights room's own record so the next open keeps it: the choice is a
    // machine-wide fact about a model, not one a page forgets on reload.
    static std::map<std::string, std::string> gChosenQuant;
    static std::map<std::string, std::shared_ptr<std::mutex>> gLoadGuards;
    static std::map<std::string, int> gGenerating;
    static std::map<std::string, bool> gPendingErase;
    static std::mutex gGenerationMutex;
    static std::deque<std::string> gJobOrder;
    static std::string gEngineError;

    // where a running job began, in whole milliseconds: the home page measures
    // a run's steps-per-second against how long it took, and whole seconds of a
    // run are too coarse to be that rate. the start sits beside the job, not in
    // its record, and is read once when the job settles.
    static std::map<std::string, long long> gStartedMs;

    // the newest jobs this engine keeps: past this its oldest record is dropped, so
    // the merged status answer stays a few kilobytes however long the machine has
    // been up. the cap is the one number the shared ledger states, and the status
    // page's words about what is kept follow it.

    static void pruneJobs()
    {
      ols::forgetPastKept(gJobOrder, ols::kKeptJobs, gJobs, gEvents, gEnded, gCancel, gStartedMs);
    }
    static std::string gWeightsDir;
    static std::string gOutputDir;
    static std::string gRefsDir;

    // What a settled run cost, filed beside the settings it ran at. A page that
    // wants to say "this draw will take about a minute" has one honest source: what
    // a run of these same settings cost on this machine before. A model's size says
    // nothing about its time, and the suite's log is a file no browser reads, so the
    // module keeps the times itself and answers them at /api/timings. A generation
    // is keyed by the model and by everything that changes the work - the sides
    // asked for, the steps, the guidance, the sampler and scheduler as this run
    // resolved them, the step cache, the device the work ran on, the offload and
    // sage-attention switches, how many pictures the ask asked for, and how many
    // reference pictures it started from. An open is keyed by the model and the
    // precision it was opened as. The seed is deliberately not in any key: another
    // seed is the same work at these settings. A run that was cancelled or failed
    // kept only part of its work and is not filed.
    static std::map<std::string, json> gTimings;
    static bool gTimingsLoaded = false;

    // Called with gMutex held. Read the ledger back once per process, so a machine
    // that was shut down and started again still knows what its runs cost. A file
    // that cannot be read is left alone: the ledger fills up again as work runs, and
    // a page that has nothing to read says it has no sample rather than guessing.
    static void ensureTimingsLoadedLocked()
    {
      if (gTimingsLoaded)
        return;
      gTimingsLoaded = true;
      std::string body;
      if (!readFileText(gWeightsDir + "/.run-timings.json", body))
        return;
      const json doc = json::parse(body, nullptr, false);
      if (doc.is_discarded() || !doc.is_object())
        return;
      for (const auto &entry : doc.items())
      {
        if (entry.value().is_object() && entry.value().contains("count"))
          gTimings[entry.key()] = entry.value();
      }
    }

    // Called with gMutex held. The ledger holds one row per set of settings this
    // machine has run - tens of rows after a full suite - so rewriting the whole
    // file is as cheap as merging a row into it, and a machine that dies mid-write
    // loses at most the one run it was filing.
    static bool saveTimingsLocked()
    {
      json doc = json::object();
      for (const auto &entry : gTimings)
        doc[entry.first] = entry.second;
      return writeFileText(gWeightsDir + "/.run-timings.json", doc.dump());
    }

    // Called with gMutex held, from the one place a job settles. The record carries
    // its own settings, so filing happens where the truth is: a run that finished
    // every step of its window says what these settings cost, and nothing else does.
    static void noteTimingLocked(const json &record)
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
        if (precision.empty())
          precision = "as shipped";
        settings["precision"] = precision;
      }
      else
      {
        // a run that never got as far as naming its settings cannot be filed: the
        // settings are the key, and an ask that was refused never had any
        if (!record.contains("settings") || !record["settings"].is_object())
          return;
        settings = record["settings"];
      }
      // the key is the settings themselves, printed by the json library with its
      // keys in order, so the same ask from any door lands on one row
      const std::string key = kind + "|" + record.value("model", std::string()) + "|" + settings.dump();
      ensureTimingsLoadedLocked();
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
        row["updated"] = nowSeconds();
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
        row["updated"] = nowSeconds();
      }
      saveTimingsLocked();
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

    static void touchContext(const std::string &modelId);
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
        closeContextLocked(it->first);
        it = gPendingErase.erase(it);
      }
    }

    static void endJob(const std::string &job)
    {
      {
        std::lock_guard<std::mutex> lock(gMutex);
        gEnded[job] = true;
      }
      {
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
      {
        std::lock_guard<std::mutex> lock(gMutex);
        json &record = gJobs[job];
        record["state"] = state;
        if (ols::stateEnded(state))
        {
          record["ended_at"] = nowSeconds();
          // the run is over: state how long it took, and let go of the start it
          // was measured from
          const auto started = gStartedMs.find(job);
          if (started != gStartedMs.end())
          {
            record["elapsed_ms"] = steadyMs() - started->second;
            gStartedMs.erase(job);
          }
          // the run is over and its record now carries its own settings and its
          // whole cost, so this is where the machine files what those settings cost
          noteTimingLocked(record);
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
        // bar of a three-step run shrinking back to two.
        const int kept = record.contains("done") ? record["done"].get<int>() : 0;
        const int window = record.contains("max") ? record["max"].get<int>() : 0;
        if (window == total && done < kept)
          return;
        record["done"] = done;
        record["max"] = total;
      }
      pushEvent(job, json{{"type", "progress"}, {"done", done}, {"max", total}}.dump());
    }

    static void noteResult(const std::string &job, const std::string &url)
    {
      std::lock_guard<std::mutex> lock(gMutex);
      gJobs[job]["url"] = url;
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
      const std::string key = url.rfind("https://civitai.com/", 0) == 0 ? gCivitaiKey : gHfToken;
      return ols::platform::curlFetch(dest, url, key, maxSeconds);
    }

    // How long one round of an internet fetch is allowed to run: one curl for the
    // whole of a file answers when the file is done or the link died, and nothing
    // in between - so cancel waits for all of it and the bar states nothing while
    // thirty gigabytes arrive. Each round resumes from the bytes already down.
    constexpr int kFetchSliceSeconds = 20;

    // A file from the internet, brought down in rounds. True when the address
    // answered the whole of it - either curl said it finished, or the size the card
    // states now stands on the disk. False when the caller stopped asking, or when
    // the link stopped carrying the file; which of those it was is the caller's to
    // ask, because it is the one that knows whether cancel was pressed.
    static bool fetchWhole(const std::string &url, const std::string &dest, long long want,
                           const std::function<bool()> &cancelled, int &answerOut)
    {
      const long long goal = want > 0 ? want : 0;
      int stalls = 0;
      for (;;)
      {
        if (cancelled && cancelled())
          return false;
        const long long before = ols::fileSize(dest);
        const int answer = runCurl(url, dest, kFetchSliceSeconds);
        answerOut = answer;
        const long long after = ols::fileSize(dest);
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
      static std::map<std::string, std::pair<std::string, json>> parsed;
      const std::string path = gWeightsDir + "/.hf-tree-" + flatRepo + ".json";
      std::error_code ec{};
      const auto when = std::filesystem::last_write_time(path, ec);
      if (ec)
        return json();
      const std::string stamp = std::to_string(when.time_since_epoch().count());
      std::lock_guard<std::mutex> lock(gMutex);
      const auto found = parsed.find(path);
      if (found != parsed.end() && found->second.first == stamp)
        return found->second.second;
      std::string body;
      if (!readFileText(path, body))
        return json();
      json tree = json::parse(body, nullptr, false);
      if (tree.is_discarded())
        tree = json();
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
        if (path.rfind(prefix, 0) == 0 &&
            path.size() >= 12 && path.compare(path.size() - 12, 12, ".safetensors") == 0 &&
            path.find("-of-", prefix.size()) != std::string::npos)
        {
          out.push_back(path);
        }
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
      const std::uintmax_t fileSize = std::filesystem::file_size(path);
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
        const uint64_t to = offsets[1].get<uint64_t>();
        if (to > highest)
          highest = to;
      }
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
      const uint64_t widths[10] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0};
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
          if (type > 9)
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
      // written; the values only walk.
      for (uint64_t i = 0; i < nKV; ++i)
      {
        std::string key;
        if (!readString(key))
          return false;
        uint32_t type = 0;
        if (!readTag(type))
          return false;
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
      // The data block starts where the header's own count of its records ends,
      // padded up to the alignment the writer uses; this file's bytes sit at the
      // GGUF default of 32.
      const uint64_t dataStart = (headerEnd + 31ull) & ~31ull;
      return highestOffset > 0 && dataStart + highestOffset < fileSize;
    }

    // The switches the web interface offers: kept on the server so a page reload
    // shows the same page. Nothing here is about what a model may draw: a
    // picture is shown whole, always, so there is no filter to remember and no
    // switch to turn it off. "cpu" is the one that names which part of the
    // machine works: the card is left to whatever else the machine is busy with,
    // and every weight and every step is worked where the threads are.
    static const std::vector<std::string> &kPrefKeys()
    {
      static const std::vector<std::string> keys = {"offload", "tiling", "sage_attn", "cpu"};
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
      const long long wantSize = spec.value("size", 0LL);
      if (wantSize > 0 && wantSize != static_cast<long long>(std::filesystem::file_size(path)))
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
        std::ifstream probe{path, std::ios::binary};
        char magic[4] = {};
        if (probe && probe.read(magic, 4) && std::string(magic, 4) == "GGUF")
          return ggufComplete(path);
        return safetensorsComplete(path);
      }
      return got == want;
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
        // which is said on the card rather than hidden here.
        std::vector<ModelForm> h3;
        for (const char *rung : {"Q2_K", "Q4_K", "Q8_0"})
        {
          h3.push_back(ModelForm{
              rung,
              {{"diffusion_model",
                std::string("hf:unsloth/MiniMax-H3-GGUF/minimax_h3_fl2va_pruned-") + rung + ".gguf"}}});
        }
        h3.push_back(ModelForm{"Sakura-Q4_K_M",
                               {{"llm", "hf:webmp3/Sakura-MiniMax-H3-Uncensored-GGUF/"
                                        "Sakura-MiniMax-H3-TextEncoder-Uncensored-Q4_K_M-14.05GiB.gguf"}}});
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

    static long long weightsBytesOf(const std::string &modelId)
    {
      const ImageModelDef *def = findModel(modelId);
      long long bytes = 0;
      if (!def)
        return 0;
      for (const auto &file : modelFiles(def, std::string()))
      {
        const std::string path = ols::modelFilePath(gWeightsDir, modelId, localNameOf(file.second));
        std::error_code ec{};
        const auto size = std::filesystem::file_size(path, ec);
        if (!ec)
          bytes += static_cast<long long>(size);
      }
      return bytes;
    }

    // Caller holds gCtxMutex.
    static void touchContext(const std::string &modelId)
    {
      ++gUseTick;
      gCtxUse[modelId] = gUseTick;
      gCtxBytes[modelId] = weightsBytesOf(modelId);
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
      gPendingErase.erase(modelId);
    }

    // On some GPUs the engine's flash-attention kernel has no device code for
    // that card (it aborts the whole process, which took the server down
    // mid-suite on the compute-7.0 box). The engine's own probe cannot see
    // this, so ask the CUDA runtime for the device's compute capability and
    // turn flash-attention off before the model is opened. Log it once.
    static int cudaDeviceComputeMajor()
    {
#ifdef OLS_CUDART_LINKED
      int major = -1;
      // 75 is the runtime's compute-capability-major attribute, a fixed number
      if (cudaDeviceGetAttribute(&major, 75, 0) != 0)
        return -1;
      return major;
#else
      return -1;
#endif
    }

    static sd_ctx_t *openContext(const std::string &modelId, bool offload, bool sage, bool cpu,
                                 std::string &error)
    {
      const ImageModelDef *def = findModel(modelId);
      if (!def)
      {
        error = "unknown model";
        return nullptr;
      }
      const std::string base = ols::modelFileDir(gWeightsDir, modelId);
      sd_ctx_params_t params;
      sd_ctx_params_init(&params);
      std::vector<std::pair<std::string, std::string>> wantedFiles = modelFiles(def, std::string());
      std::vector<std::string> paths;
      paths.reserve(wantedFiles.size());
      for (const auto &file : wantedFiles)
      {
        paths.push_back(base + localNameOf(file.second));
      }
      for (size_t i = 0; i < wantedFiles.size(); ++i)
      {
        const std::string slot = wantedFiles[i].first;
        const char *path = paths[i].c_str();
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
        else if (slot == "audio_vae")
          // the audio half of a joint video+audio model: the engine's page passes
          // it as its own component ("Pass the four MiniMax-H3 components
          // separately", docs/minimax_h3.md:7) and says what leaving it out costs
          // ("Omitting --audio-vae still runs the joint diffusion model but
          // produces video without a decoded audio track", docs line 46) - half of
          // what the model makes would go unmade.
          params.audio_vae_path = path;
      }
      params.n_threads = gThreads;
      params.enable_mmap = true;
      params.sage_attn = sage;
      if (!def->wtype.empty())
        params.wtype = str_to_sd_type(def->wtype.c_str());
      static const int kCudaMajor = cudaDeviceComputeMajor();
      if (kCudaMajor >= 0 && kCudaMajor < 8)
      {
        static const std::string kFlashOff = "off";
        params.flash_attn_mode = kFlashOff.c_str();
        std::fprintf(stderr, "[image] cuda device is compute %d: flash attention was not built for it, so it is off\n",
                     kCudaMajor);
        std::fflush(stderr);
      }
      // The card of this machine, named in the engine's words, is what a model
      // opens on. "cpu" leaves that card alone - every weight and every step
      // then works where the threads are, which is what a machine asks when the
      // card is busy with something else - and with no device named the engine
      // keeps its own cpu path, so nothing of the card is opened for it.
      const std::string gpu = cpu ? std::string() : gpuDeviceName();
      if (!gpu.empty())
      {
        params.backend = gpu.c_str();
        params.params_backend = gpu.c_str();
        params.auto_fit = true;
      }
      else if (offload)
      {
        params.auto_fit = true;
      }

      std::shared_ptr<std::mutex> guard;
      {
        std::lock_guard<std::mutex> lock(gCtxMutex);
        const auto standing = gCtx.find(modelId);
        if (standing != gCtx.end())
        {
          const bool stands_on = gCtxCpu.count(modelId) ? gCtxCpu[modelId] : false;
          if (stands_on != cpu)
          {
            std::fprintf(stderr, "[image] %s stands open on the %s and this asks the %s: the weights are moved\n",
                         modelId.c_str(), stands_on ? "cpu" : "card", cpu ? "cpu" : "card");
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
          touchContext(modelId);
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
          touchContext(modelId);
          return found->second;
        }
      }
      sd_ctx_t *ctx = new_sd_ctx(&params);
      bool freed = false;
      while (!ctx)
      {
        std::string victim;
        {
          std::lock_guard<std::mutex> lock(gCtxMutex);
          victim = lruVictimLocked(modelId);
          if (!victim.empty())
            closeContextLocked(victim);
        }
        if (victim.empty())
          break;
        freed = true;
        ctx = new_sd_ctx(&params);
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
        gCtxCpu[modelId] = cpu;
        touchContext(modelId);
        trimOverBudgetLocked(modelId);
      }
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
          int fetchAnswer = 0;
          const bool cameWhole =
              fetchWhole(url, dest, spec.value("size", 0LL), stopAskedNow, fetchAnswer);
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
            noteError(job, transferred != 0 ? "the fetch did not finish for " + url
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

    static std::string nameOf(const std::string &path)
    {
      return std::filesystem::path(path).filename().string();
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
      std::string error;
      // The load hand opens the model the way the page's switches name it, so a
      // model loaded by hand stands on the same device the next run would put it
      // on - and a run that names another of them moves the weights.
      sd_ctx_t *ctx = openContext(modelId, prefOn("offload"), prefOn("sage_attn"), prefOn("cpu"),
                                  error);
      if (!ctx)
      {
        noteError(job, error);
        markState(job, ols::kStateError);
        return;
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
      std::uint32_t crc = 0xFFFFFFFFu;
      for (char c : bytes)
      {
        crc ^= static_cast<unsigned char>(c);
        for (int bit = 0; bit < 8; ++bit)
          crc = (crc >> 1) ^ (0xEDB88320u & ~(crc & 1u) + 1u);
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
      // than its first second.
      const int shown = count < kPreviewFrames ? count : kPreviewFrames;
      std::vector<int> picks;
      picks.reserve(static_cast<std::size_t>(shown));
      for (int at = 0; at < shown; ++at)
        picks.push_back(static_cast<int>((static_cast<long long>(at) * count) / shown));

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
      putBE32(control, static_cast<std::uint32_t>(shown));
      putBE32(control, 0u); // forever
      pngChunk(out, "acTL", control);

      std::uint32_t sequence = 0u;
      for (int index : picks)
      {
        const sd_image_t &frame = frames[index];
        if (frame.width != srcW || frame.height != srcH)
          continue;
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
        control2.push_back('\x00'); // the frame is complete: nothing is kept from
        control2.push_back('\x01'); // a whole second is divided into previewFps
        putBE16(control2, static_cast<std::uint16_t>(previewFps));
        putBE16(control2, 0u);
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
      if (sequence < 1u)
        return false;
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
    // codec for - a much bigger file that even the plainest player reads.
    static bool writeAviClip(const std::string &path, const sd_image_t *frames, int count, int fps,
                             const sd_audio_t *audio, bool keepTheFramesWhole = false)
    {
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

      std::string avih;
      putLE32(avih, static_cast<std::uint32_t>(1000000 / fps));
      putLE32(avih, hasAudio ? align * rate : 0u);
      putLE32(avih, 0u);
      putLE32(avih, 0x110u);
      putLE32(avih, static_cast<std::uint32_t>(count));
      putLE32(avih, 0u);
      putLE32(avih, hasAudio ? 2u : 1u);
      putLE32(avih, frameBytes);
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
      putLE32(video, static_cast<std::uint32_t>(count));
      putLE32(video, frameBytes);
      putLE32(video, static_cast<std::uint32_t>(-1));
      putLE32(video, 0u);
      putLE32(video, 0u);
      putLE16(video, static_cast<std::uint16_t>(wide));
      putLE16(video, static_cast<std::uint16_t>(tall));

      std::string videoFormat;
      putLE32(videoFormat, 0x28u);
      putLE32(videoFormat, wide);
      // plain pixels stand bottom row up, which is how the height is said; JPEG
      // frames carry their own order.
      putLE32(videoFormat, rawFrames ? 0u - tall : tall);
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
        // The sound stream is PCM at the rate the model carried it at. The numbers
        // are the ones the writers this container is read by stand here: the type is
        // uncompressed PCM, the rate, the whole count of samples, the ceiling of one
        // chunk, and the block alignment - the count and the alignment are what let a
        // reader seek to a moment of a clip without walking it.
        std::string audioHead;
        audioHead += "auds";
        putLE32(audioHead, 1u);
        putLE16(audioHead, 0u);
        putLE16(audioHead, 0u);
        putLE32(audioHead, 0u);
        putLE32(audioHead, 0u);
        putLE32(audioHead, 1u);
        putLE32(audioHead, rate);
        putLE32(audioHead, 0u);
        putLE32(audioHead, static_cast<std::uint32_t>(pcm.size() / align));
        putLE32(audioHead, 8192u);
        putLE32(audioHead, static_cast<std::uint32_t>(-1));
        putLE32(audioHead, align);
        putLE32(audioHead, 0u);
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
      index.reserve(static_cast<std::size_t>(count) + 1u);

      std::vector<std::uint8_t> jpeg;
      std::string raw;
      for (int at = 0; at < count; ++at)
      {
        const sd_image_t &frame = frames[at];
        if (frame.width != wide || frame.height != tall)
          continue;
        const char *payload;
        std::size_t payloadSize = 0u;
        if (rawFrames)
        {
          raw.clear();
          appendRawFrameBytes(raw, frame, rowStride);
          payload = raw.data();
          payloadSize = raw.size();
        }
        else
        {
          jpeg.clear();
          if (!stbi_write_jpg_to_func(appendJpegBytes, &jpeg, static_cast<int>(wide),
                                      static_cast<int>(tall), static_cast<int>(frame.channel),
                                      frame.data, 90))
            return false;
          payload = reinterpret_cast<const char *>(jpeg.data());
          payloadSize = jpeg.size();
        }
        ClipIndexEntry entry{"00dc", 0x10u, static_cast<std::uint32_t>(out.size()),
                             static_cast<std::uint32_t>(payloadSize)};
        out += entry.kind;
        putLE32(out, entry.size);
        out.append(payload, payloadSize);
        index.push_back(entry);
        if (payloadSize % 2u != 0u)
          out.push_back('\0');
      }
      if (index.empty())
        return false;

      if (hasAudio)
      {
        ClipIndexEntry entry{"01wb", 0u, static_cast<std::uint32_t>(out.size()),
                             static_cast<std::uint32_t>(pcm.size())};
        out += entry.kind;
        putLE32(out, entry.size);
        out += pcm;
        index.push_back(entry);
        if (pcm.size() % 2u != 0u)
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
      static const int kMaxBatchPerRun = 4;
      static const int kMaxReferenceImages = 10;
      const int width = request.value("width", def->width > 0 ? def->width : kDefaultSide);
      const int height = request.value("height", def->height > 0 ? def->height : kDefaultSide);
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
      // how long a clip is, in frames rather than in seconds: a model whose card
      // says its work moves names a length beside it, and the bounds here are what
      // this machine will run. The engine asks for the frame count to sit on the
      // grid its temporal compression works on and rounds a count that is off the
      // grid up on its own (docs/minimax_h3.md), so what comes back can be a few
      // frames longer than what was asked - which is why the record keeps what
      // came out rather than what was asked.
      static const int kMinFrames = 5;
      static const int kMaxFrames = 241;
      const int framesAsked = request.value("frames", def->frames > 0 ? def->frames : kMinFrames);
      if (def->video && (framesAsked < kMinFrames || framesAsked > kMaxFrames))
      {
        noteError(job, "the clip asked for is outside " + std::to_string(kMinFrames) + ".." +
                           std::to_string(kMaxFrames) + " frames");
        markState(job, ols::kStateError);
        return;
      }
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
      {
        std::lock_guard<std::mutex> lock(gCtxMutex);
        ++gGenerating[modelId];
      }
      std::string error;
      // "cpu" names which part of the machine this run works on; a run that
      // names nothing takes the switch the page left standing. the three switches
      // are named here once because the run's own record carries them: what a run
      // cost is filed under the switches it ran with, and a name read twice can
      // drift apart from the one that was used.
      const bool offloadAsked = request.value("offload", false);
      const bool sageAsked = request.value("sage_attn", false);
      const bool cpuAsked = request.value("cpu", prefOn("cpu"));
      sd_ctx_t *ctx = openContext(modelId, offloadAsked, sageAsked, cpuAsked, error);
      if (!ctx)
      {
        {
          std::lock_guard<std::mutex> lock(gCtxMutex);
          --gGenerating[modelId];
          finishPendingLocked();
        }
        noteError(job, error);
        markState(job, ols::kStateError);
        return;
      }

      sd_sample_params_t sample;
      sd_sample_params_init(&sample);
      sample.guidance.txt_cfg = guidance == 0.0f ? 1.0f : guidance;
      sample.sample_steps = steps;
      sample.sample_method = sd_get_default_sample_method(ctx);
      sample.scheduler = sd_get_default_scheduler(ctx, sample.sample_method);
      // The sampler and scheduler the model's own card names, asked in the
      // engine's words: the engine's choice for the model's family stands where
      // the card names nothing, and a name the engine does not know is left as
      // the engine's own choice rather than answered as a failure of the run.
      // Which solver this run really sampled with is kept beside it, named in the
      // words the picture table uses, so the run's cost is filed under the solver
      // that did the work and not under a name the engine quietly replaced.
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
      }
      if (!def->scheduler.empty())
      {
        const enum scheduler_t asked = str_to_scheduler(def->scheduler.c_str());
        if (asked != SCHEDULER_COUNT)
        {
          sample.scheduler = asked;
          schedulerUsed = def->scheduler;
        }
      }
      std::vector<float> sigmas;
      if (!def->sigmas.empty())
      {
        std::istringstream parts(def->sigmas);
        std::string part;
        while (std::getline(parts, part, ','))
          sigmas.push_back(std::stof(part));
        sample.custom_sigmas = sigmas.data();
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
      img.vae_tiling_params.enabled = request.value("tiling", false);
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
      // repeats of one ask agree on one estimate. The guidance is the number the
      // run used, which is what a zero means here - the engine takes a zero as one.
      {
        bool deviceCpu = false;
        {
          std::lock_guard<std::mutex> ctxLock(gCtxMutex);
          const auto standing = gCtxCpu.find(modelId);
          if (standing != gCtxCpu.end())
            deviceCpu = standing->second;
        }
        std::lock_guard<std::mutex> lock(gMutex);
        json &jobRecord = gJobs[job];
        json asked = json::object();
        asked["width"] = width;
        asked["height"] = height;
        asked["steps"] = steps;
        asked["guidance"] = guidanceAsked == 0.0 ? 1.0 : guidanceAsked;
        asked["sampler"] = samplerUsed;
        asked["scheduler"] = schedulerUsed;
        asked["cache"] = cacheOn;
        asked["device"] = deviceCpu ? std::string("cpu") : std::string("card");
        asked["offload"] = offloadAsked;
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
      };
      CancelProbe probe{job, ctx, sample.sample_steps, img.batch_count, 0};
      auto thunk = [](int step, int stepsTotal, float, void *userData)
      {
        auto *probe = static_cast<CancelProbe *>(userData);
        const std::string jobId = probe->job;
        // the engine counts its own passes, which are not always one per step the
        // run named - one cold run reported 140 for a window of four - so a page
        // would print a step count and a rate for a window that run never asked
        // for; what the page reads is progress inside the window this run named.
        const int window = probe->sampleSteps > 0 ? probe->sampleSteps : 1;
        const int ticks = stepsTotal > 0 ? stepsTotal : 1;
        const int pictures = probe->pictures > 0 ? probe->pictures : 1;
        int within = static_cast<int>((static_cast<long long>(step + 1) * window) / ticks);
        if (within > window)
          within = window;
        if (within < 1)
          within = 1;
        // a run that makes more than one picture names that window for every one
        // of them: a page that reads only one picture's window calls a two-picture
        // run done while its second picture is still being made. the steps of all
        // the pictures this run was asked for are the work it has left.
        const int done = probe->picturesDone * window + within;
        noteProgress(jobId, done, window * pictures);
        if (within >= window && probe->picturesDone < pictures - 1)
          probe->picturesDone += 1;
        if (cancelledRequested(jobId))
          sd_cancel_generation(probe->ctx, SD_CANCEL_ALL);
      };
      std::lock_guard<std::mutex> generationLock(gGenerationMutex);
      sd_set_progress_callback(thunk, &probe);

      sd_image_t *images = nullptr;
      int produced = 0;
      bool ok = false;
      // a clip's sound, when one came out: it is part of what the model made
      // rather than something this machine adds afterwards, so it is kept beside
      // the frames and freed with them.
      sd_audio_t *clipAudio = nullptr;
      int encodedFps = 0;
      struct Release
      {
        sd_image_t **images;
        int *produced;
        const std::string *model;
        sd_audio_t **audio;
        ~Release()
        {
          sd_set_progress_callback(nullptr, nullptr);
          {
            std::lock_guard<std::mutex> ctxLock(gCtxMutex);
            --gGenerating[*model];
            finishPendingLocked();
          }
          if (*audio)
            free_sd_audio(*audio);
          // What a run made is allocated inside the library, and its header says
          // plainly that the caller must hand it back through free_sd_images
          // ("caller needs to call free_sd_images to free the memory after use ...
          // This helps avoid CRT problems on Windows when memory is allocated in
          // the library but freed in the caller, which may use a different CRT"),
          // include/stable-diffusion.h:641.
          if (*images)
            free_sd_images(*images, *produced);
        }
      } release{&images, &produced, &modelId, &clipAudio};
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
        // the engine said no and carries no words of its own at this door: what is
        // known is that the run asked for a picture or a clip and none came out.
        noteError(job, def->video ? "the clip did not come out" : "the picture did not come out");
        markState(job, ols::kStateError);
        return;
      }

      const long long created = nowSeconds();
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
        const bool clipWritten = writeAviClip(gOutputDir + "/" + clipName, images, produced, rate, clipAudio);
        bool posterWritten = false;
        if (images[0].data)
          posterWritten = stbi_write_png((gOutputDir + "/" + posterName).c_str(),
                                         static_cast<int>(images[0].width), static_cast<int>(images[0].height),
                                         static_cast<int>(images[0].channel), images[0].data,
                                         static_cast<int>(images[0].width * images[0].channel)) != 0;
        const bool previewWritten =
            writeApngPreview(gOutputDir + "/" + previewName, images, produced, kPreviewFps, kPreviewSide);
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
        // the record of a clip says what it is made of, in the words this
        // machine's own file door answers with, so a page that holds the record
        // can reach the part it wants rather than the whole clip: a card draws
        // the poster, moves the preview, and offers the clip and the sound to a
        // person. What is absent is said as absent rather than guessed at.
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
                       {"frames", produced},
                       {"fps", rate},
                       {"duration_ms", static_cast<long long>(produced) * 1000 / rate},
                       {"poster", posterWritten ? posterName : nullptr},
                       {"preview", previewWritten ? previewName : nullptr},
                       {"audio", trackWritten ? trackName : nullptr},
                       {"raw", rawWritten ? rawName : nullptr},
                       {"channels", clipAudio ? static_cast<long long>(clipAudio->channels) : 0},
                       {"sample_rate", clipAudio ? static_cast<long long>(clipAudio->sample_rate) : 0},
                       {"references", json(sources)}};
        writeFileText(gOutputDir + "/" + clipName + ".json", record.dump());
        noteResult(job, "/files/outputs/" + clipName);
        json event = {{"type", "video"},
                      {"clip", "/files/outputs/" + clipName},
                      {"poster", posterWritten ? "/files/outputs/" + posterName : nullptr},
                      {"preview", previewWritten ? "/files/outputs/" + previewName : nullptr},
                      {"audio", trackWritten ? "/files/outputs/" + trackName : nullptr},
                      {"raw", rawWritten ? "/files/outputs/" + rawName : nullptr},
                      {"frames", produced},
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
                       {"created", created},
                       {"model", modelId},
                       {"name", def->name},
                       {"width", image.width},
                       {"height", image.height},
                       {"prompt", prompt},
                       {"steps", steps},
                       {"seed", img.seed},
                       {"references", json(sources)}};
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
      if (kind == "generate")
      {
        const json probe = json::parse(payloadJson, nullptr, false);
        if (probe.is_discarded())
        {
          gEngineError = "Invalid request body";
          return std::string();
        }
        if (wanted.empty())
          wanted = probe.value("model", std::string());
        quant = probe.value("quant", std::string());
      }
      else if (!payloadJson.empty())
      {
        // a download and an open carry the same shape: which precision of the
        // model is meant, when a page named one.
        const json probe = json::parse(payloadJson, nullptr, false);
        if (!probe.is_discarded())
        {
          if (wanted.empty())
            wanted = probe.value("model", std::string());
          quant = probe.value("quant", std::string());
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
        gChosenQuant[wanted] = quant;
      }
      const std::string job = makeId("img");
      {
        std::lock_guard<std::mutex> lock(gMutex);
        gJobs[job] = {{"id", job},
                      {"kind", kind},
                      {"model", wanted},
                      {"state", ols::kStateRunning},
                      {"started_at", nowSeconds()}};
        if (!quant.empty())
          gJobs[job]["quant"] = quant;
        gEvents[job];
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
      std::lock_guard<std::mutex> lock(gMutex);
      ensureTimingsLoadedLocked();
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
        // one step of the window is what the whole run costs divided by the steps
        // it asked for: a page measures the work left in a run with this, and the
        // division belongs here, where the step count is known, not where a page
        // would have to guess it.
        const long long steps = row.value("steps", 0LL);
        if (steps > 0)
          row["mean_ms_per_step"] = row["mean_ms"].get<long long>() / steps;
        const long long pictures = row.value("batch", 1LL);
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
        auto &frames = gEvents[id];
        while (sent < frames.size())
        {
          const std::string frame = "data: " + frames[sent] + "\n\n";
          lock.unlock();
          const bool alive = OpenLocalServices::writeStreamChunk(client, frame);
          lock.lock();
          if (!alive)
            return;
          ++sent;
        }
        if (gEnded[id])
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
        const bool present = modelInstalled(def.id);
        long long bytes = 0;
        // What of this model physically sits in the room, stood behind or not. The
        // two are different sentences and were told as one: a model whose files were
        // copied in by hand - or read, hashed and left unvouched by the bug above -
        // has its bytes on this machine, and a page that only ever heard on_disk:
        // false called that weights not downloaded and offered to fetch them again.
        long long lyingHere = 0;
        bool unchecked = false;
        // the names this model's files stand under, in the same words every other
        // table gives them: a path beside the weights room. a model whose files
        // are not here still names what they would be, and says so by its own
        // on_disk flag - the names are the fact a page reads the quant off.
        json files = json::array();
        for (const auto &file : modelFiles(&def, std::string()))
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
            bool present = true;
            long long total = 0;
            for (const auto &file : offer.files)
            {
              if (!slotOnDisk(&def, file.second))
                here = false;
              std::error_code ec{};
              if (std::filesystem::file_size(modelRoom + localNameOf(file.second), ec))
                present = false;
              total += bytesOf(file.second);
            }
            json one = {{"name", offer.label}, {"here", here}};
            one["present"] = present;
            if (total > 0)
              one["bytes"] = total;
            quants.push_back(one);
          }
        }
        std::string held;
        {
          std::lock_guard<std::mutex> lock(gMutex);
          const auto found = gChosenQuant.find(def.id);
          if (found != gChosenQuant.end())
            held = found->second;
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
                       {"clip_skip", def.clip_skip},
                       {"flow_shift", def.flow_shift == INFINITY ? -1.0f : def.flow_shift},
                       {"prompt", def.prompt},
                       {"specialty", def.specialty},
                       {"negative", def.negative},
                       {"sigmas", def.sigmas},
                       {"edit", def.edit},
                       {"redraw", def.redraw},
                       {"refs", def.refs},
                       // Whether this model's work is a moving picture, and the
                       // clip length and frame rate its card runs it at home at:
                       // a page reads these rather than guessing from what a file
                       // is named, and a run that names no clip length draws them.
                       {"video", def.video},
                       {"frames", def.frames},
                       {"fps", def.fps},
                       {"files", files},
                       {"on_disk", present},
                       {"bytes", bytes},
                       // present_bytes: what of this model sits in the room whether or
                       // not anything has read it. unchecked: some of it is here with
                       // nothing stood behind it. Both are said apart from on_disk so
                       // a machine holding the bytes is never called a machine without
                       // them - which is what a page showed as weights not downloaded.
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
        // bytes: a page that says a model is open can say where it stands.
        resident.push_back({{"model", entry.first},
                            {"bytes", charge},
                            {"device", (gCtxCpu.count(entry.first) && gCtxCpu[entry.first]) ? "cpu" : "card"}});
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
          continue;
        }
        if (!prefKeyNamed(key))
          continue;
        if (entry.value().is_boolean())
          gPrefs[key] = entry.value().get<bool>() ? "1" : "0";
        else if (entry.value().is_string())
          gPrefs[key] = entry.value().get<std::string>() == "1" ? "1" : "0";
      }
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
      std::sort(items.begin(), items.end(), [](const json &a, const json &b)
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

    // Whether a name a page gave can stand as one of this machine's files: one
    // name, no path parts, and nothing that could step out of the folder the
    // door reads inside. One rule serves every door that names a file.
    static bool safeItemName(const std::string &name)
    {
      return !name.empty() && name.find('/') == std::string::npos &&
             name.find("..") == std::string::npos && name.find('\\') == std::string::npos;
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
      std::sort(items.begin(), items.end(), [](const json &a, const json &b)
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
        return false;
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
        return false;
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
          gRefError = "Could not make a place for the kept picture";
        }
        return false;
      }
      std::error_code copyEc{};
      if (!std::filesystem::copy_file(from, kept,
                                      std::filesystem::copy_options::overwrite_existing, copyEc))
      {
        {
          std::lock_guard<std::mutex> lock(gRefErrorMutex);
          gRefError = "Could not copy the picture into the references folder: " +
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
          gRefError = "Could not write the record next to the kept picture";
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
          gRefError = "Unsupported format: expected png, jpg, or webp";
        }
        return false;
      }
      const std::string bytes = ols::decodeBase64(imageB64);
      if (bytes.empty())
      {
        {
          std::lock_guard<std::mutex> lock(gRefErrorMutex);
          gRefError = "No image bytes received";
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
          gRefError = "Could not write the picture into the references folder";
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
          gRefError = "Could not write the record next to the kept picture";
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
      const std::string base = kind == "outputs" ? gOutputDir
                                                 : (kind == "refs" ? gRefsDir : gWeightsDir);
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
        : mThreads(threads),
          mWeightsDir(weightsDir),
          mOutputDir(outputDir),
          mHfToken(hfToken),
          mCivitaiKey(civitaiKey),
          mKeptMax(keptMax)
    {
      gWeightsDir = weightsDir;
      gOutputDir = outputDir;
      {
        const std::filesystem::path out{outputDir};
        const std::filesystem::path data = out.parent_path();
        gRefsDir = (data / "refs").string();
      }
      gThreads = threads;
      gHfToken = hfToken;
      gCivitaiKey = civitaiKey;
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
      mHfToken = token;
      gHfToken = token;
    }

    void ImageService::setCivitaiKey(const std::string &token)
    {
      mCivitaiKey = token;
      gCivitaiKey = token;
    }

  } // namespace imagegen
} // namespace ols
