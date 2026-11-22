#include "schema/TableSchemaJson.h"

#include "common/Error.h"

#include <climits>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_set>

namespace schema {

namespace {

// A cursor over the catalog text: position and 1-based line counter.
// Parsing stops at the first error, thrown as "line N: <message>".
class Cursor {
public:
    explicit Cursor(const std::string& text) : text_(text) {}

    bool done() const { return pos_ >= text_.size(); }
    char peek() const { return done() ? '\0' : text_[pos_]; }

    [[noreturn]] void fail(const std::string& message) const {
        throw common::Error("line " + std::to_string(line_) + ": " + message);
    }

    void skip_ws() {
        while (!done()) {
            const char c = text_[pos_];
            if (c == '\n') {
                ++line_;
            } else if (c != ' ' && c != '\t' && c != '\r') {
                break;
            }
            ++pos_;
        }
    }

    void expect(char c, const std::string& what) {
        skip_ws();
        if (peek() != c) {
            fail("expected '" + std::string(1, c) + "' " + what);
        }
        ++pos_;
    }

    // Skips whitespace and consumes `c` if it is next.
    bool accept(char c) {
        skip_ws();
        if (peek() != c) {
            return false;
        }
        ++pos_;
        return true;
    }

    // A JSON string: double-quoted, standard escapes, \uXXXX for BMP code
    // points (encoded as UTF-8).
    std::string parse_string() {
        skip_ws();
        refuse_literal();
        if (peek() != '"') {
            fail("expected a string");
        }
        ++pos_;
        std::string out;
        while (true) {
            if (done()) {
                fail("unterminated string");
            }
            const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') {
                return out;
            }
            if (c == '\\') {
                parse_escape(out);
            } else if (c < 0x20) {
                fail("control character in string");
            } else {
                out += static_cast<char>(c);
            }
        }
    }

    // A JSON integer: optional '-', digits, no fraction or exponent — the
    // only number shape the catalog format needs.
    long long parse_integer() {
        skip_ws();
        refuse_literal();
        const bool negative = peek() == '-';
        if (negative) {
            ++pos_;
        }
        if (peek() < '0' || peek() > '9') {
            fail(negative ? "expected digits after '-'" : "expected an integer");
        }
        long long value = 0;
        while (peek() >= '0' && peek() <= '9') {
            const int digit = peek() - '0';
            if (value > (LLONG_MAX - digit) / 10) {
                fail("integer too large");
            }
            value = value * 10 + digit;
            ++pos_;
        }
        if (peek() == '.' || peek() == 'e' || peek() == 'E') {
            fail("the catalog uses integers only");
        }
        return negative ? -value : value;
    }

private:
    void refuse_literal() const {
        if (peek() == 't' || peek() == 'f' || peek() == 'n') {
            fail("the catalog uses only objects, arrays, strings, and integers");
        }
    }

    void parse_escape(std::string& out) {
        if (done()) {
            fail("unterminated string");
        }
        const char e = text_[pos_++];
        switch (e) {
            case '"': out += '"'; return;
            case '\\': out += '\\'; return;
            case '/': out += '/'; return;
            case 'b': out += '\b'; return;
            case 'f': out += '\f'; return;
            case 'n': out += '\n'; return;
            case 'r': out += '\r'; return;
            case 't': out += '\t'; return;
            case 'u': parse_unicode_escape(out); return;
            default:
                if (e == '\n') {
                    ++line_;
                }
                fail(std::string("unknown escape '\\") + e + "'");
        }
    }

