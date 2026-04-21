 ENTRY( _start )
 SECTIONS
 {
 /* text: test code section */
 . = 0x0;
 .text :
 {
 *(.text)
 }
 . = ALIGN(4);
 __bss_start = .;
 .sbss :
 {
 *(.sbss)
 }
 .bss :
 {
 *(.bss)
 }
 . = ALIGN(4);
 __bss_end = .;
 .rodata :
 {
 *(.rodata)
 }
 .srodata :
 {
 *(.srodata)
 }
 /* data: Initialized data segment */
 .data :
 {
 *(.data)
 }
 /* End of uninitalized data segement */
 _end = .;
 }
