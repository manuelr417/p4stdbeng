#ifndef EX11_TABLE_CSV_H
#define EX11_TABLE_CSV_H

#include "schema/TableSchema.h"
#include "tuple/Tuple.h"

#include <cstddef>
#include <ostream>
#include <string>
#include <vector>

namespace schema {

/// One parsed CSV line as a schema-shaped tuple. Throws a named reason
/// (no line prefix — the caller adds "line N: ").
tuple::Tuple parse_csv_row(const TableSchema& schema, const std::string& line);

/// Everything one CSV load reports: the valid rows in file order and how
/// many lines were skipped.
struct CsvLoadResult {
    std::vector<tuple::Tuple> tuples;
    std::size_t skipped = 0;
};

/// Loads CSV rows shaped by `schema`: one record per line, comma-separated
/// with one token per column in position order. Invalid rows print
/// "line N: reason" to `diagnostics` and are skipped, never fatal; blank
/// lines are skipped silently; a trailing \r is stripped. Throws only
/// when the file itself cannot be read.
CsvLoadResult load_csv(const TableSchema& schema, const std::string& path,
                       std::ostream& diagnostics);

}

#endif // EX11_TABLE_CSV_H
