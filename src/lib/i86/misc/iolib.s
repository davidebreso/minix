! Some I/O related routines like:
!	unsigned char inb(int port);
!	unsigned short inw(int port);
!	void outb(int port, int value);
!	void outw(int port, int value);
!	void rep_inb(int port, unsigned char *buf, size_t count);
!	void rep_inw(int port, unsigned short *buf, size_t count);
!	void rep_outb(int port, unsigned char *buf, size_t count);
!	void rep_outw(int port, unsigned short *buf, size_t count);
!	void intr_enable(void);
!	void intr_disable(void);

.text
.define _inb
_inb:
	push	bp
	mov	bp, sp
	mov	dx, 4(bp)		! port
	inb	dx			! read 1 byte
	xorb	ah, ah
	pop	bp
	ret

.define _inw
_inw:
	push	bp
	mov	bp, sp
	mov	dx, 4(bp)		! port
	in	dx			! read 1 word
	pop	bp
	ret

.define _outb
_outb:
	push	bp
	mov	bp, sp
	mov	dx, 4(bp)		! port
	mov	ax, 4+2(bp)		! value
	outb	dx			! output 1 byte
	pop	bp
	ret

.define _outw
_outw:
	push	bp
	mov	bp, sp
	mov	dx, 4(bp)		! port
	mov	ax, 4+2(bp)		! value
	out	dx			! output 1 word
	pop	bp
	ret

.define _rep_inb
_rep_inb:
	push	bp
	mov	bp, sp
	cld
	push	di
	mov	dx, 4(bp)		! port
	mov	di, 6(bp)		! buf
	mov	cx, 8(bp)		! byte count
	jcxz	1f
0:	inb	dx			! input 1 byte
	stosb				! write 1 byte
	loop	0b			! many times
1:	pop	di
	pop	bp
	ret

.define _rep_inw
_rep_inw:
	push	bp
	mov	bp, sp
	cld
	push	di
	mov	dx, 4(bp)		! port
	mov	di, 6(bp)		! buf
	mov	cx, 8(bp)		! byte count
	shr	cx, #1			! word count
 	jcxz	1f
0:	in	dx			! input 1 word
	stosw				! write 1 byte
	loop	0b			! many times
1:	pop	di
	pop	bp
	ret

.define _rep_outb
_rep_outb:
	push	bp
	mov	bp, sp
	cld
	push	si
	mov	dx, 4(bp)		! port
	mov	si, 6(bp)		! buf
	mov	cx, 8(bp)		! byte count
	jcxz	1f
0:	lodsb				! read 1 byte
	outb	dx			! output 1 byte
	loop	0b			! many times
1:	pop	si
	pop	bp
	ret

.define _rep_outw
_rep_outw:
	push	bp
	mov	bp, sp
	cld
	push	si
	mov	dx, 4(bp)		! port
	mov	si, 6(bp)		! buf
	mov	cx, 8(bp)		! byte count
	shr	cx, #1			! word count
	jcxz	1f
0:	lodsw				! read 1 word
	out	dx			! output 1 word
	loop	0b			! many times
1:	pop	si
	pop	bp
	ret

.define _intr_disable
_intr_disable:
	push	bp
	mov	bp, sp
	cli
	pop	bp
	ret

.define _intr_enable
_intr_enable:
	push	bp
	mov	bp, sp
	sti
	pop	bp
	ret
