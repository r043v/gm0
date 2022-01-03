
#include "./gdl.h"

void createOutzone(outzone * out, u32 x, u32 y, u32 sx, u32 sy){
	out->x = x ;
	out->y = y ;
	out->start  = xy2scr( x, y ) ;
	out->end    = xy2scr( x + sx - 1, y + sy - 1 ) ;
	out->width  = sx ;
	out->height = sy ;
}

void mapSetOutZone( map * m, outzone*out ){
	memcpy( out, &m->out, sizeof(outzone) );
//	m->out = out;
	m->uncutDrawx = out->width >> m->xTileDec ;
	m->pixelMorex = out->width - (m->uncutDrawx << m->xTileDec) ;
	m->uncutDrawy = out->height >> m->yTileDec;
	m->pixelMorey = out->height - ( m->uncutDrawy << m->yTileDec ) ;
	m->maxScrollx = ( m->tileSizex * m->sizeInTilex ) - out->width  ;
	m->maxScrolly = ( m->tileSizey * m->sizeInTiley ) - out->height ;
}

void mapMoveOutZone( map * m, u32 x, u32 y ){
	m->out.x = x ;
	m->out.y = y ;
	m->out.start = xy2scr( x, y ) ;
	m->out.end   = xy2scr( x+m->out.width, y+m->out.height ) ;
}

u32 mapSet( map * m, arDeep*array, gfx*gfx,
	u32 tileNumber, u32 tileSizex, u32 tileSizey,
	u32 sizex, u32 sizey,
	u32 scrollx, u32 scrolly,
	outzone*out
)
{	u32 size = sizex*sizey ;
	if(!array || !gfx || !size) return 0 ;

	m->array = array;

	//m->tileset = tileset	;
	m->gfx = gfx;
	m->tileNumber = tileNumber ;
	m->tileSizex = tileSizex ;
	m->tileSizey = tileSizey ;
	m->scrollx = scrollx ;
	m->scrolly = scrolly ;
	m->sizeInTilex = sizex ;
	m->sizeInTiley = sizey ;
	m->sizeInTile = sizex * sizey ;
	m->drawTile = drawGfm ;

	// precompute some usefull value
	m->xTileDec = computeDec( tileSizex );
	m->yTileDec = computeDec( tileSizey );

	m->sizeInPixelx = sizex << m->xTileDec ;
	m->sizeInPixely = sizey << m->yTileDec ;
	m->firstTileBlitx = ( scrollx >> m->xTileDec ) ;
	m->firstTileBlity = ( scrolly >> m->yTileDec ) ;
	m->currentDecx = scrollx - ( m->firstTileBlitx << m->xTileDec ) ;
	m->currentDecy = scrolly - ( m->firstTileBlity << m->yTileDec ) ;

	mapSetOutZone( m, out ) ;

	m->pixelessx = m->pixelMorex + m->currentDecx ;
	m->pixelessy = m->pixelMorey + m->currentDecy ;
	m->morex = m->pixelessx >= tileSizex ;
	m->morey = m->pixelessy >= tileSizey ;

	if( m->morex ) m->pixelessx -= tileSizex ;
	if( m->morey ) m->pixelessy -= tileSizey ;

	m->tiledrawx = m->uncutDrawx + m->morex + ( m->pixelessx != 0 ) ;
	m->tiledrawy = m->uncutDrawy + m->morey + ( m->pixelessy != 0 ) ;

	return 1;
}

u32 mapSetScroll( map * m, u32 x, u32 y ){
	if( x > m->maxScrollx ) x = m->maxScrollx ;
	if( y > m->maxScrolly ) y = m->maxScrolly ;
	m->scrollx = x ;
	m->scrolly = y ;

	return mapScroll(m,0,0);
}

u32 mapScroll( map * m, u32 way, u32 pawa ){
	u32 rtn=0;

	if(way && pawa){
		u32 cpt;

		if(way&1)	// up
			for(cpt=0;cpt<pawa;cpt++)
			{	if(m->scrolly) m->scrolly--;
				 else rtn |= 1;
			};

		if(way&2)	// down
			for(cpt=0;cpt<pawa;cpt++)
			{	if(m->scrolly < m->maxScrolly) m->scrolly++;
				 else rtn |= 2;
			};

		if(way&4)	// left
			for(cpt=0;cpt<pawa;cpt++)
			{	if(m->scrollx) m->scrollx--;
				 else rtn |= 4;
			};

		if(way&8)	// right
			for(cpt=0;cpt<pawa;cpt++)
			{	if(m->scrollx < m->maxScrollx) m->scrollx++;
				 else rtn |= 8;
			};
	}

	m->firstTileBlitx = ( m->scrollx >> m->xTileDec ) ;
	m->firstTileBlity = ( m->scrolly >> m->yTileDec ) ;
	m->currentDecx = m->scrollx - ( m->firstTileBlitx << m->xTileDec ) ;
	m->currentDecy = m->scrolly - ( m->firstTileBlity << m->yTileDec ) ;

	m->pixelessx = m->pixelMorex + m->currentDecx ;
	m->pixelessy = m->pixelMorey + m->currentDecy ;
	m->morex = m->pixelessx >= m->tileSizex ;
	m->morey = m->pixelessy >= m->tileSizey ;

	if( m->morex ) m->pixelessx -= m->tileSizex ;
	if( m->morey ) m->pixelessy -= m->tileSizey ;

	m->tiledrawx = m->uncutDrawx + m->morex + ( m->pixelessx != 0 ) ;
	m->tiledrawy = m->uncutDrawy + m->morey + ( m->pixelessy != 0 ) ;

	return rtn;
}

u32 mapGetDx( map*m, int x ){ return x - ( ( x >> m->xTileDec ) << m->xTileDec ) ; }
u32 mapGetDy( map*m, int y ){ return y - ( ( y >> m->yTileDec ) << m->yTileDec ) ; }

void mapDraw( map*m ){
	internalSaveBlitLimit();
	//outzone * o = &m->out ;
//	if( o )
		setBlitLimitPointer(m->out.start,m->out.end,m->out.x,m->out.x+m->out.width);
//	else
//		fullBlitLimit();

	clDeep *screen = m->out.start;
	clDeep *scr ;

	u32 cptx,cpty;
	int blitPosx,blitPosy ;

	u32 tile;

	// left clipped colon (from up to down)
	blitPosx = ( m->out.x ) - m->currentDecx;
	blitPosy = ( m->out.y ) - m->currentDecy;

	for(cpty=0;cpty<m->tiledrawy;cpty++){ // show first colon in clipped
		tile = m->array[m->firstTileBlitx+m->sizeInTilex*(m->firstTileBlity+cpty)];
		if( tile >= m->tileNumber ) tile = 0;
		if( tile )
			m->gfx[ tile ].draw( &m->gfx[ tile ], blitPosx,blitPosy );
		blitPosy += m->tileSizey;
	};

	// right clipped colon
	blitPosx = m->out.x + m->out.width - m->pixelessx;
	blitPosy = m->out.y - m->currentDecy;

	for(cpty=0;cpty<m->tiledrawy;cpty++){
		tile = m->array[m->firstTileBlitx+m->tiledrawx-1+m->sizeInTilex*(m->firstTileBlity+cpty)];
		if( tile >= m->tileNumber ) tile = 0;
		if(tile)
			m->gfx[ tile ].draw( &m->gfx[ tile ], blitPosx,blitPosy );
		blitPosy += m->tileSizey;
	};

	// up clipped line
	blitPosx = (m->out.x) - m->currentDecx + m->tileSizex;
	blitPosy = (m->out.y) - m->currentDecy;

	for(cptx=1;cptx<m->uncutDrawx+m->morex;cptx++){ // show first colon in clipped
		tile = m->array[m->firstTileBlitx+cptx+m->sizeInTilex*m->firstTileBlity];
		if( tile >= m->tileNumber ) tile = 0;
		if(tile)
			m->gfx[ tile ].draw( &m->gfx[ tile ], blitPosx,blitPosy );
		blitPosx += m->tileSizex;
	};

	// down clipped line
	blitPosx = (m->out.x) - m->currentDecx + m->tileSizex;
	blitPosy = (m->out.y) + m->out.height - m->pixelessy;

	for(cptx=1;cptx<m->uncutDrawx+m->morex;cptx++){
		tile = m->array[m->firstTileBlitx+cptx+m->sizeInTilex*(m->firstTileBlity+m->tiledrawy - 1)];
		if( tile >= m->tileNumber ) tile = 0;
		if(tile)
			m->gfx[ tile ].draw( &m->gfx[ tile ], blitPosx,blitPosy );
		blitPosx += m->tileSizex;
	};

	// show map center, unclipped
	screen -= m->currentDecy*bufWidth;
	screen -= m->currentDecx;

	for(cpty=1;cpty<m->uncutDrawy+m->morey;cpty++){
		screen += bufWidth<<m->yTileDec;
		scr = screen;
		for(cptx=1;cptx<m->uncutDrawx+m->morex;cptx++){
			scr += m->tileSizex;
			tile = m->array[m->firstTileBlitx+cptx+m->sizeInTilex*(m->firstTileBlity+cpty)];
			if( tile >= m->tileNumber ) continue ;
			if( tile )
				m->gfx[ tile ].udraw( &m->gfx[ tile ], scr );
		};
	};

	internalLoadBlitLimit();
}

