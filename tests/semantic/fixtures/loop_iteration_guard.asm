; s4 is the value the loop waits for (push-constant word 4). The counter steps by 2, so an odd value can never be
; reached and the loop cannot leave on its own; an even value is reached after value / 2 visits. Every visit stores
; the running value at the lane's own output word, so the last value stored says how far the loop ran.
v_lshlrev_b32_e32 v2, 2, v0
s_mov_b32 s9, 0
again:
s_add_u32 s9, s9, 2
v_mov_b32_e32 v1, s9
buffer_store_dword v1, v2, s[0:3], 0 offen
s_cmp_eq_u32 s9, s4
s_cbranch_scc0 again
s_endpgm
