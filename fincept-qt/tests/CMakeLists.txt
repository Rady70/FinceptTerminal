# ── Fincept Terminal unit tests (Qt Test) ────────────────────────────────────
#
# Reached ONLY from the top-level CMakeLists.txt under `if(FINCEPT_BUILD_TESTS)`,
# which is OFF by default. Nothing in this file affects a normal build.
#
#   cmake -B build -DFINCEPT_BUILD_TESTS=ON ...
#   cmake --build build
#   ctest --test-dir build --output-on-failure          # add -C Release on MSVC
#
# ─────────────────────────────────────────────────────────────────────────────
# HARD RULE — read this before adding a target.
#
#   A test target lists the SPECIFIC sources it needs, explicitly, by path.
#
# Do NOT "solve" a missing symbol by refactoring the app into an OBJECT library
# and linking it here. The release/CI presets build with CMAKE_UNITY_BUILD=ON
# (batch size 20) while `win-dev` builds unity OFF, so any change to the main
# source list re-batches every file after it and can produce file-scope symbol
# redefinitions that appear ONLY on CI and never reproduce locally. See the
# "unity build" notes in ../CLAUDE.md.
#
# If a unit cannot be tested without dragging in half the application, that is a
# signal about the unit, not about this file: extract the pure logic into a small
# header (or a leaf .cpp) and test that. Do not grow the link line.
#
# Current cost of the whole suite: 3 executables, 5 translation units.
# ─────────────────────────────────────────────────────────────────────────────

# Qt Test ships with every Qt installation, so this adds no new dependency.
# _FINCEPT_QT_VER_ARGS is inherited from the parent scope so the version pin
# (6.8.3 EXACT by default) stays identical to the one the app is built against.
find_package(Qt6 ${_FINCEPT_QT_VER_ARGS} QUIET COMPONENTS Test)
if(NOT Qt6Test_FOUND)
    message(FATAL_ERROR
        "FINCEPT_BUILD_TESTS=ON but the Qt6 Test module was not found.\n"
        "Qt Test ships with the standard Qt installer — re-run the Qt maintenance "
        "tool and make sure 'Qt Test' is selected for your kit, or configure with "
        "-DFINCEPT_BUILD_TESTS=OFF.")
endif()

# Windows has no rpath: a freshly built tst_*.exe cannot find Qt6Core.dll unless
# the Qt bin dir is on PATH. Resolve it once and inject it per-test below, so
# `ctest` works straight out of a fresh configure without anyone having to run
# windeployqt against the test binaries.
#
# Best-effort by design: if the Qt layout is unusual (distro split packages, no
# qmake shipped) we simply skip the injection rather than fail the configure —
# on those layouts Qt is on the loader path already.
set(_fincept_qt_bindir "")
set(_fincept_qt_libdir "")
if(TARGET Qt6::qmake)
    get_target_property(_fincept_qt_qmake Qt6::qmake IMPORTED_LOCATION)
    if(_fincept_qt_qmake AND EXISTS "${_fincept_qt_qmake}")
        get_filename_component(_fincept_qt_bindir "${_fincept_qt_qmake}" DIRECTORY)
        get_filename_component(_fincept_qt_prefix "${_fincept_qt_bindir}" DIRECTORY)
        set(_fincept_qt_libdir "${_fincept_qt_prefix}/lib")
    endif()
