#include "screens/info/ContactScreen.h"

#include "ui/theme/Theme.h"

#include <QDesktopServices>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

namespace fincept::screens {

using namespace fincept::ui;

// ── Style constants ──────────────────────────────────────────────────────────

static QString PANEL() {
    return QString("background: %1; border: 1px solid %2; border-radius: 2px;")
        .arg(colors::BG_SURFACE(), colors::BORDER_DIM());
}

static const char* MF = "font-family:'Consolas','Courier New',monospace;";

static QLabel* make_header(const QString& icon, const QString& title, const QString& icon_color) {
    auto* lbl = new QLabel(QString("%1  %2").arg(icon, title));
    lbl->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: bold; letter-spacing: 0.5px; "
                               "background: %2; padding: 10px 14px; border-bottom: 1px solid %3; %4")
                           .arg(icon_color, colors::BG_RAISED(), colors::BORDER_DIM(), MF));
    return lbl;
}

static QWidget* make_contact_card(const QString& title, const QString& value, const QString& detail) {
    auto* card = new QWidget(nullptr);
    card->setStyleSheet(PANEL());
    auto* vl = new QVBoxLayout(card);
    vl->setContentsMargins(14, 12, 14, 12);
    vl->setSpacing(4);

    auto* t = new QLabel(title);
    t->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: bold; "
                             "letter-spacing: 0.5px; background: transparent; %2")
                         .arg(colors::TEXT_SECONDARY(), MF));
    vl->addWidget(t);

    auto* v = new QLabel(value);
    v->setStyleSheet(QString("color: %1; font-size: 13px; font-weight: 600; background: transparent; %2")
                         .arg(colors::TEXT_PRIMARY(), MF));
    vl->addWidget(v);

    auto* d = new QLabel(detail);
    d->setStyleSheet(
        QString("color: %1; font-size: 11px; background: transparent; %2").arg(colors::TEXT_TERTIARY(), MF));
    d->setWordWrap(true);
    vl->addWidget(d);

    return card;
}

// ── Constructor ──────────────────────────────────────────────────────────────

ContactScreen::ContactScreen(QWidget* parent) : QWidget(parent) {
    setStyleSheet(QString("QWidget#ContactRoot { background: %1; }").arg(colors::BG_BASE()));
    setObjectName("ContactRoot");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    scroll_ = new QScrollArea;
    scroll_->setWidgetResizable(true);
    scroll_->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    scroll_->setWidget(build_page());
    root->addWidget(scroll_, 1);
}

// ── Re-translation ────────────────────────────────────────────────────────────
// Static-content screen with no live state — on language change we rebuild
// the page from scratch rather than caching every label/button as a member.
// QScrollArea::setWidget() takes ownership and deletes the previous content.

void ContactScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange && scroll_) {
        scroll_->setWidget(build_page());
    }
    QWidget::changeEvent(event);
}

// ── Page builder ──────────────────────────────────────────────────────────────

