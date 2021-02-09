# defines a directory for build, for example, RH6_x86_64
lsb_dist     := $(shell if [ -x /usr/bin/lsb_release ] ; then lsb_release -is ; else echo Linux ; fi)
lsb_dist_ver := $(shell if [ -x /usr/bin/lsb_release ] ; then lsb_release -rs | sed 's/[.].*//' ; else uname -r | sed 's/[-].*//' ; fi)
uname_m      := $(shell uname -m)

short_dist_lc := $(patsubst CentOS,rh,$(patsubst RedHatEnterprise,rh,\
                   $(patsubst RedHat,rh,\
                     $(patsubst Fedora,fc,$(patsubst Ubuntu,ub,\
                       $(patsubst Debian,deb,$(patsubst SUSE,ss,$(lsb_dist))))))))
short_dist    := $(shell echo $(short_dist_lc) | tr a-z A-Z)
pwd           := $(shell pwd)
rpm_os        := $(short_dist_lc)$(lsb_dist_ver).$(uname_m)

# this is where the targets are compiled
build_dir ?= $(short_dist)$(lsb_dist_ver)_$(uname_m)$(port_extra)
bind      := $(build_dir)/bin
libd      := $(build_dir)/lib64
objd      := $(build_dir)/obj
dependd   := $(build_dir)/dep

# use 'make port_extra=-g' for debug build
ifeq (-g,$(findstring -g,$(port_extra)))
  DEBUG = true
endif

CC          ?= gcc
CXX         ?= g++
cc          := $(CC) -std=c11
cpp         := $(CXX) -std=c++11 
# if not linking libstdc++
ifdef NO_STL
cppflags    := -fno-rtti -fno-exceptions
cpplink     := $(CC)
else
cppflags    :=
cpplink     := $(CXX)
endif
arch_cflags := -mavx -maes -fno-omit-frame-pointer
#gcc_wflags  := -Wall -Wextra -Werror
gcc_wflags  := -Wall -Wextra
fpicflags   := -fPIC
soflag      := -shared

ifdef DEBUG
default_cflags := -ggdb
else
default_cflags := -ggdb -O3 -Ofast
endif
# rpmbuild uses RPM_OPT_FLAGS
CFLAGS := $(default_cflags)
#RPM_OPT_FLAGS ?= $(default_cflags)
#CFLAGS ?= $(RPM_OPT_FLAGS)
cflags := $(gcc_wflags) $(CFLAGS) $(arch_cflags)

# where to find the raids/xyz.h files
INCLUDES    ?= -Iinclude
includes    := $(INCLUDES)
DEFINES     ?=
defines     := $(DEFINES)
cpp_lnk     :=
sock_lib    :=
math_lib    := -lm
thread_lib  := -pthread -lrt

# test submodules exist (they don't exist for dist_rpm, dist_dpkg targets)
have_kv_submodule     := $(shell if [ -f ./raikv/GNUmakefile ]; then echo yes; else echo no; fi )
have_aeron_submodule  := $(shell if [ -f ./aeron/GNUmakefile ]; then echo yes; else echo no; fi )

lnk_lib     :=
dlnk_lib    :=
lnk_dep     :=
dlnk_dep    :=

# if building submodules, reference them rather than the libs installed
ifeq (yes,$(have_kv_submodule))
kv_lib      := raikv/$(libd)/libraikv.a
kv_dll      := raikv/$(libd)/libraikv.so
lnk_lib     += $(kv_lib)
lnk_dep     += $(kv_lib)
dlnk_lib    += -Lraikv/$(libd) -lraikv
dlnk_dep    += $(kv_dll)
rpath2       = ,-rpath,$(pwd)/raikv/$(libd)
kv_include  := -Iraikv/include
else
lnk_lib     += -lraikv
dlnk_lib    += -lraikv
endif

ifeq (yes,$(have_aeron_submodule))
aeron_lib        := aeron/$(libd)/libaeron_static.a
aeron_driver_lib := aeron/$(libd)/libaeron_driver_static.a
aeron_dll        := aeron/$(libd)/libaeron.so
aeron_include    := -Iaeron/aeron-client/src/main/c
lnk_lib          += -rdynamic $(aeron_lib)
lnk_dep          += $(aeron_lib)
dlnk_lib         += -Laeron/$(libd) -laeron
dlnk_dep         += $(aeron_dll)
rpath5            = ,-rpath,$(pwd)/aeron/$(libd)
aeron_client_lib     := aeron/$(libd)/libaeron_client.a
aeron_client_dll     := aeron/$(libd)/libaeron_client_shared.so
aeron_client_include := -Iaeron/aeron-client/src/main/cpp
cpp_lnk_lib          += -rdynamic $(aeron_client_lib)
cpp_lnk_dep          += $(aeron_client_lib)
cpp_dlnk_lib         += -Laeron/$(libd) -laeron_client_shared
cpp_dlnk_dep         += $(aeron_client_dll)
else
lnk_lib       += -laeron
dlnk_lib      += -laeron
aeron_include := -I/usr/include/aeron
endif

