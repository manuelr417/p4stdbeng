// p4stdbeng — the check suite. The vendored suites run in order (storage,
// sql, index, exec, loader, config) ending with the differential oracle;
// each returns its failure count. Exit code 0 iff every check passes.

#include <iostream>

int run_storage_checks();
int run_sql_checks();
int run_index_checks();
int run_bptree_checks();
int run_exec_checks();
int run_loader_checks();
int run_bptree_checks();
int run_config_checks();
int run_operators_checks();
int run_joins_checks();
int run_cli_checks();

int main() {
    const int storage_failures = run_storage_checks();
    const int sql_failures = run_sql_checks();
    const int index_failures = run_index_checks();
    const int exec_failures = run_exec_checks();
    const int loader_failures = run_loader_checks();
    const int bptree_failures = run_bptree_checks();
    const int config_failures = run_config_checks();
    const int operators_failures = run_operators_checks();
    const int joins_failures = run_joins_checks();
    const int cli_failures = run_cli_checks();
    const int failures = storage_failures + sql_failures + index_failures +
                         bptree_failures + exec_failures + loader_failures +
                         config_failures + operators_failures + joins_failures +
                         cli_failures;
    std::cout << (failures == 0 ? "\nAll checks passed." : "\nSome checks FAILED.")
              << "\n";
    return failures == 0 ? 0 : 1;
}
