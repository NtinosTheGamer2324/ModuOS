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

	; Information request tag (type=1)
	; Ask GRUB to include specific information tags in the Multiboot2 info structure.
	; https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html
	align 8
	dw 1
	dw 0
	dd 16
	dd 8    ; framebuffer info tag
	dd 0    ; padding
	
	; framebuffer request tag (type=5)
	; NOTE: Multiboot2 header tags must be 8-byte aligned and size must be a multiple of 8.
	align 8
	dw 5
	dw 0
	dd 24
	dd 1024
	dd 768
	dd 32
	dd 0 ; padding

	; end tag
	align 8
	dw 0
	dw 0
	dd 8
header_end: