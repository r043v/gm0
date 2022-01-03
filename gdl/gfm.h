// transparent blit routine for 16 & 32b buffer | Gdl2
// (C) 2k4/2k21 by r043v, under CC-BY-NC-SA

#ifndef _gdlGfm_
#define _gdlGfm_

#include "gdl.h"
#include "string.h"

	/* screen buffer  */
		extern clDeep *pixel ;

/* routines */

	/*** data2Gfm */
		clDeep * data2Gfm(unsigned char *data);
	/*
		will convert an agf (a 4b gfm) to a 16 or 32b gfm	*/

		clDeep ** unCrunchGfm(clDeep ** gfm, u32 frmNb);
		// uncrunch some 4b gfm

		void getGfmRealSize(clDeep *Gfm, int *up, int *down);
		clDeep** flipGfm(clDeep **Gfm, u32 nb);
		clDeep** cropGfm(clDeep **Gfm, u32 nb, u32,u32,u32,u32 );

		void drawGfmFakeHeight(clDeep *Gfm, int x, int y, u32 height, int way);

		clDeep** cropGfm(clDeep **Gfm, u32 nb, u32 up, u32 down, u32 left, u32 right);
		clDeep * cropSprite( clDeep * i, int * sx, int * sy, u32 up, u32 down, u32 left, u32 right );
		clDeep* scanImg(clDeep *img, int sx, int sy, clDeep *trClr);


	/*** drawGfm */
		void drawGfm(clDeep *Gfm, int x, int y);
	/*
		draw an entirely clipped picture at specified screen coordonate
		 x and y are position in pixel from up left of the screen,
			coordonate also indicate up left of the picture blitted
		 picture must be a blitable gfm (16b or 32b)				*/

		void idrawGfm(clDeep *Gfm, int x, int y);


	/*** udrawGfm */
		void udrawGfm(clDeep *Gfm,clDeep *scr);
	/*
		draw a frame at specified screen adress,
		 to compute adress from classic coordonate use xy2scr macro
		 coordonate is pixel position from up left of screen, it's also up left sprite position
		 take care, this routine is not clipped for width !
		 picture must be a 16b or 32b gfm
		 note : drawGfm will use udrawGfm if the picture don't require any width clipping	*/

		void iudrawGfm(clDeep *Gfm,clDeep *scr);


			// directly draw a 4b Gfm use it if you need to work on picture palette (16 color)
//		void udraw4bGfm( u8*Gfm, clDeep*mypal, clDeep*scr, u32 scrw );

	/*** Gfm2array */
		clDeep* Gfm2array(clDeep *Gfm);
	/*
		convert a gfm into an raw array, 2 args
		. first is pointer to the gfm, a clDeep*
		. second is transparent color, a clDeep, default is pink */

		void Gfm2cldArray(clDeep *Gfm, u8 * out);

	/*


		pixel/pixel colide routine
									*/
	void	logCldArray(u8 *Ar);
//	u8*		Gfm2cldArray(clDeep *Gfm);
	int		colide(u8*s1,int x1,int y1,u8*s2,int x2,int y2);

// blit zone definition routines ..

// delimate current blit zone
void   setBlitLimit(u32 x, u32 y, u32 x1, u32 x2);
void setBlitLimitPointer(clDeep *start, clDeep *end, u32 widthStart, u32 widthEnd);
void setBlitLimitX(u32 widthStart, u32 widthEnd);


// save or load a blit zone
void  saveBlitLimit(void); // save current blit limit
void  loadBlitLimit(void); // retreve saved blit limit

void internalLoadBlitLimit(void);
void internalSaveBlitLimit(void);

// maximize blit zone
void fullBlitLimit(void); // define blit limit as full screen
void setOutBuffer(clDeep*bf,int w, int h);
void cdrawGfm(clDeep *Gfm);

//void udraw4bGfm(u8*Gfm,clDeep*scr,clDeep*mypal);
void udraw4bGfm( u8*Gfm, u8*scr );

//void rdrawGfm(clDeep *Gfm, int x, int y, int angle);

/* macros */

	/*** xy2scr .. will convert screen coordonate to screen address */
	#define xy2scr(x,y)		(&pixel[(x)+(y)*bufWidth])

    #ifdef FORCE_GFM_4
		  #define getGfmSize(x,y,gfm) *(x)=*(u8*)(gfm+8);*(y)=*(u8*)(gfm+10);
    #else
		  #define getGfm4Size(x,y,gfm) *(x)=*(u8*)(gfm+8);*(y)=*(u8*)(gfm+10);
			#define getGfmSize(x,y,gfm)	 *(x)=((u8*)gfm)[0];*(y)=((u8*)gfm)[1]
    #endif

#endif
