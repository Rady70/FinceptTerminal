#pragma once

// MarketLab Terminal — external-browser launch guard (FINCEPT_FORK_PLAN.md §11.2).
//
// §11.2 requires that no application action opens a Fincept-owned URL in an
// external browser or embedded web view. The launches this fork keeps are the
// "open this item in my browser" actions of registered screens — the article
// link of a news or RSS item, a government-data resource, a ReliefWeb/HDX
// report, an RD-Agent log viewer, the user's own stream — and none of those
// destinations is known statically. Every one of them arrives from fetched
// content or from a table cell filled by it, so a feed the user subscribed to
// can carry a fincept.in link in an item and a click hands it straight to the
// OS. That launch is precisely what §11.2 prohibits, and no amount of UI
// hiding reaches it, because the URL is data, not a shipped default.
//
// One helper rather than the same four lines in a dozen lambdas across a dozen
// screens: the check is identical at every site, and the source audit
// (marketlab/audit_hosted_paths.py) pins the open_url sink count per file — so
// funnelling the launch through here leaves the product with a single
// browser-handoff site, and it is the one that consults the deny-list.
//
// Deliberately not a URL policy layer: it owns no hosts of its own, keeps the
// deny-list in HostedPathGuard, and changes nothing for a third-party
// destination, which is handed to the OS exactly as before.

#include <QString>

class QUrl;
class QWidget;

namespace fincept::network {

class ExternalUrlGuard {
  public:
    /// Open `url` in the user's browser (or whatever the OS registers for its
    /// scheme) unless it names a Fincept-owned destination. Returns false and
    /// opens nothing when the guard refuses; otherwise returns whatever the
    /// platform handoff reported.
    ///
    /// A refusal is always logged with the rejected host. When `parent` is
    /// given it is also shown to the user over that widget, because a click
    /// that silently does nothing reads as a broken button rather than a
    /// boundary.
    static bool open_external(const QUrl& url, QWidget* parent = nullptr);
};

} // namespace fincept::network