QWidget* ContactScreen::build_page() {
    auto* page = new QWidget(this);
    page->setStyleSheet(QString("background: %1;").arg(colors::BG_BASE()));
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(24, 24, 24, 24);
    vl->setSpacing(12);

    // ── Header ───────────────────────────────────────────────────────────────
    auto* back_btn = new QPushButton(tr("< BACK"));
    back_btn->setCursor(Qt::PointingHandCursor);
    back_btn->setStyleSheet(QString("QPushButton { color: %1; background: transparent; border: none; "
                                    "font-size: 12px; %2 } QPushButton:hover { color: %3; }")
                                .arg(colors::TEXT_SECONDARY(), MF, colors::TEXT_PRIMARY()));
    connect(back_btn, &QPushButton::clicked, this, &ContactScreen::navigate_back);
    vl->addWidget(back_btn, 0, Qt::AlignLeft);

    auto* title = new QLabel(tr("CONTACT US"));
    title->setStyleSheet(QString("color: %1; font-size: 20px; font-weight: 700; letter-spacing: 1px; "
                                 "background: transparent; %2")
                             .arg(colors::AMBER(), MF));
    vl->addWidget(title);

    auto* subtitle = new QLabel(tr("This fork has no commercial support channel"));
    subtitle->setStyleSheet(
        QString("color: %1; font-size: 13px; background: transparent; %2").arg(colors::TEXT_TERTIARY(), MF));
    vl->addWidget(subtitle);

    vl->addSpacing(8);

    // ── Contact Information ──────────────────────────────────────────────────
    // MarketLab: plain-text identity only — no email handlers, no Discord, no
    // GitHub issue links, no browser launches (FINCEPT_FORK_PLAN.md §4, §5.3).
    {
        auto* panel = new QWidget(this);
        panel->setStyleSheet(PANEL());
        auto* pvl = new QVBoxLayout(panel);
        pvl->setContentsMargins(0, 0, 0, 0);
        pvl->setSpacing(0);

        pvl->addWidget(make_header("@", tr("CONTACT INFORMATION"), colors::AMBER));

        auto* body = new QWidget(this);
        body->setStyleSheet("background: transparent;");
        auto* grid = new QGridLayout(body);
        grid->setContentsMargins(14, 12, 14, 12);
        grid->setSpacing(10);

        grid->addWidget(make_contact_card(tr("FORK REPOSITORY"), tr("github.com/Rady70/FinceptTerminal"),
                                          tr("Source of this personal fork")),
                        0, 0);
        grid->addWidget(make_contact_card(tr("UPSTREAM"), tr("Fincept Terminal v4.5.0 (ec88590)"),
                                          tr("Released application-code baseline, AGPL-3.0-or-later")),
                        0, 1);
        grid->addWidget(
            make_contact_card(tr("DOCUMENTATION"), tr("Docs screen"), tr("Bundled documentation inside the terminal")),
            1, 0);
        grid->addWidget(make_contact_card(tr("DIAGNOSTICS"), tr("About → Diagnostics"),
                                          tr("Crash dumps and state locations are listed in the About screen")),
                        1, 1);

        pvl->addWidget(body);
        vl->addWidget(panel);
    }

    // ── Quick Actions ────────────────────────────────────────────────────────
    // MarketLab: the upstream email/discord/github actions are removed; the
    // page carries only static identity text.
    {
        auto* panel = new QWidget(this);
        panel->setStyleSheet(PANEL());
        auto* pvl = new QVBoxLayout(panel);
        pvl->setContentsMargins(0, 0, 0, 0);
        pvl->setSpacing(0);

        pvl->addWidget(make_header(">>", tr("QUICK ACTIONS"), colors::CYAN));

        auto* body = new QWidget(this);
        body->setStyleSheet("background: transparent;");
        auto* hl = new QHBoxLayout(body);
        hl->setContentsMargins(14, 12, 14, 12);
        hl->setSpacing(10);

        auto* note = new QLabel(tr("MarketLab Terminal is a personal, local-first research build of Fincept "
                                   "Terminal. It is not affiliated with Fincept Corporation, offers no support "
                                   "channel, and opens no external contact links."));
        note->setWordWrap(true);
        note->setStyleSheet(
            QString("color: %1; font-size: 12px; background: transparent; %2").arg(colors::TEXT_SECONDARY(), MF));
        hl->addWidget(note, 1);

        pvl->addWidget(body);
        vl->addWidget(panel);
    }

    // ── Common Issues ────────────────────────────────────────────────────────
    {
        auto* panel = new QWidget(this);
        panel->setStyleSheet(PANEL());
        auto* pvl = new QVBoxLayout(panel);
        pvl->setContentsMargins(0, 0, 0, 0);
        pvl->setSpacing(0);

        pvl->addWidget(make_header("?", tr("COMMON ISSUES"), colors::AMBER));

        auto* body = new QWidget(this);
        body->setStyleSheet("background: transparent;");
        auto* bvl = new QVBoxLayout(body);
        bvl->setContentsMargins(14, 10, 14, 12);
        bvl->setSpacing(6);

        struct Issue {
            QString q;
            QString a;
        };
        const Issue issues[] = {
            {tr("Python setup fails or times out"),
             tr("Ensure you have a stable internet connection. Retry setup or check firewall settings.")},
            {tr("Data not loading or showing stale"),
             tr("Check your internet connection. Try refreshing the screen or restarting the terminal.")},
            {tr("A screen is missing or unavailable"),
             tr("Unavailable screens are listed with their reasons in About → Capabilities and are "
                "deliberately unreachable in this build.")},
        };

        for (const auto& issue : issues) {
            auto* q = new QLabel(QString("> %1").arg(issue.q));
            q->setStyleSheet(QString("color: %1; font-size: 12px; font-weight: 600; background: transparent; %2")
                                 .arg(colors::TEXT_PRIMARY(), MF));
            bvl->addWidget(q);

            auto* a = new QLabel(QString("  %1").arg(issue.a));
            a->setWordWrap(true);
            a->setStyleSheet(
                QString("color: %1; font-size: 11px; background: transparent; %2").arg(colors::TEXT_TERTIARY(), MF));
            bvl->addWidget(a);
            bvl->addSpacing(4);
        }

        pvl->addWidget(body);
        vl->addWidget(panel);
    }

    vl->addStretch();
    return page;
}

} // namespace fincept::screens
