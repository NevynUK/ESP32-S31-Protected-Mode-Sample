# The kernel and the user application share one image.  user_main.c is placed by
# linker.lf in page-aligned IRAM of its own, and that is the only kernel-image
# memory any user address space maps.
idf_component_register(
    SRCS
        "kernel_main.c"
        "isolation_test.c"
        "vm.c"
        "umode.S"
        "user_main.c"
    INCLUDE_DIRS
        "."
    LDFRAGMENTS
        "linker.lf"
)

# user_main.c runs at a different virtual address from the one it is linked at,
# so it must be position-independent in practice: no absolute data references,
# no jump tables, no library calls, no gp-relative relaxation.
set_source_files_properties(user_main.c PROPERTIES COMPILE_OPTIONS
    "-fno-jump-tables;-fno-tree-switch-conversion;-fno-tree-loop-distribute-patterns;-fno-builtin;-fno-stack-protector;-mno-relax;-mno-save-restore")
