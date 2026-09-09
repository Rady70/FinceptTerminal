// DeveloperSection.cpp — DataHub Inspector.

#include "screens/settings/DeveloperSection.h"

#include "screens/devtools/DataHubInspector.h"
#include "screens/settings/SettingsStyles.h"
#include "ui/theme/Theme.h"

#include <QLabel>
#include <QString>
#include <QVBoxLayout>

namespace fincept::screens {

DeveloperSection::DeveloperSection(QWidget* parent) : QWidget(parent) {
    using namespace settings_styles;

    auto* vl = new QVBoxLayout(this);
    vl->setContentsMargins(16, 16, 16, 16);
    vl->setSpacing(12);

    // ── DataHub Inspector ────────────────────────────────────────────────────
    inspector_title_ = new QLabel(tr("DataHub Inspector"));
    inspector_title_->setStyleSheet(section_title_ss());
    vl->addWidget(inspector_title_);

    inspector_desc_ = new QLabel(tr("Live view over the in-process pub/sub layer. Shows every active topic, its "
                                    "subscriber count, total publishes, and time since last publish. Refreshes "
                                    "once per second while this tab is visible."));
    inspector_desc_->setWordWrap(true);
    inspector_desc_->setStyleSheet(QString("color:%1;font-size:11px;").arg(ui::colors::TEXT_SECONDARY()));
    vl->addWidget(inspector_desc_);

    vl->addWidget(new devtools::DataHubInspector(this), 1);
}

void DeveloperSection::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void DeveloperSection::retranslateUi() {
    if (inspector_title_)
        inspector_title_->setText(tr("DataHub Inspector"));
    if (inspector_desc_)
        inspector_desc_->setText(tr("Live view over the in-process pub/sub layer. Shows every active topic, its "
                                    "subscriber count, total publishes, and time since last publish. Refreshes "
                                    "once per second while this tab is visible."));
}

} // namespace fincept::screens
