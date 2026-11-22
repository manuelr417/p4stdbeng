#ifndef EX11_TABLE_SCHEMA_JSON_H
#define EX11_TABLE_SCHEMA_JSON_H

#include "schema/TableSchema.h"

#include <optional>
#include <string>
#include <vector>

namespace schema {

/// One declared index in the catalog's optional "indexes" array: the
/// index's name, the one column it orders, and the index type ("ISAM"
/// or "BTREE" — absent means ISAM, the shape every pre-B+tree catalog
/// wrote).
struct TableIndex {
    std::string name;
    std::string column;
    std::string type = "ISAM";
};

/// One declared foreign key in the catalog's optional "foreign_keys"
/// array: the referencing column of this table, the parent table's
/// name, and the parent column it references — always the parent's
/// declared primary key (validated when the table is created, not
/// re-checked here).
struct TableForeignKey {
    std::string column;
    std::string parent_table;
    std::string parent_column;
};

/// A table's whole catalog: the schema plus its declared indexes, plus
/// the optional primary key and its declared foreign keys. `primary_key`
/// names one column when the table declares the constraint; `foreign_keys`
/// is empty for tables without references. Catalogs written before
/// primary keys and foreign keys existed carry neither and load with
/// defaults.
struct Catalog {
    TableSchema schema;
    std::vector<TableIndex> indexes;
    std::optional<std::string> primary_key;
    std::vector<TableForeignKey> foreign_keys;
};

/// Parse catalog text — the JSON shape of tablecatalog/person.json: an
/// object with "table_name" and a "columns" array of objects, each with
/// "column_name", "column_type" (Integer/String/Double), and "size". The
/// optional "indexes" array holds objects with "name", "column", and an
/// optional "type"; a catalog without one loads with no indexes. Throws
/// a named error ("line N: ...") on malformed text.
Catalog parse_catalog(const std::string& text);

/// Read the file at `path` and parse_catalog it. A missing or unreadable
/// file throws like any other error.
Catalog load_catalog(const std::string& path);

/// The schema-only forms: same parse/load, indexes discarded.
TableSchema parse_table_schema(const std::string& text);
TableSchema load_table_schema(const std::string& path);

/// Serialize `schema`, `indexes`, the table's optional primary key, and
/// its declared foreign keys to the catalog JSON shape and install it at
/// `path` atomically: the text is built and validated first (empty
/// schema, unnamed or duplicate indexes, indexes naming unknown columns,
/// a primary key naming an unknown column, foreign keys naming unknown
/// columns or repeating a column — all throw), then written to
/// `path + ".tmp"` and renamed over `path` — a failed save never leaves
/// a half-written catalog behind. A nullopt primary key and an empty
/// foreign-key array write the shape catalogs always wrote.
void save_table_schema(const std::string& path, const TableSchema& schema,
                       const std::vector<TableIndex>& indexes,
                       const std::optional<std::string>& primary_key = std::nullopt,
                       const std::vector<TableForeignKey>& foreign_keys = {});

}

#endif // EX11_TABLE_SCHEMA_JSON_H
