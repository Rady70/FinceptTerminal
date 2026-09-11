#pragma once

#include <QTableWidgetItem>

namespace fincept::ui {

// A QTableWidgetItem that SORTS by a numeric value while DISPLAYING arbitrary
// (formatted) text. Plain QTableWidgetItem::operator< compares the DisplayRole
// as a string, so "10" sorts before "9" and "$1,000" before "$200" — wrong for
// any numeric column. Use this for price / quantity / ratio / percent cells so
// header-click sorting orders by magnitude.
//
// The presence flag keeps a missing reading out of the numeric ordering: a row
// whose cell shows "--" has no value to compare, so it must not sort as a 0
// next to a genuine zero. Missing cells sort after present ones (ascending).
//
//   table->setItem(row, col, new ui::NumericTableWidgetItem(QString::number(v,'f',2), v));
class NumericTableWidgetItem : public QTableWidgetItem {
  public:
    NumericTableWidgetItem(const QString& display_text, double value, bool has_value = true)
        : QTableWidgetItem(display_text), value_(value), has_value_(has_value) {}

    bool operator<(const QTableWidgetItem& other) const override {
        if (const auto* o = dynamic_cast<const NumericTableWidgetItem*>(&other)) {
            if (has_value_ != o->has_value_)
                return has_value_; // a present reading sorts before a missing one
            if (!has_value_)
                return false; // two missing cells are equivalent
            return value_ < o->value_;
        }
        return QTableWidgetItem::operator<(other);
    }

    double numeric_value() const { return value_; }
    bool has_numeric_value() const { return has_value_; }

    void set_numeric_value(double value, bool has_value) {
        value_ = value;
        has_value_ = has_value;
    }

  private:
    double value_ = 0.0;
    bool has_value_ = true;
};

} // namespace fincept::ui