aekv_lib := $(libd)/libaekv.a
rpath       := -Wl,-rpath,$(pwd)/$(libd)$(rpath1)$(rpath2)$(rpath3)$(rpath4)$(rpath5)$(rpath6)$(rpath7)
dlnk_lib    += -lpcre2-8 -lcrypto -ldl
malloc_lib  :=
lnk_lib     += -lpcre2-8 -ldl

includes += $(kv_include) $(aeron_include)

.PHONY: everything
everything: $(kv_lib) $(aekv_lib) all

clean_subs :=
dlnk_dll_depend :=
dlnk_lib_depend :=

# build submodules if have them
ifeq (yes,$(have_kv_submodule))
$(kv_lib) $(kv_dll):
	$(MAKE) -C raikv
.PHONY: clean_kv
clean_kv:
	$(MAKE) -C raikv clean
clean_subs += clean_kv
endif
ifeq (yes,$(have_aeron_submodule))
$(aeron_lib) $(aeron_dll) $(aeron_driver_lib):
	$(MAKE) -C aeron
.PHONY: clean_aeron
clean_aeron:
	$(MAKE) -C aeron clean
clean_subs += clean_aeron
endif

# copr/fedora build (with version env vars)
# copr uses this to generate a source rpm with the srpm target
-include .copr/Makefile

# debian build (debuild)
# target for building installable deb: dist_dpkg
-include deb/Makefile

# targets filled in below
all_exes    :=
all_libs    :=
all_dlls    :=
all_depends :=
gen_files   :=

libaekv_files := ev_aeron coroutine
libaekv_objs  := $(addprefix $(objd)/, $(addsuffix .o, $(libaekv_files)))
libaekv_dbjs  := $(addprefix $(objd)/, $(addsuffix .fpic.o, $(libaekv_files)))
libaekv_deps  := $(addprefix $(dependd)/, $(addsuffix .d, $(libaekv_files))) \
                  $(addprefix $(dependd)/, $(addsuffix .fpic.d, $(libaekv_files)))
libaekv_dlnk  := $(dlnk_lib)
libaekv_spec  := $(version)-$(build_num)
libaekv_ver   := $(major_num).$(minor_num)

$(libd)/libaekv.a: $(libaekv_objs)
$(libd)/libaekv.so: $(libaekv_dbjs) $(dlnk_dep)

all_libs    += $(libd)/libaekv.a
all_dlls    += $(libd)/libaekv.so
all_depends += $(libaekv_deps)

server_defines     := -DAEKV_VER=$(ver_build)
aeron_server_files := server
aeron_server_objs  := $(addprefix $(objd)/, $(addsuffix .o, $(aeron_server_files)))
aeron_server_deps  := $(addprefix $(dependd)/, $(addsuffix .d, $(aeron_server_files)))
aeron_server_libs  := $(aekv_lib)
aeron_server_lnk   := $(aekv_lib) $(lnk_lib)

$(bind)/aeron_server: $(aeron_server_objs) $(aeron_server_libs) $(lnk_dep)

all_exes    += $(bind)/aeron_server
all_depends += $(aeron_server_deps)

cping_defines        := -D_DEFAULT_SOURCE
cpong_defines        := $(cping_defines)
cping_coro_defines   := $(cping_defines)
cpong_coro_defines   := $(cping_defines)
sample_util_defines  := $(cping_defines)
basic_sub_defines    := $(cping_defines)
basic_pub_defines    := $(cping_defines)
BasicSub_defines     := $(cping_defines)
BasicPub_defines     := $(cping_defines)
aeronmd_defines      := -D_DEFAULT_SOURCE -DDISABLE_BOUNDS_CHECKS -DHAVE_STRUCT_MMSGHDR \
                        -DHAVE_EPOLL -DHAVE_BSDSTDLIB_H -DHAVE_RECVMMSG -DHAVE_SENDMMSG \
			-D_FILE_OFFSET_BITS=64