u32 mapGetTile( map * m, u32 x, u32 y, u32 method ){
	if(	x >= m->sizeInPixelx
	||	y >= m->sizeInPixely
	) return 0xffff;

	if( method == GetTile_Tile ){
		x >>= m->xTileDec ;
		y >>= m->yTileDec ;

		return m->array[y*m->sizeInTilex+x];
	}

	// GetTile_Size

	u32 xx = x >> m->xTileDec ;
	u32 yy = y >> m->yTileDec ;
	u32 tile = m->array[yy*m->sizeInTilex+xx];
//	if( !tile ) return 0;

	return tile;
/*

	u32 dy = y - ( yy << yTileDec ); // y pos in tile

	u8 *g = (u8*)( gfx[ tile ].gfx ),
	*up = g+2,
	*down = g+3
//	*sy = g+1;
	;

	return ( dy < *down || dy > *up ) ? 0 : tile;

*/

/*	u16 * up = &tilesRealSize[ tile * 2 ];//, *down=up+1
//	if( dy < *down || dy > *up ) tile = 0;
//	printf("%u [%u.%u] => %u\n",dy,*up,*down,tile);
//	return tile;

	return ( dy < up[1] || dy > *up ) ? 0 : tile;
*/
}

void mapSetTile( map*m, u32 tile, u32 x, u32 y){
	if(	tile >= m->tileNumber
	 ||	x >= m->sizeInPixelx
	 ||	y >= m->sizeInPixely
	) return;

	x >>= m->xTileDec ;
	y >>= m->yTileDec ;

	u32 p = y*m->sizeInTilex+x;
	if( p >= m->sizeInTile ) return;

	m->array[ p ] = tile ;
}

u32 mapGetOutZoneTile( map*m, u32 x, u32 y){
	return mapGetTile(m,x+m->scrollx,y+m->scrolly,GetTile_Tile);
}

void mapSetOutZoneTile( map*m, u32 tile, u32 x, u32 y ){
	if(tile >= m->tileNumber) return ;
	mapSetTile(m,tile,x+m->scrollx,y+m->scrolly);
}

u32 mapGetScreenTile(map*m,u32 x, u32 y){
	if( (x < m->out.x) || (y < m->out.y) ) return 0xffff;
	if( (x >= m->out.x + m->out.width)
	 || (y >= m->out.y + m->out.height)) return 0xffff;

	int xx = x+m->scrollx;
	int yy = y+m->scrolly;
	xx -= m->currentDecx; xx -= m->out.x;
	yy -= m->currentDecy; yy -= m->out.y;
	return mapGetTile(m,xx,yy,GetTile_Tile);
}

void mapSetScreenTile( map*m, u32 tile, u32 x, u32 y){
	if(tile >= m->tileNumber) return ;
	mapSetTile(m,tile,x+m->scrollx-m->currentDecx-m->out.x,y+m->scrolly-m->currentDecy-m->out.y);
}

void mapPlayAnim( map*m, anim **b, int x, int y, u32 way){
	playAnim( b, x - m->scrollx, y - m->scrolly, way ) ;
}

void mapDrawGfm( map*m, clDeep *Gfm, int x, int y){
	drawGfm( Gfm, x - m->scrollx, y - m->scrolly ) ;
}

