# Utah jersey: leftover Coyotes crest — findings (2026-10-27)

Symptom: Utah jerseys show Mammoth base art, but a full-colour Coyotes kachina still draws on the chest (and a
shoulder patch) in team select and in game. "UTAH MAMMOTH" name text is already fine.

## Tools built (all in C:\nhl24build)
- run-utah-dump.ps1 / run-bindlog.ps1: texture dump (user\texture_dump, user\texture_dump2) with NHLPORT_LOG_BIND=1.
- bindlog.patch + bindlog2.patch (+ rebuild-with-bindlog*.ps1): logs "DRAWBIND stage N pgm H: [content_hash WxH fmt]..."
  for every unique draw (in user\log\shad_log.txt). content_hash == dump/override file name.
  v2 also hashes/dumps images that never went through RefreshImage (content_hash 0).
- Override gating in ApplyTextureOverride (texture_cache.cpp): only hashes textures that are small square UI sizes
  or whose WxH equals the size of an existing override file. NHLPORT_LOG_BIND=1 removes that gate.

## What the logs show
- 14,839 unique draws; 769 use an overridden Utah jersey texture (e.g. 5f236bf75943d23f 2048x2048 Bc3Srgb).
- Every NON-overridden, hashed texture co-bound with those draws is something else (Stanley Cup patch 87549887/60eabd59,
  gear masks, faces, shared 256x256 noise b7ae47b6...). None contains the kachina.
- Several co-bound textures have content hash 0 (never hashed): 512x512 Bc3Srgb (~199 draws), 512 Bc5, 512 Bc3Unorm,
  1024 Bc5, 128x128 Bc3Unorm/Bc5. A runtime-generated or streamed crest would look like this.
- Dump caveat: textures that are overridden are NOT written to the dump, and PIL cannot read sRGB DDS (DXGI 72/78/99)
  without patching the DXGI id to the UNORM one (see ~/scan/ddsload.py approach: 72->71, 78->77, 99->98).
- Real Coyotes texture found in dump: 89b65be9f83aa094 (512x512 Bc1Srgb), collar/"COYOTES" wordmark + kachina layout.
  Not yet overridden. Only seen in 1 draw.

## Open questions
1. Which texture/draw actually paints the chest kachina? Zero-hash 512x512 Bc3Srgb is the main suspect.
2. Black socks on some players: not investigated (likely uniform table yUAn colours, see scratch\apply_utah_uniforms.py).

## Update 2026-10-27 (Claude)
- Black socks / black net: caused by 38 fully black or fully transparent override DDS files. Removed them (kept in texture_override_off); the 45 non-blank overrides are in texture_override and are fine.
- Net posts: Utah home arena paints them with the team colour from kOtt row 22 field niVV (was 750000, Antigravity's edit made it 1e4268). Set to c8102e, posts are red now. Other changed fields: XooE, tBBf, dmdk, onIR, wQkS, oYnX, xDrX, eYeY, FRXv.
- Coyotes crest + shoulder patch still show on Utah jerseys (team select and in game). Overrides of the jersey base fabric (5f236bf75943d23f) don't touch them.
- bindlog3 layer dumps (user\texture_dump2, *_L<n>_*.dds) are NOT valid images (noise), and DebugDumpLayers readbacks caused a Vulkan device-lost crash. The 512x512 Bc3Srgb 8-layer array @0x11ba1a0000 and the 128x128 Bc3Unorm/Bc5 arrays have zero content hash, so they are GPU-composed, not file-loaded.
- Conclusion: the crest must be replaced at its SOURCE asset, before compositing.

## Question for Antigravity
Which asset or texture is the Coyotes kachina logo that gets composited onto the jersey chest and shoulder patch in game? I need the source image (its XXH3 hash or the Frostbite resource name / path), not the finished jersey texture, so I can override it with the Utah logo in user\texture_override. Please also say how you found it (which hook or table), and whether you already know the shoulder patch source separately from the chest crest.

## RESOLVED 2026-10-30 (Claude): Coyotes chest crest + shoulder patches on Utah
- Source: three 512x512 Bc3Srgb texture arrays (refreshed from CPU, hashed after upload). Array C (8 layers): L0 ba10aa88955cd737 = Coyotes chest kachina, L1/L2 e69f43c5d25dcaeb = shoulder patches. Array A L0 7c6ed265... = Montreal chest crest (blanking all layers / by ordinal broke other teams).
- Fix (debug-build feature, uncommitted in texture_cache.cpp ApplyTextureOverride): user\array_blank.txt = `512x512:Bc3Srgb:0f:H` blanks only layers 0-3 whose mip2 hash is listed in user\array_blank_hashes.txt (ba10aa88955cd737, e69f43c5d25dcaeb). Verified in team select and in game; other teams keep crests.
- Spec fields: WxH:FormatSubstr:layerMaskHex[:N|H]. N = Nth array by first-seen order (unstable between scenes), H = hash list. ARRAYHASH lines in log list hashes of layers 0-3.
- Only nhl24-arrayblank.exe / nhl24-teamsel.exe contain this; nhl24-clean.exe and nhl24.exe do not.