cl_includes          := -Iaeron/HdrHistogram_c/src -Iaeron/aeron-samples/src/main/c
cping_includes       := $(cl_includes) -Iaeron/aeron-driver/src/main/c -Iaeron/aeron-driver/src/main/c
cpong_includes       := $(cl_includes) -Iaeron/aeron-driver/src/main/c -Iaeron/aeron-driver/src/main/c
cping_coro_includes  := $(cl_includes)
cpong_coro_includes  := $(cl_includes)
sample_util_includes := $(cl_includes)
basic_sub_includes   := $(cl_includes)
basic_pub_includes   := $(cl_includes)
BasicSub_includes    := -Iaeron/aeron-samples/src/main/cpp -Iaeron/aeron-client/src/main/cpp
BasicPub_includes    := -Iaeron/aeron-samples/src/main/cpp -Iaeron/aeron-client/src/main/cpp
aeronmd_includes     := -Iaeron/aeron-driver/src/main/c -Iaeron/aeron-driver/src/main/c

cping_coro_files := cping_coro sample_util
cping_coro_objs  := $(addprefix $(objd)/, $(addsuffix .o, $(cping_coro_files)))
cping_coro_deps  := $(addprefix $(dependd)/, $(addsuffix .d, $(cping_coro_files)))
cping_coro_libs  := $(aeron_lib) $(aeron_driver_lib) $(aekv_lib)
cping_coro_lnk   := -rdynamic $(aeron_lib) $(aeron_driver_lib) $(aekv_lib) aeron/HdrHistogram_c/$(libd)/libhdrhist.a -ldl

$(bind)/cping_coro: $(cping_coro_objs) $(cping_coro_libs) $(lnk_dep)

cpong_coro_files := cpong_coro sample_util
cpong_coro_objs  := $(addprefix $(objd)/, $(addsuffix .o, $(cpong_coro_files)))
cpong_coro_deps  := $(addprefix $(dependd)/, $(addsuffix .d, $(cpong_coro_files)))
cpong_coro_libs  := $(aeron_lib) $(aeron_driver_lib) $(aekv_lib)
cpong_coro_lnk   := -rdynamic $(aeron_lib) $(aeron_driver_lib) $(aekv_lib) aeron/HdrHistogram_c/$(libd)/libhdrhist.a -ldl

$(bind)/cpong_coro: $(cpong_coro_objs) $(cpong_coro_libs) $(lnk_dep)

cping_files := cping sample_util
cping_objs  := $(addprefix $(objd)/, $(addsuffix .o, $(cping_files)))
cping_deps  := $(addprefix $(dependd)/, $(addsuffix .d, $(cping_files)))
cping_libs  := $(aeron_lib) $(aeron_driver_lib) $(aekv_lib)
cping_lnk   := -rdynamic $(aeron_lib) $(aeron_driver_lib) $(aekv_lib) aeron/HdrHistogram_c/$(libd)/libhdrhist.a -lbsd -ldl

$(bind)/cping: $(cping_objs) $(cping_libs) $(lnk_dep)

cpong_files := cpong sample_util
cpong_objs  := $(addprefix $(objd)/, $(addsuffix .o, $(cpong_files)))
cpong_deps  := $(addprefix $(dependd)/, $(addsuffix .d, $(cpong_files)))
cpong_libs  := $(aeron_lib) $(aeron_driver_lib) $(aekv_lib)
cpong_lnk   := -rdynamic $(aeron_lib) $(aeron_driver_lib) $(aekv_lib) aeron/HdrHistogram_c/$(libd)/libhdrhist.a -lbsd -ldl

$(bind)/cpong: $(cpong_objs) $(cpong_libs) $(lnk_dep)

basic_sub_files := basic_sub sample_util
basic_sub_objs  := $(addprefix $(objd)/, $(addsuffix .o, $(basic_sub_files)))
basic_sub_deps  := $(addprefix $(dependd)/, $(addsuffix .d, $(basic_sub_files)))
basic_sub_libs  := $(aeron_lib)
basic_sub_lnk   := -rdynamic $(aeron_lib) -ldl

$(bind)/basic_sub: $(basic_sub_objs) $(basic_sub_libs) $(lnk_dep)

basic_pub_files := basic_pub sample_util
basic_pub_objs  := $(addprefix $(objd)/, $(addsuffix .o, $(basic_pub_files)))
basic_pub_deps  := $(addprefix $(dependd)/, $(addsuffix .d, $(basic_pub_files)))
basic_pub_libs  := $(aeron_lib)
basic_pub_lnk   := -rdynamic $(aeron_lib) -ldl

$(bind)/basic_pub: $(basic_pub_objs) $(basic_pub_libs) $(lnk_dep)

