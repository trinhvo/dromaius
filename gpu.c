#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "gbemu.h"

#define GB_SCREEN_WIDTH  160
#define GB_SCREEN_HEIGHT 144

#define SCREEN_WIDTH  GB_SCREEN_WIDTH
#define SCREEN_HEIGHT GB_SCREEN_HEIGHT

#define DEBUG_WIDTH   150
#define DEBUG_HEIGHT  80

#define WINDOW_SCALE  2

extern settings_t settings;
extern mem_t mem;
extern cpu_t cpu;
extern gpu_t gpu;

SDL_Window *mainWindow;
SDL_Window *debugWindow;

SDL_Renderer *screenRenderer;
SDL_Renderer *debugRenderer;

SDL_Texture *screenTexture;
SDL_Texture *debugTexture;

uint32_t *screenPixels;
uint32_t *debugPixels;

void initDisplay() {
	if(SDL_Init(SDL_INIT_EVERYTHING) == -1) {
		printf("Failed to initialize SDL.\n");
		exit(1);
	}

	SDL_Window *debugWindow = SDL_CreateWindow("GB Emulator - debug",
	                      SDL_WINDOWPOS_UNDEFINED,
	                      SDL_WINDOWPOS_UNDEFINED,
	                      WINDOW_SCALE * DEBUG_WIDTH, WINDOW_SCALE * DEBUG_HEIGHT,
	                      SDL_WINDOW_OPENGL);
	
	SDL_Window *mainWindow = SDL_CreateWindow("GB Emulator",
	                      SDL_WINDOWPOS_CENTERED,
	                      SDL_WINDOWPOS_CENTERED,
	                      WINDOW_SCALE * SCREEN_WIDTH, WINDOW_SCALE * SCREEN_HEIGHT,
	                      SDL_WINDOW_OPENGL);

	// Position the debug window right of the main window
	int xmain, ymain;
	SDL_GetWindowPosition(mainWindow, &xmain, &ymain);
	SDL_SetWindowPosition(debugWindow, xmain + WINDOW_SCALE * SCREEN_WIDTH, ymain);


	if(!mainWindow || !debugWindow) {
		printf("Failed to create a window.\n");
		exit(1);
	}

	screenRenderer = SDL_CreateRenderer(mainWindow, -1, 0);
	debugRenderer = SDL_CreateRenderer(debugWindow, -1, 0);

	SDL_RenderSetLogicalSize(screenRenderer, SCREEN_WIDTH, SCREEN_HEIGHT);
	SDL_RenderSetLogicalSize(debugRenderer, DEBUG_WIDTH, DEBUG_HEIGHT);

	screenTexture = SDL_CreateTexture(screenRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_WIDTH, SCREEN_HEIGHT);
	debugTexture = SDL_CreateTexture(debugRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, DEBUG_WIDTH, DEBUG_HEIGHT);

	// filtering
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
}

void initGPU() {
	int i, j;

	gpu.mode = GPU_MODE_HBLANK;
	gpu.mclock = 0;
	gpu.r.line = 0;
	gpu.r.scx = 0;
	gpu.r.scy = 0;
	gpu.r.flags = 0;

	screenPixels = (uint32_t *)calloc(SCREEN_WIDTH * SCREEN_HEIGHT, sizeof(uint32_t));
	debugPixels = (uint32_t *)calloc(DEBUG_WIDTH * DEBUG_HEIGHT, sizeof(uint32_t));

	gpu.vram = (uint8_t *)calloc(0x2000, sizeof(uint8_t));
	gpu.oam = (uint8_t *)calloc(0xA0, sizeof(uint8_t));
	
	gpu.tileset = malloc(0x200 * sizeof(uint8_t **)); // = 512 dec
	for(i=0; i<512; i++) {
		gpu.tileset[i] = malloc(8 * sizeof(uint8_t *));
		for(j=0; j<8; j++) {
			gpu.tileset[i][j] = calloc(8, sizeof(uint8_t));
		}
	}
	
	gpu.spritedata = malloc(0x28 * sizeof(sprite_t)); // = 40 dec
	for(i=0; i<40; i++) {
		gpu.spritedata[i].x = -8;
		gpu.spritedata[i].y = -16;
		gpu.spritedata[i].tile = 0;
		gpu.spritedata[i].flags = 0;
	}
}

