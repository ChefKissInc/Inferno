project('qemu', ['c'], meson_version: '>=1.5.0',
        default_options: ['warning_level=2', 'c_std=gnu11', 'cpp_std=gnu++11',
                          'b_staticpic=false', 'b_pie=true', 'buildtype=debugoptimized', 'b_ndebug=if-release'],
        version: files('VERSION'))

meson.add_devenv({ 'MESON_BUILD_ROOT' : meson.project_build_root() })

meson.add_postconf_script(find_program('scripts/symlink-install-tree.py'))

####################
# Global variables #
####################

not_found = dependency('', required: false)
keyval = import('keyval')
ss = import('sourceset')
fs = import('fs')

host_os = host_machine.system()

# Temporary directory used for files created while
# configure runs. Since it is in the build directory
# we can safely blow away any previous version of it
# (and we need not jump through hoops to try to delete
# it when configure exits.)
tmpdir = meson.current_build_dir() / 'meson-private/temp'

if get_option('qemu_suffix').startswith('/')
  error('qemu_suffix cannot start with a /')
endif

qemu_confdir = get_option('sysconfdir') / get_option('qemu_suffix')
qemu_datadir = get_option('datadir') / get_option('qemu_suffix')

qemu_desktopdir = get_option('datadir') / 'applications'
qemu_icondir = get_option('datadir') / 'icons'

genh = []
qapi_trace_events = []

bsd_oses = ['gnu/kfreebsd', 'freebsd', 'netbsd', 'openbsd', 'dragonfly', 'darwin']
supported_oses = ['windows', 'freebsd', 'netbsd', 'openbsd', 'darwin', 'sunos', 'linux', 'emscripten']
supported_cpus = ['ppc', 'ppc64', 's390x', 'riscv32', 'riscv64', 'x86', 'x86_64',
  'arm', 'aarch64', 'loongarch64', 'mips', 'mips64', 'sparc64', 'wasm32']

cpu = host_machine.cpu_family()

target_dirs = ['aarch64-softmmu']

# type of binaries to build
have_system = false
foreach target : target_dirs
  have_system = have_system or target.endswith('-softmmu')
endforeach

############
# Programs #
############

sh = find_program('sh')
python = import('python').find_installation()

cc = meson.get_compiler('c')
all_languages = ['c']
if host_os == 'windows' and add_languages('cpp', required: false, native: false)
  all_languages += ['cpp']
  cxx = meson.get_compiler('cpp')
endif
if host_os == 'darwin' and \
   add_languages('objc', required: true, native: false)
  all_languages += ['objc']
  objc = meson.get_compiler('objc')
endif

dtrace = not_found
stap = not_found
if 'dtrace' in get_option('trace_backends')
  dtrace = find_program('dtrace', required: true)
  stap = find_program('stap', required: false)
  if stap.found()
    # Workaround to avoid dtrace(1) producing a file with 'hidden' symbol
    # visibility. Define STAP_SDT_V2 to produce 'default' symbol visibility
    # instead. QEMU --enable-modules depends on this because the SystemTap
    # semaphores are linked into the main binary and not the module's shared
    # object.
    add_global_arguments('-DSTAP_SDT_V2',
                         native: false, language: all_languages)
  endif
endif

#####################
# Option validation #
#####################

# Tracing backends
if 'ftrace' in get_option('trace_backends') and host_os != 'linux'
  error('ftrace is supported only on Linux')
endif
if 'syslog' in get_option('trace_backends') and not cc.compiles('''
    #include <syslog.h>
    int main(void) {
        openlog("qemu", LOG_PID, LOG_DAEMON);
        syslog(LOG_INFO, "configure");
        return 0;
    }''')
  error('syslog is not supported on this system')
endif

# Miscellaneous Linux-only features
get_option('mpath') \
  .require(host_os == 'linux', error_message: 'Multipath is supported only on Linux')

multiprocess_allowed = get_option('multiprocess') \
  .require(host_os == 'linux', error_message: 'Multiprocess QEMU is supported only on Linux') \
  .allowed()

have_tcg = get_option('tcg').allowed() and have_system

have_tools = get_option('tools') \
  .disable_auto_if(not have_system) \
  .allowed()
have_block = have_system or have_tools

#######################################
# Variables for host and accelerators #
#######################################

if cpu not in supported_cpus
  host_arch = 'unknown'
elif cpu == 'x86'
  host_arch = 'i386'
elif cpu == 'mips64'
  host_arch = 'mips'
elif cpu in ['riscv32', 'riscv64']
  host_arch = 'riscv'
else
  host_arch = cpu
endif

if cpu == 'aarch64'
  kvm_targets = ['aarch64-softmmu']
else
  kvm_targets = []
endif
accelerator_targets = { 'CONFIG_KVM': kvm_targets }

if cpu == 'aarch64'
  accelerator_targets += {
    'CONFIG_HVF': ['aarch64-softmmu']
  }
endif

##################
# Compiler flags #
##################

foreach lang : all_languages
  compiler = meson.get_compiler(lang)
  if compiler.get_id() == 'gcc' and compiler.version().version_compare('>=7.4')
    # ok
  elif compiler.get_id() == 'clang' and compiler.compiles('''
      #ifdef __apple_build_version__
      # if __clang_major__ < 15 || (__clang_major__ == 15 && __clang_minor__ < 0)
      #  error You need at least XCode Clang v15.0 to compile QEMU
      # endif
      #else
      # if __clang_major__ < 10 || (__clang_major__ == 10 && __clang_minor__ < 0)
      #  error You need at least Clang v10.0 to compile QEMU
      # endif
      #endif''')
    # ok
  elif compiler.get_id() == 'emscripten'
    # ok
  else
    error('You either need GCC v7.4 or Clang v10.0 (or XCode Clang v15.0) to compile QEMU')
  endif
endforeach

# default flags for all hosts
# We use -fwrapv to tell the compiler that we require a C dialect where
# left shift of signed integers is well defined and has the expected
# 2s-complement style results. (Both clang and gcc agree that it
# provides these semantics.)

qemu_common_flags = [
  '-D_GNU_SOURCE', '-D_FILE_OFFSET_BITS=64', '-D_LARGEFILE_SOURCE',
  '-fno-strict-aliasing', '-fno-common', '-fwrapv' ]
if get_option('debug')
  qemu_common_flags += ['-fno-omit-frame-pointer']
else
  qemu_common_flags += ['-DG_DISABLE_ASSERT']
endif
qemu_cflags = []
qemu_ldflags = []

if host_os == 'darwin'
  # Disable attempts to use ObjectiveC features in os/object.h since they
  # won't work when we're compiling with gcc as a C compiler.
  if compiler.get_id() == 'gcc'
    qemu_common_flags += '-DOS_OBJECT_USE_OBJC=0'
  endif
elif host_os == 'sunos'
  # needed for CMSG_ macros in sys/socket.h
  qemu_common_flags += '-D_XOPEN_SOURCE=600'
  # needed for TIOCWIN* defines in termios.h
  qemu_common_flags += '-D__EXTENSIONS__'
elif host_os == 'haiku'
  qemu_common_flags += ['-DB_USE_POSITIVE_POSIX_ERRORS', '-D_BSD_SOURCE', '-fPIC']
endif

# Choose instruction set (currently x86-only)

qemu_isa_flags = []

# __sync_fetch_and_and requires at least -march=i486. Many toolchains
# use i686 as default anyway, but for those that don't, an explicit
# specification is necessary
if host_arch == 'i386' and not cc.links('''
  static int sfaa(int *ptr)
  {
    return __sync_fetch_and_and(ptr, 0);
  }

  int main(void)
  {
    int val = 42;
    val = __sync_val_compare_and_swap(&val, 0, 1);
    sfaa(&val);
    return val;
  }''')
  qemu_isa_flags += ['-march=i486']
endif

# Pick x86-64 baseline version
if host_arch in ['i386', 'x86_64']
  if get_option('x86_version') == '0' and host_arch == 'x86_64'
    error('x86_64-v1 required for x86-64 hosts')
  endif

  # add flags for individual instruction set extensions
  if get_option('x86_version') >= '1'
    if host_arch == 'i386'
      qemu_common_flags = ['-mfpmath=sse'] + qemu_common_flags
    else
      # present on basically all processors but technically not part of
      # x86-64-v1, so only include -mneeded for x86-64 version 2 and above
      qemu_isa_flags += ['-mcx16']
    endif
  endif
  if get_option('x86_version') >= '2'
    qemu_isa_flags += ['-mpopcnt']
    qemu_isa_flags += cc.get_supported_arguments('-mneeded')
  endif
  if get_option('x86_version') >= '3'
    qemu_isa_flags += ['-mmovbe', '-mabm', '-mbmi', '-mbmi2', '-mfma', '-mf16c']
  endif

  # add required vector instruction set (each level implies those below)
  if get_option('x86_version') == '1'
    qemu_isa_flags += ['-msse2']
  elif get_option('x86_version') == '2'
    qemu_isa_flags += ['-msse4.2']
  elif get_option('x86_version') == '3'
    qemu_isa_flags += ['-mavx2']
  elif get_option('x86_version') == '4'
    qemu_isa_flags += ['-mavx512f', '-mavx512bw', '-mavx512cd', '-mavx512dq', '-mavx512vl']
  endif
endif

qemu_common_flags = qemu_isa_flags + qemu_common_flags

if get_option('prefer_static')
  qemu_ldflags += get_option('b_pie') ? '-static-pie' : '-static'
endif

# Meson currently only handles pie as a boolean for now, so if the user
# has explicitly disabled PIE we need to extend our cflags.
#
# -no-pie is supposedly a linker flag that has no effect on the compiler
# command line, but some distros, that didn't quite know what they were
# doing, made local changes to gcc's specs file that turned it into
# a compiler command-line flag.
#
# What about linker flags?  For a static build, no PIE is implied by -static
# which we added above (and if it's not because of the same specs patching,
# there's nothing we can do: compilation will fail, report a bug to your
# distro and do not use --disable-pie in the meanwhile).  For dynamic linking,
# instead, we can't add -no-pie because it overrides -shared: the linker then
# tries to build an executable instead of a shared library and fails.  So
# don't add -no-pie anywhere and cross fingers. :(
#
# Emscripten doesn't support -no-pie but meson can't catch the compiler
# warning. So explicitly omit the flag for Emscripten.
if not get_option('b_pie') and host_os != 'emscripten'
  qemu_common_flags += cc.get_supported_arguments('-fno-pie', '-no-pie')
endif

if not get_option('stack_protector').disabled()
  stack_protector_probe = '''
    int main(int argc, char *argv[])
    {
      char arr[64], *p = arr, *c = argv[argc - 1];
      while (*c) {
          *p++ = *c++;
      }
      return 0;
    }'''
  have_stack_protector = false
  foreach arg : ['-fstack-protector-strong', '-fstack-protector-all']
    # We need to check both a compile and a link, since some compiler
    # setups fail only on a .c->.o compile and some only at link time
    if cc.compiles(stack_protector_probe, args: ['-Werror', arg]) and \
       cc.links(stack_protector_probe, args: ['-Werror', arg])
      have_stack_protector = true
      qemu_cflags += arg
      qemu_ldflags += arg
      break
    endif
  endforeach
  get_option('stack_protector') \
    .require(have_stack_protector, error_message: 'Stack protector not supported')
endif

coroutine_backend = get_option('coroutine_backend')
ucontext_probe = '''
  #include <ucontext.h>
  #ifdef __stub_makecontext
  #error Ignoring glibc stub makecontext which will always fail
  #endif
  int main(void) { makecontext(0, 0, 0); return 0; }'''

# On Windows the only valid backend is the Windows specific one.
# For POSIX prefer ucontext, but it's not always possible. The fallback
# is sigcontext.
supported_backends = []
if host_os == 'windows'
  supported_backends += ['windows']
elif host_os == 'emscripten'
  supported_backends += ['wasm']
else
  if host_os != 'darwin' and cc.links(ucontext_probe)
    supported_backends += ['ucontext']
  endif
  supported_backends += ['sigaltstack']
endif

if coroutine_backend == 'auto'
  coroutine_backend = supported_backends[0]
elif coroutine_backend not in supported_backends
  error('"@0@" backend requested but not available.  Available backends: @1@' \
        .format(coroutine_backend, ', '.join(supported_backends)))
endif

# Compiles if SafeStack *not* enabled
safe_stack_probe = '''
  int main(void)
  {
  #if defined(__has_feature)
  #if __has_feature(safe_stack)
  #error SafeStack Enabled
  #endif
  #endif
      return 0;
  }'''
if get_option('safe_stack') != not cc.compiles(safe_stack_probe)
  safe_stack_arg = get_option('safe_stack') ? '-fsanitize=safe-stack' : '-fno-sanitize=safe-stack'
  if get_option('safe_stack') != not cc.compiles(safe_stack_probe, args: safe_stack_arg)
    error(get_option('safe_stack') \
          ? 'SafeStack not supported by your compiler' \
          : 'Cannot disable SafeStack')
  endif
  qemu_cflags += safe_stack_arg
  qemu_ldflags += safe_stack_arg
endif
if get_option('safe_stack') and coroutine_backend != 'ucontext'
  error('SafeStack is only supported with the ucontext coroutine backend')
endif

if get_option('asan')
  if cc.has_argument('-fsanitize=address')
    qemu_cflags = ['-fsanitize=address'] + qemu_cflags
    qemu_ldflags = ['-fsanitize=address'] + qemu_ldflags
  else
    error('Your compiler does not support -fsanitize=address')
  endif
endif

if get_option('ubsan')
  # Detect static linking issue with ubsan:
  # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=84285
  if cc.links('int main(int argc, char **argv) { return argc + 1; }',
              args: [qemu_ldflags, '-fsanitize=undefined'])
    qemu_cflags += ['-fsanitize=undefined']
    qemu_ldflags += ['-fsanitize=undefined']

    # Suppress undefined behaviour from function call to mismatched type.
    # In addition, tcg prologue does not emit function type prefix
    # required by function call sanitizer.
    if cc.has_argument('-fno-sanitize=function')
      qemu_cflags += ['-fno-sanitize=function']
    endif
  else
    error('Your compiler does not support -fsanitize=undefined')
  endif
endif

# Thread sanitizer is, for now, much noisier than the other sanitizers;
# keep it separate until that is not the case.
if get_option('tsan')
  if get_option('asan') or get_option('ubsan')
    error('TSAN is not supported with other sanitizers')
  endif
  if not cc.has_function('__tsan_create_fiber',
                         args: '-fsanitize=thread',
                         prefix: '#include <sanitizer/tsan_interface.h>')
    error('Cannot enable TSAN due to missing fiber annotation interface')
  endif
  tsan_warn_suppress = []
  # gcc (>=11) will report constructions not supported by tsan:
  # "error: ‘atomic_thread_fence’ is not supported with ‘-fsanitize=thread’"
  # https://gcc.gnu.org/gcc-11/changes.html
  # However, clang does not support this warning and this triggers an error.
  if cc.has_argument('-Wno-tsan')
    tsan_warn_suppress = ['-Wno-tsan']
  endif
  qemu_cflags = ['-fsanitize=thread'] + tsan_warn_suppress + qemu_cflags
  qemu_ldflags = ['-fsanitize=thread'] + qemu_ldflags
endif

# Detect support for PT_GNU_RELRO + DT_BIND_NOW.
# The combination is known as "full relro", because .got.plt is read-only too.
qemu_ldflags += cc.get_supported_link_arguments('-Wl,-z,relro', '-Wl,-z,now')

if host_os == 'windows'
  qemu_ldflags += cc.get_supported_link_arguments('-Wl,--no-seh', '-Wl,--nxcompat')
  qemu_ldflags += cc.get_supported_link_arguments('-Wl,--dynamicbase', '-Wl,--high-entropy-va')
endif

if get_option('cfi')
  cfi_flags=[]
  # Check for dependency on LTO
  if not get_option('b_lto')
    error('Selected Control-Flow Integrity but LTO is disabled')
  endif
  # Check for cfi flags. CFI requires LTO so we can't use
  # get_supported_arguments, but need a more complex "compiles" which allows
  # custom arguments
  if cc.compiles('int main () { return 0; }', name: '-fsanitize=cfi-icall',
                 args: ['-flto', '-fsanitize=cfi-icall'] )
    cfi_flags += '-fsanitize=cfi-icall'
  else
    error('-fsanitize=cfi-icall is not supported by the compiler')
  endif
  if cc.compiles('int main () { return 0; }',
                 name: '-fsanitize-cfi-icall-generalize-pointers',
                 args: ['-flto', '-fsanitize=cfi-icall',
                        '-fsanitize-cfi-icall-generalize-pointers'] )
    cfi_flags += '-fsanitize-cfi-icall-generalize-pointers'
  else
    error('-fsanitize-cfi-icall-generalize-pointers is not supported by the compiler')
  endif
  if get_option('cfi_debug')
    if cc.compiles('int main () { return 0; }',
                   name: '-fno-sanitize-trap=cfi-icall',
                   args: ['-flto', '-fsanitize=cfi-icall',
                          '-fno-sanitize-trap=cfi-icall'] )
      cfi_flags += '-fno-sanitize-trap=cfi-icall'
    else
      error('-fno-sanitize-trap=cfi-icall is not supported by the compiler')
    endif
  endif
  add_global_arguments(cfi_flags, native: false, language: all_languages)
  add_global_link_arguments(cfi_flags, native: false, language: all_languages)
