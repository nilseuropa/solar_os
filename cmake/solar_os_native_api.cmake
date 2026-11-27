# Check committed generated files on every build. Generation is an explicit
# developer action; builds never silently rewrite the SDK or ABI baseline.
idf_build_get_property(solar_os_native_api_python PYTHON)
add_custom_target(solar_os_native_api_check
    COMMAND "${solar_os_native_api_python}"
        "${CMAKE_CURRENT_LIST_DIR}/../scripts/generate_native_api.py" --check
    COMMAND "${solar_os_native_api_python}"
        "${CMAKE_CURRENT_LIST_DIR}/../scripts/audit_native_system.py" --check
    VERBATIM)
add_dependencies(__idf_src solar_os_native_api_check)