void freeGPU() {
	int i, j;

	free(screenPixels);
	free(debugPixels);

	free(gpu.vram);
	free(gpu.oam);
	
	for(i=0; i<512; i++) {
		for(j=0; j<8; j++) {
			free(gpu.tileset[i][j]);
		}
		free(gpu.tileset[i]);
	}
	free(gpu.tileset);
	
	free(gpu.spritedata);
}

uint8_t gpuReadIOByte(uint16_t addr) {
	uint8_t res;
	
	switch(addr) {
		case 0:
			return gpu.r.flags;

		case 1: // LCD status
			//printf("  gpu.r.line = %d, gpu.r.lineComp = %d.\n", gpu.r.line, gpu.r.lineComp);
			return (gpu.mode & 0x03) +
				(gpu.r.line == gpu.r.lineComp ? 0x04 : 0x00) +
				(gpu.hBlankInt ? 0x08 : 0x00) +
				(gpu.vBlankInt ? 0x10 : 0x00) +
				(gpu.OAMInt ? 0x20 : 0x00) +
				(gpu.CoinInt ? 0x40 : 0x00);

		case 2:
			return gpu.r.scy;
			
		case 3:
			return gpu.r.scx;
			
		case 4:
			return gpu.r.line;

		case 5:
			return gpu.r.lineComp;
			
		default:
			//printf("TODO! read from unimplemented 0x%02X\n", addr + 0xFF40);
			return 0x00; // TODO: Unhandled I/O, no idea what GB does here
	}
}

void gpuWriteIOByte(uint8_t b, uint16_t addr) {
	uint8_t i;
	
	switch(addr) {
		case 0:
			gpu.r.flags = b;
			break;

		case 1: // LCD status, writing is only allowed on the interrupt enable flags
			gpu.hBlankInt = (b & 0x08 ? 1 : 0);
			gpu.vBlankInt = (b & 0x10 ? 1 : 0);	
			gpu.OAMInt = (b & 0x20 ? 1 : 0);	
			gpu.CoinInt = (b & 0x40 ? 1 : 0);
			printf("written 0x%02X to lcd STAT\n", b);
			break;
		
		case 2:
			gpu.r.scy = b;
			break;
			
		case 3:
			gpu.r.scx = b;
			break;

		case 4:
			//printf("written to read-only 0xFF44!\n");
			// Actually this resets LY, if I understand the specs correctly.
			gpu.r.line = 0;
			printf("reset GPU line counter.\n");
			break;

		case 5:
			gpu.r.lineComp = b;
			printf("written %d to lineComp\n", b);
			break;
			
		case 6: // DMA from XX00-XX9F to FE00-FE9F
			for(i=0; i<=0x9F; i++) {
				writeByte(readByte((b << 8) + i), 0xFE00 + i);
			}
			break;
			
		case 7: // Background palette
			for(i=0; i<4; i++) {
				gpu.bgpalette[i] = (b >> (i * 2)) & 3;
			}
			break;
			
		case 8: // Object palette 0
			for(i=0; i<4; i++) {
				gpu.objpalette[0][i] = (b >> (i * 2)) & 3;
			}
			break;
			
		case 9: // Object palette 1
			for(i=0; i<4; i++) {
				gpu.objpalette[1][i] = (b >> (i * 2)) & 3;
			}
			break;
			
		default:
			printf("TODO! write to unimplemented 0x%02X\n", addr + 0xFF40);
			break;
	}
}

