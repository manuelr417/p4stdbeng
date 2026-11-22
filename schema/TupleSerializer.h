#ifndef EX11_TUPLE_SERIALIZER_H
#define EX11_TUPLE_SERIALIZER_H

#include "schema/TableSchema.h"
#include "tuple/Tuple.h"

#include <cstddef>
#include <string>

namespace schema {

/// The one serializer in the engine, bound to one table schema at
/// construction: the schema supplies everything structural — the arity,
/// each column's type, and each String's declared width — so the record
/// bytes are values only, laid out in the schema's creation order. There
/// is no per-call schema parameter; every method works against the bound
/// schema. The bound schema must outlive the serializer, and it must have
/// a record layout (record_size() > 0); record buffers hold exactly
/// record_size() bytes. Data that does not fit the schema throws.
class TupleSerializer {
public:
    explicit TupleSerializer(const TableSchema& schema);

    /// Total fixed record width of the bound schema: 4 bytes per Integer
    /// column, 8 per Double, column_size per String. Returns 0 for an
    /// empty schema or one with undeclared-width String columns — such a
    /// schema has no record layout.
    std::size_t record_size() const;

    /// Validates `tuple` against the bound schema (arity, types, widths,
    /// NUL) and packs it into `record`. Throws before touching the buffer.
    void serialize(const tuple::Tuple& tuple, char* record) const;

    /// Reads a record written by serialize(): reassembles the logical
    /// tuple in creation order, verifies that every String's padding is
    /// all NUL, and validates the result against the schema.
    tuple::Tuple deserialize(const char* record) const;

private:
    const TableSchema& schema_; // must outlive the serializer
};

}

#endif // EX11_TUPLE_SERIALIZER_H
