# Instrument a generated copy, never the selected compiler checkout. Every edit
# must match one exact upstream statement/block; API drift fails configuration.
function(sl_make_pass_overlay input output)
  file(READ "${input}" sl_pipeline)
  string(REPLACE "\r\n" "\n" sl_pipeline "${sl_pipeline}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
  macro(sl_replace_once old new)
    string(FIND "${sl_pipeline}" "${old}" sl_position)
    if(sl_position LESS 0)
      message(FATAL_ERROR "Unsupported upstream pass pipeline: required anchor absent: ${old}")
    endif()
    string(LENGTH "${old}" sl_length)
    math(EXPR sl_after "${sl_position}+${sl_length}")
    string(SUBSTRING "${sl_pipeline}" ${sl_after} -1 sl_tail)
    string(FIND "${sl_tail}" "${old}" sl_duplicate)
    if(NOT sl_duplicate LESS 0)
      message(FATAL_ERROR "Unsupported upstream pass pipeline: ambiguous anchor: ${old}")
    endif()
    string(REPLACE "${old}" "${new}" sl_pipeline "${sl_pipeline}")
  endmacro()
  macro(sl_wrap statement index before_ir after_ir)
    sl_replace_once("${statement}"
      "sl::compiler_pass(${index}, false, ${before_ir});\n\t${statement}\n\tsl::compiler_pass(${index}, true, ${after_ir});")
  endmacro()
  sl_wrap("auto ir = Frontend::TranslateProgram(decoded, cfg, translate_options);" 1 nullptr "&ir")
  set(sl_old [=[	IR::RewriteToSsa(ir.blocks);
	IR::ConstantPropagationPass(ir.blocks);
	IR::ResolveControlFlowIdentities(ir);
	IR::RemoveIdentities(ir.blocks);
	IR::EliminateDeadCode(ir.blocks);
	const auto read_lane_stats = IR::EliminateReadLane(ir, ir.wave_size);]=])
  set(sl_new [=[	sl::compiler_pass(2, false, &ir);
	IR::RewriteToSsa(ir.blocks);
	sl::compiler_pass(2, true, &ir);
	sl::compiler_pass(3, false, &ir);
	IR::ConstantPropagationPass(ir.blocks);
	sl::compiler_pass(3, true, &ir);
	sl::compiler_pass(4, false, &ir);
	IR::ResolveControlFlowIdentities(ir);
	sl::compiler_pass(4, true, &ir);
	sl::compiler_pass(5, false, &ir);
	IR::RemoveIdentities(ir.blocks);
	sl::compiler_pass(5, true, &ir);
	sl::compiler_pass(6, false, &ir);
	IR::EliminateDeadCode(ir.blocks);
	sl::compiler_pass(6, true, &ir);
	sl::compiler_pass(7, false, &ir);
	const auto read_lane_stats = IR::EliminateReadLane(ir, ir.wave_size);
	sl::compiler_pass(7, true, &ir);
	sl::compiler_pass(8, false, &ir);]=])
  sl_replace_once("${sl_old}" "${sl_new}")
  sl_replace_once("\tLowerTessellationMemory(ir, options);"
    "\tsl::compiler_pass(8, true, &ir);\n\tsl::compiler_pass(9, false, &ir);\n\tLowerTessellationMemory(ir, options);\n\tsl::compiler_pass(9, true, &ir);")
  sl_wrap("IR::TrackResources(ir, decoded, native_cfg);" 10 "&ir" "&ir")
  sl_replace_once("\tIR::EliminateDeadCode(ir.blocks);\n\tTranslateResult result;"
    "\tsl::compiler_pass(11, false, &ir);\n\tIR::EliminateDeadCode(ir.blocks);\n\tsl::compiler_pass(11, true, &ir);\n\tTranslateResult result;")
  sl_wrap("IR::ApplyResourceSpecialization(ir, specialization);" 14 "&ir" "&ir")
  sl_replace_once("\tfor (auto& inst: ir.value_storage) {"
    "\tsl::compiler_pass(15, false, &ir);\n\tfor (auto& inst: ir.value_storage) {")
  sl_replace_once("\tir.value_storage.clear();\n\tIR::RemoveIdentities(ir.blocks);\n\tIR::EliminateDeadCode(ir.blocks);"
    "\tir.value_storage.clear();\n\tsl::compiler_pass(15, true, &ir);\n\tsl::compiler_pass(16, false, &ir);\n\tIR::RemoveIdentities(ir.blocks);\n\tsl::compiler_pass(16, true, &ir);\n\tsl::compiler_pass(17, false, &ir);\n\tIR::EliminateDeadCode(ir.blocks);\n\tsl::compiler_pass(17, true, &ir);")
  sl_wrap("IR::CollectShaderInfo(ir, options.input_info);" 18 "&ir" "&ir")
  sl_wrap("IR::AllocateBindings(ir, push_data_start_dword);" 19 "&ir" "&ir")
  sl_wrap("auto spirv = Spirv::EmitProgram(ir, options.input_info);" 20 "&ir" "&ir")
  # Keep the upstream filename in diagnostics. Inserted hooks shift line numbers;
  # the packaged generated copy is the authoritative instrumented source.
  file(TO_CMAKE_PATH "${input}" sl_input_name)
  file(WRITE "${output}" "#include \"shader_lab/compiler_trace.hpp\"\n#line 1 \"${sl_input_name}\"\n${sl_pipeline}")
endfunction()
