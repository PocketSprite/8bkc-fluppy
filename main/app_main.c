#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "appfs.h"
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ugui.h"
#include "8bkc-hal.h"
#include "8bkc-ugui.h"
#include "tilegfx.h"
#include "appfs.h"
#include "rom/rtc.h"
#include "sndmixer.h"
#include "esp_timer.h"
#include <math.h>

#include "graphics.h"


extern const uint8_t slap_wav_start[] asm("_binary_slap_wav_start");
extern const uint8_t slap_wav_end[]   asm("_binary_slap_wav_end");
extern const uint8_t whoosh_wav_start[] asm("_binary_whoosh_wav_start");
extern const uint8_t whoosh_wav_end[]   asm("_binary_whoosh_wav_end");


#define PIPEMAP_W (10*4) //tiles
#define PIPEMAP_PIPEOFF 12 //tiles

#define TILE_GNDA 88 //2 tiles
#define TILE_GNDB 90

int xpos=0;
tilegfx_map_t *pipemap;
int score=0;
int hiscore=0;
tilegfx_map_t *scoremap;

void pipemap_clear() {
	//Clear sky part of pipemap
	for (int y=0; y<14; y++) {
		for (int x=0; x<PIPEMAP_W; x++) {
			tilegfx_set_tile(pipemap, x, y, 0xffff);
		}
	}
	//Pre-fill pipe map with ground.
	for (int x=0; x<PIPEMAP_W; x++) {
		tilegfx_set_tile(pipemap, x, 14, (TILE_GNDA+(x%2)));
		tilegfx_set_tile(pipemap, x, 15, TILE_GNDB);
	}
}

void move_bgnd_tiles(int draw_pipes) {
	xpos++;
	if ((xpos%(PIPEMAP_PIPEOFF*8)==0) && draw_pipes) {
		//xpos is exactly a multiple of the inter-pipe space - we need to generate a new pipe to scroll in.
		int first_tile_in_map=(xpos/8)%PIPEMAP_W;	//This is the Xpos of the tile that is now shown on the left
													//side of the screen.
		//Generate new pipe. 
		//Room between ceiling and floor is (16-2)=14 tiles. Need 4 tiles for room between start and end,
		//4 tiles for pipe head and tail -> 6 tiles variation.
		int npos=rand()%6; //position of the hole between pipes
		//mpx is set to the xpos of the bit of the pipe map that just scrolled out of place and will be re-used
		//eventually.
		int mpx=first_tile_in_map-PIPEMAP_PIPEOFF;
		if (mpx<0) mpx+=PIPEMAP_W;
		//Erase old pipe, draw new pipe
		for (int x=0; x<PIPEMAP_PIPEOFF; x++) {
			for (int y=0; y<14; y++) {
				uint16_t t;
				if (x<4) {
					//Draw pipe. Copy-paste the tile from the pipe tilemap.
					t=tilegfx_get_tile(&map_pipe_pipe, x, y+npos);
				} else {
					//Space between pipes; draw blank tile
					t=0xffff;
				}
				tilegfx_set_tile(pipemap, mpx, y, t);
			}
			//Increase Xpos in pipe map, wrap around if needed.
			mpx++;
			if (mpx>=PIPEMAP_W) mpx-=PIPEMAP_W;
		}
	}
}

void render_bgnd() {
	tilegfx_tile_map_render(&map_bgnd_bgnd, xpos/2, 0, NULL);
	tilegfx_tile_map_render(pipemap, xpos, 0, NULL);
}

void render_text(int text_line) {
	tilegfx_rect_t trect={.h=24, .w=160, .x=cos(xpos/7.0)*4, .y=16+sin(xpos/7.0)*4};
	tilegfx_tile_map_render(&map_menu_menugfx, 0, text_line*8*3, &trect);
}

#define MENU_OPT_START 0
#define MENU_OPT_EXIT 1

#define MENU_OFF_LINE 10 //offset of 1st menu item
#define MENU_OFF_ITEM 3 //offset, in tiles, between menu items
#define MENU_OFF_SEL 6 //how far to move down in tiles for selected version of item
#define MENU_ITEMS 2 //We only have 'start' and 'exit'

//Get bitmap of keys, but only put an 1 for a key that wasn't pressed the previous call but
//is pressed now.
int get_keydown() {
	static int oldBtns=0xffff;
	int newBtns=kchal_get_keys();
	int ret=(oldBtns^newBtns)&newBtns;
	oldBtns=newBtns;
	return ret;
}


int do_menu() {
	int sel=0;
	//Clear all pipes off the map
	pipemap_clear();
	while(1) {
		//Move background, but do not show tiles
		move_bgnd_tiles(0);
		render_bgnd();
		//Show logo
		render_text(0);
		//render menu
		for (int i=0; i<MENU_ITEMS; i++) {
			tilegfx_rect_t trect={.h=MENU_OFF_ITEM*8, .w=160, .x=0, .y=i*MENU_OFF_ITEM*8+72};
			int oy=MENU_OFF_LINE+MENU_OFF_ITEM*i;
			if (sel==i) oy+=MENU_OFF_SEL;
			tilegfx_tile_map_render(&map_menu_menugfx, 0, oy*8, &trect);
		}
		tilegfx_flush();
		
		int btn=get_keydown();
		if (btn&KC_BTN_UP) sel--;
		if (btn&KC_BTN_DOWN) sel++;
		if (btn&KC_BTN_A) return sel;
		if (sel<0) sel=0;
		if (sel>=MENU_ITEMS) sel=MENU_ITEMS-1;
	}
}

