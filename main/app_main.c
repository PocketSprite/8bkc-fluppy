#include <math.h>   // cos, sin
#include <stdint.h> // unit8_t, etc.

#include "8bkc-hal.h"      // kchalinit, etc. includes nvs functions
#include "powerbtn_menu.h" // Power Button menu stuff powerbtn_menu_show, constants, etc
#include "sndmixer.h"      // sndmixer functions
#include "tilegfx.h"       // tilegfx functions and types

#include "graphics.h" // Tile data

/*
 This is Fluppy Bird, an example on how to use the PocketSprite SDK.
*/

//We include a bunch of wave files in the binary using the component.mk Makefile in
//this directory, but that doesn't generate any declarations for the resulting symbols. We
//do that here manually, so we can refer to them later.
extern const uint8_t slap_wav_start[]   asm("_binary_slap_wav_start");
extern const uint8_t slap_wav_end[]     asm("_binary_slap_wav_end");
extern const uint8_t whoosh_wav_start[] asm("_binary_whoosh_wav_start");
extern const uint8_t whoosh_wav_end[]   asm("_binary_whoosh_wav_end");


//We use a separate tilemap in RAM that we manually copy data into to generate the moving pipes. This
//declares the width (height is always a screenful) and the distance between pipes.
#define PIPEMAP_W (10*4) //tiles, width of pipemap
#define PIPEMAP_PIPEOFF 12 //tiles, distance between pipes

//These defines are for the tile ID in the tilemap for the ground tiles.
#define TILE_GNDA 88 //2 tiles
#define TILE_GNDB 90
//These are for the starting rows of the numbers 0-9. These numbers have 3 rows of 8x8 tiles 
//each; we indicate these here using H, M and L. The numbers are 2 tiles wide; this is hardcoded later
//in the code.
#define NO_STARTH 400
#define NO_STARTM 420
#define NO_STARTL 440

//Menu options: first one is start, second one is exit.
#define MENU_OPT_START 0
#define MENU_OPT_EXIT 1

//These define the menu item position etc in menu.tmx.
#define MENU_OFF_LINE 10 //offset of 1st menu item
#define MENU_OFF_ITEM 3 //offset, in tiles, between menu items
#define MENU_OFF_SEL 6 //how far to move down in tiles for selected version of item
#define MENU_ITEMS 2 //We only have 'start' and 'exit'

//Bird definitions
#define BIRD_X 32		//X-pos of the bird sprite
#define FLAP_VEL 1.7	//Velocity upwards of the bird when a key is pressed
#define FLAP_FALL 0.08	//Each frame, this is subtracred from the velocity.

//The bird sprite is 3x2 tiles, but the actual image is smaller. This defines its bounding box,
//in pixels.
#define BIRD_BBOX_W 17
#define BIRD_BBOX_H 12

//Some globals (yes, dirty, but this is an example):
int xpos=0;					//X scroll position of the map. Continuously increasing (except when dead).
tilegfx_map_t *pipemap;		//Tilemap with all generated pipes in it
int score=0;				//Current players score
int hiscore=0;				//All-time high score
tilegfx_map_t *scoremap;	//Map with score and highscore. This is shown clipped when playing, full when dead.

static void do_powerbtn_menu() {
	int i=powerbtn_menu_show(tilegfx_get_fb());
	if (i==POWERBTN_MENU_EXIT) kchal_exit_to_chooser();
	if (i==POWERBTN_MENU_POWERDOWN) kchal_power_down();
}

//Clear pipes off the pipemap.
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

//This moves the background and tiles one 'tick'. If draw_pipes is true, if needed it draws new pipes to scroll
//in from the right of the screen.
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
		if (mpx<0) mpx+=PIPEMAP_W; //wraparound if needed
		//Erase old pipe, draw new pipe
		for (int x=0; x<PIPEMAP_PIPEOFF; x++) {
			for (int y=0; y<14; y++) {
				uint16_t t;
				if (x<4) {
					//Draw pipe. Copy-paste the tile from the pipe tilemap in flash
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

//Render the background to the frame buffer. Renders the trees/cities as well as the pipes/ground.
void render_bgnd() {
	tilegfx_tile_map_render(&map_bgnd_bgnd, xpos/2, 0, NULL);
	tilegfx_tile_map_render(pipemap, xpos, 0, NULL);
}

//Render one of the 'Game Over', 'Fluppy Bird' or 'Get Ready' texts to screen. Moves these text
//around a bit to make it look more dynamic.
void render_text(int text_line) {
	tilegfx_rect_t trect={.h=24, .w=160, .x=cos(xpos/7.0)*4, .y=16+sin(xpos/7.0)*4};
	tilegfx_tile_map_render(&map_menu_menugfx, 0, text_line*8*3, &trect);
}

//Get bitmap of keys, but only put an 1 for a key that wasn't pressed the previous call but
//is pressed now.
int get_keydown() {
	static int oldBtns=0xffff;
	int newBtns=kchal_get_keys();
	int ret=(oldBtns^newBtns)&newBtns;
	oldBtns=newBtns;
	return ret;
}

//Show the main menu.
int do_menu() {
	int sel=0;
	//Clear all pipes off the map
	pipemap_clear();
	while(1) {
		//Move background, but do not show pipes
		move_bgnd_tiles(0);
		render_bgnd();
		//Show logo
		render_text(0);
		//render menu
		for (int i=0; i<MENU_ITEMS; i++) {
			tilegfx_rect_t trect={.h=MENU_OFF_ITEM*8, .w=160, .x=0, .y=i*MENU_OFF_ITEM*8+72};
			int oy=MENU_OFF_LINE+MENU_OFF_ITEM*i;
			if (sel==i) oy+=MENU_OFF_SEL; //skip to selected version of menu item
			tilegfx_tile_map_render(&map_menu_menugfx, 0, oy*8, &trect);
		}
		//Flush to display.
		tilegfx_flush();
		
		//Handle key presses.
		int btn=get_keydown();
		if (btn&KC_BTN_UP) sel--;
		if (btn&KC_BTN_DOWN) sel++;
		if (btn&KC_BTN_A) return sel;
		if (btn&KC_BTN_POWER) do_powerbtn_menu();
		if (sel<0) sel=0;
		if (sel>=MENU_ITEMS) sel=MENU_ITEMS-1;
	}
}

//This function draws a (four-digit) number to a tilemap.
void map_set_val(tilegfx_map_t *map, int x, int y, int val) {
	//Clear out old digits first
	for (int i=0; i<4*2; i++) {
		tilegfx_set_tile(map, x+i, y, 0xffff);
		tilegfx_set_tile(map, x+i, y+1, 0xffff);
		tilegfx_set_tile(map, x+i, y+2, 0xffff);
	}
	//Draw needed digits
	for (int i=3; i>=0; i--) {
		if (val==0 && i!=3) break; //if rest is 0 and we already drew a digit, bail out
		int number_offset=(val%10)*2;
		//6 tiles to draw per digit. This code could've been written slightly better...
		tilegfx_set_tile(map, x+i*2, y, number_offset+NO_STARTH);
		tilegfx_set_tile(map, x+i*2, y+1, number_offset+NO_STARTM);
		tilegfx_set_tile(map, x+i*2, y+2, number_offset+NO_STARTL);
		tilegfx_set_tile(map, x+i*2+1, y, number_offset+NO_STARTH+1);
		tilegfx_set_tile(map, x+i*2+1, y+1, number_offset+NO_STARTM+1);
		tilegfx_set_tile(map, x+i*2+1, y+2, number_offset+NO_STARTL+1);
		val=val/10;
	}
}

//Update the score map to reflect the current score and high score.
void update_scoremap() {
	map_set_val(scoremap, 12, 0, score);
	map_set_val(scoremap, 12, 3, hiscore);
}

//Returns the tile number of the tile in the pipemap at the given screen coordinate.
int pipe_tile_at_pos(int x, int y) {
	//Convert the given screen coordinate to a tile coordinate in the pipe map, keeping scrolling into
	//account.
	int tile_x=((xpos+x)%(PIPEMAP_W*8))/8;
	int tile_y=y/8;
	if (tile_y>pipemap->h) return 0xffff; //off of map
	return tilegfx_get_tile(pipemap, tile_x, tile_y);
}

//Returns true if the bird touches a pipe (or the ground).
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

//Play a round of Fluppy Bard
void play_game() {
	float bird_ypos=50; //Horizontal position of the bird
	float bird_vel=0; //Horizontal velocity of the bird
	int starting=1; //1 while on the start screen, 0 when playing
	int dead=0; //False while still alive.
	int startxpos=xpos; //Note where we started for the score keeping.
	while(1) {
		//Parse input
		int btn=get_keydown();
		if (btn&KC_BTN_POWER) do_powerbtn_menu();
		if (btn & (KC_BTN_A | KC_BTN_B)) {
			starting=0;	//If we're on the start screen, go actually play now.
			if (!dead) {
				//Flap!
				bird_vel=FLAP_VEL;
				//Also play flapping sound.
				int id=sndmixer_queue_wav(whoosh_wav_start, whoosh_wav_end, 1);
				sndmixer_play(id);
			} else {
				if (bird_ypos>50*8) return; //bird is long dead, return to menu.
			}
		}

		//Game logic
		if (!starting) {
			//Move bird down
			bird_vel-=FLAP_FALL;
			bird_ypos-=bird_vel;
			if (bird_ypos<0) bird_ypos=0; //bird clips to top instead of flying off screen
			if (!dead && bird_touches_pipe(bird_ypos)) {
				//We died.
				dead=1;
				//Play the sound.
				int id=sndmixer_queue_wav(slap_wav_start, slap_wav_end, 1);
				sndmixer_play(id);
				if (score>hiscore) {
					//Hey, we set a new high score.
					hiscore=score;
					//Also save to nvs for posterity.
					nvs_set_i32(kchal_get_app_nvsh(), "hi", hiscore);
				}
			}
		}
		//Handle scoring. Essentially, this uses the scroll position since the start of the game to see how
		//many pipes we passed. (Maybe this is somewhat janky because the distance between pipes, which start at
		//specific offsets, and the gamestart position, which can be anywhere, varies? Ah well...)
		score=(((xpos-startxpos+32)/8)-(PIPEMAP_W-PIPEMAP_PIPEOFF))/PIPEMAP_PIPEOFF;
		if (score<0) score=0; //still starting, we don't want negative scores.
		update_scoremap(); //reflect score on screen.
		
		//Render everything
		if (!dead) {
			move_bgnd_tiles(!starting);
		}
		render_bgnd();
		if (starting) {
			render_text(2); //display 'get ready'
		}
		if (dead) {
			render_text(1); //display 'Game Over'
			//show full score stats
			tilegfx_rect_t srect={.h=6*8, .w=20*8, .x=0, .y=72};
			tilegfx_tile_map_render(scoremap, 0, 0, &srect);
		}
		//Show the bird. Animation of the bird is taken care of by tilegfx.
		tilegfx_rect_t trect={.h=3*8, .w=3*8, .x=BIRD_X, .y=bird_ypos};
		tilegfx_tile_map_render(&map_bird_bird, dead?24:0, 0, &trect);
		if (!dead && !starting) {
			//Show score by rendering only that part of the scoremap.
			tilegfx_rect_t srect={.h=3*8, .w=8*8, .x=12*8, .y=0};
			tilegfx_tile_map_render(scoremap, 12*8, 0, &srect);
		}
		//Send to screen.
		tilegfx_flush();
	}
}

void app_main() {
	kchal_init(); //Initialize the PocketSprite SDK.
	tilegfx_init(1, 50); //Initialize TileGFX, Doublesized mode, 50FPS
	sndmixer_init(2, 22050); //Initialize sound mixer. 2 channels at 22050Hz sample rate.
	//Create and clear the tilemap containing the dynamically shown pipes.
	pipemap=tilegfx_create_tilemap(PIPEMAP_W, 16, &tileset_fbtiles);
	pipemap_clear();
	//Create the tilemap containing scoring info. Duplicate from the map in flash to make it editable.
	scoremap=tilegfx_dup_tilemap(&map_score_score);
	//Grab highscore from previous sessions.
	nvs_get_i32(kchal_get_app_nvsh(), "hi", &hiscore);

	while(1) {
		//Show the menu.
		int r=do_menu();
		//Handle what user selected.
		if (r==MENU_OPT_START) {
			//Start a game
			play_game();
		} else { //MENU_OPT_EXIT
			//Exit.
			kchal_exit_to_chooser();
		}
	}
}
