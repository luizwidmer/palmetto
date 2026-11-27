; cs_w64_ds_append  synthetic RDNA2 DS_APPEND LDS fixture
	v_mov_b32_e32 v1, 0
	v_mov_b32_e32 v2, 0x10203040
	v_mov_b32_e32 v3, 0xdeadbeef
	ds_write_b32 v1, v2 offset:4
	s_waitcnt lgkmcnt(0)
	s_barrier
	v_cmp_lt_u32_e32 vcc, v0, 40
	s_and_saveexec_b64 s[8:9], vcc
	ds_append v3 offset:4
	s_or_b64 exec, exec, s[8:9]
	s_waitcnt lgkmcnt(0)
	s_barrier
	v_lshlrev_b32_e32 v30, 3, v0
	buffer_store_dword v3, v30, s[0:3], 0 offen
	v_add_nc_u32_e32 v31, 4, v30
	ds_read_b32 v4, v1 offset:4
	s_waitcnt lgkmcnt(0)
	buffer_store_dword v4, v31, s[0:3], 0 offen
	s_endpgm
