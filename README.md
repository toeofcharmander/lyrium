Since DAO is a 32bit game, it has a cap of 2gb address space available. The longer you play, the more that space becomes fragmented. Every texture wants a contiguous block, and when none are available DirectX will throw an out of memory error, which causes the texture flickering causing the game to crash. The failure was there was 20mb of free memory, split across ~430 separate gaps. So memory was available, just not in one piece (so, fragmentation).

The real issue was memory fragmentation. I found that at startup, the engine reserves 795mb for it's own memory pool, ~286mb for module images (these are dll's, CUDA, physX, etc), leaving ~229mb for managed texture duplicates.

As this doesn't happen on console, it's likely due to the higher resolution textures available on PC. 

The engine writes textures by locking that duplicate, and DEFAULT textures textures can't be locked down. So when the game locks a texture, it now gets handed a temporary buffer instead. It fills that with the relevant pixels, copies it over to the graphics card, and discards the buffer immediately. The engine can't tell the difference. 

Big thanks to Nathan Baggs for the initial research into this.