endif

# Check further flags that make QEMU more robust against malicious parties

hardening_flags = [
    # Initialize all stack variables to zero. This makes
    # it harder to take advantage of uninitialized stack
    # data to drive exploits
    '-ftrivial-auto-var-init=zero',
]

# Zero out registers used during a function call
# upon its return. This makes it harder to assemble
# ROP gadgets into something usable
#
# NB: Clang 17 is broken and SEGVs
# https://github.com/llvm/llvm-project/issues/75168
#
# NB2: This clashes with the "retguard" extension of OpenBSD's Clang
# https://gitlab.com/qemu-project/qemu/-/issues/2278
if host_os != 'openbsd' and \
   cc.compiles('extern struct { void (*cb)(void); } s; void f(void) { s.cb(); }',
               name: '-fzero-call-used-regs=used-gpr',
               args: ['-O2', '-fzero-call-used-regs=used-gpr'])
    hardening_flags += '-fzero-call-used-regs=used-gpr'
endif

qemu_common_flags += cc.get_supported_arguments(hardening_flags)

add_global_arguments(qemu_common_flags, native: false, language: all_languages)
add_global_link_arguments(qemu_ldflags, native: false, language: all_languages)

# Collect warning flags we want to set, sorted alphabetically
warn_flags = [
  # First enable interesting warnings
  '-Wempty-body',
  '-Wendif-labels',
  '-Wexpansion-to-defined',
  '-Wformat-security',
  '-Wformat-y2k',
  '-Wignored-qualifiers',
  '-Wimplicit-fallthrough=2',
  '-Winit-self',
  '-Wmissing-format-attribute',
  '-Wmissing-prototypes',
  '-Wnested-externs',
  '-Wold-style-declaration',
  '-Wold-style-definition',
  '-Wredundant-decls',
  '-Wshadow=local',
  '-Wstrict-prototypes',
  '-Wtype-limits',
  '-Wundef',
  '-Wno-vla',
  '-Wwrite-strings',

  # Then disable some undesirable warnings
  '-Wno-gnu-variable-sized-type-not-at-end',
  '-Wno-initializer-overrides',
  '-Wno-missing-include-dirs',
  '-Wno-psabi',
  '-Wno-shift-negative-value',
  '-Wno-string-plus-int',
  '-Wno-tautological-type-limit-compare',
  '-Wno-typedef-redefinition',
]

if host_os != 'darwin'
  tsa_has_cleanup = cc.compiles('''
    struct __attribute__((capability("mutex"))) mutex {};
    void lock(struct mutex *m) __attribute__((acquire_capability(m)));
    void unlock(struct mutex *m) __attribute__((release_capability(m)));

    void test(void) {
      struct mutex __attribute__((cleanup(unlock))) m;
      lock(&m);
    }
  ''', args: ['-Wthread-safety', '-Werror'])
  if tsa_has_cleanup
    warn_flags += ['-Wthread-safety']
  endif
endif

# Set up C++ compiler flags
qemu_cxxflags = []
if 'cpp' in all_languages
  qemu_cxxflags = ['-D__STDC_LIMIT_MACROS', '-D__STDC_CONSTANT_MACROS', '-D__STDC_FORMAT_MACROS'] + qemu_cflags
endif

add_project_arguments(qemu_cflags, native: false, language: 'c')
add_project_arguments(cc.get_supported_arguments(warn_flags), native: false, language: 'c')
if 'cpp' in all_languages
  add_project_arguments(qemu_cxxflags, native: false, language: 'cpp')
  add_project_arguments(cxx.get_supported_arguments(warn_flags), native: false, language: 'cpp')
endif
if 'objc' in all_languages
  # Note sanitizer flags are not applied to Objective-C sources!
  add_project_arguments(objc.get_supported_arguments(warn_flags), native: false, language: 'objc')
endif
linux_arch = {
  'aarch64'     : 'arm64',
  'loongarch64' : 'loongarch',
  'mips'        : 'mips',
  'mips64'      : 'mips',
  'ppc'         : 'powerpc',
  'ppc64'       : 'powerpc',
  'riscv32'     : 'riscv',
  'riscv64'     : 'riscv',
  's390x'       : 's390',
  'x86'         : 'x86',
  'x86_64'      : 'x86',
}.get(cpu, '')

if host_os == 'linux' and linux_arch != ''
  linux_headers_asm = meson.current_source_dir() / 'linux-headers' / ('asm-' + linux_arch)
  if not fs.is_dir(linux_headers_asm)
    error('linux-headers/asm-@0@ does not exist.'.format(linux_arch))
  endif
  run_command(python, files('scripts/mksymlink.py'),
              linux_headers_asm,
              meson.current_build_dir() / 'linux-headers' / 'asm',
              check: true)
endif

if host_os == 'linux'
  add_project_arguments('-isystem', meson.current_source_dir() / 'linux-headers',
                        '-isystem', 'linux-headers',
                        language: all_languages)
endif

add_project_arguments('-iquote', '.',
                      '-iquote', meson.current_source_dir(),
                      '-iquote', meson.current_source_dir() / 'include',
                      language: all_languages)

mlib_wrap = subproject('mlib')
mlib_include_dir = meson.current_build_dir() / 'mlib-include'
run_command(python, files('scripts/mksymlink.py'),
            mlib_wrap.get_variable('mlib_source_dir'),
            mlib_include_dir / 'mlib',
            check: true)
add_project_arguments('-isystem', mlib_include_dir,
                      language: all_languages)

# If a host-specific include directory exists, list that first...
host_include = meson.current_source_dir() / 'host/include/'
if fs.is_dir(host_include / host_arch)
  add_project_arguments('-iquote', host_include / host_arch,
                        language: all_languages)
endif
# ... followed by the generic fallback.
add_project_arguments('-iquote', host_include / 'generic',
                      language: all_languages)

sparse = find_program('cgcc', required: get_option('sparse'))
if sparse.found()
  run_target('sparse',
             command: [find_program('scripts/check_sparse.py'),
                       'compile_commands.json', sparse.full_path(), '-Wbitwise',
                       '-Wno-transparent-union', '-Wno-old-initializer',
                       '-Wno-non-pointer-null'])
endif

#####################################
# Host-specific libraries and flags #
#####################################

libm = cc.find_library('m', required: false)
threads = dependency('threads')
util = cc.find_library('util', required: false)
winmm = []
socket = []
version_res = []
coref = []
iokit = []
emulator_link_args = []
midl = not_found
widl = not_found
pathcch = not_found
synchronization = not_found
host_dsosuf = '.so'
if host_os == 'windows'
  midl = find_program('midl', required: false)
  widl = find_program('widl', required: false)

  # MinGW uses lowercase for library names
  pathcch = cc.find_library('pathcch', required: true)
  synchronization = cc.find_library('synchronization', required: true)
  socket = cc.find_library('ws2_32', required: true)
  winmm = cc.find_library('winmm', required: true)

  win = import('windows')
  version_res = win.compile_resources('version.rc',
                                      depend_files: files('pc-bios/qemu-nsis.ico'),
                                      include_directories: include_directories('.'))
  host_dsosuf = '.dll'
elif host_os == 'darwin'
  coref = dependency('appleframeworks', modules: 'CoreFoundation')
  iokit = dependency('appleframeworks', modules: 'IOKit', required: false)
  host_dsosuf = '.dylib'
elif host_os == 'sunos'
  socket = [cc.find_library('socket'),
            cc.find_library('nsl'),
            cc.find_library('resolv')]
elif host_os == 'haiku'
  socket = [cc.find_library('posix_error_mapper'),
            cc.find_library('network'),
            cc.find_library('bsd')]
elif host_os == 'openbsd'
  if have_tcg
    # Disable OpenBSD W^X if available
    emulator_link_args = cc.get_supported_link_arguments('-Wl,-z,wxneeded')
  endif
endif

###############################################
# Host-specific configuration of accelerators #
###############################################

accelerators = []
if get_option('kvm').allowed() and host_os == 'linux'
  accelerators += 'CONFIG_KVM'
endif
if get_option('whpx').allowed() and host_os == 'windows'
  if get_option('whpx').enabled() and host_machine.cpu() != 'x86_64'
    error('WHPX requires 64-bit host')
  elif cc.has_header('winhvplatform.h', required: get_option('whpx')) and \
       cc.has_header('winhvemulation.h', required: get_option('whpx'))
    accelerators += 'CONFIG_WHPX'
  endif
endif

hvf = not_found
if get_option('hvf').allowed()
  hvf = dependency('appleframeworks', modules: 'Hypervisor',
                   required: get_option('hvf'))
  if hvf.found()
    accelerators += 'CONFIG_HVF'
  endif
endif

tcg_arch = host_arch
if have_tcg
  if host_arch == 'unknown'
    if not get_option('tcg_interpreter')
      error('Unsupported CPU @0@, try --enable-tcg-interpreter'.format(cpu))
    endif
  elif host_arch == 'wasm32'
    if not get_option('tcg_interpreter')
      error('WebAssembly host requires --enable-tcg-interpreter')
    endif
  elif get_option('tcg_interpreter')
    warning('Use of the TCG interpreter is not recommended on this host')
    warning('architecture. There is a native TCG execution backend available')
    warning('which provides substantially better performance and reliability.')
    warning('It is strongly recommended to remove the --enable-tcg-interpreter')
    warning('configuration option on this architecture to use the native')
    warning('backend.')
  endif
  if get_option('tcg_interpreter')
    tcg_arch = 'tci'
  elif host_arch == 'x86_64'
    tcg_arch = 'i386'
  elif host_arch == 'ppc64'
    tcg_arch = 'ppc'
  endif
  add_project_arguments('-iquote', meson.current_source_dir() / 'tcg' / tcg_arch,
                        language: all_languages)

  accelerators += 'CONFIG_TCG'
endif

if 'CONFIG_KVM' not in accelerators and get_option('kvm').enabled()
  error('KVM not available on this platform')
endif
if 'CONFIG_HVF' not in accelerators and get_option('hvf').enabled()
  error('HVF not available on this platform')
endif
if 'CONFIG_WHPX' not in accelerators and get_option('whpx').enabled()
  error('WHPX not available on this platform')
endif

################
# Dependencies #
################

# When bumping glib minimum version, please check also whether to increase
# the _WIN32_WINNT setting in osdep.h according to the value from glib.
# You should also check if any of the glib.version() checks
# below can also be removed.
glib_req_ver = '>=2.66.0'
glib_pc = dependency('glib-2.0', version: glib_req_ver, required: true,
                    method: 'pkg-config')
glib_cflags = []

# This workaround is required due to a bug in pkg-config file for glib as it
# doesn't define GLIB_STATIC_COMPILATION for pkg-config --static
if host_os == 'windows' and get_option('prefer_static')
  glib_cflags += ['-DGLIB_STATIC_COMPILATION']
endif

# Sanity check that the current size_t matches the
# size that glib thinks it should be. This catches
# problems on multi-arch where people try to build
# 32-bit QEMU while pointing at 64-bit glib headers

if not cc.compiles('''
  #include <glib.h>
  #include <unistd.h>

  #define QEMU_BUILD_BUG_ON(x) \
  typedef char qemu_build_bug_on[(x)?-1:1] __attribute__((unused));

  int main(void) {
     QEMU_BUILD_BUG_ON(sizeof(size_t) != GLIB_SIZEOF_SIZE_T);
     return 0;
  }''', dependencies: glib_pc, args: glib_cflags)
  error('''sizeof(size_t) doesn't match GLIB_SIZEOF_SIZE_T.
        You probably need to set PKG_CONFIG_LIBDIR" to point
        to the right pkg-config files for your build target.''')
endif

glib = declare_dependency(dependencies: [glib_pc],
                          compile_args: glib_cflags,
                          version: glib_pc.version())

# Check whether glib has gslice, which we have to avoid for correctness.
# TODO: remove this check and the corresponding workaround (qtree) when
# the minimum supported glib is >= 2.75.3
glib_has_gslice = glib.version().version_compare('<2.75.3')
# Check whether glib has the aligned_alloc family of functions.
# <https://docs.gtk.org/glib/func.aligned_alloc.html>
glib_has_aligned_alloc = glib.version().version_compare('>=2.72.0')

# override glib dep to include the above refinements
meson.override_dependency('glib-2.0', glib)

# The path to glib.h is added to all compilation commands.
add_project_dependencies(glib.partial_dependency(compile_args: true, includes: true),
                         native: false, language: all_languages)

lttng = not_found
if 'ust' in get_option('trace_backends')
  lttng = dependency('lttng-ust', required: true, version: '>= 2.1',
                     method: 'pkg-config')
endif

pixman = dependency('pixman-1', required: true, version: '>=0.21.8',
                    method: 'pkg-config')

zlib = dependency('zlib', required: true)

libaio = not_found
if not get_option('linux_aio').auto() or have_block
  libaio = cc.find_library('aio', has_headers: ['libaio.h'],
                           required: get_option('linux_aio'))
endif

linux_io_uring_test = '''
  #include <liburing.h>
  #include <linux/errqueue.h>

  int main(void) { return 0; }'''

linux_io_uring = not_found
if not get_option('linux_io_uring').auto() or have_block
  linux_io_uring = dependency('liburing', version: '>=0.3',
                              required: get_option('linux_io_uring'),
                              method: 'pkg-config')
  if not cc.links(linux_io_uring_test)
    linux_io_uring = not_found
  endif
endif

libnfs = not_found
if not get_option('libnfs').auto() or have_block
  libnfs = dependency('libnfs', version: ['>=1.9.3', '<6.0.0'],
                      required: get_option('libnfs'),
                      method: 'pkg-config')
endif

cocoa = dependency('appleframeworks',
                   modules: ['Cocoa', 'CoreVideo', 'QuartzCore'],
                   required: get_option('cocoa'))

vmnet = dependency('appleframeworks', modules: 'vmnet', required: get_option('vmnet'))
if vmnet.found() and not cc.has_header_symbol('vmnet/vmnet.h',
                                              'VMNET_BRIDGED_MODE',
                                              dependencies: vmnet)
  vmnet = not_found
  if get_option('vmnet').enabled()
    error('vmnet.framework API is outdated')
  else
    warning('vmnet.framework API is outdated, disabling')
  endif
endif

seccomp = not_found
seccomp_has_sysrawrc = false
if not get_option('seccomp').auto() or have_system or have_tools
  seccomp = dependency('libseccomp', version: '>=2.3.0',
                       required: get_option('seccomp'),
                       method: 'pkg-config')
  if seccomp.found()
    seccomp_has_sysrawrc = cc.has_header_symbol('seccomp.h',
                                                'SCMP_FLTATR_API_SYSRAWRC',
                                                dependencies: seccomp)
  endif
endif

libcap_ng = not_found
if not get_option('cap_ng').auto() or have_system or have_tools
  libcap_ng = cc.find_library('cap-ng', has_headers: ['cap-ng.h'],
                              required: get_option('cap_ng'))
endif
if libcap_ng.found() and not cc.links('''
   #include <cap-ng.h>
   int main(void)
   {
     capng_capability_to_name(CAPNG_EFFECTIVE);
     return 0;
   }''', dependencies: libcap_ng)
  libcap_ng = not_found
  if get_option('cap_ng').enabled()
    error('could not link libcap-ng')
  else
    warning('could not link libcap-ng, disabling')
  endif
endif

if get_option('xkbcommon').auto() and not have_system and not have_tools
  xkbcommon = not_found
else
  xkbcommon = dependency('xkbcommon', required: get_option('xkbcommon'),
                         method: 'pkg-config')
endif

slirp = not_found
if not get_option('slirp').auto() or have_system
  slirp = dependency('slirp', required: get_option('slirp'),
                     method: 'pkg-config')
  # slirp < 4.7 is incompatible with CFI support in QEMU.  This is because
  # it passes function pointers within libslirp as callbacks for timers.
  # When using a system-wide shared libslirp, the type information for the
  # callback is missing and the timer call produces a false positive with CFI.
  # Do not use the "version" keyword argument to produce a better error.
  # with control-flow integrity.
  if get_option('cfi') and slirp.found() and slirp.version().version_compare('<4.7')
    if get_option('slirp').enabled()
      error('Control-Flow Integrity requires libslirp 4.7.')
    else
      warning('Cannot use libslirp since Control-Flow Integrity requires libslirp >= 4.7.')
      slirp = not_found
    endif
  endif
