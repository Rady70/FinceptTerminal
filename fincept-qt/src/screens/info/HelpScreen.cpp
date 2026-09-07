#include "screens/info/HelpScreen.h"

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

static const char* MF = "font-family:'Consolas','Courier New',monospace;";

// ── Collapsible FAQ item ──────────────────────────────────────────────────────

static QWidget* make_faq(const QString& question, const QString& answer, const QString& icon = "?") {
    auto* container = new QWidget(nullptr);
    container->setStyleSheet("background: transparent;");
    auto* vl = new QVBoxLayout(container);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    // The disclosure arrow is part of the label: the previous version computed
    // an arrow into a local and then Q_UNUSED'd it, so the button text never
    // showed whether the answer was open or closed.
    auto btn_text = [question, icon](bool open) {
        const QString arrow = open ? QStringLiteral("▾") : QStringLiteral("▸");
        return icon.isEmpty() ? QString("  %1  %2").arg(arrow, question)
                              : QString("  %1  %2  %3").arg(arrow, icon, question);
    };

    auto* q_btn = new QPushButton(btn_text(false));
    q_btn->setCursor(Qt::PointingHandCursor);
    q_btn->setAccessibleName(question);
    q_btn->setStyleSheet(QString("QPushButton { color: %1; background: %2; border: 1px solid %3;"
                                 " padding: 11px 14px; text-align: left;"
                                 " font-size: 12px; font-weight: 600; %4 }"
                                 "QPushButton:hover { background: %5; border-color: %6; color: %7; }")
                             .arg(colors::TEXT_PRIMARY(), colors::BG_SURFACE(), colors::BORDER_DIM(), MF,
                                  colors::BG_RAISED(), colors::AMBER(), colors::AMBER()));

    auto* a_lbl = new QLabel(answer);
    a_lbl->setWordWrap(true);
    a_lbl->setStyleSheet(
        QString("color: %1; font-size: 12px; background: %2;"
                " border: 1px solid %3; border-top: none;"
                " border-left: 3px solid %4;"
                " padding: 12px 16px 12px 18px; %5")
            .arg(colors::TEXT_SECONDARY(), colors::BG_SURFACE(), colors::BORDER_DIM(), colors::AMBER(), MF));
    a_lbl->setVisible(false);

    QObject::connect(q_btn, &QPushButton::clicked, a_lbl, [a_lbl, q_btn, btn_text]() {
        bool show = !a_lbl->isVisible();
        a_lbl->setVisible(show);
        q_btn->setText(btn_text(show));
        // tint open state
        q_btn->setStyleSheet(
            QString("QPushButton { color: %1; background: %2; border: 1px solid %3;"
                    " border-left: 3px solid %3;"
                    " padding: 11px 14px; text-align: left;"
                    " font-size: 12px; font-weight: 600; font-family:'Consolas','Courier New',monospace; }"
                    "QPushButton:hover { background: %4; }")
                .arg(show ? colors::AMBER() : colors::TEXT_PRIMARY(), show ? colors::BG_RAISED() : colors::BG_SURFACE(),
                     show ? colors::AMBER() : colors::BORDER_DIM(), colors::BG_RAISED()));
    });

    vl->addWidget(q_btn);
    vl->addWidget(a_lbl);
    return container;
}

// ── Section header ────────────────────────────────────────────────────────────

static QWidget* section_header(const QString& title, const QString& subtitle = {}) {
    auto* w = new QWidget(nullptr);
    w->setStyleSheet("background: transparent;");
    auto* vl = new QVBoxLayout(w);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(3);

    auto* t = new QLabel(title);
    t->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: bold; letter-spacing: 1px;"
                             " background: transparent; %2")
                         .arg(colors::AMBER(), MF));
    vl->addWidget(t);

    if (!subtitle.isEmpty()) {
        auto* s = new QLabel(subtitle);
        s->setStyleSheet(
            QString("color: %1; font-size: 11px; background: transparent; %2").arg(colors::TEXT_TERTIARY(), MF));
        vl->addWidget(s);
    }
    return w;
}

// ── Constructor ───────────────────────────────────────────────────────────────

HelpScreen::HelpScreen(QWidget* parent) : QWidget(parent) {
    setStyleSheet(QString("QWidget#HelpRoot { background: %1; }").arg(colors::BG_BASE()));
    setObjectName("HelpRoot");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    scroll_ = new QScrollArea;
    scroll_->setWidgetResizable(true);
    scroll_->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    scroll_->setWidget(build_page());
    root->addWidget(scroll_, 1);

    // ── Theme wiring ──────────────────────────────────────────────────────────
    connect(&ui::ThemeManager::instance(), &ui::ThemeManager::theme_changed, this, [this](const ui::ThemeTokens&) {
        setStyleSheet(QString("QWidget#HelpRoot { background: %1; }").arg(colors::BG_BASE()));
    });
}

// ── Re-translation ────────────────────────────────────────────────────────────
// Static-content screen with no live state — on language change we rebuild
// the page from scratch rather than caching every label/button as a member.
// QScrollArea::setWidget() takes ownership and deletes the previous content.

void HelpScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange && scroll_) {
        scroll_->setWidget(build_page());
    }
    QWidget::changeEvent(event);
}

// ── Page builder ──────────────────────────────────────────────────────────────

QWidget* HelpScreen::build_page() {
    auto* page = new QWidget;
    page->setStyleSheet(QString("background: %1;").arg(colors::BG_BASE()));
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(28, 24, 28, 32);
    vl->setSpacing(0);

    // ── Hero banner ───────────────────────────────────────────────────────────
    {
        auto* hero = new QWidget(page);
        hero->setStyleSheet(QString("background: %1; border: 1px solid %2; border-left: 4px solid %3;")
                                .arg(colors::BG_SURFACE(), colors::BORDER_DIM(), colors::AMBER()));
        auto* hl = new QHBoxLayout(hero);
        hl->setContentsMargins(20, 18, 20, 18);
        hl->setSpacing(16);

        auto* text_vl = new QVBoxLayout;
        text_vl->setSpacing(5);

        auto* title = new QLabel(tr("HELP CENTER"));
        title->setStyleSheet(QString("color: %1; font-size: 22px; font-weight: 700; letter-spacing: 2px;"
                                     " background: transparent; %2")
                                 .arg(colors::AMBER(), MF));
        text_vl->addWidget(title);

        auto* sub = new QLabel(tr("Local-first research workspace — bundled documentation and usage notes."));
        sub->setStyleSheet(
            QString("color: %1; font-size: 12px; background: transparent; %2").arg(colors::TEXT_SECONDARY(), MF));
        text_vl->addWidget(sub);

        hl->addLayout(text_vl, 1);

        // MarketLab: the Fincept email / Discord / GitHub contact chips are
        // replaced by plain text — no external-browser launches exist in this
        // fork (FINCEPT_FORK_PLAN.md §4, §5.3).
        auto* chips_vl = new QVBoxLayout;
        chips_vl->setSpacing(5);

        auto make_chip = [](const QString& icon, const QString& text, const QString& color,
                            const QString& url = {}) -> QWidget* {
            Q_UNUSED(url);
            auto* chip = new QLabel(icon.isEmpty() ? text : QString("%1  %2").arg(icon, text));
            chip->setStyleSheet(QString("color: %1; font-size: 11px; background: transparent;"
                                        " font-family:'Consolas','Courier New',monospace;")
                                    .arg(color));
            return chip;
        };
        chips_vl->addWidget(make_chip("", tr("No account, subscription, or hosted service required"), colors::POSITIVE));
        chips_vl->addWidget(
            make_chip("", tr("Fork repository: github.com/Rady70/FinceptTerminal"), colors::TEXT_TERTIARY));
        chips_vl->addWidget(
            make_chip("", tr("Upstream: Fincept Terminal v4.5.0 (AGPL-3.0-or-later)"), colors::TEXT_TERTIARY));
        hl->addLayout(chips_vl);

        vl->addWidget(hero);
    }

    vl->addSpacing(20);

    // ── Quick Actions ─────────────────────────────────────────────────────────
    {
        vl->addWidget(section_header(tr("QUICK ACTIONS"), tr("Common tasks you can do right now")));
        vl->addSpacing(8);

        auto* grid = new QGridLayout;
        grid->setSpacing(8);

        // Each Action carries a stable English `key` (used to wire signals)
        // separate from the translated label/desc strings.
        struct Action {
            const char* icon;
            const char* key;
            QString label;
            QString desc;
        };
        // MarketLab: quick actions are local only — no account actions, no
        // external links (FINCEPT_FORK_PLAN.md §4, §5.3, §6).
        const Action actions[] = {
            {"", "documentation", tr("Documentation"), tr("Bundled docs — open the Docs screen")},
            {"", "settings", tr("Settings"), tr("Configure data sources, LLM providers, and appearance")},
            {"", "about", tr("About"), tr("Fork identity, upstream base, and capabilities")},
        };

        int col = 0, row = 0;
        for (const auto& a : actions) {
            auto* btn = new QPushButton;
            btn->setCursor(Qt::PointingHandCursor);
            btn->setFixedHeight(60);
            btn->setStyleSheet(QString("QPushButton { background: %1; color: %2; border: 1px solid %3;"
                                       " text-align: left; padding: 0; }"
                                       "QPushButton:hover { background: %4; border-color: %5; }")
                                   .arg(colors::BG_SURFACE(), colors::TEXT_PRIMARY(), colors::BORDER_DIM(),
                                        colors::BG_RAISED(), colors::AMBER()));

            auto* bl = new QVBoxLayout(btn);
            bl->setContentsMargins(12, 8, 12, 8);
            bl->setSpacing(3);

            auto* top = new QHBoxLayout;
            top->setSpacing(7);
            if (a.icon[0]) {
                auto* icon_lbl = new QLabel(QString::fromUtf8(a.icon));
                icon_lbl->setStyleSheet("background: transparent; font-size: 14px;");
                top->addWidget(icon_lbl);
            }
            auto* name_lbl = new QLabel(a.label);
            name_lbl->setStyleSheet(QString("background: transparent; color: %1; font-size: 12px;"
                                            " font-weight: bold; %2")
                                        .arg(colors::TEXT_PRIMARY(), MF));
            top->addWidget(name_lbl);
            top->addStretch();
            bl->addLayout(top);

            auto* desc_lbl = new QLabel(a.desc);
            desc_lbl->setStyleSheet(
                QString("background: transparent; color: %1; font-size: 10px; %2").arg(colors::TEXT_TERTIARY(), MF));
            bl->addWidget(desc_lbl);

            if (col == 3) {
                col = 0;
                ++row;
            }
            grid->addWidget(btn, row, col++);
            btn->setAccessibleName(a.label);
            btn->setToolTip(a.desc);

            const QString key = QString::fromLatin1(a.key);
            if (key == "documentation")
                connect(btn, &QPushButton::clicked, this, &HelpScreen::navigate_docs);
            else if (key == "settings")
                connect(btn, &QPushButton::clicked, this, &HelpScreen::navigate_settings);
            else if (key == "about")
                connect(btn, &QPushButton::clicked, this, &HelpScreen::navigate_about);
        }

        vl->addLayout(grid);
    }

    vl->addSpacing(24);

    // ── FAQ ───────────────────────────────────────────────────────────────────
    {
        vl->addWidget(section_header(tr("FREQUENTLY ASKED QUESTIONS"), tr("Click a question to expand the answer")));
        vl->addSpacing(8);

        struct FAQ {
            const char* icon;
            QString q;
            QString a;
        };
        const FAQ faqs[] = {
            {"", tr("Do I need a Fincept account?"),
             tr("No. MarketLab Terminal is a local-first fork: the workspace opens without any "
                "account, subscription, or hosted service.")},

            {"", tr("Where does market data come from?"),
             tr("Public market data (quotes, history, symbol search) comes from independently "
                "configured public providers such as Yahoo Finance via the bundled Python "
                "scripts. Sources and retrieval status are displayed with each result.")},

            {"", tr("Can I place live orders?"),
             tr("No. This fork exposes no external broker or exchange order route. "
                "Historical simulation and paper backtests remain available in Backtesting.")},

            {"", tr("Why does Python install at first launch?"),
             tr("The terminal embeds a Python 3.11 runtime for its public-data and analytics "
                "scripts. The one-time install happens automatically at first start.")},

            {"", tr("What are the system requirements?"),
             tr("Windows 10+ (x64). 8 GB RAM recommended. Internet access is required only for "
                "public data feeds; the local workspace opens without any network.")},

            {"", tr("Is my data secure?"),
             tr("Credentials (if you configure any third-party provider) are stored encrypted via "
                "SecureStorage. No Fincept-hosted service is ever contacted, and provider keys "
                "are used only for direct connections from your machine.")},

            {"", tr("Where is my data stored?"),
             tr("All state lives under %LOCALAPPDATA%\\com.marketlab.terminal (per profile: data, "
                "logs, cache, files, workspaces). The official Fincept profile is never read, "
                "migrated, or modified.")},
        };

        for (const auto& f : faqs)
            vl->addWidget(make_faq(f.q, f.a, QString::fromUtf8(f.icon)));

        vl->addSpacing(24);
    }

    // ── Getting Started ────────────────────────────────────────────────────────
    {
        vl->addWidget(section_header(tr("GETTING STARTED"), tr("First steps with MarketLab Terminal")));
        vl->addSpacing(8);

        struct Step {
            const char* num;
            QString title;
            QString detail;
        };
        const Step steps[] = {
            {"1", tr("Complete first-time setup"), tr("The setup wizard installs the bundled Python runtime.")},
            {"2", tr("Open the workspace"), tr("The local dashboard opens directly — no account or login.")},
            {"3", tr("Fetch public data"), tr("Use Markets, Watchlist, or Equity Research for quotes and history.")},
            {"4", tr("Configure providers"), tr("Set up LLM or data providers in Settings — all optional and local.")},
        };

        auto* steps_widget = new QWidget(page);
        steps_widget->setStyleSheet(
            QString("background: %1; border: 1px solid %2;").arg(colors::BG_SURFACE(), colors::BORDER_DIM()));
        auto* swl = new QVBoxLayout(steps_widget);
        swl->setContentsMargins(0, 0, 0, 0);
        swl->setSpacing(0);

        for (int i = 0; i < 4; ++i) {
            const auto& s = steps[i];
            auto* row = new QWidget(steps_widget);
            bool last = (i == 3);
            row->setStyleSheet(QString("background: transparent; border-bottom: %1;")
                                   .arg(last ? "none" : QString("1px solid %1;").arg(colors::BORDER_DIM())));
            auto* rl = new QHBoxLayout(row);
            rl->setContentsMargins(16, 12, 16, 12);
            rl->setSpacing(14);

            auto* num = new QLabel(s.num);
            num->setFixedSize(26, 26);
            num->setAlignment(Qt::AlignCenter);
            num->setStyleSheet(QString("background: %1; color: %2; font-size: 12px; font-weight: bold;"
                                       " border-radius: 13px; %3")
                                   .arg(colors::AMBER(), colors::BG_BASE(), MF));
            rl->addWidget(num);

            auto* txt_vl = new QVBoxLayout;
            txt_vl->setSpacing(2);
            auto* ttl = new QLabel(s.title);
            ttl->setStyleSheet(QString("color: %1; font-size: 12px; font-weight: bold;"
                                       " background: transparent; %2")
                                   .arg(colors::TEXT_PRIMARY(), MF));
            auto* det = new QLabel(s.detail);
            det->setStyleSheet(
                QString("color: %1; font-size: 11px; background: transparent; %2").arg(colors::TEXT_SECONDARY(), MF));
            det->setWordWrap(true);
            txt_vl->addWidget(ttl);
            txt_vl->addWidget(det);
            rl->addLayout(txt_vl, 1);

            swl->addWidget(row);
        }

        vl->addWidget(steps_widget);
        vl->addSpacing(24);
    }

    // ── Contact & Resources ───────────────────────────────────────────────────
    // MarketLab: plain-text identity only — no external links, email handlers,
    // or browser launches (FINCEPT_FORK_PLAN.md §4, §5.3).
    {
        vl->addWidget(section_header(tr("CONTACT & RESOURCES")));
        vl->addSpacing(8);

        auto* grid2 = new QGridLayout;
        grid2->setSpacing(8);

        struct Contact {
            const char* icon;
            QString label;
            QString value;
        };
        const Contact contacts[] = {
            {"", tr("Fork repository"), tr("github.com/Rady70/FinceptTerminal (source only)")},
            {"", tr("Upstream"), tr("Fincept Terminal v4.5.0 — commit ec88590, AGPL-3.0-or-later")},
            {"", tr("Support"), tr("Personal research build — no commercial support channel")},
            {"", tr("Docs"), tr("Bundled Docs screen and About → Diagnostics")},
        };

        int ci = 0;
        for (const auto& c : contacts) {
            auto* card = new QWidget(page);
            card->setStyleSheet(
                QString("background: %1; border: 1px solid %2;").arg(colors::BG_SURFACE(), colors::BORDER_DIM()));
            auto* cl = new QHBoxLayout(card);
            cl->setContentsMargins(14, 12, 14, 12);
            cl->setSpacing(10);

            if (c.icon[0]) {
                auto* ico = new QLabel(QString::fromUtf8(c.icon));
                ico->setStyleSheet("background: transparent; font-size: 18px;");
                cl->addWidget(ico);
            }

            auto* tvl = new QVBoxLayout;
            tvl->setSpacing(2);
            auto* lbl = new QLabel(c.label);
            lbl->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: bold; background: transparent; %2")
                                   .arg(colors::TEXT_TERTIARY(), MF));

            auto* val = new QLabel(c.value);
            val->setWordWrap(true);
            val->setStyleSheet(QString("color: %1; font-size: 11px; background: transparent; %2")
                                   .arg(colors::CYAN(), MF));

            tvl->addWidget(lbl);
            tvl->addWidget(val);
            cl->addLayout(tvl, 1);

            grid2->addWidget(card, ci / 2, ci % 2);
            ++ci;
        }

        vl->addLayout(grid2);
    }

    vl->addStretch();
    return page;
}

} // namespace fincept::screens
