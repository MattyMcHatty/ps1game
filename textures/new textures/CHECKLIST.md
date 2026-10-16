# New textures: 4bpp conversion checklist

Each texture is converted with an exact palette (no re-quantising) by
`tools/convert_new_textures.py`, at the VRAM slot and CLUT slot of the TIM it
replaces unless a note says otherwise.

## Skipped (send these back)

Still more than 16 palette entries after the remake:
- [ ] demon dog.png (16 colours + transparency = 17; also a new 2x2 layout where the old sheet is a 3-frame strip, so demondog.c's UVs will need changing)

Remade and converted (second batch):
- [x] hadad_idle, hadad_stp1, hadad_stp2 (128x128, moved to (608,0), (928,0), (608,128); hadad.c reads UVs from the TIM)
- [x] mgn_ky_stn -> (848,320)
- [x] flm_rnds -> (848,256)
- [x] stnd_rnds -> (832,256)
- [x] helluminator -> (832,320)
- [x] valve handle (menu icon only, same slot)
- [x] anzu_tab moved to (832,0), shared with room art the piano room never draws, to free (832,256) for the four pickups above

Held back by decision:
- [ ] mushroom_head.png (8bpp 128x128 sprites need about 2.4x the old VRAM; skipped for now)
- [ ] grinder.png (left at 8bpp: at 4bpp its slot lands at U 128+, which the Garden's texture window wraps)

## 1. Straight swaps (same slot, 8bpp to 4bpp)

128x128:
- [x] arms2  - [x] bed  - [x] bones  - [x] catacomb inner door  - [x] cobblestones
- [x] crib  - [x] drain  - [x] dresser  - [x] flowerbed  - [x] fountain
- [x] gaol_mud  - [x] greenhouse  - [x] hatch  - [x] heads  - [x] hedge
- [x] kchn_wl  - [x] ladder  - [x] lamashtu tablet  - [x] legs  - [x] loculus
- [x] oil_container  - [x] pipe  - [x] plinth  - [x] plinth_diamond
- [x] rafflesia1  - [x] rafflesia2  - [x] rusty  - [x] stables wood  - [x] stn_gls
- [x] torsos  - [x] trck_clue  - [x] vines (its mesh bakes the tpage: rebake vines.smd)
- [x] grdn_gte (room art; the gate panel is section 4)

32x32 / 64x64 / other:
- [x] cppr_pt  - [x] key  - [x] sml_med (64x64 -> 32x32; UVs come from the TIM)
- [x] creep  - [x] crucifaxe  - [x] poison_flower_base
- [x] ghit  - [x] ls_atk  - [x] ls_idle  - [x] shadow  - [x] zombie_alert  - [x] zombie_sleep

## 2. 16bpp to 4bpp (need a palette and a mesh rebake)
- [x] wd_dr: new CLUT slot; rebake fat_door.smd
- [x] wdcrate: new CLUT slot; crate.c uploads the CLUT; rebake wdcrate.smd

## 3. Pickup sprites moved
Pickups draw under the room's 128 texture window. At 4bpp these slots would put
them at U 128+, so they move into space the 16bpp textures freed (U < 128, Voff < 128).
- [x] graveolver -> (912,0)
- [x] hatch_key (64x64 -> 32x32) -> (896,80)
- [x] pno_key -> (904,96)
- [x] bl_ky_stn -> (920,96)
- [x] ylw_ky_stn -> (648,256)

## 4. Rework
- [x] Double door: door panels use both halves of double_door; dbl_dr_hlf retired
- [x] Inner double door: both halves of inr_dbl_dr; inr_dbl_dr_half retired
- [x] Garden gate: both halves of grdn_gte; grdngtl and grdngtr retired
- [x] Anzu: anzu_tab (4bpp, one texture) split across the six tiles and the Tablets prop; anzu1-6 retired
- [x] Crawler: crawler.png -> crawler_a/b (two 4bpp halves, one palette, same slots)
- [x] Lumberer: lumberer.png -> lumberer_a/b (two 4bpp halves, one palette, same slots)

## 5. Bookkeeping and build
- [x] disc.xml: add ANZUTAB, drop retired TIMs
- [x] tools/gen_tim_slots.py -> src/tim_slots.h
- [x] tools/vram_map.py -> tools/VRAM_MAP.txt (no new overlaps)
- [x] tools/check_tex_banks.py, tools/check_texmgr_cap.py, tools/heap_budget.py
- [x] Decode every new TIM and compare it with its PNG
- [x] cmake --build, mkpsxiso, boot test (headless boot to the title, no exceptions; rooms not play-tested)

## Results
- 64 TIMs written (both batches); every one decodes back to its PNG exactly (0 mismatched pixels).
- VRAM map: no unexpected pixel or CLUT collisions.
- Texture banks: Catacombs 524 KB -> 372 KB, Mansion 512 KB -> 472 KB, Garden 424 KB -> 342 KB.
- Retired and deleted: dbl_dr_hlf, inr_dbl_dr_half, grdngtl, grdngtr, anzu1-6 (.tim only; PNGs kept).