endif

vde = not_found
if not get_option('vde').auto() or have_system or have_tools
  vde = cc.find_library('vdeplug', has_headers: ['libvdeplug.h'],
                           required: get_option('vde'))
endif
if vde.found() and not cc.links('''
   #include <libvdeplug.h>
   int main(void)
   {
     struct vde_open_args a = {0, 0, 0};
     char s[] = "";
     vde_open(s, s, &a);
     return 0;
   }''', dependencies: vde)
  vde = not_found
  if get_option('cap_ng').enabled()
    error('could not link libvdeplug')
  else
    warning('could not link libvdeplug, disabling')
  endif
endif

pulse = not_found
if not get_option('pa').auto() or (host_os == 'linux' and have_system)
  pulse = dependency('libpulse', required: get_option('pa'),
                     method: 'pkg-config')
endif
alsa = not_found
if not get_option('alsa').auto() or (host_os == 'linux' and have_system)
  alsa = dependency('alsa', required: get_option('alsa'),
                    method: 'pkg-config')
endif
jack = not_found
if not get_option('jack').auto() or have_system
  jack = dependency('jack', required: get_option('jack'),
                    method: 'pkg-config')
endif
pipewire = not_found
if not get_option('pipewire').auto() or (host_os == 'linux' and have_system)
  pipewire = dependency('libpipewire-0.3', version: '>=0.3.60',
                    required: get_option('pipewire'),
                    method: 'pkg-config')
endif
sndio = not_found
if not get_option('sndio').auto() or have_system
  sndio = dependency('sndio', required: get_option('sndio'),
                    method: 'pkg-config')
endif

rt = cc.find_library('rt', required: false)

zstd = not_found
if not get_option('zstd').auto() or have_block
  zstd = dependency('libzstd', version: '>=1.4.0',
                    required: get_option('zstd'),
                    method: 'pkg-config')
endif

blkio = not_found
if not get_option('blkio').auto() or have_block
  blkio = dependency('blkio',
                     method: 'pkg-config',
                     required: get_option('blkio'))
endif
curl = not_found
if not get_option('curl').auto() or have_block
  curl = dependency('libcurl', version: '>=7.29.0',
                    method: 'pkg-config',
                    required: get_option('curl'))
endif
libudev = not_found
if host_os == 'linux' and (have_system or have_tools)
  libudev = dependency('libudev',
                       method: 'pkg-config',
                       required: get_option('libudev'))
endif

mpathlibs = [libudev]
mpathpersist = not_found
if host_os == 'linux' and have_tools and get_option('mpath').allowed()
  mpath_test_source = '''
    #include <libudev.h>
    #include <mpath_persist.h>
    unsigned mpath_mx_alloc_len = 1024;
    int logsink;
    static struct config *multipath_conf;
    extern struct udev *udev;
    extern struct config *get_multipath_config(void);
    extern void put_multipath_config(struct config *conf);
    struct udev *udev;
    struct config *get_multipath_config(void) { return multipath_conf; }
    void put_multipath_config(struct config *conf) { }
    int main(void) {
        udev = udev_new();
        multipath_conf = mpath_lib_init();
        return 0;
    }'''
  libmpathpersist = cc.find_library('mpathpersist',
                                    required: get_option('mpath'))
  if libmpathpersist.found()
    mpathlibs += libmpathpersist
    if get_option('prefer_static')
      mpathlibs += cc.find_library('devmapper',
                                     required: get_option('mpath'))
    endif
    mpathlibs += cc.find_library('multipath',
                                 required: get_option('mpath'))
    foreach lib: mpathlibs
      if not lib.found()
        mpathlibs = []
        break
      endif
    endforeach
    if mpathlibs.length() == 0
      msg = 'Dependencies missing for libmpathpersist'
    elif cc.links(mpath_test_source, dependencies: mpathlibs)
      mpathpersist = declare_dependency(dependencies: mpathlibs)
    else
      msg = 'Cannot detect libmpathpersist API'
    endif
    if not mpathpersist.found()
      if get_option('mpath').enabled()
        error(msg)
      else
        warning(msg + ', disabling')
      endif
    endif
  endif
endif

iconv = not_found
curses = not_found
if have_system and get_option('curses').allowed()
  curses_test = '''
    #ifdef __APPLE__
    #define _XOPEN_SOURCE_EXTENDED 1
    #endif
    #include <locale.h>
    #include <curses.h>
    #include <wchar.h>
    int main(void) {
      wchar_t wch = L'w';
      setlocale(LC_ALL, "");
      resize_term(0, 0);
      addwstr(L"wide chars\n");
      addnwstr(&wch, 1);
      add_wch(WACS_DEGREE);
      return 0;
    }'''

  curses_dep_list = host_os == 'windows' ? ['ncurses', 'ncursesw'] : ['ncursesw']
  curses = dependency(curses_dep_list,
                      required: false,
                      method: 'pkg-config')
  msg = get_option('curses').enabled() ? 'curses library not found' : ''
  curses_compile_args = ['-DNCURSES_WIDECHAR=1']
  if curses.found()
    if cc.links(curses_test, args: curses_compile_args, dependencies: [curses])
      curses = declare_dependency(compile_args: curses_compile_args, dependencies: [curses],
                                  version: curses.version())
    else
      msg = 'curses package not usable'
      curses = not_found
    endif
  endif
  if not curses.found()
    has_curses_h = cc.has_header('curses.h', args: curses_compile_args)
    if host_os != 'windows' and not has_curses_h
      message('Trying with /usr/include/ncursesw')
      curses_compile_args += ['-I/usr/include/ncursesw']
      has_curses_h = cc.has_header('curses.h', args: curses_compile_args)
    endif
    if has_curses_h
      curses_libname_list = (host_os == 'windows' ? ['pdcurses'] : ['ncursesw', 'cursesw'])
      foreach curses_libname : curses_libname_list
        libcurses = cc.find_library(curses_libname,
                                    required: false)
        if libcurses.found()
          if cc.links(curses_test, args: curses_compile_args, dependencies: libcurses)
            curses = declare_dependency(compile_args: curses_compile_args,
                                        dependencies: [libcurses])
            break
          else
            msg = 'curses library not usable'
          endif
        endif
      endforeach
    endif
  endif
  if get_option('iconv').allowed()
    foreach link_args : [ ['-liconv'], [] ]
      # Programs will be linked with glib and this will bring in libiconv on FreeBSD.
      # We need to use libiconv if available because mixing libiconv's headers with
      # the system libc does not work.
      # However, without adding glib to the dependencies -L/usr/local/lib will not be
      # included in the command line and libiconv will not be found.
      if cc.links('''
        #include <iconv.h>
        int main(void) {
          iconv_t conv = iconv_open("WCHAR_T", "UCS-2");
          return conv != (iconv_t) -1;
        }''', args: link_args, dependencies: glib)
        iconv = declare_dependency(link_args: link_args, dependencies: glib)
        break
      endif
    endforeach
  endif
  if curses.found() and not iconv.found()
    if get_option('iconv').enabled()
      error('iconv not available')
    endif
    msg = 'iconv required for curses UI but not available'
    curses = not_found
  endif
  if not curses.found() and msg != ''
    if get_option('curses').enabled()
      error(msg)
    else
      warning(msg + ', disabling')
    endif
  endif
endif

brlapi = not_found
if not get_option('brlapi').auto() or have_system
  brlapi = cc.find_library('brlapi', has_headers: ['brlapi.h'],
                         required: get_option('brlapi'))
  if brlapi.found() and not cc.links('''
    #include <brlapi.h>
    #include <stddef.h>
    int main(void) {
      return brlapi__openConnection(NULL, NULL, NULL) == BRLAPI_INVALID_FILE_DESCRIPTOR;
    }''', dependencies: brlapi)
    brlapi = not_found
    if get_option('brlapi').enabled()
      error('could not link brlapi')
    else
      warning('could not link brlapi, disabling')
    endif
  endif
endif

sdl = not_found
if not get_option('sdl').auto() or have_system
  sdl = dependency('sdl2', required: get_option('sdl'))
  sdl_image = not_found
endif
if sdl.found()
  # Some versions of SDL have problems with -Wundef
  if not cc.compiles('''
                     #include <SDL.h>
                     #include <SDL_syswm.h>
                     int main(int argc, char *argv[]) { return 0; }
                     ''', dependencies: sdl, args: '-Werror=undef')
    sdl = declare_dependency(compile_args: '-Wno-undef',
                             dependencies: sdl,
                             version: sdl.version())
  endif
  sdl_image = dependency('SDL2_image', required: get_option('sdl_image'),
                         method: 'pkg-config')
else
  if get_option('sdl_image').enabled()
    error('sdl-image required, but SDL was @0@'.format(
          get_option('sdl').disabled() ? 'disabled' : 'not found'))
  endif
  sdl_image = not_found
endif

rbd = not_found
if not get_option('rbd').auto() or have_block
  librados = cc.find_library('rados', required: get_option('rbd'))
  librbd = cc.find_library('rbd', has_headers: ['rbd/librbd.h'],
                           required: get_option('rbd'))
  if librados.found() and librbd.found()
    if cc.links('''
      #include <stdio.h>
      #include <rbd/librbd.h>
      int main(void) {
        rados_t cluster;
        rados_create(&cluster, NULL);
        #if LIBRBD_VERSION_CODE < LIBRBD_VERSION(1, 12, 0)
        #error
        #endif
        return 0;
      }''', dependencies: [librbd, librados])
      rbd = declare_dependency(dependencies: [librbd, librados])
    elif get_option('rbd').enabled()
      error('librbd >= 1.12.0 required')
    else
      warning('librbd >= 1.12.0 not found, disabling')
    endif
  endif
endif

glusterfs = not_found
glusterfs_ftruncate_has_stat = false
glusterfs_iocb_has_stat = false
if not get_option('glusterfs').auto() or have_block
  glusterfs = dependency('glusterfs-api', version: '>=3',
                         required: get_option('glusterfs'),
                         method: 'pkg-config')
  if glusterfs.found()
    glusterfs_ftruncate_has_stat = cc.links('''
      #include <glusterfs/api/glfs.h>

      int
      main(void)
      {
          /* new glfs_ftruncate() passes two additional args */
          return glfs_ftruncate(NULL, 0, NULL, NULL);
      }
    ''', dependencies: glusterfs)
    glusterfs_iocb_has_stat = cc.links('''
      #include <glusterfs/api/glfs.h>

      /* new glfs_io_cbk() passes two additional glfs_stat structs */
      static void
      glusterfs_iocb(glfs_fd_t *fd, ssize_t ret, struct glfs_stat *prestat, struct glfs_stat *poststat, void *data)
      {}

      int
      main(void)
      {
          glfs_io_cbk iocb = &glusterfs_iocb;
          iocb(NULL, 0 , NULL, NULL, NULL);
          return 0;
      }
    ''', dependencies: glusterfs)
  endif
endif

libssh = not_found
if not get_option('libssh').auto() or have_block
  libssh = dependency('libssh', version: '>=0.8.7',
                    method: 'pkg-config',
                    required: get_option('libssh'))
endif

libbzip2 = not_found
if not get_option('bzip2').auto() or have_block
  libbzip2 = cc.find_library('bz2', has_headers: ['bzlib.h'],
                             required: get_option('bzip2'))
  if libbzip2.found() and not cc.links('''
     #include <bzlib.h>
     int main(void) { BZ2_bzlibVersion(); return 0; }''', dependencies: libbzip2)
    libbzip2 = not_found
    if get_option('bzip2').enabled()
      error('could not link libbzip2')
    else
      warning('could not link libbzip2, disabling')
    endif
  endif
endif

if host_os == 'darwin'
  if host_arch == 'aarch64'
    brew_inc_dirs = include_directories('/opt/homebrew/include/', is_system: true)
  else
    brew_inc_dirs = include_directories('/usr/local/include/', is_system: true)
  endif
  raw_liblzfse = cc.find_library('lzfse',
    dirs: ['/opt/homebrew/lib/', '/usr/local/lib/'],
    header_include_directories: brew_inc_dirs,
    has_headers: ['lzfse.h'],
    required: true
  )
  liblzfse = declare_dependency(
    dependencies: raw_liblzfse,
    include_directories: brew_inc_dirs
  )
else
  liblzfse = cc.find_library('lzfse', has_headers: ['lzfse.h'], required: true)
endif
if liblzfse.found() and not cc.links('''
   #include <lzfse.h>
   int main(void) { lzfse_decode_scratch_size(); return 0; }''',
   dependencies: liblzfse)

  error('could not link liblzfse')
endif

oss = not_found
if get_option('oss').allowed() and have_system
  if not cc.has_header('sys/soundcard.h')
    # not found
  elif host_os == 'netbsd'
    oss = cc.find_library('ossaudio', required: get_option('oss'))
  else
    oss = declare_dependency()
  endif

  if not oss.found()
    if get_option('oss').enabled()
      error('OSS not found')
    endif
  endif
endif
dsound = not_found
if not get_option('dsound').auto() or (host_os == 'windows' and have_system)
  if cc.has_header('dsound.h')
    dsound = declare_dependency(link_args: ['-lole32', '-ldxguid'])
  endif

  if not dsound.found()
    if get_option('dsound').enabled()
      error('DirectSound not found')
    endif
  endif
endif

coreaudio = not_found
if not get_option('coreaudio').auto() or (host_os == 'darwin' and have_system)
  coreaudio = dependency('appleframeworks', modules: 'CoreAudio',
                         required: get_option('coreaudio'))
endif

libcbor = not_found
if not get_option('libcbor').auto() or have_system
  libcbor = dependency('libcbor', version: '>=0.7.0',
                       required: get_option('libcbor'))
endif

gnutls = not_found
if get_option('gnutls').enabled() or (get_option('gnutls').auto() and have_system)
  gnutls = dependency('gnutls', version: '>=3.5.18',
                      method: 'pkg-config',
                      required: get_option('gnutls'))
endif

nettle = not_found
hogweed = not_found
xts = 'none'

nettle = dependency('nettle', version: '>=3.10',
                    method: 'pkg-config',
                    required: true)
if not cc.has_header('nettle/xts.h', dependencies: nettle)
  xts = 'private'
endif

gmp = dependency('gmp', required: false, method: 'pkg-config')
if nettle.found() and gmp.found()
  hogweed = dependency('hogweed', version: '>=3.4',
                       method: 'pkg-config',
                       required: true)
endif

gtk = not_found
gtkx11 = not_found
vte = not_found
have_gtk_clipboard = get_option('gtk_clipboard').enabled()

if get_option('gtk') \
             .disable_auto_if(not have_system) \
             .allowed()
  gtk = dependency('gtk+-3.0', version: '>=3.22.0',
                   method: 'pkg-config',
                   required: get_option('gtk'))
  if gtk.found()
    gtkx11 = dependency('gtk+-x11-3.0', version: '>=3.22.0',
                        method: 'pkg-config',
                        required: false)
    gtk = declare_dependency(dependencies: [gtk, gtkx11],
                             version: gtk.version())

    if not get_option('vte').auto() or have_system
      vte = dependency('vte-2.91',
                       method: 'pkg-config',
                       required: get_option('vte'))
    endif
  elif have_gtk_clipboard
    error('GTK clipboard requested, but GTK not found')
  endif
endif

x11 = dependency('x11', method: 'pkg-config', required: gtkx11.found())

png = not_found
if get_option('png').allowed() and have_system
   png = dependency('libpng', version: '>=1.6.34', required: get_option('png'),
                    method: 'pkg-config')
endif
vnc = not_found
jpeg = not_found
sasl = not_found
if get_option('vnc') \
             .disable_auto_if(not have_system) \
             .allowed()
  vnc = declare_dependency() # dummy dependency
  jpeg = dependency('libjpeg', required: get_option('vnc_jpeg'),
                    method: 'pkg-config')
  sasl = cc.find_library('sasl2', has_headers: ['sasl/sasl.h'],
                         required: get_option('vnc_sasl'))
  if sasl.found()
    sasl = declare_dependency(dependencies: sasl,
                              compile_args: '-DSTRUCT_IOVEC_DEFINED')
  endif
endif

pam = not_found
if not get_option('auth_pam').auto() or have_system
  pam = cc.find_library('pam', has_headers: ['security/pam_appl.h'],
                        required: get_option('auth_pam'))
endif
if pam.found() and not cc.links('''
   #include <stddef.h>
   #include <security/pam_appl.h>
   int main(void) {
     const char *service_name = "qemu";
     const char *user = "frank";
     const struct pam_conv pam_conv = { 0 };
     pam_handle_t *pamh = NULL;
     pam_start(service_name, user, &pam_conv, &pamh);
     return 0;
   }''', dependencies: pam)
  pam = not_found
  if get_option('auth_pam').enabled()
    error('could not link libpam')
  else
    warning('could not link libpam, disabling')
  endif
endif

