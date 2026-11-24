# Links a target against the shared source library built in CORE_BUILD_DIR.
CORE_LIBRARY = $$CORE_BUILD_DIR/libomacardscore.a

LIBS += -L$$CORE_BUILD_DIR -lomacardscore
PRE_TARGETDEPS += $$CORE_LIBRARY

# The library is a file target, so make would skip the sub-make whenever the
# archive exists. FORCE keeps header edits reachable: the core Makefile owns
# the object-level dependencies and decides whether anything really rebuilds.
core_library.target = $$CORE_LIBRARY
core_library.depends = FORCE
core_library.commands = @mkdir -p $$CORE_BUILD_DIR && { test -f $$CORE_BUILD_DIR/Makefile || (cd $$CORE_BUILD_DIR && qmake6 $$CORE_PROJECT -o Makefile >/dev/null); } && \$(MAKE) --no-print-directory -C $$CORE_BUILD_DIR
QMAKE_EXTRA_TARGETS += core_library
