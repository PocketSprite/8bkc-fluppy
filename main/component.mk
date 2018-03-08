#
# Main component makefile.
#
# This Makefile can be left empty. By default, it will take the sources in the 
# src/ directory, compile them and link them into lib(subdirectory_name).a 
# in the build directory. This behaviour is entirely configurable,
# please read the ESP-IDF documents if you need to do this.
#


GFX_TILE_FILES := gfx/bgnd.tmx gfx/bird.tmx gfx/menu.tmx gfx/pipe.tmx gfx/score.tmx

$(eval $(call ConvertTiles,$(GFX_TILE_FILES),graphics))

COMPONENT_OBJS += app_main.o

COMPONENT_EMBED_FILES := sound/slap.wav sound/whoosh.wav