snappy = not_found
if not get_option('snappy').auto() or have_system
  snappy = cc.find_library('snappy', has_headers: ['snappy-c.h'],
                           required: get_option('snappy'))
endif
if snappy.found() and not cc.links('''
   #include <snappy-c.h>
   int main(void) { snappy_max_compressed_length(4096); return 0; }''', dependencies: snappy)
  snappy = not_found
  if get_option('snappy').enabled()
    error('could not link libsnappy')
  else
    warning('could not link libsnappy, disabling')
  endif
endif

lzo = not_found
if not get_option('lzo').auto() or have_system
  lzo = cc.find_library('lzo2', has_headers: ['lzo/lzo1x.h'],
                        required: get_option('lzo'))
endif
if lzo.found() and not cc.links('''
   #include <lzo/lzo1x.h>
   int main(void) { lzo_version(); return 0; }''', dependencies: lzo)
  lzo = not_found
  if get_option('lzo').enabled()
    error('could not link liblzo2')
  else
    warning('could not link liblzo2, disabling')
  endif
endif

usbredir = not_found
if not get_option('usb_redir').auto() or have_system
  usbredir = dependency('libusbredirparser-0.5', required: get_option('usb_redir'),
                        version: '>=0.6', method: 'pkg-config')
endif
libusb = not_found
if not get_option('libusb').auto() or have_system
  libusb = dependency('libusb-1.0', required: get_option('libusb'),
                      version: '>=1.0.13', method: 'pkg-config')
endif

libpmem = not_found
if not get_option('libpmem').auto() or have_system
  libpmem = dependency('libpmem', required: get_option('libpmem'),
                       method: 'pkg-config')
endif
libdaxctl = not_found
if not get_option('libdaxctl').auto() or have_system
  libdaxctl = dependency('libdaxctl', required: get_option('libdaxctl'),
                         version: '>=57', method: 'pkg-config')
endif
avahi = not_found
avahi_glib = not_found
if host_os == 'linux' and have_system
  avahi = dependency('avahi-client', required: false, method: 'pkg-config')
  avahi_glib = dependency('avahi-glib', required: false, method: 'pkg-config')
endif
tasn1 = dependency('libtasn1',
                   required: true,
                   method: 'pkg-config')
keyutils = not_found
if not get_option('libkeyutils').auto() or have_block
  keyutils = dependency('libkeyutils', required: get_option('libkeyutils'),
                        method: 'pkg-config')
endif

has_gettid = cc.has_function('gettid')

# Malloc tests

malloc = []
if get_option('malloc') == 'system'
  has_malloc_trim = \
    get_option('malloc_trim').allowed() and \
    cc.has_function('malloc_trim', prefix: '#include <malloc.h>')
else
  has_malloc_trim = false
  malloc = cc.find_library(get_option('malloc'), required: true)
endif
if not has_malloc_trim and get_option('malloc_trim').enabled()
  if get_option('malloc') == 'system'
    error('malloc_trim not available on this platform.')
  else
    error('malloc_trim not available with non-libc memory allocator')
  endif
endif

osdep_prefix = '''
  #ifndef _GNU_SOURCE
  #define _GNU_SOURCE
  #endif

  #include <stddef.h>
  #include <sys/types.h>

  #include <string.h>
  #include <limits.h>
  /* Put unistd.h before time.h as that triggers localtime_r/gmtime_r
   * function availability on recentish Mingw-w64 platforms. */
  #include <unistd.h>
  #include <time.h>
  #include <errno.h>
  #include <fcntl.h>
'''

if get_option('fuse').disabled() and get_option('fuse_lseek').enabled()
  error('Cannot enable fuse-lseek while fuse is disabled')
endif

fuse = dependency('fuse3', required: get_option('fuse'),
                  version: '>=3.1', method: 'pkg-config')

fuse_lseek = not_found
if get_option('fuse_lseek').allowed()
  if fuse.version().version_compare('>=3.8')
    # Dummy dependency
    fuse_lseek = declare_dependency()
  elif get_option('fuse_lseek').enabled()
    if fuse.found()
      error('fuse-lseek requires libfuse >=3.8, found ' + fuse.version())
    else
      error('fuse-lseek requires libfuse, which was not found')
    endif
  endif
endif

#################
# config-host.h #
#################

config_host_data = configuration_data()

audio_drivers_selected = []
if have_system
  audio_drivers_available = {
    'alsa': alsa.found(),
    'coreaudio': coreaudio.found(),
    'dsound': dsound.found(),
    'jack': jack.found(),
    'oss': oss.found(),
    'pa': pulse.found(),
    'pipewire': pipewire.found(),
    'sdl': sdl.found(),
    'sndio': sndio.found(),
  }
  foreach k, v: audio_drivers_available
    config_host_data.set('CONFIG_AUDIO_' + k.to_upper(), v)
  endforeach

  # Default to native drivers first, OSS second, SDL third
  audio_drivers_priority = \
    [ 'pa', 'coreaudio', 'dsound', 'sndio', 'oss' ] + \
    (host_os == 'linux' ? [] : [ 'sdl' ])
  audio_drivers_default = []
  foreach k: audio_drivers_priority
    if audio_drivers_available[k]
      audio_drivers_default += k
    endif
  endforeach

  foreach k: get_option('audio_drv_list')
    if k == 'default'
      audio_drivers_selected += audio_drivers_default
    elif not audio_drivers_available[k]
      error('Audio driver "@0@" not available.'.format(k))
    else
      audio_drivers_selected += k
    endif
  endforeach
endif
config_host_data.set('CONFIG_AUDIO_DRIVERS',
                     '"' + '", "'.join(audio_drivers_selected) + '", ')

have_host_block_device = (host_os != 'darwin' or
    cc.has_header('IOKit/storage/IOMedia.h'))

qga_fsfreeze = false
qga_fstrim = false
if host_os == 'linux'
    if cc.has_header_symbol('linux/fs.h', 'FIFREEZE')
        qga_fsfreeze = true
    endif
    if cc.has_header_symbol('linux/fs.h', 'FITRIM')
        qga_fstrim = true
    endif
elif host_os == 'freebsd' and cc.has_header_symbol('ufs/ffs/fs.h', 'UFSSUSPEND')
    qga_fsfreeze = true
endif

if get_option('block_drv_ro_whitelist') == ''
  config_host_data.set('CONFIG_BDRV_RO_WHITELIST', '')
else
  config_host_data.set('CONFIG_BDRV_RO_WHITELIST',
        '"' + get_option('block_drv_ro_whitelist').replace(',', '", "') + '", ')
endif
if get_option('block_drv_rw_whitelist') == ''
  config_host_data.set('CONFIG_BDRV_RW_WHITELIST', '')
else
  config_host_data.set('CONFIG_BDRV_RW_WHITELIST',
        '"' + get_option('block_drv_rw_whitelist').replace(',', '", "') + '", ')
endif

foreach k : get_option('trace_backends')
  config_host_data.set('CONFIG_TRACE_' + k.to_upper(), true)
endforeach
config_host_data.set_quoted('CONFIG_TRACE_FILE', get_option('trace_file'))
config_host_data.set_quoted('CONFIG_TLS_PRIORITY', get_option('tls_priority'))
config_host_data.set_quoted('CONFIG_BINDIR', get_option('prefix') / get_option('bindir'))
config_host_data.set_quoted('CONFIG_PREFIX', get_option('prefix'))
config_host_data.set_quoted('CONFIG_QEMU_CONFDIR', get_option('prefix') / qemu_confdir)
config_host_data.set_quoted('CONFIG_QEMU_DATADIR', get_option('prefix') / qemu_datadir)
config_host_data.set_quoted('CONFIG_QEMU_DESKTOPDIR', get_option('prefix') / qemu_desktopdir)

config_host_data.set_quoted('CONFIG_QEMU_HELPERDIR', get_option('prefix') / get_option('libexecdir'))
config_host_data.set_quoted('CONFIG_QEMU_ICONDIR', get_option('prefix') / qemu_icondir)
config_host_data.set_quoted('CONFIG_QEMU_LOCALEDIR', get_option('prefix') / get_option('localedir'))
config_host_data.set_quoted('CONFIG_QEMU_LOCALSTATEDIR', get_option('prefix') / get_option('localstatedir'))
config_host_data.set_quoted('CONFIG_SYSCONFDIR', get_option('prefix') / get_option('sysconfdir'))

have_slirp_smbd = get_option('slirp_smbd') \
  .require(host_os != 'windows', error_message: 'Host smbd not supported on this platform.') \
  .allowed()
if have_slirp_smbd
  smbd_path = get_option('smbd')
  if smbd_path == ''
    smbd_path = (host_os == 'sunos' ? '/usr/sfw/sbin/smbd' : '/usr/sbin/smbd')
  endif
  config_host_data.set_quoted('CONFIG_SMBD_COMMAND', smbd_path)
endif

config_host_data.set('HOST_' + host_arch.to_upper(), 1)

kvm_targets_c = '""'
if get_option('kvm').allowed() and host_os == 'linux'
  kvm_targets_c = '"' + '" ,"'.join(kvm_targets) + '"'
endif
config_host_data.set('CONFIG_KVM_TARGETS', kvm_targets_c)

config_host_data.set('CONFIG_BDRV_WHITELIST_TOOLS', get_option('block_drv_whitelist_in_tools'))
config_host_data.set('CONFIG_BRLAPI', brlapi.found())
config_host_data.set('CONFIG_BSD', host_os in bsd_oses)
config_host_data.set('CONFIG_FREEBSD', host_os == 'freebsd')
config_host_data.set('CONFIG_COCOA', cocoa.found())
config_host_data.set('CONFIG_DARWIN', host_os == 'darwin')
config_host_data.set('CONFIG_GCOV', get_option('b_coverage'))
config_host_data.set('CONFIG_LIBUDEV', libudev.found())
config_host_data.set('CONFIG_LINUX', host_os == 'linux')
config_host_data.set('CONFIG_POSIX', host_os != 'windows')
config_host_data.set('CONFIG_WIN32', host_os == 'windows')
config_host_data.set('CONFIG_LZO', lzo.found())
config_host_data.set('CONFIG_MPATH', mpathpersist.found())
config_host_data.set('CONFIG_BLKIO', blkio.found())
if blkio.found()
  config_host_data.set('CONFIG_BLKIO_WRITE_EROS_FUA',
                       blkio.version().version_compare('>=1.4.0'))
endif
config_host_data.set('CONFIG_CURL', curl.found())
config_host_data.set('CONFIG_CURSES', curses.found())
config_host_data.set('CONFIG_GLUSTERFS', glusterfs.found())
if glusterfs.found()
  config_host_data.set('CONFIG_GLUSTERFS_XLATOR_OPT', glusterfs.version().version_compare('>=4'))
  config_host_data.set('CONFIG_GLUSTERFS_DISCARD', glusterfs.version().version_compare('>=5'))
  config_host_data.set('CONFIG_GLUSTERFS_FALLOCATE', glusterfs.version().version_compare('>=6'))
  config_host_data.set('CONFIG_GLUSTERFS_ZEROFILL', glusterfs.version().version_compare('>=6'))
  config_host_data.set('CONFIG_GLUSTERFS_FTRUNCATE_HAS_STAT', glusterfs_ftruncate_has_stat)
  config_host_data.set('CONFIG_GLUSTERFS_IOCB_HAS_STAT', glusterfs_iocb_has_stat)
endif
config_host_data.set('CONFIG_GTK', gtk.found())
config_host_data.set('CONFIG_VTE', vte.found())
config_host_data.set('CONFIG_GTK_CLIPBOARD', have_gtk_clipboard)
config_host_data.set('CONFIG_AVAHI', avahi.found() and avahi_glib.found())
config_host_data.set('CONFIG_LIBCAP_NG', libcap_ng.found())
config_host_data.set('CONFIG_LIBDAXCTL', libdaxctl.found())
config_host_data.set('CONFIG_LIBNFS', libnfs.found())
config_host_data.set('CONFIG_LIBSSH', libssh.found())
config_host_data.set('CONFIG_LINUX_AIO', libaio.found())
config_host_data.set('CONFIG_LINUX_IO_URING', linux_io_uring.found())
config_host_data.set('CONFIG_LIBPMEM', libpmem.found())
config_host_data.set('CONFIG_RBD', rbd.found())
config_host_data.set('CONFIG_RELOCATABLE', get_option('relocatable'))
config_host_data.set('CONFIG_SAFESTACK', get_option('safe_stack'))
config_host_data.set('CONFIG_SDL', sdl.found())
config_host_data.set('CONFIG_SDL_IMAGE', sdl_image.found())
config_host_data.set('CONFIG_SECCOMP', seccomp.found())
if seccomp.found()
  config_host_data.set('CONFIG_SECCOMP_SYSRAWRC', seccomp_has_sysrawrc)
endif
config_host_data.set('CONFIG_SLIRP', slirp.found())
config_host_data.set('CONFIG_SNAPPY', snappy.found())
config_host_data.set('CONFIG_SOLARIS', host_os == 'sunos')
if have_tcg
  config_host_data.set('CONFIG_TCG', 1)
  config_host_data.set('CONFIG_TCG_INTERPRETER', tcg_arch == 'tci')
endif
config_host_data.set('CONFIG_TSAN', get_option('tsan'))
config_host_data.set('CONFIG_USB_LIBUSB', libusb.found())
config_host_data.set('CONFIG_VDE', vde.found())
config_host_data.set('CONFIG_VMNET', vmnet.found())
config_host_data.set('CONFIG_PNG', png.found())
config_host_data.set('CONFIG_VNC', vnc.found())
config_host_data.set('CONFIG_VNC_JPEG', jpeg.found())
config_host_data.set('CONFIG_VNC_SASL', sasl.found())
config_host_data.set('CONFIG_VTE', vte.found())
config_host_data.set('CONFIG_XKBCOMMON', xkbcommon.found())
config_host_data.set('CONFIG_KEYUTILS', keyutils.found())
config_host_data.set('CONFIG_GETTID', has_gettid)
config_host_data.set('CONFIG_GNUTLS', gnutls.found())
config_host_data.set('CONFIG_TASN1', tasn1.found())
config_host_data.set('CONFIG_HOGWEED', hogweed.found())
config_host_data.set('CONFIG_QEMU_PRIVATE_XTS', xts == 'private')
config_host_data.set('CONFIG_MALLOC_TRIM', has_malloc_trim)
config_host_data.set('CONFIG_ZSTD', zstd.found())
config_host_data.set('CONFIG_FUSE', fuse.found())
config_host_data.set('CONFIG_FUSE_LSEEK', fuse_lseek.found())
config_host_data.set('CONFIG_X11', x11.found())
config_host_data.set('CONFIG_CFI', get_option('cfi'))
config_host_data.set('QEMU_VERSION', '"@0@"'.format(meson.project_version()))
config_host_data.set('QEMU_VERSION_MAJOR', meson.project_version().split('QEMU-')[1].split('_')[0].split('.')[0])
config_host_data.set('QEMU_VERSION_MINOR', meson.project_version().split('QEMU-')[1].split('_')[0].split('.')[1])
config_host_data.set('QEMU_VERSION_MICRO', meson.project_version().split('QEMU-')[1].split('_')[0].split('.')[2])

config_host_data.set_quoted('CONFIG_HOST_DSOSUF', host_dsosuf)
config_host_data.set('HAVE_HOST_BLOCK_DEVICE', have_host_block_device)

have_coroutine_pool = get_option('coroutine_pool')
if get_option('debug_stack_usage') and have_coroutine_pool
  message('Disabling coroutine pool to measure stack usage')
  have_coroutine_pool = false
endif
config_host_data.set('CONFIG_COROUTINE_POOL', have_coroutine_pool)
config_host_data.set('CONFIG_DEBUG_GRAPH_LOCK', get_option('debug_graph_lock'))
config_host_data.set('CONFIG_DEBUG_MUTEX', get_option('debug_mutex'))
config_host_data.set('CONFIG_DEBUG_STACK_USAGE', get_option('debug_stack_usage'))
config_host_data.set('CONFIG_DEBUG_TCG', get_option('debug_tcg'))
config_host_data.set('CONFIG_DEBUG_REMAP', get_option('debug_remap'))
config_host_data.set('CONFIG_QOM_CAST_DEBUG', get_option('qom_cast_debug'))
config_host_data.set('CONFIG_FSFREEZE', qga_fsfreeze)
config_host_data.set('CONFIG_FSTRIM', qga_fstrim)

# has_header
config_host_data.set('CONFIG_EPOLL', cc.has_header('sys/epoll.h'))
config_host_data.set('CONFIG_LINUX_MAGIC_H', cc.has_header('linux/magic.h'))
valgrind = false
if get_option('valgrind').allowed()
    if cc.has_header('valgrind/valgrind.h')
        valgrind = true
    else
        if get_option('valgrind').enabled()
            error('valgrind requested but valgrind.h not found')
        endif
    endif
