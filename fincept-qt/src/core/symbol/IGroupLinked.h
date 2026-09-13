#pragma once
#include "core/symbol/SymbolGroup.h"
#include "core/symbol/SymbolRef.h"

// Q_DECLARE_INTERFACE comes from <QObject>. <QtPlugin> is not needed here:
// qplugin.h's constexpr offsetof members are a hard error under clang 20 with
// Qt 6.8 headers, which breaks the Windows clang-tidy gate for every TU that
// reaches this header (e.g. WatchlistScreen.cpp).
#include <QObject>

namespace fincept {

/// Mix-in for screens that participate in symbol-group linking.
/// `set_group(None)` unlinks; subscribers must not re-publish in on_group_symbol_changed.
class IGroupLinked {
  public:
    virtual ~IGroupLinked() = default;

    virtual void set_group(SymbolGroup g) = 0;
    virtual SymbolGroup group() const = 0;

    virtual void on_group_symbol_changed(const SymbolRef& ref) = 0;

    /// Returns invalid SymbolRef if nothing selected.
    virtual SymbolRef current_symbol() const = 0;
};

} // namespace fincept

Q_DECLARE_INTERFACE(fincept::IGroupLinked, "in.fincept.IGroupLinked/1.0")
