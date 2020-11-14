#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include <png.h>

const char * path = "./font.gfm2.h" ;
const char * name = "font" ;

#define GFM_DEEP_1 0
#define GFM_DEEP_4 1
#define GFM_DEEP_8 2
#define GFM_DEEP_32 3
#define GFM_DEEP_16_565 4

//using namespace std;

#define u8 u_int8_t
#define u16 u_int16_t
#define u32 u_int32_t

u32 * png2img(const char* file, int*width , int*height){
  png_byte color_type;
  png_byte bit_depth;
  png_structp png_ptr;
  png_infop info_ptr;
  png_bytep * row_pointers;

  char header[8];

  /* open file and test for it being a png */
  FILE *fp = fopen(file, "rb");
  if (!fp) return 0;

  fread(header, 1, 8, fp);
  if(png_sig_cmp((const unsigned char*)header, 0, 8)) return 0;

  /* initialize stuff */
  png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

  if(!png_ptr) return 0;

  info_ptr = png_create_info_struct(png_ptr);
  if(!info_ptr) return 0;

  if(setjmp(png_jmpbuf(png_ptr))) return 0;

  png_init_io(png_ptr, fp);
  png_set_sig_bytes(png_ptr, 8);

  png_read_info(png_ptr, info_ptr);

  *width = png_get_image_width(png_ptr, info_ptr);
  *height = png_get_image_height(png_ptr, info_ptr);
  color_type = png_get_color_type(png_ptr, info_ptr);
  bit_depth = png_get_bit_depth(png_ptr, info_ptr);

  if(bit_depth == 16)
    png_set_strip_16(png_ptr);

  if(color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png_ptr);

  // PNG_COLOR_TYPE_GRAY_ALPHA is always 8 or 16bit depth.
  if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
    png_set_expand_gray_1_2_4_to_8(png_ptr);

  if(png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png_ptr);

  // These color_type don't have an alpha channel then fill it with 0xff.
  if(color_type == PNG_COLOR_TYPE_RGB ||
     color_type == PNG_COLOR_TYPE_GRAY ||
     color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);

  if(color_type == PNG_COLOR_TYPE_GRAY ||
     color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png_ptr);

  png_read_update_info(png_ptr, info_ptr);

  row_pointers = (png_bytep*) malloc(sizeof(png_bytep) * *height);
  if( !row_pointers ) return 0;

  for (int y=0; y<*height; y++){
    row_pointers[y] = (png_byte*) malloc(png_get_rowbytes(png_ptr,info_ptr));
    if( !row_pointers[y] ) return 0;
  }

  png_read_image(png_ptr, row_pointers);

  fclose(fp);

  u32 * img = (u32*)malloc( *width * *height * 4 ), *i = img;
  if( !img ) return 0;

  int x, y;

  for( y = 0 ; y < *height ; y++){
    png_byte* row = row_pointers[y];
    for( x = 0; x < *width; x++) {
       *i++ = *(u32*)&row[x * 4];
    }
    free(row);
  }

  free(row_pointers);

  return img;
}

u32 * getPart( u32 * i, u32 w, u32 h, u32 nw, u32 index ){
  u32 xoffset = nw*index;
  u32 * p = &i[ xoffset ] ;
  u32 * o = (u32*)malloc(nw*h*4), *op = o;

  for( u32 y=0; y<h; y++ ){
    memcpy( op, p, nw*4 );
    op += nw;
    p += w;
  }

  return o;
}

void getRealSize( u32*i, u32 trColor, u32*rx, u32*ry, u32*rsx, u32*rsy ){
  u32 x = *rx, y = *ry, sx = *rsx, sy = *rsy;
  u32 *p=i, xstart=sx, ystart=sy, xend=0, yend=0, nbPx=sx*sy, *pend = &p[nbPx] ;

  for( u32 yy=0; yy<sy; yy++ ){
    p = &i[ sx*yy ];
    for( u32 xx=0; xx<sx; xx++ ){
      if( *p++ != trColor ){
        if( yy < ystart )
          ystart = yy;
         else
        if( yy > yend )
          yend = yy;

        if( xx < xstart )
          xstart = xx;
         else
        if( xx > xend )
          xend = xx;
//        printf("*");
      }// else printf("-");
    };
    //printf("\n");
  }

  if( xstart > xend ){
    *rx = *ry = *rsx = *rsy = 0 ;
    return;
  }

  *rx = xstart;
  *ry = ystart;
  *rsx = ( xend - xstart ) + 1 ;
  *rsy = ( yend - ystart ) + 1 ;
}

union gfmHeader {
  u16 raw;
  struct {
    u8 raw1;
    u8 raw2;
  };
  struct {
    u16 largeSize : 1 ; // 1B or 2B for size
    u16 largeInfo : 1 ; // jump&data length, short 0 (max 15), long 1 (max 255)
    u16 deep : 3 ; // 1/4/8/32/16b color deep 0:1 1:4 2:8 3:32 4:16
    u16 palShipped : 1 ;
    u16 lineOffsetTable : 1 ; // present or not
    u16 largeOffset : 1 ; // 1B or 2B offset
  };
};

int palGetIndex( u32*pal, u32 color ){
  color &= 0xffffff;
  u32 nb = *pal++;
  for( u32 i = 0 ; i < nb ; i++ )
    if( color == pal[i] )
      return i;
  printf("\n color not found %6X",color);
  return -1;
}

u32 scanLine( u32 *i, u32 w, union gfmHeader *h, u8 *out, u32 *pal ){
  u32 *pi = i, *pend = i + w;
  u8 *o = out;
  *o = 0;
  u8 *colonNumber = o++;
  while( pi != pend ){
    u32 * start = pi ;
    while(pi != pend && *pi == 0xff00ff) pi++ ;
    u32 jump = pi - start; // jump
    start = pi;
    u32 * data = start;
    while(pi != pend && *pi != 0xff00ff) pi++ ;
    u32 length = pi - start; // data
    if( !length ) break; // end of line
    *colonNumber += 1;
    //printf(" skip %u, %u px", jump, length);

    if( h->largeInfo ){
      *o++ = jump;
      *o++ = length;
    } else *o++ = ( jump << 4 ) | length ;

    if( !h->deep ) continue ; // no color information in 1 bit

    switch( h->deep ){ // 0:1b 1:4b 2:8b 3:32b 4:16b

      // palette index
      case 1: // 4b, 16 colors, 1B for 2px
        while( length > 1 ){
          int p1 = palGetIndex(pal,*data++);
          int p2 = palGetIndex(pal,*data++);
          *o++ = ( p1 << 4 ) | p2 ;
          length -= 2;
        };

        if( length ) *o++ = palGetIndex(pal,*data) ;
      break;

      case 2: // 8b, 256 colors, 1B/px
        while( length-- )
          *o++ = palGetIndex(pal,*data++) ;
      break;

      // direct color
      case 3: // 32b, full rgb
      { u32 l = length*4;
        memcpy(o,data,l);
        o += l;
      }
      break;

      // 16b, 565
      case 4:
        while( length-- ){
          *o++ = 0;
          *o++ = 0;
        };
      break;
    }; // switch deep
  }; // line loop

  return o - out;
}

void gfm2h(const char *path, const char *name, u32 frame, u8 *data, u32 size ){
  FILE *f = fopen(path,"a") ;
  fprintf(f,"unsigned char %s%u[%u] = {",name,frame,size) ;
  u8 *end = data+(size-1), *p = data;
  while( p != end ) fprintf( f," 0x%X,", *p++);
  fprintf( f," 0x%X } ;\n", *p);
  fclose(f) ;
}

void createHeaderArray( const char *path, const char *name, u32 frameNumber ){
  FILE *f = fopen(path,"a") ;
  fprintf(f,"\nunsigned char * %s[%u] = {",name,frameNumber) ;
  u32 frm = 0;
  frameNumber -= 1;
  while( frm != frameNumber )
    fprintf(f," %s%u,",name,frm++) ;
  fprintf(f," %s%u } ;\n",name,frameNumber) ;
  fclose(f) ;
}

void createHeader( const char *path ){
  FILE *f = fopen(path,"w") ;
  fclose(f);
}

int main(int argc, char const *argv[]) {
  int width, height, nbpx;

  // load png into a 8b raw buffer
  u32 * img = png2img( argv[1], &width, &height );
  if( !img ){
    printf("cannot load picture\n");
    return 0;
  }

  u32 croppedWidth = height ; // part width
  u32 split = width / croppedWidth ; // part number
  printf("%u x %u -> %u pictures of %u x %u\n",width,height,split,croppedWidth,height);

  u32 transparentColor = 0xffff00ff;

  u32 palBuffer[ 256 ], *pal = &palBuffer[1] ;
  *palBuffer = 0 ; // color number

  // generate pal
  u32 n = 0;
  for( n=0;n<width*height; n++ ){
    u32 c = img[n]&0xffffff;
    if( c == (transparentColor&0xffffff) ) continue ;
    u32 found = 0;
    for( u32 i = 0; i<*palBuffer; i++ ){
      if( c == pal[i] ){ found = 1; break; }
    }
    if( !found ){
      pal[ *palBuffer ] = c ;
      *palBuffer += 1;
    }
  }

  // log pal
  printf( "%u colors\n", *palBuffer );
  for( n=0 ; n < *palBuffer ; n++ ){
    u32 c = pal[n];
    printf("%u 0x%6X\n",n,c);
  }

  createHeader( path );

  for( n=0 ; n < split ; n++ ){
    u8 * outBuffer = (u8*)malloc( 1024*1024 ), *buffer = outBuffer ;

    u32 * i = getPart( img, width, height, croppedWidth, n );

    u32 totalSize = 0 ;

    union gfmHeader h;
    h.deep = GFM_DEEP_4 ;
    h.lineOffsetTable = 0 ;
    h.palShipped = n == 0 ;

    memcpy( buffer, &h, sizeof( h ) ) ;
    totalSize += sizeof( h ) ; // add header size
    buffer += sizeof( h );

    // get real picture size
    u32 x=0, y=0, sx=croppedWidth, sy=height;
    getRealSize( i, 0xffff00ff, &x, &y, &sx, &sy );

    printf("\n\n -- %u %u %u, %ux%u\n",n,x,y,sx,sy);

    // save real img position & size
    *buffer++ = x ;
    *buffer++ = y ;
    *buffer++ = sx ;
    *buffer++ = sy ;

    totalSize += 4 ; // fixme, largeSize

    u8 *offset = 0 ;
//    u16 *offset16;

    if( h.lineOffsetTable ){ // line offset table shipped
      offset = buffer ; // line offset table
      totalSize += sy*(1) ; // add line offset table size
      buffer += sy*(1) ;
    }

    if( h.palShipped ){
      u32 l = *palBuffer + 1 ;
      memcpy( buffer, palBuffer, l );
      totalSize += l;
      buffer += l;
    }

    // generate cropped picture
    u32 * reali = (u32*)malloc( 4*sx*sy ), *pi = reali;
    for( u32 yy=y; yy<y+sy; yy++ ){
      for( u32 xx=x; xx<x+sx; xx++ ){
        u32 px = i[yy*croppedWidth+xx]; // height = width
        *pi++ = px == transparentColor ? 0xff00ff : px ;
        px == transparentColor ? printf("  ") : printf("**") ;
      }
      printf("\n");
    }

    free( i ) ; // release original uncropped img part

//    buffer = &outBuffer[totalSize] ; // first line buffer offset

    for( u32 line=0; line<sy; line++ ){
        if( h.lineOffsetTable )
          offset[ line ] = totalSize ;
        u32 * l = &reali[line*sx] ;
        //printf("\nline %u",line) ;
        u32 size = scanLine( l, sx, &h, buffer, palBuffer );
        totalSize += size ;
        /*
        printf("\n%u part, %uB, ",*buffer,size);
        for( u32 i=0;i<size;i++ )
          printf(" %02X",buffer[i]);
        */
        buffer += size ;
    }

    free( reali ); // release cropped img part

    /*
    if( h.lineOffsetTable ){
      printf("\n\n -- offset - ");
      for( u32 line=0; line<sy; line++ ){
        printf(" %u",offset[line]);
      }
    }
    */
    printf("\n total size %uB\n",totalSize);

    outBuffer = (u8*)realloc( outBuffer, totalSize );

    gfm2h( path, name, n, outBuffer, totalSize );


    if( n == 90 ){
      u8 *p = outBuffer;
      printf("\n -- -- --\n");
      while( totalSize-- ){
        printf("%02X",*p++);
      };
      printf("\n -- -- --\n");
    }

    free( outBuffer );
  }

  createHeaderArray(path, name, split);

  free( img );

  return 0;
}
