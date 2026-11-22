cmake_minimum_required(VERSION 4.3)
project(p4stdbeng)

set(CMAKE_CXX_STANDARD 20)

set(CORE_SOURCES
    common/Error.h
    config/Config.cpp config/Config.h
    file/BlockFile.cpp
    bufpool/BufferPool.cpp bufpool/DataFrame.h bufpool/BufferPool.h
    policy/ReplacementPolicy.h policy/LRUPolicy.cpp policy/LRUPolicy.h
    policy/PolicyFactory.cpp policy/PolicyFactory.h
    bufpool/BufferManager.cpp bufpool/BufferManager.h bufpool/PinnedPage.h
    heapfile/SlottedPage.cpp heapfile/SlottedPage.h
    heapfile/HeapFile.cpp heapfile/HeapFile.h
    tuple/Column.h tuple/Tuple.cpp tuple/Tuple.h
    schema/ColumnSchema.h schema/TableSchema.cpp schema/TableSchema.h
    schema/TupleSerializer.cpp schema/TupleSerializer.h
    schema/TableSchemaJson.cpp schema/TableSchemaJson.h
    schema/TableCsv.cpp schema/TableCsv.h
    schema/TableGenerator.cpp schema/TableGenerator.h
    sql/Token.h
    sql/Lexer.h sql/Lexer.cpp
    sql/Ast.h
    sql/Parser.h sql/Parser.cpp
    sql/AstPrinter.h sql/AstPrinter.cpp
    sql/AstDraw.h sql/AstDraw.cpp
    index/KeyCodec.h index/KeyCodec.cpp
    index/Entries.h
    index/IndexPages.h index/IndexPages.cpp
    index/BptPages.h index/BptPages.cpp
    index/BPlusTree.h index/BPlusTree.cpp
    index/IsamIndex.h index/IsamIndex.cpp
    exec/Value.h exec/Value.cpp
    exec/Binder.h exec/Binder.cpp
    exec/JoinedSchema.h exec/JoinedSchema.cpp
    exec/Pushdown.h exec/Pushdown.cpp
    exec/Evaluator.h exec/Evaluator.cpp
    exec/Planner.h exec/Planner.cpp
    exec/Executor.h exec/Executor.cpp
    operators/Operator.h
    operators/SeqScan.h operators/SeqScan.cpp
    operators/Filter.h operators/Filter.cpp
    operators/Project.h operators/Project.cpp
    operators/Limit.h operators/Limit.cpp
    operators/NestedLoopsJoin.h operators/NestedLoopsJoin.cpp
    operators/IndexNestedLoopsJoin.h operators/IndexNestedLoopsJoin.cpp
    operators/Partitions.h operators/Partitions.cpp
    operators/HashJoin.h operators/HashJoin.cpp
    operators/RowIdScan.h operators/RowIdScan.cpp
    tools/BulkLoader.h tools/BulkLoader.cpp)

add_library(dbcore STATIC ${CORE_SOURCES})
target_include_directories(dbcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(p4stdbeng
    main.cpp
    check_storage.cpp
    check_sql.cpp
    check_index.cpp
    check_exec.cpp
    check_loader.cpp
    check_bptree.cpp
    check_config.cpp
    check_operators.cpp
    check_joins.cpp
    check_cli.cpp)
target_link_libraries(p4stdbeng PRIVATE dbcore)
target_compile_definitions(p4stdbeng PRIVATE
    P4DBENG_CLI_PATH="${CMAKE_BINARY_DIR}/p4stdbeng_cli")

add_executable(p4stdbeng_cli p4stdbeng_cli.cpp)
target_link_libraries(p4stdbeng_cli PRIVATE dbcore)

add_executable(p4stdbeng_bench p4stdbeng_bench.cpp)
target_link_libraries(p4stdbeng_bench PRIVATE dbcore)

enable_testing()
add_test(NAME p4stdbeng_checks COMMAND p4stdbeng)
set_tests_properties(p4stdbeng_checks PROPERTIES TIMEOUT 600
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
