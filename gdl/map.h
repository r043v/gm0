
#ifndef _gdlMap_
#define _gdlMap_

#include "gdl.h"

#define arDeep u8 //u16
#define arDeepDec 0

#define GetTile_Tile 0
#define GetTile_Size 1
#define GetTile_Px 2

#define scrollUp 1
#define scrollDown 2
#define scrollLeft 4
#define scrollRight 8

//#define maxMapAnims 4
#define maxTilesNb 16

typedef struct gfx {
	const void * gfx;
	int8_t dx, dy;
	int16_t d;
	void (*udraw)( struct gfx *, clDeep *);
	void (*draw)( struct gfx *, int x, int y);
} gfx ;

typedef struct outzone {
	u32 height,width;
	u32 x,y;
	clDeep *start, *end;
} outzone ;

typedef struct map {
	outzone out;
	u32 scrollx, scrolly;
	u32 tileSizex, tileSizey;
	u32 xTileDec, yTileDec;
	u32 sizeInTilex, sizeInTiley, sizeInTile;
	u32 sizeInPixelx, sizeInPixely;
	u32 firstTileBlitx, firstTileBlity;
	u32 uncutDrawx, uncutDrawy;
	u32 pixelMorex, pixelMorey;
	u32 currentDecx, currentDecy;
	u32 maxScrollx, maxScrolly;

	void (*drawTile)( clDeep*, int, int );

//	clDeep ** tileset;
	gfx * gfx;
//	struct anim ** Animate[ maxTilesNb ];
//	u16 tileDec[ maxTilesNb ];
	u16 * tilesRealSize;
	u32 tileNumber;
	arDeep * array;

	u32 pixelessx, pixelessy;
	u32 morex, morey;
	u32 tiledrawx ,tiledrawy;
} map ;

	u32  mapGetTile( map * m, u32 x, u32 y, u32 method ) ;
	void mapSetTile( map * m, u32 tile, u32 x, u32 y ) ;
	u32  mapGetOutZoneTile( map * m, u32 x, u32 y ) ;
	void mapSetOutZoneTile( map * m, u32 tile, u32 x, u32 y ) ;
	u32  mapGetScreenTile( map * m, u32 x, u32 y ) ;
	void mapSetScreenTile( map * m, u32 tile, u32 x, u32 y ) ;

	u32  mapSetScroll( map * m, u32 x, u32 y ) ;
	u32  mapScroll( map * m, u32 way, u32 pawa ) ;
	void mapSetOutZone( map * m, outzone*out ) ;
	void mapMoveOutZone( map * m, u32 x, u32 y ) ;

	void mapPlayAnim( map * m, anim **b, int x, int y, u32 way ) ;
	void mapDrawGfm( map * m, clDeep *Gfm, int x, int y ) ;

	u32  mapGetDx( map * m,  int x ) ;
	u32  mapGetDy( map * m,  int y ) ;

	void mapSetAnimatedTile( map * m, u32 tile, anim **a ) ;
	void mapDraw( map * m ) ;
	u32  mapSet( map * m, arDeep*array,gfx*tileset,u32 tileNumber,u32 tileSizex,u32 tileSizey,u32 sizex,u32 sizey,u32 scrollx,u32 scrolly, outzone*out );
	void mapFollow(
			map * m,
		 int x, int y, // follow pos
		 u32 sx, u32 sy, // follow size
		 //u32 speed, // tick for a px
		 rate * rate,
		 u32 maxUp, u32 maxDown, u32 maxLeft, u32 maxRight,
		 u32 minUp, u32 minDown, u32 minLeft, u32 minRight
	);


void createOutzone(outzone* out,u32 x, u32 y, u32 sx, u32 sy);

void udrawGfx( gfx * g, clDeep * s );
void uplayGfx( gfx * g, clDeep * s );
void drawGfx( gfx * g, int x, int y );
void playGfx( gfx * g, int x, int y );
void udrawGfxD( gfx * g, clDeep * s );
void uplayGfxD( gfx * g, clDeep * s );
void drawGfxD( gfx * g, int x, int y );
void playGfxD( gfx * g, int x, int y );
void setGfxDrawFn( gfx * g, u8 animated, u8 dec );


#endif
