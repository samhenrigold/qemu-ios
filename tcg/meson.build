if not get_option('tcg').allowed()
   subdir_done()
endif

tcg_ss = ss.source_set()

tcg_ss.add(files(
  'optimize.c',
  'region.c',
  'tcg.c',
  'tcg-common.c',
  'tcg-op.c',
  'tcg-op-ldst.c',
  'tcg-op-gvec.c',
  'tcg-op-vec.c',
))

if get_option('tcg_interpreter')
  libffi = dependency('libffi', version: '>=3.0', required: true,
                      method: 'pkg-config')
  tcg_ss.add(libffi)
  tcg_ss.add(files('tci.c'))
endif

tcg_ss.add(when: libdw, if_true: files('debuginfo.c'))
if host_os == 'linux'
  tcg_ss.add(files('perf.c'))
endif

if get_option('tcg_threaded_interpreter')
  # TCTI dispatches through ~52 files of generated aarch64 gadgets; they are
  # far too large and repetitive to keep in the tree, so they are generated at
  # build time by the same script UTM uses.
  gadget_generator = '@0@/tcti-gadget-gen.py'.format(tcg_arch)
  tcti_sources = [
      'tcti_gadgets.h',
      'tcti_misc_gadgets.c',
      'tcti_misc_gadgets.h',
      'tcti_setcond_gadgets.c',
      'tcti_setcond_gadgets.h',
      'tcti_brcond_gadgets.c',
      'tcti_brcond_gadgets.h',
      'tcti_mov_gadgets.c',
      'tcti_mov_gadgets.h',
      'tcti_load_signed_gadgets.c',
      'tcti_load_signed_gadgets.h',
      'tcti_load_unsigned_gadgets.c',
      'tcti_load_unsigned_gadgets.h',
      'tcti_store_gadgets.c',
      'tcti_store_gadgets.h',
      'tcti_arithmetic_gadgets.c',
      'tcti_arithmetic_gadgets.h',
      'tcti_logical_gadgets.c',
      'tcti_logical_gadgets.h',
      'tcti_extension_gadgets.c',
      'tcti_extension_gadgets.h',
      'tcti_bitwise_gadgets.c',
      'tcti_bitwise_gadgets.h',
      'tcti_byteswap_gadgets.c',
      'tcti_byteswap_gadgets.h',
      'tcti_qemu_ld_aligned_signed_le_gadgets.c',
      'tcti_qemu_ld_aligned_signed_le_gadgets.h',
      'tcti_qemu_ld_unaligned_signed_le_gadgets.c',
      'tcti_qemu_ld_unaligned_signed_le_gadgets.h',
      'tcti_qemu_ld_slowpath_signed_le_gadgets.c',
      'tcti_qemu_ld_slowpath_signed_le_gadgets.h',
      'tcti_qemu_ld_aligned_unsigned_le_gadgets.c',
      'tcti_qemu_ld_aligned_unsigned_le_gadgets.h',
      'tcti_qemu_ld_unaligned_unsigned_le_gadgets.c',
      'tcti_qemu_ld_unaligned_unsigned_le_gadgets.h',
      'tcti_qemu_ld_slowpath_unsigned_le_gadgets.c',
      'tcti_qemu_ld_slowpath_unsigned_le_gadgets.h',
      'tcti_qemu_st_aligned_le_gadgets.c',
      'tcti_qemu_st_aligned_le_gadgets.h',
      'tcti_qemu_st_unaligned_le_gadgets.c',
      'tcti_qemu_st_unaligned_le_gadgets.h',
      'tcti_qemu_st_slowpath_le_gadgets.c',
      'tcti_qemu_st_slowpath_le_gadgets.h',
      'tcti_simd_base_gadgets.c',
      'tcti_simd_base_gadgets.h',
      'tcti_simd_arithmetic_gadgets.c',
      'tcti_simd_arithmetic_gadgets.h',
      'tcti_simd_logical_gadgets.c',
      'tcti_simd_logical_gadgets.h',
      'tcti_simd_immediate_gadgets.c',
      'tcti_simd_immediate_gadgets.h',
  ]
  tcti_gadgets = custom_target('tcti-gadgets.h',
                            output: tcti_sources,
                            input: gadget_generator,
                            command: [find_program(gadget_generator)],
                            build_by_default: false,
                            build_always_stale: false)
  tcti_gadgets = declare_dependency(sources: tcti_gadgets)
else
  tcti_gadgets = []
endif

tcg_ss = tcg_ss.apply({})

libtcg_user = static_library('tcg_user',
                             tcg_ss.sources() + genh,
                             dependencies: tcg_ss.dependencies() + tcti_gadgets,
                             c_args: '-DCONFIG_USER_ONLY',
                             build_by_default: false)

tcg_user = declare_dependency(objects: libtcg_user.extract_all_objects(recursive: false),
                              dependencies: tcg_ss.dependencies())
user_ss.add(tcg_user)

libtcg_system = static_library('tcg_system',
                                tcg_ss.sources() + genh,
                                dependencies: tcg_ss.dependencies() + tcti_gadgets,
                                c_args: '-DCONFIG_SOFTMMU',
                                build_by_default: false)

tcg_system = declare_dependency(objects: libtcg_system.extract_all_objects(recursive: false),
                                dependencies: tcg_ss.dependencies())
system_ss.add(tcg_system)
