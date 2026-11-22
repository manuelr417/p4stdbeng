#include "schema/TupleSerializer.h"

#include "common/Error.h"

#include <cassert>
#include <cstring>

namespace schema {

namespace {

// The width of one column in a record: natural size for numerics, the
// declared width for Strings. 0 flags an undeclared String, which cannot
// live in a record.
std::size_t column_width(const ColumnSchema& column) {
    switch (column.column_type()) {
        case tuple::ColumnType::Integer: return 4;
        case tuple::ColumnType::Double:  return 8;
        case tuple::ColumnType::String:  return column.column_size();
    }
    return 0;
}

}

TupleSerializer::TupleSerializer(const TableSchema& schema)
    : schema_(schema) {}

std::size_t TupleSerializer::record_size() const {
    if (schema_.column_count() == 0) {
        return 0;
    }
    std::size_t total = 0;
    for (std::size_t i = 0; i < schema_.column_count(); ++i) {
        const std::size_t width = column_width(schema_.column_info(i));
        if (width == 0) {
            return 0; // an undeclared-width String has no record layout
        }
        total += width;
    }
    return total;
}

void TupleSerializer::serialize(const tuple::Tuple& tuple, char* record) const {
    assert(record_size() > 0 && "the schema has no record layout");
    schema_.validate(tuple);
    // Column i is wire position i and tuple slot i: creation order is the
    // one order everywhere. The schema is the layout, so only values are
    // written.
    char* field = record;
    for (std::size_t i = 0; i < schema_.column_count(); ++i) {
        const tuple::Column& column = tuple.column(i);
        const std::size_t width = column_width(schema_.column_info(i));
        switch (column.column_type()) {
            case tuple::ColumnType::Integer: {
                const int value = std::get<int>(column.column_value());
                std::memcpy(field, &value, 4);
                break;
            }
            case tuple::ColumnType::Double: {
                const double value = std::get<double>(column.column_value());
                std::memcpy(field, &value, 8);
                break;
            }
            case tuple::ColumnType::String: {
                const std::string& value = std::get<std::string>(column.column_value());
                std::memcpy(field, value.data(), value.size());
                std::memset(field + value.size(), 0, width - value.size());
                break;
            }
        }
        field += width;
    }
}

tuple::Tuple TupleSerializer::deserialize(const char* record) const {
    assert(record_size() > 0 && "the schema has no record layout");
    tuple::Tuple tuple;
    const char* field = record;
    for (std::size_t i = 0; i < schema_.column_count(); ++i) {
        const ColumnSchema& meta = schema_.column_info(i);
        const std::size_t width = column_width(meta);
        switch (meta.column_type()) {
            case tuple::ColumnType::Integer: {
                int value = 0;
                std::memcpy(&value, field, 4);
                tuple.add(value);
                break;
            }
            case tuple::ColumnType::Double: {
                double value = 0.0;
                std::memcpy(&value, field, 8);
                tuple.add(value);
                break;
            }
            case tuple::ColumnType::String: {
                // Value bytes up to the first NUL; everything behind it
                // must be padding. A value filling the whole width has no
                // NUL and no padding — both are fine.
                const std::size_t length = ::strnlen(field, width);
                for (std::size_t k = length; k < width; ++k) {
                    if (field[k] != '\0') {
                        throw common::Error("column '" + meta.column_name() +
                                            "': padding contains garbage");
                    }
                }
                tuple.add(std::string(field, length));
                break;
            }
        }
        field += width;
    }
    schema_.validate(tuple);
    return tuple;
}

}
