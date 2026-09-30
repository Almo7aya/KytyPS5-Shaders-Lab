// Independent GFX10.3 assembler check for vulkan_tests.cpp; no Kyty encoder used.
// EXPECTED: 87 02 02 7e 00 20 70 e0 00 01 00 80 00 00 81 bf
v_mov_b32_e32 v1, 7
buffer_store_dword v1, v0, s[0:3], 0 idxen
s_endpgm