void mapFollow(
	map * m,
	int x, int y, // follow pos
	u32 sx, u32 sy, // follow size
	rate * rate,
	u32 maxUp, u32 maxDown, u32 maxLeft, u32 maxRight, // screen move box
	u32 minUp, u32 minDown, u32 minLeft, u32 minRight  // min screen box
){
	int scrollx = m->scrollx;
	int scrolly = m->scrolly;

	if( x < 0 ) x = 0;
	if( y < 0 ) y = 0;
	if( x > (int)m->sizeInPixelx ) x = m->sizeInPixelx;
	if( y > (int)m->sizeInPixely ) y = m->sizeInPixely;

	u32 screenX = (u32)x - ( m->out.x + scrollx );
	u32 screenY = (u32)y - ( m->out.y + scrolly );

//	pixel[ screenX + screenY * 320 ] = 0xff00ff;

	u32 screenEndX = screenX + (sx-1);
	u32 screenEndY = screenY + (sy-1);

	minRight = m->out.width - minRight;
	maxRight = m->out.width - maxRight;

	minDown = m->out.height - minDown;
	maxDown = m->out.height - maxDown;

//	printf("min [%u.%u.%u.%u]\n",minUp,minDown,minLeft,minRight);
//	printf("max [%u.%u.%u.%u]\n",maxUp,maxDown,maxLeft,maxRight);
//	printf("%i,%i -> scroll %u,%u -> screen [%u,%u -> %u,%u]\n",x,y,scrollx,scrolly,screenX,screenY,screenEndX,screenEndY);

	// scroll screen crop X
	if( screenX < minLeft ){
//		printf("left box\n");
		scrollx -= minLeft - screenX;
	} else {
		if( screenEndX > minRight ){
			scrollx += screenEndX - minRight ;
//			printf("right box\n");
		}
	}

	// scroll screen crop Y
	if( screenY < minUp ){
		scrolly -= minUp - screenY ;
//		printf("up box\n");
	} else {
		if( screenEndY > minDown ){
			scrolly += screenEndY - minDown ;
//			printf("down box\n");
		}
	}

//printf("loop : %u\n", rate->update());

u32 px = rateUpdate( rate );

//acc += tick - last;
//last = tick;

if( px ){// acc >= speed ){
//	u32 px = 0;
//	while( acc >= speed ){ px++; acc -= speed; }

//	printf("scroll %u px\n", px);

		if( screenX < maxLeft ){
//			printf("left slow box\n");
			scrollx -= px ;
		} else {
			if( screenEndX > maxRight ){
//				printf("right slow box\n");
				scrollx += px ;
			}
		}

		if( screenY < maxUp ){
			scrolly -= px ;
//			printf("up slow box\n");
		} else {
			if( screenEndY > maxDown ){
//				printf("down slow box\n");
				scrolly += px ;
			}
		}
	}

	// scroll crop
	if( scrollx < 0 ) scrollx = 0 ;
	if( scrolly < 0 ) scrolly = 0 ;

//	printf("%u scroll %i.%i\n",tick,scrollx,scrolly);

	mapSetScroll( m, scrollx, scrolly );
//	if(this->scrollx >= this-> msx*32 - 320) this->scrollx =  msx*32 - 321 ;
//  if(this->scrolly > msy*32 - 240) this->scrolly = msy*32 - 240 ;
}


void udrawGfx( gfx * g, clDeep * s ){
	udrawGfm( (clDeep*)g->gfx, s );
}

void uplayGfx( gfx * g, clDeep * s ){
	playAnimScreen( (anim **)g->gfx, s );
}

void drawGfx( gfx * g, int x, int y ){
	drawGfm( (clDeep*)g->gfx, x, y );
}

void playGfx( gfx * g, int x, int y ){
	playAnim( (anim **)g->gfx, x, y, 0 );
}

void udrawGfxD( gfx * g, clDeep * s ){
	udrawGfm( (clDeep*)g->gfx, s + g->d );
}

void uplayGfxD( gfx * g, clDeep * s ){
	playAnimScreen( (anim **)g->gfx, s + g-> d );
}

void drawGfxD( gfx * g, int x, int y ){
	drawGfm( (clDeep*)g->gfx, x + g->dx, y + g->dy );
}

void playGfxD( gfx * g, int x, int y ){
	playAnim( (anim **)g->gfx, g->dx, g->dy, 0 );
}

void setGfxDrawFn( gfx * g, u8 animated, u8 dec ){
	if( dec ){
		if( animated ){
			g->udraw = &uplayGfxD;
			g->draw = &playGfxD;
		} else {
			g->udraw = &udrawGfxD;
			g->draw = &drawGfxD;
		}
	} else {
		if( animated ){
			g->udraw = &uplayGfx;
			g->draw = &playGfx;
		} else {
			g->udraw = &udrawGfx;
			g->draw = &drawGfx;
		}
	}
}