endif
config_host_data.set('CONFIG_VALGRIND_H', valgrind)
config_host_data.set('HAVE_BTRFS_H', cc.has_header('linux/btrfs.h'))
config_host_data.set('HAVE_DRM_H', cc.has_header('libdrm/drm.h'))
config_host_data.set('HAVE_OPENAT2_H', cc.has_header('linux/openat2.h'))
config_host_data.set('HAVE_PTY_H', cc.has_header('pty.h'))
config_host_data.set('HAVE_SYS_DISK_H', cc.has_header('sys/disk.h'))
config_host_data.set('HAVE_SYS_IOCCOM_H', cc.has_header('sys/ioccom.h'))
config_host_data.set('HAVE_SYS_KCOV_H', cc.has_header('sys/kcov.h'))
if host_os == 'windows'
  config_host_data.set('HAVE_AFUNIX_H', cc.has_header('afunix.h'))
endif

# has_function
config_host_data.set('CONFIG_CLOSE_RANGE', cc.has_function('close_range'))
config_host_data.set('CONFIG_ACCEPT4', cc.has_function('accept4'))
config_host_data.set('CONFIG_CLOCK_ADJTIME', cc.has_function('clock_adjtime'))
config_host_data.set('CONFIG_DUP3', cc.has_function('dup3'))
config_host_data.set('CONFIG_FALLOCATE', cc.has_function('fallocate'))
config_host_data.set('CONFIG_POSIX_FALLOCATE', cc.has_function('posix_fallocate'))
config_host_data.set('CONFIG_SCHED_GETCPU', cc.has_function('sched_getcpu', prefix: '#include <sched.h>'))
# Note that we need to specify prefix: here to avoid incorrectly
# thinking that Windows has posix_memalign()
config_host_data.set('CONFIG_POSIX_MEMALIGN', cc.has_function('posix_memalign', prefix: '#include <stdlib.h>'))
config_host_data.set('CONFIG_ALIGNED_MALLOC', cc.has_function('_aligned_malloc'))
config_host_data.set('CONFIG_VALLOC', cc.has_function('valloc'))
config_host_data.set('CONFIG_MEMALIGN', cc.has_function('memalign'))
config_host_data.set('CONFIG_PPOLL', cc.has_function('ppoll'))
config_host_data.set('CONFIG_PREADV', cc.has_function('preadv', prefix: '#include <sys/uio.h>'))
config_host_data.set('CONFIG_PTHREAD_FCHDIR_NP', cc.has_function('pthread_fchdir_np'))
config_host_data.set('CONFIG_SENDFILE', cc.has_function('sendfile'))
config_host_data.set('CONFIG_SETNS', cc.has_function('setns') and cc.has_function('unshare'))
config_host_data.set('CONFIG_SYNCFS', cc.has_function('syncfs'))
config_host_data.set('CONFIG_SYNC_FILE_RANGE', cc.has_function('sync_file_range'))
config_host_data.set('CONFIG_TIMERFD', cc.has_function('timerfd_create'))
config_host_data.set('CONFIG_GETLOADAVG', cc.has_function('getloadavg'))
config_host_data.set('HAVE_COPY_FILE_RANGE', cc.has_function('copy_file_range'))
config_host_data.set('HAVE_GETIFADDRS', cc.has_function('getifaddrs'))
config_host_data.set('HAVE_GLIB_WITH_SLICE_ALLOCATOR', glib_has_gslice)
config_host_data.set('HAVE_GLIB_WITH_ALIGNED_ALLOC', glib_has_aligned_alloc)
config_host_data.set('HAVE_OPENPTY', cc.has_function('openpty', dependencies: util))
config_host_data.set('HAVE_STRCHRNUL', cc.has_function('strchrnul', prefix: osdep_prefix))
config_host_data.set('HAVE_SYSTEM_FUNCTION', cc.has_function('system', prefix: '#include <stdlib.h>'))
if rbd.found()
  config_host_data.set('HAVE_RBD_NAMESPACE_EXISTS',
                       cc.has_function('rbd_namespace_exists',
                                       dependencies: rbd,
                                       prefix: '#include <rbd/librbd.h>'))
endif

have_asan_fiber = false
if get_option('asan') and \
   not cc.has_function('__sanitizer_start_switch_fiber',
                         args: '-fsanitize=address',
                         prefix: '#include <sanitizer/asan_interface.h>')
  warning('Missing ASAN due to missing fiber annotation interface')
  warning('Without code annotation, the report may be inferior.')
else
  have_asan_fiber = true
endif
config_host_data.set('CONFIG_ASAN_IFACE_FIBER', have_asan_fiber)

have_inotify_init = cc.has_header_symbol('sys/inotify.h', 'inotify_init')
have_inotify_init1 = cc.has_header_symbol('sys/inotify.h', 'inotify_init1')
inotify = not_found
if (have_inotify_init or have_inotify_init1) and host_os == 'freebsd'
  # libinotify-kqueue
  inotify = cc.find_library('inotify')
  if have_inotify_init
    have_inotify_init = inotify.found()
  endif
  if have_inotify_init1
    have_inotify_init1 = inotify.found()
  endif
endif
config_host_data.set('CONFIG_INOTIFY', have_inotify_init)
config_host_data.set('CONFIG_INOTIFY1', have_inotify_init1)

# has_header_symbol
config_host_data.set('CONFIG_BLKZONED',
                     cc.has_header_symbol('linux/blkzoned.h', 'BLKOPENZONE'))
config_host_data.set('CONFIG_EPOLL_CREATE1',
                     cc.has_header_symbol('sys/epoll.h', 'epoll_create1'))
config_host_data.set('CONFIG_FALLOCATE_PUNCH_HOLE',
                     cc.has_header_symbol('linux/falloc.h', 'FALLOC_FL_PUNCH_HOLE') and
                     cc.has_header_symbol('linux/falloc.h', 'FALLOC_FL_KEEP_SIZE'))
config_host_data.set('CONFIG_FALLOCATE_ZERO_RANGE',
                     cc.has_header_symbol('linux/falloc.h', 'FALLOC_FL_ZERO_RANGE'))
config_host_data.set('CONFIG_FIEMAP',
                     cc.has_header('linux/fiemap.h') and
                     cc.has_header_symbol('linux/fs.h', 'FS_IOC_FIEMAP'))
config_host_data.set('CONFIG_GETCPU',
                     cc.has_header_symbol('sched.h', 'getcpu', prefix: osdep_prefix))
config_host_data.set('CONFIG_GETRANDOM',
                     cc.has_function('getrandom') and
                     cc.has_header_symbol('sys/random.h', 'GRND_NONBLOCK'))
config_host_data.set('CONFIG_PRCTL_PR_SET_TIMERSLACK',
                     cc.has_header_symbol('sys/prctl.h', 'PR_SET_TIMERSLACK'))
config_host_data.set('CONFIG_RTNETLINK',
                     cc.has_header_symbol('linux/rtnetlink.h', 'IFLA_PROTO_DOWN'))
config_host_data.set('CONFIG_SYSMACROS',
                     cc.has_header_symbol('sys/sysmacros.h', 'makedev'))
config_host_data.set('HAVE_OPTRESET',
                     cc.has_header_symbol('getopt.h', 'optreset'))
config_host_data.set('HAVE_IPPROTO_MPTCP',
                     cc.has_header_symbol('netinet/in.h', 'IPPROTO_MPTCP'))
if libaio.found()
  config_host_data.set('HAVE_IO_PREP_PWRITEV2',
                       cc.has_header_symbol('libaio.h', 'io_prep_pwritev2'))
endif
if linux_io_uring.found()
  config_host_data.set('HAVE_IO_URING_PREP_WRITEV2',
                       cc.has_header_symbol('liburing.h', 'io_uring_prep_writev2'))
endif
config_host_data.set('HAVE_TCP_KEEPCNT',
                     cc.has_header_symbol('netinet/tcp.h', 'TCP_KEEPCNT') or
                     cc.compiles('''
                     #include <ws2tcpip.h>
                     #ifndef TCP_KEEPCNT
                     #error
                     #endif
                     int main(void) { return 0; }''',
                     name: 'Win32 TCP_KEEPCNT'))
# On Darwin TCP_KEEPIDLE is available under different name, TCP_KEEPALIVE.
# https://github.com/apple/darwin-xnu/blob/xnu-4570.1.46/bsd/man/man4/tcp.4#L172
config_host_data.set('HAVE_TCP_KEEPIDLE',
                     cc.has_header_symbol('netinet/tcp.h', 'TCP_KEEPIDLE') or
                     cc.has_header_symbol('netinet/tcp.h', 'TCP_KEEPALIVE') or
                     cc.compiles('''
                     #include <ws2tcpip.h>
                     #ifndef TCP_KEEPIDLE
                     #error
                     #endif
                     int main(void) { return 0; }''',
                     name: 'Win32 TCP_KEEPIDLE'))
config_host_data.set('HAVE_TCP_KEEPINTVL',
                     cc.has_header_symbol('netinet/tcp.h', 'TCP_KEEPINTVL') or
                     cc.compiles('''
                     #include <ws2tcpip.h>
                     #ifndef TCP_KEEPINTVL
                     #error
                     #endif
                     int main(void) { return 0; }''',
                     name: 'Win32 TCP_KEEPINTVL'))

# has_member
config_host_data.set('HAVE_SIGEV_NOTIFY_THREAD_ID',
                     cc.has_member('struct sigevent', 'sigev_notify_thread_id',
                                   prefix: '#include <signal.h>'))
config_host_data.set('HAVE_STRUCT_STAT_ST_ATIM',
                     cc.has_member('struct stat', 'st_atim',
                                   prefix: '#include <sys/stat.h>'))
config_host_data.set('HAVE_BLK_ZONE_REP_CAPACITY',
                     cc.has_member('struct blk_zone', 'capacity',
                                   prefix: '#include <linux/blkzoned.h>'))

# has_type
config_host_data.set('CONFIG_IOVEC',
                     cc.has_type('struct iovec',
                                 prefix: '#include <sys/uio.h>'))
config_host_data.set('HAVE_UTMPX',
                     cc.has_type('struct utmpx',
                                 prefix: '#include <utmpx.h>'))

config_host_data.set('CONFIG_EVENTFD', cc.links('''
  #include <sys/eventfd.h>
  int main(void) { return eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); }'''))
config_host_data.set('CONFIG_FDATASYNC', cc.links(osdep_prefix + '''
  int main(void) {
  #if defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
  return fdatasync(0);
  #else
  #error Not supported
  #endif
  }'''))

has_madvise = cc.links(osdep_prefix + '''
  #include <sys/mman.h>
  int main(void) { return madvise(NULL, 0, MADV_DONTNEED); }''')
missing_madvise_proto = false
if has_madvise
  # Some platforms (illumos and Solaris before Solaris 11) provide madvise()
  # but forget to prototype it. In this case, has_madvise will be true (the
  # test program links despite a compile warning). To detect the
  # missing-prototype case, we try again with a definitely-bogus prototype.
  # This will only compile if the system headers don't provide the prototype;
  # otherwise the conflicting prototypes will cause a compiler error.
  missing_madvise_proto = cc.links(osdep_prefix + '''
    #include <sys/mman.h>
    extern int madvise(int);
    int main(void) { return madvise(0); }''')
endif
config_host_data.set('CONFIG_MADVISE', has_madvise)
config_host_data.set('HAVE_MADVISE_WITHOUT_PROTOTYPE', missing_madvise_proto)

config_host_data.set('CONFIG_MEMFD', cc.links(osdep_prefix + '''
  #include <sys/mman.h>
  int main(void) { return memfd_create("foo", MFD_ALLOW_SEALING); }'''))
config_host_data.set('CONFIG_OPEN_BY_HANDLE', cc.links(osdep_prefix + '''
  #if !defined(AT_EMPTY_PATH)
  # error missing definition
  #else
  int main(void) { struct file_handle fh; return open_by_handle_at(0, &fh, 0); }
  #endif'''))

# On Darwin posix_madvise() has the same return semantics as plain madvise(),
# i.e. errno is set and -1 is returned. That's not really how POSIX defines the
# function. On the flip side, it has madvise() which is preferred anyways.
if host_os != 'darwin'
  config_host_data.set('CONFIG_POSIX_MADVISE', cc.links(osdep_prefix + '''
    #include <sys/mman.h>
    int main(void) { return posix_madvise(NULL, 0, POSIX_MADV_DONTNEED); }'''))
endif

config_host_data.set('CONFIG_PTHREAD_SETNAME_NP_W_TID', cc.links(osdep_prefix + '''
  #include <pthread.h>

  static void *f(void *p) { return NULL; }
  int main(void)
  {
    pthread_t thread;
    pthread_create(&thread, 0, f, 0);
    pthread_setname_np(thread, "ChefKiss Inferno");
    return 0;
  }''', dependencies: threads))
config_host_data.set('CONFIG_PTHREAD_SETNAME_NP_WO_TID', cc.links(osdep_prefix + '''
  #include <pthread.h>

  static void *f(void *p) { pthread_setname_np("ChefKiss Inferno"); return NULL; }
  int main(void)
  {
    pthread_t thread;
    pthread_create(&thread, 0, f, 0);
    return 0;
  }''', dependencies: threads))
config_host_data.set('CONFIG_PTHREAD_SET_NAME_NP', cc.links(osdep_prefix + '''
  #include <pthread.h>
  #include <pthread_np.h>

  static void *f(void *p) { return NULL; }
  int main(void)
  {
    pthread_t thread;
    pthread_create(&thread, 0, f, 0);
    pthread_set_name_np(thread, "ChefKiss Inferno");
    return 0;
  }''', dependencies: threads))
config_host_data.set('CONFIG_PTHREAD_CONDATTR_SETCLOCK', cc.links(osdep_prefix + '''
  #include <pthread.h>

  int main(void)
  {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    return 0;
  }''', dependencies: threads))
config_host_data.set('CONFIG_PTHREAD_AFFINITY_NP', cc.links(osdep_prefix + '''
  #include <pthread.h>

  static void *f(void *p) { return NULL; }
  int main(void)
  {
    int setsize = CPU_ALLOC_SIZE(64);
    pthread_t thread;
    cpu_set_t *cpuset;
    pthread_create(&thread, 0, f, 0);
    cpuset = CPU_ALLOC(64);
    CPU_ZERO_S(setsize, cpuset);
    pthread_setaffinity_np(thread, setsize, cpuset);
    pthread_getaffinity_np(thread, setsize, cpuset);
    CPU_FREE(cpuset);
    return 0;
  }''', dependencies: threads))
config_host_data.set('CONFIG_SIGNALFD', cc.links(osdep_prefix + '''
  #include <sys/signalfd.h>
  int main(void) { return signalfd(-1, NULL, SFD_CLOEXEC); }'''))
config_host_data.set('CONFIG_SPLICE', cc.links(osdep_prefix + '''
  int main(void)
  {
    int len, fd = 0;
    len = tee(STDIN_FILENO, STDOUT_FILENO, INT_MAX, SPLICE_F_NONBLOCK);
    splice(STDIN_FILENO, NULL, fd, NULL, len, SPLICE_F_MOVE);
    return 0;
  }'''))

config_host_data.set('HAVE_MLOCKALL', cc.links(osdep_prefix + '''
  #include <sys/mman.h>
  int main(void) {
    return mlockall(MCL_FUTURE);
  }'''))

config_host_data.set('HAVE_MLOCK_ONFAULT', cc.links(osdep_prefix + '''
  #include <sys/mman.h>
  int main(void) {
      return mlockall(MCL_FUTURE | MCL_ONFAULT);
  }'''))

have_l2tpv3 = false
if get_option('l2tpv3').allowed() and have_system
  have_l2tpv3 = cc.has_type('struct mmsghdr',
    prefix: osdep_prefix + '''
      #include <sys/socket.h>
      #include <linux/ip.h>''')
endif
config_host_data.set('CONFIG_L2TPV3', have_l2tpv3)

have_netmap = false
if get_option('netmap').allowed() and have_system
  have_netmap = cc.compiles('''
    #include <inttypes.h>
    #include <net/if.h>
    #include <net/netmap.h>
    #include <net/netmap_user.h>
    #if (NETMAP_API < 11) || (NETMAP_API > 15)
    #error
    #endif
    int main(void) { return 0; }''')
  if not have_netmap and get_option('netmap').enabled()
    error('Netmap headers not available')
  endif
endif
config_host_data.set('CONFIG_NETMAP', have_netmap)

# Work around a system header bug with some kernel/XFS header
# versions where they both try to define 'struct fsxattr':
# xfs headers will not try to redefine structs from linux headers
# if this macro is set.
config_host_data.set('HAVE_FSXATTR', cc.links('''
  #include <linux/fs.h>
  struct fsxattr foo;
  int main(void) {
    return 0;
  }'''))