BasicSub_files := BasicSub
BasicSub_objs  := $(addprefix $(objd)/, $(addsuffix .o, $(BasicSub_files)))
BasicSub_deps  := $(addprefix $(dependd)/, $(addsuffix .d, $(BasicSub_files)))
BasicSub_libs  := $(aeron_client_lib)
BasicSub_lnk   := $(aeron_client_lib) -ldl

$(bind)/BasicSub: $(BasicSub_objs) $(BasicSub_libs) $(lnk_dep)

BasicPub_files := BasicPub
BasicPub_objs  := $(addprefix $(objd)/, $(addsuffix .o, $(BasicPub_files)))
BasicPub_deps  := $(addprefix $(dependd)/, $(addsuffix .d, $(BasicPub_files)))
BasicPub_libs  := $(aeron_client_lib)
BasicPub_lnk   := $(aeron_client_lib) -ldl

$(bind)/BasicPub: $(BasicPub_objs) $(BasicPub_libs) $(lnk_dep)

aeronmd_files := aeronmd
aeronmd_objs  := $(addprefix $(objd)/, $(addsuffix .o, $(aeronmd_files)))
aeronmd_deps  := $(addprefix $(dependd)/, $(addsuffix .d, $(aeronmd_files)))
aeronmd_libs  := $(aeron_driver_lib)
aeronmd_lnk   := -rdynamic $(aeron_driver_lib) -lbsd -ldl

$(bind)/aeronmd: $(aeronmd_objs) $(aeronmd_libs) $(lnk_dep)

coro_test_files := coro_test
coro_test_objs  := $(addprefix $(objd)/, $(addsuffix .o, $(coro_test_files)))
coro_test_deps  := $(addprefix $(dependd)/, $(addsuffix .d, $(coro_test_files)))
coro_test_libs  := $(aekv_lib)
coro_test_lnk   := $(aekv_lib) $(lnk_lib)

$(bind)/coro_test: $(coro_test_objs) $(coro_test_libs) $(lnk_dep)

all_exes    += $(bind)/cping $(bind)/cpong \
               $(bind)/cping_coro $(bind)/cpong_coro \
               $(bind)/basic_sub $(bind)/basic_pub \
               $(bind)/BasicSub $(bind)/BasicPub \
	       $(bind)/aeronmd $(bind)/coro_test
all_depends += $(cping_deps) $(cpong_deps) \
               $(cping_coro_deps) $(cpong_coro_deps) \
               $(basic_sub_deps) $(basic_pub_deps) \
               $(BasicSub_deps) $(BasicPub_deps) \
	       $(aeronmd_deps) $(coro_test_deps)

all_dirs := $(bind) $(libd) $(objd) $(dependd)

# the default targets
.PHONY: all
all: $(all_libs) $(all_dlls) $(all_exes)

.PHONY: dnf_depend
dnf_depend:
	sudo dnf -y install make gcc-c++ git redhat-lsb openssl-devel pcre2-devel chrpath liblzf-devel zlib-devel libbsd-devel

.PHONY: yum_depend
yum_depend:
	sudo yum -y install make gcc-c++ git redhat-lsb openssl-devel pcre2-devel chrpath liblzf-devel zlib-devel libbsd-devel

.PHONY: deb_depend
deb_depend:
	sudo apt-get install -y install make g++ gcc devscripts libpcre2-dev chrpath git lsb-release libssl-dev

# create directories
$(dependd):
	@mkdir -p $(all_dirs)

# remove target bins, objs, depends
.PHONY: clean
clean: $(clean_subs)
	rm -r -f $(bind) $(libd) $(objd) $(dependd)
	if [ "$(build_dir)" != "." ] ; then rmdir $(build_dir) ; fi

.PHONY: clean_dist
clean_dist:
	rm -rf dpkgbuild rpmbuild

.PHONY: clean_all
clean_all: clean clean_dist

# force a remake of depend using 'make -B depend'
.PHONY: depend
depend: $(dependd)/depend.make

$(dependd)/depend.make: $(dependd) $(all_depends)
	@echo "# depend file" > $(dependd)/depend.make
	@cat $(all_depends) >> $(dependd)/depend.make

.PHONY: dist_bins
dist_bins: $(all_libs) $(all_dlls) $(bind)/aeron_server
	chrpath -d $(libd)/libaekv.so
	chrpath -d $(bind)/aeron_server

.PHONY: dist_rpm
dist_rpm: srpm
	( cd rpmbuild && rpmbuild --define "-topdir `pwd`" -ba SPECS/aekv.spec )

# dependencies made by 'make depend'
-include $(dependd)/depend.make

