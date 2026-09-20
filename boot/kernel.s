        org     $1000
start:  lea     msg(pc),a0
        movea.l #$ff008000,a1       ; goldfish-tty REG_PUT_CHAR (32-bit only!)
.loop:  moveq   #0,d0
        move.b  (a0)+,d0
        beq.s   .fpu
        move.l  d0,(a1)
        bra.s   .loop
.fpu:   fmove.l #65,fp0             ; supervisor-mode FPU check
        fadd.l  #1,fp0              ; -> 66 = 'B'
        fmove.l fp0,d0
        andi.l  #$ff,d0
        move.l  d0,(a1)
        moveq   #10,d0
        move.l  d0,(a1)
        stop    #$2700
msg:    dc.b    "HELLO FROM 68030 FPU=",0
