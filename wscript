# -*- Mode: python; py-indent-offset: 4; indent-tabs-mode: nil; coding: utf-8; -*-

from waflib import Utils
import os

VERSION = '0.1'
APPNAME = 'ndn-repo-ng'

def options(opt):
    opt.load(['compiler_cxx', 'gnu_dirs'])
    opt.load(['default-compiler-flags', 'coverage', 'sanitizers', 'boost', 'sqlite3', 'mongodb'],
             tooldir=['.waf-tools'])

    optgrp = opt.add_option_group('Repo-ng Options')
    optgrp.add_option('--with-examples', action='store_true', default=False,
                      help='Build examples')
    optgrp.add_option('--with-tests', action='store_true', default=False,
                      help='Build unit tests')
    optgrp.add_option('--without-tools', action='store_false', default=True, dest='with_tools',
                      help='Do not build tools')

def configure(conf):
    conf.load(['compiler_cxx', 'gnu_dirs',
               'default-compiler-flags', 'boost', 'sqlite3', 'mongodb'])

    conf.env['WITH_EXAMPLES'] = conf.options.with_examples
    conf.env['WITH_TESTS'] = conf.options.with_tests
    conf.env['WITH_TOOLS'] = conf.options.with_tools

    conf.check_cfg(package='libndn-cxx', args=['--cflags', '--libs'], uselib_store='NDN_CXX',
                   pkg_config_path=os.environ.get('PKG_CONFIG_PATH', '%s/pkgconfig' % conf.env.LIBDIR))

    conf.check_sqlite3()
    conf.check_mongodb()

    USED_BOOST_LIBS = ['system', 'program_options', 'iostreams', 'filesystem', 'thread', 'log']
    if conf.env['WITH_TESTS']:
        USED_BOOST_LIBS += ['unit_test_framework']
    conf.check_boost(lib=USED_BOOST_LIBS, mt=True)

    conf.check_compiler_flags()

    # Loading "late" to prevent tests from being compiled with profiling flags
    conf.load('coverage')
    conf.load('sanitizers')

    conf.define('DEFAULT_CONFIG_FILE', '%s/ndn/repo-ng.conf' % conf.env['SYSCONFDIR'])
    conf.define_cond('DISABLE_SQLITE3_FS_LOCKING', not conf.options.with_sqlite_locking)
    conf.define_cond('HAVE_TESTS', conf.env['WITH_TESTS'])

    conf.write_config_header('src/config.hpp')

    conf.recurse('tools')

def build(bld):
    bld.objects(target='repo-objects',
                source=bld.path.ant_glob('src/**/*.cpp',
                                         excl=['src/main.cpp']),
                use='NDN_CXX BOOST SQLITE3 MONGODB',
                includes='src',
                export_includes='src')

    bld.program(name='ndn-repo-ng',
                target='bin/ndn-repo-ng',
                source='src/main.cpp',
                use='repo-objects')

    bld.recurse('tests')
    bld.recurse('tools')
    bld.recurse('examples')

    bld.install_files('${SYSCONFDIR}/ndn', 'repo-ng.conf.sample')

    if Utils.unversioned_sys_platform() == 'linux':
        bld(features='subst',
            name='repo-ng.service',
            source='systemd/repo-ng.service.in',
            target='systemd/repo-ng.service')
