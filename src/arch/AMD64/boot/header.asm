section .multiboot_header
header_start:
	; magic number
	dd 0xe85250d6 ; multiboot2
	; architecture
	dd 0 ; protected mode i386
	; header length
	dd header_end - header_start
	; checksum
	dd 0x100000000 - (0xe85250d6 + 0 + (header_end - header_start))

	; Request a Multiboot2 linear framebuffer.
	; Use a conservative "standard" mode that is known to work well on QXL.
	align 8
	; framebuffer request tag (Multiboot2 header tag type 5)
	dw 5          ; type
	dw 0          ; flags
	dd 20         ; size
	dd 1024       ; width
	dd 768        ; height
	dd 32         ; depth

	; end tag
	align 8
	dw 0
	dw 0
	dd 8
header_end: