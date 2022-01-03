

typedef struct outzone {
	u32 height, width ; // zone size
	u32 up, down, left, right ; // position in buffer
	clDeep *start, *end ; // up / down buffer pointer
} outzone ;

typedef struct gbuffer {
  clDeep *buffer ; // pixel buffer
  u32 bufWidth, bufHeight ; // buffer real size

  // display limit into the buffer
  clDeep *start, *end ; // up / down
  int width, left, right ; // left / right
} gbuffer ;

int frmWidth, frmWidthStart, frmWidthEnd;
u32 bufWidth, bufHeight;
