; Correct setjmp/longjmp for the vbcc 65816 far model (SNES port).
;
; vbcc r2's libvc setjmp.o saves/restores a soft stack pointer at zero-page
; symbol `sp`, but the far model actually keeps the C stack in the hardware
; stack (S register) and never maintains `sp` (verified by disassembling a
; function prologue: locals are pushed with phy and addressed N,s). So the
; stock setjmp corrupts re-entrant calls layout-dependently and its longjmp
; wedges. This drop-in replacement (linked before -lvc, so the stock object is
; never pulled) saves the hardware stack pointer and the 3-byte far return
; address instead.
;
; ABI (read off vbcc call sites): the first (pointer) argument arrives in
; A = offset, X = bank; a normal prologue stashes it as `sta r0 / stx r0+2`,
; making r0 a far pointer occupying r0..r1 (dp 0..3). longjmp's second arg
; `val` is pushed before the call, so it sits at 4,s. An int result is returned
; in the 16-bit accumulator A. Entry is a16/x16.
;
; jsl pushes the 3-byte far return address so that at function entry
; S+1 = PCL, S+2 = PCH, S+3 = PBR.
; jmp_buf (5 bytes): [0..1] = saved S, [2] = PCL, [3] = PCH, [4] = PBR.

	section	"DONTMERGE_text.far.setjmp","acrx"
	a16
	x16

	global	_setjmp
	global	_longjmp
	zpage	r0
	zpage	r2

; int setjmp(jmp_buf buf)   ; buf: A=offset, X=bank
_setjmp:
	sta	r0		; r0..r1 = jmp_buf far pointer
	stx	r0+2
	tsc			; A = S (return address is at S+1..S+3)
	sta	[r0]		; buf[0..1] = S
	sep	#32
	a8
	lda	1,s		; PCL
	ldy	#2
	sta	[r0],y		; buf[2] = PCL
	lda	2,s		; PCH
	ldy	#3
	sta	[r0],y		; buf[3] = PCH
	lda	3,s		; PBR
	ldy	#4
	sta	[r0],y		; buf[4] = PBR
	rep	#32
	a16
	lda	#0		; setjmp returns 0
	rtl

; void longjmp(jmp_buf buf, int val)   ; buf: A=offset, X=bank; val at 4,s
_longjmp:
	sta	r0		; r0..r1 = jmp_buf far pointer
	stx	r0+2
	lda	4,s		; A = val
	bne	lj_nz
	lda	#1		; longjmp(buf, 0) must appear as setjmp returning 1
lj_nz:
	sta	r2		; stash return value (r2 = dp 4..5, clear of r0/r1)
	lda	[r0]		; A = saved S
	clc
	adc	#3
	tcs			; S = savedS + 3 (3 byte-pushes land at savedS+1..+3)
	sep	#32
	a8
	ldy	#4
	lda	[r0],y		; PBR
	pha
	ldy	#3
	lda	[r0],y		; PCH
	pha
	ldy	#2
	lda	[r0],y		; PCL
	pha
	rep	#32
	a16
	lda	r2		; A = return value
	rtl
