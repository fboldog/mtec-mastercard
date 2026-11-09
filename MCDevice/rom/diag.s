| diag.s - the DiagArea of the mastercard.device boot ROM.
|
| expansion.library copies this to RAM (da_Size bytes, from the nibble-wide
| ROM) and calls DiagPoint with a0 = board, a2 = this copy, a3 = ConfigDev,
| a6 = SysBase. DiagPoint copies the driver blob from the byte-wide ROM at
| board offset BLOBX into new memory, adds the memory's address to every long
| the relocation table names, and fills the RomTag below from the driver's own
| (now relocated) RomTag, with RTF_COLDSTART added. expansion finds that
| RomTag in the copy and exec starts the driver with the other residents.
|
| The blob, written by pack.py, is byte-wide on even board addresses, so
| movep.l reads one long of it from 8 board bytes:
|   0 memory size (code + data + bss)   4 image bytes   8 relocations
|  12 RomTag offset                    16 image          ... relocation offsets,
| one word each: the driver is far below 64 KB.

	.text
da:
	.byte	0x10			| da_Config: nibble-wide, run at config time
	.byte	0			| da_Flags
	.short	da_end-da		| da_Size
	.short	DiagPoint-da
	.short	BootPoint-da
	.short	0			| da_Name
	.short	0, 0

| Called by strap to boot from one of our partitions: start dos.library.
BootPoint:
	lea	dosname(pc),a1
	jsr	-96(a6)			| FindResident
	tst.l	d0
	beq.s	1f
	movea.l	d0,a0
	movea.l	22(a0),a0		| rt_Init
	jsr	(a0)
1:	rts

DiagPoint:
	movem.l	d2-d7/a2-a6,-(sp)
	lea	BLOBX(a0),a4
	movep.l	0(a4),d0		| memory size
	movep.l	8(a4),d6		| image bytes
	movep.l	16(a4),d5		| relocations
	movep.l	24(a4),d4		| RomTag offset
	lea	32(a4),a4
	move.l	#0x10001,d1		| MEMF_PUBLIC | MEMF_CLEAR
	jsr	-198(a6)		| AllocMem
	tst.l	d0
	beq.s	9f
	movea.l	d0,a5
	movea.l	d0,a1
	lsr.l	#2,d6
	bra.s	2f
1:	movep.l	0(a4),d1		| copy the image, a long at a time
	move.l	d1,(a1)+
	addq.l	#8,a4
2:	dbf	d6,1b
	move.l	a5,d2
	bra.s	4f
3:	moveq	#0,d1			| relocate: 16-bit offsets
	movep.w	0(a4),d1
	addq.l	#4,a4
	add.l	d2,0(a5,d1.l)
4:	dbf	d5,3b
	jsr	-636(a6)		| CacheClearU
	lea	0(a5,d4.l),a0		| the driver's RomTag
	lea	rtag(pc),a1		| ours, in this RAM copy
	move.l	a1,2(a1)		| rt_MatchTag
	move.l	a1,d1
	add.l	#26,d1
	move.l	d1,6(a1)		| rt_EndSkip
	move.l	10(a0),10(a1)		| rt_Flags, rt_Version, rt_Type, rt_Pri
	bset	#0,10(a1)		| RTF_COLDSTART
	move.l	14(a0),14(a1)		| rt_Name
	move.l	18(a0),18(a1)		| rt_IdString
	move.l	22(a0),22(a1)		| rt_Init: the driver's autoinit table
	moveq	#1,d0
9:	movem.l	(sp)+,d2-d7/a2-a6
	rts

dosname:
	.asciz	"dos.library"
	.even
rtag:
	.short	0x4AFC			| RTC_MATCHWORD
	.long	0, 0
	.byte	0, 0, 0, 0
	.long	0, 0, 0
da_end:
