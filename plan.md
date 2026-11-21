# plan.md — the work that is still open
this is a living task list.
when work is complete, edit this document, add any additional user guidance received during the session, and move the work item to Awaiting User Verification, when completed.
do not edit or remove empty headers, or document format examples.
working rules, standards, and the commit and push discipline live in `dev.md`; machine access specifics live in `C:\Projects\local.md`.
always read this file immediately before making updates: the user may change it at any time. never revert user changes: write around them.

## 1. TODO - not actionable yet.
these tasks are upcoming, but some of the details have not been firmly decided.
do not edit, or act on anything in this section, unless instructed by user

## 2. PENDING - actionable work.
user may add or update items here during your session- read it carefully before making updates.
keep report on changes BRIEF. The entire section should not exceed 20 lines.

review format. do not edit or remove this format:

- <job title>
  Prompt: <full user message>
  Follow-on guidance: <summary of any changes the user makes during the session>
  Changes: <all files modified, summary with line change counts, brief functional summaries, etc>


## 3. Awaiting User Verification
when work is complete, edit this document, add any additional user guidance received during the session, and move the work item here, when completed.

- Model cards wear a watermark of the model's own mark
  Prompt: use browser mcp/web search, review all image models cards; find or generate unique icons, for each model in the image studio. display the images as a watermark, on the background of the model card, for easy visual identification … for example- the qwen ones should have the qwen logo, etc … use the actual image models, with reference images and your prompts, to generate the originals, then clean up backgrounds/format as needed
  Follow-on guidance: the first pass drew from words alone and was rejected - "you are missing reference images"; each mark then had to start from that family's published logo or card art, fed through the img2img door. The user then narrowed the drawer: "use SDXL Turbo or klein only- don't use each model to make it's own logo."
  Changes: webui/model-marks.js new (18 lines: twelve marks as data-URI PNG silhouettes, 78.7 KB raw); webui/app.js +8 (renderModels names each card's mark and hangs it on the card as --model-mark); webui/style.css +23 (the mark is painted through a mask on .model-main::before, so the colour stays the card's ink at any theme); webui/imagegen.html +1 (model-marks.js loaded after common.js); README.md +1 sentence on the marks.
  The marks: each family's reference picture (Qwen-Image logo, Pony mascot, Animagine and Juggernaut card art, CyberRealistic portrait, MiniMax-H3 card image, the bfl.ai wordmark, SDXL sample tile, Z-Image showcase) was kept through the refs door and redrawn through FLUX.3 Klein or SDXL Turbo with a prompt naming the emblem; FLUX.3 drew the wordmark, Qwen hexagon and Z hexagon, SDXL drew the latent tile, RealVis lens, Pony head, Cyber bust, Animagine head, MiniMax film frame. Where a card publishes no picture (RealVisXL, Lustify) the mark was drawn from words alone. A cleaned pass dropped the field, cropped and shrank each drawing to 128 and carried its shades as alpha; the turbo Qwen mark is the family mark with a drawn lightning bolt cut out of it. Verified live in the browser with a clean console.
