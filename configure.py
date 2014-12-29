#!/usr/bin/python3
import os, os.path, textwrap, argparse, sys, shlex, subprocess, tempfile, re

configure_args = str.join(' ', [shlex.quote(x) for x in sys.argv[1:]])

def add_tristate(arg_parser, name, dest, help):
    arg_parser.add_argument('--enable-' + name, dest = dest, action = 'store_true', default = None,
                            help = 'Enable ' + help)
    arg_parser.add_argument('--disable-' + name, dest = dest, action = 'store_false', default = None,
                            help = 'Disable ' + help)

def apply_tristate(var, test, note, missing):
    if (var is None) or var:
        if test():
            return True
        elif var == True:
            print(missing)
            sys.exit(1)
        else:
            print(note)
            return False
    return False

def try_compile(compiler, source = '', flags = []):
    with tempfile.NamedTemporaryFile() as sfile:
        sfile.file.write(bytes(source, 'utf-8'))
        sfile.file.flush()
        return subprocess.call([compiler, '-x', 'c++', '-o', '/dev/null', '-c', sfile.name] + flags,
                               stdout = subprocess.DEVNULL,
                               stderr = subprocess.DEVNULL) == 0

def warning_supported(warning, compiler):
    # gcc ignores -Wno-x even if it is not supported
    adjusted = re.sub('^-Wno-', '-W', warning)
    return try_compile(flags = [adjusted], compiler = compiler)

def debug_flag(compiler):
    src_with_auto = textwrap.dedent('''\
        template <typename T>
        struct x { auto f() {} };

        x<int> a;
        ''')
    if try_compile(source = src_with_auto, flags = ['-g', '-std=gnu++1y'], compiler = compiler):
        return '-g'
    else:
        print('Note: debug information disabled; upgrade your compiler')
        return ''

modes = {
    'debug': {
        'sanitize': '-fsanitize=address -fsanitize=leak -fsanitize=undefined',
        'sanitize_libs': '-lubsan -lasan',
        'opt': '-O0 -DDEBUG -DDEFAULT_ALLOCATOR',
        'libs': '',
    },
    'release': {
        'sanitize': '',
        'sanitize_libs': '',
        'opt': '-O2',
        'libs': '',
    },
}

tests = [
    'tests/test-reactor',
    'tests/fileiotest',
    'tests/echotest',
    'tests/l3_test',
    'tests/ip_test',
    'tests/timertest',
    'tests/tcp_test',
    'tests/futures_test',
    'tests/udp_server',
    'tests/udp_client',
    'tests/blkdiscard_test',
    'tests/sstring_test',
    'tests/memcached/test_ascii_parser',
    'tests/tcp_server',
    'tests/tcp_client',
    'tests/allocator_test',
    'tests/output_stream_test',
    'tests/udp_zero_copy',
    ]

apps = [
    'apps/httpd/httpd',
    'apps/seastar/seastar',
    'apps/memcached/memcached',
    'apps/memcached/flashcached',
    ]

all_artifacts = apps + tests

arg_parser = argparse.ArgumentParser('Configure seastar')
arg_parser.add_argument('--static', dest = 'static', action = 'store_const', default = '',
                        const = '-static',
                        help = 'Static link (useful for running on hosts outside the build environment')
arg_parser.add_argument('--pie', dest = 'pie', action = 'store_true',
                        help = 'Build position-independent executable (PIE)')
arg_parser.add_argument('--so', dest = 'so', action = 'store_true',
                        help = 'Build shared object (SO) instead of executable')
arg_parser.add_argument('--mode', action='store', choices=list(modes.keys()) + ['all'], default='all')
arg_parser.add_argument('--with', dest='artifacts', action='append', choices=all_artifacts, default=[])
arg_parser.add_argument('--cflags', action = 'store', dest = 'user_cflags', default = '',
                        help = 'Extra flags for the C++ compiler')
arg_parser.add_argument('--ldflags', action = 'store', dest = 'user_ldflags', default = '',
                        help = 'Extra flags for the linker')
arg_parser.add_argument('--compiler', action = 'store', dest = 'cxx', default = 'g++',
                        help = 'C++ compiler path')
arg_parser.add_argument('--with-osv', action = 'store', dest = 'with_osv', default = '',
                        help = 'Shortcut for compile for OSv')
arg_parser.add_argument('--dpdk-target', action = 'store', dest = 'dpdk_target', default = '',
                        help = 'Path to DPDK SDK target location (e.g. <DPDK SDK dir>/x86_64-native-linuxapp-gcc)')
add_tristate(arg_parser, name = 'hwloc', dest = 'hwloc', help = 'hwloc support')
add_tristate(arg_parser, name = 'xen', dest = 'xen', help = 'Xen support')
args = arg_parser.parse_args()

libnet = [
    'net/proxy.cc',
    'net/virtio.cc',
    'net/dpdk.cc',
    'net/net.cc',
    'net/ip.cc',
    'net/ethernet.cc',
    'net/arp.cc',
    'net/native-stack.cc',
    'net/ip_checksum.cc',
    'net/udp.cc',
    'net/tcp.cc',
    'net/dhcp.cc',
    ]

core = [
    'core/reactor.cc',
    'core/posix.cc',
    'core/memory.cc',
    'core/resource.cc',
    'core/scollectd.cc',
    'core/app-template.cc',
    'core/dpdk_rte.cc',
    'util/conversions.cc',
    'net/packet.cc',
    'net/posix-stack.cc',
    ]

defines = []
libs = '-laio -lboost_program_options -lboost_system -lstdc++ -lm -lboost_unit_test_framework -lboost_thread -lcryptopp'
hwloc_libs = '-lhwloc -lnuma -lpciaccess -lxml2 -lz'

def have_xen():
    source  = '#include <stdint.h>\n'
    source += '#include <xen/xen.h>\n'
    source += '#include <xen/sys/evtchn.h>\n'
    source += '#include <xen/sys/gntdev.h>\n'
    source += '#include <xen/sys/gntalloc.h>\n'

    return try_compile(compiler = args.cxx, source = source)

if apply_tristate(args.xen, test = have_xen,
                  note = 'Note: xen-devel not installed.  No Xen support.',
                  missing = 'Error: required package xen-devel not installed.'):
    libs += ' -lxenstore'
    defines.append("HAVE_XEN")
    libnet += [ 'net/xenfront.cc' ]
    core += [
                'core/xen/xenstore.cc',
                'core/xen/gntalloc.cc',
                'core/xen/evtchn.cc',
            ]


memcache_base = [
    'apps/memcached/ascii.rl'
] + libnet + core

memcache = [
    'apps/memcached/memcache.cc',
] + memcache_base

deps = {
    'apps/seastar/seastar': ['apps/seastar/main.cc'] + core,
    'tests/test-reactor': ['tests/test-reactor.cc'] + core,
    'apps/httpd/httpd': ['apps/httpd/httpd.cc', 'apps/httpd/request_parser.rl'] + libnet + core,
    'apps/memcached/memcached': ['apps/memcached/memcached.cc'] + memcache,
    'apps/memcached/flashcached': ['apps/memcached/flashcached.cc'] + memcache,
    'tests/memcached/test_ascii_parser': ['tests/memcached/test_ascii_parser.cc'] + memcache_base,
    'tests/fileiotest': ['tests/fileiotest.cc'] + core,
    'tests/echotest': ['tests/echotest.cc'] + core + libnet,
    'tests/l3_test': ['tests/l3_test.cc'] + core + libnet,
    'tests/ip_test': ['tests/ip_test.cc'] + core + libnet,
    'tests/tcp_test': ['tests/tcp_test.cc'] + core + libnet,
    'tests/timertest': ['tests/timertest.cc'] + core,
    'tests/futures_test': ['tests/futures_test.cc'] + core,
    'tests/udp_server': ['tests/udp_server.cc'] + core + libnet,
    'tests/udp_client': ['tests/udp_client.cc'] + core + libnet,
    'tests/tcp_server': ['tests/tcp_server.cc'] + core + libnet,
    'tests/tcp_client': ['tests/tcp_client.cc'] + core + libnet,
    'tests/blkdiscard_test': ['tests/blkdiscard_test.cc'] + core,
    'tests/sstring_test': ['tests/sstring_test.cc'] + core,
    'tests/allocator_test': ['tests/allocator_test.cc', 'core/memory.cc', 'core/posix.cc'],
    'tests/output_stream_test': ['tests/output_stream_test.cc'] + core + libnet,
    'tests/udp_zero_copy': ['tests/udp_zero_copy.cc'] + core + libnet,
}

warnings = [
    '-Wno-mismatched-tags',  # clang-only
    ]

# The "--with-osv=<path>" parameter is a shortcut for a bunch of other
# settings:
if args.with_osv:
    args.so = True
    args.hwloc = False
    args.user_cflags = (args.user_cflags +
        ' -DDEFAULT_ALLOCATOR -fvisibility=default -DHAVE_OSV -I' +
        args.with_osv + '/include')

if args.dpdk_target:
    args.user_cflags = (args.user_cflags +
        ' -DHAVE_DPDK -I' +
        args.dpdk_target + '/include -Wno-error=literal-suffix -Wno-literal-suffix')
    libs += (' -L' + args.dpdk_target + '/lib ' +
        '-Wl,--whole-archive -lrte_pmd_bond -lrte_pmd_vmxnet3_uio -lrte_pmd_virtio_uio -lrte_pmd_i40e -lrte_pmd_ixgbe -lrte_pmd_e1000 -lrte_pmd_ring -Wl,--no-whole-archive -lrte_distributor -lrte_kni -lrte_pipeline -lrte_table -lrte_port -lrte_timer -lrte_hash -lrte_lpm -lrte_power -lrte_acl -lrte_meter -lrte_sched -lrte_kvargs -lrte_mbuf -lrte_ip_frag -lethdev -lrte_eal -lrte_malloc -lrte_mempool -lrte_ring -lrte_cmdline -lrte_cfgfile -lrt -lm -ldl')

warnings = [w
            for w in warnings
            if warning_supported(warning = w, compiler = args.cxx)]

warnings = ' '.join(warnings)

dbgflag = debug_flag(args.cxx)

def have_hwloc():
    return try_compile(compiler = args.cxx, source = '#include <hwloc.h>\n#include <numa.h>')

if apply_tristate(args.hwloc, test = have_hwloc,
                  note = 'Note: hwloc-devel/numactl-devel not installed.  No NUMA support.',
                  missing = 'Error: required packages hwloc-devel/numactl-devel not installed.'):
    libs += ' ' + hwloc_libs
    defines.append('HAVE_HWLOC')
    defines.append('HAVE_NUMA')

if args.so:
    args.pie = '-shared'
    args.fpie = '-fpic'
elif args.pie:
    args.pie = '-pie'
    args.fpie = '-fpie'
else:
    args.pie = ''
    args.fpie = ''

defines = ' '.join(['-D' + d for d in defines])

globals().update(vars(args))

build_modes = modes if args.mode == 'all' else [args.mode]
build_artifacts = all_artifacts if not args.artifacts else args.artifacts

outdir = 'build'
buildfile = 'build.ninja'
os.makedirs(outdir, exist_ok = True)
do_sanitize = True
if args.static:
    do_sanitize = False
with open(buildfile, 'w') as f:
    f.write(textwrap.dedent('''\
        configure_args = {configure_args}
        builddir = {outdir}
        cxx = {cxx}
        cxxflags = -std=gnu++1y {dbgflag} {fpie} -Wall -Werror -fvisibility=hidden -pthread -I. {user_cflags} {warnings} {defines}
        ldflags = {dbgflag} -Wl,--no-as-needed {static} {pie} -fvisibility=hidden -pthread {user_ldflags}
        libs = {libs}
        rule ragel
            command = ragel -G2 -o $out $in
            description = RAGEL $out
        ''').format(**globals()))
    for mode in build_modes:
        modeval = modes[mode]
        if modeval['sanitize'] and not do_sanitize:
            print('Note: --static disables debug mode sanitizers')
            modeval['sanitize'] = ''
            modeval['sanitize_libs'] = ''
        f.write(textwrap.dedent('''\
            cxxflags_{mode} = {sanitize} {opt} -I $builddir/{mode}/gen
            libs_{mode} = {libs} {sanitize_libs}
            rule cxx.{mode}
              command = $cxx -MMD -MT $out -MF $out.d $cxxflags $cxxflags_{mode} -c -o $out $in
              description = CXX $out
              depfile = $out.d
            rule link.{mode}
              command = $cxx  $cxxflags_{mode} $ldflags -o $out $in $libs $libs_{mode}
              description = LINK $out
            ''').format(mode = mode, **modeval))
        f.write('build {mode}: phony {artifacts}\n'.format(mode = mode,
            artifacts = str.join(' ', ('$builddir/' + mode + '/' + x for x in build_artifacts))))
        compiles = {}
        ragels = {}
        for binary in build_artifacts:
            srcs = deps[binary]
            objs = ['$builddir/' + mode + '/' + src.replace('.cc', '.o')
                    for src in srcs
                    if src.endswith('.cc')]
            f.write('build $builddir/{}/{}: link.{} {}\n'.format(mode, binary, mode, str.join(' ', objs)))
            for src in srcs:
                if src.endswith('.cc'):
                    obj = '$builddir/' + mode + '/' + src.replace('.cc', '.o')
                    compiles[obj] = src
                elif src.endswith('.rl'):
                    hh = '$builddir/' + mode + '/gen/' + src.replace('.rl', '.hh')
                    ragels[hh] = src
                else:
                    raise Exeception('No rule for ' + src)
        for obj in compiles:
            src = compiles[obj]
            gen_headers = ragels.keys()
            f.write('build {}: cxx.{} {} || {} \n'.format(obj, mode, src, ' '.join(gen_headers)))
        for hh in ragels:
            src = ragels[hh]
            f.write('build {}: ragel {}\n'.format(hh, src))
    f.write(textwrap.dedent('''\
        rule configure
          command = python3 configure.py $configure_args
          generator = 1
        build build.ninja: configure | configure.py
        rule cscope
            command = find -name '*.[chS]' -o -name "*.cc" -o -name "*.hh" | cscope -bq -i-
            description = CSCOPE
        build cscope: cscope
        default {modes_list}
        ''').format(modes_list = ' '.join(build_modes), **globals()))
