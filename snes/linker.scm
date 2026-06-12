;; SNES HiROM memory description for the MicroPython port.
;; Based on Calypsi's stock linker-rules/snes-HiROM.scm with two changes:
;;  - near RAM starts at $1000, reserving $0100-$0FFF for the test mailbox
;;    (fixed addresses polled by the Mesen Lua harness, see DECISIONS.md)
;;  - explicit stack block size, and our own vector sections around `reset`
(define memories
  '((memory DirectPage
            (address (#x0 . #xff))
            (type RAM)
            (section registers ztiny tiny))
    ;; nearly all of bank-0 RAM is C stack: mp_init alone needs ~6KB
    ;; (measured; Calypsi frames save 32-bit pseudo registers)
    (memory LoRAM
            (address (#x100 . #x1fff))
            (type RAM)
            (qualifier near)
            (section stack data znear near))
    ;; far data stays in bank $7E; all of bank $7F is the MicroPython GC heap
    ;; (fixed addresses in port/main.c, no linker object — far objects must
    ;; not cross the bank boundary)
    (memory HiRAM
            (address (#x7e2000 . #x7effff))
            (type RAM)
            (qualifier far)
            (section heap zfar far huge))

    (memory LoROM
            (address (#x8000 . #xffaf))
            (type ROM)
            (qualifier near)
            (scatter-to LoROM-store)
            (section code compactcode cdata cnear switch itiny idata inear data_init_table))

    (memory HeaderExtended
            (address (#xffb0 . #xffbf))
            (type ROM)
            (qualifier near)
            (section snesheaderextended)
            (scatter-to HeaderExt-store))

    (memory HeaderBasic
            (address (#xffc0 . #xffdf))
            (type ROM)
            (qualifier near)
            (section snesheader)
            (scatter-to Header-store))

    (memory Vector
            (address (#xffe0 . #xffff))
            (type ROM)
            (qualifier near)
            (scatter-to Vector-store)
            (section (snesvec1 #xffe0) (reset #xfffc) (snesvec2 #xfffe)))

    (memory HiROM-c0
            (address (#xc00000 . #xc0ffff))
            (type ROM)
            (qualifier far)
            (section farcode cdata cnear cfar chuge switch ifar ihuge
                     (LoROM-store #xc08000)
                     (HeaderExt-store #xc0ffb0)
                     (Header-store #xc0ffc0)
                     (Vector-store #xc0ffe0)))

    (memory HiROM
            (address (#xc10000 . #xc7ffff))
            (type ROM)
            (qualifier far)
            (section farcode cdata cnear cfar chuge switch ifar ihuge))

    (block stack (size #x1d00))
    (base-address _DirectPageStart DirectPage 0)
    (base-address _NearBaseAddress LoRAM 0)
    ))