# Some versions of Mac OS X incorrectly define SIZE_MAX
config_host_data.set('HAVE_BROKEN_SIZE_MAX', not cc.compiles('''
    #include <stdint.h>
    #include <stdio.h>
    int main(void) {
        return printf("%zu", SIZE_MAX);
    }''', args: ['-Werror']))

# See if 64-bit atomic operations are supported.
# Note that without __atomic builtins, we can only
# assume atomic loads/stores max at pointer size.
config_host_data.set('CONFIG_ATOMIC64', cc.links('''
  #include <stdint.h>
  int main(void)
  {
    uint64_t x = 0, y = 0;
    y = __atomic_load_n(&x, __ATOMIC_RELAXED);
    __atomic_store_n(&x, y, __ATOMIC_RELAXED);
    __atomic_compare_exchange_n(&x, &y, x, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    __atomic_exchange_n(&x, y, __ATOMIC_RELAXED);
    __atomic_fetch_add(&x, y, __ATOMIC_RELAXED);
    return 0;
  }''', args: qemu_isa_flags))

# has_int128_type is set to false on Emscripten to avoid errors by libffi
# during runtime.
has_int128_type = host_os != 'emscripten' and cc.compiles('''
  __int128_t a;
  __uint128_t b;
  int main(void) { b = a; }''')
config_host_data.set('CONFIG_INT128_TYPE', has_int128_type)

has_int128 = has_int128_type and cc.links('''
  __int128_t a;
  __uint128_t b;
  int main (void) {
    a = a + b;
    b = a * b;
    a = a * a;
    return 0;
  }''')
config_host_data.set('CONFIG_INT128', has_int128)

if has_int128_type
  # "do we have 128-bit atomics which are handled inline and specifically not
  # via libatomic". The reason we can't use libatomic is documented in the
  # comment starting "GCC is a house divided" in include/qemu/atomic128.h.
  # We only care about these operations on 16-byte aligned pointers, so
  # force 16-byte alignment of the pointer, which may be greater than
  # __alignof(unsigned __int128) for the host.
  atomic_test_128 = '''
    int main(int ac, char **av) {
      __uint128_t *p = __builtin_assume_aligned(av[ac - 1], 16);
      p[1] = __atomic_load_n(&p[0], __ATOMIC_RELAXED);
      __atomic_store_n(&p[2], p[3], __ATOMIC_RELAXED);
      __atomic_compare_exchange_n(&p[4], &p[5], p[6], 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
      return 0;
    }'''
  has_atomic128 = cc.links(atomic_test_128, args: qemu_isa_flags)

  config_host_data.set('CONFIG_ATOMIC128', has_atomic128)

  if not has_atomic128
    # Even with __builtin_assume_aligned, the above test may have failed
    # without optimization enabled.  Try again with optimizations locally
    # enabled for the function.  See
    #   https://gcc.gnu.org/bugzilla/show_bug.cgi?id=107389
    has_atomic128_opt = cc.links('__attribute__((optimize("O1")))' + atomic_test_128,
                                 args: qemu_isa_flags)
    config_host_data.set('CONFIG_ATOMIC128_OPT', has_atomic128_opt)

    if not has_atomic128_opt
      config_host_data.set('CONFIG_CMPXCHG128', cc.links('''
        int main(void)
        {
          __uint128_t x = 0, y = 0;
          __sync_val_compare_and_swap_16(&x, y, x);
          return 0;
        }
      ''', args: qemu_isa_flags))
    endif
  endif
endif

config_host_data.set('CONFIG_GETAUXVAL', cc.links(osdep_prefix + '''
  #include <sys/auxv.h>
  int main(void) {
    return getauxval(AT_HWCAP) == 0;
  }'''))

config_host_data.set('CONFIG_ELF_AUX_INFO', cc.links(osdep_prefix + '''
  #include <sys/auxv.h>
  int main(void) {
    unsigned long hwcap = 0;
    elf_aux_info(AT_HWCAP, &hwcap, sizeof(hwcap));
    return hwcap;
  }'''))

have_keyring = get_option('keyring') \
  .require(host_os == 'linux', error_message: 'keyring is only available on Linux') \
  .require(cc.compiles('''
    #include <errno.h>
    #include <asm/unistd.h>
    #include <linux/keyctl.h>
    #include <sys/syscall.h>
    #include <unistd.h>
    int main(void) {
        return syscall(__NR_keyctl, KEYCTL_READ, 0, NULL, NULL, 0);
    }'''), error_message: 'keyctl syscall not available on this system').allowed()
config_host_data.set('CONFIG_SECRET_KEYRING', have_keyring)

have_cpuid_h = cc.links('''
  #include <cpuid.h>
  int main(void) {
    unsigned a, b, c, d;
    unsigned max = __get_cpuid_max(0, 0);

    if (max >= 1) {
        __cpuid(1, a, b, c, d);
    }

    if (max >= 7) {
        __cpuid_count(7, 0, a, b, c, d);
    }

    return 0;
  }''')
config_host_data.set('CONFIG_CPUID_H', have_cpuid_h)

if have_cpuid_h
  have_avx2 = cc.links('''
    #include <immintrin.h>
    static int __attribute__((target("avx2"))) bar(void *a) {
      __m256i x = *(__m256i *)a;
      return _mm256_testz_si256(x, x);
    }
    int main(int argc, char *argv[]) { return bar(argv[argc - 1]); }
  ''')
  have_avx512bw = cc.links('''
    #include <immintrin.h>
    static int __attribute__((target("avx512bw"))) bar(void *a) {
      __m512i *x = a;
      __m512i res= _mm512_abs_epi8(*x);
      return res[1];
    }
    int main(int argc, char *argv[]) { return bar(argv[0]); }
  ''')
  if get_option('x86_version') >= '3' and not have_avx2
    error('Cannot enable AVX optimizations due to missing intrinsics')
  elif get_option('x86_version') >= '4' and not have_avx512bw
    error('Cannot enable AVX512 optimizations due to missing intrinsics')
  endif
else
  have_avx2 = false
  have_avx512bw = false
  if get_option('x86_version') >= '3'
    error('Cannot enable AVX optimizations due to missing cpuid.h')
  endif
endif
config_host_data.set('CONFIG_AVX2_OPT', have_avx2)
config_host_data.set('CONFIG_AVX512BW_OPT', have_avx512bw)

# For both AArch64 and AArch32, detect if builtins are available.
config_host_data.set('CONFIG_ARM_AES_BUILTIN', cc.compiles('''
    #include <arm_neon.h>
    #ifndef __ARM_FEATURE_AES
    __attribute__((target("+crypto")))
    #endif
    void foo(uint8x16_t *p) { *p = vaesmcq_u8(*p); }
  '''))

if get_option('membarrier').disabled()
  have_membarrier = false
elif host_os == 'windows'
  have_membarrier = true
elif host_os == 'linux'
  have_membarrier = cc.compiles('''
    #include <linux/membarrier.h>
    #include <sys/syscall.h>
    #include <unistd.h>
    #include <stdlib.h>
    int main(void) {
        syscall(__NR_membarrier, MEMBARRIER_CMD_QUERY, 0);
        syscall(__NR_membarrier, MEMBARRIER_CMD_SHARED, 0);
        exit(0);
    }''')
endif
config_host_data.set('CONFIG_MEMBARRIER', get_option('membarrier') \
  .require(have_membarrier, error_message: 'membarrier system call not available') \
  .allowed())

config_host_data.set('CONFIG_AF_VSOCK', cc.has_header_symbol(
  'linux/vm_sockets.h', 'AF_VSOCK',
  prefix: '#include <sys/socket.h>',
))

have_vss = false
have_vss_sdk = false # old xp/2003 SDK
if host_os == 'windows' and 'cpp' in all_languages
  have_vss = cxx.compiles('''
    #define __MIDL_user_allocate_free_DEFINED__
    #include <vss.h>
    int main(void) { return VSS_CTX_BACKUP; }''')
  have_vss_sdk = cxx.has_header('vscoordint.h')
endif
config_host_data.set('HAVE_VSS_SDK', have_vss_sdk)

# Older versions of MinGW do not import _lock_file and _unlock_file properly.
# This was fixed for v6.0.0 with commit b48e3ac8969d.
if host_os == 'windows'
  config_host_data.set('HAVE__LOCK_FILE', cc.links('''
    #include <stdio.h>
    int main(void) {
      _lock_file(NULL);
      _unlock_file(NULL);
      return 0;
    }''', name: '_lock_file and _unlock_file'))
endif

if host_os == 'windows'
  mingw_has_setjmp_longjmp = cc.links('''
    #include <setjmp.h>
    int main(void) {
      /*
       * These functions are not available in setjmp header, but may be
       * available at link time, from libmingwex.a.
       */
      extern int __mingw_setjmp(jmp_buf);
      extern void __attribute__((noreturn)) __mingw_longjmp(jmp_buf, int);
      jmp_buf env;
      __mingw_setjmp(env);
      __mingw_longjmp(env, 0);
    }
  ''', name: 'mingw setjmp and longjmp')

  if cpu == 'aarch64' and not mingw_has_setjmp_longjmp
    error('mingw must provide setjmp/longjmp for windows-arm64')
  endif
endif

# Detect host pointer size for the target configuration loop.
host_long_bits = cc.sizeof('void *') * 8

########################
# Target configuration #
########################

minikconf = find_program('scripts/minikconf.py')

config_all_accel = {}
config_all_devices = {}
config_devices_mak_list = []
config_devices_h = {}
config_target_h = {}
config_target_mak = {}
config_base_arch_mak = {}

host_kconfig = \
  (libcbor.found() ? ['CONFIG_LIBCBOR=y'] : []) + \
  (gnutls.found() ? ['CONFIG_GNUTLS=y'] : []) + \
  (x11.found() ? ['CONFIG_X11=y'] : []) + \
  (host_os == 'linux' ? ['CONFIG_LINUX=y'] : [])

ignored = [ 'TARGET_XML_FILES', 'TARGET_ABI_DIR' ]

actual_target_dirs = []
foreach target : target_dirs
  config_target = { 'TARGET_NAME': target.split('-')[0] }
  if target.endswith('softmmu')
    config_target += { 'CONFIG_SYSTEM_ONLY': 'y' }
    config_target += { 'CONFIG_SOFTMMU': 'y' }
  endif

  config_target += keyval.load('configs/targets' / target + '.mak')

  target_kconfig = []
  foreach sym: accelerators
    # Disallow 64-bit on 32-bit emulation and virtualization
    if host_long_bits < config_target['TARGET_LONG_BITS'].to_int()
      continue
    endif
    if sym == 'CONFIG_TCG' or target in accelerator_targets.get(sym, [])
      config_target += { sym: 'y' }
      config_all_accel += { sym: 'y' }
      target_kconfig += [ sym + '=y' ]
    endif
  endforeach
  if target_kconfig.length() == 0
    error('No accelerator available for target @0@'.format(target))
  endif

  actual_target_dirs += target

  # Add default keys
  config_target += { 'TARGET_' + config_target['TARGET_ARCH'].to_upper(): 'y' }
  if 'TARGET_BASE_ARCH' not in config_target
    config_target += {'TARGET_BASE_ARCH': config_target['TARGET_ARCH']}
  endif
  if 'TARGET_ABI_DIR' not in config_target
    config_target += {'TARGET_ABI_DIR': config_target['TARGET_ARCH']}
  endif
  if 'TARGET_BIG_ENDIAN' not in config_target
    config_target += {'TARGET_BIG_ENDIAN': 'n'}
  endif

  config_target_data = configuration_data()
  foreach k, v: config_target
    if not k.startswith('TARGET_') and not k.startswith('CONFIG_')
      # do nothing
    elif ignored.contains(k)
      # do nothing
    elif k == 'TARGET_ARCH'
      config_target_data.set(k, v.to_upper())
    elif k == 'TARGET_BASE_ARCH'
      # Note that TARGET_BASE_ARCH ends up in config-target.h but it is
      # not used to select files from sourcesets.
      config_target_data.set('TARGET_' + v.to_upper(), 1)
    elif k == 'TARGET_NAME' or k == 'CONFIG_QEMU_INTERP_PREFIX'
      config_target_data.set_quoted(k, v)
    elif v == 'y'
      config_target_data.set(k, 1)
    elif v == 'n'
      config_target_data.set(k, 0)
    else
      config_target_data.set(k, v)
    endif
  endforeach
  config_target_data.set('QEMU_ARCH',
                         'QEMU_ARCH_' + config_target['TARGET_BASE_ARCH'].to_upper())
  config_target_h += {target: configure_file(output: target + '-config-target.h',
                                               configuration: config_target_data)}

  if target.endswith('-softmmu')
    target_kconfig += 'CONFIG_' + config_target['TARGET_ARCH'].to_upper() + '=y'
    target_kconfig += 'CONFIG_TARGET_BIG_ENDIAN=' + config_target['TARGET_BIG_ENDIAN']

    config_input = meson.get_external_property(target, 'default')
    config_devices_mak = target + '-config-devices.mak'
    config_devices_mak = configure_file(
      input: ['configs/devices' / target / config_input + '.mak', 'Kconfig'],
      output: config_devices_mak,
      depfile: config_devices_mak + '.d',
      capture: true,
      command: [minikconf,
                get_option('default_devices') ? '--defconfig' : '--allnoconfig',
                config_devices_mak, '@DEPFILE@', '@INPUT@',
                host_kconfig, target_kconfig])

    config_devices_data = configuration_data()
    config_devices = keyval.load(config_devices_mak)
    foreach k, v: config_devices
      config_devices_data.set(k, 1)
    endforeach
    config_devices_mak_list += config_devices_mak
    config_devices_h += {target: configure_file(output: target + '-config-devices.h',
                                                configuration: config_devices_data)}
    config_target += config_devices
    config_all_devices += config_devices
  endif
  config_target_mak += {target: config_target}

  # build a merged config for all targets with the same TARGET_BASE_ARCH
  target_base_arch = config_target['TARGET_BASE_ARCH']
  config_base_arch = config_base_arch_mak.get(target_base_arch, {}) + config_target
  config_base_arch_mak += {target_base_arch: config_base_arch}
endforeach
target_dirs = actual_target_dirs

target_configs_h = []
foreach target: target_dirs
  target_configs_h += config_target_h[target]
  target_configs_h += config_devices_h.get(target, [])
endforeach
genh += custom_target('config-poison.h',
                      input: [target_configs_h],
                      output: 'config-poison.h',
                      capture: true,
                      command: [find_program('scripts/make-config-poison.sh'),
                                target_configs_h])

#####################
# Generated sources #
#####################

config_host_h = configure_file(output: 'config-host.h', configuration: config_host_data)
genh += config_host_h

hxtool = find_program('scripts/hxtool')
qapi_gen = find_program('scripts/qapi-gen.py')
qapi_gen_depends = [ meson.current_source_dir() / 'scripts/qapi/__init__.py',
                     meson.current_source_dir() / 'scripts/qapi/commands.py',
                     meson.current_source_dir() / 'scripts/qapi/common.py',
                     meson.current_source_dir() / 'scripts/qapi/error.py',
                     meson.current_source_dir() / 'scripts/qapi/events.py',
                     meson.current_source_dir() / 'scripts/qapi/expr.py',
                     meson.current_source_dir() / 'scripts/qapi/gen.py',
                     meson.current_source_dir() / 'scripts/qapi/introspect.py',
                     meson.current_source_dir() / 'scripts/qapi/main.py',
                     meson.current_source_dir() / 'scripts/qapi/parser.py',
                     meson.current_source_dir() / 'scripts/qapi/schema.py',
                     meson.current_source_dir() / 'scripts/qapi/source.py',
                     meson.current_source_dir() / 'scripts/qapi/types.py',
                     meson.current_source_dir() / 'scripts/qapi/features.py',
                     meson.current_source_dir() / 'scripts/qapi/visit.py',
                     meson.current_source_dir() / 'scripts/qapi-gen.py'
]

tracetool = [
  python, files('scripts/tracetool.py'),
   '--backend=' + ','.join(get_option('trace_backends'))
]
tracetool_depends = files(
  'scripts/tracetool/backend/log.py',
  'scripts/tracetool/backend/__init__.py',
  'scripts/tracetool/backend/dtrace.py',
  'scripts/tracetool/backend/ftrace.py',
  'scripts/tracetool/backend/simple.py',
  'scripts/tracetool/backend/syslog.py',
  'scripts/tracetool/backend/ust.py',
  'scripts/tracetool/format/ust_events_c.py',
  'scripts/tracetool/format/ust_events_h.py',
  'scripts/tracetool/format/__init__.py',
  'scripts/tracetool/format/d.py',
  'scripts/tracetool/format/simpletrace_stap.py',
  'scripts/tracetool/format/c.py',
  'scripts/tracetool/format/h.py',
  'scripts/tracetool/format/log_stap.py',
  'scripts/tracetool/format/stap.py',
  'scripts/tracetool/__init__.py',
)

qemu_version_cmd = [find_program('scripts/qemu-version.sh'),
                    meson.current_source_dir(),
                    get_option('pkgversion'), meson.project_version()]
qemu_version = custom_target('qemu-version.h',
                             output: 'qemu-version.h',
                             command: qemu_version_cmd,
                             capture: true,
                             build_by_default: true,
                             build_always_stale: true)
genh += qemu_version

hxdep = []
hx_headers = [
  ['qemu-options.hx', 'qemu-options.def'],
  ['qemu-img-cmds.hx', 'qemu-img-cmds.h'],
]
if have_system
  hx_headers += [
    ['hmp-commands.hx', 'hmp-commands.h'],
    ['hmp-commands-info.hx', 'hmp-commands-info.h'],
  ]
endif
foreach d : hx_headers
  hxdep += custom_target(d[1],
                input: files(d[0]),
                output: d[1],
                capture: true,
                command: [hxtool, '-h', '@INPUT0@'])
endforeach
genh += hxdep

###############
# Trace files #
###############

# TODO: add each directory to the subdirs from its own meson.build, once
# we have those
trace_events_subdirs = [
  'crypto',
  'qapi',
  'qom',
  'monitor',
  'util',
  'gdbstub',
]
if have_block
  trace_events_subdirs += [
    'authz',
    'block',
    'chardev',
    'io',
  ]
endif
if have_system
  trace_events_subdirs += [
    'accel/hvf',
    'accel/kvm',
    'audio',
    'backends',
    'hw/arm',
    'hw/arm/sep',
    'hw/audio',
    'hw/block',
    'hw/char',
    'hw/display',
    'hw/dma',
    'hw/i2c',
    'hw/input',
    'hw/intc',
    'hw/misc',
    'hw/misc/a7iop',
    'hw/misc/a7iop/mailbox',
    'hw/net',
    'hw/nvme',
    'hw/nvram',
    'hw/pci',
    'hw/pci-host',
    'hw/rtc',
    'hw/sd',
    'hw/sensor',
    'hw/spmi',
    'hw/ssi',
    'hw/timer',
    'hw/usb',
    'hw/watchdog',
    'hw/gpio',
    'net',
    'system',
    'ui',
  ]
endif
if have_system
  trace_events_subdirs += [
    'accel/tcg',
    'hw/core',
    'target/arm',
    'target/arm/hvf',
  ]
endif

###################
# Collect sources #
###################

authz_ss = ss.source_set()
blockdev_ss = ss.source_set()
block_ss = ss.source_set()
chardev_ss = ss.source_set()
common_ss = ss.source_set()
crypto_ss = ss.source_set()
hwcore_ss = ss.source_set()
io_ss = ss.source_set()
qmp_ss = ss.source_set()
qom_ss = ss.source_set()
system_ss = ss.source_set()
specific_ss = ss.source_set()
stub_ss = ss.source_set()
trace_ss = ss.source_set()
util_ss = ss.source_set()

modules = {}
target_modules = {}
hw_arch = {}
target_arch = {}
target_system_arch = {}
target_user_arch = {}
hw_common_arch = {}
target_common_arch = {}
target_common_system_arch = {}

# NOTE: the trace/ subdirectory needs the qapi_trace_events variable
# that is filled in by qapi/.
subdir('qapi')
subdir('qobject')
subdir('stubs')
subdir('trace')
subdir('util')
subdir('qom')
subdir('authz')
subdir('crypto')
subdir('ui')
subdir('gdbstub')
if have_system
  subdir('hw')
else
  subdir('hw/core')
endif

qom_ss = qom_ss.apply({})
libqom = static_library('qom', qom_ss.sources() + genh,
                        dependencies: [qom_ss.dependencies()],
                        build_by_default: false)
qom = declare_dependency(objects: libqom.extract_all_objects(recursive: false),
                         dependencies: qom_ss.dependencies())

event_loop_base = files('event-loop-base.c')
event_loop_base = static_library('event-loop-base',
                                 sources: event_loop_base + genh,
                                 build_by_default: false)
event_loop_base = declare_dependency(objects: event_loop_base.extract_all_objects(recursive: false),
                                     dependencies: [qom])

stub_ss = stub_ss.apply({})

util_ss.add_all(trace_ss)
util_ss = util_ss.apply({})
libqemuutil = static_library('qemuutil',
                             build_by_default: false,
                             sources: util_ss.sources() + stub_ss.sources() + genh,
                             dependencies: [util_ss.dependencies(), libm, threads, glib, socket, malloc])
qemuutil_deps = [event_loop_base]
if host_os != 'windows'
  qemuutil_deps += [rt]
endif
qemuutil = declare_dependency(link_with: libqemuutil,
                              sources: genh + version_res,
                              dependencies: qemuutil_deps)

if have_system
  decodetree = generator(find_program('scripts/decodetree.py'),
                         output: 'decode-@BASENAME@.c.inc',
                         arguments: ['@INPUT@', '@EXTRA_ARGS@', '-o', '@OUTPUT@'])
  subdir('target')
endif

subdir('audio')
subdir('io')
subdir('chardev')

if have_block
  block_ss.add(files(
    'block.c',
    'blockjob.c',
    'job.c',
  ))

  subdir('block')

  blockdev_ss.add(files(
    'blockdev.c',
    'iothread.c',
    'job-qmp.c',
  ))

  if host_os == 'windows'
    system_ss.add(files('os-win32.c'))
  elif host_os == 'emscripten'
    blockdev_ss.add(files('os-wasm.c'))
  else
    blockdev_ss.add(files('os-posix.c'))
  endif
endif

common_ss.add(files('cpu-common.c'))
system_ss.add(files('cpu-target.c'))

subdir('system')

# Work around a gcc bug/misfeature wherein constant propagation looks
# through an alias:
#   https://gcc.gnu.org/bugzilla/show_bug.cgi?id=99696
# to guess that a const variable is always zero.  Without lto, this is
# impossible, as the alias is restricted to page-vary-common.c.  Indeed,
# without lto, not even the alias is required -- we simply use different
# declarations in different compilation units.
pagevary = files('page-vary-common.c')
if get_option('b_lto')
  pagevary_flags = ['-fno-lto']
  if get_option('cfi')
    pagevary_flags += '-fno-sanitize=cfi-icall'
  endif
  pagevary = static_library('page-vary-common', sources: pagevary + genh,
                            c_args: pagevary_flags)
  pagevary = declare_dependency(link_with: pagevary)
endif
common_ss.add(pagevary)
specific_ss.add(files('page-vary-target.c'))

common_ss.add(files('target-info.c'))
specific_ss.add(files('target-info-stub.c'))

subdir('backends')
subdir('monitor')
subdir('net')
subdir('stats')
subdir('tcg')
subdir('fpu')
subdir('accel')

##############################################
# Internal static_libraries and dependencies #
##############################################

modinfo_collect = find_program('scripts/modinfo-collect.py')
modinfo_generate = find_program('scripts/modinfo-generate.py')
modinfo_files = []

block_mods = []
system_mods = []
emulator_modules = []
foreach d, list : modules
  if not (d == 'block' ? have_block : have_system)
    continue
  endif

  foreach m, module_ss : list
    if d == 'block'
      block_ss.add_all(module_ss)
    else
      system_ss.add_all(module_ss)
    endif
  endforeach
endforeach

foreach d, list : target_modules
  foreach m, module_ss : list
    specific_ss.add_all(module_ss)
  endforeach
endforeach

nm = find_program('nm')
undefsym = find_program('scripts/undefsym.py')
block_syms = custom_target('block.syms', output: 'block.syms',
                             input: [libqemuutil, block_mods],
                             capture: true,
                             command: [undefsym, nm, '@INPUT@'])
qemu_syms = custom_target('qemu.syms', output: 'qemu.syms',
                             input: [libqemuutil, system_mods],
                             capture: true,
                             command: [undefsym, nm, '@INPUT@'])

authz_ss = authz_ss.apply({})
libauthz = static_library('authz', authz_ss.sources() + genh,
                          dependencies: [authz_ss.dependencies()],
                          build_by_default: false)

authz = declare_dependency(objects: libauthz.extract_all_objects(recursive: false),
                           dependencies: [authz_ss.dependencies(), qom])

crypto_ss = crypto_ss.apply({})
libcrypto = static_library('crypto', crypto_ss.sources() + genh,
                           dependencies: [crypto_ss.dependencies()],
                           build_by_default: false)

crypto = declare_dependency(objects: libcrypto.extract_all_objects(recursive: false),
                            dependencies: [crypto_ss.dependencies(), authz, qom])

io_ss = io_ss.apply({})
libio = static_library('io', io_ss.sources() + genh,
                       dependencies: [io_ss.dependencies()],
                       link_with: libqemuutil,
                       build_by_default: false)

io = declare_dependency(objects: libio.extract_all_objects(recursive: false),
                        dependencies: [io_ss.dependencies(), crypto, qom])

block_ss = block_ss.apply({})
libblock = static_library('block', block_ss.sources() + genh,
                          dependencies: block_ss.dependencies(),
                          build_by_default: false)

block = declare_dependency(objects: libblock.extract_all_objects(recursive: false),
                           dependencies: [block_ss.dependencies(), crypto, io])

blockdev_ss = blockdev_ss.apply({})
libblockdev = static_library('blockdev', blockdev_ss.sources() + genh,
                             dependencies: blockdev_ss.dependencies(),
                             build_by_default: false)

blockdev = declare_dependency(objects: libblockdev.extract_all_objects(recursive: false),
                              dependencies: [blockdev_ss.dependencies(), block, event_loop_base])

qmp_ss = qmp_ss.apply({})
libqmp = static_library('qmp', qmp_ss.sources() + genh,
                        dependencies: qmp_ss.dependencies(),
                        build_by_default: false)

qmp = declare_dependency(objects: libqmp.extract_all_objects(recursive: false),
                         dependencies: qmp_ss.dependencies())

libchardev = static_library('chardev', chardev_ss.sources() + genh,
                            dependencies: chardev_ss.dependencies(),
                            build_by_default: false)

chardev = declare_dependency(objects: libchardev.extract_all_objects(recursive: false),
                             dependencies: [chardev_ss.dependencies(), io])

hwcore_ss = hwcore_ss.apply({})
libhwcore = static_library('hwcore', sources: hwcore_ss.sources() + genh,
                           build_by_default: false)
hwcore = declare_dependency(objects: libhwcore.extract_all_objects(recursive: false))
common_ss.add(hwcore)

###########
# Targets #
###########

system_ss.add(authz, blockdev, chardev, crypto, io, qmp)
common_ss.add(qom, qemuutil)

libsystem = static_library('system',
                           system_ss.all_sources() + genh,
                           c_args: ['-DCONFIG_SOFTMMU'],
                           dependencies: system_ss.all_dependencies(),
                           build_by_default: false)

# Note that this library is never used directly (only through extract_objects)
# and is not built by default; therefore, source files not used by the build
# configuration will be in build.ninja, but are never built by default.
common_all = static_library('common',
                            build_by_default: false,
                            sources: common_ss.all_sources() + genh,
                            implicit_include_directories: false,
                            dependencies: common_ss.all_dependencies())

# construct common libraries per base architecture
target_common_arch_libs = {}
target_common_system_arch_libs = {}
foreach target_base_arch, config_base_arch : config_base_arch_mak
  target_inc = [include_directories('target' / target_base_arch)]
  inc = [target_inc]

  target_common = common_ss.apply(config_base_arch, strict: false)
  target_system = system_ss.apply(config_base_arch, strict: false)
  common_deps = []
  system_deps = []
  foreach dep: target_common.dependencies()
    common_deps += dep.partial_dependency(compile_args: true, includes: true)
  endforeach
  foreach dep: target_system.dependencies()
    system_deps += dep.partial_dependency(compile_args: true, includes: true)
  endforeach

  # prevent common code to access cpu compile time definition,
  # but still allow access to cpu.h
  target_c_args = ['-DCPU_DEFS_H']
  target_system_c_args = target_c_args + ['-DCONFIG_SOFTMMU']

  if target_base_arch in target_common_arch
    src = target_common_arch[target_base_arch]
    lib = static_library(
      'common_' + target_base_arch,
      build_by_default: false,
      sources: src.all_sources() + genh,
      include_directories: inc,
      c_args: target_c_args,
      dependencies: src.all_dependencies() + common_deps +
                    system_deps)
    target_common_arch_libs += {target_base_arch: lib}
  endif

  # merge hw_common_arch in target_common_system_arch
  if target_base_arch in hw_common_arch
    hw_src = hw_common_arch[target_base_arch]
    if target_base_arch in target_common_system_arch
      target_common_system_arch[target_base_arch].add_all(hw_src)
    else
      target_common_system_arch += {target_base_arch: hw_src}
    endif
  endif

  if target_base_arch in target_common_system_arch
    src = target_common_system_arch[target_base_arch]
    lib = static_library(
      'system_' + target_base_arch,
      build_by_default: false,
      sources: src.all_sources() + genh,
      include_directories: inc,
      c_args: target_system_c_args,
      dependencies: src.all_dependencies() + common_deps + system_deps)
    target_common_system_arch_libs += {target_base_arch: lib}
  endif
endforeach

feature_to_c = find_program('scripts/feature_to_c.py')

if host_os == 'darwin'
  entitlement = find_program('scripts/entitlement.sh')
endif

traceable = []
emulators = {}
foreach target : target_dirs
  config_target = config_target_mak[target]
  target_name = config_target['TARGET_NAME']
  target_base_arch = config_target['TARGET_BASE_ARCH']
  arch_srcs = [config_target_h[target]]
  arch_deps = []
  c_args = ['-DCOMPILING_PER_TARGET',
            '-DCONFIG_TARGET="@0@-config-target.h"'.format(target),
  ]
  link_args = emulator_link_args

  target_inc = [include_directories('target' / config_target['TARGET_BASE_ARCH'])]
  if host_os == 'linux'
    target_inc += include_directories('linux-headers', is_system: true)
  endif

  t = target_system_arch[target_base_arch].apply(config_target, strict: false)
  arch_srcs += t.sources()
  arch_deps += t.dependencies()

  hw_dir = target_name == 'sparc64' ? 'sparc64' : target_base_arch
  if hw_arch.has_key(hw_dir)
    hw = hw_arch[hw_dir].apply(config_target, strict: false)
    arch_srcs += hw.sources()
    arch_deps += hw.dependencies()
  endif

  c_args += ['-DCONFIG_DEVICES="@0@-config-devices.h"'.format(target)]
  arch_srcs += config_devices_h[target]
  link_args += ['@block.syms', '@qemu.syms']

  if 'TARGET_XML_FILES' in config_target
    gdbstub_xml = custom_target(target + '-gdbstub-xml.c',
                                output: target + '-gdbstub-xml.c',
                                input: files(config_target['TARGET_XML_FILES'].split()),
                                command: [feature_to_c, '@INPUT@'],
                                capture: true)
    arch_srcs += gdbstub_xml
  endif

  t = target_arch[target_base_arch].apply(config_target, strict: false)
  arch_srcs += t.sources()
  arch_deps += t.dependencies()

  target_common = common_ss.apply(config_target, strict: false)
  objects = [common_all.extract_objects(target_common.sources())]
  arch_deps += target_common.dependencies()
  if target_base_arch in target_common_arch_libs
    src = target_common_arch[target_base_arch].apply(config_target, strict: false)
    lib = target_common_arch_libs[target_base_arch]
    objects += lib.extract_objects(src.sources())
    arch_deps += src.dependencies()
  endif

  src = system_ss.apply(config_target, strict: false)
  objects += libsystem.extract_objects(src.sources())
  arch_deps += src.dependencies()
  if target_base_arch in target_common_system_arch_libs
    src = target_common_system_arch[target_base_arch].apply(config_target, strict: false)
    lib = target_common_system_arch_libs[target_base_arch]
    objects += lib.extract_objects(src.sources())
    arch_deps += src.dependencies()
  endif

  target_specific = specific_ss.apply(config_target, strict: false)
  arch_srcs += target_specific.sources()
  arch_deps += target_specific.dependencies()

  # allow using headers from the dependencies but do not include the sources,
  # because this emulator only needs those in "objects".  For external
  # dependencies, the full dependency is included below in the executable.
  lib_deps = []
  foreach dep : arch_deps
    lib_deps += dep.partial_dependency(compile_args: true, includes: true)
  endforeach

  lib = static_library('qemu-' + target,
                 sources: arch_srcs + genh,
                 dependencies: lib_deps,
                 objects: objects,
                 include_directories: target_inc,
                 c_args: c_args,
                 build_by_default: false)

  if target.endswith('-softmmu')
    execs = [{
      'name': 'qemu-system-' + target_name,
      'win_subsystem': 'console',
      'sources': files('system/main.c'),
      'dependencies': [sdl]
    }]
    if host_os == 'windows' and (sdl.found() or gtk.found())
      execs += [{
        'name': 'qemu-system-' + target_name + 'w',
        'win_subsystem': 'windows',
        'sources': files('system/main.c'),
        'dependencies': [sdl]
      }]
    endif
  else
    execs = [{
      'name': 'qemu-' + target_name,
      'win_subsystem': 'console',
      'sources': [],
      'dependencies': []
    }]
  endif
  foreach exe: execs
    exe_name = exe['name']
    if host_os == 'darwin'
      exe_name += '-unsigned'
    endif

    emulator = executable(exe_name, exe['sources'],
               install: true,
               c_args: c_args,
               dependencies: arch_deps + exe['dependencies'],
               objects: lib.extract_all_objects(recursive: true),
               link_depends: [block_syms, qemu_syms],
               link_args: link_args,
               win_subsystem: exe['win_subsystem'])

    if host_os == 'darwin'
      icon = 'pc-bios/qemu.rsrc'
      build_input = [emulator, files(icon)]
      install_input = [
        get_option('bindir') / exe_name,
        meson.current_source_dir() / icon
      ]
      if 'CONFIG_HVF' in config_target
        entitlements = 'accel/hvf/entitlements.plist'
        build_input += files(entitlements)
        install_input += meson.current_source_dir() / entitlements
      endif

      emulators += {exe['name'] : custom_target(exe['name'],
                   input: build_input,
                   output: exe['name'],
                   command: [entitlement, '@OUTPUT@', '@INPUT@'],
                   build_by_default : true)
      }

      meson.add_install_script(entitlement, '--install',
                               get_option('bindir') / exe['name'],
                               install_input)
    else
      emulators += {exe['name']: emulator}
    endif

    traceable += [{
      'exe': exe['name'],
      'probe-prefix': 'qemu.system.' + target_name,
    }]

  endforeach
endforeach

# Other build targets

# Don't build qemu-keymap if xkbcommon is not explicitly enabled
# when we don't build tools or system
if xkbcommon.found()
  # used for the update-keymaps target, so include rules even if !have_tools
  qemu_keymap = executable('qemu-keymap', files('qemu-keymap.c', 'ui/input-keymap.c') + genh,
                           dependencies: [qemuutil, xkbcommon], install: have_tools)
endif

if have_tools
  qemu_img = executable('qemu-img', [files('qemu-img.c'), hxdep],
             link_args: '@block.syms', link_depends: block_syms,
             dependencies: [authz, block, crypto, io, qom, qemuutil], install: true)

  foreach exe: [ 'qemu-img' ]
    traceable += [{
      'exe': exe,
      'probe-prefix': 'qemu.' + exe.substring(5).replace('-', '_')
    }]
  endforeach

  executable('qemu-edid', files('qemu-edid.c', 'hw/display/edid-generate.c'),
             dependencies: [qemuutil, rt],
             install: true)

  if host_os == 'linux'
    executable('qemu-bridge-helper', files('qemu-bridge-helper.c'),
               dependencies: [qemuutil, libcap_ng],
               install: true,
               install_dir: get_option('libexecdir'))
  endif
endif

if stap.found()
  foreach t: traceable
    foreach stp: [
      {'ext': '.stp-build', 'fmt': 'stap', 'bin': meson.current_build_dir() / t['exe'], 'install': false},
      {'ext': '.stp', 'fmt': 'stap', 'bin': get_option('prefix') / get_option('bindir') / t['exe'], 'install': true},
      {'ext': '-simpletrace.stp', 'fmt': 'simpletrace-stap', 'bin': '', 'install': true},
      {'ext': '-log.stp', 'fmt': 'log-stap', 'bin': '', 'install': true},
    ]
      cmd = [
        tracetool, '--group=all', '--format=' + stp['fmt'],
        '--binary=' + stp['bin'],
        '--probe-prefix=' + t['probe-prefix'],
        '@INPUT@', '@OUTPUT@'
      ]

      custom_target(t['exe'] + stp['ext'],
                    input: trace_events_all,
                    output: t['exe'] + stp['ext'],
                    install: stp['install'],
                    install_dir: get_option('datadir') / 'systemtap/tapset',
                    command: cmd,
                    depend_files: tracetool_depends)
    endforeach
  endforeach
endif

subdir('scripts')
if gtk.found()
  subdir('po')
endif

if host_machine.system() == 'windows'
  nsis_cmd = [
    find_program('scripts/nsis.py'),
    '@OUTPUT@',
    get_option('prefix'),
    meson.current_source_dir(),
    glib_pc.get_variable('bindir'),
    host_machine.cpu(),
    '--',
    '-DDISPLAYVERSION=' + meson.project_version(),
  ]
  if gtk.found()
    nsis_cmd += '-DCONFIG_GTK=y'
  endif

  nsis = custom_target('nsis',
                       output: 'qemu-setup-' + meson.project_version() + '.exe',
                       input: files('qemu.nsi'),
                       build_always_stale: true,
                       command: nsis_cmd + ['@INPUT@'])
  alias_target('installer', nsis)
endif

#########################
# Configuration summary #
#########################

# Build environment
summary_info = {}
summary_info += {'Build directory':   meson.current_build_dir()}
summary_info += {'Source path':       meson.current_source_dir()}
summary_info += {'Download dependencies': get_option('wrap_mode') != 'nodownload'}
summary(summary_info, bool_yn: true, section: 'Build environment')

# Directories
summary_info += {'Install prefix':    get_option('prefix')}
summary_info += {'BIOS directory':    qemu_datadir}
pathsep = host_os == 'windows' ? ';' : ':'
summary_info += {'binary directory':  get_option('prefix') / get_option('bindir')}
summary_info += {'library directory': get_option('prefix') / get_option('libdir')}
summary_info += {'libexec directory': get_option('prefix') / get_option('libexecdir')}
summary_info += {'include directory': get_option('prefix') / get_option('includedir')}
summary_info += {'config directory':  get_option('prefix') / get_option('sysconfdir')}
if host_os != 'windows'
  summary_info += {'local state directory': get_option('prefix') / get_option('localstatedir')}
  summary_info += {'Manual directory':      get_option('prefix') / get_option('mandir')}
else
  summary_info += {'local state directory': 'queried at runtime'}
endif
summary(summary_info, bool_yn: true, section: 'Directories')

# Host binaries
summary_info = {}

if slirp.found() and have_system
  summary_info += {'smbd':            have_slirp_smbd ? smbd_path : false}
endif
summary(summary_info, bool_yn: true, section: 'Host binaries')

# Configurable features
summary_info = {}
summary_info += {'system-mode emulation': have_system}
summary_info += {'block layer':       have_block}
if have_system
  summary_info += {'Audio drivers':     ' '.join(audio_drivers_selected)}
endif
summary_info += {'Trace backends':    ','.join(get_option('trace_backends'))}
if 'simple' in get_option('trace_backends')
  summary_info += {'Trace output file': get_option('trace_file') + '-<pid>'}
endif
summary_info += {'QOM debugging':     get_option('qom_cast_debug')}
summary_info += {'Relocatable install': get_option('relocatable')}
summary(summary_info, bool_yn: true, section: 'Configurable features')

# Compilation information
summary_info = {}
summary_info += {'host CPU':          cpu}
summary_info += {'host endianness':   build_machine.endian()}
summary_info += {'C compiler':        ' '.join(meson.get_compiler('c').cmd_array())}
summary_info += {'Host C compiler':   ' '.join(meson.get_compiler('c', native: true).cmd_array())}
if 'cpp' in all_languages
  summary_info += {'C++ compiler':    ' '.join(meson.get_compiler('cpp').cmd_array())}
else
  summary_info += {'C++ compiler':      false}
endif
if 'objc' in all_languages
  summary_info += {'Objective-C compiler': ' '.join(meson.get_compiler('objc').cmd_array())}
else
  summary_info += {'Objective-C compiler': false}
endif
option_cflags = (get_option('debug') ? ['-g'] : [])
if get_option('optimization') != 'plain'
  option_cflags += ['-O' + get_option('optimization')]
endif
summary_info += {'CFLAGS':            ' '.join(get_option('c_args') + option_cflags)}
if 'cpp' in all_languages
  summary_info += {'CXXFLAGS':        ' '.join(get_option('cpp_args') + option_cflags)}
endif
if 'objc' in all_languages
  summary_info += {'OBJCFLAGS':       ' '.join(get_option('objc_args') + option_cflags)}
endif
link_args = get_option('c_link_args')
if link_args.length() > 0
  summary_info += {'LDFLAGS':         ' '.join(link_args)}
endif
summary_info += {'QEMU_CFLAGS':       ' '.join(qemu_common_flags + qemu_cflags)}
if 'cpp' in all_languages
  summary_info += {'QEMU_CXXFLAGS':     ' '.join(qemu_common_flags + qemu_cxxflags)}
endif
if 'objc' in all_languages
  summary_info += {'QEMU_OBJCFLAGS':    ' '.join(qemu_common_flags)}
endif
summary_info += {'QEMU_LDFLAGS':      ' '.join(qemu_ldflags)}
summary_info += {'link-time optimization (LTO)': get_option('b_lto')}
summary_info += {'PIE':               get_option('b_pie')}
summary_info += {'static build':      get_option('prefer_static')}
summary_info += {'malloc trim support': has_malloc_trim}
summary_info += {'membarrier':        have_membarrier}
summary_info += {'debug graph lock':  get_option('debug_graph_lock')}
summary_info += {'debug stack usage': get_option('debug_stack_usage')}
summary_info += {'mutex debugging':   get_option('debug_mutex')}
summary_info += {'memory allocator':  get_option('malloc')}
summary_info += {'avx2 optimization': config_host_data.get('CONFIG_AVX2_OPT')}
summary_info += {'avx512bw optimization': config_host_data.get('CONFIG_AVX512BW_OPT')}
summary_info += {'gcov':              get_option('b_coverage')}
summary_info += {'thread sanitizer':  get_option('tsan')}
summary_info += {'CFI support':       get_option('cfi')}
if get_option('cfi')
  summary_info += {'CFI debug support': get_option('cfi_debug')}
endif
summary_info += {'strip binaries':    get_option('strip')}
summary_info += {'sparse':            sparse}
summary_info += {'mingw32 support':   host_os == 'windows'}
summary(summary_info, bool_yn: true, section: 'Compilation')

# Targets and accelerators
summary_info = {}
if have_system
  summary_info += {'KVM support':       config_all_accel.has_key('CONFIG_KVM')}
  summary_info += {'HVF support':       config_all_accel.has_key('CONFIG_HVF')}
  summary_info += {'WHPX support':      config_all_accel.has_key('CONFIG_WHPX')}
endif
summary_info += {'TCG support':       config_all_accel.has_key('CONFIG_TCG')}
if config_all_accel.has_key('CONFIG_TCG')
  if get_option('tcg_interpreter')
    summary_info += {'TCG backend':   'TCI (TCG with bytecode interpreter, slow)'}
  else
    summary_info += {'TCG backend':   'native (@0@)'.format(cpu)}
  endif
  summary_info += {'TCG debug enabled': get_option('debug_tcg')}
endif
summary_info += {'target list':       ' '.join(target_dirs)}
if have_system
  summary_info += {'default devices':   get_option('default_devices')}
  summary_info += {'out of process emulation': multiprocess_allowed}
endif
summary(summary_info, bool_yn: true, section: 'Targets and accelerators')

# Block layer
summary_info = {}
summary_info += {'coroutine backend': coroutine_backend}
summary_info += {'coroutine pool':    have_coroutine_pool}
if have_block
  summary_info += {'Block whitelist (rw)': get_option('block_drv_rw_whitelist')}
  summary_info += {'Block whitelist (ro)': get_option('block_drv_ro_whitelist')}
  summary_info += {'Use block whitelist in tools': get_option('block_drv_whitelist_in_tools')}
  summary_info += {'dmg support':       get_option('dmg').allowed()}
  summary_info += {'FUSE exports':      fuse}
endif
summary(summary_info, bool_yn: true, section: 'Block layer support')

# Crypto
summary_info = {}
summary_info += {'TLS priority':      get_option('tls_priority')}
summary_info += {'GNUTLS support':    gnutls}
if nettle.found()
   summary_info += {'  XTS':             xts != 'private'}
endif
summary_info += {'rng-none':          get_option('rng_none')}
summary_info += {'Linux keyring':     have_keyring}
summary_info += {'Linux keyutils':    keyutils}
summary(summary_info, bool_yn: true, section: 'Crypto')

# UI
summary_info = {}
if host_os == 'darwin'
  summary_info += {'Cocoa support':           cocoa}
endif
summary_info += {'SDL support':       sdl}
summary_info += {'SDL image support': sdl_image}
summary_info += {'GTK support':       gtk}
summary_info += {'VTE support':       vte}
summary_info += {'PNG support':       png}
summary_info += {'VNC support':       vnc}
if vnc.found()
  summary_info += {'VNC SASL support':  sasl}
  summary_info += {'VNC JPEG support':  jpeg}
endif
summary_info += {'curses support':    curses}
summary_info += {'brlapi support':    brlapi}
summary(summary_info, bool_yn: true, section: 'User interface')

# Audio backends
summary_info = {}
if host_os not in ['darwin', 'haiku', 'windows']
  summary_info += {'OSS support':     oss}
  summary_info += {'sndio support':   sndio}
elif host_os == 'darwin'
  summary_info += {'CoreAudio support': coreaudio}
elif host_os == 'windows'
  summary_info += {'DirectSound support': dsound}
endif
if host_os == 'linux'
  summary_info += {'ALSA support':    alsa}
  summary_info += {'PulseAudio support': pulse}
endif
summary_info += {'PipeWire support':  pipewire}
summary_info += {'JACK support':      jack}
summary(summary_info, bool_yn: true, section: 'Audio backends')

# Network backends
summary_info = {}
if host_os == 'darwin'
  summary_info += {'vmnet.framework support': vmnet}
endif
summary_info += {'slirp support':     slirp}
summary_info += {'vde support':       vde}
summary_info += {'netmap support':    have_netmap}
summary_info += {'l2tpv3 support':    have_l2tpv3}
summary(summary_info, bool_yn: true, section: 'Network backends')

# Libraries
summary_info = {}
summary_info += {'libtasn1':          tasn1}
summary_info += {'PAM':               pam}
summary_info += {'iconv support':     iconv}
summary_info += {'blkio support':     blkio}
summary_info += {'curl support':      curl}
summary_info += {'Multipath support': mpathpersist}
summary_info += {'Linux AIO support': libaio}
summary_info += {'Linux io_uring support': linux_io_uring}
summary_info += {'libcap-ng support': libcap_ng}
summary_info += {'rbd support':       rbd}
summary_info += {'libusb':            libusb}
summary_info += {'usb net redir':     usbredir}
summary_info += {'libnfs support':    libnfs}
summary_info += {'seccomp support':   seccomp}
summary_info += {'GlusterFS support': glusterfs}
summary_info += {'libssh support':    libssh}
summary_info += {'lzo support':       lzo}
summary_info += {'snappy support':    snappy}
summary_info += {'bzip2 support':     libbzip2}
summary_info += {'zstd support':      zstd}
summary_info += {'libpmem support':   libpmem}
summary_info += {'libdaxctl support': libdaxctl}
summary_info += {'libcbor support':   libcbor}
summary_info += {'libudev':           libudev}
# Dummy dependency, keep .found()
summary_info += {'FUSE lseek':        fuse_lseek.found()}
if host_os == 'freebsd'
  summary_info += {'libinotify-kqueue': inotify}
endif
summary_info += {'valgrind':          valgrind}
summary(summary_info, bool_yn: true, section: 'Dependencies')

if host_arch == 'unknown'
  message()
  warning('UNSUPPORTED HOST CPU')
  message()
  message('Support for CPU host architecture ' + cpu + ' is not currently')
  message('maintained.')
  if have_tcg
    message()
    message('configure has succeeded and you can continue to build, but')
    message('QEMU will use a slow interpreter to emulate the target CPU.')
  endif
endif

if not supported_oses.contains(host_os)
  message()
  warning('UNSUPPORTED HOST OS')
  message()
  message('Support for host OS ' + host_os + 'is not currently maintained.')
  message('although configure has succeeded and you can continue to build.')
endif

actually_reloc = get_option('relocatable')
# check if get_relocated_path() is actually able to relocate paths
if get_option('relocatable') and \
  not (get_option('prefix') / get_option('bindir')).startswith(get_option('prefix') / '')
  message()
  warning('bindir not included within prefix, the installation will not be relocatable.')
  actually_reloc = false
endif
if not actually_reloc and (host_os == 'windows' or get_option('relocatable'))
  if host_os == 'windows'
    message()
    warning('Windows installs should usually be relocatable.')
  endif
  message()
  message('QEMU will have to be installed under ' + get_option('prefix') + '.')
  message('Use --disable-relocatable to remove this warning.')
endif
