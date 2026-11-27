; ps_smov_sgpr_gap  code 0x200..0x218 (scan), gfx1030, 1 invalid words
	s_nop 0                                     ; encoding: [0x00,0x00,0x80,0xbf]
	v_mov_b32_e32 v0, s4                        ; encoding: [0x04,0x02,0x00,0x7e]
	exp mrt0 v0, v0, v0, v0 done vm             ; encoding: [0x0f,0x18,0x00,0xf8,0x00,0x00,0x00,0x00]
	s_endpgm                                     ; encoding: [0x00,0x00,0x81,0xbf]

; --- llvm-mc messages ---
; <stdin>:1:1: warning: invalid instruction encoding
