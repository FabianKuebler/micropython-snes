;;; SNES cartridge header variant WITH battery SRAM (32KB) + vectors.
;;;
;;; Identical to header.s except: title, game code, cartridge type $02
;;; (ROM+RAM+battery) and RAM size $05 (32KB). Kept as a separate file so
;;; the six proven ROM images that link header.o stay byte-identical; only
;;; the workstation ROMs (mpyos, sramtest) link this one. If you touch the
;;; vectors or section layout, update header.s too — they must stay twins.
;;;
;;; HiROM battery SRAM appears in 8KB windows at banks $20-$3F:$6000-$7FFF;
;;; snes/sram_fs.c does all access via computed __far pointers (windows at
;;; $306000/$316000/$326000/$336000 for the 32KB).

              .rtmodel version, "1"
              .rtmodel core, "*"

;;; Extended header $FFB0-$FFBF (used because licensee byte is $33)
              .section snesheaderextended, root
              .ascii  "  "           ; maker code
              .ascii  "MPYW"         ; game code (W = workstation)
              .byte   0,0,0,0,0,0,0  ; reserved
              .byte   0              ; expansion RAM size
              .byte   0              ; special version
              .byte   0              ; cartridge sub-type

;;; Header $FFC0-$FFDF
              .section snesheader, root
              .ascii  "MPY WORKSTATION      "   ; 21-byte title
              .byte   0x21           ; map mode: HiROM, slow (2.68 MHz)
              .byte   0x02           ; cartridge type: ROM + RAM + battery
              .byte   0x09           ; ROM size: 512 KB
              .byte   0x05           ; RAM size: 32 KB battery SRAM
              .byte   0x01           ; country: USA (NTSC)
              .byte   0x33           ; licensee: extended header present
              .byte   0x00           ; version
              .word   0xaaaa         ; checksum complement (dummy)
              .word   0x5555         ; checksum (dummy)

;;; Native + emulation vectors. The Vector memory in linker.scm places
;;; snesvec1 at $FFE0, the C library reset at $FFFC, snesvec2 at $FFFE.
              .section snesvec1, root
              .word   0, 0                  ; $FFE0,$FFE2 unused
              .word   __vector_stub         ; $FFE4 native COP
              .word   __vector_stub         ; $FFE6 native BRK
              .word   __vector_stub         ; $FFE8 native ABORT
              .word   __vector_stub         ; $FFEA native NMI
              .word   0                     ; $FFEC unused
              .word   __vector_stub         ; $FFEE native IRQ
              .word   0, 0                  ; $FFF0,$FFF2 unused
              .word   __vector_stub         ; $FFF4 emulation COP
              .word   0                     ; $FFF6 unused
              .word   __vector_stub         ; $FFF8 emulation ABORT
              .word   __vector_stub         ; $FFFA emulation NMI

              .section snesvec2, root
              .word   __vector_stub         ; $FFFE emulation IRQ/BRK

              .section code, root
              .public __vector_stub
__vector_stub:
              rti