#define BIRD_X 32
#define FLAP_VEL 1.7
#define FLAP_FALL 0.08

#define BIRD_BBOX_W 17
#define BIRD_BBOX_H 12


#define NO_STARTH 400
#define NO_STARTM 420
#define NO_STARTL 440

void map_set_val(tilegfx_map_t *map, int x, int y, int val) {
	int started=0;
	//Clear out digits first
	for (int i=0; i<4*2; i++) {
		tilegfx_set_tile(map, x+i, y, 0xffff);
		tilegfx_set_tile(map, x+i, y+1, 0xffff);
		tilegfx_set_tile(map, x+i, y+2, 0xffff);
	}
	//Draw needed digits
	for (int i=3; i>=0; i--) {
		if (val==0 && i!=3) break; //if rest is 0 and we already drew a digit, bail out
		int v=(val%10)*2;
		//6 tiles to draw per digit. This code could've been written slightly better...
		tilegfx_set_tile(map, x+i*2, y, v+NO_STARTH);
		tilegfx_set_tile(map, x+i*2, y+1, v+NO_STARTM);
		tilegfx_set_tile(map, x+i*2, y+2, v+NO_STARTL);
		tilegfx_set_tile(map, x+i*2+1, y, v+NO_STARTH+1);
		tilegfx_set_tile(map, x+i*2+1, y+1, v+NO_STARTM+1);
		tilegfx_set_tile(map, x+i*2+1, y+2, v+NO_STARTL+1);
		val=val/10;
	}
}

void update_scoremap() {
	map_set_val(scoremap, 12, 0, score);
	map_set_val(scoremap, 12, 3, hiscore);
}


int pipe_tile_at_pos(int x, int y) {
	int tile_x=((xpos+x)%(PIPEMAP_W*8))/8;
	int tile_y=y/8;
	if (tile_y>pipemap->h) return 0xffff; //off of map
	return tilegfx_get_tile(pipemap, tile_x, tile_y);
}

int bird_touches_pipe(int y) {
	//See if front of bird touches pipe
	if (pipe_tile_at_pos(BIRD_X+BIRD_BBOX_W, y)!=0xffff) return 1;
	if (pipe_tile_at_pos(BIRD_X+BIRD_BBOX_W, y+BIRD_BBOX_H)!=0xffff) return 1;
	//See if back of bird touches pipe. Add fudge factor because the pipes don't visually
	//connect to back of last tile.
	if (pipe_tile_at_pos(BIRD_X+7, y)!=0xffff) return 1;
	if (pipe_tile_at_pos(BIRD_X+7, y+BIRD_BBOX_H)!=0xffff) return 1;
	//Nothing touched.
	return 0;
}


void play_game() {
	float bird_ypos=50;
	float bird_vel=0;
	int starting=1;
	int dead=0;
	int startxpos=xpos;
	while(1) {
		//Parse input
		int btn=get_keydown();
		if (btn & (KC_BTN_A | KC_BTN_B)) {
			starting=0;
			if (!dead) {
				bird_vel=FLAP_VEL;
				int id=sndmixer_queue_wav(whoosh_wav_start, whoosh_wav_end, 1);
				sndmixer_play(id);
			}
			if (bird_ypos>50*8) return; //bird is long dead
		}

		//Game logic
		if (!starting) {
			//Move bird down
			bird_vel-=FLAP_FALL;
			bird_ypos-=bird_vel;
			if (bird_ypos<0) bird_ypos=0; //bird clips to top instead of flying off screen
			if (!dead && bird_touches_pipe(bird_ypos)) {
				//We died.
				int id=sndmixer_queue_wav(slap_wav_start, slap_wav_end, 1);
				sndmixer_play(id);
				dead=1;
			}
		}
		//Handle scoring
		score=(((xpos-startxpos+32)/8)-(PIPEMAP_W-PIPEMAP_PIPEOFF))/PIPEMAP_PIPEOFF;
		if (score<0) score=0;
		if (score>hiscore) {
			hiscore=score;
			nvs_set_i32(kchal_get_app_nvsh(), "hi", hiscore); //save highscore
		}
		update_scoremap();
		
		//Render everything
		if (!dead) move_bgnd_tiles(!starting);
		render_bgnd();
		if (starting) render_text(2); //display 'get ready'
		if (dead) {
			render_text(1); //display 'Game Over'
			//show full score stats
			tilegfx_rect_t srect={.h=6*8, .w=20*8, .x=0, .y=72};
			tilegfx_tile_map_render(scoremap, 0, 0, &srect);
		}

		tilegfx_rect_t trect={.h=3*8, .w=3*8, .x=BIRD_X, .y=bird_ypos};
		tilegfx_tile_map_render(&map_bird_bird, dead?24:0, 0, &trect);
		if (!dead && !starting) {
			tilegfx_rect_t srect={.h=3*8, .w=8*8, .x=12*8, .y=0};
			tilegfx_tile_map_render(scoremap, 12*8, 0, &srect);
		}
		tilegfx_flush();
	}
}

void app_main() {
	kchal_init();
	tilegfx_init(1, 50); //Doublesized mode, 50FPS
	sndmixer_init(2, 22050);
	pipemap=tilegfx_create_tilemap(PIPEMAP_W, 16, &tileset_fbtiles);
	pipemap_clear();
	scoremap=tilegfx_dup_tilemap(&map_score_score);
	nvs_get_i32(kchal_get_app_nvsh(), "hi", &hiscore);

	while(1) {
		int r=do_menu();
		if (r==0) {
			play_game();
		} else {
			kchal_exit_to_chooser();
		}
	}
}
