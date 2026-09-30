// Independent GFX10.3 encoding for CPU descriptor-buffer load/modify/store tests.
// EXPECTED: 00 20 30 e0 00 01 00 80 00 00 8c bf 81 02 02 4a 00 20 70 e0 00 01 00 80 00 00 81 bf
buffer_load_dword v1, v0, s[0:3], 0 idxen
s_waitcnt 0
v_add_nc_u32_e32 v1, 1, v1
buffer_store_dword v1, v0, s[0:3], 0 idxen
s_endpgm
