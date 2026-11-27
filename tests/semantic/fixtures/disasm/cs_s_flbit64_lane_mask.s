; cs_s_flbit64_lane_mask  hand-written semantic fixture for unsigned leading-bit count of a wave mask
	v_mbcnt_lo_u32_b32 v1, -1, 0
	v_mbcnt_hi_u32_b32 v1, -1, v1
	v_lshlrev_b32_e32 v21, 2, v1
	v_cmp_eq_u32_e32 vcc, 63, v1
	s_mov_b64 s[4:5], vcc
	s_flbit_i32_b64 s6, s[4:5]
	v_mov_b32_e32 v20, s6
	buffer_store_dword v20, v21, s[0:3], 0 offen
	v_cmp_eq_u32_e32 vcc, 40, v1
	s_mov_b64 s[4:5], vcc
	s_flbit_i32_b64 s6, s[4:5]
	v_mov_b32_e32 v20, s6
	v_add_nc_u32_e32 v23, 256, v21
	buffer_store_dword v20, v23, s[0:3], 0 offen
	v_cmp_eq_u32_e32 vcc, 31, v1
	s_mov_b64 s[4:5], vcc
	s_flbit_i32_b64 s6, s[4:5]
	v_mov_b32_e32 v20, s6
	v_add_nc_u32_e32 v23, 512, v21
	buffer_store_dword v20, v23, s[0:3], 0 offen
	v_cmp_eq_u32_e32 vcc, 0, v1
	s_mov_b64 s[4:5], vcc
	s_flbit_i32_b64 s6, s[4:5]
	v_mov_b32_e32 v20, s6
	v_add_nc_u32_e32 v23, 768, v21
	buffer_store_dword v20, v23, s[0:3], 0 offen
	v_cmp_eq_u32_e32 vcc, 64, v1
	s_mov_b64 s[4:5], vcc
	s_flbit_i32_b64 s6, s[4:5]
	v_mov_b32_e32 v20, s6
	v_add_nc_u32_e32 v23, 1024, v21
	buffer_store_dword v20, v23, s[0:3], 0 offen
	s_mov_b64 vcc, -1
	s_mov_b64 s[4:5], vcc
	s_flbit_i32_b64 s6, s[4:5]
	v_mov_b32_e32 v20, s6
	v_add_nc_u32_e32 v23, 1280, v21
	buffer_store_dword v20, v23, s[0:3], 0 offen
	s_flbit_i32_b64 s6, exec
	v_mov_b32_e32 v20, s6
	v_add_nc_u32_e32 v23, 1536, v21
	buffer_store_dword v20, v23, s[0:3], 0 offen
	s_endpgm
