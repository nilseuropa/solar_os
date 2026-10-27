/* Script for -shared -z combreloc */
/* Copyright (C) 2014-2024 Free Software Foundation, Inc.
   Copying and distribution of this script, with or without modification,
   are permitted in any medium without royalty provided the copyright
   notice and this notice are preserved.  */
ENTRY(_start)
SEARCH_DIR("=/builds/idf/crosstool-NG/builds/xtensa-esp-elf/xtensa-esp-elf/lib"); SEARCH_DIR("=/usr/local/lib"); SEARCH_DIR("=/lib"); SEARCH_DIR("=/usr/lib");
SECTIONS
{
  /* Read-only sections, merged into text segment: */
  . = 0 + SIZEOF_HEADERS;
  .note.gnu.build-id  : { *(.note.gnu.build-id) }
  .hash           : { *(.hash) }
  .gnu.hash       : { *(.gnu.hash) }
  .dynsym         : { *(.dynsym) }
  .dynstr         : { *(.dynstr) }
  .gnu.version    : { *(.gnu.version) }
  .gnu.version_d  : { *(.gnu.version_d) }
  .gnu.version_r  : { *(.gnu.version_r) }
  .rela.dyn       :
    {
      *(.rela.init)
      *(.rela.text .rela.text.* .rela.gnu.linkonce.t.*)
      *(.rela.fini)
      *(.rela.rodata .rela.rodata.* .rela.gnu.linkonce.r.*)
      *(.rela.data .rela.data.* .rela.gnu.linkonce.d.*)
      *(.rela.tdata .rela.tdata.* .rela.gnu.linkonce.td.*)
      *(.rela.tbss .rela.tbss.* .rela.gnu.linkonce.tb.*)
      *(.rela.ctors)
      *(.rela.dtors)
      *(.rela.got)
      *(.rela.bss .rela.bss.* .rela.gnu.linkonce.b.*)
    }
  .rela.plt       : { *(.rela.plt) }
  /* .plt* sections are embedded in .text */
  .text           :
  {
    *(.got.plt* .plt*)
    KEEP (*(.init.literal))
    KEEP (*(SORT_NONE(.init)))
    *(.literal .text .stub .literal.* .text.* .gnu.linkonce.literal.* .gnu.linkonce.t.*.literal .gnu.linkonce.t.*)
    *(.iram1 .iram1.* .flash.text .flash.text.*)
    /* .gnu.warning sections are handled specially by elf.em.  */
    *(.gnu.warning)
    KEEP (*(.fini.literal))
    KEEP (*(SORT_NONE(.fini)))
  } =0
  PROVIDE (__etext = .);
  PROVIDE (_etext = .);
  PROVIDE (etext = .);
  .got.loc        : { *(.got.loc) }
  .xt_except_table   : ONLY_IF_RO { KEEP (*(.xt_except_table .xt_except_table.* .gnu.linkonce.e.*)) }
  /* Adjust the address for the data segment.  We want to adjust up to
     the same address within the page on the next page up.  */
  . = ALIGN (CONSTANT (MAXPAGESIZE)) - ((CONSTANT (MAXPAGESIZE) - .) & (CONSTANT (MAXPAGESIZE) - 1)); . = DATA_SEGMENT_ALIGN (CONSTANT (MAXPAGESIZE), CONSTANT (COMMONPAGESIZE));
  /* Exception handling  */
  /* Thread Local Storage sections  */
  .tdata	  : { *(.tdata .tdata.* .gnu.linkonce.td.*) }
  .tbss		  : { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) }





  .jcr            : { KEEP (*(.jcr)) }
  .data.rel.ro : {
    *(.solaros_cpp_anchor) *(.rodata .rodata.* .rodata1 .gnu.linkonce.r.*)
    *(.data.rel.ro.local* .gnu.linkonce.d.rel.ro.local.*) *(.data.rel.ro .data.rel.ro.* .gnu.linkonce.d.rel.ro.*)
    . = ALIGN(4); __module_eh_start = .; KEEP(*(.eh_frame)) LONG(0); __module_eh_end = .;
    . = ALIGN(4); __module_lsda_start = .; *(.gcc_except_table .gcc_except_table.*) __module_lsda_end = .;
    . = ALIGN(4); __module_ctor_start = .; KEEP(*(.ctors .ctors.* .init_array .init_array.*)) __module_ctor_end = .;
    . = ALIGN(4); __module_dtor_start = .; KEEP(*(.dtors .dtors.* .fini_array .fini_array.*)) __module_dtor_end = .; LONG(0);
  }
  .xt_except_table   : ONLY_IF_RW { KEEP (*(.xt_except_table .xt_except_table.* .gnu.linkonce.e.*)) }
  .dynamic        : { *(.dynamic) }
  . = DATA_SEGMENT_RELRO_END (0, .);
  .got            : { *(.got) }
  .data           :
  {
    *(.data .data.* .gnu.linkonce.d.*)
    SORT(CONSTRUCTORS)
  }
  .data1          : { *(.data1) }
  .xt_except_desc   :
  {
    *(.xt_except_desc .xt_except_desc.* .gnu.linkonce.h.*)
    *(.xt_except_desc_end)
  }
  .lit4           :
  {
    PROVIDE (_lit4_start = .);
    *(.lit4 .lit4.* .gnu.linkonce.lit4.*)
    PROVIDE (_lit4_end = .);
  }
  _edata = .; PROVIDE (edata = .);
  . = ALIGN(ALIGNOF(NEXT_SECTION));
  __bss_start = .;
  .bss            :
  {
    *(.dynbss)
    *(.bss .bss.* .gnu.linkonce.b.*)
    *(COMMON)
    /* Align here to ensure that the .bss section occupies space up to
       _end.  Align after .bss to ensure correct alignment even if the
       .bss section disappears because there are no input sections.
       FIXME: Why do we need it? When there is no .bss section, we do not
       pad the .data section.  */
    . = ALIGN(. != 0 ? 32 / 8 : 1);
  }
  . = ALIGN(32 / 8);
  . = ALIGN(32 / 8);
  _end = .; PROVIDE (end = .);
  . = DATA_SEGMENT_END (.);
  /* Stabs debugging sections.  */
  .stab          0 : { *(.stab) }
  .stabstr       0 : { *(.stabstr) }
  .stab.excl     0 : { *(.stab.excl) }
  .stab.exclstr  0 : { *(.stab.exclstr) }
  .stab.index    0 : { *(.stab.index) }
  .stab.indexstr 0 : { *(.stab.indexstr) }
  .comment 0 (INFO) : { *(.comment); LINKER_VERSION; }
  .gnu.build.attributes : { *(.gnu.build.attributes .gnu.build.attributes.*) }
  /* DWARF debug sections.
     Symbols in the DWARF debugging sections are relative to the beginning
     of the section so we begin them at 0.  */
  /* DWARF 1.  */
  .debug          0 : { *(.debug) }
  .line           0 : { *(.line) }
  /* GNU DWARF 1 extensions.  */
  .debug_srcinfo  0 : { *(.debug_srcinfo) }
  .debug_sfnames  0 : { *(.debug_sfnames) }
  /* DWARF 1.1 and DWARF 2.  */
  .debug_aranges  0 : { *(.debug_aranges) }
  .debug_pubnames 0 : { *(.debug_pubnames) }
  /* DWARF 2.  */
  .debug_info     0 : { *(.debug_info .gnu.linkonce.wi.*) }
  .debug_abbrev   0 : { *(.debug_abbrev) }
  .debug_line     0 : { *(.debug_line .debug_line.* .debug_line_end) }
  .debug_frame    0 : { *(.debug_frame) }
  .debug_str      0 : { *(.debug_str) }
  .debug_loc      0 : { *(.debug_loc) }
  .debug_macinfo  0 : { *(.debug_macinfo) }
  /* SGI/MIPS DWARF 2 extensions.  */
  .debug_weaknames 0 : { *(.debug_weaknames) }
  .debug_funcnames 0 : { *(.debug_funcnames) }
  .debug_typenames 0 : { *(.debug_typenames) }
  .debug_varnames  0 : { *(.debug_varnames) }
  /* DWARF 3.  */
  .debug_pubtypes 0 : { *(.debug_pubtypes) }
  .debug_ranges   0 : { *(.debug_ranges) }
  /* DWARF 5.  */
  .debug_addr     0 : { *(.debug_addr) }
  .debug_line_str 0 : { *(.debug_line_str) }
  .debug_loclists 0 : { *(.debug_loclists) }
  .debug_macro    0 : { *(.debug_macro) }
  .debug_names    0 : { *(.debug_names) }
  .debug_rnglists 0 : { *(.debug_rnglists) }
  .debug_str_offsets 0 : { *(.debug_str_offsets) }
  .debug_sup      0 : { *(.debug_sup) }
  .gnu.attributes 0 : { KEEP (*(.gnu.attributes)) }
  .xt.lit       0 : { KEEP (*(.xt.lit .xt.lit.* .gnu.linkonce.p.*)) }
  .xt.insn      0 : { KEEP (*(.xt.insn .gnu.linkonce.x.*)) }
  .xt.prop      0 : { KEEP (*(.xt.prop .xt.prop.* .gnu.linkonce.prop.*)) }
  /DISCARD/ : { *(.note.GNU-stack) *(.gnu_debuglink)  *(.gnu.lto_*) }
}
