! int setupw( unsigned segment, int *offset );
! returns the word at the far pointer  0x9000:offset

#include <linuxmt/config.h>

	.define	_setupw
	.text
	.even

_setupw:
	mov	cx,ds
	pop	dx
	pop	bx

#ifndef CONFIG_286PMODE
	mov	ax,#DEF_INITSEG
#else
	mov	ax,#0x0048
#endif

	mov	ds,ax
	sub	sp,*2
	mov	ax,[bx]
	mov	ds,cx
	jmp	dx