inline void setPixelColor(int x, int y, uint8_t color) {
	uint8_t palettecols[4] = {255, 192, 96, 0};
	
	if(x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT || x < 0 || y < 0) {
		return;
	}
	
	// rgba
	uint32_t pixel = palettecols[color] << 16 | palettecols[color] << 8 | palettecols[color];
	screenPixels[y * SCREEN_WIDTH + x] = pixel;
}

inline void setDebugPixelColor(int x, int y, uint8_t color) {
	uint8_t palettecols[4] = {255, 192, 96, 0};
	
	if(x >= DEBUG_WIDTH || y >= DEBUG_HEIGHT || x < 0 || y < 0) {
		return;
	}
	
	// rgba
	uint32_t pixel = palettecols[color] << 16 | palettecols[color] << 8 | palettecols[color];
	debugPixels[y * DEBUG_WIDTH + x] = pixel;
}

void printGPUDebug() {
	printf("bgtoggle=%d,spritetoggle=%d,lcdtoggle=%d,bgmap=%d,tileset=%d,scx=%d,scy=%d\n",
		(gpu.r.flags & GPU_FLAG_BG) ? 1 : 0, 
		(gpu.r.flags & GPU_FLAG_SPRITES) ? 1 : 0, 
		(gpu.r.flags & GPU_FLAG_LCD) ? 1 : 0,
		(gpu.r.flags & GPU_FLAG_TILEMAP) ? 1 : 0,
		(gpu.r.flags & GPU_FLAG_TILESET) ? 1 : 0,
		gpu.r.scx, gpu.r.scy);
		
	printf("gpu.spritedata[0].x = 0x%02X.\n", gpu.spritedata[0].x);
	printf("0xC000: %02X %02X %02X %02X.\n", readByte(0xC000), readByte(0xC001), readByte(0xC002), readByte(0xC003));
	printf("0xFE00: %02X %02X %02X %02X.\n", readByte(0xFE00), readByte(0xFE01), readByte(0xFE02), readByte(0xFE03));
	/*
	int base = 0x1800;
	int i;
	
	for(i=base; i<=base+0x3FF; i++) {
		if(i%32 == 0) printf("\n");
		printf("%02X ", gpu.vram[i]);
	}
	printf("\n");
	*/
}

void renderDebugBackground() {
	int x, y, i, j, tilenr, color;

	for (y = 0; y < 24; ++y)
	{
		for (x = 0; x < 16; ++x)
		{

			for (i = 0; i < 8; ++i)
			{
				for (j = 0; j < 8; ++j)
				{
					if (i == 0 || j == 0)
					{
						setDebugPixelColor(x * 8 + j, 8 + y * 8 + i, 3);
						//setPixelColor(160 + x * 8 + j, 40 + 8 + y * 8 + i, 3);
					}

					else
					{
						//color = gpu.bgpalette[gpu.tileset[tilenr][i][j]];
						
						color = gpu.bgpalette[gpu.tileset[y*16 + x][i][j]];
						setDebugPixelColor(x * 8 + j, 8 + y * 8 + i, color);

						//color = gpu.bgpalette[gpu.tileset[y*32 + x][i][j]];
						//setPixelColor(160 + x * 8 + j, 8 + y * 8 + i, color);
					}
				}
			}
		}
	}
}

