#pragma once
// DeveloperSection.h — DataHub Inspector and other devtools surfaces.

#include <QEvent>
#include <QLabel>
#include <QWidget>

namespace fincept::screens {

class DeveloperSection : public QWidget {
    Q_OBJECT
  public:
    explicit DeveloperSection(QWidget* parent = nullptr);

  protected:
    void changeEvent(QEvent* event) override;

  private:
    /// Re-apply tr() lookups to every widget whose text we keep a handle to.
    /// Called from changeEvent() on QEvent::LanguageChange.
    void retranslateUi();

    QLabel* inspector_title_ = nullptr;
    QLabel* inspector_desc_ = nullptr;
};

} // namespace fincept::screens