    void parse_unicode_escape(std::string& out) {
        unsigned code = 0;
        for (int i = 0; i < 4; ++i) {
            if (done()) {
                fail("unterminated \\u escape");
            }
            const char h = text_[pos_++];
            code <<= 4;
            if (h >= '0' && h <= '9') {
                code |= static_cast<unsigned>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                code |= static_cast<unsigned>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                code |= static_cast<unsigned>(h - 'A' + 10);
            } else {
                fail("bad \\u escape");
            }
        }
        if (code >= 0xD800 && code <= 0xDFFF) {
            fail("surrogate \\u escapes are not supported");
        }
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    const std::string& text_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
};

// Walks one JSON object: for every member, `on_key` is called with the
// key after its ':' has been consumed, and parses the value. `what` names
// the object in error messages ("catalog", "column", "index"). Each key
// may appear once; `on_key` refuses the unknown ones.
void walk_object(Cursor& cur, const std::string& what, const std::string& opening,
                 const std::function<void(const std::string&)>& on_key) {
    cur.expect('{', opening);
    if (cur.accept('}')) {
        return;
    }
    std::unordered_set<std::string> seen;
    while (true) {
        const std::string key = cur.parse_string();
        cur.expect(':', "after the key");
        if (!seen.insert(key).second) {
            cur.fail("duplicate key '" + key + "'");
        }
        on_key(key);
        cur.skip_ws();
        if (cur.done()) {
            cur.fail("unterminated " + what + " object");
        }
        if (cur.accept(',')) {
            continue;
        }
        if (cur.accept('}')) {
            return;
        }
        cur.fail("expected ',' or '}'");
    }
}

// Walks one JSON array, calling `on_item` to parse each element. `what`
// names the array in error messages ("columns", "indexes").
void walk_array(Cursor& cur, const std::string& what,
                const std::function<void()>& on_item) {
    cur.expect('[', "for '" + what + "'");
    cur.skip_ws();
    if (cur.done()) {
        cur.fail("unterminated " + what + " array");
    }
    if (cur.accept(']')) {
        return;
    }
    while (true) {
        on_item();
        cur.skip_ws();
        if (cur.done()) {
            cur.fail("unterminated " + what + " array");
        }
        if (cur.accept(',')) {
            continue;
        }
        if (cur.accept(']')) {
            return;
        }
        cur.fail("expected ',' or ']'");
    }
}

[[noreturn]] void unexpected_key(Cursor& cur, const std::string& key) {
    cur.fail("unexpected key '" + key + "'");
}

tuple::ColumnType parse_column_type(Cursor& cur) {
    const std::string text = cur.parse_string();
    if (text == "Integer") return tuple::ColumnType::Integer;
    if (text == "String") return tuple::ColumnType::String;
    if (text == "Double") return tuple::ColumnType::Double;
    cur.fail("unknown column type '" + text + "'");
}

// One "columns" entry: column_name, column_type, and a positive size, all
// required. The column is added straight to `schema`, whose add_column
// refuses a duplicate name.
void parse_column(Cursor& cur, TableSchema& schema) {
    std::optional<std::string> name;
    std::optional<tuple::ColumnType> type;
    std::optional<long long> size;
    walk_object(cur, "column", "for a column entry", [&](const std::string& key) {
        if (key == "column_name") name = cur.parse_string();
        else if (key == "column_type") type = parse_column_type(cur);
        else if (key == "size") size = cur.parse_integer();
        else unexpected_key(cur, key);
    });
    if (!name) cur.fail("column entry is missing 'column_name'");
    if (!type) cur.fail("column entry is missing 'column_type'");
    if (!size) cur.fail("column entry is missing 'size'");
    if (name->empty()) cur.fail("column name is empty");
    if (*size <= 0) cur.fail("column '" + *name + "': size must be a positive integer");
    if (schema.column_index(*name)) cur.fail("column '" + *name + "' already exists");
    schema.add_column(*name, *type, static_cast<std::size_t>(*size));
}

// One "indexes" entry: "name" and "column" required, "type" optional
// ("ISAM" — the default when absent — or "BTREE").
void parse_index(Cursor& cur, std::vector<TableIndex>& indexes) {
    TableIndex index;
    bool seen_name = false;
    bool seen_column = false;
    walk_object(cur, "index", "for an index entry", [&](const std::string& key) {
        if (key == "name") { index.name = cur.parse_string(); seen_name = true; }
        else if (key == "column") { index.column = cur.parse_string(); seen_column = true; }
        else if (key == "type") index.type = cur.parse_string();
        else unexpected_key(cur, key);
    });
    if (!seen_name) cur.fail("index entry is missing 'name'");
    if (!seen_column) cur.fail("index entry is missing 'column'");
    if (index.name.empty()) cur.fail("index name is empty");
    if (index.column.empty()) cur.fail("index '" + index.name + "': column is empty");
    if (index.type != "ISAM" && index.type != "BTREE") {
        cur.fail("index '" + index.name + "': unknown type '" + index.type + "'");
    }
    for (const TableIndex& other : indexes) {
        if (other.name == index.name) {
            cur.fail("index '" + index.name + "' already exists");
        }
    }
    indexes.push_back(index);
}

// One "foreign_keys" entry: "column", "parent_table", and
// "parent_column" all required. The referencing column is resolved
// against the schema after the whole object is walked (the array may
// precede "columns" in the text); the parent side is validated when the
// table is created, not at load — the loader reads one file in
// isolation.
void parse_foreign_key(Cursor& cur, std::vector<TableForeignKey>& foreign_keys) {
    TableForeignKey key;
    bool seen_column = false;
    bool seen_parent_table = false;
    bool seen_parent_column = false;
    walk_object(cur, "foreign key", "for a foreign key entry", [&](const std::string& key_name) {
        if (key_name == "column") { key.column = cur.parse_string(); seen_column = true; }
        else if (key_name == "parent_table") { key.parent_table = cur.parse_string(); seen_parent_table = true; }
        else if (key_name == "parent_column") { key.parent_column = cur.parse_string(); seen_parent_column = true; }
        else unexpected_key(cur, key_name);
    });
    if (!seen_column) cur.fail("foreign key entry is missing 'column'");
    if (!seen_parent_table) cur.fail("foreign key entry is missing 'parent_table'");
    if (!seen_parent_column) cur.fail("foreign key entry is missing 'parent_column'");
    if (key.column.empty()) cur.fail("foreign key entry: column is empty");
    if (key.parent_table.empty()) {
        cur.fail("foreign key on column '" + key.column + "': parent_table is empty");
    }
    if (key.parent_column.empty()) {
        cur.fail("foreign key on column '" + key.column + "': parent_column is empty");
    }
    foreign_keys.push_back(key);
}

}

// The top-level catalog object: "table_name" and "columns" required,
// "indexes", "primary_key", and "foreign_keys" optional, unknown keys
// refused — a fixed format, and a
// typo'd key should be an error, not a silently ignored member.
Catalog parse_catalog(const std::string& text) {
    Cursor cur(text);
    std::optional<std::string> table_name;
    TableSchema columns("");
    bool seen_columns = false;
    std::vector<TableIndex> indexes;
    std::optional<std::string> primary_key;
    std::vector<TableForeignKey> foreign_keys;
    walk_object(cur, "catalog", "opening the catalog", [&](const std::string& key) {
        if (key == "table_name") {
            table_name = cur.parse_string();
        } else if (key == "columns") {
            seen_columns = true;
            walk_array(cur, "columns", [&] { parse_column(cur, columns); });
            if (columns.column_count() == 0) {
                cur.fail("catalog declares no columns");
            }
        } else if (key == "indexes") {
            walk_array(cur, "indexes", [&] { parse_index(cur, indexes); });
        } else if (key == "primary_key") {
            primary_key = cur.parse_string();
        } else if (key == "foreign_keys") {
            walk_array(cur, "foreign_keys", [&] { parse_foreign_key(cur, foreign_keys); });
        } else {
            unexpected_key(cur, key);
        }
    });
    if (!table_name) cur.fail("catalog is missing 'table_name'");
    if (!seen_columns) cur.fail("catalog is missing 'columns'");
    cur.skip_ws();
    if (!cur.done()) cur.fail("unexpected text after the catalog");
    if (table_name->empty()) cur.fail("table name is empty");
    // The constraint names a column, so the name must resolve — a typo'd
    // primary_key is an error, not a silently ignored member.
    if (primary_key && !columns.column_index(*primary_key)) {
        cur.fail("catalog's primary_key names unknown column '" + *primary_key + "'");
    }
    // Foreign keys resolve against the (complete) schema, and one column
    // carries at most one reference.
    for (const TableForeignKey& key : foreign_keys) {
        if (!columns.column_index(key.column)) {
            cur.fail("foreign key names unknown column '" + key.column + "'");
        }
        for (const TableForeignKey& other : foreign_keys) {
            if (&other != &key && other.column == key.column) {
                cur.fail("column '" + key.column + "' already carries a foreign key");
            }
        }
    }

    Catalog catalog{TableSchema(*table_name), std::move(indexes), std::move(primary_key),
                    std::move(foreign_keys)};
    for (std::size_t i = 0; i < columns.column_count(); ++i) {
        const ColumnSchema& column = columns.column_info(i);
        catalog.schema.add_column(column.column_name(), column.column_type(),
                                  column.column_size());
    }
    return catalog;
}

TableSchema parse_table_schema(const std::string& text) {
    return parse_catalog(text).schema;
}

Catalog load_catalog(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw common::Error("cannot open '" + path + "'");
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parse_catalog(text.str());
}

TableSchema load_table_schema(const std::string& path) {
    return load_catalog(path).schema;
}

// --- the writer -------------------------------------------------------

namespace {

// JSON string escaping for names the writer emits: quotes, backslashes,
// and control characters become escapes; everything else passes through.
std::string json_escape(const std::string& value) {
    std::string out;
    for (const char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string column_type_text(tuple::ColumnType type) {
    switch (type) {
        case tuple::ColumnType::Integer: return "Integer";
        case tuple::ColumnType::String: return "String";
        case tuple::ColumnType::Double: return "Double";
    }
    return "";
}

std::string catalog_text(const TableSchema& schema, const std::vector<TableIndex>& indexes,
                         const std::optional<std::string>& primary_key,
                         const std::vector<TableForeignKey>& foreign_keys) {
    std::string text = "{\n";
    text += "  \"table_name\": \"" + json_escape(schema.table_name()) + "\",\n";
    text += "  \"columns\": [\n";
    for (std::size_t i = 0; i < schema.column_count(); ++i) {
        const ColumnSchema& column = schema.column_info(i);
        text += "    { \"column_name\": \"" + json_escape(column.column_name()) +
                "\", \"column_type\": \"" + column_type_text(column.column_type()) +
                "\", \"size\": " + std::to_string(column.column_size()) + " }";
        text += (i + 1 < schema.column_count()) ? ",\n" : "\n";
    }
    text += "  ],\n";
    // The primary key, only when the table declares one: catalogs
    // without the constraint keep the shape catalogs always had.
    if (primary_key) {
        text += "  \"primary_key\": \"" + json_escape(*primary_key) + "\",\n";
    }
    // The foreign keys, only when the table declares any.
    if (!foreign_keys.empty()) {
        text += "  \"foreign_keys\": [\n";
        for (std::size_t i = 0; i < foreign_keys.size(); ++i) {
            text += "    { \"column\": \"" + json_escape(foreign_keys[i].column) +
                    "\", \"parent_table\": \"" + json_escape(foreign_keys[i].parent_table) +
                    "\", \"parent_column\": \"" + json_escape(foreign_keys[i].parent_column) + "\" }";
            text += (i + 1 < foreign_keys.size()) ? ",\n" : "\n";
        }
        text += "  ],\n";
    }
    text += "  \"indexes\": [";
    if (!indexes.empty()) {
        text += "\n";
        for (std::size_t i = 0; i < indexes.size(); ++i) {
            text += "    { \"name\": \"" + json_escape(indexes[i].name) +
                    "\", \"column\": \"" + json_escape(indexes[i].column) +
                    "\", \"type\": \"" + json_escape(indexes[i].type) + "\" }";
            text += (i + 1 < indexes.size()) ? ",\n" : "\n";
        }
        text += "  ]\n";
    } else {
        text += "]\n";
    }
    text += "}\n";
    return text;
}

// Refuses a catalog the parser would not read back.
void check_saveable(const TableSchema& schema, const std::vector<TableIndex>& indexes,
                    const std::optional<std::string>& primary_key,
                    const std::vector<TableForeignKey>& foreign_keys) {
    if (schema.table_name().empty()) {
        throw common::Error("cannot save a catalog with no table name");
    }
    const std::string prefix =
        "cannot save a catalog for table '" + schema.table_name() + "': ";
    if (schema.column_count() == 0) {
        throw common::Error(prefix + "it declares no columns");
    }
    if (primary_key && !schema.column_index(*primary_key)) {
        throw common::Error(prefix + "the primary key names unknown column '" + *primary_key + "'");
    }
    for (const TableForeignKey& key : foreign_keys) {
        if (!schema.column_index(key.column)) {
            throw common::Error(prefix + "a foreign key names unknown column '" +
                                key.column + "'");
        }
        if (key.parent_table.empty() || key.parent_column.empty()) {
            throw common::Error(prefix + "the foreign key on column '" + key.column +
                                "' names an empty parent");
        }
        for (const TableForeignKey& other : foreign_keys) {
            if (&other != &key && other.column == key.column) {
                throw common::Error(prefix + "column '" + key.column +
                                    "' carries more than one foreign key");
            }
        }
    }
    std::unordered_set<std::string> seen_names;
    for (const TableIndex& index : indexes) {
        if (index.name.empty()) {
            throw common::Error(prefix + "an index name is empty");
        }
        if (!seen_names.insert(index.name).second) {
            throw common::Error(prefix + "index '" + index.name + "' already exists");
        }
        if (!schema.column_index(index.column)) {
            throw common::Error(prefix + "index '" + index.name +
                                "' names unknown column '" + index.column + "'");
        }
        if (index.type != "ISAM" && index.type != "BTREE") {
            throw common::Error(prefix + "index '" + index.name +
                                "' has unknown type '" + index.type + "'");
        }
    }
}

}

void save_table_schema(const std::string& path, const TableSchema& schema,
                       const std::vector<TableIndex>& indexes,
                       const std::optional<std::string>& primary_key,
                       const std::vector<TableForeignKey>& foreign_keys) {
    // Validate and build the whole text before any byte moves.
    check_saveable(schema, indexes, primary_key, foreign_keys);
    const std::string text = catalog_text(schema, indexes, primary_key, foreign_keys);

    // Install atomically: write the sibling temp file, flush it, then
    // rename over the target. A failure anywhere before the rename leaves
    // the previous catalog exactly as it was.
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        out.flush();
        if (!out) {
            throw common::Error("cannot write '" + temp + "'");
        }
    }
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        std::remove(temp.c_str());
        throw common::Error("cannot install '" + path + "' from '" + temp + "'");
    }
}

}
