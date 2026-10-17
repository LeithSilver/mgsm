/* Leaves room after the code segment for what vita-elf-create appends there (import and export tables, ~4 KB):
   without it, a build whose code happens to end just short of a page boundary fails with "Cannot allocate N
   bytes for SCE data at end of segment 0; segment 1 overlaps" (README.md §6.29). Only the location counter
   moves (a section here would belong to the code segment and collide with those tables instead). Goes with
   -z max-page-size=0x10000 (CMakeLists.txt), which opens the same gap in the file: without it the data
   segment's file offset stays packed right after the code and the tables overlap it there ("Layout error"). */
SECTIONS
{
  . = . + 0x2000;
}
INSERT AFTER .eh_frame;