endif()
if(NOT _fincept_qt_bindir AND Qt6_DIR)
    # Qt6_DIR is <prefix>/lib/cmake/Qt6 for a standard installer layout.
    get_filename_component(_fincept_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
    if(EXISTS "${_fincept_qt_prefix}/bin")
        set(_fincept_qt_bindir "${_fincept_qt_prefix}/bin")
        set(_fincept_qt_libdir "${_fincept_qt_prefix}/lib")
    endif()
endif()

# fincept_add_test(<name> <source> [<extra sources>...])
#
# <name> must be tst_<unit>. Extra sources are the app .cpp files this unit
# genuinely needs — keep that list as short as it can possibly be.
function(fincept_add_test _name)
    add_executable(${_name} ${ARGN})

    # Qt6::Test pulls in Qt6::Core. Deliberately no Widgets/Network/Sql here:
    # a unit test that needs those is testing the wrong seam.
    target_link_libraries(${_name} PRIVATE Qt6::Test Qt6::Core)

    # Sources use project-root-relative includes ("trading/OrderValidator.h"),
    # matching target_include_directories(FinceptTerminal PRIVATE src).
    target_include_directories(${_name} PRIVATE "${PROJECT_SOURCE_DIR}/src")

    set_target_properties(${_name} PROPERTIES
        AUTOMOC ON            # Q_OBJECT test classes live in the .cpp (+ #include "<name>.moc")
        UNITY_BUILD OFF       # never batch test TUs, whatever the parent preset says
        FOLDER "tests")

    # NOTE: no install() rule, by design. Test executables must never ship in a
    # release artifact. They also land in ${CMAKE_CURRENT_BINARY_DIR} (build/tests/),
    # not next to FinceptTerminal.exe, so the installer's
    # install(DIRECTORY "$<TARGET_FILE_DIR:FinceptTerminal>/" ... FILES_MATCHING "*.dll")
    # rule cannot pick them up either.

    add_test(NAME ${_name} COMMAND ${_name})

    if(WIN32 AND _fincept_qt_bindir)
        set_tests_properties(${_name} PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_fincept_qt_bindir}")
    elseif(UNIX AND NOT APPLE AND _fincept_qt_libdir)
        set_tests_properties(${_name} PROPERTIES
            ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:${_fincept_qt_libdir}")
    endif()
endfunction()

# ── Suites ───────────────────────────────────────────────────────────────────

# src/trading/brokers/BrokerModifyFields.h — header-only, zero app sources.
fincept_add_test(tst_broker_modify_fields
    tst_broker_modify_fields.cpp)

# src/core/result/Result.h — header-only, zero app sources.
fincept_add_test(tst_result
    tst_result.cpp)

# src/trading/OrderValidator.{h,cpp} — the .cpp includes only its own header,
# which includes only trading/TradingTypes.h (Qt Core types + plain structs, no
# .cpp of its own). One extra TU, no service/network/broker-registry linkage.
fincept_add_test(tst_order_validator
    tst_order_validator.cpp
    "${PROJECT_SOURCE_DIR}/src/trading/OrderValidator.cpp")

# MarketLab: capability defaults, screen gating, and the hosted-path deny-list
# (Qt Core only — two leaf TUs, no service linkage).
fincept_add_test(tst_marketlab_boundary
    tst_marketlab_boundary.cpp
    "${PROJECT_SOURCE_DIR}/src/core/capability/CapabilityManager.cpp"
    "${PROJECT_SOURCE_DIR}/src/network/http/HostedPathGuard.cpp")

# MarketLab: the rejection the two direct clients (QuantLibClient and
# CloudClient) share. Those clients own their own
# QNetworkAccessManager, so the shared HttpClient deny-list never sees them and
# each has to reject for itself — the decision they all call lives in the
# DirectRouteGuard leaf so it can be linked here against Qt Core alone.
# Deliberately NOT Qt6::Network: a binary with no network stack in it cannot
# initiate I/O, which is half of what this suite asserts.
fincept_add_test(tst_direct_clients
    tst_direct_clients.cpp
    "${PROJECT_SOURCE_DIR}/src/network/http/DirectRouteGuard.cpp"
    "${PROJECT_SOURCE_DIR}/src/network/http/ProbeGuard.cpp"
    "${PROJECT_SOURCE_DIR}/src/network/http/HostedPathGuard.cpp")

# MarketLab: quote / candle parsing — the missing-vs-zero decision, and the
# source / retrieval-time / status the result carries (FINCEPT_FORK_PLAN.md §4).
# src/services/equity/EquityQuoteParse.h is header-only and depends only on Qt
# Core plus EquityResearchModels.h (also header-only), so this adds one TU and
# no app sources at all.
fincept_add_test(tst_equity_parse
    tst_equity_parse.cpp)

# MarketLab: the same missing-vs-zero decision in the *markets* domain — the
# history/quote/sparkline boundary every live chart widget and the MCP markets
# tools read through. MarketDataService.cpp itself pulls PythonRunner,
# PythonWorker, CacheManager, SettingsRepository, DataHub and Logger, so per the
# HARD RULE the parsers live in src/services/markets/MarketQuoteParse.h, which
# is header-only over Qt Core alone. One TU, no app sources.
fincept_add_test(tst_market_parse
    tst_market_parse.cpp)

# MarketLab: economics response-envelope classification — the CFTC-shaped
# {"error": {...}} contract, string/array errors, success:false, and the
# error_code prefix. EconomicsService applies the same classifier to fresh and
# cached payloads so a provider error can never be cached or replayed as a
# success result. Header-only over Qt Core; no app sources.
fincept_add_test(tst_economics_envelope
    tst_economics_envelope.cpp)

# MarketLab: the CFTC net-position sign rule — a positive net gets "+", an
# exact zero is neutral and must not render as "+0". Header-only over Qt Core;
# no app sources.
fincept_add_test(tst_cftc_net_format
    tst_cftc_net_format.cpp)

# MarketLab: portfolio presentation rules — trend direction, contribution
# availability and presence-aware ordering keep missing, zero and directional
# states distinct. Header-only over plain types; no app sources.
fincept_add_test(tst_portfolio_rules
    tst_portfolio_rules.cpp)

# MarketLab: redirect containment in GuardedNetworkAccessManager.
#
# THE ONE EXCEPTION to "a unit test that needs Network is testing the wrong
# seam", and it is an exception on purpose rather than by drift. That rule was
# written for pure-logic units, and it still holds for every other target above
# — tst_direct_clients in particular keeps its Core-only link line precisely so
# the binary has no network stack to initiate I/O with, and nothing here changes
# that for any existing suite.
#
# This suite asserts a TRANSPORT behaviour, not a decision: that Qt, having
# followed a 302 without ever re-entering createRequest(), is stopped at the
# redirect hop. No predicate's return value can prove that, because the defect
# lived in what Qt did after the predicate had already said yes. Proving it
# needs a real reply following a real 302 over a real socket, so the test starts
# its own loopback QTcpServers — it resolves no name, contacts nothing off the
# machine, and needs no internet.
#
# Qt6::Network is added AFTER fincept_add_test() rather than inside it, so the
# function keeps handing every other target Qt6::Test + Qt6::Core and nothing
# else. Three leaf TUs: the guard, the deny-list it consults, and Logger for the
# LOG_WARN in the refusal paths.
fincept_add_test(tst_redirect_guard
    tst_redirect_guard.cpp
    "${PROJECT_SOURCE_DIR}/src/network/http/GuardedNetworkAccessManager.cpp"
    "${PROJECT_SOURCE_DIR}/src/network/http/HostedPathGuard.cpp"
    "${PROJECT_SOURCE_DIR}/src/core/logging/Logger.cpp")
target_link_libraries(tst_redirect_guard PRIVATE Qt6::Network)

message(STATUS "Fincept tests: tst_broker_modify_fields, tst_result, tst_order_validator, tst_marketlab_boundary, tst_direct_clients, tst_equity_parse, tst_market_parse, tst_economics_envelope, tst_cftc_net_format, tst_portfolio_rules, tst_redirect_guard")
