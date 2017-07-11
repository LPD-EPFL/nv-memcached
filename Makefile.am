bin_PROGRAMS = memcached
pkginclude_HEADERS = protocol_binary.h
noinst_PROGRAMS = memcached-debug sizes testapp timedrun

BUILT_SOURCES=

testapp_SOURCES = testapp.cpp util.cpp util.h

timedrun_SOURCES = timedrun.cpp

sizes_SOURCES = sizes.h sizes.cpp

memcached_SOURCES = memcached.cpp memcached.h \
                    hash.cpp hash.h \
                    jenkins_hash.cpp jenkins_hash.h \
                    murmur3_hash.cpp murmur3_hash.h \
                    slabs.cpp slabs.h \
                    items.cpp items.h \
                    assoc.cpp assoc.h \
                    thread.cpp daemon.cpp \
                    stats.cpp stats.h \
                    util.cpp util.h \
                    trace.h cache.h sasl_defs.h

if BUILD_CACHE
memcached_SOURCES += cache.cpp
testapp_SOURCES += cache.cpp
endif

if BUILD_SOLARIS_PRIVS
memcached_SOURCES += solaris_priv.cpp
endif

if ENABLE_SASL
memcached_SOURCES += sasl_defs.cpp
endif

memcached_LDFLAGS = 
memcached_LDADD =
memcached_DEPENDENCIES =

if USE_NV_LF
memcached_SOURCES += atomic_ops_if.h hashtable.h hashtable.cpp \
            intset.cpp intset.h lf-linkedlist.cpp lf-linkedlist.h \
            lf-common.h common.h random.h latency.h barrier.h \
            main_test_loop.h utils.h measurements.h nv_lf_util.h nv_lf_util.cpp
CPPFLAGS += -DNVM
CPPFLAGS += -I$(NVRAM_PATH)/include/libnvram
CPPFLAGS += -I$(NVML_PATH)/include
CPPFLAGS += -I$(JEMALLOC_PATH)/include
CPPFLAGS += -I$(ATOMIC_OPS_PATH)/include -I$(ATOMIC_OPS_PATH)/include/atomic_ops -I$(ATOMIC_OPS_PATH)/include/atomic_ops/sysdeps
CPPFLAGS += -I./external/include

LIBS += -lrt -lm
LIBS += -L${JEMALLOC_PATH}/lib -Wl,-rpath,${JEMALLOC_PATH}/lib -ljemalloc
LIBS += -L${NVRAM_PATH}/lib -lnvram
LIBS += -L${NVML_PATH}/lib -lpmem -lpmemobj
LIBS += -L./external/lib 
endif

LIBS += -lpthread

memcached_debug_SOURCES = $(memcached_SOURCES)
memcached_CPPFLAGS = -DNDEBUG
memcached_debug_LDADD = @PROFILER_LDFLAGS@
memcached_debug_CFLAGS = @PROFILER_FLAGS@

memcached_LDADD =
memcached_DEPENDENCIES =
memcached_debug_DEPENDENCIES =
CLEANFILES=

if BUILD_DTRACE
BUILT_SOURCES += memcached_dtrace.h
CLEANFILES += memcached_dtrace.h
endif

if DTRACE_INSTRUMENT_OBJ
memcached_LDADD += memcached_dtrace.o
memcached_DEPENDENCIES += memcached_dtrace.o
memcached_debug_LDADD += memcached_debug_dtrace.o
memcached_debug_DEPENDENCIES += memcached_debug_dtrace.o
CLEANFILES += memcached_dtrace.o memcached_debug_dtrace.o
endif

memcached_dtrace.h: memcached_dtrace.d
	${DTRACE} -h -s memcached_dtrace.d
	sed -e 's,void \*,const void \*,g' memcached_dtrace.h | \
            sed -e 's,char \*,const char \*,g' | tr '\t' ' ' > mmc_dtrace.tmp
	mv mmc_dtrace.tmp memcached_dtrace.h

memcached_dtrace.o: $(memcached_OBJECTS)
	$(DTRACE) $(DTRACEFLAGS) -G -o memcached_dtrace.o -s ${srcdir}/memcached_dtrace.d $(memcached_OBJECTS)

memcached_debug_dtrace.o: $(memcached_debug_OBJECTS)
	$(DTRACE) $(DTRACEFLAGS) -G -o memcached_debug_dtrace.o -s ${srcdir}/memcached_dtrace.d $(memcached_debug_OBJECTS)


SUBDIRS = doc
DIST_DIRS = scripts
EXTRA_DIST = doc scripts t memcached.spec memcached_dtrace.d version.m4 README.md

MOSTLYCLEANFILES = *.gcov *.gcno *.gcda *.tcov

test:	memcached-debug sizes testapp
	$(srcdir)/sizes
	$(srcdir)/testapp
	prove $(srcdir)/t
	@if test `basename $(PROFILER)` = "gcov"; then \
	  for file in memcached_debug-*.gc??; do \
	    mv -f $$file `echo $$file | sed 's/memcached_debug-//'`; \
	  done && \
	  for file in *.gcda; do \
	    srcfile=`echo $$file | sed 's/.gcda/.c/'`; \
	    if test -n "`echo $(memcached_debug_SOURCES) | grep $$srcfile`"; then \
	      echo `$(PROFILER) $$srcfile` | sed 's/'$$srcfile':.*//'; \
	    fi \
	  done \
	elif test `basename $(PROFILER)` = "tcov"; then \
	  files=`grep SRCFILE memcached-debug.profile/tcovd | sed 's/SRCFILE://' | sort | uniq` && \
	  $(PROFILER) -x memcached-debug.profile $$files 2>&1; \
	  for file in *.tcov; do \
	    srcfile=`echo $$file | sed 's/.tcov//'`; \
	    if test -n "`echo $(memcached_debug_SOURCES) | grep $$srcfile`"; then \
	      echo $$srcfile : `grep 'Percent of the file executed' $$file`; \
	    fi \
	  done \
	else :; fi

docs:
	(cat Doxyfile ; echo "PROJECT_NUMBER=`cat version.num`") | doxygen -

dist-hook:
	rm -f $(distdir)/*/*~ $(distdir)/t/lib/*~ $(distdir)/*~

maintainer-clean-local:
	-rm Makefile.in
	-rm aclocal.m4
	-rm config.guess
	-rm config.sub
	-rm depcomp
	-rm install-sh
	-rm ltmain.sh
	-rm missing
	-rm configure
	-rm config.log
	-rm config.status