void renderScanline() {
	uint16_t yoff, xoff, tilenr;
	uint8_t row, col, i, px;
	uint8_t color;
	uint8_t bgScanline[161];
	int bit, offset;


	// Background
	if(gpu.r.flags & GPU_FLAG_BG) {
		// Use tilemap 1 or tilemap 0?
		yoff = (gpu.r.flags & GPU_FLAG_TILEMAP) ? GPU_TILEMAP_ADDR1 : GPU_TILEMAP_ADDR0;
	
		// divide y offset by 8 (to get whole tile)
		// then multiply by 32 (= amount of tiles in a row)
		yoff += (((gpu.r.line + gpu.r.scy) & 255) >> 3) << 5;
	
		// divide x scroll by 8 (to get whole tile)
		xoff = (gpu.r.scx >> 3) & 0x1F;
	
		// row number inside our tile, we only need 3 bits
		row = (gpu.r.line + gpu.r.scy) & 0x07;
	
		// same with column number
		col = gpu.r.scx & 0x07;
	
		// tile number from bgmap
		tilenr = gpu.vram[yoff + xoff];

		// TODO: tilemap signed stuff
		//printf("TEST: signed (int8_t)tilenr = %d\n", (int8_t)tilenr);
		if (!(gpu.r.flags & GPU_FLAG_TILESET) && tilenr < 128)
		{
			tilenr = tilenr + 256;
		}
		
		
	
		for(i=0; i<160; i++) {
			bgScanline[160 - col] = gpu.tileset[tilenr][row][col];
			color = gpu.bgpalette[gpu.tileset[tilenr][row][col]];
		
			/*
			bit = 1 << (7 - col);
			offset = tilenr * 16 + row * 2;
			color = gpu.bgpalette[((gpu.vram[offset] & bit) ? 0x01 : 0x00)
								| ((gpu.vram[offset+1] & bit) ? 0x02 : 0x00)];
			*/
			
			setPixelColor(i, gpu.r.line, color);
		
			col++;
			if(col == 8) {
				col = 0;
				xoff = (xoff + 1) & 0x1F;
				tilenr = gpu.vram[yoff + xoff];

				if (!(gpu.r.flags & GPU_FLAG_TILESET) && tilenr < 128)
				{
					tilenr = tilenr + 256;
				}
			}
		}
	}
	
	// Sprites
	if(gpu.r.flags & GPU_FLAG_SPRITES) {
		uint8_t spriteHeight = (gpu.r.flags & GPU_FLAG_SPRITESIZE) ? 16 : 8;

		// the upper 8x8 tile is "NN AND FEh", and the lower 8x8 tile is "NN OR 01h".

		for(i=0; i < 40; i++) {
			// do we need to draw the sprite?
			//printf("sprite x: %d, sprite y: %d, gpu line: %d\n", gpu.spritedata[i].x, gpu.spritedata[i].y, gpu.r.line);
			if(gpu.spritedata[i].y <= gpu.r.line && gpu.spritedata[i].y > gpu.r.line - spriteHeight) {
				// determine row, flip y if wanted
				if(gpu.spritedata[i].flags & SPRITE_FLAG_YFLIP) {
					row = (spriteHeight - 1) - (gpu.r.line - gpu.spritedata[i].y);
				} else {
					row = gpu.r.line - gpu.spritedata[i].y;
				}
				
				// loop through the collumns
				for(col=0; col<8; col++) {
					if(gpu.spritedata[i].flags & SPRITE_FLAG_XFLIP) {
						px = gpu.spritedata[i].x + (7 - col);
					} else {
						px = gpu.spritedata[i].x + col;
					}
					
					// only draw if this pixel's on the screen
					if(px >= 0 && px < 160) {
						uint8_t spriteTile = gpu.spritedata[i].tile;
						uint8_t spriteRow = row;

						if (spriteHeight == 16) {
							if (row < 8) {
								spriteTile = gpu.spritedata[i].tile & 0xFE;
							} else {
								spriteTile = gpu.spritedata[i].tile | 0x01;
								spriteRow = row % 8;
							}
						}



						// only draw pixel when color is not 0 and (sprite has priority or background pixel is 0)
						if(gpu.tileset[spriteTile][spriteRow][col] != 0 && (!(gpu.spritedata[i].flags & SPRITE_FLAG_PRIORITY) || bgScanline[px] == 0)) {
							color = gpu.objpalette[(gpu.spritedata[i].flags & SPRITE_FLAG_PALETTE) ? 1 : 0]
									[gpu.tileset[spriteTile][spriteRow][col]];
													  
							setPixelColor(px, gpu.r.line, color);
						}
					}
				}
			}
		}
	}
}

