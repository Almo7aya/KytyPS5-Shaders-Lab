// Independent assembler check for the binary used by cpu_tests.cpp.
// EXPECTED: 82 00 02 34 00 80 30 dc 01 00 00 02 00 00 8c bf 81 04 04 4a 00 80 70 dc 01 02 00 00 00 00 81 bf
v_lshlrev_b32_e32 v1, 2, v0
global_load_dword v2, v1, s[0:1]
s_waitcnt 0
v_add_nc_u32_e32 v2, 1, v2
global_store_dword v1, v2, s[0:1]
s_endpgm
