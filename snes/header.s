;;; SNES cartridge header and interrupt vectors.
;;;
;;; The stock Calypsi snes-HiROM linker rules expect us to provide sections
;;; `snesheaderextended` ($FFB0), `snesheader` ($FFC0) and vector words around
;;; the `reset` section that the C library startup places at $FFFC.
;;; All unused interrupts land on a stub RTI in near code (visible in the
;;; bank-0 mirror at $00:8000-$FFFF).

              .rtmodel version, "1"
              .rtmodel core, "*"

;;; Extended header $FFB0-$FFBF (used because licensee byte is $33)
              .section snesheaderextended, root
              .ascii  "  "           ; maker code
              .ascii  "MPYS"         ; game code
              .byte   0,0,0,0,0,0,0  ; reserved
              .byte   0              ; expansion RAM size
              .byte   0              ; special version
              .byte   0              ; cartridge sub-type

;;; Header $FFC0-$FFDF
              .section snesheader, root
              .ascii  "MICROPYTHON SNES     "   ; 21-byte title
              .byte   0x21           ; map mode: HiROM, slow (2.68 MHz)
              .byte   0x00           ; cartridge type: ROM only
              .byte   0x09           ; ROM size: 512 KB
              .byte   0x00           ; RAM size: none
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