void updateTile(uint8_t b, uint16_t addr) {
	int tile, row, col, bit;
	
	tile = (addr >> 4) & 0x1FF;
	row = (addr >> 1) & 0x07;
	
	//printf("updateTile! tile=%d, row=%d.\n", tile, row);
	
	for(col=0; col < 8; col++) {
		bit = 1 << (7 - col);
		gpu.tileset[tile][row][col] = ((gpu.vram[addr] & bit) ? 1 : 0);
		gpu.tileset[tile][row][col] |= ((gpu.vram[addr+1] & bit) ? 2 : 0);
	}
}

void gpuBuildSpriteData(uint8_t b, uint16_t addr) {
	uint16_t spriteNum = addr >> 2;
	
	if(spriteNum < 40) { // Only 40 sprites
		switch(addr & 0x03) {
			case 0: // Y-coord
				gpu.spritedata[spriteNum].y = b - 16;
				//printf("Sprite #%d to Y of %d.\n", spriteNum, b-16);
				break;
				
			case 1: // X-coord
				gpu.spritedata[spriteNum].x = b - 8;
				//printf("Sprite #%d to X of %d.\n", spriteNum, b-8);
				break;
			
			case 2: // Tile
				gpu.spritedata[spriteNum].tile = b;
				break;
				
			case 3: // Flags
				gpu.spritedata[spriteNum].flags = b;
				break;
		}
	}
}

void renderFrame() {
	// render main window
	SDL_UpdateTexture(screenTexture, NULL, screenPixels, SCREEN_WIDTH * sizeof(uint32_t));
	SDL_RenderClear(screenRenderer);
	SDL_RenderCopy(screenRenderer, screenTexture, NULL, NULL);
	SDL_RenderPresent(screenRenderer);

	// render debug window
	SDL_UpdateTexture(debugTexture, NULL, debugPixels, DEBUG_WIDTH * sizeof(uint32_t));
	SDL_RenderClear(debugRenderer);
	SDL_RenderCopy(debugRenderer, debugTexture, NULL, NULL);
	SDL_RenderPresent(debugRenderer);
}

void stepGPU() {
	gpu.mclock += cpu.dc;
	switch(gpu.mode) {
		case GPU_MODE_HBLANK:
			if(gpu.mclock >= 51) {
				gpu.mclock = 0;
				gpu.r.line++;

				//printf("coinInt = %d, line = %d, lineComp = %d\n", gpu.CoinInt, gpu.r.line, gpu.r.lineComp);
				if(gpu.CoinInt && gpu.r.line == gpu.r.lineComp) {
						cpu.intFlags |= INT_LCDSTAT;
				}

				
				if(gpu.r.line == 144) { // last line
					gpu.mode = GPU_MODE_VBLANK;
					cpu.intFlags |= INT_VBLANK;

					if(gpu.vBlankInt) {
						cpu.intFlags |= INT_LCDSTAT;
					}

					renderFrame();
				}
				else {
					gpu.mode = GPU_MODE_OAM;

					if(gpu.OAMInt) {
						cpu.intFlags |= INT_LCDSTAT;
					}
				}
			}
			break;
			
		case GPU_MODE_VBLANK:
			if(gpu.mclock >= 114) {
				gpu.mclock = 0;
				gpu.r.line++;
				
				if(gpu.r.line > 153) {
					gpu.mode = GPU_MODE_OAM;
					gpu.r.line = 0;

					if(gpu.OAMInt) {
						cpu.intFlags |= INT_LCDSTAT;
					}
				}
			}
			break;
			
		case GPU_MODE_OAM:
			if(gpu.mclock >= 20) {
				gpu.mclock = 0;
				gpu.mode = GPU_MODE_VRAM;
			}
			break;
			
		case GPU_MODE_VRAM:
			if(gpu.mclock >= 43) {
				gpu.mclock = 0;
				gpu.mode = GPU_MODE_HBLANK;
				renderScanline();

				if(gpu.hBlankInt) {
					cpu.intFlags |= INT_LCDSTAT;
				}
			}
	}
}
