#include "tuple/Tuple.h"

#include <cassert>

namespace tuple {

void Tuple::add_column(const Column& column) {
    columns_.push_back(column);
}

void Tuple::add(int value) {
    add_column(Column(value));
}

void Tuple::add(double value) {
    add_column(Column(value));
}

void Tuple::add(const std::string& value) {
    add_column(Column(value));
}

const Column& Tuple::column(std::size_t index) const {
    assert(index < columns_.size() && "tuple column index out of range");
    return columns_[index];
}

}
