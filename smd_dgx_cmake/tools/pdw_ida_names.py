"""Apply the reverse-engineered names to the Pirates of Dark Water IDB.

Run from inside IDA (File > Script file). Safe to re-run: it only sets names
and comments, never deletes code, and skips anything already named by hand.

Everything here is documented in F:\\Projects\\sega\\RE_NOTES.md.
"""
import ida_bytes, ida_name, ida_funcs, ida_ua, ida_auto, idc

CODE = {
    0x28A2: ("unpack_reverse_lz", "a0=packed src, a1=dst. Backwards bitstream LZ77."),
    0x2944: ("unpack_copy_run", None),
    0x295A: ("unpack_sized_field", "width from table at 0x2990, indexed by selector"),
    0x2960: ("unpack_bits", "n+1 bits, MSB first, walking down"),
    0x297A: ("unpack_bit", "also latches the field-width selector"),
    0x29FE: ("fade_step_colour", "one colour toward target; steps HALF units - "
                                 "the VDP ignores each channel's low bit"),
    0x2DB4: ("cram_upload", "FFD7D4 shadow -> CRAM, every frame"),
    0x2DD4: ("vram_upload", "a0=src, d0=words, d1=VDP dest"),
    0x2DF0: ("vram_upload_cmd", "d1 = full VDP address-and-command long"),
    0x4D2C4: ("cycle_menu_highlight", "16-step table at 0x4D87E -> colour 15 of "
                                      "palette lines 0-2. Not a fade."),
    0x4D6D6: ("load_portrait", "d0 = portrait index * 4. Unpacks via table 0x4D73E "
                               "to VRAM 0x8D80, draws 8x6 at nametable 0xC082, "
                               "palette line 3."),
    0x4D792: ("load_screen_gfx", "walks the VRAM descriptor list at 0x5C272"),
    0x4D84C: ("palette_clear", "both buffers, 64 entries, immediate"),
    0x4D868: ("palette_load_line", "d0 = line index, a2 = 16-colour source"),
}

DATA = {
    0x2990: ("unpack_width_table", "{3, 7, 15, 0} - field is entry+1 bits wide"),
    0x4B4CC: ("scene_table", "10 entries"),
    0x4D73E: ("portrait_table", "21 packed blocks, 1536 bytes each (8x6 tiles)"),
    0x4D87E: ("menu_highlight_cycle", "16 colours, bus format"),
    0x4D94A: ("palette_ui", "fixed palette line 2"),
    0x4D96A: ("portrait_palettes", "21 x 32 bytes, index-matched to portrait_table"),
    0x4DD4A: ("dialogue_handler_table", "12 entries"),
    0x4DDAA: ("scene_handler_table", "12 entries"),
    0x4DE0C: ("niddler_script", None),
    0x527A0: ("text_hero_select", "CHOOSE A HERO / REN / TULA / IOZ / TALK TO NIDDLER"),
    0x52804: ("text_portrait_lines", "one string per portrait, indices 15..20"),
    0x5C272: ("vram_desc_main", "{u32 src, u16 words, u16 dest}, neg-long terminated"),
    0xFF0D20: ("palette_target", "64 entries, bus format - what the scene wants"),
    0xFF0EB0: ("selected_hero", "index into portrait_table (scaled by 4)"),
    0xFFC9A8: ("sprite_table_staging", "640 bytes, DMA'd by 0x21CA"),
    0xFFCF14: ("hscroll_staging", "224 lines x 4 bytes, DMA'd by 0x2220"),
    0xFFD7D4: ("palette_shadow", "64 entries, uploaded to CRAM every frame"),
}


def apply(ea, name, comment, make_func):
    # has_user_name is the real test: auto-generated sub_/loc_/unk_ names are
    # dummy names and fair game, anything the user typed is not.
    if ida_bytes.has_user_name(ida_bytes.get_flags(ea)):
        existing = ida_name.get_name(ea)
        if existing != name:
            print(f"  {ea:06X} already named {existing!r} by hand, skipped")
            return
    if make_func and not ida_funcs.get_func(ea):
        ida_ua.create_insn(ea)
        ida_funcs.add_func(ea)
    if ida_name.set_name(ea, name, ida_name.SN_NOCHECK | ida_name.SN_FORCE):
        print(f"  {ea:06X}  {name}")
    else:
        print(f"  {ea:06X}  FAILED to name as {name}")
    if comment:
        idc.set_cmt(ea, comment, 1)


print("Naming code:")
for ea, (name, cmt) in sorted(CODE.items()):
    apply(ea, name, cmt, True)

print("Naming data:")
for ea, (name, cmt) in sorted(DATA.items()):
    apply(ea, name, cmt, False)

# The portrait table is 21 longs; typing it makes the xrefs show up.
for i in range(21):
    ea = 0x4D73E + 4 * i
    ida_bytes.del_items(ea, ida_bytes.DELIT_SIMPLE, 4)
    ida_bytes.create_data(ea, ida_bytes.FF_DWORD, 4, 0)
    idc.op_plain_offset(ea, 0, 0)

ida_auto.auto_wait()
print("done")