ifeq ($(DESTDIR),)
# 'sudo make install' puts things in /usr/local/lib, /usr/local/include
install_prefix = /usr/local
else
# debuild uses DESTDIR to put things into debian/aekv/usr
install_prefix = $(DESTDIR)/usr
endif

install: dist_bins
	install -d $(install_prefix)/lib $(install_prefix)/bin
	install -d $(install_prefix)/include/aekv
	for f in $(libd)/libaekv.* ; do \
	if [ -h $$f ] ; then \
	cp -a $$f $(install_prefix)/lib ; \
	else \
	install $$f $(install_prefix)/lib ; \
	fi ; \
	done
	install -m 644 include/aekv/*.h $(install_prefix)/include/aekv

$(objd)/%.o: src/%.cpp
	$(cpp) $(cflags) $(cppflags) $(includes) $(defines) $($(notdir $*)_includes) $($(notdir $*)_defines) -c $< -o $@

$(objd)/%.o: src/%.c
	$(cc) $(cflags) $(includes) $(defines) $($(notdir $*)_includes) $($(notdir $*)_defines) -c $< -o $@

$(objd)/%.fpic.o: src/%.cpp
	$(cpp) $(cflags) $(fpicflags) $(cppflags) $(includes) $(defines) $($(notdir $*)_includes) $($(notdir $*)_defines) -c $< -o $@

$(objd)/%.fpic.o: src/%.c
	$(cc) $(cflags) $(fpicflags) $(includes) $(defines) $($(notdir $*)_includes) $($(notdir $*)_defines) -c $< -o $@

$(objd)/%.o: test/%.cpp
	$(cpp) $(cflags) $(cppflags) $(includes) $(defines) $($(notdir $*)_includes) $($(notdir $*)_defines) -c $< -o $@

$(objd)/%.o: test/%.c
	$(cc) $(cflags) $(includes) $(defines) $($(notdir $*)_includes) $($(notdir $*)_defines) -c $< -o $@

$(libd)/%.a:
	ar rc $@ $($(*)_objs)

$(libd)/%.so:
	$(cpplink) $(soflag) $(rpath) $(cflags) -o $@.$($(*)_spec) -Wl,-soname=$(@F).$($(*)_ver) $($(*)_dbjs) $($(*)_dlnk) $(cpp_dll_lnk) $(sock_lib) $(math_lib) $(thread_lib) $(malloc_lib) $(dynlink_lib) && \
	cd $(libd) && ln -f -s $(@F).$($(*)_spec) $(@F).$($(*)_ver) && ln -f -s $(@F).$($(*)_ver) $(@F)

$(bind)/%:
	$(cpplink) $(cflags) $(rpath) -o $@ $($(*)_objs) -L$(libd) $($(*)_lnk) $(cpp_lnk) $(sock_lib) $(math_lib) $(thread_lib) $(malloc_lib) $(dynlink_lib)

$(bind)/%.static:
	$(cpplink) $(cflags) -o $@ $($(*)_objs) $($(*)_static_lnk) $(sock_lib) $(math_lib) $(thread_lib) $(malloc_lib) $(dynlink_lib)

$(dependd)/%.d: src/%.cpp
	$(cpp) $(arch_cflags) $(defines) $(includes) $($(notdir $*)_includes) $($(notdir $*)_defines) -MM $< -MT $(objd)/$(*).o -MF $@

$(dependd)/%.d: src/%.c
	$(cc) $(arch_cflags) $(defines) $(includes) $($(notdir $*)_includes) $($(notdir $*)_defines) -MM $< -MT $(objd)/$(*).o -MF $@

$(dependd)/%.fpic.d: src/%.cpp
	$(cpp) $(arch_cflags) $(defines) $(includes) $($(notdir $*)_includes) $($(notdir $*)_defines) -MM $< -MT $(objd)/$(*).fpic.o -MF $@

$(dependd)/%.fpic.d: src/%.c
	$(cc) $(arch_cflags) $(defines) $(includes) $($(notdir $*)_includes) $($(notdir $*)_defines) -MM $< -MT $(objd)/$(*).fpic.o -MF $@

$(dependd)/%.d: test/%.cpp
	$(cpp) $(arch_cflags) $(defines) $(includes) $($(notdir $*)_includes) $($(notdir $*)_defines) -MM $< -MT $(objd)/$(*).o -MF $@

$(dependd)/%.d: test/%.c
	$(cc) $(arch_cflags) $(defines) $(includes) $($(notdir $*)_includes) $($(notdir $*)_defines) -MM $< -MT $(objd)/$(*).o -MF $@

